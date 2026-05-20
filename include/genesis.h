#pragma once

// genesis.h — internal type definitions for genesis-core
// All subsystem classes are declared here; implementations live in their
// respective .cpp files. This header is included by every source file in
// the core. It is NOT the public API — that is sdk/include/ding_core.h.

#include <cstddef>
#include <cstring>
#include <cstdio>
#include <vector>

#include "ding_types.h"   // u8/u16/u32/u64/s8/s16/s32/s64, bit macros
#include "ding_audio.h"   // DingAudioBuffer

// ─────────────────────────────────────────────────────────────────────────────
// Display constants
// ─────────────────────────────────────────────────────────────────────────────
static constexpr u32 GEN_W       = 320;
static constexpr u32 GEN_H_NTSC  = 224;   // NTSC active lines
static constexpr u32 GEN_H_PAL   = 240;   // PAL active lines
static constexpr u32 GEN_H_MAX   = 240;   // framebuffer always sized for PAL

// ─────────────────────────────────────────────────────────────────────────────
// Timing (NTSC / PAL)
// Master clock: NTSC 53,693,175 Hz  PAL 53,203,424 Hz
// M68K = master / 7   NTSC ≈ 7,670,454 Hz   PAL ≈ 7,600,489 Hz
// Z80  = master / 15  NTSC ≈ 3,579,545 Hz   PAL ≈ 3,546,893 Hz
// ─────────────────────────────────────────────────────────────────────────────
static constexpr u32 NTSC_LINES   = 262;
static constexpr u32 NTSC_ACTIVE  = 224;
static constexpr u32 NTSC_CPL     = 488;   // M68K cycles per scanline
static constexpr u32 NTSC_Z80_CPL = 228;   // Z80 cycles per scanline

static constexpr u32 PAL_LINES    = 313;
static constexpr u32 PAL_ACTIVE   = 240;
static constexpr u32 PAL_CPL      = 487;
static constexpr u32 PAL_Z80_CPL  = 228;

// ─────────────────────────────────────────────────────────────────────────────
// Memory sizes
// ─────────────────────────────────────────────────────────────────────────────
static constexpr u32 GEN_WRAM_SIZE   = 0x10000;  // 64 KB main RAM
static constexpr u32 GEN_Z80RAM_SIZE = 0x2000;   // 8 KB Z80 internal RAM
static constexpr u32 GEN_SRAM_MAX    = 0x8000;   // 32 KB battery-backed SRAM (max)
static constexpr u32 GEN_VRAM_SIZE   = 0x10000;  // 64 KB video RAM
static constexpr u32 GEN_CRAM_WORDS  = 64;       // 64 palette entries (u16 each)
static constexpr u32 GEN_VSRAM_WORDS = 40;       // 40 vertical scroll entries (u16)
static constexpr u32 GEN_VDP_REG_COUNT = 24;
static constexpr u32 GEN_PAD_COUNT  = 2;

// ─────────────────────────────────────────────────────────────────────────────
// Audio
// ─────────────────────────────────────────────────────────────────────────────
static constexpr u32 GEN_AUDIO_RATE     = 44100;
static constexpr u32 GEN_AUDIO_CHANNELS = 2;
// Ring buffer: ~93 ms at 44100 Hz — enough to absorb one slow frame
static constexpr u32 GEN_AUDIO_CAPACITY = 4096;
static constexpr u32 GEN_AUDIO_STORAGE  = GEN_AUDIO_CAPACITY * GEN_AUDIO_CHANNELS;

// ─────────────────────────────────────────────────────────────────────────────
// Controller button indices
// Matches the frontend's GEN_BTN mapping.
// ─────────────────────────────────────────────────────────────────────────────
enum GenBtn : u32 {
    GEN_BTN_UP    = 0,
    GEN_BTN_DOWN  = 1,
    GEN_BTN_LEFT  = 2,
    GEN_BTN_RIGHT = 3,
    GEN_BTN_A     = 4,
    GEN_BTN_B     = 5,
    GEN_BTN_C     = 6,
    GEN_BTN_START = 7,
    GEN_BTN_X     = 8,
    GEN_BTN_Y     = 9,
    GEN_BTN_Z     = 10,
    GEN_BTN_MODE  = 11,
    GEN_BTN_COUNT = 12,
};

// ─────────────────────────────────────────────────────────────────────────────
// Forward declarations
// ─────────────────────────────────────────────────────────────────────────────
class GenBus;
class M68K;
class GenVDP;
class GenZ80;
class GenAPU;

// ─────────────────────────────────────────────────────────────────────────────
// YM2612 — FM sound chip
// Two register banks × 256 registers = 512 bytes total.
// ─────────────────────────────────────────────────────────────────────────────
class YM2612 {
public:
    u8 regs[0x200];   // bank 0: [0x000–0x0FF]  bank 1: [0x100–0x1FF]
    u8 status;        // status byte returned on read (timer A/B flags)

    YM2612();
    void reset();

    // reg write: bank selects register bank (0 or 1)
    void write(u32 bank, u8 reg, u8 val);
    u8   read() const;

    // Generate interleaved stereo PCM into buf (count = sample frame count)
    // v0.1 stub: outputs silence. Full FM synthesis in Step 7.
    void clock(float* buf, u32 count);
};

// ─────────────────────────────────────────────────────────────────────────────
// SN76489 — Programmable Sound Generator (PSG)
// 3 tone channels + 1 noise channel, each with independent volume.
// ─────────────────────────────────────────────────────────────────────────────
class SN76489 {
public:
    u8 regs[8];    // tone/noise regs (even) + volume regs (odd), 4 channels
    u8 latch;      // last register-select byte written

    SN76489();
    void reset();
    void write(u8 val);

    // Generate interleaved stereo PCM into buf.
    // v0.1 stub: outputs silence. Full synthesis in Step 7.
    void clock(float* buf, u32 count);
};

// ─────────────────────────────────────────────────────────────────────────────
// GenAPU — Audio subsystem
// Owns the YM2612, SN76489, and the DingAudioBuffer ring buffer.
// ─────────────────────────────────────────────────────────────────────────────
class GenAPU {
public:
    YM2612 ym2612;
    SN76489 psg;

    // Held between a YM2612 address-port write and the subsequent data-port write.
    // Shared across bus and Z80 port write paths — must be public.
    u8 lastYMReg;

    DingAudioBuffer audioBuf;
    float           audioStorage[GEN_AUDIO_STORAGE]; // backing store for ring buffer

    GenAPU();
    void reset();

    // Called by GenBus write path
    void writeYM (u32 bank, u8 reg, u8 val);
    u8   readYM  () const;
    void writePSG(u8 val);

    // Called at end of each frame by Genesis::runFrame
    // Generates samples and pushes them into audioBuf.
    void generateFrame(u32 samplesNeeded);
};

// ─────────────────────────────────────────────────────────────────────────────
// GenBus — Genesis memory map dispatcher
//
// Address map:
//   0x000000–0x3FFFFF  ROM (up to 4 MB)
//   0xA00000–0xA0FFFF  Z80 address space (via 68K)
//   0xA10000–0xA1001F  I/O (controllers, version register)
//   0xA11100           Z80 BUSREQ
//   0xA11200           Z80 RESET
//   0xA14000           TMSS register
//   0xC00000–0xC0001F  VDP ports
//   0xE00000–0xFFFFFF  WRAM (64 KB, mirrored)
// ─────────────────────────────────────────────────────────────────────────────
class GenBus {
public:
    // ROM — owned copy loaded at startup
    std::vector<u8> rom;

    // Working RAM
    u8 wram   [GEN_WRAM_SIZE];     // 64 KB main RAM
    u8 z80Ram [GEN_Z80RAM_SIZE];   // 8 KB Z80 internal RAM

    // Battery-backed SRAM
    u8   sramData[GEN_SRAM_MAX];
    bool hasSRAM;
    bool sramDirty;
    u32  sramStart;
    u32  sramEnd;
    u32  sramSize;

    // Controller state (two 3/6-button pads)
    u32 padState[GEN_PAD_COUNT];   // bitmask of pressed buttons (GenBtn indices)
    u8  padCtrl [GEN_PAD_COUNT];   // data-direction register (bit 6 = TH direction)
    u8  padTH   [GEN_PAD_COUNT];   // current TH line state

    // Z80 bus control signals
    bool z80BusReq;   // true = 68K holds Z80 bus
    bool z80Reset;    // true = Z80 held in reset
    u32  z80Bank;     // 9-bit bank register for Z80→68K window (0x8000–0xFFFF)

    // Subsystem back-pointers (wired by Genesis constructor after all members init)
    GenVDP* vdp;
    GenZ80* z80;
    GenAPU* apu;
    M68K*   m68k;

    GenBus();
    void reset();
    void loadROM(const u8* data, u32 size);

    // Input
    void pressButton(u32 pad, GenBtn btn, bool pressed);

    // Main read/write interface
    u8   read8 (u32 addr);
    u16  read16(u32 addr);
    u32  read32(u32 addr);
    void write8 (u32 addr, u8  val);
    void write16(u32 addr, u16 val);
    void write32(u32 addr, u32 val);

    // Size-dispatched helpers used by the M68K EA engine
    // sz: 0 = byte, 1 = word, 2 = long
    u32  readSize (u32 addr, u32 sz);
    void writeSize(u32 addr, u32 val, u32 sz);

    // Z80 sub-bus port access (called by GenZ80 for addresses 0x2000–0x7FFF)
    u8   readZ80Port (u16 addr);
    void writeZ80Port(u16 addr, u8 val);

private:
    u8   _readPad  (u32 pad);
    u8   _ioRead8  (u32 addr);
    void _ioWrite8 (u32 addr, u8 val);
};

// ─────────────────────────────────────────────────────────────────────────────
// M68K — Motorola 68000 CPU
// ─────────────────────────────────────────────────────────────────────────────
class M68K {
public:
    GenBus* bus;

    u32  d[8];       // Data registers D0–D7
    u32  a[8];       // Address registers A0–A7 (A7 = SSP in supervisor mode)
    u32  pc;         // Program counter
    u16  sr;         // Status register: [T|–|S|–|–|I2|I1|I0|–|–|–|X|N|Z|V|C]
    bool stopped;    // CPU halted by STOP instruction (resumes on interrupt)
    u32  usp;        // User stack pointer (shadowed when in supervisor mode)
    u32  cycles;     // Accumulated cycles for the current burst

    explicit M68K(GenBus* bus);
    void reset();

    // Run until cycles >= targetCycles. Resets cycles to 0, returns overshoot.
    u32  run(u32 targetCycles);
    // Decode and execute one instruction.
    void step();

    // Raise interrupt at level 1–7. Returns true if the interrupt was accepted.
    bool interrupt(u32 level);
    // Push exception frame and dispatch through the vector table.
    void exception(u32 vector);

private:
    // ── Helpers ──────────────────────────────────────────────────────────────
    s32 sext8 (u32 v) { return static_cast<s32>(static_cast<s8> (static_cast<u8> (v))); }
    s32 sext16(u32 v) { return static_cast<s32>(static_cast<s16>(static_cast<u16>(v))); }

    u16 fetch16();
    u32 fetch32();

    u32  readDn (u32 n, u32 sz);
    void writeDn(u32 n, u32 v, u32 sz);

    // Effective address engine
    u32  calcEA (u32 mode, u32 reg, u32 sz);
    u32  readEA (u32 mode, u32 reg, u32 sz);
    void writeEA(u32 mode, u32 reg, u32 val, u32 sz);

    // ── Flag operations ───────────────────────────────────────────────────────
    void _masks(u32 sz, u32& mask, u32& msb);
    void setNZ  (u32 r, u32 sz);
    void setNZVC(u32 r, u32 sz);
    u32  doAdd  (u32 src, u32 dst, u32 sz, bool withX);
    u32  doSub  (u32 src, u32 dst, u32 sz, bool withX);
    void doCmp  (u32 src, u32 dst, u32 sz);
    bool testCC (u32 cc);

    // ── Instruction groups (one per top-nibble, matching JS naming) ───────────
    void _g0(u16 op);
    void _g0Special(u16 op, u32 b11_8, u32 srcMode, u32 srcReg, u32 dstReg);
    void _doBitOp(u32 typ, u32 num, u32 mode, u32 reg, u32 v);
    void _gMOVE(u16 op, u32 sz);
    void _g4(u16 op);
    void _g4E(u16 op, u32 mode, u32 reg, u32 sz);
    void _movemToMem  (u16 op, u32 mode, u32 reg, u32 sz);
    void _movemFromMem(u16 op, u32 mode, u32 reg, u32 sz);
    void _g5(u16 op);
    void _g6(u16 op);
    void _g7(u16 op);
    void _g8(u16 op);
    void _g9(u16 op);
    void _gB(u16 op);
    void _gC(u16 op);
    void _gD(u16 op);
    void _gE(u16 op);
    u32  _doShift(u32 type, bool left, u32 v, u32 cnt, u32 sz);
};

// ─────────────────────────────────────────────────────────────────────────────
// GenVDP — Yamaha YM7101 Video Display Processor
// ─────────────────────────────────────────────────────────────────────────────
class GenVDP {
public:
    GenBus* bus;

    u8  vram [GEN_VRAM_SIZE];              // 64 KB video RAM
    u16 cram [GEN_CRAM_WORDS];             // 64 palette entries (BGR9 format)
    u16 vsram[GEN_VSRAM_WORDS];            // 40 vertical scroll values
    u8  regs [GEN_VDP_REG_COUNT];          // VDP registers 0–23
    u8  framebuf[GEN_W * GEN_H_MAX * 4];   // RGBA output (320×240)

    // ── Control port state machine ────────────────────────────────────────────
    bool ctrlPendWord;  // awaiting 2nd word of an address/DMA command
    bool ctrlPendByte;  // awaiting 2nd byte of a byte-write register command
    u16  ctrlFirst;     // first word/byte of a pending two-part command
    u16  addrReg;       // current VRAM/CRAM/VSRAM address pointer
    u8   addrInc;       // auto-increment after each data port access (reg 15)
    u8   cdReg;         // code bits: CD3–CD0 select destination memory type

    // ── Scanline counters and status flags ────────────────────────────────────
    u16  vcounter;
    u16  hcounter;
    bool vblank;
    bool hblank;
    bool dmaActive;
    u32  frame;         // completed frame count (incremented on VBlank)
    bool isPAL;
    bool vintPending;   // F flag — set on VBlank, cleared on status read

    // ── DMA state ─────────────────────────────────────────────────────────────
    u16  dmaFillData;    // last word written to data port (high byte = fill value)
    bool dmaFillPending; // VRAM fill DMA is armed, waiting for first data write
    u32  diagDmaCount;   // diagnostic: total DMA operations executed
    bool vramDirty;      // at least one VRAM write has occurred this frame

    explicit GenVDP(GenBus* bus);
    void reset();

    // ── Port I/O — called by GenBus ───────────────────────────────────────────
    u8   read8 (u32 off);
    u16  read16(u32 off);
    void write8 (u32 off, u8  val);
    void write16(u32 off, u16 val);

    // ── Timing hooks — called by Genesis::runFrame ────────────────────────────
    // tickLine: advance vcounter; renders line; returns true on VBlank entry.
    bool tickLine  (u32 line, bool pal);
    // checkHInt: returns true if an HBlank interrupt should fire this line.
    bool checkHInt (u32 line, bool pal);

private:
    // Control port
    void _writeCtrl    (u16 val, bool isByte);
    void _processCtrlWord(u16 w);

    // Data port
    void _writeData(u16 val);
    u16  _readData ();
    u16  _status   ();

    // DMA engine
    void _processDMA    (u8 cd);
    void _dmaMemoryCopy (u32 srcAddr, u32 len, u8 cd);
    void _dmaVRAMFill   (u32 len, u8 cd);
    void _dmaVRAMCopy   (u32 len, u8 cd);
    void _writeByCD     (u16 addr, u16 val, u8 cd);

    // Rendering (Step 5 will flesh these out fully)
    void _renderLine      (u32 line);
    void _renderScanline  (u32 y);
    void _renderPlaneLine (bool isB, u32 y);
    void _renderSpriteLine(u32 y);

    struct RGB { u8 r, g, b; };
    RGB  _decodeCRAMColor(u16 color);
};

// ─────────────────────────────────────────────────────────────────────────────
// GenZ80 — Zilog Z80 secondary CPU
// Drives the YM2612 and provides SMS backward compatibility.
// The JS version is a partial stub; this will be a full implementation
// in Step 6.
// ─────────────────────────────────────────────────────────────────────────────
class GenZ80 {
public:
    GenBus* bus;
    u32     cycles;

    // ── Main registers ────────────────────────────────────────────────────────
    u8  A, F, B, C, D, E, H, L;
    // ── Alternate registers (AF', BC', DE', HL') ──────────────────────────────
    u8  A_, F_, B_, C_, D_, E_, H_, L_;
    // ── Index and pointer registers ───────────────────────────────────────────
    u16 IX, IY, SP, PC;
    // ── Interrupt control ─────────────────────────────────────────────────────
    u8   IFF1, IFF2;  // interrupt flip-flops
    u8   IM;          // interrupt mode (0, 1, or 2)
    u8   I;           // interrupt vector register
    u8   R;           // DRAM refresh counter
    bool halted;

    // 8 KB internal RAM (Z80 address space 0x0000–0x1FFF)
    u8 ram[GEN_Z80RAM_SIZE];

    explicit GenZ80(GenBus* bus);
    void reset();

    // Run Z80 for at least targetCycles cycles.
    void run(u32 targetCycles);

    // Save / load state (called by Genesis::saveState / loadState)
    void saveState(struct DingSaveWriter* w);
    void loadState(struct DingSaveReader* r);

private:
    u8   _fetch  ();
    u8   _read   (u16 addr);
    void _write  (u16 addr, u8 val);
    u8   _inPort (u8 port);
    void _outPort(u8 port, u8 val);

    void _execute   (u8 op);
    void _executeCB (u8 op);              // bit / rotate / shift extended
    void _executeDD (u8 op);              // IX-prefixed
    void _executeDDCB(u16 addr, u8 op);  // IX-prefixed bit ops
    void _executeED (u8 op);              // extended (block moves, I/O, etc.)
    void _executeFD (u8 op);              // IY-prefixed
    void _executeFDCB(u16 addr, u8 op);  // IY-prefixed bit ops

    // ── ALU helpers (update F register correctly) ─────────────────────────────
    u8   _inc8(u8 v);
    u8   _dec8(u8 v);
    u8   _add8(u8 a, u8 b, bool carry = false);
    u8   _sub8(u8 a, u8 b, bool borrow = false);
    u8   _and8(u8 a, u8 b);
    u8   _or8 (u8 a, u8 b);
    u8   _xor8(u8 a, u8 b);
    void _cp8 (u8 a, u8 b);
    u16  _add16(u16 a, u16 b);
    u16  _adc16(u16 a, u16 b);
    u16  _sbc16(u16 a, u16 b);

    // ── Stack helpers ─────────────────────────────────────────────────────────
    void _push16(u16 v);
    u16  _pop16 ();
};

// ─────────────────────────────────────────────────────────────────────────────
// Genesis — main orchestrator
// Owns all subsystems; drives the run loop; exposes save/load and diagnostics.
// ─────────────────────────────────────────────────────────────────────────────
class Genesis {
public:
    // Member declaration order == initialization order in constructor.
    // bus must come first because vdp/z80/cpu take a GenBus* at construction.
    GenBus bus;
    GenVDP vdp;
    GenZ80 z80;
    GenAPU apu;
    M68K   cpu;

    // Region / timing
    bool isPAL;
    u32  linesFrame;   // total scanlines per frame (NTSC=262, PAL=313)
    u32  activeLines;  // active display lines (NTSC=224, PAL=240)
    u32  cpl;          // M68K cycles per scanline
    u32  z80cpl;       // Z80 cycles per scanline
    s32  overshoot;    // unused M68K cycles carried into next scanline

    // Error state — checked by ding_has_error / ding_diag_last_error
    bool errorFlag;
    char errorMsg[256];

    Genesis();

    // Core lifecycle
    void reset();
    bool loadROM(const u8* data, u32 size);
    void setRegion(bool pal);

    // Input — forwarded to GenBus
    void pressButton(u32 pad, u32 btn, bool pressed);

    // Run one complete video frame
    void runFrame();

    // Save / load state — serialize into / from a caller-owned buffer.
    // Returns false on failure (buffer too small, bad magic, etc.)
    bool saveState(u8* buf, u32 bufSize, u32* outSize);
    bool loadState(const u8* buf, u32 size);

    // Diagnostics — write human-readable state into null-terminated out buffer
    void diagCPU  (char* out, u32 outSize);
    void diagVideo(char* out, u32 outSize);

private:
    void _setError(const char* msg);
};
