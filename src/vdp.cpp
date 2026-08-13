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
    spriteOverflow  = false;
    spriteCollision = false;
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
    spriteOverflow  = false;
    spriteCollision = false;
	regs[1] = 0x40; // Force Display Enable bit to 1

}

// ─────────────────────────────────────────────────────────────────────────────
// Port I/O
// ─────────────────────────────────────────────────────────────────────────────
u8 GenVDP::read8(u32 off) {
    off &= 0x1Fu;
    // Offsets 0-3 = data port, 4-7 = control/status port. Both are
    // byte-accessible mirrors of a 16-bit register. Previously only
    // 0-3 were handled here; byte reads to the status port (4-7) fell
    // through to the 0xFF default, meaning every status bit — DMA busy
    // included — always read as "set" via a byte access. Any game that
    // polls DMA-busy or VBlank through a byte read to $C00005 (a common
    // pattern) would spin forever waiting for a bit that could never
    // clear. read16 already carries the correct status-read side effects
    // (clearing vintPending/sprite flags), which byte reads should share.
    if (off < 8) {
        const u16 w = read16(off & ~1u);
        return (off & 1u) ? static_cast<u8>(w) : static_cast<u8>(w >> 8);
    }
    if (off == 8) return static_cast<u8>((hcounter >> 1) & 0xFFu);
    if (off == 9) {
        // Interlace mode 2 (reg 12 bits 1:0 == 11) doubles VCounter's LSB
        // resolution — real hardware shifts the internal line count left
        // by 1 and ORs in the field bit before truncating to the 8-bit
        // port. Mode 1 and normal mode read VCounter unmodified.
        const bool im2 = (regs[12] & 0x03u) == 0x03u;
        const u32  v   = im2 ? ((vcounter << 1) | (frame & 1u)) : vcounter;
        return static_cast<u8>(v & 0xFFu);
    }
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
            vintPending     = false;
            spriteOverflow  = false;
            spriteCollision = false;
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
        // High byte (off 0) or Low byte (off 1)
        _writeVRAMByte((off & 1u) ? 1 : 0, val); 
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
         | (vintPending     ? 0x0080u : 0u)
         | (spriteOverflow  ? 0x0040u : 0u)
         | (spriteCollision ? 0x0020u : 0u)
         | (vblank          ? 0x0008u : 0u)
         | (hblank          ? 0x0004u : 0u)
         | (dmaActive       ? 0x0002u : 0u)
         | (isPAL           ? 0x0001u : 0u);
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

    // FIX: Register writes MUST take priority and abort pending address sequences
    if ((val & 0xC000u) == 0x8000u) {
        ctrlPendWord = false; 
        const u32 r = (val >> 8) & 0x1Fu;
        const u8  v = static_cast<u8>(val & 0xFFu);
        if (r < GEN_VDP_REG_COUNT) {
            regs[r] = v;

            if (r == 15) addrInc = v;
        }
        return;
    }

    if (ctrlPendWord) {
        ctrlPendWord = false;
        const u16 w1 = ctrlFirst;
        const u16 w2 = val;
        // CD1:CD0 = w1 bits 15:14, CD5:CD2 = w2 bits 7:4.
        // (Previously CD2/CD3 were never sourced — always 0 — which made
        // CRAM read (needs CD3) and VSRAM read (needs CD2) unreachable.)
        cdReg = static_cast<u8>(((w1 >> 14) & 0x03u) | ((w2 >> 2) & 0x3Cu));
        addrReg = static_cast<u32>((w1 & 0x3FFFu) | ((w2 & 0x03u) << 14));

if (cdReg & 0x20u) {
            // Real hardware gates all DMA (fill, copy, and 68K→VRAM transfer)
            // on reg 1 bit 4. If it's clear, the CD5 bit still gets latched
            // above but no transfer — pending or otherwise — happens.
            if (!(regs[1] & 0x10u)) return;

            const u32 dmaMode = (regs[23] >> 6) & 3u;
            if (dmaMode == 2) dmaFillPending = true; 
            else {
                dmaActive = true;
                _processDMA(cdReg);
                dmaActive = false;
            }
        }
        return;
    }

    ctrlFirst = val;
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
    dmaFillData = val;

    if (dmaFillPending) {
        dmaFillPending = false;
        if (regs[1] & 0x10u) {
            dmaActive = true;
            _processDMA(cdReg);
            dmaActive = false;
        }
        return;
    }

    // Word write: write high byte, then low byte
    _writeVRAMByte(0, static_cast<u8>(val >> 8));
    _writeVRAMByte(1, static_cast<u8>(val & 0xFF));
    addrReg = (addrReg + addrInc) & 0x3FFFFu;
    vramDirty = true;
}

void GenVDP::_writeVRAMByte(int bytePos, u8 val) {
    const u8 cd = cdReg;
    const u16 addr = addrReg;

    if ((cd & 0x0F) == 1) {
        // Odd-address byte swap (Charles MacDonald hw notes): a word write
        // to an odd VRAM address lands with hi/lo swapped relative to the
        // normal even-address case. Only affects raw VRAM writes, not CRAM/VSRAM.
        const int pos = (addr & 1u) ? (1 - bytePos) : bytePos;
        vram[(addr + pos) & 0xFFFFu] = val;
    } else if (cd == 3) {
        u16 current = cram[(addr >> 1) & 0x3Fu];
        if (bytePos == 0) current = (current & 0x00FF) | (val << 8);
        else             current = (current & 0xFF00) | val;
        cram[(addr >> 1) & 0x3Fu] = current;
    } else if (cd == 5) {
        u16 current = vsram[(addr >> 1) & 0x27u];
        if (bytePos == 0) current = (current & 0x00FF) | (val << 8);
        else             current = (current & 0xFF00) | val;
        vsram[(addr >> 1) & 0x27u] = current;
    }

}


u16 GenVDP::_readData() {
    const u8 cd = cdReg;
    const u16 addr = addrReg;
    u16 val = 0;
    if      (cd == 0x0) val = (static_cast<u16>(vram[addr]) << 8) | vram[(addr + 1u) & 0xFFFFu];
    else if (cd == 0x8) val = cram [(addr >> 1) & 0x3Fu];
    else if (cd == 0x4) val = vsram[(addr >> 1) & 0x27u];
    addrReg = (addrReg + addrInc) & 0x3FFFFu;
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

// ── Stall the 68K for the transfer duration ──────────────────────────────
    // DMA locks the 68K bus on real hardware; previously this ran "free"
    // (zero CPU cycles charged), so games timing off DMA completion would
    // desync. Memory→VRAM copy moves a 16-bit word per length unit; VRAM
    // fill and VRAM→VRAM copy move a single byte per length unit.
    //
    // Real hardware transfers noticeably faster during blanking than during
    // active display (roughly 3-4x). Games commonly queue their bulk level
    // DMA specifically during VBlank expecting that faster rate — charging
    // the slower active-display rate uniformly (as an earlier flat-2 version
    // of this did) makes every vblank fall further behind than real
    // hardware would, which can visibly delay or stall games that lean
    // heavily on DMA-during-vblank (e.g. Sonic's DMA queue processor).
    const u32 DMA_CYCLES_PER_BYTE = vblank ? 1u : 2u;
    const u32 bytesMoved = (dmaMode <= 1u) ? (dmaLen * 2u) : dmaLen;
    if (bus && bus->m68k)
        bus->m68k->cycles += bytesMoved * DMA_CYCLES_PER_BYTE;

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
    // The source address increments only within its 128KB (17-bit) window
    // — the window covered by regs21/regs22. The bank bits from regs23
    // (bits 23:17) are never touched by the increment on real hardware, so
    // a transfer that crosses a 128KB boundary wraps back to the start of
    // the SAME bank instead of rolling into the next one. Previously the
    // full 24-bit srcAddr was incremented and masked, letting the carry
    // propagate into the bank bits — wrong for any transfer that crosses
    // that boundary.
    const u32 bank = srcAddr & 0xFE0000u;
    u32 bankOff    = srcAddr & 0x01FFFFu;
    for (u32 i = 0; i < len; i++) {
        const u32 curSrc = bank | bankOff;
        const u8  hi   = bus->read8(curSrc);
        const u8  lo   = bus->read8((bank | ((bankOff + 1u) & 0x01FFFFu)));
        const u16 word = (static_cast<u16>(hi) << 8) | lo;
        const u16 addr = addrReg;
        if (d == 1) {
            // Odd-address byte swap, same rule as _writeVRAMByte: a word
            // landing on an odd VRAM address gets hi/lo swapped relative
            // to the even-address case. DMA previously wrote hi/lo straight
            // through regardless of addr parity, diverging from real hardware.
            if (addr & 1u) { vram[addr & 0xFFFFu] = lo; vram[(addr + 1u) & 0xFFFFu] = hi; }
            else            { vram[addr & 0xFFFFu] = hi; vram[(addr + 1u) & 0xFFFFu] = lo; }
        }
        else if (d == 3) { cram [(addr >> 1) & 0x3Fu]  = word; }
        else if (d == 5) { vsram[(addr >> 1) & 0x27u]  = word; }
        bankOff = (bankOff + 2u) & 0x01FFFFu;
        addrReg = static_cast<u16>((addrReg + addrInc) & 0xFFFFu);
    }
    vramDirty = true;
}

void GenVDP::_dmaVRAMFill(u32 len, u8 cd) {
    const u8 fillByte = static_cast<u8>(dmaFillData >> 8);
    for (u32 i = 0; i < len; i++) {
        vram[addrReg & 0xFFFFu] = fillByte;
        // Fill honors the auto-increment register (reg 15), same as normal
        // data-port writes — was hardcoded to +1, which only happened to
        // match the common case where addrInc==1 during fills.
        addrReg = (addrReg + addrInc) & 0xFFFFu;
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
        // Source always steps by 1 byte per hardware behavior — only the
        // destination (addrReg) honors addrInc.
        src     = (src + 1u) & 0xFFFFu;
        addrReg = (addrReg + addrInc) & 0xFFFFu;
    }
    vramDirty = true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Scanline timing hooks
// ─────────────────────────────────────────────────────────────────────────────
bool GenVDP::tickLine(u32 line, bool pal) {
    // HV-counter freeze (reg 0 bit 1): when set, VCounter (and HCounter,
    // which we don't model sub-line anyway) holds its last value instead
    // of advancing. Games use this for stable mid-frame raster reads.
    if (!(regs[0] & 0x02u))
        vcounter = static_cast<u16>(line);

    const u32 activeH = pal ? PAL_ACTIVE : NTSC_ACTIVE;
    vblank = (line >= activeH);
    hblank = false;
    _renderLine(line);

    const bool doVBlank = (line == activeH);

    if (doVBlank) {
        vintPending = true;
    }

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
    std::memset(lineA,      0, sizeof(lineA));
    std::memset(lineB,      0, sizeof(lineB));
    std::memset(lineSpr,    0, sizeof(lineSpr));
    std::memset(lineShMark, 0, sizeof(lineShMark));

_renderPlaneLine(true,  y);  // Plane B
    _renderPlaneLine(false, y);  // Plane A
    _renderWindowLine(y);        // Window — overlays Plane A region only
    _renderSpriteLine(y);        // Sprites

    _compositeLine(y);
}

// ─────────────────────────────────────────────────────────────────────────────
// Compositor — applies the fixed Genesis priority stack:
//   backdrop < B(lo) < A(lo) < Spr(lo) < B(hi) < A(hi) < Spr(hi)
// Reg 7 bits 3:0 select the backdrop CRAM entry (palette line 0's 16-entry
// row is NOT implied — reg 7 indexes the full 64-entry CRAM directly via
// bits 5:0, but only bits 3:0 are used on real hardware since the backdrop
// is always drawn from palette line (reg7>>4)&3, index reg7&0xF).
// ─────────────────────────────────────────────────────────────────────────────
void GenVDP::_compositeLine(u32 y) {
    const u32 backdropIdx = ((static_cast<u32>(regs[7]) >> 4) & 3u) * 16u
                           + (regs[7] & 0xFu);

    for (u32 x = 0; x < GEN_W; x++) {
        const u8 a = lineA[x];
        const u8 b = lineB[x];
        const u8 s = lineSpr[x];

        const bool aHi = (a & 0x80u) != 0u, aOn = (a & 0x7Fu) != 0u;
        const bool bHi = (b & 0x80u) != 0u, bOn = (b & 0x7Fu) != 0u;
        const bool sHi = (s & 0x80u) != 0u, sOn = (s & 0x7Fu) != 0u;

u32 colorIdx = backdropIdx;
        bool found = false;
        bool winnerIsHiPrio = false;

        // High-priority layer, in stack order Spr > A > B (top wins)
        if (!found && sOn && sHi) { colorIdx = s & 0x7Fu; found = true; winnerIsHiPrio = true; }
        if (!found && aOn && aHi) { colorIdx = a & 0x7Fu; found = true; winnerIsHiPrio = true; }
        if (!found && bOn && bHi) { colorIdx = b & 0x7Fu; found = true; winnerIsHiPrio = true; }
        // Low-priority layer, same stack order
        if (!found && sOn)        { colorIdx = s & 0x7Fu; found = true; }
        if (!found && aOn)        { colorIdx = a & 0x7Fu; found = true; }
        if (!found && bOn)        { colorIdx = b & 0x7Fu; found = true; }

        RGB rgb = _decodeCRAMColor(cram[colorIdx & 0x3Fu]);

        // Shadow/Highlight (item 6). Only active when reg 12 bit 3 is set.
        // Rule: anything at high priority renders at normal brightness (it's
        // "above" the shadow layer entirely). Anything at low priority is
        // shadowed by default, UNLESS a sprite operator pixel underneath it
        // forces highlight or shadow explicitly for that x position.
        if (regs[12] & 0x08u) {
            const u8 mark = lineShMark[x];
            bool shadow = false, highlight = false;
            if (winnerIsHiPrio) {
                // High-priority pixels ignore operator marks — always normal.
            } else if (mark == 1u) {
                highlight = true;
            } else if (mark == 2u) {
                shadow = true;
            } else {
                shadow = true;  // default: low-priority layer is shadowed
            }

            if (shadow) {
                rgb.r = static_cast<u8>(rgb.r >> 1);
                rgb.g = static_cast<u8>(rgb.g >> 1);
                rgb.b = static_cast<u8>(rgb.b >> 1);
            } else if (highlight) {
                rgb.r = static_cast<u8>(0x80u + (rgb.r >> 1));
                rgb.g = static_cast<u8>(0x80u + (rgb.g >> 1));
                rgb.b = static_cast<u8>(0x80u + (rgb.b >> 1));
            }
        }

        const u32 pi  = (y * GEN_W + x) * 4u;
        framebuf[pi]     = rgb.r;
        framebuf[pi + 1] = rgb.g;
        framebuf[pi + 2] = rgb.b;
        framebuf[pi + 3] = 255;
    }
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

// VScroll mode: reg 11 bit 2 set = per-16px-column, else full-screen.
    // Column mode indexes VSRAM in pairs: [2*col]=Plane A, [2*col+1]=Plane B
    // — NOT [isB] like the full-screen case, so this can't reuse vscroll
    // computed once outside the x-loop; it has to be resampled per column.
    const bool vscPerCol = (regs[11] & 0x04u) != 0u;
    const u32  vscrollFull = static_cast<u32>(isB ? vsram[1] : vsram[0]) & 0x3FFu;

    for (u32 x = 0; x < GEN_W; x++) {
        u32 vscroll = vscrollFull;
        if (vscPerCol) {
            const u32 vcol = (x >> 4) & 0x13u;  // 16px columns, 20 columns across 320px
            const u32 vIdx = vcol * 2u + (isB ? 1u : 0u);
            if (vIdx < GEN_VSRAM_WORDS)
                vscroll = static_cast<u32>(vsram[vIdx]) & 0x3FFu;
        }
        const u32 scrollY = (y + vscroll) & (vsize * 8u - 1u);
        const u32 tileRow = scrollY >> 3;
        const u32 fineY   = scrollY & 7u;

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
        const bool prio   = (entry & 0x8000u) != 0u;  // bit 15 = priority
        const u32 row     = (entry & 0x1000u) ? (7u - fineY) : fineY;  // bit 12 = vFlip
        const u32 col     = (entry & 0x0800u) ? (7u - fineX) : fineX;  // bit 11 = hFlip

        // 4bpp tile: 32 bytes per tile (8 rows × 4 bytes)
        // Two pixels per byte: high nibble = left pixel, low nibble = right pixel
        const u32 byteAddr = (tileIdx * 32u + row * 4u + (col >> 1)) & 0xFFFFu;
        const u8  byte_    = vram[byteAddr];
        const u8  nibble   = (col & 1u) ? (byte_ & 0xFu) : ((byte_ >> 4) & 0xFu);

u8* line = isB ? lineB : lineA;
        if (nibble == 0) { line[x] = 0; continue; }  // colour 0 = transparent

        const u8 colorIdx = static_cast<u8>((palLine * 16u + nibble) & 0x3Fu);
        line[x] = colorIdx | (prio ? 0x80u : 0u);
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Window plane (item 2). Window is Plane-A-only on real hardware: where the
// window region covers this pixel, it fully replaces Plane A's tile (not
// blended) using its own fixed, unscrolled nametable. Reg 17/18 select the
// region; reg 3 selects the window's own nametable base.
//
// Reg 17 (H pos): bit7 = 0 left-side / 1 right-side, bits4:0 = position in
//                 8px column units.
// Reg 18 (V pos): bit7 = 0 top-side  / 1 bottom-side, bits4:0 = position in
//                 8px row units.
// ─────────────────────────────────────────────────────────────────────────────
void GenVDP::_renderWindowLine(u32 y) {
    const u32 wCol   = regs[17] & 0x1Fu;
    const bool wRight = (regs[17] & 0x80u) != 0u;
    const u32 wRow   = regs[18] & 0x1Fu;
    const bool wBottom = (regs[18] & 0x80u) != 0u;

    // No window active at all: H pos 0 with left-side selected (and V pos 0
    // top-side) means "no window", which is the common no-window register
    // state games leave behind. Real hardware would still technically apply
    // an empty region here so this is just a fast-path skip, not a special case.
    if (wCol == 0u && !wRight && wRow == 0u && !wBottom) return;

    const u32 rowTile = y >> 3;
    const bool rowInWindow = wBottom ? (rowTile >= wRow) : (rowTile < wRow);

    // H40 vs H32 determines how many 32-tile-wide rows the window nametable
    // spans and its base alignment, same convention as the sprite table.
    const bool h40 = (regs[12] & 1u) != 0u;
    const u32 hsizeTiles = h40 ? 64u : 32u;   // window nametable pitch in tiles
    const u32 screenTilesW = h40 ? 40u : 32u;

    // Window base address: reg 3 bits 5:1 (H40 ignores bit 1, same as sprite
    // table alignment rule) — bits 6:1 relevant in H32.
    const u32 winBase = static_cast<u32>(regs[3] & (h40 ? 0x3Cu : 0x3Eu)) << 10;

    const u32 fineY = y & 7u;

    for (u32 x = 0; x < GEN_W; x++) {
        const u32 col = x >> 3;
        if (col >= screenTilesW) continue;

        const bool colInWindow = wRight ? (col >= wCol * 2u) : (col < wCol * 2u);
        // Window region is the union: a row that's in the window applies
        // across the whole line if H pos is 0/left, and vice versa — but
        // per Sega's documented behavior when BOTH H and V are set, the
        // window still only covers the specific horizontal OR vertical
        // strip, not their intersection. We treat "in window" as: for a
        // window row, it covers horizontally per wCol; for a window col,
        // it covers vertically per wRow. Simplify to OR of the two active regions.
        const bool inWindow = (wRow != 0u || wBottom) ? rowInWindow
                             : (wCol != 0u || wRight) ? colInWindow
                             : false;
        if (!inWindow) continue;

        const u32 fineX = x & 7u;
        const u32 ntAddr = (winBase + (rowTile * hsizeTiles + col) * 2u) & 0xFFFFu;
        const u16 entry  = (static_cast<u16>(vram[ntAddr]) << 8) | vram[ntAddr + 1u];

        const u32 tileIdx = entry & 0x7FFu;
        const u32 palLine = (entry >> 13) & 3u;
        const bool prio   = (entry & 0x8000u) != 0u;
        const u32 row     = (entry & 0x1000u) ? (7u - fineY) : fineY;
        const u32 c       = (entry & 0x0800u) ? (7u - fineX) : fineX;

        const u32 byteAddr = (tileIdx * 32u + row * 4u + (c >> 1)) & 0xFFFFu;
        const u8  byte_    = vram[byteAddr];
        const u8  nibble   = (c & 1u) ? (byte_ & 0xFu) : ((byte_ >> 4) & 0xFu);

        if (nibble == 0) { lineA[x] = 0; continue; }

        const u8 colorIdx = static_cast<u8>((palLine * 16u + nibble) & 0x3Fu);
        lineA[x] = colorIdx | (prio ? 0x80u : 0u);
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
    const bool h40 = (regs[12] & 1u) != 0u;
    // H40 mode: table must be 0x400-aligned, so bit0 of reg5 is ignored by
    // hardware. H32 mode: table is 0x200-aligned, bit0 is significant.
    const u32 sprBase = static_cast<u32>(regs[5] & (h40 ? 0x7Eu : 0x7Fu)) << 9;
    const u32 maxSpr  = h40 ? 80u : 64u;

    // Walk the link chain and collect sprites that cover this scanline.
    // Limit to 20 per line (H40) / 16 per line (H32) per hardware spec.
    const u32 lineLimit = h40 ? 20u : 16u;

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

const bool onLine = static_cast<s32>(y) >= sy &&
                             static_cast<s32>(y) <  sy + static_cast<s32>(vCells * 8u);

        if (onLine && sprCount >= lineLimit) {
            spriteOverflow = true;  // sticky until status register is read
        } else if (onLine) {
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
        const bool prio    = (spr.attr & 0x8000u) != 0u;  // bit 15 = priority
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

                // Collision: another opaque sprite pixel already landed here
                // this scanline. Checked before overwrite so draw order
                // (back-to-front priority resolution) doesn't mask it.
                if ((lineSpr[static_cast<u32>(sx)] & 0x7Fu) != 0u)
                    spriteCollision = true;

                // S/H operator colors: palette line 3, index 14 = highlight,
                // index 15 = shadow. These mark the pixel below rather than
                // drawing a real color themselves, and only take effect when
                // S/H mode is enabled (reg 12 bit 3) — with it off they're
                // just ordinary opaque pixels using CRAM entry 3*16+14/15.
                if (palLine == 3u && (nibble == 14u || nibble == 15u) && (regs[12] & 0x08u)) {
                    lineShMark[static_cast<u32>(sx)] = (nibble == 14u) ? 1u : 2u;
                    continue;
                }

                const u8 colorIdx = static_cast<u8>((palLine * 16u + nibble) & 0x3Fu);
                lineSpr[static_cast<u32>(sx)] = colorIdx | (prio ? 0x80u : 0u);
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
