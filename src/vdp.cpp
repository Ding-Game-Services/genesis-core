#include "genesis.h"
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / reset
// ─────────────────────────────────────────────────────────────────────────────
GenVDP::GenVDP(GenBus* b) : bus(b) {
    std::memset(vram,     0, sizeof(vram));
    std::memset(cram,     0, sizeof(cram));
    std::memset(vsram,    0, sizeof(vsram));
    std::memset(regs,     0, sizeof(regs));
    std::memset(framebuf, 0, sizeof(framebuf));

    ctrlPendWord   = false;
    ctrlPendByte   = false;
    ctrlFirst      = 0;
    addrReg        = 0;
    addrInc        = 2;
    cdReg          = 0;
    vcounter       = 0;
    hcounter       = 0;
    vblank         = false;
    hblank         = false;
    dmaActive      = false;
    frame          = 0;
    isPAL          = false;
    vintPending    = false;
    dmaFillData    = 0;
    dmaFillPending = false;
    diagDmaCount   = 0;
    vramDirty      = false;
}

void GenVDP::reset() {
    std::memset(vram,     0, sizeof(vram));
    std::memset(cram,     0, sizeof(cram));
    std::memset(vsram,    0, sizeof(vsram));
    std::memset(regs,     0, sizeof(regs));
    std::memset(framebuf, 0, sizeof(framebuf));

    ctrlPendWord   = false;
    ctrlPendByte   = false;
    ctrlFirst      = 0;
    addrReg        = 0;
    addrInc        = 2;
    cdReg          = 0;
    vcounter       = 0;
    hcounter       = 0;
    vblank         = false;
    hblank         = false;
    dmaActive      = false;
    vintPending    = false;
    dmaFillData    = 0;
    dmaFillPending = false;
    vramDirty      = false;
	regs[1] = 0x40; // Force Display Enable bit to 1

}

// ─────────────────────────────────────────────────────────────────────────────
// Port I/O
// ─────────────────────────────────────────────────────────────────────────────
u8 GenVDP::read8(u32 off) {
    off &= 0x1Fu;
    if (off < 4) {
        const u16 w = read16(off & ~1u);
        return (off & 1u) ? static_cast<u8>(w) : static_cast<u8>(w >> 8);
    }
    if (off == 8) return static_cast<u8>((hcounter >> 1) & 0xFFu);
    if (off == 9) return static_cast<u8>(vcounter & 0xFFu);
    return 0xFFu;
}

u16 GenVDP::read16(u32 off) {
    off &= 0x1Fu;
    switch (off & 0xFEu) {
        case 0x00:
        case 0x02: return _readData();
        case 0x04:
        case 0x06: {
            const u16 s = _status();
            vintPending  = false;
            ctrlPendWord = false;  // status read aborts any pending ctrl sequence
            ctrlPendByte = false;
            return s;
        }
        case 0x08: return static_cast<u16>(((hcounter >> 1) & 0xFFu) | (vcounter << 8));
        default:   return 0xFFFFu;
    }
}

void GenVDP::write8(u32 off, u8 val) {
    off &= 0x1Fu;
    if (off < 2) {
        // Data port byte: even offset = high byte, odd = low byte
        _writeData((off & 1u) ? static_cast<u16>(val)
                               : static_cast<u16>(val) << 8);
    } else if (off < 8) {
        _writeCtrl(val, true);
    }
}

void GenVDP::write16(u32 off, u16 val) {
    off &= 0x1Fu;
    switch (off & 0xFEu) {
        case 0x00:
        case 0x02: _writeData(val);        break;
        case 0x04:
        case 0x06: _writeCtrl(val, false); break;
        default: break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Status register
// Bit 9 = FIFO not full, 8 = FIFO empty, 7 = F (VBlank pending),
// 3 = VBlank, 2 = HBlank, 1 = DMA busy, 0 = PAL
// ─────────────────────────────────────────────────────────────────────────────
u16 GenVDP::_status() {
    return 0x0200u
         | 0x0100u
         | (vintPending ? 0x0080u : 0u)
         | (vblank      ? 0x0008u : 0u)
         | (hblank      ? 0x0004u : 0u)
         | (dmaActive   ? 0x0002u : 0u)
         | (isPAL       ? 0x0001u : 0u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Control port state machine
//
// Word path:
//   First word with bits 15:14 == 10 → register write (0x8xxx)
//   First word otherwise             → first half of two-word address command
//   Second word                      → complete address/DMA command
//
// Byte path (independent state):
//   Two consecutive byte writes form a register write word.
// ─────────────────────────────────────────────────────────────────────────────
void GenVDP::_writeCtrl(u16 val, bool isByte) {
    // TRACE: See every control command sent by the CPU
    printf("[VDP CTRL] Val: 0x%04X, Byte: %d\n", val, isByte);

    if (isByte) {
        if (!ctrlPendByte) {
            ctrlFirst    = static_cast<u16>(val & 0xFFu);
            ctrlPendByte = true;
            return;
        }
        ctrlPendByte = false;
        _processCtrlWord(static_cast<u16>((ctrlFirst << 8) | (val & 0xFFu)));
        return;
    }

    // ── Word path ─────────────────────────────────────────────────────────────
    if (ctrlPendWord) {
        ctrlPendWord = false;
        const u16 w1 = ctrlFirst;
        const u16 w2 = val;
        // CD bits: CD1:CD0 from w1 bits 15:14; CD5:CD2 from w2 bits 7:4
        cdReg   = static_cast<u8>(((w1 >> 14) & 3u) | ((w2 & 0xF0u) >> 2));
        // Address: A13:A0 from w1 bits 13:0; A15:A14 from w2 bits 1:0
        addrReg = static_cast<u16>((w1 & 0x3FFFu) | ((w2 & 0x03u) << 14));

        if (cdReg & 0x20u) {
            // DMA bit set in CD field
            const u32 dmaMode = (regs[23] >> 6) & 3u;
            if (dmaMode == 2) {
                // VRAM fill: arm pending state; DMA fires on next data port write
                dmaFillPending = true;
            } else {
                dmaActive = true;
                _processDMA(cdReg);
                dmaActive = false;
            }
        }
        return;
    }

    // First word
    if ((val & 0xC000u) == 0x8000u) {
        // Register write: 1000 RRRR RRVV VVVV (bits 13:8 = reg, bits 7:0 = val)
        const u32 r = (val >> 8) & 0x1Fu;
        const u8  v = static_cast<u8>(val & 0xFFu);
        if (r < GEN_VDP_REG_COUNT) {
            regs[r] = v;
            if (r == 15) addrInc = v;
        }
        return;
    }

    // First half of two-word address command
    ctrlFirst    = val;
    ctrlPendWord = true;
}

void GenVDP::_processCtrlWord(u16 w) {
    // Byte-path words can only produce register writes
    if ((w & 0xC000u) == 0x8000u) {
        const u32 r = (w >> 8) & 0x1Fu;
        const u8  v = static_cast<u8>(w & 0xFFu);
        if (r < GEN_VDP_REG_COUNT) {
            regs[r] = v;
            if (r == 15) addrInc = v;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Data port
// ─────────────────────────────────────────────────────────────────────────────
void GenVDP::_writeData(u16 val) {
    // TRACE: See every piece of data sent to the VDP
    printf("[VDP DATA] Val: 0x%04X, Addr: 0x%04X, CD: 0x%X\n", val, addrReg, cdReg);

    dmaFillData = val;

    if (dmaFillPending) {
        // First data write after VRAM fill DMA setup triggers the fill.
        dmaFillPending = false;
        dmaActive      = true;
        _processDMA(cdReg);
        dmaActive = false;
        vramDirty = true;
        return;
    }

    const u8  cd   = cdReg & 0xFu;
    const u16 addr = addrReg;
    if      (cd == 1) {
        vram[ addr         & 0xFFFFu] = static_cast<u8>(val >> 8);
        vram[(addr + 1u)   & 0xFFFFu] = static_cast<u8>(val);
    }
    else if (cd == 3) { cram [(addr >> 1) & 0x3Fu]  = val; }
    else if (cd == 5) { vsram[(addr >> 1) & 0x27u]  = val; }
    addrReg = static_cast<u16>((addrReg + addrInc) & 0xFFFFu);
    vramDirty = true;
}

u16 GenVDP::_readData() {
    const u8  cd   = cdReg & 0xFu;
    const u16 addr = addrReg;
    u16 val = 0;
    if      (cd == 0x0) val = (static_cast<u16>(vram[addr]) << 8) | vram[(addr + 1u) & 0xFFFFu];
    else if (cd == 0x8) val = cram [(addr >> 1) & 0x3Fu];
    else if (cd == 0x4) val = vsram[(addr >> 1) & 0x27u];
    addrReg = static_cast<u16>((addrReg + addrInc) & 0xFFFFu);
    return val;
}

// ─────────────────────────────────────────────────────────────────────────────
// DMA engine
// ─────────────────────────────────────────────────────────────────────────────
void GenVDP::_processDMA(u8 cd) {
    const u32 dmaMode = (regs[23] >> 6) & 3u;
    const u32 rawLen  = (static_cast<u32>(regs[20]) << 8) | regs[19];
    const u32 dmaLen  = rawLen ? rawLen : 0x10000u;   // length=0 → 65536 words

    // Source address: registers hold word address (byte addr = value × 2)
    const u32 srcAddr = (static_cast<u32>(regs[23] & 0x7Fu) << 17)
                      | (static_cast<u32>(regs[22])         <<  9)
                      | (static_cast<u32>(regs[21])         <<  1);
    switch (dmaMode) {
        case 0: case 1: _dmaMemoryCopy(srcAddr, dmaLen, cd); break;
        case 2:         _dmaVRAMFill  (dmaLen, cd);          break;
        // Fix: JS called _dmaVRAMCopy(srcAddr, dmaLen, cd) but the function
        // signature was (len, cd) — srcAddr landed as len, dmaLen as cd.
        // VRAM copy reads its own source from registers; srcAddr is not used.
        case 3:         _dmaVRAMCopy  (dmaLen, cd);          break;
    }

    // Clear length registers after transfer (hardware behaviour)
    regs[19] = 0;
    regs[20] = 0;
    diagDmaCount++;
}

void GenVDP::_writeByCD(u16 addr, u16 val, u8 cd) {
    const u8 d = cd & 0xFu;
    if      (d == 1) { vram[addr & 0xFFFFu] = static_cast<u8>(val); }
    else if (d == 3) { cram [(addr >> 1) & 0x3Fu]  = val; }
    else if (d == 5) { vsram[(addr >> 1) & 0x27u]  = val; }
}

void GenVDP::_dmaMemoryCopy(u32 srcAddr, u32 len, u8 cd) {
    const u8 d = cd & 0xFu;
    for (u32 i = 0; i < len; i++) {
        const u8  hi   = bus->read8(srcAddr & 0xFFFFFFu);
        const u8  lo   = bus->read8((srcAddr + 1u) & 0xFFFFFFu);
        const u16 word = (static_cast<u16>(hi) << 8) | lo;
        const u16 addr = addrReg;
        if      (d == 1) { vram[addr & 0xFFFFu] = hi; vram[(addr + 1u) & 0xFFFFu] = lo; }
        else if (d == 3) { cram [(addr >> 1) & 0x3Fu]  = word; }
        else if (d == 5) { vsram[(addr >> 1) & 0x27u]  = word; }
        srcAddr = (srcAddr + 2u) & 0xFFFFFFu;
        addrReg = static_cast<u16>((addrReg + addrInc) & 0xFFFFu);
    }
    vramDirty = true;
}

void GenVDP::_dmaVRAMFill(u32 len, u8 cd) {
    // Fill with the HIGH byte of the last data port write.
    // The destination is always VRAM for fill mode (cd bit 0 = 1 means VRAM).
    const u8 fillByte = static_cast<u8>(dmaFillData >> 8);
    const u8 d        = cd & 0xFu;
    for (u32 i = 0; i < len; i++) {
        if (d == 1) vram[addrReg & 0xFFFFu] = fillByte;
        addrReg = static_cast<u16>((addrReg + addrInc) & 0xFFFFu);
    }
    vramDirty = true;
}

void GenVDP::_dmaVRAMCopy(u32 len, u8 /*cd*/) {
    // Fix: JS used ((regs[23] & 0x7F) << 8) | regs[22] which ignores reg 21
    // entirely and mixes the DMA-mode register into the address.
    // Correct: VRAM copy source is a 16-bit VRAM address in regs[22]:regs[21].
    u16 src = (static_cast<u16>(regs[22]) << 8) | regs[21];
    for (u32 i = 0; i < len; i++) {
        vram[addrReg & 0xFFFFu] = vram[src & 0xFFFFu];
        src     = (src     + 1u) & 0xFFFFu;
        addrReg = static_cast<u16>((addrReg + addrInc) & 0xFFFFu);
    }
    vramDirty = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Scanline timing hooks
// ─────────────────────────────────────────────────────────────────────────────
bool GenVDP::tickLine(u32 line, bool pal) {
    vcounter = static_cast<u16>(line);
    const u32 activeH = pal ? PAL_ACTIVE : NTSC_ACTIVE;
    vblank = (line >= activeH);
    hblank = false;
    _renderLine(line);
    const bool doVBlank = (line == activeH);
    if (doVBlank) vintPending = true;
    // Note: frame counter is incremented by Genesis::runFrame, not here
    return doVBlank;
}

bool GenVDP::checkHInt(u32 line, bool pal) {
    if (!(regs[0] & 0x10u)) return false;   // HInt disabled in reg 0 bit 4
    const u32 activeH = pal ? PAL_ACTIVE : NTSC_ACTIVE;
    if (line >= activeH) return false;
    const u32 cnt = regs[10];
    return (line % (cnt + 1u)) == 0u;
}

// ─────────────────────────────────────────────────────────────────────────────
// Rendering
// ─────────────────────────────────────────────────────────────────────────────
void GenVDP::_renderLine(u32 line) {
    // FIX: Use explicit size to prevent crashes. 
    // This clears the screen to black at the start of every frame.
    if (line == 0) std::memset(framebuf, 0, GEN_W * GEN_H_MAX * 4);
    
    const u32 activeH = isPAL ? GEN_H_PAL : GEN_H_NTSC;
    if (line < activeH && (regs[1] & 0x40u)) {
        _renderScanline(line);
    }
}


void GenVDP::_renderScanline(u32 y) {
    _renderPlaneLine(true,  y);  // Plane B — drawn first (behind)
    _renderPlaneLine(false, y);  // Plane A — drawn second (in front of B)
    _renderSpriteLine(y);        // Sprites — drawn last (on top)
}



// ─────────────────────────────────────────────────────────────────────────────
// Plane rendering (Plane A or Plane B)
//
// Nametable layout (per entry, 16 bits):
//   Bits 15:13 = palette line (0–3)
//   Bit  12    = vertical flip
//   Bit  11    = horizontal flip
//   Bits 10:0  = tile index (0–2047)
// ─────────────────────────────────────────────────────────────────────────────
void GenVDP::_renderPlaneLine(bool isB, u32 y) {
    // Scroll size register (reg 16): bits 1:0 = H size, bits 5:4 = V size
    // Values: 0=32, 1=64, 2=invalid(0), 3=128 tiles
    static const u32 sizeLut[4] = { 32, 64, 0, 128 };
    const u32 hsize = sizeLut[regs[16]        & 3u];
    const u32 vsize = sizeLut[(regs[16] >> 4) & 3u];
    if (!hsize || !vsize) return;

    // Nametable base addresses
    // Plane A: reg 2 bits 5:3 → VRAM bits 15:13
    // Plane B: reg 4 bits 2:0 → VRAM bits 15:13
    const u32 planeBase = isB ? (static_cast<u32>(regs[4] & 0x07u) << 13)
                               : (static_cast<u32>(regs[2] & 0x38u) << 10);

    // HScroll table (reg 13 bits 5:0 → VRAM bits 15:10)
    const u32 hscBase  = static_cast<u32>(regs[13] & 0x3Fu) << 10;
    const u32 hscMode  = regs[11] & 0x03u;   // 0=full, 2=cell, 3=line
    const u32 planeOff = isB ? 2u : 0u;       // Plane B entry is 2 bytes after A

    const u32 hscLineOff = (hscMode == 3) ? (y * 4u)
                         : (hscMode == 2) ? ((y >> 3) * 4u)
                         : 0u;
    const u32 hscAddr = (hscBase + hscLineOff + planeOff) & 0xFFFFu;
    // VDP stores the complement of the scroll: effective_x = (screen_x + stored_complement) & mask
    const u32 raw_hs  = (static_cast<u32>(vram[hscAddr]) << 8) | vram[hscAddr + 1u];
    const u32 hscroll = (0x400u - raw_hs) & 0x3FFu;

    // VScroll (simplified: full-screen vertical scroll only; line/cell not implemented)
    const u32 vscroll = static_cast<u32>(isB ? vsram[1] : vsram[0]) & 0x3FFu;

    const u32 scrollY = (y + vscroll) & (vsize * 8u - 1u);
    const u32 tileRow = scrollY >> 3;
    const u32 fineY   = scrollY & 7u;

    for (u32 x = 0; x < GEN_W; x++) {
        const u32 scrollX = (x + hscroll) & (hsize * 8u - 1u);
        const u32 tileCol = scrollX >> 3;
        const u32 fineX   = scrollX & 7u;

        // Nametable address for this tile
        const u32 ntAddr = (planeBase
                         + ((tileRow & (vsize - 1u)) * hsize
                         +  (tileCol & (hsize - 1u))) * 2u) & 0xFFFFu;
        const u16 entry  = (static_cast<u16>(vram[ntAddr]) << 8) | vram[ntAddr + 1u];

        const u32 tileIdx = entry & 0x7FFu;
        const u32 palLine = (entry >> 13) & 3u;
        const u32 row     = (entry & 0x1000u) ? (7u - fineY) : fineY;  // bit 12 = vFlip
        const u32 col     = (entry & 0x0800u) ? (7u - fineX) : fineX;  // bit 11 = hFlip

        // 4bpp tile: 32 bytes per tile (8 rows × 4 bytes)
        // Two pixels per byte: high nibble = left pixel, low nibble = right pixel
        const u32 byteAddr = (tileIdx * 32u + row * 4u + (col >> 1)) & 0xFFFFu;
        const u8  byte_    = vram[byteAddr];
        const u8  nibble   = (col & 1u) ? (byte_ & 0xFu) : ((byte_ >> 4) & 0xFu);
        if (nibble == 0) continue;  // colour 0 = transparent

        const RGB rgb = _decodeCRAMColor(cram[(palLine * 16u + nibble) & 0x3Fu]);
        const u32 pi  = (y * GEN_W + x) * 4u;
        framebuf[pi]     = rgb.r;
        framebuf[pi + 1] = rgb.g;
        framebuf[pi + 2] = rgb.b;
        framebuf[pi + 3] = 255;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Sprite rendering for one scanline
//
// Sprite attribute table entry (8 bytes per sprite):
//   +0,+1 : Y position (biased by 128 — value 128 = screen Y 0)
//   +2    : size (bits 3:2 = H cells - 1, bits 1:0 = V cells - 1)
//   +3    : link (index of next sprite in chain; 0 = end)
//   +4,+5 : attribute word (palLine, vFlip, hFlip, tile index)
//   +6,+7 : X position (biased by 128)
//
// Tile order within a multi-cell sprite: column-major
//   (each column is filled top-to-bottom before moving to the next column)
// ─────────────────────────────────────────────────────────────────────────────
void GenVDP::_renderSpriteLine(u32 y) {
    const u32 sprBase = static_cast<u32>(regs[5] & 0x7Fu) << 9;
    const u32 maxSpr  = (regs[12] & 1u) ? 80u : 64u;

    // Walk the link chain and collect sprites that cover this scanline.
    // Limit to 20 per line (H40) / 16 per line (H32) per hardware spec.
    const u32 lineLimit = (regs[12] & 1u) ? 20u : 16u;

    struct SprInfo { s32 sx, sy; u32 hCells, vCells; u16 attr; };
    SprInfo sprites[80];
    u32     sprCount = 0;

    u32 link = 0;
    for (u32 n = 0; n < maxSpr; n++) {
        const u32 base  = (sprBase + link * 8u) & 0xFFFFu;
        const u32 yraw  = ((static_cast<u32>(vram[base])     << 8) | vram[base + 1u]) & 0x3FFu;
        const u8  sz    = vram[base + 2u];
        link            = vram[base + 3u] & 0x7Fu;
        const u16 attr  = (static_cast<u16>(vram[base + 4u]) << 8) | vram[base + 5u];
        const u32 xraw  = ((static_cast<u32>(vram[base + 6u]) << 8) | vram[base + 7u]) & 0x1FFu;

        const s32 sy     = static_cast<s32>(yraw) - 128;
        const u32 vCells = (sz & 3u) + 1u;

        if (static_cast<s32>(y) >= sy &&
            static_cast<s32>(y) <  sy + static_cast<s32>(vCells * 8u) &&
            sprCount < lineLimit)
        {
            sprites[sprCount++] = {
                static_cast<s32>(xraw) - 128,
                sy,
                ((sz >> 2) & 3u) + 1u,
                vCells,
                attr
            };
        }
        if (link == 0) break;
    }

    // Draw back-to-front so sprite 0 (first in chain, highest priority) wins
    for (s32 si = static_cast<s32>(sprCount) - 1; si >= 0; si--) {
        const SprInfo& spr = sprites[si];
        const u32  tileIdx = spr.attr & 0x7FFu;
        const u32  palLine = (spr.attr >> 13) & 3u;
        const bool vFlip   = (spr.attr & 0x1000u) != 0u;
        const bool hFlip   = (spr.attr & 0x0800u) != 0u;

        const u32 localY = static_cast<u32>(static_cast<s32>(y) - spr.sy);
        const u32 cyRaw  = localY >> 3;
        const u32 cy     = vFlip ? (spr.vCells - 1u - cyRaw) : cyRaw;
        const u32 row    = vFlip ? (7u - (localY & 7u)) : (localY & 7u);

        for (u32 cx = 0; cx < spr.hCells; cx++) {
            // Column-major tile order: tile at (cx, cy) = tileIdx + cx*vCells + cy
            const u32 tile = (tileIdx + cx * spr.vCells + cy) & 0x7FFu;

            for (u32 col = 0; col < 8u; col++) {
                const u32 c  = hFlip ? (7u - col) : col;
                const s32 sx = spr.sx + static_cast<s32>(cx * 8u + col);
                if (sx < 0 || sx >= static_cast<s32>(GEN_W)) continue;

                const u32 bAddr  = (tile * 32u + row * 4u + (c >> 1)) & 0xFFFFu;
                const u8  nibble = (c & 1u) ? (vram[bAddr] & 0xFu)
                                            : ((vram[bAddr] >> 4) & 0xFu);
                if (nibble == 0) continue;

                const RGB rgb = _decodeCRAMColor(cram[(palLine * 16u + nibble) & 0x3Fu]);
                const u32 pi  = (y * GEN_W + static_cast<u32>(sx)) * 4u;
                framebuf[pi]     = rgb.r;
                framebuf[pi + 1] = rgb.g;
                framebuf[pi + 2] = rgb.b;
                framebuf[pi + 3] = 255;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CRAM color decode
//
// Genesis CRAM entry format: 0000 BBB0 GGG0 RRR0
// Each component is 3 bits sitting in nibble positions.
//
// Fix: JS used * 36 which maps 0→0 and 7→252 (not 255).
// Standard 3-bit → 8-bit bit-replication: (v<<5)|(v<<2)|(v>>1)
//   gives 0→0 and 7→(224|28|3)=255 exactly.
// ─────────────────────────────────────────────────────────────────────────────
GenVDP::RGB GenVDP::_decodeCRAMColor(u16 color) {
    const u32 r = (color >> 1) & 7u;   // bits  3:1
    const u32 g = (color >> 5) & 7u;   // bits  7:5
    const u32 b = (color >> 9) & 7u;   // bits 11:9
    return {
        static_cast<u8>((r << 5) | (r << 2) | (r >> 1)),
        static_cast<u8>((g << 5) | (g << 2) | (g >> 1)),
        static_cast<u8>((b << 5) | (b << 2) | (b >> 1))
    };
}
