#include "genesis.h"
#include "ding_audio.h"
#include <cstring>

// ─────────────────────────────────────────────────────────────────────────────
// YM2612 — OPN2 FM synthesizer (v0.1 register stub)
//
// Register layout: two banks × 256 bytes
//   Bank 0: channels 1–3, global timers, LFO, DAC
//   Bank 1: channels 4–6
//
// Full FM synthesis (operators, envelopes, algorithms, LFO) is deferred.
// Writes are stored for later use by a full synthesis implementation.
// ─────────────────────────────────────────────────────────────────────────────
YM2612::YM2612() {
    std::memset(regs, 0, sizeof(regs));
    status = 0;
}

void YM2612::reset() {
    std::memset(regs, 0, sizeof(regs));
    status = 0;
}

void YM2612::write(u32 bank, u8 reg, u8 val) {
    regs[(bank << 8) | reg] = val;
}

u8 YM2612::read() const {
    // Status byte: bit 1 = timer B overflow, bit 0 = timer A overflow
    // Both cleared here — real timers deferred to full implementation
    return status & 0x03u;
}

void YM2612::clock(float* buf, u32 count) {
    // v0.1: silence. Full OPN2 synthesis in a dedicated chat session.
    std::memset(buf, 0, count * 2u * sizeof(float));
}

// ─────────────────────────────────────────────────────────────────────────────
// SN76489 — Programmable Sound Generator (v0.1 register stub)
//
// 3 tone channels + 1 noise channel, each with 4-bit volume attenuation.
// The latch byte selects which channel/register subsequent writes go to.
//
// Tone register layout in regs[]:
//   regs[0,2,4] = tone frequency low nibble (channels 0–2)
//   regs[1,3,5] = volume attenuation (channels 0–2)
//   regs[6]     = noise control
//   regs[7]     = noise volume attenuation
// ─────────────────────────────────────────────────────────────────────────────
SN76489::SN76489() {
    std::memset(regs, 0, sizeof(regs));
    // All channels silent at startup (max attenuation = 0xF)
    regs[1] = regs[3] = regs[5] = regs[7] = 0x0Fu;
    latch = 0;
}

void SN76489::reset() {
    std::memset(regs, 0, sizeof(regs));
    regs[1] = regs[3] = regs[5] = regs[7] = 0x0Fu;
    latch = 0;
}

void SN76489::write(u8 val) {
    if (val & 0x80u) {
        // Latch byte: selects channel and register type
        latch = val;
        const u32 ch  = (val >> 5) & 3u;
        const u32 typ = (val >> 4) & 1u;   // 0 = tone/noise, 1 = volume
        if (typ) {
            regs[ch * 2u + 1u] = val & 0x0Fu;  // volume (lower 4 bits)
        } else {
            regs[ch * 2u] = val & 0x0Fu;        // tone low nibble
        }
    } else {
        // Data byte: updates the register selected by latch
        const u32 ch  = (latch >> 5) & 3u;
        const u32 typ = (latch >> 4) & 1u;
        if (!typ) {
            // Tone frequency: high 6 bits go into upper part of freq register
            regs[ch * 2u] = static_cast<u8>(
                (regs[ch * 2u] & 0x0Fu) | ((val & 0x3Fu) << 4));
        }
        // Volume data writes use the latch-byte path above
    }
}

void SN76489::clock(float* buf, u32 count) {
    // v0.1: silence. Full PSG synthesis in a dedicated chat session.
    std::memset(buf, 0, count * 2u * sizeof(float));
}

// ─────────────────────────────────────────────────────────────────────────────
// GenAPU — Audio subsystem
// Owns YM2612, SN76489, and the DingAudioBuffer ring buffer.
// The ring buffer is the handoff point between the emulator core and the
// frontend's Web Audio callback (or SDL audio callback on native).
// ─────────────────────────────────────────────────────────────────────────────
GenAPU::GenAPU() : lastYMReg(0) {
    std::memset(&audioBuf,    0, sizeof(audioBuf));
    std::memset(audioStorage, 0, sizeof(audioStorage));
    ding_audio_init(&audioBuf, audioStorage, GEN_AUDIO_CAPACITY,
                    GEN_AUDIO_CHANNELS, GEN_AUDIO_RATE);
}

void GenAPU::reset() {
    ym2612.reset();
    psg.reset();
    lastYMReg = 0;
    ding_audio_reset(&audioBuf);
}

// ── Register access (called by GenBus write path) ────────────────────────────

void GenAPU::writeYM(u32 bank, u8 reg, u8 val) {
    ym2612.write(bank, reg, val);
}

u8 GenAPU::readYM() const {
    return ym2612.read();
}

void GenAPU::writePSG(u8 val) {
    psg.write(val);
}

// ── Frame generation ─────────────────────────────────────────────────────────
// Called by Genesis::runFrame at the end of each video frame.
// samplesNeeded is computed from the audio sample rate and frame timing.
// The frontend reads samples from audioBuf via ding_audio_read().
//
// v0.1: push silence. When YM2612 and SN76489 synthesis are implemented,
// this function will:
//   1. Run ym2612.clock() and psg.clock() to generate PCM into temp buffers
//   2. Mix the two outputs (YM2612 dominates; PSG is quieter)
//   3. Push mixed samples into audioBuf via ding_audio_write() or write_sample()
// ─────────────────────────────────────────────────────────────────────────────
void GenAPU::generateFrame(u32 samplesNeeded) {
    const float silent[2] = { 0.0f, 0.0f };
    for (u32 i = 0; i < samplesNeeded; i++) {
        ding_audio_write_sample(&audioBuf, silent);
    }
}
