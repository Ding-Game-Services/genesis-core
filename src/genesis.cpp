#include "genesis.h"
#include "ding_savestate.h" 
#include "ding_md5.h"
#include <cstring>
#include <cstdio>

// ─────────────────────────────────────────────────────────────────────────────
// Constructor
//
// Initialization order MUST match declaration order in genesis.h:
//   bus → vdp → z80 → apu → cpu
// vdp, z80, and cpu all take a GenBus* in their constructors.
// bus must be fully constructed before any of them are initialized.
// ─────────────────────────────────────────────────────────────────────────────
Genesis::Genesis()
    : bus()
    , vdp(&bus)
    , z80(&bus)
    , apu()
    , cpu(&bus)
    , isPAL(false)
    , linesFrame (NTSC_LINES)
    , activeLines(NTSC_ACTIVE)
    , cpl        (NTSC_CPL)
    , z80cpl     (NTSC_Z80_CPL)
    , overshoot  (0)
    , errorFlag  (false)
{
    // Wire subsystem cross-references into the bus.
    // Can't happen in the bus constructor because the other
    // subsystems don't exist yet at that point.
    bus.vdp  = &vdp;
    bus.z80  = &z80;
    bus.apu  = &apu;
    bus.m68k = &cpu;
    errorMsg[0] = '\0';
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────
void Genesis::reset() {
    bus.reset();
    vdp.reset();
    z80.reset();
    apu.reset();
    cpu.reset();
    overshoot = 0;
    errorFlag = false;
    errorMsg[0] = '\0';
}

bool Genesis::loadROM(const u8* data, u32 size) {
    if (!data || size < 0x200u) {
        _setError("ROM too small (minimum 512 bytes)");
        return false;
    }
    bus.loadROM(data, size);
    // Reset subsystems in the correct order
    vdp.reset();
    z80.reset();
    apu.reset();
    cpu.reset();   // reads reset vectors from bus, so bus.loadROM must come first
    vdp.frame = 0;
    overshoot = 0;
    errorFlag = false;
    errorMsg[0] = '\0';
    return true;
}

void Genesis::setRegion(bool pal) {
    isPAL        = pal;
    vdp.isPAL    = pal;
    if (pal) {
        linesFrame  = PAL_LINES;
        activeLines = PAL_ACTIVE;
        cpl         = PAL_CPL;
        z80cpl      = PAL_Z80_CPL;
    } else {
        linesFrame  = NTSC_LINES;
        activeLines = NTSC_ACTIVE;
        cpl         = NTSC_CPL;
        z80cpl      = NTSC_Z80_CPL;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Input
// ─────────────────────────────────────────────────────────────────────────────
void Genesis::pressButton(u32 pad, u32 btn, bool pressed) {
    if (pad < GEN_PAD_COUNT)
        bus.pressButton(pad, static_cast<GenBtn>(btn), pressed);
}

// ─────────────────────────────────────────────────────────────────────────────
// Run one video frame
//
// Execution model (per scanline):
//   1. Run M68K for ~cpl cycles (minus last-line overshoot)
//   2. Run Z80 for ~z80cpl cycles (only when 68K is not holding the bus)
//   3. Tick VDP — fires VBlank interrupt at line == activeLines
//   4. Fire HBlank interrupt if enabled and counter matches
//
// After all scanlines: push audio samples into the ring buffer.
// ─────────────────────────────────────────────────────────────────────────────
void Genesis::runFrame() {
for (u32 line = 0; line < linesFrame; line++) {

    static int dbgCount = 0;

    if (!bus.z80Reset) {
        z80.run(z80cpl);
    }

    const bool vblankStart = vdp.tickLine(line, isPAL);


    const u32 lineStartCycles = cpu.cycles;

    while (cpu.cycles < lineStartCycles + 488) {
        cpu.step();
    }
    // ------------------


    if (vblankStart) {
        printf(
            "VBLANK HIT line=%u REG1=%02X\n",
            line,
            vdp.regs[1]
        );
    }

if (vblankStart) {

    static int irqdbg = 0;

    if (irqdbg < 5) {
        printf(
            "VBLANK reached REG1=%02X\n",
            vdp.regs[1]
        );
        irqdbg++;
    }

    if (vdp.regs[1] & 0x20u) {

        printf(
            "VBLANK IRQ6 frame=%u line=%u\n",
            frame,
            line
        );

        bool accepted = cpu.interrupt(6);

printf(
    "IRQ6 accepted=%d\n",
    accepted
);
    }
}
        if (vdp.checkHInt(line, isPAL)) {
            // H-INT enabled? (reg 0 bit 4)
            if (vdp.regs[0] & 0x10u) {
                cpu.interrupt(4);
            }
        }
    }   // end per-scanline loop

    const u32 samplesPerFrame =
        GEN_AUDIO_RATE / (isPAL ? 50u : 60u);

    apu.generateFrame(samplesPerFrame);

    vdp.frame++;
    frame++;

static int dbg = 0;
if (++dbg == 60) {
    char buf[2048];
    diagVideo(buf, sizeof(buf));
    printf("%s\n", buf);
}
}   // end runFrame()


// ─────────────────────────────────────────────────────────────────────────────
// Save state
//
// Serializes all mutable emulation state into a caller-owned buffer using
// the SDK's .ding block format. The ROM itself is not saved — the loader
// validates the save state against the ROM's MD5 on restore.
// ─────────────────────────────────────────────────────────────────────────────
bool Genesis::saveState(u8* buf, u32 bufSize, u32* outSize) {
    if (!buf || bufSize < 4096u) {
        _setError("Save state buffer too small");
        return false;
    }

    // Compute ROM MD5 for header binding
    u8 romHash[16] = {};
    if (!bus.rom.empty())
        ding_md5(bus.rom.data(), bus.rom.size(), romHash);

    DingSaveWriter w;
    ding_save_writer_init(&w, buf, bufSize, "Sega Genesis");

    // ── M68K CPU state ────────────────────────────────────────────────────────
    struct M68KBlock {
        u32 d[8], a[8], pc, usp;
        u16 sr;
        u8  stopped, pad[1];
    } m68k;
    for (u32 i=0;i<8;i++){m68k.d[i]=cpu.d[i]; m68k.a[i]=cpu.a[i];}
    m68k.pc=cpu.pc; m68k.usp=cpu.usp; m68k.sr=cpu.sr;
    m68k.stopped=cpu.stopped?1u:0u; m68k.pad[0]=0;
    ding_save_write_block(&w, "M68K", &m68k, sizeof(m68k));

    // ── Z80 state (regs + RAM) ────────────────────────────────────────────────
    z80.saveState(&w);

    // ── VDP registers and control state ──────────────────────────────────────
    struct VDPBlock {
        u8  regs[GEN_VDP_REG_COUNT];
        u16 addrReg, ctrlFirst;
        u8  addrInc, cdReg;
        u8  ctrlPendWord, ctrlPendByte;
        u8  isPAL, vintPending, dmaFillPending;
        u16 dmaFillData;
        u32 frame;
    } vb;
    std::memcpy(vb.regs, vdp.regs, sizeof(vb.regs));
    vb.addrReg=vdp.addrReg; vb.ctrlFirst=vdp.ctrlFirst;
    vb.addrInc=vdp.addrInc; vb.cdReg=vdp.cdReg;
    vb.ctrlPendWord=vdp.ctrlPendWord?1u:0u; vb.ctrlPendByte=vdp.ctrlPendByte?1u:0u;
    vb.isPAL=vdp.isPAL?1u:0u; vb.vintPending=vdp.vintPending?1u:0u;
    vb.dmaFillPending=vdp.dmaFillPending?1u:0u;
    vb.dmaFillData=vdp.dmaFillData; vb.frame=vdp.frame;
    ding_save_write_block(&w, "VDP_STATE", &vb, sizeof(vb));
    ding_save_write_block(&w, "VRAM",  vdp.vram,  GEN_VRAM_SIZE);
    ding_save_write_block(&w, "CRAM",  vdp.cram,  GEN_CRAM_WORDS  * sizeof(u16));
    ding_save_write_block(&w, "VSRAM", vdp.vsram, GEN_VSRAM_WORDS * sizeof(u16));

    // ── Main RAM ──────────────────────────────────────────────────────────────
    ding_save_write_block(&w, "WRAM", bus.wram, GEN_WRAM_SIZE);

    // ── Battery SRAM (optional) ───────────────────────────────────────────────
    if (bus.hasSRAM && bus.sramSize > 0)
        ding_save_write_block(&w, "SRAM", bus.sramData, bus.sramSize);

    // ── Bus signals and pad state ─────────────────────────────────────────────
    struct BusBlock {
        u32 padState[GEN_PAD_COUNT];
        u8  padCtrl[GEN_PAD_COUNT], padTH[GEN_PAD_COUNT];
        u8  z80BusReq, z80Reset;
        u32 z80Bank;
    } bb;
    bb.padState[0]=bus.padState[0]; bb.padState[1]=bus.padState[1];
    bb.padCtrl[0]=bus.padCtrl[0];   bb.padCtrl[1]=bus.padCtrl[1];
    bb.padTH[0]=bus.padTH[0];       bb.padTH[1]=bus.padTH[1];
    bb.z80BusReq=bus.z80BusReq?1u:0u; bb.z80Reset=bus.z80Reset?1u:0u;
    bb.z80Bank=bus.z80Bank;
    ding_save_write_block(&w, "BUS", &bb, sizeof(bb));

    // ── Region and timing ─────────────────────────────────────────────────────
    struct RegionBlock { u8 isPAL; s32 overshoot; } rb;
    rb.isPAL=isPAL?1u:0u; rb.overshoot=overshoot;
    ding_save_write_block(&w, "REGION", &rb, sizeof(rb));

    size_t finishSize = 0;
ding_save_writer_finish(&w, &finishSize);
if (outSize) *outSize = static_cast<u32>(finishSize);
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Load state
// ─────────────────────────────────────────────────────────────────────────────
bool Genesis::loadState(const u8* buf, u32 size) {
    if (!buf || size < 64u) {
        _setError("Save state buffer invalid");
        return false;
    }

    DingSaveReader r;
    if (ding_save_reader_init(&r, buf, size) != DING_SS_OK) {
        _setError("Save state corrupted or wrong format");
        return false;
    }

    // ── M68K ─────────────────────────────────────────────────────────────────
    struct M68KBlock {
        u32 d[8], a[8], pc, usp;
        u16 sr;
        u8  stopped, pad[1];
    } m68k = {};
    ding_save_read_block(&r, "M68K", &m68k, sizeof(m68k), nullptr);
    for (u32 i=0;i<8;i++){cpu.d[i]=m68k.d[i]; cpu.a[i]=m68k.a[i];}
    cpu.pc=m68k.pc; cpu.usp=m68k.usp; cpu.sr=m68k.sr;
    cpu.stopped=m68k.stopped!=0u;

    // ── Z80 ──────────────────────────────────────────────────────────────────
    z80.loadState(&r);

    // ── VDP state ─────────────────────────────────────────────────────────────
    struct VDPBlock {
        u8  regs[GEN_VDP_REG_COUNT];
        u16 addrReg, ctrlFirst;
        u8  addrInc, cdReg;
        u8  ctrlPendWord, ctrlPendByte;
        u8  isPAL, vintPending, dmaFillPending;
        u16 dmaFillData;
        u32 frame;
    } vb = {};
    ding_save_read_block(&r, "VDP_STATE", &vb, sizeof(vb), nullptr);
    std::memcpy(vdp.regs, vb.regs, sizeof(vb.regs));
    vdp.addrReg=vb.addrReg; vdp.ctrlFirst=vb.ctrlFirst;
    vdp.addrInc=vb.addrInc; vdp.cdReg=vb.cdReg;
    vdp.ctrlPendWord=vb.ctrlPendWord!=0u; vdp.ctrlPendByte=vb.ctrlPendByte!=0u;
    vdp.isPAL=vb.isPAL!=0u; vdp.vintPending=vb.vintPending!=0u;
    vdp.dmaFillPending=vb.dmaFillPending!=0u;
    vdp.dmaFillData=vb.dmaFillData; vdp.frame=vb.frame;
    ding_save_read_block(&r, "VRAM",  vdp.vram,  GEN_VRAM_SIZE,                   nullptr);
    ding_save_read_block(&r, "CRAM",  vdp.cram,  GEN_CRAM_WORDS  * sizeof(u16),   nullptr);
    ding_save_read_block(&r, "VSRAM", vdp.vsram, GEN_VSRAM_WORDS * sizeof(u16),   nullptr);

    // ── RAM ───────────────────────────────────────────────────────────────────
    ding_save_read_block(&r, "WRAM", bus.wram, GEN_WRAM_SIZE, nullptr);
    if (bus.hasSRAM && bus.sramSize > 0)
        ding_save_read_block(&r, "SRAM", bus.sramData, bus.sramSize, nullptr);

    // ── Bus ───────────────────────────────────────────────────────────────────
    struct BusBlock {
        u32 padState[GEN_PAD_COUNT];
        u8  padCtrl[GEN_PAD_COUNT], padTH[GEN_PAD_COUNT];
        u8  z80BusReq, z80Reset;
        u32 z80Bank;
    } bb = {};
    ding_save_read_block(&r, "BUS", &bb, sizeof(bb), nullptr);
    bus.padState[0]=bb.padState[0]; bus.padState[1]=bb.padState[1];
    bus.padCtrl[0]=bb.padCtrl[0];   bus.padCtrl[1]=bb.padCtrl[1];
    bus.padTH[0]=bb.padTH[0];       bus.padTH[1]=bb.padTH[1];
    bus.z80BusReq=bb.z80BusReq!=0u; bus.z80Reset=bb.z80Reset!=0u;
    bus.z80Bank=bb.z80Bank;

    // ── Region ────────────────────────────────────────────────────────────────
    struct RegionBlock { u8 isPAL; s32 overshoot; } rb = {};
    ding_save_read_block(&r, "REGION", &rb, sizeof(rb), nullptr);
    setRegion(rb.isPAL != 0u);
    overshoot = rb.overshoot;

    // Force a full framebuf redraw from restored VRAM state
    vdp.vramDirty = true;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostics
// ─────────────────────────────────────────────────────────────────────────────
void Genesis::diagCPU(char* out, u32 outSize) {
    if (!out || outSize == 0) return;
    const u32 ipl = (cpu.sr >> 8) & 7u;
    std::snprintf(out, outSize,
        "M68K  PC=%08X  SR=%04X (S=%u T=%u IPL=%u X=%u N=%u Z=%u V=%u C=%u)%s\n"
        "  D0=%08X D1=%08X D2=%08X D3=%08X\n"
        "  D4=%08X D5=%08X D6=%08X D7=%08X\n"
        "  A0=%08X A1=%08X A2=%08X A3=%08X\n"
        "  A4=%08X A5=%08X A6=%08X A7=%08X\n"
        "Z80   PC=%04X  SP=%04X  IFF=%u/%u  IM=%u%s\n"
        "  AF=%02X%02X  BC=%02X%02X  DE=%02X%02X  HL=%02X%02X\n"
        "  IX=%04X  IY=%04X\n",
        cpu.pc, cpu.sr,
        (cpu.sr >> 13) & 1, (cpu.sr >> 15) & 1, ipl,
        (cpu.sr >> 4) & 1, (cpu.sr >> 3) & 1, (cpu.sr >> 2) & 1,
        (cpu.sr >> 1) & 1,  cpu.sr & 1,
        cpu.stopped ? " [STOPPED]" : "",
        cpu.d[0],cpu.d[1],cpu.d[2],cpu.d[3],
        cpu.d[4],cpu.d[5],cpu.d[6],cpu.d[7],
        cpu.a[0],cpu.a[1],cpu.a[2],cpu.a[3],
        cpu.a[4],cpu.a[5],cpu.a[6],cpu.a[7],
        z80.PC, z80.SP, z80.IFF1, z80.IFF2, z80.IM,
        z80.halted ? " [HALTED]" : "",
        z80.A, z80.F, z80.B, z80.C, z80.D, z80.E, z80.H, z80.L,
        z80.IX, z80.IY);
}

void Genesis::diagVideo(char* out, u32 outSize) {
    if (!out || outSize == 0) return;
    // Print VDP registers in groups of 8
    int used = std::snprintf(out, outSize,
        "VDP  frame=%u  vctr=%u  vblank=%u  hint=%u  dma=%u\n"
        "     display=%u  vint_en=%u  hint_en=%u\n"
        "Regs:\n",
        vdp.frame, vdp.vcounter,
        vdp.vblank ? 1u : 0u,
        (vdp.regs[0] & 0x10u) ? 1u : 0u,
        vdp.dmaActive ? 1u : 0u,
        (vdp.regs[1] & 0x40u) ? 1u : 0u,
        (vdp.regs[1] & 0x20u) ? 1u : 0u,
        (vdp.regs[0] & 0x10u) ? 1u : 0u);
    for (u32 i = 0; i < GEN_VDP_REG_COUNT && used < static_cast<int>(outSize)-8; i++) {
        used += std::snprintf(out + used, outSize - static_cast<u32>(used),
            "  R%02u=%02X%s", i, vdp.regs[i], ((i & 7u)==7u||i==GEN_VDP_REG_COUNT-1)?"\n":"");
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Internal helpers
// ─────────────────────────────────────────────────────────────────────────────
void Genesis::_setError(const char* msg) {
    errorFlag = true;
    std::snprintf(errorMsg, sizeof(errorMsg), "%s", msg);
}
