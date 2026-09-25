#include "core/adf.hpp"

#include <array>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>

namespace amiga {

namespace {

// Writes MFM words: each data bit (even positions, mask $5555) gets a clock
// bit before it that is 1 only when both neighbouring data bits are 0.
class MfmWriter {
public:
    explicit MfmWriter(std::span<uint16_t, AdfImage::kTrackWords> out) : out_(out) {}

    // `data` holds data bits at the even positions only.
    void data16(uint32_t data) noexcept {
        const uint32_t d = data & 0x5555u;
        uint32_t clocks = ~((d << 1) | (d >> 1)) & 0xAAAAu;
        if (last_data_bit_) clocks &= 0x7FFFu;  // bit 15's left neighbour is the previous word's bit 0
        raw(static_cast<uint16_t>(d | clocks));
    }

    void raw(uint16_t word) noexcept {
        if (position_ < out_.size()) out_[position_++] = word;
        last_data_bit_ = (word & 1u) != 0;
    }

    // A long's odd or even bits, as two MFM words.
    void half_long(uint32_t bits) noexcept {
        data16(bits >> 16);
        data16(bits & 0xFFFFu);
    }

    [[nodiscard]] size_t position() const noexcept { return position_; }

private:
    std::span<uint16_t, AdfImage::kTrackWords> out_;
    size_t position_ = 0;
    bool last_data_bit_ = false;
};

constexpr uint32_t odd_bits(uint32_t value) noexcept { return (value >> 1) & 0x5555'5555u; }
constexpr uint32_t even_bits(uint32_t value) noexcept { return value & 0x5555'5555u; }

uint32_t read_long(std::span<const uint8_t> bytes, size_t index) noexcept {
    const size_t i = index * 4;
    return uint32_t{bytes[i]} << 24 | uint32_t{bytes[i + 1]} << 16 | uint32_t{bytes[i + 2]} << 8 | bytes[i + 3];
}

}  // namespace

AdfImage::AdfImage(std::vector<uint8_t> data) : data_(std::move(data)) {
    if (data_.size() != kSize) {
        throw std::runtime_error("ADF image must be exactly 901120 bytes (got " + std::to_string(data_.size()) + ")");
    }
}

AdfImage AdfImage::load(const std::filesystem::path& path) {
    std::ifstream file(path, std::ios::binary);
    if (!file) throw std::runtime_error("cannot open disk image: " + path.string());
    std::vector<uint8_t> data{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
    if (file.bad()) throw std::runtime_error("error reading disk image: " + path.string());
    return AdfImage(std::move(data));
}

void AdfImage::encode_track(unsigned track, std::span<uint16_t, kTrackWords> out) const noexcept {
    MfmWriter writer(out);
    for (unsigned s = 0; s < kSectorsPerTrack; ++s) {
        const std::span<const uint8_t> data = sector(track, s);

        writer.data16(0);  // two zero bytes before the sync
        writer.data16(0);
        writer.raw(kSyncWord);
        writer.raw(kSyncWord);

        const uint32_t info = 0xFF00'0000u | (track & 0xFFu) << 16 | s << 8 | (kSectorsPerTrack - s);
        writer.half_long(odd_bits(info));
        writer.half_long(even_bits(info));
        for (int i = 0; i < 4; ++i) writer.half_long(0);  // label, odd
        for (int i = 0; i < 4; ++i) writer.half_long(0);  // label, even

        const uint32_t header_checksum = (odd_bits(info) ^ even_bits(info)) & 0x5555'5555u;
        writer.half_long(odd_bits(header_checksum));
        writer.half_long(even_bits(header_checksum));

        uint32_t data_checksum = 0;
        for (size_t i = 0; i < kSectorSize / 4; ++i) {
            const uint32_t value = read_long(data, i);
            data_checksum ^= odd_bits(value) ^ even_bits(value);
        }
        data_checksum &= 0x5555'5555u;
        writer.half_long(odd_bits(data_checksum));
        writer.half_long(even_bits(data_checksum));

        for (size_t i = 0; i < kSectorSize / 4; ++i) writer.half_long(odd_bits(read_long(data, i)));
        for (size_t i = 0; i < kSectorSize / 4; ++i) writer.half_long(even_bits(read_long(data, i)));
    }
    while (writer.position() < kTrackWords) writer.data16(0);  // inter-sector gap to the index
}

}  // namespace amiga
