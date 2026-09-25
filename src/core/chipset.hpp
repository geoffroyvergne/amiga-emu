#pragma once

#include <array>
#include <cstdint>

#include "core/agnus.hpp"
#include "core/blitter.hpp"
#include "core/copper.hpp"
#include "core/denise.hpp"
#include "core/disk_controller.hpp"
#include "core/memory_bus.hpp"
#include "core/paula.hpp"
#include "core/paula_audio.hpp"
#include "core/sprites.hpp"

namespace amiga {

// The OCS custom chips as one unit: routes register accesses at $DFF000 to
// Agnus, the Copper, Denise and Paula, and advances them in lockstep with the
// beam, one color clock at a time.
//
// Accesses to real but not-yet-emulated registers are logged once per register
// and ignored (reads return 0).
class Chipset final : public CustomChipPort {
public:
    // First beam line shown in the output viewport.
    static constexpr uint16_t kFirstDisplayLine = Denise::kViewportStartV;

    using Frame = std::array<uint32_t, size_t{Denise::kOutputWidth} * Denise::kDisplayLines>;

    explicit Chipset(MemoryBus& bus) noexcept : bus_(bus) {}

    uint16_t read_custom(uint16_t offset) override;
    void write_custom(uint16_t offset, uint16_t value) override;

    // Advances one color clock. Returns true when the beam has wrapped to the
    // start of a new frame, i.e. the frame in frame() is complete.
    bool tick();

    [[nodiscard]] uint8_t interrupt_level() const noexcept { return paula_.interrupt_level(); }

    // An MFM word read by the selected floppy drive's head.
    void disk_word(uint16_t word) noexcept {
        uint16_t requests = 0;
        disk_.disk_word(word, bus_.chip_ram(), agnus_.dma_enabled(reg::kDskEn), requests);
        paula_.request(requests);
    }

    // 640x256 ARGB8888, one entry per Denise output line.
    [[nodiscard]] const Frame& frame() const noexcept { return frame_; }

    // Last value written to each custom register (for debugging; reads of
    // write-only registers on hardware return garbage, not this).
    [[nodiscard]] uint16_t last_written(uint16_t offset) const noexcept { return shadow_[(offset & 0x1FFu) / 2]; }
    [[nodiscard]] bool was_written(uint16_t offset) const noexcept { return written_[(offset & 0x1FFu) / 2]; }

    [[nodiscard]] Agnus& agnus() noexcept { return agnus_; }
    [[nodiscard]] Denise& denise() noexcept { return denise_; }
    [[nodiscard]] Paula& paula() noexcept { return paula_; }
    [[nodiscard]] Copper& copper() noexcept { return copper_; }
    [[nodiscard]] Blitter& blitter() noexcept { return blitter_; }
    [[nodiscard]] DiskController& disk() noexcept { return disk_; }
    [[nodiscard]] PaulaAudio& audio() noexcept { return audio_; }
    [[nodiscard]] const Sprites& sprites() const noexcept { return sprites_; }
    [[nodiscard]] const PaulaAudio& audio() const noexcept { return audio_; }

private:
    static constexpr uint16_t kFirstWriteOnlyRegister = 0x020;  // DSKPTH
    static constexpr uint16_t kOpenBusValue = 0xFFFF;
    void render_line(uint16_t vpos) noexcept;
    void warn_unemulated(uint16_t offset, bool is_write) noexcept;
    void check_bplcon0(uint16_t value) noexcept;
    void start_blit_if_enabled() noexcept;

    MemoryBus& bus_;
    Agnus agnus_;
    Denise denise_;
    Paula paula_;
    Copper copper_;
    Blitter blitter_;
    DiskController disk_;
    PaulaAudio audio_;
    Sprites sprites_;
    Frame frame_{};
    LineFetch line_fetch_{};
    bool warned_bplcon0_ = false;
    std::array<bool, MemoryBus::kCustomSize / 2> warned_{};
    std::array<uint16_t, MemoryBus::kCustomSize / 2> shadow_{};
    std::array<bool, MemoryBus::kCustomSize / 2> written_{};
};

}  // namespace amiga
