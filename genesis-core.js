'use strict';

// ═══════════════════════════════════════════════════════════════
// GENESIS CORE — v0.1.0
// Sega Genesis / Mega Drive emulator core
// CPU:   Motorola 68000 (M68K) — complete implementation
// Video: Yamaha YM7101 VDP   — timing stub (v0.1)
// Co-CPU: Zilog Z80           — timing stub (v0.1)
// Audio: YM2612 + SN76489    — silence stub (v0.1)
// ═══════════════════════════════════════════════════════════════

// ── Display constants ────────────────────────────────────────
const GEN_W     = 320;
const GEN_H     = 224;  // NTSC active lines
const GEN_SCALE = 2;

// ── GEN_BTN indices (matches frontend) ───────────────────────
const GEN_BTN = { UP:0, DOWN:1, LEFT:2, RIGHT:3, A:4, B:5, C:6, START:7, X:8, Y:9, Z:10, MODE:11 };

// ── Timing (NTSC) ────────────────────────────────────────────
// Master clock 53,693,175 Hz; M68K = master/7 ≈ 7,670,454 Hz
// 262 lines/frame × 60 fps; ~488 M68K cycles/line
const NTSC_LINES        = 262;
const NTSC_ACTIVE       = 224;
const NTSC_CPL          = 488;   // M68K cycles per scanline (NTSC)
const PAL_LINES         = 313;
const PAL_ACTIVE        = 240;
const PAL_CPL           = 487;   // M68K cycles per scanline (PAL)

// ─────────────────────────────────────────────────────────────
// MD5 — pure-JS, no dependencies
// Based on RFC 1321 reference implementation (public domain)
// ─────────────────────────────────────────────────────────────
function md5(arrayBuffer) {
  const data = new Uint8Array(arrayBuffer);
  const len  = data.length;

  // Pre-processing: pad message to multiple of 512 bits
  const bitLen    = len * 8;
  const padLen    = ((len % 64) < 56) ? (56 - (len % 64)) : (120 - (len % 64));
  const msgLen    = len + padLen + 8;
  const msg       = new Uint8Array(msgLen);
  msg.set(data);
  msg[len] = 0x80;
  // Append bit length as little-endian 64-bit
  for (let i = 0; i < 4; i++) msg[len + padLen + i]     = (bitLen >>> (i*8)) & 0xFF;

  // Per-round shift amounts and K constants
  const S = [7,12,17,22,7,12,17,22,7,12,17,22,7,12,17,22,
             5, 9,14,20,5, 9,14,20,5, 9,14,20,5, 9,14,20,
             4,11,16,23,4,11,16,23,4,11,16,23,4,11,16,23,
             6,10,15,21,6,10,15,21,6,10,15,21,6,10,15,21];
  const K = new Uint32Array(64);
  for (let i = 0; i < 64; i++) K[i] = (Math.abs(Math.sin(i+1)) * 0x100000000) >>> 0;

  let h0 = 0x67452301, h1 = 0xEFCDAB89, h2 = 0x98BADCFE, h3 = 0x10325476;

  for (let i = 0; i < msgLen; i += 64) {
    const M = new Uint32Array(16);
    for (let j = 0; j < 16; j++)
      M[j] = msg[i+j*4] | (msg[i+j*4+1]<<8) | (msg[i+j*4+2]<<16) | (msg[i+j*4+3]<<24);
    let a=h0, b=h1, c=h2, d=h3;
    for (let j = 0; j < 64; j++) {
      let F, g;
      if      (j < 16) { F = (b & c) | (~b & d);        g = j; }
      else if (j < 32) { F = (d & b) | (~d & c);        g = (5*j+1) % 16; }
      else if (j < 48) { F = b ^ c ^ d;                 g = (3*j+5) % 16; }
      else             { F = c ^ (b | ~d);               g = (7*j)   % 16; }
      F = (F + a + K[j] + M[g]) >>> 0;
      a = d; d = c; c = b;
      b = (b + ((F << S[j]) | (F >>> (32-S[j])))) >>> 0;
    }
    h0 = (h0+a)>>>0; h1 = (h1+b)>>>0; h2 = (h2+c)>>>0; h3 = (h3+d)>>>0;
  }

  const le = (v) => [v&0xFF,(v>>8)&0xFF,(v>>16)&0xFF,(v>>24)&0xFF];
  return [...le(h0),...le(h1),...le(h2),...le(h3)].map(b=>b.toString(16).padStart(2,'0')).join('').toUpperCase();
}

// ─────────────────────────────────────────────────────────────
// ROM header parsing
// Genesis domestic title: 0x120–0x14F
// International title:    0x150–0x17F
// ─────────────────────────────────────────────────────────────
function romHeaderTitle(arrayBuffer) {
  const v = new Uint8Array(arrayBuffer);
  if (v.length < 0x180) return '';
  let s = '';
  for (let i = 0x150; i < 0x180; i++) {
    const b = v[i];
    if (b === 0) break;
    if (b >= 0x20 && b < 0x7F) s += String.fromCharCode(b);
  }
  return s.trim().replace(/\s+/g, ' ');
}

// ─────────────────────────────────────────────────────────────
// BUS — Genesis memory map dispatcher
// 0x000000–0x3FFFFF : ROM (up to 4 MB)
// 0xA00000–0xA0FFFF : Z80 address space (via 68K)
// 0xA10000–0xA1001F : I/O (controllers, version)
// 0xA11100          : Z80 BUSREQ
// 0xA11200          : Z80 RESET
// 0xA14000          : TMSS register
// 0xC00000–0xC0001F : VDP ports
// 0xE00000–0xFFFFFF : WRAM (64 KB, repeating)
// ─────────────────────────────────────────────────────────────
class GenBus {
  constructor() {
    this.rom        = null;
    this.romSize    = 0;
    this.fullRom    = null;
    this.wram       = new Uint8Array(0x10000);  // 64 KB WRAM
    this.z80Ram     = new Uint8Array(0x2000);   // 8 KB Z80 internal RAM
    this.sramData   = new Uint8Array(0x8000);   // 32 KB SRAM (max)
    this.hasSRAM    = false;
    this.sramDirty  = false;
    this.sramStart  = 0;
    this.sramEnd    = 0;

    // Controller state: each entry = bitmask of pressed buttons (our internal format)
    this._padState = [0, 0];
    // Control register bit 6 = TH direction (1=output); bit 5 = TH value when output
    this._padCtrl  = [0x40, 0x40];
    this._padTH    = [1, 1];    // TH line state for each pad

// Z80 bus signals
    this.z80BusReq = false;
    this.z80Reset  = true;
    this.z80Bank   = 0;      // 15-bit bank register for Z80→68K window

    // Subsystem references (set by Genesis class)
    this.vdp  = null;
    this.z80  = null;
    this.apu  = null;
    this.m68k = null;
  }

  loadROM(buf) {
    this.fullRom = new Uint8Array(buf);
    this.rom     = this.fullRom;
    this.romSize = this.rom.length;

    // Detect battery-backed SRAM from ROM header at 0x1B0
    // Format: "RA" (0x52 0x41) followed by type byte and address range
    if (this.romSize >= 0x1C0) {
      const b0 = this.rom[0x1B0], b1 = this.rom[0x1B1];
      if (b0 === 0x52 && b1 === 0x41) {
        const typ = this.rom[0x1B2];
        if ((typ & 0xA0) === 0xA0) { // SRAM type
          this.hasSRAM   = true;
          this.sramStart = ((this.rom[0x1B4]<<24)|(this.rom[0x1B5]<<16)|(this.rom[0x1B6]<<8)|this.rom[0x1B7]) >>> 0;
          this.sramEnd   = ((this.rom[0x1B8]<<24)|(this.rom[0x1B9]<<16)|(this.rom[0x1BA]<<8)|this.rom[0x1BB]) >>> 0;
          const sz = Math.max(this.sramEnd - this.sramStart + 1, 0x2000);
          this.sramData  = new Uint8Array(sz);
        }
      }
    }
  }

  pressButton(pad, btn, pressed) {
    if (pressed) this._padState[pad] |=  (1 << btn);
    else         this._padState[pad] &= ~(1 << btn);
  }

  _readPad(pad) {
    const s  = this._padState[pad];
    const th = this._padTH[pad];
    // Returns 6-bit value (bits 5-0), active LOW for pressed buttons
    // Bit 6 = TH state, bits 7 = ??? (usually 1)
    let v = 0x7F; // all unpressed (high)
    if (th) {
      // TH=1: C, B, Right, Left, Down, Up (bits 5..0)
      if (s & (1<<GEN_BTN.UP))    v &= ~0x01;
      if (s & (1<<GEN_BTN.DOWN))  v &= ~0x02;
      if (s & (1<<GEN_BTN.LEFT))  v &= ~0x04;
      if (s & (1<<GEN_BTN.RIGHT)) v &= ~0x08;
      if (s & (1<<GEN_BTN.B))     v &= ~0x10;
      if (s & (1<<GEN_BTN.C))     v &= ~0x20;
      v |= 0x40; // TH=1
    } else {
      // TH=0: Start, A, 0, 0, Down, Up (bits 5..0)
      if (s & (1<<GEN_BTN.UP))    v &= ~0x01;
      if (s & (1<<GEN_BTN.DOWN))  v &= ~0x02;
      v &= ~0x0C; // bits 2-3 always 0 when TH=0
      if (s & (1<<GEN_BTN.A))     v &= ~0x10;
      if (s & (1<<GEN_BTN.START)) v &= ~0x20;
      v &= ~0x40; // TH=0
    }
    return v;
  }

  // ── Z80 port access (0x2000–0x7FFF) ──────────────────────
  readZ80Port(addr) {
    // YM2612: 0x4000–0x4003
    if (addr >= 0x4000 && addr <= 0x4003) return this.apu ? this.apu.readYM() : 0xFF;
    // VDP: 0x7F00–0x7FFF (mirrored)
    if (addr >= 0x7F00) return this.vdp ? this.vdp.read8(addr & 0x1F) : 0xFF;
    return 0xFF;
  }

  writeZ80Port(addr, val) {
    // YM2612: 0x4000–0x4003
    if (addr >= 0x4000 && addr <= 0x4003) {
      if (this.apu) {
        const bank = (addr >> 1) & 1;
        if (addr & 1) this.apu.writeYM(bank, this.apu._lastYMReg ?? 0, val);
        else          this.apu._lastYMReg = val;
      }
      return;
    }
    // Bank register: 0x6000–0x60FF — serial 9-bit shift register
    if (addr >= 0x6000 && addr < 0x6100) {
      this.z80Bank = ((this.z80Bank >> 1) | ((val & 1) << 8)) & 0x1FF;
      return;
    }
    // PSG: 0x7F11 (mirrored)
    if (addr >= 0x7F00) {
      if (this.apu) this.apu.writePSG(val);
      return;
    }
    // VDP: 0x7F00–0x7FFF
    if (addr >= 0x7E00 && addr < 0x7F00) {
      if (this.vdp) this.vdp.write8(addr & 0x1F, val);
      return;
    }
  }

  // ── Size helpers ──────────────────────────────────────────
  readSize(addr, size) {
    if (size === 0) return this.read8(addr);
    if (size === 1) return this.read16(addr);
    return this.read32(addr);
  }
  writeSize(addr, val, size) {
    if (size === 0) this.write8(addr, val);
    else if (size === 1) this.write16(addr, val);
    else this.write32(addr, val);
  }

  // ── Core read ─────────────────────────────────────────────
  read8(addr) {
    addr >>>= 0;
    const a24 = addr & 0xFFFFFF;

    // ROM
    if (a24 < 0x400000) {
      if (a24 < this.romSize) return this.rom[a24];
      return 0xFF;
    }
    // SRAM
    if (this.hasSRAM && a24 >= this.sramStart && a24 <= this.sramEnd)
      return this.sramData[(a24 - this.sramStart) & (this.sramData.length-1)];
    // Z80 address space
    if (a24 >= 0xA00000 && a24 < 0xA10000)
      return this.z80Ram[a24 & 0x1FFF];
    // I/O area
    if (a24 >= 0xA10000 && a24 < 0xA10020) return this._ioRead8(a24);
    if (a24 === 0xA11100 || a24 === 0xA11101) return 0x00; // Z80 bus always immediately granted
    if (a24 === 0xA11200 || a24 === 0xA11201) return this.z80Reset ? 0x00 : 0xFF;
    // VDP
    if (a24 >= 0xC00000 && a24 < 0xC00020) return this.vdp ? this.vdp.read8(a24 & 0x1F) : 0xFF;
    // WRAM — 64 KB at 0xFF0000, mirrored throughout 0xE00000–0xFFFFFF
    if (a24 >= 0xE00000) return this.wram[a24 & 0xFFFF];

    return 0xFF; // open bus
  }

  read16(addr) {
    addr >>>= 0;
    const a24 = addr & 0xFFFFFF;
    // Fast paths for hot regions
    if (a24 < 0x400000) {
      if (a24+1 < this.romSize) return (this.rom[a24] << 8) | this.rom[a24+1];
      return 0xFFFF;
    }
    if (a24 >= 0xE00000) {
      const wa = a24 & 0xFFFF;
      return (this.wram[wa] << 8) | this.wram[wa+1];
    }
    if (a24 >= 0xC00000 && a24 < 0xC00020) return this.vdp ? this.vdp.read16(a24 & 0x1F) : 0xFFFF;
    // Fall back to two byte reads
    return (this.read8(addr) << 8) | this.read8(addr+1);
  }

  read32(addr) {
    addr >>>= 0;
    return (((this.read16(addr) << 16) | this.read16(addr+2)) >>> 0);
  }

  // ── Core write ────────────────────────────────────────────
  write8(addr, val) {
    addr >>>= 0; val &= 0xFF;
    const a24 = addr & 0xFFFFFF;

    // SRAM
    if (this.hasSRAM && a24 >= this.sramStart && a24 <= this.sramEnd) {
      this.sramData[(a24 - this.sramStart) & (this.sramData.length-1)] = val;
      this.sramDirty = true; return;
    }
    // Z80 area
    if (a24 >= 0xA00000 && a24 < 0xA10000) { this.z80Ram[a24 & 0x1FFF] = val; return; }
    // I/O
    if (a24 >= 0xA10000 && a24 < 0xA10020) { this._ioWrite8(a24, val); return; }
    if (a24 === 0xA11100 || a24 === 0xA11101) { this.z80BusReq = (val & 1) !== 0; return; }
    if (a24 === 0xA11200 || a24 === 0xA11201) { this.z80Reset  = (val & 1) === 0; return; }
    if (a24 >= 0xA14000 && a24 < 0xA14004)   { /* TMSS — accept any write */ return; }
    // VDP
    if (a24 >= 0xC00000 && a24 < 0xC00020) { if (this.vdp) this.vdp.write8(a24 & 0x1F, val); return; }
    // WRAM
    if (a24 >= 0xE00000) { this.wram[a24 & 0xFFFF] = val; return; }
  }

  write16(addr, val) {
    addr >>>= 0; val &= 0xFFFF;
    const a24 = addr & 0xFFFFFF;
    if (a24 >= 0xE00000) {
      const wa = a24 & 0xFFFF;
      this.wram[wa] = (val >> 8) & 0xFF;
      this.wram[wa+1] = val & 0xFF;
      return;
    }
    if (a24 >= 0xC00000 && a24 < 0xC00020) { if (this.vdp) this.vdp.write16(a24 & 0x1F, val); return; }
    if (a24 >= 0xA11100 && a24 < 0xA11300) { this.write8(addr, (val>>8)&0xFF); this.write8(addr+1, val&0xFF); return; }
    this.write8(addr, (val >> 8) & 0xFF);
    this.write8(addr+1, val & 0xFF);
  }

  write32(addr, val) {
    addr >>>= 0; val >>>= 0;
    this.write16(addr,   (val >>> 16) & 0xFFFF);
    this.write16(addr+2, val & 0xFFFF);
  }

  // ── I/O register reads/writes (0xA10001–0xA1001F, odd bytes) ─
  _ioRead8(addr) {
    const off = addr & 0x1F;
    switch(off) {
      case 0x01: return 0xA0;  // version: overseas, NTSC, no TMSS
      case 0x03: return this._readPad(0);
      case 0x05: return this._readPad(1);
      case 0x07: return 0xFF;  // expansion port
      case 0x09: return this._padCtrl[0];
      case 0x0B: return this._padCtrl[1];
      default:   return 0xFF;
    }
  }

  _ioWrite8(addr, val) {
    const off = addr & 0x1F;
    switch(off) {
      case 0x03: break;  // data writes go to shift registers; we ignore for 3-btn
      case 0x05: break;
      case 0x09: {
        this._padCtrl[0] = val;
        // Bit 6: TH direction; if output, bit 6 of data port sets TH
        // For simplicity, TH follows ctrl bit 0 (set by game writing data port)
        break;
      }
      case 0x0B: { this._padCtrl[1] = val; break; }
      // Latch TH from data writes
      case 0x02: {
        // Writing data port 1 — TH is bit 6 when ctrl says it's output
        if (this._padCtrl[0] & 0x40) this._padTH[0] = (val >> 6) & 1;
        break;
      }
      case 0x04: {
        if (this._padCtrl[1] & 0x40) this._padTH[1] = (val >> 6) & 1;
        break;
      }
    }
  }
}

// ─────────────────────────────────────────────────────────────
// M68K — Motorola 68000 CPU
// Full instruction set, approximate cycle counts
// ─────────────────────────────────────────────────────────────
class M68K {
  constructor(bus) {
    this.bus     = bus;
    this.d       = new Uint32Array(8);  // D0–D7
    this.a       = new Uint32Array(8);  // A0–A7
    this.pc      = 0;
    this.sr      = 0x2700;              // supervisor, IPL=7
    this.stopped = false;
    this._usp    = 0;
    this.cycles  = 0;
  }

  reset() {
    this.d.fill(0);
    this.a.fill(0);
    this.a[7]    = this.bus.read32(0);  // SSP from vector table
    this.pc      = this.bus.read32(4);  // PC  from vector table
    this.sr      = 0x2700;
    this.stopped = false;
    this._usp    = 0;
    this.cycles  = 0;
  }


  // ── Sign extension ──────────────────────────────────────────
  sext8 (v) { return ((v &   0xFF) << 24) >> 24; }
  sext16(v) { return ((v & 0xFFFF) << 16) >> 16; }

  // ── Register accessors ──────────────────────────────────────
  readDn(n, sz) {
    if (sz === 0) return this.d[n] & 0xFF;
    if (sz === 1) return this.d[n] & 0xFFFF;
    return this.d[n] >>> 0;
  }
  writeDn(n, v, sz) {
    if      (sz === 0) this.d[n] = (this.d[n] & 0xFFFFFF00) | (v & 0xFF);
    else if (sz === 1) this.d[n] = (this.d[n] & 0xFFFF0000) | (v & 0xFFFF);
    else               this.d[n] = v >>> 0;
  }

  // ── Instruction fetch ────────────────────────────────────────
  fetch16() {
    const v = this.bus.read16(this.pc);
    this.pc = (this.pc + 2) >>> 0;
    return v;
  }
  fetch32() {
    const v = this.bus.read32(this.pc);
    this.pc = (this.pc + 4) >>> 0;
    return v;
  }

  // ── Effective address ────────────────────────────────────────
  // Returns the memory address for modes 2–7.
  // Modes 0 (Dn) and 1 (An) are handled directly in readEA/writeEA.
  calcEA(mode, reg, sz) {
    switch (mode) {
      case 2: return this.a[reg] >>> 0;                              // (An)
      case 3: {                                                       // (An)+
        const addr = this.a[reg] >>> 0;
        this.a[reg] = (this.a[reg] + (sz===0 ? (reg===7?2:1) : sz===1?2:4)) >>> 0;
        return addr;
      }
      case 4: {                                                       // -(An)
        this.a[reg] = (this.a[reg] - (sz===0 ? (reg===7?2:1) : sz===1?2:4)) >>> 0;
        return this.a[reg];
      }
      case 5: {                                                       // d16(An)
        const d = this.sext16(this.fetch16());
        return (this.a[reg] + d) >>> 0;
      }
      case 6: {                                                       // d8(An,Xn)
        const ext = this.fetch16();
        const xi  = (ext >> 12) & 7;
        const xv  = (ext & 0x8000) ? this.a[xi] : this.d[xi];
        const x   = (ext & 0x0800) ? (xv | 0) : this.sext16(xv & 0xFFFF);
        return (this.a[reg] + x + this.sext8(ext & 0xFF)) >>> 0;
      }
      case 7: switch (reg) {
        case 0: { const a = this.sext16(this.fetch16()); return a >>> 0; }  // xxx.W
        case 1: return this.fetch32();                                        // xxx.L
        case 2: { const b=this.pc; const d=this.sext16(this.fetch16()); return (b+d)>>>0; } // d16(PC)
        case 3: {                                                              // d8(PC,Xn)
          const b=this.pc; const ext=this.fetch16();
          const xi=(ext>>12)&7;
          const xv=(ext&0x8000)?this.a[xi]:this.d[xi];
          const x=(ext&0x0800)?(xv|0):this.sext16(xv&0xFFFF);
          return (b + x + this.sext8(ext&0xFF)) >>> 0;
        }
      }
    }
    return 0;
  }

  readEA(mode, reg, sz) {
    if (mode === 0) return this.readDn(reg, sz);
    if (mode === 1) { // An — sign extended
      return sz === 1 ? this.sext16(this.a[reg] & 0xFFFF) >>> 0 : this.a[reg] >>> 0;
    }
    if (mode === 7 && reg === 4) {   // #imm
      if (sz === 0) { const v=this.fetch16()&0xFF; return v; }
      if (sz === 1) return this.fetch16();
      return this.fetch32();
    }
    return this.bus.readSize(this.calcEA(mode, reg, sz), sz);
  }

  writeEA(mode, reg, val, sz) {
    if (mode === 0) { this.writeDn(reg, val, sz); return; }
    if (mode === 1) { // MOVEA etc — sign extend to 32
      this.a[reg] = sz===1 ? this.sext16(val&0xFFFF)>>>0 : val>>>0; return;
    }
    this.bus.writeSize(this.calcEA(mode, reg, sz), val, sz);
  }

  // Get current EA address without advancing PC (for address-only ops)
  // Must be called AFTER readEA or manually after any extension words
  getEAAddr(mode, reg, sz) {
    if (mode < 2) return 0; // register — no address
    return this.calcEA(mode, reg, sz);
  }

  // ── Flag helpers ─────────────────────────────────────────────
  _masks(sz) {
    return sz===0 ? [0xFF,0x80] : sz===1 ? [0xFFFF,0x8000] : [0xFFFFFFFF,0x80000000];
  }

  setNZ(r, sz) {
    const [mask, msb] = this._masks(sz);
    let sr = this.sr & ~0x0C;
    if ((r & mask) === 0)  sr |= 0x04;
    if (r & msb)           sr |= 0x08;
    this.sr = sr;
  }

  setNZVC(r, sz) {
    const [mask, msb] = this._masks(sz);
    let sr = this.sr & ~0x0F;
    if ((r & mask) === 0)  sr |= 0x04;
    if (r & msb)           sr |= 0x08;
    this.sr = sr; // V and C cleared
  }

  doAdd(src, dst, sz, withX) {
    const [mask, msb] = this._masks(sz);
    const x = withX ? ((this.sr >> 4) & 1) : 0;
    const s = src & mask, d = dst & mask;
    const sum = s + d + x;
    const r = sum & mask;
    const c = sum > mask;
    const v = !!(~(s ^ d) & (s ^ r) & msb);
    let sr = this.sr & ~0x1F;
    if (c) sr |= 0x11;  // C and X
    if (v) sr |= 0x02;
    if (withX) { sr |= (r!==0) ? (this.sr&0x04)&~0x04 : (this.sr&0x04)|0x04; /* preserve Z if 0 */ }
    else if (r===0) sr |= 0x04;
    if (r & msb) sr |= 0x08;
    this.sr = sr;
    return r;
  }

  doSub(src, dst, sz, withX) {
    const [mask, msb] = this._masks(sz);
    const x = withX ? ((this.sr >> 4) & 1) : 0;
    const s = src & mask, d = dst & mask;
    const diff = d - s - x;
    const r = diff & mask;
    const borrow = diff < 0;
    const v = !!((s ^ d) & (d ^ r) & msb);
    let sr = this.sr & ~0x1F;
    if (borrow) sr |= 0x11; // C (borrow) and X
    if (v)      sr |= 0x02;
    if (withX) { sr |= (r!==0) ? (this.sr&0x04)&~0x04 : (this.sr&0x04)|0x04; }
    else if (r===0) sr |= 0x04;
    if (r & msb) sr |= 0x08;
    this.sr = sr;
    return r;
  }

  doCmp(src, dst, sz) {
    const [mask, msb] = this._masks(sz);
    const s = src & mask, d = dst & mask;
    const diff = d - s;
    const r = diff & mask;
    const borrow = diff < 0;
    const v = !!((s ^ d) & (d ^ r) & msb);
    let sr = this.sr & ~0x0F; // CMP does not update X
    if (borrow) sr |= 0x01;
    if (v)      sr |= 0x02;
    if (r===0)  sr |= 0x04;
    if (r & msb) sr |= 0x08;
    this.sr = sr;
  }

  testCC(cc) {
    const c = (this.sr & 1)!==0, v = (this.sr & 2)!==0,
          z = (this.sr & 4)!==0, n = (this.sr & 8)!==0;
    switch (cc & 0xF) {
      case  0: return true;
      case  1: return false;
      case  2: return !c && !z;
      case  3: return c || z;
      case  4: return !c;
      case  5: return c;
      case  6: return !z;
      case  7: return z;
      case  8: return !v;
      case  9: return v;
      case 10: return !n;
      case 11: return n;
      case 12: return n === v;
      case 13: return n !== v;
      case 14: return !z && (n === v);
      case 15: return z  || (n !== v);
    }
    return false;
  }

  // ── Exception / Interrupt ────────────────────────────────────
  exception(vector) {
    // Push PC then SR to SSP
    this.a[7] = (this.a[7] - 4) >>> 0;
    this.bus.write32(this.a[7], this.pc);
    this.a[7] = (this.a[7] - 2) >>> 0;
    this.bus.write16(this.a[7], this.sr & 0xFFFF);
    this.sr = (this.sr | 0x2000) & ~0x8000; // enter supervisor, clear trace
if (this.bus._logEvent) this.bus._logEvent('irq', 
    `EXCEPTION vec=${vector} PC=0x${((this.pc-2)>>>0).toString(16).toUpperCase()} op=0x${this.bus.read16((this.pc-2)>>>0).toString(16).toUpperCase()}`);  
   this.pc = this.bus.read32((vector * 4) >>> 0) >>> 0;
    this.cycles += 34;
  }

  // Returns true if interrupt was accepted
  interrupt(level) {
    const ipl = (this.sr >> 8) & 7;
    if (level <= ipl && level !== 7) return false;
    this.stopped = false;
    // Save SR and PC, set new IPL
    const saveSR = this.sr;
    this.sr = (this.sr & ~0x0700) | (level << 8) | 0x2000;
    this.sr &= ~0x8000;
    this.a[7] = (this.a[7] - 4) >>> 0;
    this.bus.write32(this.a[7], this.pc);
    this.a[7] = (this.a[7] - 2) >>> 0;
    this.bus.write16(this.a[7], saveSR & 0xFFFF);
    // Auto-vector: vector = 24 + level
    this.pc = this.bus.read32(((24 + level) * 4) >>> 0) >>> 0;
    this.cycles += 44;
    return true;
  }

  // ── Main decode/execute ──────────────────────────────────────
  step() {
    if (this.stopped) { this.cycles += 4; return; }
    const op = this.fetch16();
    const g  = (op >> 12) & 0xF;
    switch (g) {
      case 0x0: this._g0(op); break;
      case 0x1: this._gMOVE(op, 0); break;  // MOVE.B
      case 0x2: this._gMOVE(op, 2); break;  // MOVE.L
      case 0x3: this._gMOVE(op, 1); break;  // MOVE.W
      case 0x4: this._g4(op); break;
      case 0x5: this._g5(op); break;
      case 0x6: this._g6(op); break;
      case 0x7: this._g7(op); break;
      case 0x8: this._g8(op); break;
      case 0x9: this._g9(op); break;
      case 0xA: this.exception(10); break;  // A-line
      case 0xB: this._gB(op); break;
      case 0xC: this._gC(op); break;
      case 0xD: this._gD(op); break;
      case 0xE: this._gE(op); break;
      case 0xF: this.exception(11); break;  // F-line
    }
  }

  // ── Group 0 : Bit / Immediate / MOVEP ────────────────────────
  _g0(op) {
    const srcMode = (op >> 3) & 7, srcReg = op & 7;
    const dstReg  = (op >> 9) & 7;
    const b11_8   = (op >> 8) & 0xF;

    // MOVEP: 0000 DDD1 zz 001 AAA
    if ((op & 0x0138) === 0x0108) {
      const dir  = (op >> 7) & 1;   // 0 = mem->reg, 1 = reg->mem
      const long = (op >> 6) & 1;   // 0 = word, 1 = long
      const disp = this.sext16(this.fetch16());
      const base = (this.a[srcReg] + disp) >>> 0;
      if (!dir) { // memory → Dn
        if (long) {
          this.d[dstReg] = (this.bus.read8(base)<<24)|(this.bus.read8(base+2)<<16)|(this.bus.read8(base+4)<<8)|this.bus.read8(base+6);
        } else {
          this.writeDn(dstReg, (this.bus.read8(base)<<8)|this.bus.read8(base+2), 1);
        }
      } else { // Dn → memory
        if (long) {
          const v = this.d[dstReg] >>> 0;
          this.bus.write8(base,   (v>>24)&0xFF); this.bus.write8(base+2, (v>>16)&0xFF);
          this.bus.write8(base+4, (v>> 8)&0xFF); this.bus.write8(base+6,  v&0xFF);
        } else {
          const v = this.d[dstReg] & 0xFFFF;
          this.bus.write8(base, (v>>8)&0xFF); this.bus.write8(base+2, v&0xFF);
        }
      }
      this.cycles += long ? 24 : 16; return;
    }

    // Dynamic bit ops: 0000 DDD1 tt MMMMMM (bit 8 = 1, not MOVEP)
    if ((op & 0x0100) && (b11_8 & 1)) {
      const typ = (op >> 6) & 3;
      const bitVal = this.readEA(srcMode, srcReg, 0);
      const num = this.d[dstReg] & (srcMode===0 ? 31 : 7);
      this._doBitOp(typ, num, srcMode, srcReg, bitVal); this.cycles += 6; return;
    }

    // Static bit ops: 0000 1000 tt MMMMMM
    if ((b11_8 & 0xC) === 0x8 && !(b11_8 & 3)) {
      // This is BTST static etc - but b11_8 could be 8,9,A,B,C...
    }

    // Decode by bits 11-8 for immediate ops
    const sz = (op >> 6) & 3;
    if (sz === 3) {
      // Special cases with sz=11
      this._g0Special(op, b11_8, srcMode, srcReg, dstReg); return;
    }

    switch (b11_8) {
      case 0x0: { // ORI
        const imm = sz===0 ? this.fetch16()&0xFF : sz===1 ? this.fetch16() : this.fetch32();
        const v = this.readEA(srcMode, srcReg, sz) | imm;
        this.writeEA(srcMode, srcReg, v, sz); this.setNZVC(v, sz); this.cycles += 8; break;
      }
      case 0x2: { // ANDI
        const imm = sz===0 ? this.fetch16()&0xFF : sz===1 ? this.fetch16() : this.fetch32();
        const v = this.readEA(srcMode, srcReg, sz) & imm;
        this.writeEA(srcMode, srcReg, v, sz); this.setNZVC(v, sz); this.cycles += 8; break;
      }
      case 0x4: { // SUBI
        const imm = sz===0 ? this.fetch16()&0xFF : sz===1 ? this.fetch16() : this.fetch32();
        const r = this.doSub(imm, this.readEA(srcMode, srcReg, sz), sz, false);
        this.writeEA(srcMode, srcReg, r, sz); this.cycles += 8; break;
      }
      case 0x6: { // ADDI
        const imm = sz===0 ? this.fetch16()&0xFF : sz===1 ? this.fetch16() : this.fetch32();
        const r = this.doAdd(imm, this.readEA(srcMode, srcReg, sz), sz, false);
        this.writeEA(srcMode, srcReg, r, sz); this.cycles += 8; break;
      }
      case 0x8: { // BTST/BCHG/BCLR/BSET (static)
        const num = this.fetch16() & (srcMode===0 ? 31 : 7);
        const typ = (op >> 6) & 3;
        const v = this.readEA(srcMode, srcReg, 0);
        this._doBitOp(typ, num, srcMode, srcReg, v); this.cycles += 8; break;
      }
      case 0xA: { // EORI
        const imm = sz===0 ? this.fetch16()&0xFF : sz===1 ? this.fetch16() : this.fetch32();
        const v = this.readEA(srcMode, srcReg, sz) ^ imm;
        this.writeEA(srcMode, srcReg, v, sz); this.setNZVC(v, sz); this.cycles += 8; break;
      }
      case 0xC: { // CMPI
        const imm = sz===0 ? this.fetch16()&0xFF : sz===1 ? this.fetch16() : this.fetch32();
        this.doCmp(imm, this.readEA(srcMode, srcReg, sz), sz); this.cycles += 8; break;
      }
      default: // Dynamic bit ops: 0000 DDD1 tt MMMMMM
        if (op & 0x0100) {
          const typ = (op >> 6) & 3;
          const num = this.d[dstReg] & (srcMode===0 ? 31 : 7);
          const v = this.readEA(srcMode, srcReg, 0);
          this._doBitOp(typ, num, srcMode, srcReg, v); this.cycles += 6;
        }
    }
  }

  _g0Special(op, b11_8, srcMode, srcReg, dstReg) {
    // sz==3 sub-cases
    switch (b11_8) {
      case 0x0: { // ORI #imm, SR/CCR
        const imm = this.fetch16();
        if (op & 0x40) { this.sr |= imm & 0x1F; } // ORI to CCR
        else           { this.sr |= imm & 0xA71F; } // ORI to SR
        this.cycles += 20; break;
      }
      case 0x2: { // ANDI #imm, SR/CCR
        const imm = this.fetch16();
        if (op & 0x40) { this.sr = (this.sr & ~0x1F) | (imm & 0x1F); }
        else           { this.sr &= imm & 0xA71F; }
        this.cycles += 20; break;
      }
      case 0xA: { // EORI #imm, SR/CCR
        const imm = this.fetch16();
        if (op & 0x40) { this.sr ^= imm & 0x1F; }
        else           { this.sr ^= imm & 0xA71F; }
        this.cycles += 20; break;
      }
    }
  }

  _doBitOp(typ, num, mode, reg, v) {
    const mask = 1 << num;
    const bitSet = (v & mask) !== 0;
    // Set Z flag based on tested bit
    this.sr = (this.sr & ~0x04) | (bitSet ? 0 : 0x04);
    if (typ === 0) return; // BTST — test only
    let newV;
    if (typ === 1) newV = v ^ mask;  // BCHG
    if (typ === 2) newV = v & ~mask; // BCLR
    if (typ === 3) newV = v |  mask; // BSET
    this.writeEA(mode, reg, newV, 0);
  }

  // ── Group MOVE ────────────────────────────────────────────────
  _gMOVE(op, sz) {
    const srcMode = (op >> 3) & 7, srcReg = op & 7;
    const dstReg  = (op >> 9) & 7, dstMode = (op >> 6) & 7;
    const val = this.readEA(srcMode, srcReg, sz);

    if (dstMode === 1) { // MOVEA — no flags, sign extend
      const ext = sz === 1 ? this.sext16(val & 0xFFFF) : (val | 0);
      this.a[dstReg] = ext >>> 0;
    } else {
      this.writeEA(dstMode, dstReg, val, sz);
      this.setNZVC(val, sz);
    }
    this.cycles += 4;
  }

  // ── Group 4 : Misc ────────────────────────────────────────────
  _g4(op) {
    const b11_8  = (op >> 8) & 0xF;
    const sz     = (op >> 6) & 3;
    const mode   = (op >> 3) & 7;
    const reg    = op & 7;
    const dstReg = (op >> 9) & 7;

    switch (b11_8) {
      case 0x0: { // NEGX or MOVE-from-SR
        if (sz === 3) { // MOVE SR → EA
          const v = this.sr & 0xFFFF;
          this.writeEA(mode, reg, v, 1); this.cycles += 6; return;
        }
        const v = this.readEA(mode, reg, sz);
        const r = this.doSub(v, 0, sz, true); // NEGX = 0 - src - X
        this.writeEA(mode, reg, r, sz); this.cycles += 6; return;
      }
      case 0x2: { // CLR
        if (sz === 3) { // MOVE #imm → CCR
          const imm = this.fetch16(); this.sr = (this.sr & ~0x1F) | (imm & 0x1F);
          this.cycles += 20; return;
        }
        // CLR reads EA (for side effects) then writes zero
        this.readEA(mode, reg, sz);
        this.writeEA(mode, reg, 0, sz);
        this.sr = (this.sr & ~0x0F) | 0x04; // Z=1, N=V=C=0
        this.cycles += 4; return;
      }
      case 0x4: { // NEG or MOVE #imm → SR
        if (sz === 3) { // MOVE #imm → SR (privileged)
          const imm = this.fetch16(); this.sr = imm & 0xA71F;
          this.cycles += 20; return;
        }
        const v = this.readEA(mode, reg, sz);
        const r = this.doSub(v, 0, sz, false);
        this.writeEA(mode, reg, r, sz); this.cycles += 6; return;
      }
      case 0x6: { // NOT or MOVE #imm → SR (0100 0110 11)
        if (sz === 3) { // MOVE #imm → SR
          const imm = this.fetch16(); this.sr = imm & 0xA71F;
          this.cycles += 20; return;
        }
        const v = (~this.readEA(mode, reg, sz));
        this.writeEA(mode, reg, v, sz); this.setNZVC(v, sz); this.cycles += 4; return;
      }
      case 0x8: { // NBCD / PEA / SWAP / MOVEM
        if (sz === 0) { // NBCD
          // BCD negate — simplified, not fully accurate for all edge cases
          const v = this.readEA(mode, reg, 0);
          const x = (this.sr >> 4) & 1;
          let r = (0x100 - v - x) & 0xFF;
          let c = (v | x) !== 0;
          if ((r & 0xF) === 0 && (v & 0xF) === 0 && !x) { r = 0; c = false; }
          this.writeEA(mode, reg, r, 0);
          let sr = this.sr & ~0x15;
          if (c) sr |= 0x11;
          if (r === 0) sr |= 0x04;
          this.sr = sr;
          this.cycles += 6; return;
        }
        if (sz === 1) { // PEA or SWAP
          if (mode === 0) { // SWAP Dn
            const v = this.d[reg];
            this.d[reg] = ((v & 0xFFFF) << 16) | ((v >> 16) & 0xFFFF);
            this.setNZVC(this.d[reg], 2); this.cycles += 4; return;
          }
          // PEA
          const addr = this.calcEA(mode, reg, 2);
          this.a[7] = (this.a[7] - 4) >>> 0;
          this.bus.write32(this.a[7], addr);
          this.cycles += 12; return;
        }
        if (sz === 2) { // EXT.W Dn (and MOVEM word reg→mem)
          if (mode === 0) { // EXT.W
            this.d[reg] = (this.d[reg] & 0xFFFF0000) | (this.sext8(this.d[reg] & 0xFF) & 0xFFFF);
            this.setNZVC(this.d[reg] & 0xFFFF, 1); this.cycles += 4; return;
          }
          // MOVEM.W registers → memory
          this._movemToMem(op, mode, reg, 1); return;
        }
if (sz === 3) { // EXT.L Dn or MOVEM.L reg→mem
          if (mode === 0) { // EXT.L
            this.d[reg] = this.sext16(this.d[reg] & 0xFFFF) >>> 0;
            this.setNZVC(this.d[reg], 2); this.cycles += 4; return;
          }
          // MOVEM.L registers → memory
          this._movemToMem(op, mode, reg, 2); return;
        }
        break;
      }
      case 0xA: { // TST / ILLEGAL
        if (sz === 3) {
          // ILLEGAL: 0100 1010 1111 1100
          if ((op & 0xFF) === 0xFC) { this.exception(4); return; }
          // TAS — test and set (rarely used on Genesis, stub as TST)
        }
        const v = this.readEA(mode, reg, sz);
        this.setNZVC(v, sz); this.cycles += 4; return;
      }
      case 0xC: { // MOVEM long
        const sz2 = (op >> 6) & 1; // 0=word, 1=long
        if ((op >> 7) & 1) { // mem → registers
          this._movemFromMem(op, mode, reg, sz2 ? 2 : 1); return;
        } else {             // registers → mem
          this._movemToMem(op, mode, reg, sz2 ? 2 : 1); return;
        }
      }
      case 0xE: { // Misc: TRAP / LINK / UNLK / MOVE USP / RTE / RTS / etc.
        this._g4E(op, mode, reg, sz); return;
      }
      default: { // CHK / LEA
        if ((op & 0x01C0) === 0x0180) { // CHK
          const [mask, msb] = this._masks(1);
          const dn = this.sext16(this.d[dstReg] & 0xFFFF);
          const ub = this.sext16(this.readEA(mode, reg, 1));
          if (dn < 0) {
            this.sr |= 0x08; // N=1
            this.exception(6);
          } else if (dn > ub) {
            this.sr &= ~0x08; // N=0
            this.exception(6);
          }
          this.cycles += 10; return;
        }
        if ((op & 0x01C0) === 0x01C0) { // LEA
          this.a[dstReg] = this.calcEA(mode, reg, 2);
          this.cycles += 4; return;
        }
      }
    }
  }

  _g4E(op, mode, reg, sz) {
    const lo8 = op & 0xFF;
    if ((op & 0xF0) === 0x40) { // TRAP #n
      this.exception(32 + (op & 0xF)); this.cycles += 34; return;
    }
    if ((op & 0xF8) === 0x50) { // LINK An, #disp
      const disp = this.sext16(this.fetch16());
      this.a[7] = (this.a[7] - 4) >>> 0;
      this.bus.write32(this.a[7], this.a[reg]);
      this.a[reg] = this.a[7];
      this.a[7]   = (this.a[7] + disp) >>> 0;
      this.cycles += 16; return;
    }
    if ((op & 0xF8) === 0x58) { // UNLK An
      this.a[7]   = this.a[reg] >>> 0;
      this.a[reg] = this.bus.read32(this.a[7]);
      this.a[7]   = (this.a[7] + 4) >>> 0;
      this.cycles += 12; return;
    }
    if ((op & 0xF0) === 0x60) { // MOVE USP
      const dir = (op >> 3) & 1;
      if (dir) this.a[reg] = this._usp; // USP → An
      else     this._usp = this.a[reg]; // An → USP
      this.cycles += 4; return;
    }
switch (lo8) {
      case 0x70: this.cycles += 132; return;  // RESET — pulses bus reset, CPU continues
      case 0x71: this.cycles += 4; return;    // NOP (0x4E71)
      case 0x72: {                            // STOP #imm — load SR, halt until interrupt
        const imm = this.fetch16();
        this.sr = imm & 0xA71F;
        this.stopped = true;
        this.cycles += 4;
        return;
      }
      case 0x73: { // RTE
        this.sr = this.bus.read16(this.a[7]) & 0xA71F;
        this.a[7] = (this.a[7] + 2) >>> 0;
        this.pc   = this.bus.read32(this.a[7]);
        this.a[7] = (this.a[7] + 4) >>> 0;
        this.cycles += 20; return;
      }
      case 0x75: { // RTS
        this.pc   = this.bus.read32(this.a[7]);
        this.a[7] = (this.a[7] + 4) >>> 0;
        this.cycles += 16; return;
      }
      case 0x76: { // TRAPV
        if ((this.sr & 2)) this.exception(7);
        this.cycles += 4; return;
      }
      case 0x77: { // RTR
        const ccr = this.bus.read16(this.a[7]) & 0x1F;
        this.a[7] = (this.a[7] + 2) >>> 0;
        this.sr   = (this.sr & ~0x1F) | ccr;
        this.pc   = this.bus.read32(this.a[7]);
        this.a[7] = (this.a[7] + 4) >>> 0;
        this.cycles += 20; return;
      }
    }
    // JMP / JSR
    if ((op & 0xC0) === 0x80) { // JSR
      const addr = this.calcEA(mode, reg, 2);
      this.a[7] = (this.a[7] - 4) >>> 0;
      this.bus.write32(this.a[7], this.pc);
      this.pc = addr; this.cycles += 16; return;
    }
    if ((op & 0xC0) === 0xC0) { // JMP
      this.pc = this.calcEA(mode, reg, 2); this.cycles += 8; return;
    }
    // NOP
    if (lo8 === 0x71) { this.cycles += 4; return; }
    this.cycles += 4;
  }

  _movemToMem(op, mode, reg, sz) {
    const list = this.fetch16();
    let addr;
    const preDec = (mode === 4);
    if (preDec) {
      // Predecrement: order is A7..A0, D7..D0 (list is reversed)
      // Calculate total size first
      const bytes = (sz === 2 ? 4 : 2) * _bitCount(list);
      addr = (this.a[reg] - bytes) >>> 0;
      this.a[reg] = addr;
      let a = addr;
      for (let i = 0; i < 16; i++) {
        const bit = 15 - i; // reversed
        if (!(list & (1 << i))) continue;
        const r = bit >= 8 ? this.a[bit-8] : this.d[bit];
        if (sz === 2) { this.bus.write32(a, r >>> 0); a += 4; }
        else          { this.bus.write16(a, r & 0xFFFF); a += 2; }
      }
    } else {
      addr = this.calcEA(mode, reg, sz);
      let a = addr;
      for (let i = 0; i < 16; i++) {
        if (!(list & (1 << i))) continue;
        const r = i >= 8 ? this.a[i-8] : this.d[i];
        if (sz === 2) { this.bus.write32(a, r >>> 0); a += 4; }
        else          { this.bus.write16(a, r & 0xFFFF); a += 2; }
      }
    }
    this.cycles += 8 + _bitCount(list) * (sz===2 ? 8 : 4);
  }

  _movemFromMem(op, mode, reg, sz) {
    const list = this.fetch16();
    let addr = this.calcEA(mode, reg, sz);
    for (let i = 0; i < 16; i++) {
      if (!(list & (1 << i))) continue;
      if (i < 8) {
        if (sz === 2) this.d[i] = this.bus.read32(addr) >>> 0;
        else          this.d[i] = (this.d[i]&0xFFFF0000) | this.bus.read16(addr);
        addr += sz===2 ? 4 : 2;
      } else {
        if (sz === 2) this.a[i-8] = this.bus.read32(addr) >>> 0;
        else          this.a[i-8] = this.sext16(this.bus.read16(addr)) >>> 0;
        addr += sz===2 ? 4 : 2;
      }
    }
    if (mode === 3) this.a[reg] = addr; // postincrement update
    this.cycles += 12 + _bitCount(list) * (sz===2 ? 8 : 4);
  }

  // ── Group 5 : ADDQ / SUBQ / Scc / DBcc ────────────────────────
  _g5(op) {
    const sz   = (op >> 6) & 3;
    const mode = (op >> 3) & 7;
    const reg  = op & 7;
    let imm = (op >> 9) & 7; if (imm === 0) imm = 8;

    if (sz === 3) { // Scc / DBcc
      if (mode === 1) { // DBcc
        const cc  = (op >> 8) & 0xF;
        const disp = this.sext16(this.fetch16());
        if (!this.testCC(cc)) {
          const cnt = (this.d[reg] & 0xFFFF) - 1;
          this.writeDn(reg, cnt, 1);
          if ((cnt & 0xFFFF) !== 0xFFFF) {
            this.pc = (this.pc - 2 + disp) >>> 0; this.cycles += 10;
          } else {
            this.cycles += 14;
          }
        } else { this.cycles += 12; }
        return;
      }
      // Scc
      const cc = (op >> 8) & 0xF;
      const v  = this.testCC(cc) ? 0xFF : 0x00;
      this.writeEA(mode, reg, v, 0); this.cycles += 4; return;
    }

    if ((op >> 8) & 1) { // SUBQ
      const v  = this.readEA(mode, reg, sz);
      const r  = mode === 1 ? ((this.a[reg] - imm) >>> 0) : this.doSub(imm, v, sz, false);
      if (mode === 1) this.a[reg] = r;
      else this.writeEA(mode, reg, r, sz);
      this.cycles += 4;
    } else { // ADDQ
      const v  = this.readEA(mode, reg, sz);
      const r  = mode === 1 ? ((this.a[reg] + imm) >>> 0) : this.doAdd(imm, v, sz, false);
      if (mode === 1) this.a[reg] = r;
      else this.writeEA(mode, reg, r, sz);
      this.cycles += 4;
    }
  }

// ── Group 6 : BRA / BSR / Bcc ─────────────────────────────────
  _g6(op) {
    const cc       = (op >> 8) & 0xF;
    const byteField = op & 0xFF;
    let   disp     = this.sext8(byteField);
    if (byteField === 0) { disp = this.sext16(this.fetch16()); }

    // After fetching all extension words, this.pc == return address.
    // M68K branch target = (opcode_addr + 2) + disp.
    // For byte disp: this.pc = opcode_addr + 2, so target = this.pc + disp.
    // For word disp: this.pc = opcode_addr + 4, so target = this.pc - 2 + disp.
    const adj    = byteField === 0 ? -2 : 0;
    const target = (this.pc + adj + disp) >>> 0;

    if (cc === 1) { // BSR — push return address, jump to target
      this.a[7] = (this.a[7] - 4) >>> 0;
      this.bus.write32(this.a[7], this.pc); // return addr = pc past extension words
      this.pc = target;
      this.cycles += 18; return;
    }

    if (cc === 0 || this.testCC(cc)) { // BRA or taken Bcc
      this.pc = target;
      this.cycles += 10;
    } else {
      this.cycles += 8;
    }
  }

  // ── Group 7 : MOVEQ ────────────────────────────────────────────
  _g7(op) {
    const reg = (op >> 9) & 7;
    const v   = this.sext8(op & 0xFF);
    this.d[reg] = v >>> 0;
    this.setNZVC(v, 2); this.cycles += 4;
  }

  // ── Group 8 : OR / DIVU / DIVS / SBCD ─────────────────────────
  _g8(op) {
    const dn   = (op >> 9) & 7;
    const sz   = (op >> 6) & 3;
    const mode = (op >> 3) & 7;
    const reg  = op & 7;

    if (sz === 3) { // DIVU / DIVS
      const signed = (op >> 8) & 1;
      const divisor = this.readEA(mode, reg, 1);
      if (divisor === 0) { this.exception(5); return; }
      const dvnd = this.d[dn] >>> 0;
      let q, r;
      if (signed) {
        const sd = this.sext16(divisor), sv = dvnd | 0;
        q = Math.trunc(sv / sd); r = sv % sd;
        if (q < -32768 || q > 32767) { this.sr = (this.sr & ~0x0F)|0x02; this.cycles+=140; return; }
      } else {
        q = Math.trunc(dvnd / divisor); r = dvnd % divisor;
        if (q > 65535) { this.sr = (this.sr & ~0x0F)|0x02; this.cycles+=140; return; }
      }
      this.d[dn] = ((r & 0xFFFF) << 16) | (q & 0xFFFF);
      let sr = this.sr & ~0x0F;
      if ((q & 0xFFFF) === 0) sr |= 0x04;
      if (q & 0x8000) sr |= 0x08;
      this.sr = sr; this.cycles += 140; return;
    }

    // SBCD
    if (sz === 0 && (op & 0x1F0) === 0x100) {
      const rmMode = (op >> 3) & 1; // 0=Dn, 1=-(An)
      let src, dst;
      if (rmMode) {
        this.a[reg] = (this.a[reg]-1)>>>0; src = this.bus.read8(this.a[reg]);
        this.a[dn]  = (this.a[dn]-1) >>>0; dst = this.bus.read8(this.a[dn]);
      } else { src=this.d[reg]&0xFF; dst=this.d[dn]&0xFF; }
      const x = (this.sr>>4)&1;
      let lo = (dst&0xF) - (src&0xF) - x;
      let hi = ((dst>>4)&0xF) - ((src>>4)&0xF);
      if (lo < 0) { lo += 10; hi--; }
      if (hi < 0) { hi += 10; this.sr |= 0x11; } else { this.sr &= ~0x11; }
      const r = (hi<<4)|(lo&0xF);
      if (rmMode) this.bus.write8(this.a[dn], r);
      else        this.writeDn(dn, r, 0);
      if (r !== 0) this.sr &= ~0x04;
      this.cycles += 6; return;
    }

    // OR
    const v = this.readEA(mode, reg, sz);
    if ((op & 0x100) && mode > 1) { // OR Dn → EA
      const dst = this.readEA(mode, reg, sz);
      const r = dst | this.readDn(dn, sz);
      this.writeEA(mode, reg, r, sz); this.setNZVC(r, sz);
    } else { // OR EA → Dn
      const r = this.readDn(dn, sz) | v;
      this.writeDn(dn, r, sz); this.setNZVC(r, sz);
    }
    this.cycles += 4;
  }

  // ── Group 9 : SUB / SUBA / SUBX ───────────────────────────────
  _g9(op) {
    const dn   = (op >> 9) & 7;
    const sz   = (op >> 6) & 3;
    const mode = (op >> 3) & 7;
    const reg  = op & 7;
    const dir  = (op >> 8) & 1;

    if (sz === 3) { // SUBA
      const opSz = dir ? 2 : 1;
      const src  = this.readEA(mode, reg, opSz);
      const ext  = opSz === 1 ? this.sext16(src) : (src | 0);
      this.a[dn] = (this.a[dn] - ext) >>> 0; this.cycles += 8; return;
    }

    // SUBX: 1001 DDD1 ss 00 mDDD
    if (dir && (mode === 0 || mode === 4)) {
      const rmMode = (mode === 4);
      let src, dst;
      if (rmMode) {
        this.a[reg] = (this.a[reg] - (sz===0?1:sz===1?2:4)) >>> 0; src=this.bus.readSize(this.a[reg],sz);
        this.a[dn]  = (this.a[dn]  - (sz===0?1:sz===1?2:4)) >>> 0; dst=this.bus.readSize(this.a[dn],sz);
      } else { src=this.readDn(reg,sz); dst=this.readDn(dn,sz); }
      const r = this.doSub(src, dst, sz, true);
      if (rmMode) this.bus.writeSize(this.a[dn], r, sz);
      else        this.writeDn(dn, r, sz);
      this.cycles += 4; return;
    }

    if (dir) { // SUB EA ← Dn
      const dst = this.readEA(mode, reg, sz);
      const r   = this.doSub(this.readDn(dn, sz), dst, sz, false);
      this.writeEA(mode, reg, r, sz);
    } else {   // SUB Dn ← EA
      const r = this.doSub(this.readEA(mode, reg, sz), this.readDn(dn, sz), sz, false);
      this.writeDn(dn, r, sz);
    }
    this.cycles += 4;
  }

  // ── Group B : CMP / CMPA / CMPM / EOR ─────────────────────────
  _gB(op) {
    const dn   = (op >> 9) & 7;
    const sz   = (op >> 6) & 3;
    const mode = (op >> 3) & 7;
    const reg  = op & 7;
    const dir  = (op >> 8) & 1;

    if (sz === 3) { // CMPA
      const opSz = dir ? 2 : 1;
      const src  = this.readEA(mode, reg, opSz);
      const ext  = opSz===1 ? this.sext16(src&0xFFFF) : (src|0);
      this.doCmp(ext >>> 0, this.a[dn], 2); this.cycles += 6; return;
    }

    if (dir) { // CMPM or EOR
      if (mode === 1) { // CMPM (An)+ , (An)+
        const src = this.readEA(3, reg, sz); // postincrement reg
        const dst = this.readEA(3, dn,  sz); // postincrement dn (An)
        this.doCmp(src, dst, sz); this.cycles += 12; return;
      }
      // EOR Dn → EA
      const v = this.readEA(mode, reg, sz) ^ this.readDn(dn, sz);
      this.writeEA(mode, reg, v, sz); this.setNZVC(v, sz); this.cycles += 4; return;
    }

    // CMP EA → Dn
    this.doCmp(this.readEA(mode, reg, sz), this.readDn(dn, sz), sz); this.cycles += 4;
  }

  // ── Group C : AND / MULU / MULS / ABCD / EXG ──────────────────
  _gC(op) {
    const dn   = (op >> 9) & 7;
    const sz   = (op >> 6) & 3;
    const mode = (op >> 3) & 7;
    const reg  = op & 7;
    const dir  = (op >> 8) & 1;

    if (sz === 3) { // MULU / MULS
      const signed = dir;
      const src = this.readEA(mode, reg, 1) & 0xFFFF;
      const dst = this.d[dn] & 0xFFFF;
      let r;
      if (signed) {
        r = (this.sext16(src) * this.sext16(dst)) | 0;
      } else {
        r = (src * dst) >>> 0;
      }
      this.d[dn] = r >>> 0;
      let sr = this.sr & ~0x0F;
      if (r === 0) sr |= 0x04;
      if (r & 0x80000000) sr |= 0x08;
      this.sr = sr; this.cycles += 70; return;
    }

    // ABCD: 1100 DDD1 0000 mDDD
    if (dir && sz === 0 && mode <= 1) {
      const rmMode = (mode === 1);
      let src, dst;
      if (rmMode) {
        this.a[reg]=(this.a[reg]-1)>>>0; src=this.bus.read8(this.a[reg]);
        this.a[dn] =(this.a[dn]-1) >>>0; dst=this.bus.read8(this.a[dn]);
      } else { src=this.d[reg]&0xFF; dst=this.d[dn]&0xFF; }
      const x=(this.sr>>4)&1;
      let lo=(dst&0xF)+(src&0xF)+x;
      let hi=((dst>>4)&0xF)+((src>>4)&0xF);
      if (lo > 9) { lo -= 10; hi++; }
      if (hi > 9) { hi -= 10; this.sr|=0x11; } else { this.sr&=~0x11; }
      const r=(hi<<4)|(lo&0xF);
      if (rmMode) this.bus.write8(this.a[dn],r);
      else        this.writeDn(dn,r,0);
      if (r!==0) this.sr&=~0x04;
      this.cycles+=6; return;
    }

    // EXG: 1100 DDD1 1z 000 DDD
    if (dir && (sz===1||sz===2)) {
      if ((op & 0xF8) === 0x40) { // Dn ↔ Dn
        const t=this.d[dn]; this.d[dn]=this.d[reg]; this.d[reg]=t;
      } else if ((op & 0xF8) === 0x48) { // An ↔ An
        const t=this.a[dn]; this.a[dn]=this.a[reg]; this.a[reg]=t;
      } else { // Dn ↔ An (1100 DDD1 10001 DDD)
        const t=this.d[dn]; this.d[dn]=this.a[reg]; this.a[reg]=t>>>0;
      }
      this.cycles+=6; return;
    }

    // AND
    if (dir && mode > 1) { // AND Dn → EA
      const r = this.readEA(mode,reg,sz) & this.readDn(dn,sz);
      this.writeEA(mode,reg,r,sz); this.setNZVC(r,sz);
    } else { // AND EA → Dn
      const r = this.readDn(dn,sz) & this.readEA(mode,reg,sz);
      this.writeDn(dn,r,sz); this.setNZVC(r,sz);
    }
    this.cycles+=4;
  }

  // ── Group D : ADD / ADDA / ADDX ───────────────────────────────
  _gD(op) {
    const dn   = (op >> 9) & 7;
    const sz   = (op >> 6) & 3;
    const mode = (op >> 3) & 7;
    const reg  = op & 7;
    const dir  = (op >> 8) & 1;

    if (sz === 3) { // ADDA
      const opSz = dir ? 2 : 1;
      const src  = this.readEA(mode, reg, opSz);
      const ext  = opSz===1 ? this.sext16(src&0xFFFF) : (src|0);
      this.a[dn] = (this.a[dn] + ext) >>> 0; this.cycles+=8; return;
    }

    // ADDX: 1101 DDD1 ss 00 mDDD
    if (dir && (mode===0||mode===4)) {
      const rmMode = (mode===4);
      let src, dst;
      if (rmMode) {
        this.a[reg]=(this.a[reg]-(sz===0?1:sz===1?2:4))>>>0; src=this.bus.readSize(this.a[reg],sz);
        this.a[dn] =(this.a[dn] -(sz===0?1:sz===1?2:4))>>>0; dst=this.bus.readSize(this.a[dn], sz);
      } else { src=this.readDn(reg,sz); dst=this.readDn(dn,sz); }
      const r=this.doAdd(src,dst,sz,true);
      if (rmMode) this.bus.writeSize(this.a[dn],r,sz);
      else        this.writeDn(dn,r,sz);
      this.cycles+=4; return;
    }

    if (dir) { // ADD Dn → EA
      const r = this.doAdd(this.readDn(dn,sz), this.readEA(mode,reg,sz), sz, false);
      this.writeEA(mode,reg,r,sz);
    } else {   // ADD EA → Dn
      const r = this.doAdd(this.readEA(mode,reg,sz), this.readDn(dn,sz), sz, false);
      this.writeDn(dn,r,sz);
    }
    this.cycles+=4;
  }

  // ── Group E : Shift / Rotate ───────────────────────────────────
  _gE(op) {
    const sz   = (op >> 6) & 3;
    const mode = (op >> 3) & 7;
    const reg  = op & 7;
    const dir  = (op >> 8) & 1; // 0=right, 1=left
    const type = (op >> 3) & 3; // 00=AS 01=LS 10=ROX 11=RO

    if (sz === 3) { // memory shift (by 1)
      const v = this.readEA(mode, reg, 1);
      const r = this._doShift(type, dir, v, 1, 1);
      this.writeEA(mode, reg, r, 1); this.cycles += 8; return;
    }

    const regImm = (op >> 5) & 1; // 0=immediate count, 1=Dn
    let cnt = regImm ? (this.d[(op>>9)&7] & 63) : ((op>>9)&7||8);
    const shType = (op >> 3) & 3;
    const [mask, msb] = this._masks(sz);
    const v = this.readDn(reg, sz) & mask;
    const r = this._doShift(shType, dir, v, cnt, sz);
    this.writeDn(reg, r, sz); this.cycles += 4 + cnt * 2;
  }

  _doShift(type, left, v, cnt, sz) {
    const [mask, msb] = this._masks(sz);
    const bits = sz===0 ? 8 : sz===1 ? 16 : 32;
    let r = v, lastOut = 0;
    let c = false, x = (this.sr >> 4) & 1;

    cnt &= 63;
    if (cnt === 0) {
      // No shift: C=0, V=0, Z/N from value
      let sr = this.sr & ~0x0F;
      if ((v & mask) === 0) sr |= 0x04;
      if (v & msb)          sr |= 0x08;
      this.sr = sr; return v;
    }

    for (let i = 0; i < cnt; i++) {
      if (type === 0) { // ASL / ASR
        if (left) { lastOut=(r&msb)!==0; r=(r<<1)&mask; }
        else      { lastOut=(r&1)!==0;   r=((r|((r&msb)?mask+1:0))>>1)&mask; /* arithmetic: sign-extend */ }
        // Actually for ASR: replicate sign bit
        if (!left) r = ((v & msb ? msb | (v >> 1) : v >> 1) & mask);
      } else if (type === 1) { // LSL / LSR
        if (left) { lastOut=(r&msb)!==0; r=(r<<1)&mask; }
        else      { lastOut=(r&1)!==0;   r=(r>>>1)&mask; }
      } else if (type === 2) { // ROXL / ROXR
        if (left) { lastOut=(r&msb)!==0; r=((r<<1)|(x?1:0))&mask; x=lastOut?1:0; }
        else      { lastOut=(r&1)!==0;   r=((r>>>1)|(x?msb:0))&mask; x=lastOut?1:0; }
      } else { // ROL / ROR
        if (left) { lastOut=(r&msb)!==0; r=((r<<1)|(lastOut?1:0))&mask; }
        else      { lastOut=(r&1)!==0;   r=((r>>>1)|(lastOut?msb:0))&mask; }
      }
      c = lastOut;
    }

    // For ASL/ASR, redo properly to avoid loop inaccuracy
    if (type === 0 || type === 1) {
      if (left) {
        if (cnt >= bits) { c=false; r=0; }
        else { c=(v & (msb >>> (cnt-1))) !== 0; r=(v << cnt) & mask; }
      } else {
        if (cnt >= bits) { c = type===0 ? (v & msb)!==0 : false; r = type===0 ? (v&msb?mask:0) : 0; }
        else { c=(v & (1<<(cnt-1)))!==0; r = type===0 ? ((v|((v&msb)?~(mask>>>1)&mask:0))>>cnt)&mask : (v>>>cnt)&mask; }
      }
    }

    // V flag for ASL: set if any bits shifted out differ from final sign
    let vf = false;
    if (type === 0 && left && cnt > 0) {
      // Set if sign bit changed at any point
      const origSign = (v & msb) !== 0;
      const newSign  = (r & msb) !== 0;
      vf = origSign !== newSign;
    }

    let sr = this.sr & ~0x1F;
    if (c)             sr |= 0x11; // C and X
    if (vf)            sr |= 0x02;
    if ((r & mask)===0) sr |= 0x04;
    if (r & msb)       sr |= 0x08;
    if (type===3||type===2) sr = (sr&~0x10)|(this.sr&0x10); // ROL/ROR don't affect X
    this.sr = sr;
    return r & mask;
  }

  // ── Run N cycles ─────────────────────────────────────────────
  run(targetCycles) {
    while (this.cycles < targetCycles) this.step();
    const overshoot = this.cycles - targetCycles;
    this.cycles = 0;
    return overshoot;
  }
}

// Helper: count set bits
function _bitCount(n) {
  let c = 0; while (n) { c += n & 1; n >>>= 1; } return c;
}

// ─────────────────────────────────────────────────────────────
// VDP — Yamaha YM7101 Video Display Processor (v0.1 timing stub)
// Full register interface; blank framebuffer output; correct
// VBlank/HBlank interrupt timing for game compatibility.
// ─────────────────────────────────────────────────────────────
class GenVDP {
  constructor(bus) {
    this.bus = bus;  // Add bus reference
    this.vram    = new Uint8Array(0x10000);   // 64 KB
    this.cram    = new Uint16Array(64);       // 64 × 9-bit palette entries
    this.vsram   = new Uint16Array(40);       // 40 × 11-bit vertical scroll
    this.regs    = new Uint8Array(24);        // VDP registers 0–23
    this.framebuf = new Uint8ClampedArray(GEN_W * 240 * 4); // RGBA — 240 covers both PAL (240) and NTSC (224)

 // Control port state machine
    // Byte and word writes have independent pending state — a byte write
    // mid-sequence must not corrupt a pending word-command and vice versa.
    this._ctrlPendWord = false;  // waiting for 2nd word of address/DMA command
    this._ctrlPendByte = false;  // waiting for 2nd byte of a byte-write reg command
    this._ctrlFirst = 0;
    this._addrReg   = 0;       // VRAM/CRAM/VSRAM address register
    this._addrInc   = 2;       // auto-increment (VDP reg 15)
    this._cdReg     = 0;       // code register (CD bits)

    // Counters
    this.vcounter   = 0;
    this.hcounter   = 0;
    this.vblank     = false;
    this.hblank     = false;
this.dmaActive  = false;
    this.frame      = 0;
    this._isPAL     = false;
    this._vintPending = false;  // F flag — VBlank interrupt pending

 // Pending FIFO (simplified)
    this._readBuffer = 0;
    this._dmaFillData = 0;  // last word written to data port (used by VRAM fill)
    this._diagDmaCount = 0; // diagnostic: total DMA operations fired
	this._dmaFillPending = false;
this._vramDirty = false;

    this._fillFramebuf();
  }


_fillFramebuf() {
    this.framebuf.fill(0);
    if (!(this.regs[1] & 0x40)) return;
    for (let y = 0; y < GEN_H; y++) this._renderScanline(y);
  }

  _renderScanline(y) {
    this._renderPlaneLine(true,  y); // Plane B (back)
    this._renderPlaneLine(false, y); // Plane A (front)
    this._renderSpriteLine(y);
  }

  _renderPlaneLine(isB, y) {
    const hsize = [32, 64, 0, 128][ this.regs[16]       & 3];
    const vsize = [32, 64, 0, 128][(this.regs[16] >> 4) & 3];
    if (!hsize || !vsize) return;

    const planeBase   = isB ? (this.regs[4] & 0x07) << 13 : (this.regs[2] & 0x38) << 10;
    const hscBase     = (this.regs[13] & 0x3F) << 10;
    const hscMode     = this.regs[11] & 0x03;
    const hscPlaneOff = isB ? 2 : 0;

    const hscOff  = hscMode === 3 ? y * 4 : hscMode === 2 ? (y >> 3) * 4 : 0;
    const hscAddr = (hscBase + hscOff + hscPlaneOff) & 0xFFFF;
    const hscroll = (0x400 - ((this.vram[hscAddr] << 8) | this.vram[hscAddr + 1])) & 0x3FF;
    const vscroll = (isB ? this.vsram[1] : this.vsram[0]) & 0x3FF;

    const scrollY = (y + vscroll) & (vsize * 8 - 1);
    const tileRow = scrollY >> 3;
    const fineY   = scrollY & 7;

    for (let x = 0; x < GEN_W; x++) {
      const scrollX = (x + hscroll) & (hsize * 8 - 1);
      const tileCol = scrollX >> 3;
      const fineX   = scrollX & 7;

      const ntAddr  = (planeBase + ((tileRow & (vsize - 1)) * hsize + (tileCol & (hsize - 1))) * 2) & 0xFFFF;
      const entry   = (this.vram[ntAddr] << 8) | this.vram[ntAddr + 1];
      const tileIdx = entry & 0x7FF;
      const palLine = (entry >> 13) & 3;
      const row     = (entry >> 12) & 1 ? (7 - fineY) : fineY; // vFlip
      const col     = (entry >> 11) & 1 ? (7 - fineX) : fineX; // hFlip

      const byteAddr = (tileIdx * 32 + row * 4 + (col >> 1)) & 0xFFFF;
      const nibble   = (col & 1) ? (this.vram[byteAddr] & 0xF) : ((this.vram[byteAddr] >> 4) & 0xF);
      if (nibble === 0) continue;

      const rgb = this._decodeCRAMColor(this.cram[(palLine * 16 + nibble) & 0x3F]);
      const pi  = (y * GEN_W + x) * 4;
      this.framebuf[pi]     = rgb.r;
      this.framebuf[pi + 1] = rgb.g;
      this.framebuf[pi + 2] = rgb.b;
      this.framebuf[pi + 3] = 255;
    }
  }

  // ── DMA engine ─────────────────────────────────────────────
  _writeByCD(addr, val, dst) {
    const d = dst & 0xF;
    if      (d === 1) this.vram[(addr) & 0xFFFF] = val;
    else if (d === 3) this.cram[(addr >> 1) & 0x3F] = val;
    else if (d === 5) this.vsram[(addr >> 1) & 0x27] = val;
  }

_processDMA(cd) {
    const dmaMode = (this.regs[23] >> 6) & 3;
    const rawLen  = (this.regs[20] << 8) | this.regs[19];
    const dmaLen  = rawLen === 0 ? 0x10000 : rawLen;
    const srcAddr = ((this.regs[23] & 0x7F) << 17) | (this.regs[22] << 9) | (this.regs[21] << 1);
    switch (dmaMode) {
      case 0: case 1: this._dmaMemoryCopy(srcAddr, dmaLen, cd); break;
      case 2: this._dmaVRAMFill(dmaLen, cd); break;
      case 3: this._dmaVRAMCopy(srcAddr, dmaLen, cd); break;
    }
    this.regs[19] = 0; this.regs[20] = 0;
    this._diagDmaCount++;
  }

_dmaMemoryCopy(srcAddr, len, cd) {
    // DMA transfers are 16-bit words; len is word count
    const d = cd & 0xF;
    const inc = this._addrInc;
    for (let i = 0; i < len; i++) {
      const hi  = this.bus.read8(srcAddr & 0xFFFFFF);
      const lo  = this.bus.read8((srcAddr + 1) & 0xFFFFFF);
      const word = (hi << 8) | lo;
      const addr = this._addrReg & 0xFFFF;
      if      (d === 1) { this.vram[addr] = hi; this.vram[(addr+1) & 0xFFFF] = lo; }
      else if (d === 3) { this.cram[(addr >> 1) & 0x3F] = word; }
      else if (d === 5) { this.vsram[(addr >> 1) & 0x27] = word; }
      srcAddr = (srcAddr + 2) & 0xFFFFFF;
      this._addrReg = (this._addrReg + inc) & 0xFFFF;
    }
  }

_dmaVRAMFill(len, cd) {
    // Fill value = high byte of last data port write (Genesis hardware behavior)
    const fillVal = (this._dmaFillData >> 8) & 0xFF;
    const inc = this._addrInc;
    for (let i = 0; i < len; i++) {
      const addr = this._addrReg & 0xFFFF;
      this.vram[addr] = fillVal;
      this._addrReg = (addr + inc) & 0xFFFF;
    }
  }

  _dmaVRAMCopy(len, cd) {
    let src = ((this.regs[23] & 0x7F) << 8) | this.regs[22];
    const inc = this._addrInc;
    for (let i = 0; i < len; i++) {
      this._writeByCD(this._addrReg, this.vram[src & 0xFFFF], cd);
      src = (src + 1) & 0xFFFF;
      this._addrReg = (this._addrReg + inc) & 0xFFFF;
    }
  }

  _readDMAData() {
    const cd = this._cdReg & 0xF, addr = this._addrReg & 0xFFFF;
    if (cd === 1) return this.vram[addr];
    if (cd === 3) return this.cram[(addr >> 1) & 0x3F];
    if (cd === 5) return this.vsram[(addr >> 1) & 0x27];
    return 0;
  }

  _writeDMAData(val) { this._writeByCD(this._addrReg, val, this._cdReg); }

 _renderSprites() {
    // Sprite attribute table base (reg 5, bits 6-0, << 9)
    const sprBase  = (this.regs[5] & 0x7F) << 9;
    const h40      = this.regs[12] & 1;
    const maxSpr   = h40 ? 80 : 64;

    // Walk the link chain to build ordered sprite list
    const sprites = [];
    let link = 0;
    for (let n = 0; n < maxSpr; n++) {
      const base = (sprBase + link * 8) & 0xFFFF;
      const yraw = ((this.vram[base] << 8) | this.vram[base + 1]) & 0x3FF;
      const sz   = this.vram[base + 2];
      link       = this.vram[base + 3] & 0x7F;
      const attr = (this.vram[base + 4] << 8) | this.vram[base + 5];
      const xraw = ((this.vram[base + 6] << 8) | this.vram[base + 7]) & 0x1FF;

      sprites.push({
        x: xraw - 128,
        y: yraw - 128,
        hCells: ((sz >> 2) & 3) + 1,
        vCells: (sz & 3) + 1,
        attr,
      });
      if (link === 0) break;
    }

    // Draw back-to-front so sprite 0 ends up on top
    for (let si = sprites.length - 1; si >= 0; si--) {
      const { x: sx, y: sy, hCells, vCells, attr } = sprites[si];
      const tileIdx = attr & 0x7FF;
      const palLine = (attr >> 13) & 3;
      const vFlip   = (attr >> 12) & 1;
      const hFlip   = (attr >> 11) & 1;

      for (let cy = 0; cy < vCells; cy++) {
        for (let cx = 0; cx < hCells; cx++) {
          // Genesis sprite tile order: column-major (top→bottom within each column)
          const tile = (tileIdx + cx * vCells + cy) & 0x7FF;

          for (let row = 0; row < 8; row++) {
            const screenY = sy + cy * 8 + (vFlip ? (7 - row) : row);
            if (screenY < 0 || screenY >= GEN_H) continue;

            for (let col = 0; col < 8; col++) {
              const screenX = sx + cx * 8 + (hFlip ? (7 - col) : col);
              if (screenX < 0 || screenX >= GEN_W) continue;

              const r = vFlip ? (7 - row) : row;
              const c = hFlip ? (7 - col) : col;

              // 4bpp decode: high nibble = left pixel, low nibble = right pixel
              const byteAddr = (tile * 32 + r * 4 + (c >> 1)) & 0xFFFF;
              const nibble   = (c & 1) ? (this.vram[byteAddr] & 0xF) : ((this.vram[byteAddr] >> 4) & 0xF);
              if (nibble === 0) continue;

              const rgb = this._decodeCRAMColor(this.cram[(palLine * 16 + nibble) & 0x3F]);
              const pi  = (screenY * GEN_W + screenX) * 4;
              this.framebuf[pi]     = rgb.r;
              this.framebuf[pi + 1] = rgb.g;
              this.framebuf[pi + 2] = rgb.b;
              this.framebuf[pi + 3] = 255;
            }
          }
        }
      }
    }
  }

  _renderSpriteLine(y) {
    const sprBase = (this.regs[5] & 0x7F) << 9;
    const maxSpr  = (this.regs[12] & 1) ? 80 : 64;

    // Walk link chain to collect visible sprites
    const sprites = [];
    let link = 0;
    for (let n = 0; n < maxSpr; n++) {
      const base = (sprBase + link * 8) & 0xFFFF;
      const yraw = ((this.vram[base] << 8) | this.vram[base + 1]) & 0x3FF;
      const sz   = this.vram[base + 2];
      link       = this.vram[base + 3] & 0x7F;
      const attr = (this.vram[base + 4] << 8) | this.vram[base + 5];
      const xraw = ((this.vram[base + 6] << 8) | this.vram[base + 7]) & 0x1FF;
      const sy   = yraw - 128;
      const vCells = (sz & 3) + 1;
      if (y >= sy && y < sy + vCells * 8)
        sprites.push({ x: xraw - 128, sy, hCells: ((sz >> 2) & 3) + 1, vCells, attr });
      if (link === 0) break;
    }

    // Draw back-to-front so sprite 0 wins
    for (let si = sprites.length - 1; si >= 0; si--) {
      const { x: sx, sy, hCells, vCells, attr } = sprites[si];
      const tileIdx = attr & 0x7FF;
      const palLine = (attr >> 13) & 3;
      const vFlip   = (attr >> 12) & 1;
      const hFlip   = (attr >> 11) & 1;
      const localY  = y - sy;
      const cy      = vFlip ? (vCells - 1 - (localY >> 3)) : (localY >> 3);
      const row     = vFlip ? (7 - (localY & 7)) : (localY & 7);

      for (let cx = 0; cx < hCells; cx++) {
        const tile = (tileIdx + cx * vCells + cy) & 0x7FF;
        for (let col = 0; col < 8; col++) {
          const screenX = sx + cx * 8 + (hFlip ? (7 - col) : col);
          if (screenX < 0 || screenX >= GEN_W) continue;
          const c        = hFlip ? (7 - col) : col;
          const byteAddr = (tile * 32 + row * 4 + (c >> 1)) & 0xFFFF;
          const nibble   = (c & 1) ? (this.vram[byteAddr] & 0xF) : ((this.vram[byteAddr] >> 4) & 0xF);
          if (nibble === 0) continue;
          const rgb = this._decodeCRAMColor(this.cram[(palLine * 16 + nibble) & 0x3F]);
          const pi  = (y * GEN_W + screenX) * 4;
          this.framebuf[pi]     = rgb.r;
          this.framebuf[pi + 1] = rgb.g;
          this.framebuf[pi + 2] = rgb.b;
          this.framebuf[pi + 3] = 255;
        }
      }
    }
  }

  _decodeCRAMColor(color) {
    // Convert 9-bit CRAM color to RGB
    const r = ((color >> 1) & 0x07) * 36;  // 3 bits for red
    const g = ((color >> 5) & 0x07) * 36;  // 3 bits for green
    const b = ((color >> 9) & 0x07) * 36;  // 3 bits for blue
    return { r, g, b };
  }

  // Returns true when VBlank interrupt should fire (level 6)
  tickLine(line, isPAL) {
    this.vcounter = line;
    const activeLines = isPAL ? PAL_ACTIVE : NTSC_ACTIVE;
    this.vblank  = (line >= activeLines);
    this.hblank  = false;
    // Render the line
    this._renderLine(line);
    const doVBlank = (line === activeLines);
    if (doVBlank) this._vintPending = true; // set F flag
    return doVBlank;
  }

_renderLine(line) {
    if (line === 0) this.framebuf.fill(0); // clear at frame start
    if (line < GEN_H && (this.regs[1] & 0x40)) this._renderScanline(line);
  }

  // Returns true when HBlank interrupt should fire (level 4)
  // Called mid-line based on HInt counter
  checkHInt(line, isPAL) {
    const hIntCnt = this.regs[10];
    if (!(this.regs[0] & 0x10)) return false; // HInt disabled
    return (line <= (isPAL ? PAL_ACTIVE : NTSC_ACTIVE)) && ((line % (hIntCnt+1)) === 0);
  }

  // ── Port I/O ────────────────────────────────────────────────
  read8(off) {
    off &= 0x1F;
    if (off < 4) return (this.read16(off & ~1) >> ((off&1)?0:8)) & 0xFF;
    if (off === 8 || off === 9) { // H/V counter
      return off===8 ? (this.hcounter >> 1) & 0xFF : this.vcounter & 0xFF;
    }
    return 0xFF;
  }

  read16(off) {
    off &= 0x1F;
 switch (off & 0xFE) {
      case 0x00:
      case 0x02: // Data port (0xC00000-0xC00003)
        return this._readData();
      case 0x04:
      case 0x06: { // Control port (0xC00004-0xC00007)
        const s = this._status();
        this._vintPending = false;
        return s;
      }
      case 0x08: // H/V counter
        return ((this.hcounter>>1)&0xFF) | (this.vcounter<<8);
      default: return 0xFFFF;
    }
  }

  write8(off, val) {
    off &= 0x1F;
    if (off < 2) { // Data port
      if (off & 1) this._writeData(val);
      else         this._writeData(val << 8);
    } else if (off < 8) { // Control port bytes (0xC00004-0xC00007)
      this._writeCtrl(val & 0xFF, true);
    }
  }

 write16(off, val) {
    off &= 0x1F;
    switch (off & 0xFE) {
      case 0x00:
      case 0x02: this._writeData(val); break;    // 0xC00000 and 0xC00002 both data port
      case 0x04:
      case 0x06: this._writeCtrl(val, false); break;  // 0xC00004 and 0xC00006 both ctrl port
    }
  }

_status() {
    // Bit 9=FIFO full, 8=FIFO empty, 7=F(VBlank pending), 3=VBlank, 2=HBlank, 1=DMA, 0=PAL
    return 0x0200 // FIFO never full
         | 0x0100 // FIFO always empty
         | (this._vintPending ? 0x0080 : 0) // F flag — games poll this for VSync
         | (this.vblank ? 0x0008 : 0)
         | (this.hblank ? 0x0004 : 0)
         | (this.dmaActive ? 0x0002 : 0)
         | (this._isPAL ? 0x0001 : 0);
  }

 _writeCtrl(val, isByte) {
    if (isByte) {
      // Byte writes: two consecutive bytes form a register write word (0x8R 0xVV)
      // This path is independent of the word-write pending state.
      if (!this._ctrlPendByte) { this._ctrlFirst = val; this._ctrlPendByte = true; return; }
      const w = (this._ctrlFirst << 8) | val;
      this._ctrlPendByte = false;
      this._processCtrlWord(w); return;
    }
    // Word write path
    if (this._ctrlPendWord) {
      // Second word of address/DMA command
      const w2 = val;
      const w1 = this._ctrlFirst;
      this._ctrlPendWord = false;
      // CD5..2 from w2 bits 7..4; CD1..0 from w1 bits 15..14
      // Address: w1 bits 13..0 | w2 bits 1..0 (high two address bits)
      const cd   = ((w1 >> 14) & 3) | ((w2 & 0xF0) >> 2);
      const addr = (w1 & 0x3FFF) | ((w2 & 0x03) << 14);
      this._cdReg   = cd;
      this._addrReg = addr;
 if (cd & 0x20) {
    const dmaMode = (this.regs[23] >> 6) & 3;
    if (dmaMode === 2) {
        // VRAM fill: hold state, fire on next data port write
        this._dmaFillPending = true;
    } else {
        this._processDMA(cd);
        this.dmaActive = false;
    }
}
 return;
    }
    // First word: register write or first half of address command
    if ((val & 0xC000) === 0x8000) {
      // Register write: 1000 RRRR RRVV VVVV  (bits 13..8 = reg, 7..0 = val)
      const r = (val >> 8) & 0x1F;
      const v = val & 0xFF;
      if (r < 24) {
        this.regs[r] = v;
        if (r === 15) this._addrInc = v;
      }
      return;
    }
    // First half of two-word address/DMA command
    this._ctrlFirst    = val;
    this._ctrlPendWord = true;
  }

  _processCtrlWord(w) {
    if ((w & 0xC000) === 0x8000) {
      const r=(w>>8)&0x1F, v=w&0xFF;
      if (r<24) { this.regs[r]=v; if(r===15)this._addrInc=v; }
    }
  }

_writeData(val) {
    this._dmaFillData = val;

    // VRAM fill DMA fires on first data write after control setup
    if (this._dmaFillPending) {
        this._dmaFillPending = false;
        this._processDMA(this._cdReg);
        this.dmaActive = false;
        return;
    }

    const cd   = this._cdReg & 0xF;
    const addr = this._addrReg & 0xFFFF;
    if      (cd === 1) { this.vram[addr & 0xFFFF] = (val >> 8) & 0xFF; this.vram[(addr+1) & 0xFFFF] = val & 0xFF; }
    else if (cd === 3) { this.cram[(addr >> 1) & 0x3F] = val; }
    else if (cd === 5) { this.vsram[(addr >> 1) & 0x27] = val; }
    this._addrReg = (this._addrReg + this._addrInc) & 0xFFFF;
    this._vramDirty = true;
}

_readData() {
    const cd   = this._cdReg & 0xF;
    const addr = this._addrReg & 0xFFFF;
    let val = 0;
    if      (cd === 0x0) val = (this.vram[addr]<<8)|this.vram[(addr+1)&0xFFFF]; // VRAM read
    else if (cd === 0x8) val = this.cram[(addr>>1)&0x3F];                        // CRAM read
    else if (cd === 0x4) val = this.vsram[(addr>>1)&0x27];                       // VSRAM read
    this._addrReg = (this._addrReg + this._addrInc) & 0xFFFF;
    return val;
  }
}


// ─────────────────────────────────────────────────────────────
// Z80 — Zilog Z80 co-processor (v0.1 stub)
// Runs YM2612 FM chip; provides backward compatibility for SMS
// games; handles decompression and special effects.
// ─────────────────────────────────────────────────────────────
class GenZ80 {
  constructor(bus) {
    this.bus = bus;           // Reference to main bus for memory access
    this.cycles = 0;          // Cycle counter
    
    // Registers
    this.A = 0; this.F = 0;
    this.B = 0; this.C = 0;
    this.D = 0; this.E = 0;
    this.H = 0; this.L = 0;
    this.A_ = 0; this.F_ = 0; // Alternate registers
    this.B_ = 0; this.C_ = 0;
    this.D_ = 0; this.E_ = 0;
    this.H_ = 0; this.L_ = 0;
    this.IX = 0; this.IY = 0;
    this.SP = 0xFD00;         // Stack pointer (Z80 RAM)
    this.PC = 0;              // Program counter
    
    // Flags: S Z - H - P N C
    this.SF = 0x80; this.ZF = 0x40; this.HF = 0x10;
    this.PF = 0x04; this.NF = 0x02; this.CF = 0x01;
    
    // Control flags
    this.IFF1 = 0; this.IFF2 = 0;  // Interrupt flip-flops
    this.IM = 0;                   // Interrupt mode
    this.halted = false;
    
    // Memory space (8KB internal RAM)
    this.ram = new Uint8Array(0x2000);
  }

  reset() {
    this.PC = 0;
    this.SP = 0xFD00;
    this.IFF1 = 0;
    this.IFF2 = 0;
    this.IM = 0;
    this.halted = false;
    this.cycles = 0;
  }

  // Main execution loop - run for target cycles
  run(targetCycles) {
    const startCycles = this.cycles;
    while (this.cycles - startCycles < targetCycles) {
      if (this.halted) {
        this.cycles += 4; // Advance time while halted
        continue;
      }
      const opcode = this._read(this.PC++);
      this._execute(opcode);
    }
  }

  // Execute single instruction
  _execute(opcode) {
    switch (opcode) {
      // 8-bit load group
      case 0x3E: this.A = this._fetch(); this.cycles += 7; break;  // LD A,n
      case 0x06: this.B = this._fetch(); this.cycles += 7; break;  // LD B,n
      case 0x0E: this.C = this._fetch(); this.cycles += 7; break;  // LD C,n
      case 0x16: this.D = this._fetch(); this.cycles += 7; break;  // LD D,n
      case 0x1E: this.E = this._fetch(); this.cycles += 7; break;  // LD E,n
      case 0x26: this.H = this._fetch(); this.cycles += 7; break;  // LD H,n
      case 0x2E: this.L = this._fetch(); this.cycles += 7; break;  // LD L,n
      
      // Load register to register
      case 0x7F: /* LD A,A */ this.cycles += 4; break;
      case 0x78: this.A = this.B; this.cycles += 4; break;  // LD A,B
      case 0x79: this.A = this.C; this.cycles += 4; break;  // LD A,C
      case 0x7A: this.A = this.D; this.cycles += 4; break;  // LD A,D
      case 0x7B: this.A = this.E; this.cycles += 4; break;  // LD A,E
      case 0x7C: this.A = this.H; this.cycles += 4; break;  // LD A,H
      case 0x7D: this.A = this.L; this.cycles += 4; break;  // LD A,L
      case 0x7E: this.A = this._read((this.H << 8) | this.L); this.cycles += 7; break;  // LD A,(HL)
      
      // Load to memory
      case 0x77: this._write((this.H << 8) | this.L, this.A); this.cycles += 7; break;  // LD (HL),A
      case 0x70: this._write((this.H << 8) | this.L, this.B); this.cycles += 7; break;  // LD (HL),B
      case 0x71: this._write((this.H << 8) | this.L, this.C); this.cycles += 7; break;  // LD (HL),C
      case 0x72: this._write((this.H << 8) | this.L, this.D); this.cycles += 7; break;  // LD (HL),D
      case 0x73: this._write((this.H << 8) | this.L, this.E); this.cycles += 7; break;  // LD (HL),E
      case 0x74: this._write((this.H << 8) | this.L, this.H); this.cycles += 7; break;  // LD (HL),H
      case 0x75: this._write((this.H << 8) | this.L, this.L); this.cycles += 7; break;  // LD (HL),L
      
      // 16-bit load group
      case 0x01: this.C = this._fetch(); this.B = this._fetch(); this.cycles += 10; break;  // LD BC,nn
      case 0x11: this.E = this._fetch(); this.D = this._fetch(); this.cycles += 10; break;  // LD DE,nn
      case 0x21: this.L = this._fetch(); this.H = this._fetch(); this.cycles += 10; break;  // LD HL,nn
      case 0x31: { const low = this._fetch(); const high = this._fetch(); this.SP = (high << 8) | low; this.cycles += 10; } break;  // LD SP,nn
      
      // Stack operations
      case 0x32: {  // LD (nn),A
        const low = this._fetch();
        const high = this._fetch();
        this._write((high << 8) | low, this.A);
        this.cycles += 13;
      } break;
      
      case 0x22: {  // LD (nn),HL
        const low = this._fetch();
        const high = this._fetch();
        const addr = (high << 8) | low;
        this._write(addr, this.L);
        this._write(addr + 1, this.H);
        this.cycles += 16;
      } break;
      
      // Increment/Decrement
      case 0x3C: this.A = this._inc8(this.A); this.cycles += 4; break;  // INC A
      case 0x04: this.B = this._inc8(this.B); this.cycles += 4; break;  // INC B
      case 0x0C: this.C = this._inc8(this.C); this.cycles += 4; break;  // INC C
      case 0x14: this.D = this._inc8(this.D); this.cycles += 4; break;  // INC D
      case 0x1C: this.E = this._inc8(this.E); this.cycles += 4; break;  // INC E
      case 0x24: this.H = this._inc8(this.H); this.cycles += 4; break;  // INC H
      case 0x2C: this.L = this._inc8(this.L); this.cycles += 4; break;  // INC L
      case 0x34: {  // INC (HL)
        const addr = (this.H << 8) | this.L;
        const val = this._read(addr);
        this._write(addr, this._inc8(val));
        this.cycles += 11;
      } break;
      
      // Arithmetic operations
      case 0x80: this.A = this._add8(this.A, this.B); this.cycles += 4; break;  // ADD A,B
      case 0x81: this.A = this._add8(this.A, this.C); this.cycles += 4; break;  // ADD A,C
      case 0x82: this.A = this._add8(this.A, this.D); this.cycles += 4; break;  // ADD A,D
      case 0x83: this.A = this._add8(this.A, this.E); this.cycles += 4; break;  // ADD A,E
      case 0x84: this.A = this._add8(this.A, this.H); this.cycles += 4; break;  // ADD A,H
      case 0x85: this.A = this._add8(this.A, this.L); this.cycles += 4; break;  // ADD A,L
      case 0x86: {  // ADD A,(HL)
        const val = this._read((this.H << 8) | this.L);
        this.A = this._add8(this.A, val);
        this.cycles += 7;
      } break;
      case 0xC6: {  // ADD A,n
        const val = this._fetch();
        this.A = this._add8(this.A, val);
        this.cycles += 7;
      } break;
      
      // Jump instructions
      case 0xC3: {  // JP nn
        const low = this._fetch();
        const high = this._fetch();
        this.PC = (high << 8) | low;
        this.cycles += 10;
      } break;
      
      case 0x00: this.cycles += 4; break;  // NOP
      case 0x76: this.halted = true; this.cycles += 4; break;  // HALT
      
      // Unimplemented opcodes - just consume cycles
      default:
        this.cycles += 4;
        break;
    }
  }

  // Helper methods
  _fetch() {
    return this._read(this.PC++);
  }

  _read(addr) {
    // Z80 address space map:
    // 0x0000-0x1FFF: Z80 RAM
    // 0x2000-0x3FFF: YM2612 (mirrored)
    // 0x4000-0x5FFF: Bank register
    // 0x6000-0x7FFF: VDP (mirrored)
    // 0x8000-0xFFFF: 68K banked memory
    
    if (addr < 0x2000) {
      return this.ram[addr];
    } else if (addr >= 0x8000) {
      // Map to 68K memory through bank register
      const bank = this.bus.z80Bank;
      const m68kAddr = (bank << 15) | (addr & 0x7FFF);
      return this.bus.read8(m68kAddr);
    }
    // For other addresses, return bus-specific values
    return this.bus.readZ80Port(addr);
  }

  _write(addr, val) {
    if (addr < 0x2000) {
      this.ram[addr] = val;
    } else if (addr >= 0x8000) {
      // Write to 68K memory through bank register
      const bank = this.bus.z80Bank;
      const m68kAddr = (bank << 15) | (addr & 0x7FFF);
      this.bus.write8(m68kAddr, val);
    } else {
      this.bus.writeZ80Port(addr, val);
    }
  }

  _inc8(val) {
    const result = (val + 1) & 0xFF;
    this.F = (result === 0 ? this.ZF : 0) |
             (result & 0x80 ? this.SF : 0) |
             ((val & 0x0F) === 0x0F ? this.HF : 0);
    return result;
  }

  _add8(a, b) {
    const result = a + b;
    this.F = (result & 0x100 ? this.CF : 0) |
             ((result & 0xFF) === 0 ? this.ZF : 0) |
             (result & 0x80 ? this.SF : 0) |
             (((a & 0x0F) + (b & 0x0F)) > 0x0F ? this.HF : 0);
    return result & 0xFF;
  }

  // Save state support
  saveState(st) {
    st.write8Array(this.ram);
    st.write32(this.cycles);
    st.write8(this.A); st.write8(this.F);
    st.write8(this.B); st.write8(this.C);
    st.write8(this.D); st.write8(this.E);
    st.write8(this.H); st.write8(this.L);
    st.write8(this.A_); st.write8(this.F_);
    st.write8(this.B_); st.write8(this.C_);
    st.write8(this.D_); st.write8(this.E_);
    st.write8(this.H_); st.write8(this.L_);
    st.write16(this.IX); st.write16(this.IY);
    st.write16(this.SP); st.write16(this.PC);
    st.write8(this.IFF1); st.write8(this.IFF2);
    st.write8(this.IM); st.write8(this.halted ? 1 : 0);
  }

  loadState(st) {
    st.read8Array(this.ram);
    this.cycles = st.read32();
    this.A = st.read8(); this.F = st.read8();
    this.B = st.read8(); this.C = st.read8();
    this.D = st.read8(); this.E = st.read8();
    this.H = st.read8(); this.L = st.read8();
    this.A_ = st.read8(); this.F_ = st.read8();
    this.B_ = st.read8(); this.C_ = st.read8();
    this.D_ = st.read8(); this.E_ = st.read8();
    this.H_ = st.read8(); this.L_ = st.read8();
    this.IX = st.read16(); this.IY = st.read16();
    this.SP = st.read16(); this.PC = st.read16();
    this.IFF1 = st.read8(); this.IFF2 = st.read8();
    this.IM = st.read8(); this.halted = st.read8() !== 0;
  }
}


// ─────────────────────────────────────────────────────────────
// YM2612 — FM Sound (v0.1 silence stub)
// ─────────────────────────────────────────────────────────────
class YM2612 {
  constructor() {
    this.regs = new Uint8Array(0x200); // two banks × 256 regs
    this.diag = { channels: Array.from({length:6}, (_,i)=>({idx:i,on:false,freq:0,vol:0,output:0})) };
  }
  write(bank, reg, val) { this.regs[(bank<<8)|reg] = val; }
  read()                { return 0x00; } // status: timer A/B flags (none active)
  clock(samples)        { /* silence */ return new Float32Array(samples * 2); }
}

// ─────────────────────────────────────────────────────────────
// SN76489 — PSG (v0.1 silence stub)
// ─────────────────────────────────────────────────────────────
class SN76489 {
  constructor() {
    this.regs  = new Uint8Array(8);
    this._latch = 0;
    this.diag   = { channels: Array.from({length:4}, (_,i)=>({idx:i,on:false,freq:0,vol:0,output:0})) };
  }
  write(val) {
    if (val & 0x80) {
      this._latch = val;
      const ch = (val >> 5) & 3;
      const typ = (val >> 4) & 1; // 0=tone/noise, 1=vol
      if (typ) this.regs[ch*2+1] = val & 0xF;
      else     this.regs[ch*2]   = val & 0xF;
    } else {
      const ch   = (this._latch >> 5) & 3;
      const typ  = (this._latch >> 4) & 1;
      if (!typ)  this.regs[ch*2] = (this.regs[ch*2] & 0xF) | ((val & 0x3F) << 4);
    }
  }
  clock(samples) { return new Float32Array(samples * 2); }
}

// ─────────────────────────────────────────────────────────────
// APU — Audio subsystem (wraps YM2612 + SN76489)
// Web Audio API output; silence in v0.1
// ─────────────────────────────────────────────────────────────
class GenAPU {
  constructor() {
    this.ym2612  = new YM2612();
    this.psg     = new SN76489();
    this.ctx     = null;
    this._node   = null;
    this._bufSize = 2048;
    this._sampleRate = 44100;
    this.diag    = {
      ym2612: this.ym2612.diag,
      psg:    this.psg.diag,
    };
  }

  initAudio() {
    try {
      this.ctx = new (window.AudioContext || window.webkitAudioContext)({ sampleRate: this._sampleRate });
      this._node = this.ctx.createScriptProcessor(this._bufSize, 0, 2);
      this._node.onaudioprocess = (e) => {
        // v0.1: silence
        e.outputBuffer.getChannelData(0).fill(0);
        e.outputBuffer.getChannelData(1).fill(0);
      };
      this._node.connect(this.ctx.destination);
    } catch(e) { console.warn('[gen-apu] Web Audio init failed:', e); }
  }

  stopAudio() {
    try {
      if (this._node)  { this._node.disconnect(); this._node = null; }
      if (this.ctx)    { this.ctx.close().catch(()=>{}); this.ctx = null; }
    } catch(e) {}
  }

  // YM2612 register write (called by bus)
  writeYM(bank, reg, val) { this.ym2612.write(bank, reg, val); }
  readYM()                { return this.ym2612.read(); }

  // PSG data write
  writePSG(val) { this.psg.write(val); }
}

// ─────────────────────────────────────────────────────────────
// GENESIS — main orchestrator
// ─────────────────────────────────────────────────────────────
class Genesis {
  constructor() {
    this.bus  = new GenBus();
    this.vdp = new GenVDP(this.bus);
    this.z80  = new GenZ80(this.bus);
    this.apu  = new GenAPU();
    this.cpu  = new M68K(this.bus);

    // Wire subsystem cross-references into bus
    this.bus.vdp  = this.vdp;
    this.bus.z80  = this.z80;
    this.bus.apu  = this.apu;
    this.bus.m68k = this.cpu;
	this.bus._logEvent = null; // wired from frontend

    // Hook VDP port writes for YM2612 (at 0xA00000-range, routed by bus stubs)
    // YM2612 lives at 0xA04000-0xA04003
    this._patchBusForAudio();

    this._isPAL       = false;
    this._linesFrame  = NTSC_LINES;
    this._activeLines = NTSC_ACTIVE;
    this._cpl         = NTSC_CPL;      // M68K cycles per line
    this._z80cpl      = Math.round(this._cpl * 3579545 / 7670454); // ~228
    this._overshoot   = 0;
  }

  _patchBusForAudio() {
    // Intercept YM2612 and PSG writes within bus.write8/16
    const orig8 = this.bus.write8.bind(this.bus);
    const apu   = this.apu;
    this.bus.write8 = (addr, val) => {
      const a24 = addr & 0xFFFFFF;
      if (a24 >= 0xA04000 && a24 <= 0xA04003) {
        // YM2612: A04000/A04001 = bank 0, A04002/A04003 = bank 1
        const bank = (a24 >> 1) & 1;
        if (a24 & 1) apu.writeYM(bank, apu._lastYMReg ?? 0, val);
        else         apu._lastYMReg = val;
        return;
      }
      if (a24 === 0xC00011 || a24 === 0xC00013 || a24 === 0xC00015 || a24 === 0xC00017) {
        apu.writePSG(val); return; // PSG at VDP area odd bytes
      }
      orig8(addr, val);
    };
  }

  loadROM(buf) {
    this.bus.loadROM(buf);
    this.cpu.reset();
    this.z80.reset();
    this.vdp.frame = 0;
  }

setRegion(region) {
    this._isPAL = (region === 'PAL');
    this.vdp._isPAL = this._isPAL;
    if (this._isPAL) {
      this._linesFrame  = PAL_LINES;
      this._activeLines = PAL_ACTIVE;
      this._cpl         = PAL_CPL;
      this._z80cpl      = Math.round(this._cpl * 3546893 / 7600489);
    } else {
      this._linesFrame  = NTSC_LINES;
      this._activeLines = NTSC_ACTIVE;
      this._cpl         = NTSC_CPL;
      this._z80cpl      = Math.round(this._cpl * 3579545 / 7670454);
    }
  }

  pressButton(pad, btn, pressed) {
    this.bus.pressButton(pad, btn, pressed);
  }

  // Run exactly one video frame
  runFrame() {
    const cpu = this.cpu, vdp = this.vdp;
    const pal = this._isPAL;

    for (let line = 0; line < this._linesFrame; line++) {
      // Run M68K for one scanline (minus any overshoot from last line)
      const target = this._cpl - this._overshoot;
      cpu.cycles = 0;
      cpu.run(target);
      this._overshoot = Math.max(0, cpu.cycles - target);

      // Z80 co-runs at its own clock ratio
      if (!this.bus.z80BusReq) this.z80.run(this._z80cpl);

      // VDP line tick — fires VBlank IRQ at line == activeLines
      const doVBlank = vdp.tickLine(line, pal);
      if (doVBlank) {
        vdp.frame++;
        // VBlank interrupt — level 6 auto-vector
        if (vdp.regs[1] & 0x20) cpu.interrupt(6);
      }

      // HBlank interrupt — level 4 auto-vector
      if (vdp.checkHInt(line, pal)) cpu.interrupt(4);
    }
  }

  // ── Save state ──────────────────────────────────────────────
  saveState() {
    return {
      cpu: {
        d: [...this.cpu.d], a: [...this.cpu.a],
        pc: this.cpu.pc, sr: this.cpu.sr,
        stopped: this.cpu.stopped, usp: this.cpu._usp,
      },
z80: {
        a:this.z80.A, f:this.z80.F, b:this.z80.B, c:this.z80.C,
        d:this.z80.D, e:this.z80.E, h:this.z80.H, l:this.z80.L,
        pc:this.z80.PC, sp:this.z80.SP,
        ram: Array.from(this.z80.ram),
      },
      wram: Array.from(this.bus.wram),
      sram: this.bus.hasSRAM ? Array.from(this.bus.sramData) : null,
      vdpRegs: Array.from(this.vdp.regs),
      padState: [...this.bus._padState],
      region: this._isPAL ? 'PAL' : 'NTSC',
    };
  }

  loadState(s) {
    if (!s) return;
    if (s.cpu) {
      s.cpu.d.forEach((v,i) => this.cpu.d[i]=v>>>0);
      s.cpu.a.forEach((v,i) => this.cpu.a[i]=v>>>0);
      this.cpu.pc = s.cpu.pc >>> 0;
      this.cpu.sr = s.cpu.sr & 0xFFFF;
      this.cpu.stopped = !!s.cpu.stopped;
      this.cpu._usp    = s.cpu.usp >>> 0;
    }
if (s.z80) {
      const zm = {a:'A',f:'F',b:'B',c:'C',d:'D',e:'E',h:'H',l:'L',pc:'PC',sp:'SP'};
      Object.entries(zm).forEach(([k,K]) => { if(s.z80[k]!==undefined) this.z80[K]=s.z80[k]; });
      if (s.z80.ram) this.z80.ram.set(new Uint8Array(s.z80.ram));
    }
    if (s.wram) this.bus.wram.set(new Uint8Array(s.wram));
    if (s.sram && this.bus.hasSRAM) this.bus.sramData.set(new Uint8Array(s.sram));
    if (s.vdpRegs) s.vdpRegs.forEach((v,i) => { if(i<24) this.vdp.regs[i]=v; });
    if (s.padState) this.bus._padState = [...s.padState];
    if (s.region) this.setRegion(s.region);
  }
}

// ─────────────────────────────────────────────────────────────
// dingBuildState — build a flat address→value snapshot of WRAM
// for the D!NG achievement engine.
// Genesis WRAM: 64 KB at 0xFF0000–0xFFFFFF.
// Keys are "0xFF0000" through "0xFFFFFF" (uppercase hex).
// ─────────────────────────────────────────────────────────────
function dingBuildState(gen) {
  const state = {};
  const wram  = gen.bus.wram;
  const BASE  = 0xFF0000;
  for (let i = 0; i < wram.length; i++) {
    state['0x' + (BASE + i).toString(16).toUpperCase().padStart(6,'0')] = wram[i];
  }
  return state;
}
