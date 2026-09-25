#pragma once

#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

namespace amiga {

// An Amiga Disk File: the decoded sectors of a double-density floppy,
// 80 cylinders x 2 heads x 11 sectors x 512 bytes, track by track
// (track = cylinder * 2 + head).
//
// The drive needs what the head would read: encode_track() produces the raw
// MFM stream of a track in the standard AmigaDOS format that trackdisk.device
// expects, for each sector:
//   $AAAA $AAAA              two zero bytes
//   $4489 $4489              sync words (MFM with a missing clock bit)
//   info (odd, even)         $FF, track, sector, sectors until end of track
//   label (odd x4, even x4)  16 zero bytes
//   header checksum          XOR of the info and label longs
//   data checksum            XOR of the data longs
//   data (odd x128, even x128)
// Each long is stored as its odd bits then its even bits, each MFM-encoded.
class AdfImage {
public:
    static constexpr unsigned kCylinders = 80;
    static constexpr unsigned kHeads = 2;
    static constexpr unsigned kTracks = kCylinders * kHeads;
    static constexpr unsigned kSectorsPerTrack = 11;
    static constexpr unsigned kSectorSize = 512;
    static constexpr size_t kSize = size_t{kTracks} * kSectorsPerTrack * kSectorSize;  // 901120

    static constexpr unsigned kSectorWords = 544;  // MFM words per encoded sector
    // One revolution at 300 RPM with 2 us MFM cells: 100000 cells = 6250 words.
    static constexpr unsigned kTrackWords = 6250;
    static constexpr uint16_t kSyncWord = 0x4489;

    // Throws std::runtime_error if the data is not a 901120-byte image.
    explicit AdfImage(std::vector<uint8_t> data);
    static AdfImage load(const std::filesystem::path& path);

    // Raw MFM words of a track: 11 sectors, then a gap of encoded zeros.
    void encode_track(unsigned track, std::span<uint16_t, kTrackWords> out) const noexcept;

    [[nodiscard]] std::span<const uint8_t> sector(unsigned track, unsigned sector) const noexcept {
        return std::span<const uint8_t>(data_).subspan((size_t{track} * kSectorsPerTrack + sector) * kSectorSize,
                                                       kSectorSize);
    }

private:
    std::vector<uint8_t> data_;
};

}  // namespace amiga
