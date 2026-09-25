#include "core/chipset.hpp"

#include <cstdio>
#include <span>

#include "core/custom_registers.hpp"
#include "core/timing.hpp"

namespace amiga {

uint16_t Chipset::read_custom(uint16_t offset) {
    uint16_t value = 0;
    if (offset == reg::kDmaConR) {  // BBUSY never set: blits complete at once
        agnus_.read_register(offset, value);
        return static_cast<uint16_t>(value | (blitter_.zero() ? 0x2000u : 0u));
    }
    if (offset == 0x000) return blitter_.last_d();  // BLTDDAT
    // Strobe registers act on any access: a read of COPJMP1/2 (MOVE.W
    // $DFF088,Dn, as some games do) restarts the Copper just like a write.
    if (offset == reg::kCopJmp1 || offset == reg::kCopJmp2) {
        copper_.write_register(offset, 0);
        return kOpenBusValue;
    }
    if (disk_.read_register(offset, value, agnus_.dma_enabled(reg::kDskEn))) return value;
    if (offset == Denise::kClxDat) return denise_.read_collisions();
    if (agnus_.read_register(offset, value) || paula_.read_register(offset, value) ||
        denise_.read_register(offset, value)) {
        return value;
    }
    // Every readable register lies below $020; from $020 on they are all
    // write-only (or strobes). Reading one gets nothing driven onto the data
    // bus, which floats high: $FFFF on an OCS A500. Games rely on it without
    // knowing: Barbarian's ORI.W #$8020,$DFF096 (a read-modify-write of
    // DMACON) writes back $FFFF, and that is what switches DMA master on.
    if (offset >= kFirstWriteOnlyRegister) return kOpenBusValue;
    warn_unemulated(offset, false);
    return 0;
}

void Chipset::write_custom(uint16_t offset, uint16_t value) {
    shadow_[offset / 2u] = value;
    written_[offset / 2u] = true;
    if (offset == reg::kBplCon0) {  // latched by both Agnus and Denise
        check_bplcon0(value);
        agnus_.write_register(offset, value);
        denise_.write_register(offset, value);
        return;
    }
    uint16_t disk_requests = 0;
    if (disk_.write_register(offset, value, disk_requests)) {
        paula_.request(disk_requests);
        return;
    }
    if (audio_.write_register(offset, value) || sprites_.write_register(offset, value, agnus_.hpos())) return;
    bool blit_started = false;
    if (blitter_.write_register(offset, value, blit_started)) {
        if (blit_started) start_blit_if_enabled();
        return;
    }
    if (offset == reg::kDmaCon) {
        agnus_.write_register(offset, value);
        if (blitter_.pending()) start_blit_if_enabled();  // a blit waits for BLTEN
        return;
    }
    if (agnus_.write_register(offset, value) || copper_.write_register(offset, value) ||
        paula_.write_register(offset, value) || denise_.write_register(offset, value)) {
        return;
    }
    warn_unemulated(offset, true);
}

bool Chipset::tick() {
    const uint16_t vpos = agnus_.vpos();
    const uint16_t hpos = agnus_.hpos();

    if (vpos == 0 && hpos == 0) {  // start of vertical blank
        paula_.request(reg::kIntVertb);
        copper_.vertical_blank();
    }

    copper_.tick(bus_.chip_ram(), vpos, hpos, agnus_.dma_enabled(reg::kCopEn), *this);

    uint16_t audio_requests = 0;
    audio_.tick(bus_.chip_ram(), agnus_.dmacon(), audio_requests);
    if (audio_requests != 0) paula_.request(audio_requests);

    if (hpos == 0) sprites_.begin_line();
    // Sprite DMA slots come early in the line (from $15), before the display.
    if (hpos == 0x15) sprites_.dma_line(bus_.chip_ram(), vpos, agnus_.dma_enabled(reg::kSprEn));
    if (hpos == timing::kPalColorClocksPerLine - 1) render_line(vpos);

    return agnus_.advance_beam();
}

void Chipset::start_blit_if_enabled() noexcept {
    if (!agnus_.dma_enabled(reg::kBltEn)) return;
    blitter_.run(bus_.chip_ram());
    paula_.request(reg::kIntBlit);
}

// Bitplane DMA runs on every line of the vertical window, even outside the
// viewport (overscan), so the pointers advance exactly as on hardware.
void Chipset::render_line(uint16_t vpos) noexcept {
    agnus_.fetch_line(bus_.chip_ram(), vpos, line_fetch_);
    if (vpos < kFirstDisplayLine || vpos >= kFirstDisplayLine + Denise::kDisplayLines) return;
    const size_t line = vpos - kFirstDisplayLine;
    const Denise::Row row{frame_.data() + line * Denise::kOutputWidth, Denise::kOutputWidth};
    denise_.render_line(line_fetch_, agnus_.window(), sprites_, row);
}

// Settings OCS can't display as asked: warn once, render what Agnus fetches.
void Chipset::check_bplcon0(uint16_t value) noexcept {
    const unsigned bpu = (value >> reg::kBplCon0BpuShift) & 7u;
    const bool hires = (value & reg::kBplCon0Hires) != 0;
    if (warned_bplcon0_ || (bpu <= 6 && !(hires && bpu > 4))) return;
    warned_bplcon0_ = true;
    std::fprintf(stderr,
                 "[chipset] warning: BPLCON0 $%04X asks for %u %s bitplanes; OCS fetches %s\n",
                 value, bpu, hires ? "hires" : "lowres", bpu > 6 ? "none" : "only 4");
}

void Chipset::warn_unemulated(uint16_t offset, bool is_write) noexcept {
    bool& warned = warned_[offset / 2u];
    if (warned) return;
    warned = true;
    std::fprintf(stderr, "[chipset] warning: %s of unemulated custom register $DFF%03X ignored\n",
                 is_write ? "write" : "read", offset);
}

}  // namespace amiga
