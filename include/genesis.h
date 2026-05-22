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
};

class SN76489 {
public:
    u8 regs[8];
    u8 latch;
    SN76489();
    void reset();
    void write(u8 val);
    void clock(float* buf, u32 count);
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
    void _masks(u32 sz, u32& mask, u32& msb);
    void setNZ  (u32 r, u32 sz);
    void setNZVC(u32 r, u32 sz);
    u32  doAdd  (u32 src, u32 dst, u32 sz, bool withX);
    u32  doSub  (u32 src, u32 dst, u32 sz, bool withX);
    void doCmp  (u32 src, u32 dst, u32 sz);
    bool testCC (u32 cc);
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
    u16  dmaFillData;
    u32  diagDmaCount;
    bool vramDirty;
    explicit GenVDP(GenBus* bus);
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
    void _renderSpriteLine(u32 y);
    struct RGB { u8 r, g, b; };
    RGB  _decodeCRAMColor(u16 color);
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
    GenZ80 z80;
    GenAPU apu;
    M68K   cpu;
    bool isPAL;
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
private:
    void _setError(const char* msg);
};
