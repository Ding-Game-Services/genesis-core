#pragma once

#include <cstddef>
#include <cstring>
#include <cstdio>
#include <vector>

#include "ding_types.h"
#include "ding_audio.h"

// FIX: Forward declarations. 
// This tells the compiler these types exist without needing the full header yet.
struct DingSaveWriter;
struct DingSaveReader;

static constexpr u32 GEN_W       = 320;
static constexpr u32 GEN_H_NTSC  = 224;
static constexpr u32 GEN_H_PAL   = 240;
static constexpr u32 GEN_H_MAX   = 240;

static constexpr u32 NTSC_LINES   = 262;
static constexpr u32 NTSC_ACTIVE  = 224;
static constexpr u32 NTSC_CPL     = 488;
static constexpr u32 NTSC_Z80_CPL = 228;

static constexpr u32 PAL_LINES    = 313;
static constexpr u32 PAL_ACTIVE   = 240;
static constexpr u32 PAL_CPL      = 487;
static constexpr u32 PAL_Z80_CPL  = 228;

static constexpr u32 GEN_WRAM_SIZE   = 0x10000;
static constexpr u32 GEN_Z80RAM_SIZE = 0x2000;
static constexpr u32 GEN_SRAM_MAX    = 0x8000;
static constexpr u32 GEN_VRAM_SIZE   = 0x10000;
static constexpr u32 GEN_CRAM_WORDS  = 64;
static constexpr u32 GEN_VSRAM_WORDS = 40;
static constexpr u32 GEN_VDP_REG_COUNT = 24;
static constexpr u32 GEN_PAD_COUNT  = 2;

static constexpr u32 GEN_AUDIO_RATE     = 44100;
static constexpr u32 GEN_AUDIO_CHANNELS = 2;
static constexpr u32 GEN_AUDIO_CAPACITY = 4096;
static constexpr u32 GEN_AUDIO_STORAGE  = GEN_AUDIO_CAPACITY * GEN_AUDIO_CHANNELS;

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

class GenBus;
class M68K;
class GenVDP;
class GenZ80;
class GenAPU;

class YM2612 {
public:
    u8 regs[0x200];
    u8 status;
    YM2612();
    void reset();
    void write(u32 bank, u8 reg, u8 val);
    u8   read() const;
    void clock(float* buf, u32 count);
    float renderSample();   // advances state by 1/GEN_AUDIO_RATE, returns mono sample
private:
    // v1 FM engine: functional 4-op/6-channel synthesis with simplified
    // (non-exponential-table) envelope timing. Detune (DT1) and LFO are
    // not yet applied — deferred alongside exact hardware rate tables.
    struct FMOperator {
        double phase;
        float  envLevel;   // 0..1 linear envelope amplitude
        u8     envState;   // 0=attack 1=decay1 2=decay2/sustain 3=release 4=idle
        bool   keyOn;
        FMOperator() : phase(0.0), envLevel(0.0f), envState(4), keyOn(false) {}
    };
    struct FMChannel {
        FMOperator op[4];
        double fbHist[2];   // op1 self-feedback history (last two samples)
        FMChannel() { fbHist[0] = fbHist[1] = 0.0; }
    };
    FMChannel ch_[6];

    void   _keyOnOff(u8 val);
    double _channelFreqHz(u32 ch) const;
    float  _stepEnvelope(FMOperator& o, u32 ar, u32 d1r, u32 d2r, u32 rr, u32 sl);
    float  _renderOperator(u32 ch, u32 opIdx, double freqHz, float modInput);
};

class SN76489 {
public:
    u8 regs[8];   // kept for diagnostics; not used by synthesis path
    u8 latch;

    // Synthesis state
    u16 freq[3];        // 10-bit tone frequency, channels 0-2
    u8  atten[4];        // 4-bit attenuation, channels 0-2 + noise (index 3)
    u8  noiseCtrl;       // bit2 = FB (white/periodic), bits1:0 = rate select
    double tonePhase[3];
    double noisePhase;
    u16 noiseShift;

    SN76489();
    void reset();
    void write(u8 val);
    void clock(float* buf, u32 count);
    float renderSample();  // advances state by 1/GEN_AUDIO_RATE, returns mono sample
};

class GenAPU {
public:
    YM2612 ym2612;
    SN76489 psg;
    u8 lastYMReg;
    DingAudioBuffer audioBuf;
    float           audioStorage[GEN_AUDIO_STORAGE];
    GenAPU();
    void reset();
    void writeYM (u32 bank, u8 reg, u8 val);
    u8   readYM  () const;
    void writePSG(u8 val);
    void generateFrame(u32 samplesNeeded);
};

class GenBus {
public:
    std::vector<u8> rom;
    u8 wram   [GEN_WRAM_SIZE];
    u8 z80Ram [GEN_Z80RAM_SIZE];
    u8   sramData[GEN_SRAM_MAX];
    bool hasSRAM;
    bool sramDirty;
    u32  sramStart;
    u32  sramEnd;
    u32  sramSize;
    u32 padState[GEN_PAD_COUNT];
    u8  padCtrl [GEN_PAD_COUNT];
    u8  padTH   [GEN_PAD_COUNT];
 bool z80BusReq;
    bool z80Reset;
    u32  z80Bank;
    // Diagnostic-only: tracks every write attempt to the Z80 RESET
    // register ($A11200/$A11201), regardless of resulting value, so we
    // can tell "68k never tried to release Z80" apart from "68k tried
    // but the write didn't stick." Not part of save state.
    u32  z80ResetWriteCount;
    u8   z80ResetLastVal;
    bool isPAL;
    GenVDP* vdp;
    GenZ80* z80;
    GenAPU* apu;
    M68K*   m68k;
    GenBus();
    void reset();
    void loadROM(const u8* data, u32 size);
    void pressButton(u32 pad, GenBtn btn, bool pressed);
    u8   read8 (u32 addr);
    u16  read16(u32 addr);
    u32  read32(u32 addr);
    void write8 (u32 addr, u8  val);
    void write16(u32 addr, u16 val);
    void write32(u32 addr, u32 val);
    u32  readSize (u32 addr, u32 sz);
    void writeSize(u32 addr, u32 val, u32 sz);
    u8   readZ80Port (u16 addr);
    void writeZ80Port(u16 addr, u8 val);
private:
    u8   _readPad  (u32 pad);
    u8   _ioRead8  (u32 addr);
    void _ioWrite8 (u32 addr, u8 val);
};

class M68K {
public:
    GenBus* bus;
    u32  d[8], a[8], pc;
    u16  sr;
    bool stopped;
    u32  usp, cycles;
    explicit M68K(GenBus* bus);
    void reset();
    u32  run(u32 targetCycles);
    void step();
    bool interrupt(u32 level);
    void exception(u32 vector);
	
	    // Debug: ring buffer of the last N instruction start addresses.
 static constexpr u32 TRACE_SIZE = 64;
    u32 traceBuf[TRACE_SIZE];
    u32 traceIdx;
    u64 totalSteps;
private:
    s32 sext8 (u32 v) { return static_cast<s32>(static_cast<s8> (static_cast<u8> (v))); }
    s32 sext16(u32 v) { return static_cast<s32>(static_cast<s16>(static_cast<u16>(v))); }
    u16 fetch16();
    u32 fetch32();
    u32  readDn (u32 n, u32 sz);
    void writeDn(u32 n, u32 v, u32 sz);
    u32  calcEA (u32 mode, u32 reg, u32 sz);
    u32  readEA (u32 mode, u32 reg, u32 sz);
    void writeEA(u32 mode, u32 reg, u32 val, u32 sz);
    // Read-modify-write helpers: compute the effective address ONCE and hand
    // it back so the paired write reuses it instead of recomputing calcEA().
    // Recomputing calcEA() for (An)+/-(An) double-steps the address register,
    // and for modes that consume extension words (d16(An), Absolute, PC-rel)
    // it double-fetches those words and desyncs PC. This is the fix for that.
    u32  _rmwRead (u32 mode, u32 reg, u32 sz, u32& ea);
    void _rmwWrite(u32 mode, u32 reg, u32 sz, u32 ea, u32 val);
    void _masks(u32 sz, u32& mask, u32& msb);
    void setNZ  (u32 r, u32 sz);
    void setNZVC(u32 r, u32 sz);
    u32  doAdd  (u32 src, u32 dst, u32 sz, bool withX);
    u32  doSub  (u32 src, u32 dst, u32 sz, bool withX);
    void doCmp  (u32 src, u32 dst, u32 sz);
    bool testCC (u32 cc);
    void _g0(u16 op);
    void _g0Special(u16 op, u32 b11_8, u32 srcMode, u32 srcReg, u32 dstReg);
	u32  _doBitOp(u32 typ, u32 num, u32 v, bool& doWrite);
    void _gMOVE(u16 op);
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

class GenVDP {
public:
    GenBus* bus;
    u8  vram [GEN_VRAM_SIZE];
    u16 cram [GEN_CRAM_WORDS];
    u16 vsram[GEN_VSRAM_WORDS];
    u8  regs [GEN_VDP_REG_COUNT];
    u8  framebuf[GEN_W * GEN_H_MAX * 4];
    bool ctrlPendWord, ctrlPendByte;
    u16  ctrlFirst, addrReg;
    u8   addrInc, cdReg;
    u16  vcounter, hcounter;
bool vblank, hblank, dmaActive;
    u32  frame;
    bool isPAL, vintPending, dmaFillPending;
    bool spriteOverflow, spriteCollision;  // status bits 6 and 5
    u16  dmaFillData;
    u32  diagDmaCount;
bool vramDirty;
    // Diagnostic-only: counts writes landing at VRAM address >= 0xC000
    // (nametable region) from ANY path — CPU port writes or any DMA
    // mode — plus the last address/value written there, if any.
    u32  highVramWriteCount;
    u16  highVramLastAddr;
    u8   highVramLastVal;
    // Narrower diagnostic: counts writes to 0xC000-0xE0FF (the actual
    // nametable range) where the value is NOT the 0xFF fill sentinel —
    // i.e. real tile-index data. Answers "has real data ever landed
    // here at all" independent of how many fill/clear writes happened.
 u32  nametableRealDataCount;
    u16  nametableRealLastAddr;
    u8   nametableRealLastVal;
    // Diagnostic-only: captures VRAM-fill DMA (mode 2) parameters.
    // "First" is latched once (never overwritten) so we see the very
    // first fill that ever ran. "Last" updates every time, so comparing
    // the two shows whether fill length/dest is changing over time or
    // it's the same fill re-firing repeatedly.
    u32  fillDmaTriggerCount;
    u16  fillFirstDestAddr, fillFirstLen;
    u8   fillFirstCd;
u16  fillLastDestAddr, fillLastLen;
    u8   fillLastCd;
    // Diagnostic-only: same idea, but for memory->VRAM copy DMA (mode
    // 0/1) whose destination range overlaps the nametables (0xC000-
    // 0xE1FF). Fill DMA was ruled out as the source of the post-write
    // 0xFF stomping; this checks whether a copy is the culprit instead.
    u32  copyOverlapCount;
    u32  copyOverlapFirstSrc, copyOverlapLastSrc;
    u16  copyOverlapFirstDest, copyOverlapFirstLen;
    u16  copyOverlapLastDest,  copyOverlapLastLen;
u8   copyOverlapFirstCd, copyOverlapLastCd;
    // Diagnostic-only: ring buffer of the last 8 writes to VDP regs
    // 21-23 (DMA source bank/mode registers) — logs exactly what value
    // was written to which register, in order, so we can see the real
    // write sequence leading into a bad DMA instead of inferring it.
    static constexpr u32 REG_LOG_SIZE = 8;
    struct RegLogEntry { u32 reg; u8 val; u32 pc; };
    RegLogEntry regLog[REG_LOG_SIZE];
    u32 regLogIdx;
    u32 regLogCount;
    explicit GenVDP(GenBus* bus);
 // Per-line compositing buffers (item 1: priority bit support).
// Encoding: 0 = transparent, else (colorIndex & 0x3F) | (priority ? 0x80 : 0).
// colorIndex is the direct CRAM word index (palLine*16+nibble), not a raw palette nibble,
// so 0x3F is enough range (CRAM has 64 entries) and bit 7 is free for priority.
u8   lineA[GEN_W];
    u8   lineB[GEN_W];
    u8   lineSpr[GEN_W];
    // Shadow/Highlight operator markers (item 6). Sprite palette line 3,
    // index 14/15 are not real colors on real hardware — they're "operator"
    // pixels that force highlight/shadow on whatever's underneath instead
    // of drawing anything themselves. 0 = none, 1 = force highlight, 2 = force shadow.
    u8   lineShMark[GEN_W];
    void reset();
    u8   read8 (u32 off);
    u16  read16(u32 off);
    void write8 (u32 off, u8  val);
    void write16(u32 off, u16 val);
    bool tickLine  (u32 line, bool pal);
    bool checkHInt (u32 line, bool pal);
private:
    void _writeCtrl    (u16 val, bool isByte);
    void _processCtrlWord(u16 w);
    void _writeData(u16 val);
	void _writeVRAMByte(int bytePos, u8 val);
    u16  _readData ();
    u16  _status   ();
    void _processDMA    (u8 cd);
    void _dmaMemoryCopy (u32 srcAddr, u32 len, u8 cd);
    void _dmaVRAMFill   (u32 len, u8 cd);
    void _dmaVRAMCopy   (u32 len, u8 cd);
    void _writeByCD     (u16 addr, u16 val, u8 cd);
    void _renderLine      (u32 line);
    void _renderScanline  (u32 y);
void _renderPlaneLine (bool isB, u32 y);
    void _renderWindowLine(u32 y);
    void _renderSpriteLine(u32 y);
    void _compositeLine   (u32 y);
    struct RGB { u8 r, g, b; };
 RGB  _decodeCRAMColor(u16 color);
    void _logRegWrite(u32 r, u8 v);
};

class GenZ80 {
public:
    GenBus* bus;
    u32     cycles;
    u8  A, F, B, C, D, E, H, L;
    u8  A_, F_, B_, C_, D_, E_, H_, L_;
    u16 IX, IY, SP, PC;
    u8   IFF1, IFF2, IM, I, R;
    bool halted;
    u8 ram[GEN_Z80RAM_SIZE];

    explicit GenZ80(GenBus* bus);
    void reset();
    void run(u32 targetCycles);
    bool interrupt();

    // FIX: Removed 'struct' keyword. 
    // Now using the Type name defined in the SDK.
    void saveState(DingSaveWriter* w);
    void loadState(DingSaveReader* r);

    u8   _fetch  ();
    u8   _read   (u16 addr);
    void _write  (u16 addr, u8 val);
    u8   _inPort (u8 port);
    void _outPort(u8 port, u8 val);
    void _execute   (u8 op);
    void _executeCB (u8 op);
    void _executeDD (u8 op);
    void _executeDDCB(u16 addr, u8 op);
    void _executeED (u8 op);
    void _executeFD (u8 op);
    void _executeFDCB(u16 addr, u8 op);
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
    void _push16(u16 v);
    u16  _pop16 ();
};

class Genesis {
public:
    GenBus bus;
    GenVDP vdp;
    u32 frame = 0;
    bool isPAL = false;	
    GenZ80 z80;
    GenAPU apu;
    M68K   cpu;
    u32  linesFrame, activeLines, cpl, z80cpl;
    s32  overshoot;
    bool errorFlag;
    char errorMsg[256];
    Genesis();
    void reset();
    bool loadROM(const u8* data, u32 size);
    void setRegion(bool pal);
    void pressButton(u32 pad, u32 btn, bool pressed);
    void runFrame();
    bool saveState(u8* buf, u32 bufSize, u32* outSize);
    bool loadState(const u8* buf, u32 size);
void diagCPU  (char* out, u32 outSize);
    void diagVideo(char* out, u32 outSize);
void diagTrace(char* out, u32 outSize);
 u32 z80RunCount = 0;
    u32 z80SkipCount = 0;
    // Diagnostic-only: how many times VBlank (level 6) was requested vs.
    // actually accepted by the CPU. If requests climb but accepts never
    // do, the interrupt is being permanently masked (IPL never drops
    // below 7), which would explain a VBlank-driven VRAM transfer queue
    // never flushing.
    u32 vintRequestCount = 0;
    u32 vintAcceptCount  = 0;
private:
    void _setError(const char* msg);
};
