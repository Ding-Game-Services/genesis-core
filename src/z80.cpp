#include "genesis.h"
#include "ding_savestate.h"
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Flag bit constants (F register layout)
// ─────────────────────────────────────────────────────────────────────────────
static constexpr u8 FS = 0x80u;  // Sign
static constexpr u8 FZ = 0x40u;  // Zero
static constexpr u8 FY = 0x20u;  // Undocumented (bit 5 of result)
static constexpr u8 FH = 0x10u;  // Half-carry
static constexpr u8 FX = 0x08u;  // Undocumented (bit 3 of result)
static constexpr u8 FV = 0x04u;  // Parity / Overflow
static constexpr u8 FN = 0x02u;  // Subtract
static constexpr u8 FC = 0x01u;  // Carry

static inline u8 zparity(u8 v) {
    v ^= v >> 4; v ^= v >> 2; v ^= v >> 1;
    return (~v) & 1u;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / reset
// ─────────────────────────────────────────────────────────────────────────────
GenZ80::GenZ80(GenBus* b) : bus(b), cycles(0) {
    std::memset(ram, 0, sizeof(ram));
    A=F=B=C=D=E=H=L=0;
    A_=F_=B_=C_=D_=E_=H_=L_=0;
    IX=IY=0; SP=0xFD00u; PC=0;
    IFF1=IFF2=IM=I=R=0;
    halted=false;
}

void GenZ80::reset() {
    PC=0; SP=0xFD00u;
    IFF1=IFF2=IM=I=R=0;
    halted=false; cycles=0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Run loop
// ─────────────────────────────────────────────────────────────────────────────
void GenZ80::run(u32 targetCycles) {
    while (cycles < targetCycles) {
        if (halted) { cycles += 4; continue; }
        const u8 op = _fetch();
        R = static_cast<u8>((R & 0x80u) | ((R + 1u) & 0x7Fu));
        switch (op) {
            case 0xCB: { const u8 s=_fetch(); R=static_cast<u8>((R&0x80u)|((R+1u)&0x7Fu)); _executeCB(s); break; }
            case 0xDD: { const u8 s=_fetch(); R=static_cast<u8>((R&0x80u)|((R+1u)&0x7Fu)); _executeDD(s); break; }
            case 0xED: { const u8 s=_fetch(); R=static_cast<u8>((R&0x80u)|((R+1u)&0x7Fu)); _executeED(s); break; }
            case 0xFD: { const u8 s=_fetch(); R=static_cast<u8>((R&0x80u)|((R+1u)&0x7Fu)); _executeFD(s); break; }
            default:   _execute(op); break;
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Memory / port access
// ─────────────────────────────────────────────────────────────────────────────
u8 GenZ80::_fetch() { return _read(PC++); }

u8 GenZ80::_read(u16 addr) {
    if (addr < 0x2000u) return ram[addr];
    if (addr >= 0x8000u) {
        const u32 m68k = (static_cast<u32>(bus->z80Bank) << 15) | (addr & 0x7FFFu);
        return bus->read8(m68k);
    }
    return bus->readZ80Port(addr);
}

void GenZ80::_write(u16 addr, u8 val) {
    if (addr < 0x2000u) { ram[addr]=val; return; }
    if (addr >= 0x8000u) {
        const u32 m68k = (static_cast<u32>(bus->z80Bank) << 15) | (addr & 0x7FFFu);
        bus->write8(m68k, val); return;
    }
    bus->writeZ80Port(addr, val);
}

// Genesis Z80 uses memory-mapped I/O; route IN/OUT through the bus
u8   GenZ80::_inPort(u8 port)          { return bus->readZ80Port(static_cast<u16>((B<<8)|port)); }
void GenZ80::_outPort(u8 port, u8 val) { bus->writeZ80Port(static_cast<u16>((B<<8)|port), val); }

// ─────────────────────────────────────────────────────────────────────────────
// Stack helpers
// ─────────────────────────────────────────────────────────────────────────────
void GenZ80::_push16(u16 v) {
    SP -= 2;
    _write(SP,     static_cast<u8>(v >> 8));
    _write(SP + 1, static_cast<u8>(v));
}

u16 GenZ80::_pop16() {
    const u16 v = static_cast<u16>((_read(SP) << 8) | _read(static_cast<u16>(SP + 1)));
    SP += 2;
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// ALU helpers — all update F; return result
// ─────────────────────────────────────────────────────────────────────────────
u8 GenZ80::_add8(u8 a, u8 b, bool carry) {
    const u8  c   = carry ? 1u : 0u;
    const u16 r16 = static_cast<u16>(a) + b + c;
    const u8  res = static_cast<u8>(r16);
    F = (res & FS)
      | (res ? 0u : FZ)
      | (res & FY)
      | (((a & 0xFu) + (b & 0xFu) + c) >= 0x10u ? FH : 0u)
      | (res & FX)
      | ((~(a ^ b) & (a ^ res) & 0x80u) ? FV : 0u)
      | (r16 > 0xFFu ? FC : 0u);
    return res;
}

u8 GenZ80::_sub8(u8 a, u8 b, bool borrow) {
    const u8  br  = borrow ? 1u : 0u;
    const u16 r16 = static_cast<u16>(a) - b - br;
    const u8  res = static_cast<u8>(r16);
    F = (res & FS)
      | (res ? 0u : FZ)
      | (res & FY)
      | (static_cast<int>(a & 0xFu) - static_cast<int>(b & 0xFu) - br < 0 ? FH : 0u)
      | (res & FX)
      | (((a ^ b) & (a ^ res) & 0x80u) ? FV : 0u)
      | FN
      | (r16 > 0xFFu ? FC : 0u);
    return res;
}

u8 GenZ80::_and8(u8 a, u8 b) {
    const u8 res = a & b;
    F = (res & FS) | (res ? 0u : FZ) | (res & FY) | FH | (res & FX) | (zparity(res) ? FV : 0u);
    return res;
}

u8 GenZ80::_or8(u8 a, u8 b) {
    const u8 res = a | b;
    F = (res & FS) | (res ? 0u : FZ) | (res & FY) | (res & FX) | (zparity(res) ? FV : 0u);
    return res;
}

u8 GenZ80::_xor8(u8 a, u8 b) {
    const u8 res = a ^ b;
    F = (res & FS) | (res ? 0u : FZ) | (res & FY) | (res & FX) | (zparity(res) ? FV : 0u);
    return res;
}

void GenZ80::_cp8(u8 a, u8 b) {
    const u16 r16 = static_cast<u16>(a) - b;
    const u8  res = static_cast<u8>(r16);
    // Note: undocumented Y/X flags come from operand b for CP, not result
    F = (res & FS)
      | (res ? 0u : FZ)
      | (b & FY)
      | ((a & 0xFu) < (b & 0xFu) ? FH : 0u)
      | (b & FX)
      | (((a ^ b) & (a ^ res) & 0x80u) ? FV : 0u)
      | FN
      | (r16 > 0xFFu ? FC : 0u);
}

u8 GenZ80::_inc8(u8 v) {
    const u8 res = v + 1u;
    F = (res & FS)
      | (res ? 0u : FZ)
      | (res & FY)
      | ((v & 0xFu) == 0xFu ? FH : 0u)
      | (res & FX)
      | (v == 0x7Fu ? FV : 0u)
      | (F & FC);     // C unchanged
    return res;
}

u8 GenZ80::_dec8(u8 v) {
    const u8 res = v - 1u;
    F = (res & FS)
      | (res ? 0u : FZ)
      | (res & FY)
      | ((v & 0xFu) == 0x0u ? FH : 0u)
      | (res & FX)
      | (v == 0x80u ? FV : 0u)
      | FN
      | (F & FC);     // C unchanged
    return res;
}

// 16-bit: ADD HL,rr — S,Z,V unchanged; H,N,C updated
u16 GenZ80::_add16(u16 a, u16 b) {
    const u32 r = static_cast<u32>(a) + b;
    const u16 res = static_cast<u16>(r);
    F = (F & (FS | FZ | FV))
      | ((res >> 8) & FY)
      | (((a & 0xFFFu) + (b & 0xFFFu)) >= 0x1000u ? FH : 0u)
      | ((res >> 8) & FX)
      | (r > 0xFFFFu ? FC : 0u);
    return res;
}

// ADC HL,rr — all flags
u16 GenZ80::_adc16(u16 a, u16 b) {
    const u8  c  = (F & FC) ? 1u : 0u;
    const u32 r  = static_cast<u32>(a) + b + c;
    const u16 res = static_cast<u16>(r);
    F = (static_cast<u8>(res >> 8) & FS)
      | (res ? 0u : FZ)
      | ((res >> 8) & FY)
      | (((a & 0xFFFu) + (b & 0xFFFu) + c) >= 0x1000u ? FH : 0u)
      | ((res >> 8) & FX)
      | ((~(a ^ b) & (a ^ res) & 0x8000u) ? FV : 0u)
      | (r > 0xFFFFu ? FC : 0u);
    return res;
}

// SBC HL,rr — all flags
u16 GenZ80::_sbc16(u16 a, u16 b) {
    const u8  c  = (F & FC) ? 1u : 0u;
    const u32 r  = static_cast<u32>(a) - b - c;
    const u16 res = static_cast<u16>(r);
    F = (static_cast<u8>(res >> 8) & FS)
      | (res ? 0u : FZ)
      | ((res >> 8) & FY)
      | (static_cast<int>(a & 0xFFFu) - static_cast<int>(b & 0xFFFu) - c < 0 ? FH : 0u)
      | ((res >> 8) & FX)
      | (((a ^ b) & (a ^ res) & 0x8000u) ? FV : 0u)
      | FN
      | (r > 0xFFFFu ? FC : 0u);
    return res;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main execute — opcodes 0x00–0xFF
// ─────────────────────────────────────────────────────────────────────────────
void GenZ80::_execute(u8 op) {
    // Local helpers for register access by index 0=B 1=C 2=D 3=E 4=H 5=L 6=(HL) 7=A
    auto HL  = [&]() -> u16 { return static_cast<u16>((H << 8) | L); };
    auto get = [&](u32 r) -> u8 {
        switch (r & 7u) {
            case 0: return B; case 1: return C; case 2: return D; case 3: return E;
            case 4: return H; case 5: return L; case 6: return _read(HL()); default: return A;
        }
    };
    auto set = [&](u32 r, u8 v) {
        switch (r & 7u) {
            case 0: B=v; break; case 1: C=v; break; case 2: D=v; break; case 3: E=v; break;
            case 4: H=v; break; case 5: L=v; break; case 6: _write(HL(), v); break; default: A=v; break;
        }
    };
    auto cc = [&](u32 c) -> bool {
        switch (c & 7u) {
            case 0: return !(F & FZ); case 1: return (F & FZ) != 0;
            case 2: return !(F & FC); case 3: return (F & FC) != 0;
            case 4: return !(F & FV); case 5: return (F & FV) != 0;
            case 6: return !(F & FS); case 7: return (F & FS) != 0;
        }
        return false;
    };

    // Dense LD r,r block (0x40–0x7F) and ALU block (0x80–0xBF)
    if (op >= 0x40u && op < 0xC0u) {
        if (op == 0x76u) { halted = true; cycles += 4; return; }  // HALT
        if (op < 0x80u) {
            // LD r,r
            const u8 v = get(op & 7u);
            set((op >> 3) & 7u, v);
            cycles += ((op & 7u) == 6u || ((op >> 3) & 7u) == 6u) ? 7u : 4u;
        } else {
            // ALU block 0x80–0xBF
            const u8 v = get(op & 7u);
            const u32 cyc = (op & 7u) == 6u ? 7u : 4u;
            switch ((op >> 3) & 7u) {
                case 0: A = _add8(A, v, false); break;
                case 1: A = _add8(A, v, (F & FC) != 0); break;  // ADC
                case 2: A = _sub8(A, v, false); break;
                case 3: A = _sub8(A, v, (F & FC) != 0); break;  // SBC
                case 4: A = _and8(A, v); break;
                case 5: A = _xor8(A, v); break;
                case 6: A = _or8 (A, v); break;
                case 7: _cp8(A, v);      break;
            }
            cycles += cyc;
        }
        return;
    }

    switch (op) {
        // ── 0x00–0x0F ───────────────────────────────────────────────────────
        case 0x00: cycles += 4; break;  // NOP
        case 0x01: C=_fetch(); B=_fetch(); cycles+=10; break;  // LD BC,nn
        case 0x02: _write(static_cast<u16>((B<<8)|C), A); cycles+=7; break;  // LD (BC),A
        case 0x03: { u16 v=static_cast<u16>((B<<8)|C)+1; B=v>>8; C=v; cycles+=6; break; }
        case 0x04: B=_inc8(B); cycles+=4; break;
        case 0x05: B=_dec8(B); cycles+=4; break;
        case 0x06: B=_fetch(); cycles+=7; break;
        case 0x07: {  // RLCA
            const u8 c=(A>>7)&1u; A=(A<<1)|c;
            F=(F&(FS|FZ|FV))|(A&(FY|FX))|(c?FC:0u); cycles+=4; break;
        }
        case 0x08: { u8 ta=A,tf=F; A=A_; F=F_; A_=ta; F_=tf; cycles+=4; break; }  // EX AF,AF'
        case 0x09: { u16 hl=static_cast<u16>((H<<8)|L); u16 r=_add16(hl,static_cast<u16>((B<<8)|C)); H=r>>8; L=r; cycles+=11; break; }
        case 0x0A: A=_read(static_cast<u16>((B<<8)|C)); cycles+=7; break;
        case 0x0B: { u16 v=static_cast<u16>((B<<8)|C)-1; B=v>>8; C=v; cycles+=6; break; }
        case 0x0C: C=_inc8(C); cycles+=4; break;
        case 0x0D: C=_dec8(C); cycles+=4; break;
        case 0x0E: C=_fetch(); cycles+=7; break;
        case 0x0F: {  // RRCA
            const u8 c=A&1u; A=(A>>1)|(c<<7);
            F=(F&(FS|FZ|FV))|(A&(FY|FX))|(c?FC:0u); cycles+=4; break;
        }
        // ── 0x10–0x1F ───────────────────────────────────────────────────────
        case 0x10: {  // DJNZ e
            const s8 d=static_cast<s8>(_fetch()); B--;
            if (B) { PC=static_cast<u16>(PC+d); cycles+=13; } else { cycles+=8; } break;
        }
        case 0x11: E=_fetch(); D=_fetch(); cycles+=10; break;
        case 0x12: _write(static_cast<u16>((D<<8)|E), A); cycles+=7; break;
        case 0x13: { u16 v=static_cast<u16>((D<<8)|E)+1; D=v>>8; E=v; cycles+=6; break; }
        case 0x14: D=_inc8(D); cycles+=4; break;
        case 0x15: D=_dec8(D); cycles+=4; break;
        case 0x16: D=_fetch(); cycles+=7; break;
        case 0x17: {  // RLA
            const u8 c=(F&FC)?1u:0u, out=(A>>7)&1u;
            A=(A<<1)|c; F=(F&(FS|FZ|FV))|(A&(FY|FX))|(out?FC:0u); cycles+=4; break;
        }
        case 0x18: { const s8 d=static_cast<s8>(_fetch()); PC=static_cast<u16>(PC+d); cycles+=12; break; }  // JR
        case 0x19: { u16 hl=static_cast<u16>((H<<8)|L); u16 r=_add16(hl,static_cast<u16>((D<<8)|E)); H=r>>8; L=r; cycles+=11; break; }
        case 0x1A: A=_read(static_cast<u16>((D<<8)|E)); cycles+=7; break;
        case 0x1B: { u16 v=static_cast<u16>((D<<8)|E)-1; D=v>>8; E=v; cycles+=6; break; }
        case 0x1C: E=_inc8(E); cycles+=4; break;
        case 0x1D: E=_dec8(E); cycles+=4; break;
        case 0x1E: E=_fetch(); cycles+=7; break;
        case 0x1F: {  // RRA
            const u8 c=(F&FC)?1u:0u, out=A&1u;
            A=(A>>1)|(c<<7); F=(F&(FS|FZ|FV))|(A&(FY|FX))|(out?FC:0u); cycles+=4; break;
        }
        // ── 0x20–0x2F ───────────────────────────────────────────────────────
        case 0x20: { const s8 d=static_cast<s8>(_fetch()); if(!(F&FZ)){PC=static_cast<u16>(PC+d);cycles+=12;}else{cycles+=7;} break; }  // JR NZ
        case 0x21: L=_fetch(); H=_fetch(); cycles+=10; break;
        case 0x22: { const u8 lo=_fetch(),hi=_fetch(); const u16 a=static_cast<u16>((hi<<8)|lo); _write(a,L); _write(a+1,H); cycles+=16; break; }
        case 0x23: { u16 v=static_cast<u16>((H<<8)|L)+1; H=v>>8; L=v; cycles+=6; break; }
        case 0x24: H=_inc8(H); cycles+=4; break;
        case 0x25: H=_dec8(H); cycles+=4; break;
        case 0x26: H=_fetch(); cycles+=7; break;
        case 0x27: {  // DAA
            u8 a=A; u8 cf=0,hf=(F&FH)?1u:0u;
            if (!(F & FN)) {
                if (hf || (a & 0xFu) > 9u)  { a += 0x06u; cf |= (a < 0x06u) ? FC : 0u; }
                if ((F&FC) || A > 0x99u)     { a += 0x60u; cf = FC; }
            } else {
                if (hf) a -= 0x06u;
                if (F & FC) { a -= 0x60u; cf = FC; }
            }
            F = (a & FS) | (a ? 0u : FZ) | (a & FY) | (a & FX)
              | (zparity(a) ? FV : 0u) | (F & FN) | cf
              | (((A ^ a) & 0x10u) ? FH : 0u);
            A = a; cycles += 4; break;
        }
        case 0x28: { const s8 d=static_cast<s8>(_fetch()); if(F&FZ){PC=static_cast<u16>(PC+d);cycles+=12;}else{cycles+=7;} break; }  // JR Z
        case 0x29: { u16 hl=static_cast<u16>((H<<8)|L); u16 r=_add16(hl,hl); H=r>>8; L=r; cycles+=11; break; }
        case 0x2A: { const u8 lo=_fetch(),hi=_fetch(); const u16 a=static_cast<u16>((hi<<8)|lo); L=_read(a); H=_read(a+1); cycles+=16; break; }
        case 0x2B: { u16 v=static_cast<u16>((H<<8)|L)-1; H=v>>8; L=v; cycles+=6; break; }
        case 0x2C: L=_inc8(L); cycles+=4; break;
        case 0x2D: L=_dec8(L); cycles+=4; break;
        case 0x2E: L=_fetch(); cycles+=7; break;
        case 0x2F: A=~A; F=(F&(FS|FZ|FV|FC))|(A&(FY|FX))|FH|FN; cycles+=4; break;  // CPL
        // ── 0x30–0x3F ───────────────────────────────────────────────────────
        case 0x30: { const s8 d=static_cast<s8>(_fetch()); if(!(F&FC)){PC=static_cast<u16>(PC+d);cycles+=12;}else{cycles+=7;} break; }  // JR NC
        case 0x31: { const u8 lo=_fetch(),hi=_fetch(); SP=static_cast<u16>((hi<<8)|lo); cycles+=10; break; }
        case 0x32: { const u8 lo=_fetch(),hi=_fetch(); _write(static_cast<u16>((hi<<8)|lo),A); cycles+=13; break; }
        case 0x33: SP++; cycles+=6; break;
        case 0x34: { u8 v=_read(HL()); _write(HL(),_inc8(v)); cycles+=11; break; }
        case 0x35: { u8 v=_read(HL()); _write(HL(),_dec8(v)); cycles+=11; break; }
        case 0x36: { _write(HL(),_fetch()); cycles+=10; break; }
        case 0x37: F=(F&(FS|FZ|FV))|(A&(FY|FX))|FC; cycles+=4; break;  // SCF
        case 0x38: { const s8 d=static_cast<s8>(_fetch()); if(F&FC){PC=static_cast<u16>(PC+d);cycles+=12;}else{cycles+=7;} break; }  // JR C
        case 0x39: { u16 hl=static_cast<u16>((H<<8)|L); u16 r=_add16(hl,SP); H=r>>8; L=r; cycles+=11; break; }
        case 0x3A: { const u8 lo=_fetch(),hi=_fetch(); A=_read(static_cast<u16>((hi<<8)|lo)); cycles+=13; break; }
        case 0x3B: SP--; cycles+=6; break;
        case 0x3C: A=_inc8(A); cycles+=4; break;
        case 0x3D: A=_dec8(A); cycles+=4; break;
        case 0x3E: A=_fetch(); cycles+=7; break;
        case 0x3F: F=(F&(FS|FZ|FV))|(A&(FY|FX))|((F&FC)?FH:0u)|((F&FC)?0u:FC); cycles+=4; break;  // CCF
        // ── 0xC0–0xFF ───────────────────────────────────────────────────────
        // RET cc
        case 0xC0: case 0xC8: case 0xD0: case 0xD8: case 0xE0: case 0xE8: case 0xF0: case 0xF8:
            if (cc((op>>3)&7u)) { PC=_pop16(); cycles+=11; } else { cycles+=5; } break;
        // POP rr
        case 0xC1: { u16 v=_pop16(); B=v>>8; C=v; cycles+=10; break; }
        case 0xD1: { u16 v=_pop16(); D=v>>8; E=v; cycles+=10; break; }
        case 0xE1: { u16 v=_pop16(); H=v>>8; L=v; cycles+=10; break; }
        case 0xF1: { u16 v=_pop16(); A=v>>8; F=v; cycles+=10; break; }
        // JP cc,nn
        case 0xC2: case 0xCA: case 0xD2: case 0xDA: case 0xE2: case 0xEA: case 0xF2: case 0xFA: {
            const u8 lo=_fetch(),hi=_fetch();
            if (cc((op>>3)&7u)) PC=static_cast<u16>((hi<<8)|lo);
            cycles+=10; break;
        }
        case 0xC3: { const u8 lo=_fetch(),hi=_fetch(); PC=static_cast<u16>((hi<<8)|lo); cycles+=10; break; }  // JP nn
        // CALL cc,nn
        case 0xC4: case 0xCC: case 0xD4: case 0xDC: case 0xE4: case 0xEC: case 0xF4: case 0xFC: {
            const u8 lo=_fetch(),hi=_fetch();
            if (cc((op>>3)&7u)) { _push16(PC); PC=static_cast<u16>((hi<<8)|lo); cycles+=17; } else { cycles+=10; } break;
        }
        // PUSH rr
        case 0xC5: _push16(static_cast<u16>((B<<8)|C)); cycles+=11; break;
        case 0xD5: _push16(static_cast<u16>((D<<8)|E)); cycles+=11; break;
        case 0xE5: _push16(static_cast<u16>((H<<8)|L)); cycles+=11; break;
        case 0xF5: _push16(static_cast<u16>((A<<8)|F)); cycles+=11; break;
        // ALU with immediate
        case 0xC6: A=_add8(A,_fetch(),false); cycles+=7; break;
        case 0xCE: A=_add8(A,_fetch(),(F&FC)!=0); cycles+=7; break;
        case 0xD6: A=_sub8(A,_fetch(),false); cycles+=7; break;
        case 0xDE: A=_sub8(A,_fetch(),(F&FC)!=0); cycles+=7; break;
        case 0xE6: A=_and8(A,_fetch()); cycles+=7; break;
        case 0xEE: A=_xor8(A,_fetch()); cycles+=7; break;
        case 0xF6: A=_or8 (A,_fetch()); cycles+=7; break;
        case 0xFE: _cp8(A,_fetch()); cycles+=7; break;
        // RST
        case 0xC7: case 0xCF: case 0xD7: case 0xDF:
        case 0xE7: case 0xEF: case 0xF7: case 0xFF:
            _push16(PC); PC=static_cast<u16>(op & 0x38u); cycles+=11; break;
        case 0xC9: PC=_pop16(); cycles+=10; break;  // RET
        case 0xCD: { const u8 lo=_fetch(),hi=_fetch(); _push16(PC); PC=static_cast<u16>((hi<<8)|lo); cycles+=17; break; }  // CALL nn
        case 0xD3: { _outPort(_fetch(), A); cycles+=11; break; }  // OUT (n),A
        case 0xDB: { A=_inPort(_fetch()); cycles+=11; break; }    // IN A,(n)
        case 0xD9: {  // EXX
            u8 t;
            t=B; B=B_; B_=t; t=C; C=C_; C_=t;
            t=D; D=D_; D_=t; t=E; E=E_; E_=t;
            t=H; H=H_; H_=t; t=L; L=L_; L_=t;
            cycles+=4; break;
        }
        case 0xE3: {  // EX (SP),HL
            const u8 lo=_read(SP), hi=_read(static_cast<u16>(SP+1));
            _write(SP,L); _write(static_cast<u16>(SP+1),H);
            H=hi; L=lo; cycles+=19; break;
        }
        case 0xE9: PC=static_cast<u16>((H<<8)|L); cycles+=4; break;  // JP (HL)
        case 0xEB: { u8 t=D;D=H;H=t; t=E;E=L;L=t; cycles+=4; break; }  // EX DE,HL
        case 0xF3: IFF1=IFF2=0; cycles+=4; break;  // DI
        case 0xFB: IFF1=IFF2=1; cycles+=4; break;  // EI
        case 0xF9: SP=static_cast<u16>((H<<8)|L); cycles+=6; break;  // LD SP,HL
        default: cycles+=4; break;  // Unimplemented: treat as NOP
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// CB prefix: rotates, shifts, bit operations
// ─────────────────────────────────────────────────────────────────────────────
void GenZ80::_executeCB(u8 op) {
    auto HL  = [&]() -> u16 { return static_cast<u16>((H<<8)|L); };
    auto get = [&](u32 r) -> u8 {
        switch (r & 7u) {
            case 0:return B; case 1:return C; case 2:return D; case 3:return E;
            case 4:return H; case 5:return L; case 6:return _read(HL()); default:return A;
        }
    };
    auto set = [&](u32 r, u8 v) {
        switch (r & 7u) {
            case 0:B=v;break; case 1:C=v;break; case 2:D=v;break; case 3:E=v;break;
            case 4:H=v;break; case 5:L=v;break; case 6:_write(HL(),v);break; default:A=v;break;
        }
    };

    const u32 r   = op & 7u;
    const u32 bit = (op >> 3) & 7u;
    const u8  v   = get(r);
    const u32 cyc = (r == 6u) ? 15u : 8u;

    if (op >= 0xC0u) {
        // SET b,r
        set(r, v | (1u << bit)); cycles += cyc; return;
    }
    if (op >= 0x80u) {
        // RES b,r
        set(r, v & ~static_cast<u8>(1u << bit)); cycles += cyc; return;
    }
    if (op >= 0x40u) {
        // BIT b,r
        const u8 res = v & static_cast<u8>(1u << bit);
        F = (F & FC)
          | (res & FS) | (res ? 0u : FZ) | FH
          | (v & FY) | (v & FX)  // undocumented: from operand
          | (res ? 0u : FV);
        cycles += (r == 6u) ? 12u : 8u; return;
    }

    // Rotate / shift (op 0x00–0x3F)
    u8 res;
    switch ((op >> 3) & 7u) {
        case 0: {  // RLC
            const u8 c=(v>>7)&1u; res=(v<<1)|c;
            F=(res&FS)|(res?0u:FZ)|(res&FY)|(res&FX)|(zparity(res)?FV:0u)|(c?FC:0u); break;
        }
        case 1: {  // RRC
            const u8 c=v&1u; res=(v>>1)|(c<<7);
            F=(res&FS)|(res?0u:FZ)|(res&FY)|(res&FX)|(zparity(res)?FV:0u)|(c?FC:0u); break;
        }
        case 2: {  // RL
            const u8 cin=(F&FC)?1u:0u, cout=(v>>7)&1u; res=(v<<1)|cin;
            F=(res&FS)|(res?0u:FZ)|(res&FY)|(res&FX)|(zparity(res)?FV:0u)|(cout?FC:0u); break;
        }
        case 3: {  // RR
            const u8 cin=(F&FC)?1u:0u, cout=v&1u; res=(v>>1)|(cin<<7);
            F=(res&FS)|(res?0u:FZ)|(res&FY)|(res&FX)|(zparity(res)?FV:0u)|(cout?FC:0u); break;
        }
        case 4: {  // SLA
            const u8 c=(v>>7)&1u; res=v<<1;
            F=(res&FS)|(res?0u:FZ)|(res&FY)|(res&FX)|(zparity(res)?FV:0u)|(c?FC:0u); break;
        }
        case 5: {  // SRA (arithmetic: replicate sign bit)
            const u8 c=v&1u; res=static_cast<u8>((static_cast<s8>(v))>>1);
            F=(res&FS)|(res?0u:FZ)|(res&FY)|(res&FX)|(zparity(res)?FV:0u)|(c?FC:0u); break;
        }
        case 6: {  // SLL (undocumented — shifts left, inserts 1 in bit 0)
            const u8 c=(v>>7)&1u; res=(v<<1)|1u;
            F=(res&FS)|(res?0u:FZ)|(res&FY)|(res&FX)|(zparity(res)?FV:0u)|(c?FC:0u); break;
        }
        default: {  // SRL (logical: shift right, insert 0 in bit 7)
            const u8 c=v&1u; res=v>>1;
            F=(res&FS)|(res?0u:FZ)|(res&FY)|(res&FX)|(zparity(res)?FV:0u)|(c?FC:0u); break;
        }
    }
    set(r, res);
    cycles += cyc;
}

// ─────────────────────────────────────────────────────────────────────────────
// DD / FD prefix: shared indexed implementation
// Called by _executeDD (idx=IX) and _executeFD (idx=IY)
// ─────────────────────────────────────────────────────────────────────────────
static void execIndexed(GenZ80& z, u8 op, u16& idx) {
    // Register access for indexed instructions.
    // H/L (indices 4,5) map to IXH/IXL (or IYH/IYL).
    // (HL) (index 6) maps to (IX+d) with a displacement byte.
    const u8 ixh = static_cast<u8>(idx >> 8);
    const u8 ixl = static_cast<u8>(idx);

    auto get = [&](u32 r, u8 disp) -> u8 {
        switch (r & 7u) {
            case 0: return z.B; case 1: return z.C; case 2: return z.D; case 3: return z.E;
            case 4: return ixh; case 5: return ixl;
            case 6: return z._read(static_cast<u16>(static_cast<s16>(idx) + static_cast<s8>(disp)));
            default: return z.A;
        }
    };
    auto setR = [&](u32 r, u8 v, u8 disp) {
        switch (r & 7u) {
            case 0: z.B=v; break; case 1: z.C=v; break;
            case 2: z.D=v; break; case 3: z.E=v; break;
            case 4: idx=(idx&0x00FFu)|(static_cast<u16>(v)<<8); break;  // IXH/IYH
            case 5: idx=(idx&0xFF00u)|v; break;                          // IXL/IYL
            case 6: z._write(static_cast<u16>(static_cast<s16>(idx)+static_cast<s8>(disp)),v); break;
            default: z.A=v; break;
        }
    };

    // DDCB/FDCB sub-prefix
    if (op == 0xCB) {
        const s8  d   = static_cast<s8>(z._fetch());
        const u8  sub = z._fetch();
        const u16 ea  = static_cast<u16>(static_cast<s16>(idx) + d);
        z._executeDDCB(ea, sub);
        return;
    }

    // Dense LD r,r block (using IX replacement)
    if (op >= 0x40u && op < 0x80u && op != 0x76u) {
        const u8 src = get(op & 7u, 0);
        const u32 dst = (op >> 3) & 7u;
        // Only fire if src or dst touches IXH/IXL/indirect
        setR(dst, src, 0);
        z.cycles += ((op & 7u) == 6u || dst == 6u) ? 19u : 8u;
        return;
    }

    if (op >= 0x80u && op < 0xC0u) {
        const u8 disp = ((op & 7u) == 6u) ? z._fetch() : 0u;
        const u8 v    = get(op & 7u, disp);
        const u32 cyc = (op & 7u) == 6u ? 19u : 8u;
        switch ((op >> 3) & 7u) {
            case 0: z.A=z._add8(z.A,v,false); break;
            case 1: z.A=z._add8(z.A,v,(z.F&FC)!=0); break;
            case 2: z.A=z._sub8(z.A,v,false); break;
            case 3: z.A=z._sub8(z.A,v,(z.F&FC)!=0); break;
            case 4: z.A=z._and8(z.A,v); break;
            case 5: z.A=z._xor8(z.A,v); break;
            case 6: z.A=z._or8 (z.A,v); break;
            case 7: z._cp8(z.A,v); break;
        }
        z.cycles += cyc; return;
    }

    switch (op) {
        case 0x21: { const u8 lo=z._fetch(),hi=z._fetch(); idx=static_cast<u16>((hi<<8)|lo); z.cycles+=14; break; }
        case 0x22: { const u8 lo=z._fetch(),hi=z._fetch(); const u16 a=static_cast<u16>((hi<<8)|lo); z._write(a,static_cast<u8>(idx)); z._write(a+1,static_cast<u8>(idx>>8)); z.cycles+=20; break; }
        case 0x23: idx++; z.cycles+=10; break;
        case 0x24: { u8 h=static_cast<u8>(idx>>8); h=z._inc8(h); idx=(idx&0x00FFu)|(static_cast<u16>(h)<<8); z.cycles+=8; break; }  // INC IXH
        case 0x25: { u8 h=static_cast<u8>(idx>>8); h=z._dec8(h); idx=(idx&0x00FFu)|(static_cast<u16>(h)<<8); z.cycles+=8; break; }  // DEC IXH
        case 0x26: { idx=(idx&0x00FFu)|(static_cast<u16>(z._fetch())<<8); z.cycles+=11; break; }  // LD IXH,n
        case 0x29: { idx=z._add16(idx,idx); z.cycles+=15; break; }  // ADD IX,IX
        case 0x2A: { const u8 lo=z._fetch(),hi=z._fetch(); const u16 a=static_cast<u16>((hi<<8)|lo); idx=static_cast<u16>(z._read(a)|(z._read(a+1)<<8)); z.cycles+=20; break; }
        case 0x2B: idx--; z.cycles+=10; break;
        case 0x2C: { u8 l=static_cast<u8>(idx); l=z._inc8(l); idx=(idx&0xFF00u)|l; z.cycles+=8; break; }  // INC IXL
        case 0x2D: { u8 l=static_cast<u8>(idx); l=z._dec8(l); idx=(idx&0xFF00u)|l; z.cycles+=8; break; }  // DEC IXL
        case 0x2E: idx=(idx&0xFF00u)|z._fetch(); z.cycles+=11; break;  // LD IXL,n
        case 0x34: { const s8 d=static_cast<s8>(z._fetch()); const u16 a=static_cast<u16>(static_cast<s16>(idx)+d); z._write(a,z._inc8(z._read(a))); z.cycles+=23; break; }
        case 0x35: { const s8 d=static_cast<s8>(z._fetch()); const u16 a=static_cast<u16>(static_cast<s16>(idx)+d); z._write(a,z._dec8(z._read(a))); z.cycles+=23; break; }
        case 0x36: { const s8 d=static_cast<s8>(z._fetch()); const u8 n=z._fetch(); z._write(static_cast<u16>(static_cast<s16>(idx)+d),n); z.cycles+=19; break; }
        // LD r,(IX+d) — 0x46,0x4E,...,0x7E
        case 0x46: z.B=z._read(static_cast<u16>(static_cast<s16>(idx)+static_cast<s8>(z._fetch()))); z.cycles+=19; break;
        case 0x4E: z.C=z._read(static_cast<u16>(static_cast<s16>(idx)+static_cast<s8>(z._fetch()))); z.cycles+=19; break;
        case 0x56: z.D=z._read(static_cast<u16>(static_cast<s16>(idx)+static_cast<s8>(z._fetch()))); z.cycles+=19; break;
        case 0x5E: z.E=z._read(static_cast<u16>(static_cast<s16>(idx)+static_cast<s8>(z._fetch()))); z.cycles+=19; break;
        case 0x66: { const u8 v=z._read(static_cast<u16>(static_cast<s16>(idx)+static_cast<s8>(z._fetch()))); idx=(idx&0x00FFu)|(static_cast<u16>(v)<<8); z.cycles+=19; break; }  // LD H,(IX+d)
        case 0x6E: { const u8 v=z._read(static_cast<u16>(static_cast<s16>(idx)+static_cast<s8>(z._fetch()))); idx=(idx&0xFF00u)|v; z.cycles+=19; break; }  // LD L,(IX+d)
        case 0x7E: z.A=z._read(static_cast<u16>(static_cast<s16>(idx)+static_cast<s8>(z._fetch()))); z.cycles+=19; break;
        // LD (IX+d),r
        case 0x70: z._write(static_cast<u16>(static_cast<s16>(idx)+static_cast<s8>(z._fetch())),z.B); z.cycles+=19; break;
        case 0x71: z._write(static_cast<u16>(static_cast<s16>(idx)+static_cast<s8>(z._fetch())),z.C); z.cycles+=19; break;
        case 0x72: z._write(static_cast<u16>(static_cast<s16>(idx)+static_cast<s8>(z._fetch())),z.D); z.cycles+=19; break;
        case 0x73: z._write(static_cast<u16>(static_cast<s16>(idx)+static_cast<s8>(z._fetch())),z.E); z.cycles+=19; break;
        case 0x74: z._write(static_cast<u16>(static_cast<s16>(idx)+static_cast<s8>(z._fetch())),static_cast<u8>(idx>>8)); z.cycles+=19; break;  // H
        case 0x75: z._write(static_cast<u16>(static_cast<s16>(idx)+static_cast<s8>(z._fetch())),static_cast<u8>(idx)); z.cycles+=19; break;    // L
        case 0x77: z._write(static_cast<u16>(static_cast<s16>(idx)+static_cast<s8>(z._fetch())),z.A); z.cycles+=19; break;
        // ADD IX, rr
        case 0x09: idx=z._add16(idx,static_cast<u16>((z.B<<8)|z.C)); z.cycles+=15; break;
        case 0x19: idx=z._add16(idx,static_cast<u16>((z.D<<8)|z.E)); z.cycles+=15; break;
        case 0x39: idx=z._add16(idx,z.SP); z.cycles+=15; break;
        // PUSH/POP IX
        case 0xE1: idx=z._pop16(); z.cycles+=14; break;
        case 0xE5: z._push16(idx); z.cycles+=15; break;
        case 0xE3: {  // EX (SP),IX
            const u16 tmp=z._pop16(); z._push16(idx); idx=tmp; z.cycles+=23; break;
        }
        case 0xE9: z.PC=idx; z.cycles+=8; break;  // JP (IX)
        case 0xF9: z.SP=idx; z.cycles+=10; break;  // LD SP,IX
        case 0x44: z.B=static_cast<u8>(idx>>8); z.cycles+=8; break;  // LD B,IXH
        case 0x45: z.B=static_cast<u8>(idx);    z.cycles+=8; break;  // LD B,IXL
        case 0x4C: z.C=static_cast<u8>(idx>>8); z.cycles+=8; break;
        case 0x4D: z.C=static_cast<u8>(idx);    z.cycles+=8; break;
        case 0x54: z.D=static_cast<u8>(idx>>8); z.cycles+=8; break;
        case 0x55: z.D=static_cast<u8>(idx);    z.cycles+=8; break;
        case 0x5C: z.E=static_cast<u8>(idx>>8); z.cycles+=8; break;
        case 0x5D: z.E=static_cast<u8>(idx);    z.cycles+=8; break;
        case 0x7C: z.A=static_cast<u8>(idx>>8); z.cycles+=8; break;  // LD A,IXH
        case 0x7D: z.A=static_cast<u8>(idx);    z.cycles+=8; break;  // LD A,IXL
        case 0x64: idx=(idx&0x00FFu)|(static_cast<u16>(z.H)<<8); z.cycles+=8; break;  // LD IXH,H
        case 0x65: idx=(idx&0x00FFu)|(static_cast<u16>(z.L)<<8); z.cycles+=8; break;  // LD IXH,L (undoc)
        case 0x6C: idx=(idx&0xFF00u)|z.H; z.cycles+=8; break;
        case 0x6D: idx=(idx&0xFF00u)|z.L; z.cycles+=8; break;
        case 0x67: idx=(idx&0x00FFu)|(static_cast<u16>(z.A)<<8); z.cycles+=8; break;  // LD IXH,A
        case 0x6F: idx=(idx&0xFF00u)|z.A; z.cycles+=8; break;                           // LD IXL,A
        // ALU with IXH/IXL
        case 0x84: z.A=z._add8(z.A,static_cast<u8>(idx>>8),false); z.cycles+=8; break;
        case 0x85: z.A=z._add8(z.A,static_cast<u8>(idx),   false); z.cycles+=8; break;
        case 0x8C: z.A=z._add8(z.A,static_cast<u8>(idx>>8),(z.F&FC)!=0); z.cycles+=8; break;
        case 0x8D: z.A=z._add8(z.A,static_cast<u8>(idx),   (z.F&FC)!=0); z.cycles+=8; break;
        case 0x94: z.A=z._sub8(z.A,static_cast<u8>(idx>>8),false); z.cycles+=8; break;
        case 0x95: z.A=z._sub8(z.A,static_cast<u8>(idx),   false); z.cycles+=8; break;
        case 0x9C: z.A=z._sub8(z.A,static_cast<u8>(idx>>8),(z.F&FC)!=0); z.cycles+=8; break;
        case 0x9D: z.A=z._sub8(z.A,static_cast<u8>(idx),   (z.F&FC)!=0); z.cycles+=8; break;
        case 0xA4: z.A=z._and8(z.A,static_cast<u8>(idx>>8)); z.cycles+=8; break;
        case 0xA5: z.A=z._and8(z.A,static_cast<u8>(idx));    z.cycles+=8; break;
        case 0xAC: z.A=z._xor8(z.A,static_cast<u8>(idx>>8)); z.cycles+=8; break;
        case 0xAD: z.A=z._xor8(z.A,static_cast<u8>(idx));    z.cycles+=8; break;
        case 0xB4: z.A=z._or8 (z.A,static_cast<u8>(idx>>8)); z.cycles+=8; break;
        case 0xB5: z.A=z._or8 (z.A,static_cast<u8>(idx));    z.cycles+=8; break;
        case 0xBC: z._cp8(z.A,static_cast<u8>(idx>>8)); z.cycles+=8; break;
        case 0xBD: z._cp8(z.A,static_cast<u8>(idx));    z.cycles+=8; break;
        case 0x86: { const s8 d=static_cast<s8>(z._fetch()); z.A=z._add8(z.A,z._read(static_cast<u16>(static_cast<s16>(idx)+d)),false); z.cycles+=19; break; }
        case 0x8E: { const s8 d=static_cast<s8>(z._fetch()); z.A=z._add8(z.A,z._read(static_cast<u16>(static_cast<s16>(idx)+d)),(z.F&FC)!=0); z.cycles+=19; break; }
        case 0x96: { const s8 d=static_cast<s8>(z._fetch()); z.A=z._sub8(z.A,z._read(static_cast<u16>(static_cast<s16>(idx)+d)),false); z.cycles+=19; break; }
        case 0x9E: { const s8 d=static_cast<s8>(z._fetch()); z.A=z._sub8(z.A,z._read(static_cast<u16>(static_cast<s16>(idx)+d)),(z.F&FC)!=0); z.cycles+=19; break; }
        case 0xA6: { const s8 d=static_cast<s8>(z._fetch()); z.A=z._and8(z.A,z._read(static_cast<u16>(static_cast<s16>(idx)+d))); z.cycles+=19; break; }
        case 0xAE: { const s8 d=static_cast<s8>(z._fetch()); z.A=z._xor8(z.A,z._read(static_cast<u16>(static_cast<s16>(idx)+d))); z.cycles+=19; break; }
        case 0xB6: { const s8 d=static_cast<s8>(z._fetch()); z.A=z._or8 (z.A,z._read(static_cast<u16>(static_cast<s16>(idx)+d))); z.cycles+=19; break; }
        case 0xBE: { const s8 d=static_cast<s8>(z._fetch()); z._cp8(z.A,z._read(static_cast<u16>(static_cast<s16>(idx)+d))); z.cycles+=19; break; }
        default: z._execute(op); break;  // fall back to main for unmapped
    }
}

// DDCB / FDCB: indexed bit operations — ea is already IX+d or IY+d
static void execIndexedCB(GenZ80& z, u16 ea, u8 op) {
    const u8  v   = z._read(ea);
    const u32 bit = (op >> 3) & 7u;

    if (op >= 0xC0u) {
        // SET
        z._write(ea, v | static_cast<u8>(1u << bit)); z.cycles += 23; return;
    }
    if (op >= 0x80u) {
        // RES
        z._write(ea, v & ~static_cast<u8>(1u << bit)); z.cycles += 23; return;
    }
    if (op >= 0x40u) {
        // BIT
        const u8 res = v & static_cast<u8>(1u << bit);
        z.F = (z.F & FC) | (res & FS) | (res ? 0u : FZ | FV) | FH;
        z.cycles += 20; return;
    }
    // Rotate / shift
    u8 res;
    switch ((op >> 3) & 7u) {
        case 0: { const u8 c=(v>>7)&1u; res=(v<<1)|c; z.F=(res&FS)|(res?0u:FZ)|(res&FY)|(res&FX)|(zparity(res)?FV:0u)|(c?FC:0u); break; }
        case 1: { const u8 c=v&1u; res=(v>>1)|(c<<7); z.F=(res&FS)|(res?0u:FZ)|(res&FY)|(res&FX)|(zparity(res)?FV:0u)|(c?FC:0u); break; }
        case 2: { const u8 ci=(z.F&FC)?1u:0u,co=(v>>7)&1u; res=(v<<1)|ci; z.F=(res&FS)|(res?0u:FZ)|(res&FY)|(res&FX)|(zparity(res)?FV:0u)|(co?FC:0u); break; }
        case 3: { const u8 ci=(z.F&FC)?1u:0u,co=v&1u; res=(v>>1)|(ci<<7); z.F=(res&FS)|(res?0u:FZ)|(res&FY)|(res&FX)|(zparity(res)?FV:0u)|(co?FC:0u); break; }
        case 4: { const u8 c=(v>>7)&1u; res=v<<1; z.F=(res&FS)|(res?0u:FZ)|(res&FY)|(res&FX)|(zparity(res)?FV:0u)|(c?FC:0u); break; }
        case 5: { const u8 c=v&1u; res=static_cast<u8>((static_cast<s8>(v))>>1); z.F=(res&FS)|(res?0u:FZ)|(res&FY)|(res&FX)|(zparity(res)?FV:0u)|(c?FC:0u); break; }
        case 6: { const u8 c=(v>>7)&1u; res=(v<<1)|1u; z.F=(res&FS)|(res?0u:FZ)|(res&FY)|(res&FX)|(zparity(res)?FV:0u)|(c?FC:0u); break; }
        default:{ const u8 c=v&1u; res=v>>1; z.F=(res&FS)|(res?0u:FZ)|(res&FY)|(res&FX)|(zparity(res)?FV:0u)|(c?FC:0u); break; }
    }
    z._write(ea, res);
    // For DDCB/FDCB rotates, copy result to named register if bits 2:0 != 6
    if ((op & 7u) != 6u) {
        switch (op & 7u) {
            case 0: z.B=res; break; case 1: z.C=res; break; case 2: z.D=res; break; case 3: z.E=res; break;
            case 4: z.H=res; break; case 5: z.L=res; break; default: z.A=res; break;
        }
    }
    z.cycles += 23;
}

void GenZ80::_executeDD(u8 op) { execIndexed(*this, op, IX); }
void GenZ80::_executeFD(u8 op) { execIndexed(*this, op, IY); }
void GenZ80::_executeDDCB(u16 addr, u8 op) { execIndexedCB(*this, addr, op); }
void GenZ80::_executeFDCB(u16 addr, u8 op) { execIndexedCB(*this, addr, op); }

// ─────────────────────────────────────────────────────────────────────────────
// ED prefix: extended instructions
// ─────────────────────────────────────────────────────────────────────────────
void GenZ80::_executeED(u8 op) {
    auto HL  = [&]() -> u16 { return static_cast<u16>((H<<8)|L); };

    // IN r,(C) / OUT (C),r   0x40–0x79
    if ((op & 0xC7u) == 0x40u) {
        const u32 r = (op >> 3) & 7u;
        const u8  v = _inPort(C);
        if (r != 6u) {
            switch (r) {
                case 0:B=v;break; case 1:C=v;break; case 2:D=v;break; case 3:E=v;break;
                case 4:H=v;break; case 5:L=v;break; default:A=v;break;
            }
        }
        F = (v & FS) | (v ? 0u : FZ) | (v & FY) | (v & FX)
          | (zparity(v) ? FV : 0u) | (F & FC);
        cycles += 12; return;
    }
    if ((op & 0xC7u) == 0x41u) {
        u8 v;
        switch ((op>>3)&7u) {
            case 0:v=B;break; case 1:v=C;break; case 2:v=D;break; case 3:v=E;break;
            case 4:v=H;break; case 5:v=L;break; case 6:v=0;break; default:v=A;break;
        }
        _outPort(C, v); cycles += 12; return;
    }

    switch (op) {
        case 0x42: { const u16 bc=static_cast<u16>((B<<8)|C); const u16 r=_sbc16(static_cast<u16>((H<<8)|L),bc); H=r>>8; L=r; cycles+=15; break; }
        case 0x43: { const u8 lo=_fetch(),hi=_fetch(); const u16 a=static_cast<u16>((hi<<8)|lo); _write(a,C); _write(a+1,B); cycles+=20; break; }
        case 0x44: case 0x4C: case 0x54: case 0x5C: case 0x64: case 0x6C: case 0x74: case 0x7C:
            A=_sub8(0,A,false); cycles+=8; break;  // NEG
        case 0x45: case 0x55: case 0x5D: case 0x65: case 0x6D: case 0x75: case 0x7D:
            // RETN: restore IFF1 from IFF2 and return
            IFF1=IFF2; PC=_pop16(); cycles+=14; break;
        case 0x46: case 0x4E: case 0x66: case 0x6E: IM=0; cycles+=8; break;
        case 0x47: I=A; cycles+=9; break;   // LD I,A
        case 0x4A: { const u16 bc=static_cast<u16>((B<<8)|C); const u16 r=_adc16(static_cast<u16>((H<<8)|L),bc); H=r>>8; L=r; cycles+=15; break; }
        case 0x4B: { const u8 lo=_fetch(),hi=_fetch(); const u16 a=static_cast<u16>((hi<<8)|lo); C=_read(a); B=_read(a+1); cycles+=20; break; }
        case 0x4D: IFF1=IFF2; PC=_pop16(); cycles+=14; break;  // RETI
        case 0x4F: R=A; cycles+=9; break;   // LD R,A
        case 0x52: { const u16 de=static_cast<u16>((D<<8)|E); const u16 r=_sbc16(static_cast<u16>((H<<8)|L),de); H=r>>8; L=r; cycles+=15; break; }
        case 0x53: { const u8 lo=_fetch(),hi=_fetch(); const u16 a=static_cast<u16>((hi<<8)|lo); _write(a,E); _write(a+1,D); cycles+=20; break; }
        case 0x56: case 0x76: IM=1; cycles+=8; break;
        case 0x57: {  // LD A,I
            A=I; F=(I&FS)|(I?0u:FZ)|(I&FY)|(I&FX)|(IFF2?FV:0u)|(F&FC);
            cycles+=9; break;
        }
        case 0x5A: { const u16 de=static_cast<u16>((D<<8)|E); const u16 r=_adc16(static_cast<u16>((H<<8)|L),de); H=r>>8; L=r; cycles+=15; break; }
        case 0x5B: { const u8 lo=_fetch(),hi=_fetch(); const u16 a=static_cast<u16>((hi<<8)|lo); E=_read(a); D=_read(a+1); cycles+=20; break; }
        case 0x5E: case 0x7E: IM=2; cycles+=8; break;
        case 0x5F: {  // LD A,R
            A=R; F=(R&FS)|(R?0u:FZ)|(R&FY)|(R&FX)|(IFF2?FV:0u)|(F&FC);
            cycles+=9; break;
        }
        case 0x62: { const u16 r=_sbc16(static_cast<u16>((H<<8)|L),static_cast<u16>((H<<8)|L)); H=r>>8; L=r; cycles+=15; break; }
        case 0x63: { const u8 lo=_fetch(),hi=_fetch(); const u16 a=static_cast<u16>((hi<<8)|lo); _write(a,L); _write(a+1,H); cycles+=20; break; }
        case 0x67: {  // RRD
            const u8 m=_read(HL()); _write(HL(),(m>>4)|(A<<4)); A=(A&0xF0u)|(m&0x0Fu);
            F=(A&FS)|(A?0u:FZ)|(A&FY)|(A&FX)|(zparity(A)?FV:0u)|(F&FC); cycles+=18; break;
        }
        case 0x6A: { const u16 r=_adc16(static_cast<u16>((H<<8)|L),static_cast<u16>((H<<8)|L)); H=r>>8; L=r; cycles+=15; break; }
        case 0x6B: { const u8 lo=_fetch(),hi=_fetch(); const u16 a=static_cast<u16>((hi<<8)|lo); L=_read(a); H=_read(a+1); cycles+=20; break; }
        case 0x6F: {  // RLD
            const u8 m=_read(HL()); _write(HL(),(m<<4)|(A&0x0Fu)); A=(A&0xF0u)|(m>>4);
            F=(A&FS)|(A?0u:FZ)|(A&FY)|(A&FX)|(zparity(A)?FV:0u)|(F&FC); cycles+=18; break;
        }
        case 0x72: { const u16 r=_sbc16(static_cast<u16>((H<<8)|L),SP); H=r>>8; L=r; cycles+=15; break; }
        case 0x73: { const u8 lo=_fetch(),hi=_fetch(); const u16 a=static_cast<u16>((hi<<8)|lo); _write(a,static_cast<u8>(SP)); _write(a+1,static_cast<u8>(SP>>8)); cycles+=20; break; }
        case 0x7A: { const u16 r=_adc16(static_cast<u16>((H<<8)|L),SP); H=r>>8; L=r; cycles+=15; break; }
        case 0x7B: { const u8 lo=_fetch(),hi=_fetch(); const u16 a=static_cast<u16>((hi<<8)|lo); SP=static_cast<u16>(_read(a)|(_read(a+1)<<8)); cycles+=20; break; }
        // Block instructions
        case 0xA0: {  // LDI
            const u8 v=_read(HL()); _write(static_cast<u16>((D<<8)|E),v);
            { u16 hl=static_cast<u16>((H<<8)|L)+1; H=hl>>8; L=hl; }
            { u16 de=static_cast<u16>((D<<8)|E)+1; D=de>>8; E=de; }
            { u16 bc=static_cast<u16>((B<<8)|C)-1; B=bc>>8; C=bc; }
            F=(F&(FS|FZ|FC))|(((A+v)&0x02u)?FY:0u)|(((A+v)&0x08u)?FX:0u)|((B||C)?FV:0u);
            cycles+=16; break;
        }
        case 0xA8: {  // LDD
            const u8 v=_read(HL()); _write(static_cast<u16>((D<<8)|E),v);
            { u16 hl=static_cast<u16>((H<<8)|L)-1; H=hl>>8; L=hl; }
            { u16 de=static_cast<u16>((D<<8)|E)-1; D=de>>8; E=de; }
            { u16 bc=static_cast<u16>((B<<8)|C)-1; B=bc>>8; C=bc; }
            F=(F&(FS|FZ|FC))|(((A+v)&0x02u)?FY:0u)|(((A+v)&0x08u)?FX:0u)|((B||C)?FV:0u);
            cycles+=16; break;
        }
        case 0xB0: {  // LDIR
            while (true) {
                const u8 v=_read(HL()); _write(static_cast<u16>((D<<8)|E),v);
                { u16 hl=static_cast<u16>((H<<8)|L)+1; H=hl>>8; L=hl; }
                { u16 de=static_cast<u16>((D<<8)|E)+1; D=de>>8; E=de; }
                { u16 bc=static_cast<u16>((B<<8)|C)-1; B=bc>>8; C=bc; }
                cycles += 21;
                if (!B && !C) break;
                // Yield every 64 bytes to avoid infinite loops under cycle limit
                if (!(C & 0x3Fu) && !B) break;
            }
            if (!B && !C) { cycles -= 5; F=(F&(FS|FZ|FC)); }
            else           { F=(F&(FS|FZ|FC))|FV; PC-=2; }
            break;
        }
        case 0xB8: {  // LDDR
            while (true) {
                const u8 v=_read(HL()); _write(static_cast<u16>((D<<8)|E),v);
                { u16 hl=static_cast<u16>((H<<8)|L)-1; H=hl>>8; L=hl; }
                { u16 de=static_cast<u16>((D<<8)|E)-1; D=de>>8; E=de; }
                { u16 bc=static_cast<u16>((B<<8)|C)-1; B=bc>>8; C=bc; }
                cycles += 21;
                if (!B && !C) break;
            }
            if (!B && !C) { cycles -= 5; F=(F&(FS|FZ|FC)); }
            break;
        }
        case 0xA1: {  // CPI
            const u8 v=_read(HL()); const u8 r=A-v;
            { u16 hl=static_cast<u16>((H<<8)|L)+1; H=hl>>8; L=hl; }
            { u16 bc=static_cast<u16>((B<<8)|C)-1; B=bc>>8; C=bc; }
            const u8 n=r-((F&FH)?1u:0u);
            F=(r&FS)|(r?0u:FZ)|((A&0xFu)<(v&0xFu)?FH:0u)
             |(n&FX)|((n>>1)&FY)|FN|((B||C)?FV:0u)|(F&FC);
            cycles+=16; break;
        }
        case 0xB1: {  // CPIR
            while (true) {
                const u8 v=_read(HL()); const u8 r=A-v;
                { u16 hl=static_cast<u16>((H<<8)|L)+1; H=hl>>8; L=hl; }
                { u16 bc=static_cast<u16>((B<<8)|C)-1; B=bc>>8; C=bc; }
                cycles+=21;
                if (!r || (!B && !C)) break;
            }
            if (B||C) PC-=2;
            F=(F&FC)|FN; cycles -= (B||C)?0:5; break;
        }
        case 0xA3: {  // OUTI
            const u8 v=_read(HL()); _outPort(C,v);
            { u16 hl=static_cast<u16>((H<<8)|L)+1; H=hl>>8; L=hl; }
            B=_dec8(B); cycles+=16; break;
        }
        case 0xAB: {  // OUTD
            const u8 v=_read(HL()); _outPort(C,v);
            { u16 hl=static_cast<u16>((H<<8)|L)-1; H=hl>>8; L=hl; }
            B=_dec8(B); cycles+=16; break;
        }
        case 0xB3: {  // OTIR
            do {
                const u8 v=_read(HL()); _outPort(C,v);
                { u16 hl=static_cast<u16>((H<<8)|L)+1; H=hl>>8; L=hl; }
                B=_dec8(B); cycles+=21;
            } while (B);
            cycles-=5; break;
        }
        case 0xBB: {  // OTDR
            do {
                const u8 v=_read(HL()); _outPort(C,v);
                { u16 hl=static_cast<u16>((H<<8)|L)-1; H=hl>>8; L=hl; }
                B=_dec8(B); cycles+=21;
            } while (B);
            cycles-=5; break;
        }
        case 0xA2: {  // INI
            const u8 v=_inPort(C); _write(HL(),v);
            { u16 hl=static_cast<u16>((H<<8)|L)+1; H=hl>>8; L=hl; }
            B=_dec8(B); cycles+=16; break;
        }
        case 0xAA: {  // IND
            const u8 v=_inPort(C); _write(HL(),v);
            { u16 hl=static_cast<u16>((H<<8)|L)-1; H=hl>>8; L=hl; }
            B=_dec8(B); cycles+=16; break;
        }
        case 0xB2: {  // INIR
            do { const u8 v=_inPort(C); _write(HL(),v); { u16 hl=static_cast<u16>((H<<8)|L)+1; H=hl>>8; L=hl; } B=_dec8(B); cycles+=21; } while(B);
            cycles-=5; break;
        }
        case 0xBA: {  // INDR
            do { const u8 v=_inPort(C); _write(HL(),v); { u16 hl=static_cast<u16>((H<<8)|L)-1; H=hl>>8; L=hl; } B=_dec8(B); cycles+=21; } while(B);
            cycles-=5; break;
        }
        case 0xA9: {  // CPD
            const u8 v=_read(HL()); const u8 r=A-v;
            { u16 hl=static_cast<u16>((H<<8)|L)-1; H=hl>>8; L=hl; }
            { u16 bc=static_cast<u16>((B<<8)|C)-1; B=bc>>8; C=bc; }
            F=(r&FS)|(r?0u:FZ)|((A&0xFu)<(v&0xFu)?FH:0u)|FN|((B||C)?FV:0u)|(F&FC);
            cycles+=16; break;
        }
        case 0xB9: {  // CPDR
            while (true) {
                const u8 v=_read(HL()); const u8 r=A-v;
                { u16 hl=static_cast<u16>((H<<8)|L)-1; H=hl>>8; L=hl; }
                { u16 bc=static_cast<u16>((B<<8)|C)-1; B=bc>>8; C=bc; }
                cycles+=21;
                if (!r || (!B && !C)) break;
            }
            if (B||C) PC-=2;
            F=(F&FC)|FN; cycles-=(B||C)?0:5; break;
        }
        default: cycles+=8; break;  // Unknown ED: two-byte NOP
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Save / load state
// ─────────────────────────────────────────────────────────────────────────────
void GenZ80::saveState(struct DingSaveWriter* w) {
    struct Z80State {
        u8  A,F,B,C,D,E,H,L;
        u8  A_,F_,B_,C_,D_,E_,H_,L_;
        u16 IX,IY,SP,PC;
        u8  IFF1,IFF2,IM,I,R;
        u8  halted;
    } st;
    st.A=A; st.F=F; st.B=B; st.C=C; st.D=D; st.E=E; st.H=H; st.L=L;
    st.A_=A_; st.F_=F_; st.B_=B_; st.C_=C_; st.D_=D_; st.E_=E_; st.H_=H_; st.L_=L_;
    st.IX=IX; st.IY=IY; st.SP=SP; st.PC=PC;
    st.IFF1=IFF1; st.IFF2=IFF2; st.IM=IM; st.I=I; st.R=R;
    st.halted=halted?1u:0u;
    ding_save_write_block(w, "Z80_REGS", &st, sizeof(st));
    ding_save_write_block(w, "Z80_RAM",  ram, GEN_Z80RAM_SIZE);
}

void GenZ80::loadState(struct DingSaveReader* r) {
    struct Z80State {
        u8  A,F,B,C,D,E,H,L;
        u8  A_,F_,B_,C_,D_,E_,H_,L_;
        u16 IX,IY,SP,PC;
        u8  IFF1,IFF2,IM,I,R;
        u8  halted;
    } st;
    ding_save_read_block(r, "Z80_REGS", &st, sizeof(st), nullptr);
    ding_save_read_block(r, "Z80_RAM",  ram, GEN_Z80RAM_SIZE, nullptr);
    A=st.A; F=st.F; B=st.B; C=st.C; D=st.D; E=st.E; H=st.H; L=st.L;
    A_=st.A_; F_=st.F_; B_=st.B_; C_=st.C_; D_=st.D_; E_=st.E_; H_=st.H_; L_=st.L_;
    IX=st.IX; IY=st.IY; SP=st.SP; PC=st.PC;
    IFF1=st.IFF1; IFF2=st.IFF2; IM=st.IM; I=st.I; R=st.R;
    halted=st.halted!=0;
}
