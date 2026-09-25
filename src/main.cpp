#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "core/custom_registers.hpp"
#include "core/debug_dump.hpp"
#include "core/denise.hpp"
#include "core/machine.hpp"
#include "core/memory_bus.hpp"
#include "core/timing.hpp"
#include "frontend/gamepad_mapping.hpp"
#include "frontend/keyboard_joystick.hpp"
#include "frontend/sdl_keymap.hpp"

namespace {

// Denise outputs 640x256; SDL stretches it to 640x512 (each line shown twice).
constexpr int kFrameWidth = static_cast<int>(amiga::Denise::kOutputWidth);
constexpr int kFrameHeight = static_cast<int>(amiga::Denise::kDisplayLines);
constexpr int kWindowWidth = kFrameWidth;
constexpr int kWindowHeight = kFrameHeight * 2;

// Audio arrives at 71051 / 74 = 960.15 stereo samples per emulated frame; the
// window shows 50 frames per second, so the stream is fed at 48007 Hz.
constexpr int kAudioRate = static_cast<int>(amiga::timing::kPalFrameRateHz * amiga::timing::kPalColorClocksPerFrame /
                                            amiga::Machine::kColorClocksPerSample);
constexpr int kMaxQueuedAudioBytes = kAudioRate * 4 / 10;  // 100 ms of 16-bit stereo

struct SdlDeleter {
    void operator()(SDL_Window* w) const noexcept { SDL_DestroyWindow(w); }
    void operator()(SDL_Renderer* r) const noexcept { SDL_DestroyRenderer(r); }
    void operator()(SDL_Texture* t) const noexcept { SDL_DestroyTexture(t); }
};

// Demo without Kickstart: a 320x256, 5-bitplane (32 colour) picture in chip
// RAM — colour stripes over a checkerboard — shown in a display window inset
// by 16 pixels on each side, so the COLOR00 border frames it. A Copper list
// reloads the bitplane pointers every frame and draws colour bars by changing
// COLOR00 on each line (visible in the border and in stripe 0). Registers are
// set through the bus at $DFF000, exactly as the CPU would.
void load_test_pattern(amiga::Machine& m) {
    namespace reg = amiga::reg;
    constexpr unsigned kPlanes = 5;
    constexpr unsigned kWidth = 320;
    constexpr unsigned kHeight = 256;
    constexpr unsigned kBytesPerRow = kWidth / 8;
    constexpr uint32_t kBitmap = 0x1'0000;
    constexpr uint32_t kPlaneSize = kBytesPerRow * kHeight;
    constexpr uint32_t kCopperList = 0x2'0000;
    constexpr uint16_t kBackground = 0x005A;  // Workbench 1.x blue

    amiga::MemoryBus& bus = m.bus();
    const auto chip = bus.chip_ram();
    for (unsigned y = 0; y < kHeight; ++y) {
        for (unsigned x = 0; x < kWidth; ++x) {
            unsigned index = 0;
            if (y < kHeight / 2) {
                index = x / 10;  // 32 stripes, one per colour register
            } else if (x - 32 == y - kHeight / 2 || x - 33 == y - kHeight / 2) {
                index = 31;
            } else if (((x / 16) + (y / 16)) % 2 == 0) {
                index = 1;
            }
            for (unsigned plane = 0; plane < kPlanes; ++plane) {
                if ((index >> plane & 1u) != 0) {
                    chip[kBitmap + plane * kPlaneSize + y * kBytesPerRow + x / 8] |=
                        static_cast<uint8_t>(0x80u >> (x % 8));
                }
            }
        }
    }

    const auto write_reg = [&bus](uint16_t offset, uint16_t value) {
        bus.write16(amiga::MemoryBus::kCustomBase + offset, value);
    };
    // Palette: COLOR01 white, COLOR02-31 around the colour wheel (12-bit RGB).
    write_reg(reg::kColor00 + 2, 0x0FFF);
    for (unsigned i = 2; i < 32; ++i) {
        const unsigned hue = (i - 2) * 90 / 30;  // 0..89, 15 steps per sextant
        const unsigned step = hue % 15;
        const unsigned up = step, down = 15 - step;
        static constexpr unsigned kSextant[6][3] = {{2, 1, 0}, {3, 2, 0}, {0, 2, 1}, {0, 3, 2}, {1, 0, 2}, {2, 0, 3}};
        const unsigned values[4] = {0, 15, up, down};  // 0: off, 1: full, 2: rising, 3: falling
        const unsigned* s = kSextant[hue / 15];
        const auto rgb = static_cast<uint16_t>(values[s[0]] << 8 | values[s[1]] << 4 | values[s[2]]);
        write_reg(static_cast<uint16_t>(reg::kColor00 + 2 * i), rgb);
    }

    std::vector<uint16_t> list;
    for (unsigned plane = 0; plane < kPlanes; ++plane) {
        const uint32_t address = kBitmap + plane * kPlaneSize;
        list.insert(list.end(), {static_cast<uint16_t>(reg::kBpl1Pth + 4 * plane), static_cast<uint16_t>(address >> 16),
                                 static_cast<uint16_t>(reg::kBpl1Ptl + 4 * plane), static_cast<uint16_t>(address)});
    }
    list.insert(list.end(), {reg::kColor00, kBackground});
    constexpr unsigned kBarLines = 48;
    constexpr uint16_t kBarTop = amiga::Chipset::kFirstDisplayLine + 100;
    for (unsigned i = 0; i <= kBarLines; ++i) {
        const unsigned t = i < kBarLines / 2 ? i : kBarLines - 1 - i;  // 0..23..0
        const uint16_t color = i == kBarLines ? kBackground : static_cast<uint16_t>(0x0F00 | (t * 15 / 23) << 4);
        list.insert(list.end(), {static_cast<uint16_t>((kBarTop + i) << 8 | 0x07), 0xFFFE,  // WAIT line, h=$06
                                 reg::kColor00, color});
    }
    list.insert(list.end(), {0xFFFF, 0xFFFE});  // end of list: wait for an impossible position
    for (size_t i = 0; i < list.size(); ++i) {
        bus.write16(kCopperList + static_cast<uint32_t>(i) * 2, list[i]);
    }

    write_reg(reg::kDiwStrt, 0x3C91);  // window: lines $3C-$11B, lowres H $91-$1B0
    write_reg(reg::kDiwStop, 0x1CB1);
    write_reg(reg::kDdfStrt, 0x0038);  // standard lowres fetch: 20 words per plane
    write_reg(reg::kDdfStop, 0x00D0);
    write_reg(reg::kBpl1Mod, 0);
    write_reg(reg::kBpl2Mod, 0);
    write_reg(reg::kBplCon0, static_cast<uint16_t>(kPlanes << reg::kBplCon0BpuShift | reg::kBplCon0Color));
    write_reg(reg::kCop1Lch, static_cast<uint16_t>(kCopperList >> 16));
    write_reg(reg::kCop1Lcl, static_cast<uint16_t>(kCopperList));
    write_reg(reg::kDmaCon, reg::kSetClr | reg::kDmaEn | reg::kBplEn | reg::kCopEn);
}

// Host input to the emulated keyboard, mouse (port 1) and joystick (port 2).
// Clicking in the window captures the mouse (relative mode) and F12 releases
// it. Esc is an Amiga key; quit by closing the window. The first gamepad
// connected is the joystick: D-pad or left stick, South button (A/Cross) fires;
// its other buttons act as Amiga keys and mouse buttons (see gamepad_mapping.hpp).
// Disks: drop an .adf on the window, or hold F12 and press F1/F2/F3 to load
// disk1/2/3.adf from the emulator's directory (F1-F3 alone go to the Amiga).
// F12+J toggles the keyboard joystick (port 2): arrows/WASD move, Left Ctrl,
// Space or Left Alt fire; those keys then no longer reach the Amiga keyboard.
class HostInput {
public:
    HostInput(amiga::Machine& machine, SDL_Window* window, bool keyboard_joystick) noexcept
        : machine_(machine), window_(window), keyboard_joystick_on_(keyboard_joystick) {
        update_title();
    }

    // Returns false when the user asked to quit.
    bool pump() noexcept {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (log_input_) log_event(event);
            switch (event.type) {
                case SDL_EVENT_QUIT:
                case SDL_EVENT_WINDOW_CLOSE_REQUESTED:
                    return false;
                case SDL_EVENT_WINDOW_FOCUS_LOST:
                    release_everything();
                    break;
                case SDL_EVENT_DROP_FILE:  // swap the disk in DF0
                    load_disk(event.drop.data);
                    break;
                case SDL_EVENT_KEY_DOWN:
                case SDL_EVENT_KEY_UP:
                    on_key(event.key);
                    break;
                case SDL_EVENT_MOUSE_MOTION:
                    if (captured_) on_motion(event.motion.xrel, event.motion.yrel);
                    break;
                case SDL_EVENT_MOUSE_BUTTON_DOWN:
                case SDL_EVENT_MOUSE_BUTTON_UP:
                    on_button(event.button);
                    break;
                case SDL_EVENT_GAMEPAD_ADDED:  // also sent at start-up for pads already connected
                    if (gamepad_ == nullptr) {
                        gamepad_ = SDL_OpenGamepad(event.gdevice.which);
                        if (gamepad_ != nullptr) SDL_Log("Joystick: %s", SDL_GetGamepadName(gamepad_));
                    }
                    break;
                case SDL_EVENT_GAMEPAD_REMOVED:
                    if (gamepad_ != nullptr && SDL_GetGamepadID(gamepad_) == event.gdevice.which) {
                        release_pad_buttons();
                        SDL_CloseGamepad(gamepad_);
                        gamepad_ = open_other_gamepad(event.gdevice.which);
                        update_joystick();
                    }
                    break;
                case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
                case SDL_EVENT_GAMEPAD_BUTTON_UP:
                    on_pad_button(static_cast<SDL_GamepadButton>(event.gbutton.button), event.gbutton.down);
                    update_joystick();
                    break;
                case SDL_EVENT_GAMEPAD_AXIS_MOTION:
                    update_joystick();
                    break;
                default:
                    break;
            }
        }
        return true;
    }

    void set_log_input(bool enabled) noexcept { log_input_ = enabled; }

    // Releases everything held and closes the gamepad. Call before SDL_Quit();
    // the destructor only does it if that was forgotten.
    void close() noexcept {
        if (gamepad_ == nullptr) return;
        release_pad_buttons();
        SDL_CloseGamepad(gamepad_);
        gamepad_ = nullptr;
        update_joystick();
    }
    ~HostInput() { close(); }
    HostInput(const HostInput&) = delete;
    HostInput& operator=(const HostInput&) = delete;

private:
    // A digital joystick has switches: the stick counts past half its travel.
    static constexpr int16_t kStickThreshold = 16384;

    // After the active pad is unplugged: the next connected one, if any.
    static SDL_Gamepad* open_other_gamepad(SDL_JoystickID removed) noexcept {
        int count = 0;
        SDL_JoystickID* ids = SDL_GetGamepads(&count);
        SDL_Gamepad* pad = nullptr;
        for (int i = 0; ids != nullptr && i < count && pad == nullptr; ++i) {
            if (ids[i] != removed) pad = SDL_OpenGamepad(ids[i]);
        }
        SDL_free(ids);
        if (pad != nullptr) SDL_Log("Joystick: %s", SDL_GetGamepadName(pad));
        return pad;
    }

    static void log_event(const SDL_Event& e) noexcept {
        switch (e.type) {
            case SDL_EVENT_KEY_DOWN:
            case SDL_EVENT_KEY_UP:
                SDL_Log("[input] key %s %s%s", SDL_GetScancodeName(e.key.scancode), e.key.down ? "down" : "up",
                        e.key.repeat ? " (repeat)" : "");
                break;
            case SDL_EVENT_MOUSE_BUTTON_DOWN:
            case SDL_EVENT_MOUSE_BUTTON_UP:
                SDL_Log("[input] mouse button %u %s at %.0f,%.0f", e.button.button, e.button.down ? "down" : "up",
                        e.button.x, e.button.y);
                break;
            case SDL_EVENT_WINDOW_FOCUS_GAINED: SDL_Log("[input] window focus gained"); break;
            case SDL_EVENT_WINDOW_FOCUS_LOST: SDL_Log("[input] window focus LOST"); break;
            case SDL_EVENT_WINDOW_MOUSE_ENTER: SDL_Log("[input] mouse entered window"); break;
            case SDL_EVENT_WINDOW_MOUSE_LEAVE: SDL_Log("[input] mouse left window"); break;
            case SDL_EVENT_GAMEPAD_ADDED: SDL_Log("[input] gamepad added"); break;
            case SDL_EVENT_GAMEPAD_REMOVED: SDL_Log("[input] gamepad removed"); break;
            case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
            case SDL_EVENT_GAMEPAD_BUTTON_UP:
                SDL_Log("[input] gamepad button %s %s",
                        SDL_GetGamepadStringForButton(static_cast<SDL_GamepadButton>(e.gbutton.button)),
                        e.gbutton.down ? "down" : "up");
                break;
            default: break;
        }
    }

    // Port 2 = keyboard joystick OR gamepad.
    void update_joystick() noexcept {
        frontend::KeyboardJoystick::State s = keyboard_joystick_.state();
        if (gamepad_ != nullptr) {
            const auto button = [this](SDL_GamepadButton b) { return SDL_GetGamepadButton(gamepad_, b); };
            const int16_t x = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTX);
            const int16_t y = SDL_GetGamepadAxis(gamepad_, SDL_GAMEPAD_AXIS_LEFTY);
            s.up = s.up || button(SDL_GAMEPAD_BUTTON_DPAD_UP) || y < -kStickThreshold;
            s.down = s.down || button(SDL_GAMEPAD_BUTTON_DPAD_DOWN) || y > kStickThreshold;
            s.left = s.left || button(SDL_GAMEPAD_BUTTON_DPAD_LEFT) || x < -kStickThreshold;
            s.right = s.right || button(SDL_GAMEPAD_BUTTON_DPAD_RIGHT) || x > kStickThreshold;
            s.fire = s.fire || button(SDL_GAMEPAD_BUTTON_SOUTH);
        }
        if (s.up && s.down) s.up = s.down = false;  // a real stick can't report both
        if (s.left && s.right) s.left = s.right = false;
        machine_.joystick(s.up, s.down, s.left, s.right, s.fire);
    }

    void update_title() noexcept {
        std::string title = "Amiga 500";
        if (captured_) title += " - F12 releases the mouse";
        if (keyboard_joystick_on_) title += " - keyboard joystick ON (F12+J)";
        SDL_SetWindowTitle(window_, title.c_str());
    }

    // The joystick keys are also Amiga keys: release any the Amiga still
    // sees as held, and reset the joystick, when switching modes.
    void toggle_keyboard_joystick() noexcept {
        keyboard_joystick_on_ = !keyboard_joystick_on_;
        release_amiga_keys();
        keyboard_joystick_.release_all();
        update_joystick();
        update_title();
        SDL_Log("Keyboard joystick %s", keyboard_joystick_on_ ? "on: arrows/WASD + Ctrl/Space/Alt" : "off");
    }

    // Replaces the disk in DF0 (the old image is freed). Outside the emulation
    // loop's hot path: loading allocates.
    void load_disk(const std::string& path) noexcept {
        try {
            machine_.insert_disk(path);
            SDL_Log(machine_.disk_swap_pending() ? "DF0: ejected; %s goes in 3 s later (as a person swapping disks)"
                                                 : "DF0: %s",
                    path.c_str());
        } catch (const std::exception& e) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", e.what());
        }
    }

    void on_key(const SDL_KeyboardEvent& key) noexcept {
        if (key.repeat) return;  // the Amiga does its own key repeat
        if (key.scancode == SDL_SCANCODE_F12) {  // host key: never sent to the Amiga
            f12_held_ = key.down;
            if (key.down) set_captured(false);
            return;
        }
        if (f12_held_ && key.scancode == SDL_SCANCODE_J) {
            if (key.down) toggle_keyboard_joystick();
            return;
        }
        if (keyboard_joystick_on_ && keyboard_joystick_.key(key.scancode, key.down)) {
            update_joystick();
            return;
        }
        if (f12_held_ && key.scancode >= SDL_SCANCODE_F1 && key.scancode <= SDL_SCANCODE_F3) {
            if (key.down) {
                const char* base = SDL_GetBasePath();
                const int n = key.scancode - SDL_SCANCODE_F1 + 1;
                load_disk(std::string(base != nullptr ? base : "") + "disk" + std::to_string(n) + ".adf");
            }
            return;
        }
        if (const auto code = frontend::amiga_keycode(key.scancode)) {
            if (keys_down_[*code] == key.down) return;
            keys_down_[*code] = key.down;
            amiga_key(*code, key.down);
        }
    }

    // The window shows each lowres pixel as 2x2 host pixels: 1 mouse count
    // per lowres pixel of motion, keeping fractions for the next event.
    void on_motion(float xrel, float yrel) noexcept {
        remainder_x_ += xrel / 2.0f;
        remainder_y_ += yrel / 2.0f;
        const int dx = std::clamp(static_cast<int>(remainder_x_), -127, 127);
        const int dy = std::clamp(static_cast<int>(remainder_y_), -127, 127);
        remainder_x_ -= static_cast<float>(dx);
        remainder_y_ -= static_cast<float>(dy);
        if (dx != 0 || dy != 0) machine_.mouse_move(dx, dy);
    }

    void on_button(const SDL_MouseButtonEvent& button) noexcept {
        if (!captured_) {
            if (button.down && button.button == SDL_BUTTON_LEFT) set_captured(true);  // not forwarded
            return;
        }
        switch (button.button) {
            case SDL_BUTTON_LEFT: left_ = button.down; break;
            case SDL_BUTTON_RIGHT: right_ = button.down; break;
            case SDL_BUTTON_MIDDLE: middle_ = button.down; break;
            default: return;
        }
        update_mouse_buttons();
    }

    // Host mouse and gamepad shoulders share the Amiga mouse buttons.
    void update_mouse_buttons() noexcept {
        machine_.mouse_buttons(left_ || pad_left_, right_ || pad_right_, middle_);
    }

    // An Amiga key held by one more (or one fewer) host input.
    void amiga_key(uint8_t code, bool down) noexcept {
        if (down ? key_holds_.press(code) : key_holds_.release(code)) machine_.key_event(code, down);
    }

    void on_pad_button(SDL_GamepadButton button, bool down) noexcept {
        const frontend::PadAction action = frontend::pad_action(button);
        if (action.kind == frontend::PadAction::Kind::None) return;
        const auto index = static_cast<size_t>(button);
        if (index >= pad_held_.size() || pad_held_[index] == down) return;
        pad_held_[index] = down;
        switch (action.kind) {
            case frontend::PadAction::Kind::Keys:
                for (uint8_t i = 0; i < action.key_count; ++i) amiga_key(action.keys[i], down);
                break;
            case frontend::PadAction::Kind::LeftMouse: pad_left_ = down; update_mouse_buttons(); break;
            case frontend::PadAction::Kind::RightMouse: pad_right_ = down; update_mouse_buttons(); break;
            case frontend::PadAction::Kind::None: break;
        }
    }

    void release_pad_buttons() noexcept {
        for (size_t b = 0; b < pad_held_.size(); ++b) {
            if (pad_held_[b]) on_pad_button(static_cast<SDL_GamepadButton>(b), false);
        }
    }

    void set_captured(bool captured) noexcept {
        if (captured == captured_) return;
        captured_ = captured;
        if (!SDL_SetWindowRelativeMouseMode(window_, captured)) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "mouse capture %s failed: %s", captured ? "on" : "off", SDL_GetError());
        } else if (log_input_) {
            SDL_Log("[input] mouse capture %s (relative mode now %d)", captured ? "ON" : "off",
                    SDL_GetWindowRelativeMouseMode(window_));
        }
        update_title();
        if (!captured) {
            left_ = right_ = middle_ = false;
            update_mouse_buttons();
        }
    }

    void release_everything() noexcept {
        set_captured(false);
        release_amiga_keys();
        release_pad_buttons();
        keyboard_joystick_.release_all();
        update_joystick();
    }

    void release_amiga_keys() noexcept {
        for (size_t code = 0; code < keys_down_.size(); ++code) {
            if (!keys_down_[code]) continue;
            keys_down_[code] = false;
            amiga_key(static_cast<uint8_t>(code), false);
        }
    }

    amiga::Machine& machine_;
    SDL_Window* window_;
    bool captured_ = false;
    bool left_ = false, right_ = false, middle_ = false;
    float remainder_x_ = 0.0f, remainder_y_ = 0.0f;
    std::array<bool, 128> keys_down_{};  // Amiga keys held from the host keyboard
    frontend::KeyHolds key_holds_;         // all sources: keyboard and gamepad
    std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> pad_held_{};
    bool pad_left_ = false, pad_right_ = false;
    SDL_Gamepad* gamepad_ = nullptr;
    bool f12_held_ = false;
    bool log_input_ = false;
    bool keyboard_joystick_on_ = false;
    frontend::KeyboardJoystick keyboard_joystick_;
};

}  // namespace

struct Options {
    const char* rom = nullptr;
    bool headless = false;
    bool trace = true;
    uint64_t frames = 0;  // 0 = until the window is closed (headless: 500)
    const char* screenshot = nullptr;
    const char* df0 = nullptr;
    bool turbo_floppy = false;
    bool joy_keys = false;
    bool slow_ram = true;
    bool log_input = false;
};

void print_usage(const char* program) {
    std::fprintf(stderr,
                 "usage: %s [options] [kick.rom]\n"
                 "  --df0 FILE.adf     insert a disk in DF0 (or drop an .adf on the window,\n"
                 "                     or hold F12 + F1/F2/F3 for disk1/2/3.adf next to the program)\n"
                 "  --turbo-floppy     ~113x faster disk DMA (default: real disk speed, with the\n"
                 "                     emulation unthrottled while the disk is read). Faster loads,\n"
                 "                     but some loaders fail with it\n"
                 "  --joy-keys         start with the keyboard joystick on (toggle: F12+J):\n"
                 "                     arrows/WASD = port 2 directions, Left Ctrl/Space/Left Alt = fire\n"
                 "  --headless         run without a window, as fast as possible\n"
                 "  --frames N         stop after N frames (headless default: 500)\n"
                 "  --no-trace         don't record the last instructions (recorded by default,\n"
                 "                     dumped on a crash, a hang or headless exit)\n"
                 "  --log-input        log keyboard/mouse/gamepad/focus events and mouse capture\n"
                 "  --no-slow-ram      stock 512KB A500 (default: + 512KB trapdoor slow RAM at $C00000)\n"
                 "  --screenshot FILE  save the last frame as a BMP on exit\n",
                 program);
}

bool parse_options(int argc, char* argv[], Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg = argv[i];
        if (arg == "--headless") {
            options.headless = true;
        } else if (arg == "--trace") {
            options.trace = true;  // the default; kept for compatibility
        } else if (arg == "--no-trace") {
            options.trace = false;
        } else if (arg == "--log-input") {
            options.log_input = true;
        } else if (arg == "--no-slow-ram") {
            options.slow_ram = false;
        } else if (arg == "--frames" && i + 1 < argc) {
            options.frames = std::strtoull(argv[++i], nullptr, 10);
        } else if (arg == "--joy-keys") {
            options.joy_keys = true;
        } else if (arg == "--turbo-floppy") {
            options.turbo_floppy = true;
        } else if (arg == "--accurate-floppy") {
            options.turbo_floppy = false;  // the default; kept for compatibility
        } else if (arg == "--df0" && i + 1 < argc) {
            options.df0 = argv[++i];
        } else if (arg == "--screenshot" && i + 1 < argc) {
            options.screenshot = argv[++i];
        } else if (!arg.empty() && arg[0] != '-' && options.rom == nullptr) {
            options.rom = argv[i];
        } else {
            return false;
        }
    }
    return true;
}

// Saves the frame with each line doubled (640x512), as shown in the window.
bool save_screenshot(const amiga::Chipset::Frame& frame, const char* path) {
    SDL_Surface* surface = SDL_CreateSurface(kWindowWidth, kWindowHeight, SDL_PIXELFORMAT_ARGB8888);
    if (surface == nullptr) return false;
    for (int y = 0; y < kWindowHeight; ++y) {
        const uint32_t* src = frame.data() + static_cast<size_t>(y / 2) * kFrameWidth;
        auto* dst = reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(surface->pixels) + y * surface->pitch);
        std::copy(src, src + kFrameWidth, dst);
    }
    const bool ok = SDL_SaveBMP(surface, path);
    SDL_DestroySurface(surface);
    return ok;
}

// Reports a halted CPU, or one stuck in a tight loop (PC within 64 bytes for
// 5 seconds of emulated time), once, with a full state dump.
class HangDetector {
public:
    static constexpr uint32_t kLoopBytes = 64;
    static constexpr unsigned kLoopFrames = 250;

    void check(amiga::Machine& machine) {
        if (const auto deadlock = machine.take_deadlock()) amiga::dump_deadlock(machine, *deadlock, stderr);
        if (reported_) return;
        if (machine.cpu_halted()) {
            report(machine, "CPU halted");
            return;
        }
        const bool tight = machine.frame_max_pc() >= machine.frame_min_pc() &&
                           machine.frame_max_pc() - machine.frame_min_pc() < kLoopBytes && !machine.cpu().stopped();
        tight_frames_ = tight ? tight_frames_ + 1 : 0;
        if (tight_frames_ >= kLoopFrames) report(machine, "CPU looping in a tight range for 5 s");
    }

    [[nodiscard]] bool reported() const noexcept { return reported_; }

private:
    void report(amiga::Machine& machine, const char* what) {
        reported_ = true;
        std::fprintf(stderr, "\n*** %s at frame %llu ***\n", what, static_cast<unsigned long long>(machine.frame_count()));
        amiga::dump_state(machine, stderr);
    }

    unsigned tight_frames_ = 0;
    bool reported_ = false;
};

int run_headless(amiga::Machine& machine, const Options& options) {
    const uint64_t frames = options.frames != 0 ? options.frames : 500;
    HangDetector detector;
    const uint64_t start = SDL_GetTicksNS();
    for (uint64_t i = 0; i < frames && !detector.reported(); ++i) {
        machine.run_frame();
        detector.check(machine);
    }
    const double seconds = static_cast<double>(SDL_GetTicksNS() - start) / 1e9;
    std::fprintf(stderr, "[headless] %llu frames (%.1f s emulated) in %.2f s\n",
                 static_cast<unsigned long long>(machine.frame_count()),
                 static_cast<double>(machine.frame_count()) / 50.0, seconds);
    if (!detector.reported()) amiga::dump_state(machine, stderr);
    if (options.screenshot != nullptr && !save_screenshot(machine.chipset().frame(), options.screenshot)) {
        std::fprintf(stderr, "[headless] could not save %s: %s\n", options.screenshot, SDL_GetError());
        return 1;
    }
    return machine.cpu_halted() ? 2 : 0;
}

int main(int argc, char* argv[]) {
    Options options;
    if (!parse_options(argc, argv, options)) {
        print_usage(argv[0]);
        return 1;
    }
    std::fprintf(stderr, "amiga_emu built %s %s\n", __DATE__, __TIME__);

    // Allocated once, up front: RAM, ROM and the frame buffer are ~2MB.
    auto machine = std::make_unique<amiga::Machine>();
    machine->set_trace_enabled(options.trace);
    machine->set_slow_ram(options.slow_ram);
    machine->set_floppy_turbo(options.turbo_floppy);
    if (options.rom != nullptr) {
        try {
            machine->load_kickstart(options.rom);
        } catch (const std::exception& e) {
            SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", e.what());
            return 1;
        }
        if (options.df0 != nullptr) {
            try {
                machine->insert_disk(options.df0);
            } catch (const std::exception& e) {
                SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "%s", e.what());
                return 1;
            }
        }
        machine->reset();
    } else {
        SDL_Log("No Kickstart ROM given (usage: %s <kick.rom>): showing test pattern", argv[0]);
        load_test_pattern(*machine);
    }

    if (options.headless) return run_headless(*machine, options);

    // The click that brings the window to the front also captures the mouse
    // (macOS otherwise swallows it).
    SDL_SetHint(SDL_HINT_MOUSE_FOCUS_CLICKTHROUGH, "1");
    if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | SDL_INIT_AUDIO)) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_Init failed: %s", SDL_GetError());
        return 1;
    }

    std::unique_ptr<SDL_Window, SdlDeleter> window{
        SDL_CreateWindow("Amiga 500", kWindowWidth, kWindowHeight, SDL_WINDOW_RESIZABLE)};
    if (!window) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateWindow failed: %s", SDL_GetError());
        SDL_Quit();
        return 1;
    }

    std::unique_ptr<SDL_Renderer, SdlDeleter> renderer{SDL_CreateRenderer(window.get(), nullptr)};
    if (!renderer) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateRenderer failed: %s", SDL_GetError());
        window.reset();
        SDL_Quit();
        return 1;
    }
    // Our own pacer owns timing; vsync would lock us to the host refresh rate (often 60 Hz).
    SDL_SetRenderVSync(renderer.get(), 0);
    SDL_SetRenderLogicalPresentation(renderer.get(), kWindowWidth, kWindowHeight,
                                     SDL_LOGICAL_PRESENTATION_LETTERBOX);

    std::unique_ptr<SDL_Texture, SdlDeleter> texture{
        SDL_CreateTexture(renderer.get(), SDL_PIXELFORMAT_ARGB8888, SDL_TEXTUREACCESS_STREAMING,
                          kFrameWidth, kFrameHeight)};
    if (!texture) {
        SDL_LogError(SDL_LOG_CATEGORY_APPLICATION, "SDL_CreateTexture failed: %s", SDL_GetError());
        renderer.reset();
        window.reset();
        SDL_Quit();
        return 1;
    }
    SDL_SetTextureScaleMode(texture.get(), SDL_SCALEMODE_NEAREST);

    // Sound is optional: without an audio device the emulator runs silent.
    const SDL_AudioSpec audio_spec{SDL_AUDIO_S16, 2, kAudioRate};
    SDL_AudioStream* audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &audio_spec, nullptr, nullptr);
    if (audio != nullptr) {
        SDL_ResumeAudioStreamDevice(audio);
    } else {
        SDL_Log("No audio output: %s", SDL_GetError());
    }

    amiga::timing::FramePacer pacer;
    pacer.reset(SDL_GetTicksNS());

    constexpr int kPitch = kFrameWidth * static_cast<int>(sizeof(uint32_t));

    uint64_t warp_frames = 0;
    uint64_t last_present = 0;
    HostInput input(*machine, window.get(), options.joy_keys);
    input.set_log_input(options.log_input);
    HangDetector detector;
    while (input.pump() && (options.frames == 0 || machine->frame_count() < options.frames)) {
        machine->run_frame();
        detector.check(*machine);
        const bool warp = machine->disk_busy();

        // In warp, show at most 50 frames per second of host time.
        const uint64_t now = SDL_GetTicksNS();
        if (!warp || now - last_present >= amiga::timing::kFrameDurationNs) {
            last_present = now;
            SDL_UpdateTexture(texture.get(), nullptr, machine->chipset().frame().data(), kPitch);
            SDL_RenderClear(renderer.get());
            SDL_RenderTexture(renderer.get(), texture.get(), nullptr, nullptr);
            SDL_RenderPresent(renderer.get());
        }

        // Warp while loading: unthrottled (and silent) while the disk is being read.
        if (warp) {
            ++warp_frames;
            pacer.reset(SDL_GetTicksNS());
            continue;
        }
        const std::span<const int16_t> samples = machine->audio();
        if (audio != nullptr && SDL_GetAudioStreamQueued(audio) < kMaxQueuedAudioBytes) {
            SDL_PutAudioStreamData(audio, samples.data(), static_cast<int>(samples.size_bytes()));
        }

        // Sleep until the absolute deadline of the next 20 ms frame.
        if (const uint64_t wait_ns = pacer.time_until_next_frame(SDL_GetTicksNS()); wait_ns > 0) {
            SDL_DelayPrecise(wait_ns);
        }
        pacer.frame_done(SDL_GetTicksNS());
    }

    SDL_Log("Exiting after %llu frames (%llu in warp, %llu CPU cycles, %llu pacer resyncs)",
            static_cast<unsigned long long>(machine->frame_count()),
            static_cast<unsigned long long>(warp_frames),
            static_cast<unsigned long long>(machine->cpu_cycles()),
            static_cast<unsigned long long>(pacer.resync_count()));
    if (options.screenshot != nullptr) save_screenshot(machine->chipset().frame(), options.screenshot);
    input.close();  // gamepad closed while SDL is still up
    if (audio != nullptr) SDL_DestroyAudioStream(audio);

    texture.reset();
    renderer.reset();
    window.reset();
    SDL_Quit();
    return 0;
}
