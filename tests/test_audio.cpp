#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstdint>
#include <memory>
#include <vector>

#include "core/custom_registers.hpp"
#include "core/machine.hpp"
#include "core/memory_bus.hpp"
#include "core/paula_audio.hpp"
#include "test_framework.hpp"

namespace {

using amiga::MemoryBus;
using amiga::PaulaAudio;
namespace reg = amiga::reg;

constexpr uint16_t aud(unsigned channel, uint16_t reg_offset) {
    return static_cast<uint16_t>(PaulaAudio::kBase + channel * 0x10 + reg_offset);
}

struct Fixture {
    std::unique_ptr<MemoryBus> bus = std::make_unique<MemoryBus>();
    PaulaAudio audio;

    // Channel with samples {10, 20, -30, 40} at $1000: 2 words, period 4.
    void setup(unsigned channel, uint16_t volume) {
        const int8_t samples[] = {10, 20, -30, 40};
        for (unsigned i = 0; i < 4; ++i) bus->chip_ram()[0x1000 + i] = static_cast<uint8_t>(samples[i]);
        audio.write_register(aud(channel, 0x0), 0x0000);
        audio.write_register(aud(channel, 0x2), 0x1000);
        audio.write_register(aud(channel, 0x4), 2);
        audio.write_register(aud(channel, 0x6), 4);
        audio.write_register(aud(channel, 0x8), volume);
    }

    uint16_t tick(uint16_t dmacon) {
        uint16_t requests = 0;
        audio.tick(bus->chip_ram(), dmacon, requests);
        return requests;
    }
};

constexpr uint16_t kDma0 = reg::kDmaEn | PaulaAudio::kAud0En;

void test_dma_plays_buffer_in_a_loop() {
    Fixture f;
    f.setup(0, 1);
    std::vector<int> out;
    std::vector<unsigned> interrupts;
    for (unsigned t = 0; t < 26; ++t) {
        if ((f.tick(kDma0) & reg::kIntAud0) != 0) interrupts.push_back(t);
        out.push_back(f.audio.channel_output(0));
    }
    // Each sample lasts PER = 4 color clocks, high byte first, then the buffer repeats.
    const int expected[] = {10, 10, 10, 10, 20, 20, 20, 20, -30, -30, -30, -30, 40, 40, 40, 40, 10, 10, 10, 10};
    for (size_t i = 0; i < std::size(expected); ++i) EXPECT(out[i] == expected[i]);
    // AUD0 interrupt when the buffer is latched: at the start, and after its last word is fetched.
    EXPECT(interrupts.size() == 3 && interrupts[0] == 0 && interrupts[1] == 8 && interrupts[2] == 24);
}

void test_volume_stereo_and_dma_off() {
    Fixture f;
    f.setup(0, 64);
    f.setup(1, 32);
    f.tick(kDma0 | (PaulaAudio::kAud0En << 1));
    EXPECT(f.audio.left() == 10 * 64);   // channel 0: left
    EXPECT(f.audio.right() == 10 * 32);  // channel 1: right

    f.setup(3, 0x7F);  // bit 6 set: full volume (64)
    f.tick(kDma0 | (PaulaAudio::kAud0En << 3));
    EXPECT(f.audio.channel_output(3) == 10 * 64);

    f.tick(reg::kDmaEn);  // all channels off
    EXPECT(f.audio.left() == 0 && f.audio.right() == 0);
    f.tick(PaulaAudio::kAud0En);  // master DMAEN off: nothing plays
    EXPECT(f.audio.left() == 0);
}

void test_manual_mode() {
    Fixture f;
    f.setup(2, 64);
    f.audio.write_register(aud(2, 0xA), 0x05FB);  // samples 5, -5 written by the CPU
    EXPECT(f.audio.channel_output(2) == 5 * 64);
    unsigned interrupt_at = 0;
    for (unsigned t = 1; t <= 12 && interrupt_at == 0; ++t) {
        if ((f.tick(reg::kDmaEn) & (reg::kIntAud0 << 2)) != 0) interrupt_at = t;
        if (t == 4) EXPECT(f.audio.channel_output(2) == -5 * 64);
    }
    EXPECT(interrupt_at == 8);  // after both samples
}

void test_machine_audio_stream() {
    auto m = std::make_unique<amiga::Machine>();
    MemoryBus& bus = m->bus();
    const auto custom = [](uint16_t offset) { return MemoryBus::kCustomBase + offset; };
    bus.write16(0x2000, 0x7F81);  // square wave: +127, -127
    bus.write16(custom(aud(1, 0x0)), 0x0000);
    bus.write16(custom(aud(1, 0x2)), 0x2000);
    bus.write16(custom(aud(1, 0x4)), 1);
    bus.write16(custom(aud(1, 0x6)), 200);
    bus.write16(custom(aud(1, 0x8)), 64);
    bus.write16(custom(reg::kDmaCon), reg::kSetClr | reg::kDmaEn | (PaulaAudio::kAud0En << 1));
    m->run_frame();

    const auto samples = m->audio();
    EXPECT(samples.size() == 2 * 960 || samples.size() == 2 * 961);  // 71051 / 74 per frame
    int peak_left = 0, peak_right = 0, positive = 0, negative = 0;
    for (size_t i = 0; i + 1 < samples.size(); i += 2) {
        peak_left = std::max(peak_left, std::abs(int{samples[i]}));
        peak_right = std::max(peak_right, std::abs(int{samples[i + 1]}));
        positive += samples[i + 1] > 0;
        negative += samples[i + 1] < 0;
    }
    EXPECT(peak_left == 0);                 // channel 1 is on the right
    EXPECT(peak_right > 127 * 64 * 2 * 9 / 10);  // near full scale for one channel
    EXPECT(positive > 300 && negative > 300);    // both half-waves present
}

}  // namespace

void run_audio_tests() {
    test_dma_plays_buffer_in_a_loop();
    test_volume_stereo_and_dma_off();
    test_manual_mode();
    test_machine_audio_stream();
}
