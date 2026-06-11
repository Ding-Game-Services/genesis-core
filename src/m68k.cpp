#include "genesis.h"
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// Local helper
// ─────────────────────────────────────────────────────────────────────────────
static inline u32 popcount32(u32 n) {
    u32 c = 0;
    while (n) { c += n & 1u; n >>= 1; }
    return c;
}

// ─────────────────────────────────────────────────────────────────────────────
// Constructor / reset
// ─────────────────────────────────────────────────────────────────────────────
M68K::M68K(GenBus* b) : bus(b) {
    std::memset(d, 0, sizeof(d));
    std::memset(a, 0, sizeof(a));
    pc = 0; sr = 0x2700; stopped = false; usp = 0; cycles = 0;
}

void M68K::reset() {
    std::memset(d, 0, sizeof(d));
    std::memset(a, 0, sizeof(a));
    a[7]    = bus->read32(0);   // SSP from vector table
    pc      = bus->read32(4);   // Initial PC from vector table
    sr      = 0x2700;           // Supervisor, IPL=7
    stopped = false;
    usp     = 0;
    cycles  = 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// Instruction fetch
// ─────────────────────────────────────────────────────────────────────────────
u16 M68K::fetch16() {
    const u16 v = bus->read16(pc);
    pc += 2;
    return v;
}

u32 M68K::fetch32() {
    const u32 v = bus->read32(pc);
    pc += 4;
    return v;
}

// ─────────────────────────────────────────────────────────────────────────────
// Register read/write (size-aware)
// sz: 0=byte  1=word  2=long
// ─────────────────────────────────────────────────────────────────────────────
u32 M68K::readDn(u32 n, u32 sz) {
    if (sz == 0) return d[n] & 0xFFu;
    if (sz == 1) return d[n] & 0xFFFFu;
    return d[n];
}

void M68K::writeDn(u32 n, u32 v, u32 sz) {
    if      (sz == 0) d[n] = (d[n] & 0xFFFFFF00u) | (v & 0xFFu);
    else if (sz == 1) d[n] = (d[n] & 0xFFFF0000u) | (v & 0xFFFFu);
    else              d[n] = v;
}

// ─────────────────────────────────────────────────────────────────────────────
// Size masks — fills mask (all bits for sz) and msb (sign bit for sz)
// ─────────────────────────────────────────────────────────────────────────────
void M68K::_masks(u32 sz, u32& mask, u32& msb) {
    if      (sz == 0) { mask = 0xFFu;       msb = 0x80u;       }
    else if (sz == 1) { mask = 0xFFFFu;     msb = 0x8000u;     }
    else              { mask = 0xFFFFFFFFu; msb = 0x80000000u; }
}

// ─────────────────────────────────────────────────────────────────────────────
// Effective address calculation
// Advances PC through any extension words.
// ─────────────────────────────────────────────────────────────────────────────
u32 M68K::calcEA(u32 mode, u32 reg, u32 sz) {
    switch (mode) {
        case 2: return a[reg];                                      // (An)
        case 3: {                                                    // (An)+
            const u32 addr = a[reg];
            a[reg] += (sz == 0) ? (reg == 7 ? 2u : 1u)
                    : (sz == 1) ? 2u : 4u;
            return addr;
        }
        case 4: {                                                    // -(An)
            a[reg] -= (sz == 0) ? (reg == 7 ? 2u : 1u)
                    : (sz == 1) ? 2u : 4u;
            return a[reg];
        }
        case 5: {                                                    // d16(An)
            const s32 d = sext16(fetch16());
            return static_cast<u32>(static_cast<s32>(a[reg]) + d);
        }
        case 6: {                                                    // d8(An,Xn)
            const u16 ext = fetch16();
            const u32 xi  = (ext >> 12) & 7u;
            const u32 xv  = (ext & 0x8000u) ? a[xi] : d[xi];
            const s32 x   = (ext & 0x0800u) ? static_cast<s32>(xv)
                                             : sext16(xv & 0xFFFFu);
            return static_cast<u32>(static_cast<s32>(a[reg]) + x + sext8(ext & 0xFFu));
        }
        case 7: switch (reg) {
            case 0: return static_cast<u32>(sext16(fetch16()));     // xxx.W
            case 1: return fetch32();                                // xxx.L
            case 2: {                                                // d16(PC)
                const u32 base = pc;
                return static_cast<u32>(static_cast<s32>(base) + sext16(fetch16()));
            }
            case 3: {                                                // d8(PC,Xn)
                const u32 base = pc;
                const u16 ext  = fetch16();
                const u32 xi   = (ext >> 12) & 7u;
                const u32 xv   = (ext & 0x8000u) ? a[xi] : d[xi];
                const s32 x    = (ext & 0x0800u) ? static_cast<s32>(xv)
                                                  : sext16(xv & 0xFFFFu);
                return static_cast<u32>(static_cast<s32>(base) + x + sext8(ext & 0xFFu));
            }
            default: break;
        }
        break;
    }
    return 0u;
}

u32 M68K::readEA(u32 mode, u32 reg, u32 sz) {
    if (mode == 0) return readDn(reg, sz);
    if (mode == 1) {                            // An — sign-extend word reads
        return sz == 1 ? static_cast<u32>(sext16(a[reg] & 0xFFFFu)) : a[reg];
    }
    if (mode == 7 && reg == 4) {                // #imm
        if (sz == 0) return fetch16() & 0xFFu;
        if (sz == 1) return fetch16();
        return fetch32();
    }
    return bus->readSize(calcEA(mode, reg, sz), sz);
}

void M68K::writeEA(u32 mode, u32 reg, u32 val, u32 sz) {
    if (mode == 0) { writeDn(reg, val, sz); return; }
    if (mode == 1) {                            // MOVEA — sign extend to 32
        a[reg] = sz == 1 ? static_cast<u32>(sext16(val & 0xFFFFu)) : val;
        return;
    }
    bus->writeSize(calcEA(mode, reg, sz), val, sz);
}

// ─────────────────────────────────────────────────────────────────────────────
// Flag helpers
// ─────────────────────────────────────────────────────────────────────────────
void M68K::setNZ(u32 r, u32 sz) {
    u32 mask, msb; _masks(sz, mask, msb);
    u16 s = sr & ~0x0Cu;
    if ((r & mask) == 0u) s |= 0x04u;
    if  (r & msb)         s |= 0x08u;
    sr = s;
}

void M68K::setNZVC(u32 r, u32 sz) {
    u32 mask, msb; _masks(sz, mask, msb);
    u16 s = sr & ~0x0Fu;
    if ((r & mask) == 0u) s |= 0x04u;
    if  (r & msb)         s |= 0x08u;
    sr = s;  // V and C cleared
}

u32 M68K::doAdd(u32 src, u32 dst, u32 sz, bool withX) {
    u32 mask, msb; _masks(sz, mask, msb);
    const u32 x   = withX ? ((sr >> 4) & 1u) : 0u;
    const u32 s   = src & mask, dd = dst & mask;
    const u64 sum = static_cast<u64>(s) + dd + x;
    const u32 r   = static_cast<u32>(sum) & mask;
    const bool c  = sum > static_cast<u64>(mask);
    const bool v  = ((~(s ^ dd) & (s ^ r) & msb) != 0u);

    u16 ns = sr & ~0x1Fu;
    if (c) ns |= 0x11u;
    if (v) ns |= 0x02u;
    if (withX) {
        // ADDX: clear Z on nonzero result; leave Z unchanged on zero result
        if (r != 0u) ns &= ~0x04u;
        else         ns |= (sr & 0x04u);
    } else if (r == 0u) {
        ns |= 0x04u;
    }
    if (r & msb) ns |= 0x08u;
    sr = ns;
    return r;
}

u32 M68K::doSub(u32 src, u32 dst, u32 sz, bool withX) {
    u32 mask, msb; _masks(sz, mask, msb);
    const u32 x    = withX ? ((sr >> 4) & 1u) : 0u;
    const u32 s    = src & mask, dd = dst & mask;
    const u64 diff = static_cast<u64>(dd) - s - x;
    const u32 r    = static_cast<u32>(diff) & mask;
    const bool brw = (diff >> 63) != 0u;            // high bit = borrow
    const bool v   = (((s ^ dd) & (dd ^ r) & msb) != 0u);

    u16 ns = sr & ~0x1Fu;
    if (brw) ns |= 0x11u;
    if (v)   ns |= 0x02u;
    if (withX) {
        // SUBX: same Z-flag rule as ADDX
        if (r != 0u) ns &= ~0x04u;
        else         ns |= (sr & 0x04u);
    } else if (r == 0u) {
        ns |= 0x04u;
    }
    if (r & msb) ns |= 0x08u;
    sr = ns;
    return r;
}

void M68K::doCmp(u32 src, u32 dst, u32 sz) {
    u32 mask, msb; _masks(sz, mask, msb);
    const u32 s    = src & mask, dd = dst & mask;
    const u64 diff = static_cast<u64>(dd) - s;
    const u32 r    = static_cast<u32>(diff) & mask;
    const bool brw = (diff >> 63) != 0u;
    const bool v   = (((s ^ dd) & (dd ^ r) & msb) != 0u);
    u16 ns = sr & ~0x0Fu;   // CMP does not update X
    if (brw) ns |= 0x01u;
    if (v)   ns |= 0x02u;
    if (r == 0u) ns |= 0x04u;
    if (r & msb) ns |= 0x08u;
    sr = ns;
}

bool M68K::testCC(u32 cc) {
    const bool c = (sr & 0x01u) != 0u, v = (sr & 0x02u) != 0u,
               z = (sr & 0x04u) != 0u, n = (sr & 0x08u) != 0u;
    switch (cc & 0xFu) {
        case  0: return true;
        case  1: return false;
        case  2: return !c && !z;
        case  3: return  c ||  z;
        case  4: return !c;
        case  5: return  c;
        case  6: return !z;
        case  7: return  z;
        case  8: return !v;
        case  9: return  v;
        case 10: return !n;
        case 11: return  n;
        case 12: return n == v;
        case 13: return n != v;
        case 14: return !z && (n == v);
        case 15: return  z || (n != v);
    }
    return false;
}

// ─────────────────────────────────────────────────────────────────────────────
// Exception / interrupt
// ─────────────────────────────────────────────────────────────────────────────
void M68K::exception(u32 vector) {
    a[7] -= 4; bus->write32(a[7], pc);
    a[7] -= 2; bus->write16(a[7], sr);
    sr = static_cast<u16>((sr | 0x2000u) & ~0x8000u);  // supervisor, clear trace
    pc = bus->read32(vector * 4u);
    cycles += 34;
}

bool M68K::interrupt(u32 level) {
    const u32 ipl = (sr >> 8) & 7u;
    if (level <= ipl && level != 7u) return false;
    stopped = false;
    const u16 savedSR = sr;
    sr = static_cast<u16>((sr & ~0x0700u) | (level << 8) | 0x2000u);
    sr &= ~0x8000u;
    a[7] -= 4; bus->write32(a[7], pc);
    a[7] -= 2; bus->write16(a[7], savedSR);
    pc = bus->read32((24u + level) * 4u);   // auto-vector 25–31
    cycles += 44;
    return true;
}

// ─────────────────────────────────────────────────────────────────────────────
// Main decode / execute
// ─────────────────────────────────────────────────────────────────────────────
void M68K::step() {
    if (stopped) { cycles += 4; return; }
    const u16 op = fetch16();
    
    // The high nibble (bits 15-12) determines the Instruction Group
    switch ((op >> 12) & 0xFu) {
        case 0x0: _g0(op);        break; // Bit Ops / Immediate
        case 0x1: _gMOVE(op);     break; // MOVE (Size is encoded inside op)
        case 0x2: _gD(op);        break; // ADD / ADDA / ADDX
        case 0x3: _g9(op);        break; // SUB / SUBA / SUBX
        case 0x4: _g4(op);        break; // Misc / LEA / MOVEM
        case 0x5: _g5(op);        break; // ADDQ / SUBQ / Scc
        case 0x6: _g6(op);        break; // BRA / BSR / Bcc
        case 0x7: _g7(op);        break; // MOVEQ
        case 0x8: _g8(op);        break; // OR / DIV / SBCD
        case 0x9: _g9(op);        break; // SUB / SUBA / SUBX (some encodings)
        case 0xA: exception(10);  break; // A-line
        case 0xB: _gB(op);        break; // CMP / EOR
        case 0xC: _gC(op);        break; // AND / MUL / EXG
        case 0xD: _gD(op);        break; // ADD / ADDA / ADDX (some encodings)
        case 0xE: _gE(op);        break; // Shifts / Rotates
        case 0xF: exception(11);  break; // F-line
        default:  exception(11);  break;
    }
}


// ─────────────────────────────────────────────────────────────────────────────
// Group 0: Bit ops / Immediate ops / MOVEP
// ─────────────────────────────────────────────────────────────────────────────
void M68K::_g0(u16 op) {
    const u32 srcMode = (op >> 3) & 7u, srcReg = op & 7u;
    const u32 dstReg  = (op >> 9) & 7u;
    const u32 b11_8   = (op >> 8) & 0xFu;

    // MOVEP: 0000 DDD1 zz 001 AAA
    if ((op & 0x0138u) == 0x0108u) {
        const bool dir = (op >> 7) & 1u, lng = (op >> 6) & 1u;
        const s32  disp = sext16(fetch16());
        const u32  base = (a[srcReg] + static_cast<u32>(disp)) & 0xFFFFFFu;
        if (!dir) {
            if (lng) {
                d[dstReg] = (static_cast<u32>(bus->read8(base))     << 24)
                          | (static_cast<u32>(bus->read8(base + 2)) << 16)
                          | (static_cast<u32>(bus->read8(base + 4)) <<  8)
                          |  static_cast<u32>(bus->read8(base + 6));
            } else {
                writeDn(dstReg, (static_cast<u32>(bus->read8(base)) << 8)
                              |  static_cast<u32>(bus->read8(base + 2)), 1);
            }
        } else {
            if (lng) {
                const u32 v = d[dstReg];
                bus->write8(base,     static_cast<u8>(v >> 24));
                bus->write8(base + 2, static_cast<u8>(v >> 16));
                bus->write8(base + 4, static_cast<u8>(v >>  8));
                bus->write8(base + 6, static_cast<u8>(v));
            } else {
                bus->write8(base,     static_cast<u8>(d[dstReg] >> 8));
                bus->write8(base + 2, static_cast<u8>(d[dstReg]));
            }
        }
        cycles += lng ? 24u : 16u;
        return;
    }

    // Dynamic bit ops: bit 8 = 1, not MOVEP
    if ((op & 0x0100u) && (b11_8 & 1u)) {
        const u32 typ = (op >> 6) & 3u;
        const u32 v   = readEA(srcMode, srcReg, 0);
        const u32 num = d[dstReg] & (srcMode == 0 ? 31u : 7u);
        _doBitOp(typ, num, srcMode, srcReg, v);
        cycles += 6; return;
    }

    const u32 sz = (op >> 6) & 3u;
    if (sz == 3) { _g0Special(op, b11_8, srcMode, srcReg, dstReg); return; }

    // Immediate convenience macro
#define IMM(sz_) ((sz_)==0 ? (fetch16() & 0xFFu) : (sz_)==1 ? fetch16() : fetch32())
    switch (b11_8) {
        case 0x0: { const u32 i=IMM(sz), v=readEA(srcMode,srcReg,sz)|i;  writeEA(srcMode,srcReg,v,sz); setNZVC(v,sz); cycles+=8; break; } // ORI
        case 0x2: { const u32 i=IMM(sz), v=readEA(srcMode,srcReg,sz)&i;  writeEA(srcMode,srcReg,v,sz); setNZVC(v,sz); cycles+=8; break; } // ANDI
        case 0x4: { const u32 i=IMM(sz), r=doSub(i,readEA(srcMode,srcReg,sz),sz,false); writeEA(srcMode,srcReg,r,sz); cycles+=8; break; }  // SUBI
        case 0x6: { const u32 i=IMM(sz), r=doAdd(i,readEA(srcMode,srcReg,sz),sz,false); writeEA(srcMode,srcReg,r,sz); cycles+=8; break; }  // ADDI
        case 0x8: {                                                          // BTST/BCHG/BCLR/BSET static
            const u32 num=fetch16()&(srcMode==0?31u:7u), typ=(op>>6)&3u;
            _doBitOp(typ, num, srcMode, srcReg, readEA(srcMode,srcReg,0)); cycles+=8; break;
        }
        case 0xA: { const u32 i=IMM(sz), v=readEA(srcMode,srcReg,sz)^i;  writeEA(srcMode,srcReg,v,sz); setNZVC(v,sz); cycles+=8; break; } // EORI
        case 0xC: { const u32 i=IMM(sz); doCmp(i,readEA(srcMode,srcReg,sz),sz); cycles+=8; break; }  // CMPI
        default:
            if (op & 0x0100u) {   // remaining dynamic bit ops
                const u32 typ=(op>>6)&3u, num=d[dstReg]&(srcMode==0?31u:7u);
                _doBitOp(typ, num, srcMode, srcReg, readEA(srcMode,srcReg,0));
                cycles+=6;
            }
            break;
    }
#undef IMM
}

void M68K::_g0Special(u16 op, u32 b11_8, u32 /*srcMode*/, u32 /*srcReg*/, u32 /*dstReg*/) {
    switch (b11_8) {
        case 0x0: { 
            const u16 i = static_cast<u16>(fetch16());
            if(op & 0x40u) sr = static_cast<u16>(sr | (i & 0xA71Fu)); 
            else           sr = static_cast<u16>(sr | (i & 0x1Fu)); 
            cycles += 20; 
            break; 
        }
        case 0x2: { 
            const u16 i = static_cast<u16>(fetch16());
            if(op & 0x40u) sr = static_cast<u16>(sr & (i & 0xA71Fu)); 
            else           sr = static_cast<u16>((sr & ~0x1Fu) | (i & 0x1Fu)); 
            cycles += 20; 
            break; 
        }
        case 0xA: { 
            const u16 i = static_cast<u16>(fetch16());
            if(op & 0x40u) sr = static_cast<u16>(sr ^ (i & 0xA71Fu)); 
            else           sr = static_cast<u16>(sr ^ (i & 0x1Fu)); 
            cycles += 20; 
            break; 
        }
    } // This closes the switch
} // This closes the function

void M68K::_doBitOp(u32 typ, u32 num, u32 mode, u32 reg, u32 v) {
    const u32 mask = 1u << num;
    const bool bitSet = (v & mask) != 0u;
    
    // Update Condition Codes (Z and N)
    // Z = 1 if bit is 0; N = 1 if bit is 1
    u16 ns = sr & ~0x0Cu;
    if (!bitSet) ns |= 0x04u; // Set Z
    if (bitSet)  ns |= 0x08u; // Set N
    sr = ns;

    if (typ == 0) return; // BTST: Just test the bit, don't write back

    u32 nv;
    switch (typ) {
        case 1: nv = v ^ mask; break;  // BCHG (Change/Toggle)
        case 2: nv = v & ~mask; break; // BCLR (Clear)
        default: nv = v | mask; break; // BSET (Set)
    }
    
    writeEA(mode, reg, nv, 0); // Write the modified byte back to memory/register
}


// ─────────────────────────────────────────────────────────────────────────────
// Groups 1/2/3: MOVE / MOVEA
// ─────────────────────────────────────────────────────────────────────────────
void M68K::_gMOVE(u16 op) {
    // Correct M68K MOVE decoding:
    // Bits 11-9: dstMode, Bits 8-6: dstReg, Bits 5-4: size, Bits 3-0: srcMode/Reg
    const u32 srcMode = (op >> 3) & 7u; 
    const u32 srcReg  = op & 7u;
    const u32 dstReg  = (op >> 6) & 7u;
    const u32 dstMode = (op >> 9) & 7u;
    const u32 sz      = (op >> 4) & 3u; // Size: 0=B, 1=W, 2=L

    const u32 val = readEA(srcMode, srcReg, sz);

    if (dstMode == 1) { // MOVEA: Always treats value as a 32-bit address
        a[dstReg] = sz == 1 ? static_cast<u32>(sext16(val & 0xFFFFu)) : val;
    } else {
        writeEA(dstMode, dstReg, val, sz);
        setNZVC(val, sz);
    }
    cycles += 4;
}


// ─────────────────────────────────────────────────────────────────────────────
// Group 4: Miscellaneous
// ─────────────────────────────────────────────────────────────────────────────
void M68K::_g4(u16 op) {
    const u32 b11_8  = (op >> 8) & 0xFu;
    const u32 sz     = (op >> 6) & 3u;
    const u32 mode   = (op >> 3) & 7u;
    const u32 reg    = op & 7u;
    const u32 dstReg = (op >> 9) & 7u;

    switch (b11_8) {
        case 0x0: {
            if (sz == 3) { writeEA(mode,reg,sr,1); cycles+=6; return; }   // MOVE SR→EA
            const u32 r = doSub(readEA(mode,reg,sz), 0u, sz, true);       // NEGX
            writeEA(mode,reg,r,sz); cycles+=6; return;
        }
        case 0x2: {
            if (sz == 3) {                                                  // MOVE #→CCR
                sr = static_cast<u16>((sr & ~0x1Fu) | (fetch16() & 0x1Fu));
                cycles+=20; return;
            }
            readEA(mode,reg,sz); writeEA(mode,reg,0,sz);                   // CLR
            sr = static_cast<u16>((sr & ~0x0Fu) | 0x04u);
            cycles+=4; return;
        }
case 0x4: {
    if (sz == 3) {
        // MOVE EA→CCR: only update lower 5 bits (condition codes), leave SR intact
        const u32 v = readEA(mode, reg, 1);
        sr = static_cast<u16>((sr & ~0x1Fu) | (v & 0x1Fu));
        cycles += 20; return;
    }
    const u32 r = doSub(readEA(mode,reg,sz), 0u, sz, false);   // NEG
    writeEA(mode,reg,r,sz); cycles+=6; return;
}
case 0x6: {
    if (sz == 3) {
        // MOVE EA→SR: full SR write, use readEA to handle all addressing modes
        sr = static_cast<u16>(readEA(mode, reg, 1) & 0xA71Fu);
        cycles += 20; return;
    }
    const u32 v = ~readEA(mode,reg,sz);   // NOT
    writeEA(mode,reg,v,sz); setNZVC(v,sz); cycles+=4; return;
}
        case 0x8: {
            if (sz == 0) {                                                  // NBCD
                const u32 v=readEA(mode,reg,0), x=(sr>>4)&1u;
                u32 r = (0x100u - v - x) & 0xFFu;
                bool c = (v|x) != 0u;
                if (!(r&0xFu) && !(v&0xFu) && !x) { r=0; c=false; }
                writeEA(mode,reg,r,0);
                u16 ns = sr & ~0x15u;
                if (c) ns|=0x11u; if (!r) ns|=0x04u;
                sr=ns; cycles+=6; return;
            }
            if (sz == 1) {
                if (mode == 0) {                                            // SWAP Dn
                    const u32 v=d[reg];
                    d[reg]=((v&0xFFFFu)<<16)|((v>>16)&0xFFFFu);
                    setNZVC(d[reg],2); cycles+=4; return;
                }
                const u32 addr=calcEA(mode,reg,2); a[7]-=4; bus->write32(a[7],addr); // PEA
                cycles+=12; return;
            }
            if (mode == 0) {
                if (sz == 2) {  // EXT.W
                    d[reg]=(d[reg]&0xFFFF0000u)|(static_cast<u32>(sext8(d[reg]&0xFFu))&0xFFFFu);
                    setNZVC(d[reg]&0xFFFFu,1); cycles+=4; return;
                }
                // EXT.L
                d[reg]=static_cast<u32>(sext16(d[reg]&0xFFFFu));
                setNZVC(d[reg],2); cycles+=4; return;
            }
            _movemToMem(op, mode, reg, sz==3?2u:1u); return;   // MOVEM reg→mem
        }
        case 0xA: {
            if (sz==3 && (op&0xFFu)==0xFCu) { exception(4); return; }     // ILLEGAL
            setNZVC(readEA(mode,reg,sz),sz); cycles+=4; return;             // TST (TAS stubbed)
        }
        case 0xC: {                                                         // MOVEM mem→reg or reg→mem
            const u32 s2 = (op>>6)&1u;
            if ((op>>7)&1u) _movemFromMem(op,mode,reg,s2?2u:1u);
            else            _movemToMem  (op,mode,reg,s2?2u:1u);
            return;
        }
        case 0xE: { _g4E(op, mode, reg, sz); return; }
        default:
            if ((op&0x01C0u)==0x0180u) {                                   // CHK
                const s32 dn=sext16(d[dstReg]&0xFFFFu), ub=sext16(static_cast<u16>(readEA(mode,reg,1)));
                if (dn<0)  { sr|=0x08u;  exception(6); }
                else if (dn>ub) { sr&=~0x08u; exception(6); }
                cycles+=10; return;
            }
            if ((op&0x01C0u)==0x01C0u) {                                   // LEA
                a[dstReg]=calcEA(mode,reg,2); cycles+=4; return;
            }
            break;
    }
}

void M68K::_g4E(u16 op, u32 mode, u32 reg, u32 /*sz*/) {
    const u32 lo8 = op & 0xFFu;

    if ((op&0xF0u)==0x40u) { exception(32u+(op&0xFu)); cycles+=34; return; }  // TRAP
    if ((op&0xF8u)==0x50u) {                                                    // LINK
        const s32 disp=sext16(fetch16());
        a[7]-=4; bus->write32(a[7],a[reg]);
        a[reg]=a[7];
        a[7]=static_cast<u32>(static_cast<s32>(a[7])+disp);
        cycles+=16; return;
    }
    if ((op&0xF8u)==0x58u) {                                                    // UNLK
        a[7]=a[reg]; a[reg]=bus->read32(a[7]); a[7]+=4; cycles+=12; return;
    }
    if ((op&0xF0u)==0x60u) {                                                    // MOVE USP
        if ((op>>3)&1u) a[reg]=usp; else usp=a[reg]; cycles+=4; return;
    }

    switch (lo8) {
        case 0x70: cycles+=132; return;   // RESET
        case 0x71: cycles+=  4; return;   // NOP
        case 0x72: sr=static_cast<u16>(fetch16()&0xA71Fu); stopped=true; cycles+=4; return; // STOP
        case 0x73: sr=bus->read16(a[7])&0xA71Fu; a[7]+=2; pc=bus->read32(a[7]); a[7]+=4; cycles+=20; return; // RTE
        case 0x75: pc=bus->read32(a[7]); a[7]+=4; cycles+=16; return; // RTS
        case 0x76: if (sr&0x02u) exception(7); cycles+=4; return;     // TRAPV
        case 0x77: {                                                    // RTR
            const u16 ccr=bus->read16(a[7])&0x1Fu; a[7]+=2;
            sr=static_cast<u16>((sr&~0x1Fu)|ccr);
            pc=bus->read32(a[7]); a[7]+=4; cycles+=20; return;
        }
        default: break;
    }

    if ((op&0xC0u)==0x80u) {   // JSR
        const u32 addr=calcEA(mode,reg,2); a[7]-=4; bus->write32(a[7],pc); pc=addr; cycles+=16; return;
    }
    if ((op&0xC0u)==0xC0u) {   // JMP
        pc=calcEA(mode,reg,2); cycles+=8; return;
    }
    cycles+=4;
}

// ─────────────────────────────────────────────────────────────────────────────
// MOVEM helpers
// ─────────────────────────────────────────────────────────────────────────────
void M68K::_movemToMem(u16 /*op*/, u32 mode, u32 reg, u32 sz) {
    const u16 list    = fetch16();
    const u32 bytes   = (sz==2?4u:2u) * popcount32(list);
    const bool preDec = (mode == 4);

    if (preDec) {
        // Reversed mask: bit 0=A7, bit 7=A0, bit 8=D7, bit 15=D0
        u32 addr = a[reg] - bytes;
        a[reg]   = addr;
        for (u32 i=0; i<16u; i++) {
            if (!(list & (1u<<i))) continue;
            const u32 bit = 15u - i;
            const u32 rv  = (bit >= 8u) ? this->a[bit-8u] : this->d[bit];
            if (sz==2) { bus->write32(addr, rv); addr+=4; }
            else       { bus->write16(addr, static_cast<u16>(rv)); addr+=2; }
        }
    } else {
        u32 addr = calcEA(mode, reg, sz);
        for (u32 i=0; i<16u; i++) {
            if (!(list & (1u<<i))) continue;
            const u32 rv = (i>=8u) ? this->a[i-8u] : this->d[i];
            if (sz==2) { bus->write32(addr, rv); addr+=4; }
            else       { bus->write16(addr, static_cast<u16>(rv)); addr+=2; }
        }
    }
    cycles += 8u + popcount32(list) * (sz==2?8u:4u);
}

void M68K::_movemFromMem(u16 /*op*/, u32 mode, u32 reg, u32 sz) {
    const u16 list = fetch16();
    u32 addr = calcEA(mode, reg, sz);

    for (u32 i=0; i<16u; i++) {
        if (!(list & (1u<<i))) continue;
        if (i < 8u) {
            // Data register — M68K spec: word reads sign-extended to 32 bits
            if (sz==2) { this->d[i]   = bus->read32(addr); addr+=4; }
            else       { this->d[i]   = static_cast<u32>(sext16(bus->read16(addr))); addr+=2; }
        } else {
            // Address register — always sign-extended
            if (sz==2) { this->a[i-8u] = bus->read32(addr); addr+=4; }
            else       { this->a[i-8u] = static_cast<u32>(sext16(bus->read16(addr))); addr+=2; }
        }
    }
    if (mode == 3) a[reg] = addr;  // post-increment: update An
    cycles += 12u + popcount32(list) * (sz==2?8u:4u);
}

// ─────────────────────────────────────────────────────────────────────────────
// Group 5: ADDQ / SUBQ / Scc / DBcc
// ─────────────────────────────────────────────────────────────────────────────
void M68K::_g5(u16 op) {
    const u32 sz   = (op>>6)&3u, mode=(op>>3)&7u, reg=op&7u;
    u32 imm = (op>>9)&7u; if (!imm) imm=8u;

    if (sz == 3) {
        if (mode == 1) {   // DBcc
            const u32 cc=(op>>8)&0xFu; const s32 disp=sext16(fetch16());
            if (!testCC(cc)) {
                const u32 cnt = (d[reg]&0xFFFFu) - 1u;
                writeDn(reg, cnt, 1);
                if ((cnt&0xFFFFu) != 0xFFFFu) {
                    pc=static_cast<u32>(static_cast<s32>(pc)-2+disp); cycles+=10;
                } else { cycles+=14; }
            } else { cycles+=12; }
            return;
        }
        // Scc
        writeEA(mode,reg, testCC((op>>8)&0xFu)?0xFFu:0x00u, 0); cycles+=4; return;
    }

    if ((op>>8)&1u) {   // SUBQ
        if (mode==1) a[reg]-=imm;
        else writeEA(mode,reg, doSub(imm,readEA(mode,reg,sz),sz,false), sz);
    } else {            // ADDQ
        if (mode==1) a[reg]+=imm;
        else writeEA(mode,reg, doAdd(imm,readEA(mode,reg,sz),sz,false), sz);
    }
    cycles+=4;
}

// ─────────────────────────────────────────────────────────────────────────────
// Group 6: BRA / BSR / Bcc
// ─────────────────────────────────────────────────────────────────────────────
void M68K::_g6(u16 op) {
    const u32 cc        = (op>>8)&0xFu;
    const u32 byteField = op & 0xFFu;
    s32 disp            = sext8(byteField);
    s32 adj             = 0;

    if (byteField == 0) {
        disp = sext16(fetch16());
        adj  = -2;   // target = (pc after ext word) - 2 + disp
    }

    if (cc == 1) {   // BSR
        a[7]-=4; bus->write32(a[7], pc);
        pc = static_cast<u32>(static_cast<s32>(pc) + adj + disp);
        cycles+=18; return;
    }
    if (cc == 0 || testCC(cc)) {
        pc = static_cast<u32>(static_cast<s32>(pc) + adj + disp);
        cycles+=10;
    } else {
        cycles+=8;
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Group 7: MOVEQ
// ─────────────────────────────────────────────────────────────────────────────
void M68K::_g7(u16 op) {
    const u32 v = static_cast<u32>(sext8(op & 0xFFu));
    d[(op>>9)&7u] = v;
    setNZVC(v, 2); cycles+=4;
}

// ─────────────────────────────────────────────────────────────────────────────
// Group 8: OR / DIVU / DIVS / SBCD
// ─────────────────────────────────────────────────────────────────────────────
void M68K::_g8(u16 op) {
    const u32 dn=(op>>9)&7u, sz=(op>>6)&3u, mode=(op>>3)&7u, reg=op&7u;

    if (sz == 3) {   // DIVU / DIVS
        const bool sgn = (op>>8)&1u;
        const u32 div  = readEA(mode,reg,1);
        if (!div) { exception(5); return; }
        const u32 dvnd = d[dn];
        u32 quot, rem;
        if (sgn) {
            const s32 sd=sext16(div), sv=static_cast<s32>(dvnd);
            const s32 sq=sv/sd, sr_=sv%sd;
            if (sq<-32768||sq>32767) { this->sr=static_cast<u16>((this->sr&~0x0Fu)|0x02u); cycles+=140; return; }
            quot=static_cast<u32>(sq)&0xFFFFu; rem=static_cast<u32>(sr_)&0xFFFFu;
        } else {
            const u32 dw=div&0xFFFFu;
            if (dvnd/dw > 65535u) { this->sr=static_cast<u16>((this->sr&~0x0Fu)|0x02u); cycles+=140; return; }
            quot=dvnd/dw; rem=dvnd%dw;
        }
        d[dn] = (rem<<16)|quot;
        u16 ns=static_cast<u16>(this->sr&~0x0Fu);
        if (!quot) ns|=0x04u; if (quot&0x8000u) ns|=0x08u;
        this->sr=ns; cycles+=140; return;
    }

    // SBCD: 1000 DDD1 0000 mDDD
    if (sz==0 && (op&0x1F0u)==0x100u) {
        const bool rm=(op>>3)&1u;
        u32 src, dst;
        if (rm) { a[reg]--; src=bus->read8(a[reg]); a[dn]--; dst=bus->read8(a[dn]); }
        else    { src=d[reg]&0xFFu; dst=d[dn]&0xFFu; }
        const u32 x=(sr>>4)&1u;
        s32 lo=static_cast<s32>((dst&0xFu)-(src&0xFu)-x), hi=static_cast<s32>(((dst>>4)&0xFu)-((src>>4)&0xFu));
        if (lo<0){lo+=10;hi--;} if (hi<0){hi+=10;sr|=0x11u;}else{sr&=~0x11u;}
        const u32 rv=(static_cast<u32>(hi)<<4)|(static_cast<u32>(lo)&0xFu);
        if (rm) bus->write8(a[dn],static_cast<u8>(rv)); else writeDn(dn,rv,0);
        if (rv) sr&=~0x04u; cycles+=6; return;
    }

    // OR
    if ((op&0x100u)&&mode>1u) { const u32 r=readEA(mode,reg,sz)|readDn(dn,sz); writeEA(mode,reg,r,sz); setNZVC(r,sz); }
    else                       { const u32 r=readDn(dn,sz)|readEA(mode,reg,sz); writeDn(dn,r,sz);       setNZVC(r,sz); }
    cycles+=4;
}

// ─────────────────────────────────────────────────────────────────────────────
// Group 9: SUB / SUBA / SUBX
// ─────────────────────────────────────────────────────────────────────────────
void M68K::_g9(u16 op) {
    const u32 dn=(op>>9)&7u, sz=(op>>6)&3u, mode=(op>>3)&7u, reg=op&7u, dir=(op>>8)&1u;

    if (sz == 3) {   // SUBA
        const u32 os=dir?2u:1u, src=readEA(mode,reg,os);
        a[dn] -= static_cast<u32>(os==1?sext16(src):static_cast<s32>(src));
        cycles+=8; return;
    }
    if (dir && (mode==0u||mode==4u)) {   // SUBX
        const bool rm=(mode==4u); const u32 step=(sz==0)?1u:(sz==1)?2u:4u;
        u32 src, dst;
        if (rm){a[reg]-=step;src=bus->readSize(a[reg],sz);a[dn]-=step;dst=bus->readSize(a[dn],sz);}
        else   {src=readDn(reg,sz);dst=readDn(dn,sz);}
        const u32 r=doSub(src,dst,sz,true);
        if (rm) bus->writeSize(a[dn],r,sz); else writeDn(dn,r,sz);
        cycles+=4; return;
    }
    if (dir) { writeEA(mode,reg, doSub(readDn(dn,sz),readEA(mode,reg,sz),sz,false), sz); }
    else     { writeDn(dn,        doSub(readEA(mode,reg,sz),readDn(dn,sz),sz,false), sz); }
    cycles+=4;
}

// ─────────────────────────────────────────────────────────────────────────────
// Group B: CMP / CMPA / CMPM / EOR
// ─────────────────────────────────────────────────────────────────────────────
void M68K::_gB(u16 op) {
    const u32 dn=(op>>9)&7u, sz=(op>>6)&3u, mode=(op>>3)&7u, reg=op&7u, dir=(op>>8)&1u;

    if (sz == 3) {   // CMPA
        const u32 os=dir?2u:1u, src=readEA(mode,reg,os);
        doCmp(static_cast<u32>(os==1?sext16(src):static_cast<s32>(src)), a[dn], 2);
        cycles+=6; return;
    }
    if (dir) {
        if (mode==1u) {   // CMPM (An)+,(An)+
            doCmp(readEA(3,reg,sz), readEA(3,dn,sz), sz); cycles+=12; return;
        }
        const u32 v=readEA(mode,reg,sz)^readDn(dn,sz);   // EOR Dn→EA
        writeEA(mode,reg,v,sz); setNZVC(v,sz); cycles+=4; return;
    }
    doCmp(readEA(mode,reg,sz), readDn(dn,sz), sz); cycles+=4;   // CMP EA→Dn
}

// ─────────────────────────────────────────────────────────────────────────────
// Group C: AND / MULU / MULS / ABCD / EXG
// ─────────────────────────────────────────────────────────────────────────────
void M68K::_gC(u16 op) {
    const u32 dn=(op>>9)&7u, sz=(op>>6)&3u, mode=(op>>3)&7u, reg=op&7u, dir=(op>>8)&1u;

    if (sz == 3) {   // MULU / MULS
        const u32 src=readEA(mode,reg,1)&0xFFFFu, dst=d[dn]&0xFFFFu;
        const u32 r = dir ? static_cast<u32>(sext16(src)*sext16(dst)) : src*dst;
        d[dn]=r;
        u16 ns=static_cast<u16>(sr&~0x0Fu);
        if (!r) ns|=0x04u; if (r&0x80000000u) ns|=0x08u;
        sr=ns; cycles+=70; return;
    }
    // ABCD: 1100 DDD1 0000 mDDD
    if (dir&&sz==0u&&mode<=1u) {
        const bool rm=(mode==1u);
        u32 src, dst;
        if (rm){a[reg]--;src=bus->read8(a[reg]);a[dn]--;dst=bus->read8(a[dn]);}
        else   {src=d[reg]&0xFFu;dst=d[dn]&0xFFu;}
        const u32 x=(sr>>4)&1u;
        s32 lo=static_cast<s32>((dst&0xFu)+(src&0xFu)+x), hi=static_cast<s32>(((dst>>4)&0xFu)+((src>>4)&0xFu));
        if (lo>9){lo-=10;hi++;} if (hi>9){hi-=10;sr|=0x11u;}else{sr&=~0x11u;}
        const u32 rv=(static_cast<u32>(hi)<<4)|(static_cast<u32>(lo)&0xFu);
        if (rm) bus->write8(a[dn],static_cast<u8>(rv)); else writeDn(dn,rv,0);
        if (rv) sr&=~0x04u; cycles+=6; return;
    }
    // EXG
    if (dir&&(sz==1u||sz==2u)) {
        if      ((op&0xF8u)==0x40u){const u32 t=d[dn];d[dn]=d[reg];d[reg]=t;}
        else if ((op&0xF8u)==0x48u){const u32 t=a[dn];a[dn]=a[reg];a[reg]=t;}
        else                       {const u32 t=d[dn];d[dn]=a[reg];a[reg]=t;}
        cycles+=6; return;
    }
    // AND
    if (dir&&mode>1u){ const u32 r=readEA(mode,reg,sz)&readDn(dn,sz); writeEA(mode,reg,r,sz); setNZVC(r,sz); }
    else             { const u32 r=readDn(dn,sz)&readEA(mode,reg,sz); writeDn(dn,r,sz);       setNZVC(r,sz); }
    cycles+=4;
}

// ─────────────────────────────────────────────────────────────────────────────
// Group D: ADD / ADDA / ADDX
// ─────────────────────────────────────────────────────────────────────────────
void M68K::_gD(u16 op) {
    const u32 dn=(op>>9)&7u, sz=(op>>6)&3u, mode=(op>>3)&7u, reg=op&7u, dir=(op>>8)&1u;

    if (sz == 3) {   // ADDA
        const u32 os=dir?2u:1u, src=readEA(mode,reg,os);
        a[dn] += static_cast<u32>(os==1?sext16(src):static_cast<s32>(src));
        cycles+=8; return;
    }
    if (dir&&(mode==0u||mode==4u)) {   // ADDX
        const bool rm=(mode==4u); const u32 step=(sz==0)?1u:(sz==1)?2u:4u;
        u32 src, dst;
        if (rm){a[reg]-=step;src=bus->readSize(a[reg],sz);a[dn]-=step;dst=bus->readSize(a[dn],sz);}
        else   {src=readDn(reg,sz);dst=readDn(dn,sz);}
        const u32 r=doAdd(src,dst,sz,true);
        if (rm) bus->writeSize(a[dn],r,sz); else writeDn(dn,r,sz);
        cycles+=4; return;
    }
    if (dir) { writeEA(mode,reg, doAdd(readDn(dn,sz),readEA(mode,reg,sz),sz,false), sz); }
    else     { writeDn(dn,        doAdd(readEA(mode,reg,sz),readDn(dn,sz),sz,false), sz); }
    cycles+=4;
}

// ─────────────────────────────────────────────────────────────────────────────
// Group E: Shift / Rotate
// ─────────────────────────────────────────────────────────────────────────────
void M68K::_gE(u16 op) {
    const u32 sz     = (op>>6)&3u;
    const u32 mode   = (op>>3)&7u;
    const u32 reg    = op&7u;
    const bool left  = (op>>8)&1u;

    if (sz == 3) {
        // Memory shift/rotate: type is in bits 11–10 (JS had a bug reading bits 4–3)
        const u32 type = (op>>10)&3u;
        const u32 v    = readEA(mode,reg,1);
        writeEA(mode,reg, _doShift(type,left,v,1,1), 1);
        cycles+=8; return;
    }

    // Register shift/rotate: type in bits 4–3, count from imm or Dn
    const u32 type   = (op>>3)&3u;
    const bool byReg = (op>>5)&1u;
    const u32 cnt    = byReg ? (d[(op>>9)&7u]&63u)
                             : (((op>>9)&7u) ? (op>>9)&7u : 8u);
    const u32 r = _doShift(type, left, readDn(reg,sz), cnt, sz);
    writeDn(reg,r,sz);
    cycles += 4u + cnt*2u;
}

u32 M68K::_doShift(u32 type, bool left, u32 v, u32 cnt, u32 sz) {
    u32 mask, msb; _masks(sz, mask, msb);
    const u32 bits = (sz==0)?8u:(sz==1)?16u:32u;

    cnt &= 63u;

    if (cnt == 0) {
        u16 ns = static_cast<u16>(sr & ~0x0Fu);
        if ((v&mask)==0u) ns|=0x04u; if (v&msb) ns|=0x08u;
        sr=ns; return v&mask;
    }

    u32 r = v&mask; bool c=false; u32 x=(sr>>4)&1u;

    // ROX and RO require the iterative path to carry X correctly
    if (type==2||type==3) {
        for (u32 i=0; i<cnt; i++) {
            if (type==2) {   // ROXL/ROXR
                if (left){ bool o=(r&msb)!=0u; r=((r<<1)|x)&mask; x=o?1u:0u; c=o; }
                else     { bool o=(r&1u) !=0u; r=((r>>1)|(x?msb:0u))&mask; x=o?1u:0u; c=o; }
            } else {         // ROL/ROR
                if (left){ bool o=(r&msb)!=0u; r=((r<<1)|(o?1u:0u))&mask; c=o; }
                else     { bool o=(r&1u) !=0u; r=((r>>1)|(o?msb:0u))&mask; c=o; }
            }
        }
        u16 ns=sr&~0x0Fu;
        if (c)        ns|=0x01u;
        if ((r&mask)==0u) ns|=0x04u;
        if (r&msb)    ns|=0x08u;
        // ROL/ROR do not update X; ROXL/ROXR do
        if (type==2) ns=static_cast<u16>((ns&~0x10u)|(c?0x10u:0u));
        else         ns=static_cast<u16>((ns&~0x10u)|(sr&0x10u));
        sr=ns; return r;
    }

    // ASL/ASR and LSL/LSR — direct arithmetic, no loop needed
    if (left) {
        if (cnt>=bits)  { c=false; r=0u; }
        else            { c=(v&(msb>>(cnt-1u)))!=0u; r=(v<<cnt)&mask; }
    } else {
        if (cnt>=bits)  {
            if (type==0){ c=(v&msb)!=0u; r=(v&msb)?mask:0u; }   // ASR fill
            else        { c=false; r=0u; }                        // LSR
        } else {
            c=(v&(1u<<(cnt-1u)))!=0u;
            if (type==0){ const u32 sign=(v&msb)?~(mask>>cnt)&mask:0u; r=((v>>cnt)|sign)&mask; }  // ASR
            else        { r=(v>>cnt)&mask; }                                                         // LSR
        }
    }

    // ASL V flag: set if sign bit changed
    bool vf = false;
    if (type==0&&left&&cnt>0u) vf=((v&msb)!=0u)!=((r&msb)!=0u);

    u16 ns=sr&~0x1Fu;
    if (c)        ns|=0x11u;
    if (vf)       ns|=0x02u;
    if ((r&mask)==0u) ns|=0x04u;
    if (r&msb)    ns|=0x08u;
    sr=ns; return r;
}

// ─────────────────────────────────────────────────────────────────────────────
// Run loop
// ─────────────────────────────────────────────────────────────────────────────
u32 M68K::run(u32 targetCycles) {
    while (cycles < targetCycles) step();
    const u32 overshoot = cycles - targetCycles;
    cycles = 0;
    return overshoot;
}
