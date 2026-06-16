#include "genesis.h"
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// GenBus
// ─────────────────────────────────────────────────────────────────────────────

GenBus::GenBus()
    : hasSRAM(false)
    , sramDirty(false)
    , sramStart(0)
    , sramEnd(0)
    , sramSize(0)
    , z80BusReq(false)
    , z80Reset(true)
    , z80Bank(0)
    , vdp(nullptr)
    , z80(nullptr)
    , apu(nullptr)
    , m68k(nullptr)
{
    std::memset(wram,     0, sizeof(wram));
    std::memset(z80Ram,   0, sizeof(z80Ram));
    std::memset(sramData, 0, sizeof(sramData));
    std::memset(padState, 0, sizeof(padState));

    padCtrl[0] = 0x40;  padCtrl[1] = 0x40;
    padTH[0]   = 1;     padTH[1]   = 1;
}

void GenBus::reset() {
    std::memset(wram,   0, sizeof(wram));
    std::memset(z80Ram, 0, sizeof(z80Ram));
    std::memset(padState, 0, sizeof(padState));

    padCtrl[0] = 0x40;  padCtrl[1] = 0x40;
    padTH[0]   = 1;     padTH[1]   = 1;
    z80BusReq  = false;
    z80Reset   = true;
    z80Bank    = 0;
    sramDirty  = false;
}

// ─────────────────────────────────────────────────────────────────────────────
// ROM loading
// Parses the ROM header at 0x1B0 for battery-backed SRAM.
//
// Header layout (all big-endian):
//   0x1B0–0x1B1  "RA" magic (0x52 0x41) if SRAM present
//   0x1B2        type byte — must have bits 7 and 5 set (0xA0 mask) for SRAM
//   0x1B4–0x1B7  SRAM start address
//   0x1B8–0x1BB  SRAM end address
// ─────────────────────────────────────────────────────────────────────────────
void GenBus::loadROM(const u8* data, u32 size) {
    // Check for Sega Header (first 512 bytes)
    // A real header usually starts with a specific pattern or has a 
    // valid ROM ID at 0x100.
    bool hasHeader = false;
    if (size >= 0x200) {
        // Check for the "SEGA" string or specific header markers
        // Most common: address 0x100 contains the console name
        if (data[0x100] == 'S' && data[0x101] == 'e' && data[0x102] == 'g') {
            hasHeader = true;
        }
    }

    if (hasHeader) {
        // Skip the 512-byte header for the main ROM array
        rom.assign(data + 0x200, data + size);
    } else {
        rom.assign(data, data + size);
    }

    hasSRAM   = false;
    sramDirty = false;
    sramStart = 0;
    sramEnd   = 0;
    sramSize  = 0;
    std::memset(sramData, 0, sizeof(sramData));

    // Use the original data pointer for SRAM header check to avoid offset confusion
    if (size >= 0x1C0) {
        const u8 b0 = data[0x1B0];
        const u8 b1 = data[0x1B1];
        const u8 ty = data[0x1B2];
        if (b0 == 0x52 && b1 == 0x41 && (ty & 0xA0) == 0xA0) {
            hasSRAM = true;
            sramStart = (static_cast<u32>(data[0x1B4]) << 24)
                      | (static_cast<u32>(data[0x1B5]) << 16)
                      | (static_cast<u32>(data[0x1B6]) <<  8)
                      |  static_cast<u32>(data[0x1B7]);
            sramEnd   = (static_cast<u32>(data[0x1B8]) << 24)
                      | (static_cast<u32>(data[0x1B9]) << 16)
                      | (static_cast<u32>(data[0x1BA]) <<  8)
                      |  static_cast<u32>(data[0x1BB]);
            u32 declared = (sramEnd >= sramStart) ? (sramEnd - sramStart + 1) : 0;
            sramSize = (declared < 0x2000) ? 0x2000 : declared;
            if (sramSize > GEN_SRAM_MAX) sramSize = GEN_SRAM_MAX;
        }
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// Input
// ─────────────────────────────────────────────────────────────────────────────
void GenBus::pressButton(u32 pad, GenBtn btn, bool pressed) {
    if (pad >= GEN_PAD_COUNT) return;
    if (pressed) padState[pad] |=  (1u << static_cast<u32>(btn));
    else         padState[pad] &= ~(1u << static_cast<u32>(btn));
}

// Returns the 7-bit value the pad drives onto the data port.
// Bits are active-LOW (0 = button pressed).
// Bit 6 reflects the current TH line state.
//
// TH=1 (high):  bits 5–0 = C B Right Left Down Up
// TH=0 (low):   bits 5–0 = Start A 0 0 Down Up
//               bits 3–2 are always 0 in TH=0 phase
u8 GenBus::_readPad(u32 pad) {
    const u32 s  = padState[pad];
    const u8  th = padTH[pad];
    u8 v = 0x7F;  // all bits high (unpressed)

    if (th) {
        // TH=1 phase
        if (s & (1u << GEN_BTN_UP))    v &= ~0x01u;
        if (s & (1u << GEN_BTN_DOWN))  v &= ~0x02u;
        if (s & (1u << GEN_BTN_LEFT))  v &= ~0x04u;
        if (s & (1u << GEN_BTN_RIGHT)) v &= ~0x08u;
        if (s & (1u << GEN_BTN_B))     v &= ~0x10u;
        if (s & (1u << GEN_BTN_C))     v &= ~0x20u;
        v |= 0x40u;  // TH=1 reflected in bit 6
    } else {
        // TH=0 phase
        if (s & (1u << GEN_BTN_UP))    v &= ~0x01u;
        if (s & (1u << GEN_BTN_DOWN))  v &= ~0x02u;
        v &= ~0x0Cu;  // bits 3–2 always 0 in TH=0
        if (s & (1u << GEN_BTN_A))     v &= ~0x10u;
        if (s & (1u << GEN_BTN_START)) v &= ~0x20u;
        v &= ~0x40u;  // TH=0 reflected in bit 6
    }
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// I/O register reads  (0xA10000–0xA1001F, odd byte offsets)
// ─────────────────────────────────────────────────────────────────────────────
u8 GenBus::_ioRead8(u32 addr) {
    switch (addr & 0x1Fu) {
        case 0x01: return 0xA0u;        // version: overseas, NTSC, no TMSS
        case 0x03: return _readPad(0);
        case 0x05: return _readPad(1);
        case 0x07: return 0xFFu;        // expansion port (not connected)
        case 0x09: return padCtrl[0];
        case 0x0B: return padCtrl[1];
        default:   return 0xFFu;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// I/O register writes
// ─────────────────────────────────────────────────────────────────────────────
void GenBus::_ioWrite8(u32 addr, u8 val) {
    switch (addr & 0x1Fu) {
        case 0x09:
            padCtrl[0] = val;
            break;
        case 0x0B:
            padCtrl[1] = val;
            break;
        // Data port 1: writing with TH configured as output latches TH state.
        case 0x02:
            if (padCtrl[0] & 0x40u) padTH[0] = (val >> 6) & 1u;
            break;
        case 0x04:
            if (padCtrl[1] & 0x40u) padTH[1] = (val >> 6) & 1u;
            break;
        default:
            break;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Z80 sub-bus port access
// Called by GenZ80 for its address range 0x2000–0x7FFF.
// ─────────────────────────────────────────────────────────────────────────────
u8 GenBus::readZ80Port(u16 addr) {
    // YM2612: 0x4000–0x4003
    if (addr >= 0x4000u && addr <= 0x4003u)
        return apu ? apu->readYM() : 0xFFu;
    // VDP: 0x7F00–0x7FFF (mirrored)
    if (addr >= 0x7F00u)
        return vdp ? vdp->read8(addr & 0x1Fu) : 0xFFu;
    return 0xFFu;
}

void GenBus::writeZ80Port(u16 addr, u8 val) {
    // YM2612: 0x4000–0x4003
    if (addr >= 0x4000u && addr <= 0x4003u) {
        if (apu) {
            const u32 bank = (addr >> 1) & 1u;
            if (addr & 1u) apu->writeYM(bank, apu->lastYMReg, val);
            else           apu->lastYMReg = val;
        }
        return;
    }
    // Z80→68K bank register: 0x6000–0x60FF — 9-bit serial shift register
    if (addr >= 0x6000u && addr < 0x6100u) {
        z80Bank = ((z80Bank >> 1) | ((val & 1u) << 8u)) & 0x1FFu;
        return;
    }
    // PSG: any odd byte in 0x7F00–0x7FFF
    if (addr >= 0x7F00u) {
        if (addr & 1u) { if (apu) apu->writePSG(val); }
        else if (vdp)  vdp->write8(addr & 0x1Fu, val);
        return;
    }
    // VDP: 0x7E00–0x7EFF
    if (addr >= 0x7E00u) {
        if (vdp) vdp->write8(addr & 0x1Fu, val);
        return;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// read8 — byte read, full address decode
// ─────────────────────────────────────────────────────────────────────────────
u8 GenBus::read8(u32 addr) {
    const u32 a = addr & 0xFFFFFFu;

    // ROM  0x000000–0x3FFFFF
    if (a < 0x400000u) {
        return (a < static_cast<u32>(rom.size())) ? rom[a] : 0xFFu;
    }

    // SRAM
    if (hasSRAM && a >= sramStart && a <= sramEnd)
        return sramData[(a - sramStart) & (sramSize - 1u)];

    // Z80 address space  0xA00000–0xA0FFFF
    if (a >= 0xA00000u && a < 0xA10000u)
        return z80Ram[a & 0x1FFFu];

    // I/O  0xA10000–0xA1001F
    if (a >= 0xA10000u && a < 0xA10020u)
        return _ioRead8(a);

    // Z80 BUSREQ  0xA11100–0xA11101
    if (a == 0xA11100u || a == 0xA11101u)
        return 0x00u;  // bus always immediately granted

    // Z80 RESET  0xA11200–0xA11201
    if (a == 0xA11200u || a == 0xA11201u)
        return z80Reset ? 0x00u : 0xFFu;

    // VDP  0xC00000–0xC0001F
    if (a >= 0xC00000u && a < 0xC00020u)
        return vdp ? vdp->read8(a & 0x1Fu) : 0xFFu;

    // WRAM  0xE00000–0xFFFFFF (64 KB, mirrored)
    if (a >= 0xE00000u)
        return wram[a & 0xFFFFu];

    return 0xFFu;  // open bus
}

// ─────────────────────────────────────────────────────────────────────────────
// read16 — word read, hot paths inlined
// ─────────────────────────────────────────────────────────────────────────────
u16 GenBus::read16(u32 addr) {
    const u32 a = addr & 0xFFFFFFu;

    // ROM fast path
    if (a < 0x400000u) {
        if (a + 1u < static_cast<u32>(rom.size()))
            return (static_cast<u16>(rom[a]) << 8) | rom[a + 1u];
        return 0xFFFFu;
    }

    // WRAM fast path
    if (a >= 0xE00000u) {
        const u32 wa = a & 0xFFFFu;
        return (static_cast<u16>(wram[wa]) << 8) | wram[wa + 1u];
    }

    // VDP
    if (a >= 0xC00000u && a < 0xC00020u)
        return vdp ? vdp->read16(a & 0x1Fu) : 0xFFFFu;

    // Fall back to two byte reads for everything else
    return (static_cast<u16>(read8(addr)) << 8) | read8(addr + 1u);
}

// ─────────────────────────────────────────────────────────────────────────────
// read32
// ─────────────────────────────────────────────────────────────────────────────
u32 GenBus::read32(u32 addr) {
    return (static_cast<u32>(read16(addr)) << 16) | read16(addr + 2u);
}

// ─────────────────────────────────────────────────────────────────────────────
// write8
// YM2612 and PSG intercepts live here (replaces the JS monkey-patch).
// ─────────────────────────────────────────────────────────────────────────────
void GenBus::write8(u32 addr, u8 val) {
    const u32 a = addr & 0xFFFFFFu;

    // YM2612  0xA04000–0xA04003
    if (a >= 0xA04000u && a <= 0xA04003u) {
        if (apu) {
            const u32 bank = (a >> 1) & 1u;
            if (a & 1u) apu->writeYM(bank, apu->lastYMReg, val);
            else        apu->lastYMReg = val;
        }
        return;
    }

    // PSG  0xC00011 / 0xC00013 / 0xC00015 / 0xC00017 (odd bytes in VDP area)
    if (a == 0xC00011u || a == 0xC00013u || a == 0xC00015u || a == 0xC00017u) {
        if (apu) apu->writePSG(val);
        return;
    }

    // SRAM
    if (hasSRAM && a >= sramStart && a <= sramEnd) {
        sramData[(a - sramStart) & (sramSize - 1u)] = val;
        sramDirty = true;
        return;
    }

    // Z80 area  0xA00000–0xA0FFFF
    if (a >= 0xA00000u && a < 0xA10000u) {
        z80Ram[a & 0x1FFFu] = val;
        return;
    }

    // I/O  0xA10000–0xA1001F
    if (a >= 0xA10000u && a < 0xA10020u) {
        _ioWrite8(a, val);
        return;
    }

    // Z80 BUSREQ  0xA11100–0xA11101
    if (a == 0xA11100u || a == 0xA11101u) {
        z80BusReq = (val & 1u) != 0u;
        return;
    }

    // Z80 RESET  0xA11200–0xA11201
    if (a == 0xA11200u || a == 0xA11201u) {
        z80Reset = (val & 1u) == 0u;
        return;
    }

    // TMSS  0xA14000–0xA14003 (accept any write, no effect)
    if (a >= 0xA14000u && a < 0xA14004u)
        return;

    // VDP  0xC00000–0xC0001F
    if (a >= 0xC00000u && a < 0xC00020u) {
        if (vdp) vdp->write8(a & 0x1Fu, val);
        return;
    }

    // WRAM  0xE00000–0xFFFFFF
    if (a >= 0xE00000u) {
        wram[a & 0xFFFFu] = val;
        return;
    }
    // All other addresses: open bus, ignore write
}

// ─────────────────────────────────────────────────────────────────────────────
// write16
// ─────────────────────────────────────────────────────────────────────────────
void GenBus::write16(u32 addr, u16 val) {
    const u32 a = addr & 0xFFFFFFu;

    // WRAM fast path
    if (a >= 0xE00000u) {
        const u32 wa = a & 0xFFFFu;
        wram[wa]      = static_cast<u8>(val >> 8);
        wram[wa + 1u] = static_cast<u8>(val);
        return;
    }

    // VDP
    if (a >= 0xC00000u && a < 0xC00020u) {
        if (vdp) vdp->write16(a & 0x1Fu, val);
        return;
    }

    // Z80 control / BUSREQ area — fall through to write8 × 2
    if (a >= 0xA11100u && a < 0xA11300u) {
        write8(addr,      static_cast<u8>(val >> 8));
        write8(addr + 1u, static_cast<u8>(val));
        return;
    }

    // General fallback
    write8(addr,      static_cast<u8>(val >> 8));
    write8(addr + 1u, static_cast<u8>(val));
}

// ─────────────────────────────────────────────────────────────────────────────
// write32
// ─────────────────────────────────────────────────────────────────────────────
void GenBus::write32(u32 addr, u32 val) {
    write16(addr,      static_cast<u16>(val >> 16));
    write16(addr + 2u, static_cast<u16>(val));
}

// ─────────────────────────────────────────────────────────────────────────────
// Size-dispatched helpers (used by M68K EA engine)
// sz: 0 = byte, 1 = word, 2 = long
// ─────────────────────────────────────────────────────────────────────────────
u32 GenBus::readSize(u32 addr, u32 sz) {
    switch (sz) {
        case 0:  return read8 (addr);
        case 1:  return read16(addr);
        default: return read32(addr);
    }
}

void GenBus::writeSize(u32 addr, u32 val, u32 sz) {
    switch (sz) {
        case 0:  write8 (addr, static_cast<u8> (val)); break;
        case 1:  write16(addr, static_cast<u16>(val)); break;
        default: write32(addr, val);                   break;
    }
}
