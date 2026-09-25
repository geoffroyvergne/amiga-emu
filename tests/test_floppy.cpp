#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <vector>

#include "core/adf.hpp"
#include "core/custom_registers.hpp"
#include "core/disk_controller.hpp"
#include "core/floppy.hpp"
#include "core/machine.hpp"
#include "core/memory_bus.hpp"
#include "test_framework.hpp"

namespace {

using amiga::AdfImage;
using amiga::DiskController;
using amiga::FloppyDrive;
using amiga::MemoryBus;
namespace reg = amiga::reg;

using Track = std::array<uint16_t, AdfImage::kTrackWords>;

// A disk whose every byte encodes its position, so misplaced data shows.
std::vector<uint8_t> patterned_disk() {
    std::vector<uint8_t> data(AdfImage::kSize);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 7 + i / 512);
    return data;
}

// --- MFM decoding, as trackdisk.device does it ------------------------------------

uint32_t mfm_long(const Track& t, size_t word) { return uint32_t{t[word]} << 16 | t[word + 1]; }

// Odd and even halves (each two MFM words) back into a long.
uint32_t decode_long(const Track& t, size_t odd_word, size_t even_word) {
    return (mfm_long(t, odd_word) & 0x5555'5555u) << 1 | (mfm_long(t, even_word) & 0x5555'5555u);
}

void test_mfm_track_decodes_back() {
    const AdfImage disk(patterned_disk());
    const unsigned track = 37;  // cylinder 18, head 1
    Track mfm{};
    disk.encode_track(track, mfm);

    // Valid MFM everywhere except the sync words: no two adjacent 1 bits, no
    // more than three 0 bits in a row (checked across word boundaries).
    unsigned run_of_zeros = 0;
    bool previous = false;
    bool valid = true;
    for (size_t w = 0; w < mfm.size(); ++w) {
        if (mfm[w] == AdfImage::kSyncWord) {
            previous = true;
            run_of_zeros = 0;
            continue;
        }
        for (int bit = 15; bit >= 0; --bit) {
            const bool one = (mfm[w] >> bit & 1u) != 0;
            if (one && previous) valid = false;
            run_of_zeros = one ? 0 : run_of_zeros + 1;
            if (run_of_zeros > 3) valid = false;
            previous = one;
        }
    }
    EXPECT(valid);

    std::array<bool, 11> seen{};
    for (size_t w = 0; w + AdfImage::kSectorWords < mfm.size(); ++w) {
        if (mfm[w] != 0x4489 || mfm[w + 1] != 0x4489) continue;
        const size_t base = w + 2;
        const uint32_t info = decode_long(mfm, base, base + 2);
        const unsigned sector = (info >> 8) & 0xFFu;
        EXPECT((info >> 24) == 0xFF);
        EXPECT(((info >> 16) & 0xFFu) == track);
        EXPECT((info & 0xFFu) == 11 - sector);
        if (sector >= 11) continue;
        seen[sector] = true;

        uint32_t header = 0;
        for (size_t i = 0; i < 20; i += 2) header ^= mfm_long(mfm, base + i);
        EXPECT((header & 0x5555'5555u) == decode_long(mfm, base + 20, base + 22));

        const size_t data = base + 28;
        uint32_t checksum = 0;
        for (size_t i = 0; i < 512; i += 2) checksum ^= mfm_long(mfm, data + i);
        EXPECT((checksum & 0x5555'5555u) == decode_long(mfm, base + 24, base + 26));

        const auto expected = disk.sector(track, sector);
        bool same = true;
        for (size_t i = 0; i < 128; ++i) {
            const uint32_t value = decode_long(mfm, data + 2 * i, data + 256 + 2 * i);
            for (unsigned b = 0; b < 4; ++b) {
                if (static_cast<uint8_t>(value >> (24 - 8 * b)) != expected[i * 4 + b]) same = false;
            }
        }
        EXPECT(same);
    }
    for (const bool s : seen) EXPECT(s);
}

void test_adf_rejects_wrong_size() {
    EXPECT_THROWS(AdfImage(std::vector<uint8_t>(1000)), std::runtime_error);
}

// --- Drive ------------------------------------------------------------------------

constexpr uint8_t kIdle = 0xFF;                                   // nothing selected, motor off
constexpr uint8_t kSelectMotor = static_cast<uint8_t>(~(FloppyDrive::kPrbSel0 | FloppyDrive::kPrbMtr));

void test_drive_status_lines() {
    FloppyDrive drive;
    drive.control(kIdle);
    EXPECT(drive.status() == FloppyDrive::kStatusBits);  // not selected: lines float high

    drive.control(static_cast<uint8_t>(kIdle & ~FloppyDrive::kPrbSel0));
    EXPECT((drive.status() & FloppyDrive::kPraTk0) == 0);   // cylinder 0
    EXPECT((drive.status() & FloppyDrive::kPraChng) == 0);  // no disk: change latch set
    EXPECT((drive.status() & FloppyDrive::kPraRdy) == 0);

    drive.insert(std::make_unique<AdfImage>(patterned_disk()));
    EXPECT((drive.status() & FloppyDrive::kPraChng) == 0);  // still latched until a step
    const auto step_in = static_cast<uint8_t>(kIdle & ~FloppyDrive::kPrbSel0 & ~FloppyDrive::kPrbDir);
    drive.control(static_cast<uint8_t>(step_in & ~FloppyDrive::kPrbStep));  // /STEP low: step in
    drive.control(step_in);
    EXPECT(drive.cylinder() == 1);
    EXPECT((drive.status() & FloppyDrive::kPraChng) != 0);  // latch reset: disk present
    EXPECT((drive.status() & FloppyDrive::kPraTk0) != 0);

    drive.eject();
    EXPECT((drive.status() & FloppyDrive::kPraChng) == 0);

    drive.control(static_cast<uint8_t>(kIdle & ~FloppyDrive::kPrbSel0 & ~FloppyDrive::kPrbSide));
    EXPECT(drive.head() == 1);  // /SIDE low = upper head
}

void test_drive_rotation_and_index() {
    FloppyDrive drive;
    drive.insert(std::make_unique<AdfImage>(patterned_disk()));
    drive.control(kIdle);
    drive.control(kSelectMotor);  // /MTR latched on selection
    EXPECT(drive.motor_on());

    unsigned words = 0;
    std::vector<unsigned> index_at;
    for (unsigned cck = 0; cck < 2 * 709'375; ++cck) {  // two revolutions (0.2 s each)
        const FloppyDrive::Tick t = drive.tick();
        if (t.word_ready) ++words;
        if (t.index) index_at.push_back(cck);
    }
    EXPECT(words == 2 * AdfImage::kTrackWords);  // one word per 113.5 color clocks
    EXPECT(index_at.size() == 2);
    EXPECT(index_at.size() == 2 && index_at[1] - index_at[0] == 709'375);

    FloppyDrive off;
    off.insert(std::make_unique<AdfImage>(patterned_disk()));
    off.control(static_cast<uint8_t>(kIdle & ~FloppyDrive::kPrbSel0));  // selected, motor off
    bool any = false;
    for (int i = 0; i < 1000; ++i) any = any || off.tick().word_ready;
    EXPECT(!any);
}

// --- DMA --------------------------------------------------------------------------

void test_disk_dma_read_with_sync() {
    auto bus = std::make_unique<MemoryBus>();
    DiskController disk;
    uint16_t requests = 0;
    disk.write_register(DiskController::kAdkCon, reg::kSetClr | DiskController::kAdkWordSync, requests);
    disk.write_register(DiskController::kDskSync, 0x4489, requests);
    disk.write_register(DiskController::kDskPth, 0x0000, requests);
    disk.write_register(DiskController::kDskPtl, 0x1000, requests);
    disk.write_register(DiskController::kDskLen, 0x8003, requests);  // armed only
    EXPECT(!disk.active());
    disk.write_register(DiskController::kDskLen, 0x8003, requests);  // second write starts: 3 words
    EXPECT(disk.active());

    const uint16_t stream[] = {0xAAAA, 0x1234, 0x4489, 0x4489, 0x1111, 0x2222, 0x3333, 0x4444};
    for (const uint16_t word : stream) disk.disk_word(word, bus->chip_ram(), true, requests);
    // Nothing stored before the sync; the first sync word is not stored.
    EXPECT(bus->read16(0x1000) == 0x4489);
    EXPECT(bus->read16(0x1002) == 0x1111);
    EXPECT(bus->read16(0x1004) == 0x2222);
    EXPECT(bus->read16(0x1006) == 0x0000);  // length reached before $3333
    EXPECT(!disk.active());
    EXPECT((requests & reg::kIntDskSyn) != 0);
    EXPECT((requests & reg::kIntDskBlk) != 0);
}

void test_disk_dma_needs_dsken_and_can_stop() {
    auto bus = std::make_unique<MemoryBus>();
    DiskController disk;
    uint16_t requests = 0;
    disk.write_register(DiskController::kDskPtl, 0x2000, requests);
    disk.write_register(DiskController::kDskLen, 0x8002, requests);
    disk.write_register(DiskController::kDskLen, 0x8002, requests);
    disk.disk_word(0xABCD, bus->chip_ram(), false, requests);  // DSKEN off: nothing
    EXPECT(bus->read16(0x2000) == 0);
    disk.write_register(DiskController::kDskLen, 0x0000, requests);  // stop
    disk.disk_word(0xABCD, bus->chip_ram(), true, requests);
    EXPECT(bus->read16(0x2000) == 0 && requests == 0);

    disk.write_register(DiskController::kDskLen, 0xC010, requests);  // write transfer...
    disk.write_register(DiskController::kDskLen, 0xC010, requests);
    EXPECT(!disk.active() && (requests & reg::kIntDskBlk) != 0);  // ...completes at once
}

}  // namespace

// A bootable disk made here (no copyrighted content): its boot block turns
// Copper and bitplane DMA off and paints COLOR00 green forever.
AdfImage make_green_boot_disk() {
    std::vector<uint8_t> data(AdfImage::kSize);
    const uint8_t boot[] = {
        'D', 'O', 'S', 0,  0, 0, 0, 0,  0, 0, 0x03, 0x70,  // checksum filled below, root block 880
        0x41, 0xF9, 0x00, 0xDF, 0xF0, 0x00,  // LEA     $DFF000,A0
        0x31, 0x7C, 0x01, 0x80, 0x00, 0x96,  // MOVE.W  #$0180,$96(A0)   DMACON: Copper, bitplanes off
        0x31, 0x7C, 0x00, 0xF0, 0x01, 0x80,  // MOVE.W  #$00F0,$180(A0)  COLOR00 = green
        0x60, 0xF8,                          // BRA.S   (the MOVE above)
    };
    std::copy(std::begin(boot), std::end(boot), data.begin());
    // Boot block checksum: sum of the 256 longs with end-around carry is $FFFFFFFF.
    uint32_t sum = 0;
    for (size_t i = 0; i < 1024; i += 4) {
        const uint32_t value = uint32_t{data[i]} << 24 | uint32_t{data[i + 1]} << 16 | uint32_t{data[i + 2]} << 8 | data[i + 3];
        const uint32_t previous = sum;
        sum += value;
        if (sum < previous) ++sum;
    }
    const uint32_t checksum = ~sum;
    for (unsigned b = 0; b < 4; ++b) data[4 + b] = static_cast<uint8_t>(checksum >> (24 - 8 * b));
    return AdfImage(std::move(data));
}

void run_floppy_tests() {
    test_mfm_track_decodes_back();
    test_adf_rejects_wrong_size();
    test_drive_status_lines();
    test_drive_rotation_and_index();
    test_disk_dma_read_with_sync();
    test_disk_dma_needs_dsken_and_can_stop();
}
