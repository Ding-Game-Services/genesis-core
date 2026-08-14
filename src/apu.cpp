#include "genesis.h"
#include "ding_audio.h"
#include <cstring>
#include <cmath>

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
    reset();
}

void YM2612::reset() {
    std::memset(regs, 0, sizeof(regs));
    status = 0;
    for (u32 c = 0; c < 6u; c++) {
        for (u32 o = 0; o < 4u; o++) ch_[c].op[o] = FMOperator();
        ch_[c].fbHist[0] = ch_[c].fbHist[1] = 0.0;
    }
}

void YM2612::write(u32 bank, u8 reg, u8 val) {
    regs[(bank << 8) | reg] = val;
    // Key on/off (reg 0x28) is only ever addressed via bank 0 on real
    // hardware, with the target channel encoded in the low bits of val.
    if (bank == 0u && reg == 0x28u) _keyOnOff(val);
}

u8 YM2612::read() const {
    // Status byte: bit 1 = timer B overflow, bit 0 = timer A overflow
    // Both cleared here — real timers deferred to full implementation
    return status & 0x03u;
}

void YM2612::_keyOnOff(u8 val) {
    const u32 chSel = val & 0x07u;
    if (chSel == 3u || chSel == 7u) return;   // invalid slot, no such channel
    const u32 ch = (chSel < 4u) ? chSel : (chSel - 1u);   // 0-2,4-6 -> 0-5
    const u32 opMask = (val >> 4) & 0x0Fu;
    for (u32 opIdx = 0; opIdx < 4u; opIdx++) {
        FMOperator& o = ch_[ch].op[opIdx];
        const bool on = (opMask & (1u << opIdx)) != 0u;
        if (on && !o.keyOn) {
            o.keyOn = true; o.phase = 0.0; o.envState = 0; o.envLevel = 0.0f;
        } else if (!on && o.keyOn) {
            o.keyOn = false; o.envState = 3;   // begin release from current level
        }
    }
}

// Fnum/block -> Hz. Derived from the standard OPN2 relation
// Fnum = freq * 2^(20-block) * 144 / (chipClock/6); inverted here.
// chipClock ~= 7,670,454 Hz on NTSC Genesis (PAL variance not modeled yet).
double YM2612::_channelFreqHz(u32 ch) const {
    const u32 base = (ch < 3u) ? 0u : 0x100u;
    const u32 co   = ch % 3u;
    const u8 fnLo    = regs[base + 0xA0u + co];
    const u8 fnHiBlk = regs[base + 0xA4u + co];
    const u32 fnum  = (static_cast<u32>(fnHiBlk & 0x07u) << 8) | fnLo;
    const u32 block = (fnHiBlk >> 3) & 0x07u;
    static constexpr double CLOCK = 7670454.0 / 6.0;
    return static_cast<double>(fnum) * CLOCK * std::pow(2.0, static_cast<double>(block))
         / (1048576.0 * 144.0);
}

// Simplified (non-exponential-table) ADSR. Rates aren't hardware-accurate
// timing, just tuned to feel like attack/decay/sustain/release — a real
// rate-table implementation is a follow-up refinement, not a v1 blocker.
float YM2612::_stepEnvelope(FMOperator& o, u32 ar, u32 d1r, u32 d2r, u32 rr, u32 sl) {
    const float slLevel = (sl >= 15u) ? 0.0f : std::pow(10.0f, -(static_cast<float>(sl) * 3.0f) / 20.0f);
    switch (o.envState) {
        case 0: {   // Attack
            if (ar == 0u) break;   // rate 0 = never rises
            const float coeff = std::pow(static_cast<float>(ar) / 31.0f, 1.5f) * 0.35f;
            o.envLevel += (1.0f - o.envLevel) * coeff;
            if (o.envLevel >= 0.995f) { o.envLevel = 1.0f; o.envState = 1; }
            break;
        }
        case 1: {   // Decay1 -> sustain level
            if (d1r == 0u) break;   // rate 0 = holds at attack peak
            const float coeff = std::pow(static_cast<float>(d1r) / 31.0f, 1.5f) * 0.02f;
            o.envLevel -= (o.envLevel - slLevel) * coeff;
            if (o.envLevel <= slLevel + 0.001f) { o.envLevel = slLevel; o.envState = 2; }
            break;
        }
        case 2: {   // Decay2 / sustain decay toward 0
            if (d2r == 0u) break;   // rate 0 = holds indefinitely
            const float coeff = std::pow(static_cast<float>(d2r) / 31.0f, 1.5f) * 0.02f;
            o.envLevel -= o.envLevel * coeff;
            if (o.envLevel < 0.0f) o.envLevel = 0.0f;
            break;
        }
        case 3: {   // Release
            const u32 rrFull = static_cast<u32>(rr) * 2u;   // 4-bit RR -> 5-bit rate scale
            if (rrFull == 0u) break;
            const float coeff = std::pow(static_cast<float>(rrFull) / 31.0f, 1.5f) * 0.02f;
            o.envLevel -= o.envLevel * coeff;
            if (o.envLevel <= 0.0005f) { o.envLevel = 0.0f; o.envState = 4; }
            break;
        }
        default: o.envLevel = 0.0f; break;   // idle
    }
    return o.envLevel;
}

float YM2612::_renderOperator(u32 ch, u32 opIdx, double freqHz, float modInput) {
    FMOperator& o = ch_[ch].op[opIdx];

    const u32 base = (ch < 3u) ? 0u : 0x100u;
    const u32 co   = ch % 3u;
    const u32 regOff = opIdx * 4u + co;

    const u8 mulReg = regs[base + 0x30u + regOff] & 0x0Fu;
    const double mul = (mulReg == 0u) ? 0.5 : static_cast<double>(mulReg);

    const double phaseInc = (freqHz * mul) / static_cast<double>(GEN_AUDIO_RATE);
    o.phase = std::fmod(o.phase + phaseInc, 1.0);
    const double angle = (o.phase + static_cast<double>(modInput)) * 2.0 * M_PI;
    const float raw = static_cast<float>(std::sin(angle));

    const u8 tl = regs[base + 0x40u + regOff] & 0x7Fu;
    const float tlGain = std::pow(10.0f, -(static_cast<float>(tl) * 0.75f) / 20.0f);

    const u8 ar  = regs[base + 0x50u + regOff] & 0x1Fu;
    const u8 d1r = regs[base + 0x60u + regOff] & 0x1Fu;
    const u8 d2r = regs[base + 0x70u + regOff] & 0x1Fu;
    const u8 slrr = regs[base + 0x80u + regOff];
    const u8 sl = (slrr >> 4) & 0x0Fu;
    const u8 rr = slrr & 0x0Fu;

    const float envGain = _stepEnvelope(o, ar, d1r, d2r, rr, sl);
    return raw * envGain * tlGain;
}

// Algorithm connection table follows the standard documented OPN2 chart
// (operators labeled S1-S4 internally as op[0]-op[3]).
float YM2612::renderSample() {
    float mixOut = 0.0f;
    static const float fbScale[8] = {
        0.0f, 1.0f/32.0f, 1.0f/16.0f, 1.0f/8.0f, 1.0f/4.0f, 1.0f/2.0f, 1.0f, 2.0f
    };

    for (u32 ch = 0; ch < 6u; ch++) {
        const double freqHz = _channelFreqHz(ch);
        const u32 base = (ch < 3u) ? 0u : 0x100u;
        const u32 co   = ch % 3u;
        const u8 fbAlg = regs[base + 0xB0u + co];
        const u32 fb  = (fbAlg >> 3) & 0x07u;
        const u32 alg = fbAlg & 0x07u;

        const float fbMod = static_cast<float>((ch_[ch].fbHist[0] + ch_[ch].fbHist[1]) * 0.5)
                           * fbScale[fb];

        const float o1 = _renderOperator(ch, 0, freqHz, fbMod);
        ch_[ch].fbHist[1] = ch_[ch].fbHist[0];
        ch_[ch].fbHist[0] = o1;

        float chanOut = 0.0f;
        switch (alg) {
            case 0: { const float o2=_renderOperator(ch,1,freqHz,o1); const float o3=_renderOperator(ch,2,freqHz,o2); const float o4=_renderOperator(ch,3,freqHz,o3); chanOut=o4; break; }
            case 1: { const float o2=_renderOperator(ch,1,freqHz,0.0f); const float o3=_renderOperator(ch,2,freqHz,o1+o2); const float o4=_renderOperator(ch,3,freqHz,o3); chanOut=o4; break; }
            case 2: { const float o2=_renderOperator(ch,1,freqHz,0.0f); const float o3=_renderOperator(ch,2,freqHz,o2); const float o4=_renderOperator(ch,3,freqHz,o1+o3); chanOut=o4; break; }
            case 3: { const float o2=_renderOperator(ch,1,freqHz,o1); const float o3=_renderOperator(ch,2,freqHz,0.0f); const float o4=_renderOperator(ch,3,freqHz,o2+o3); chanOut=o4; break; }
            case 4: { const float o2=_renderOperator(ch,1,freqHz,o1); const float o3=_renderOperator(ch,2,freqHz,0.0f); const float o4=_renderOperator(ch,3,freqHz,o3); chanOut=o2+o4; break; }
            case 5: { const float o2=_renderOperator(ch,1,freqHz,o1); const float o3=_renderOperator(ch,2,freqHz,o1); const float o4=_renderOperator(ch,3,freqHz,o1); chanOut=o2+o3+o4; break; }
            case 6: { const float o2=_renderOperator(ch,1,freqHz,o1); const float o3=_renderOperator(ch,2,freqHz,0.0f); const float o4=_renderOperator(ch,3,freqHz,0.0f); chanOut=o2+o3+o4; break; }
            default:{ const float o2=_renderOperator(ch,1,freqHz,0.0f); const float o3=_renderOperator(ch,2,freqHz,0.0f); const float o4=_renderOperator(ch,3,freqHz,0.0f); chanOut=o1+o2+o3+o4; break; }
        }
        mixOut += chanOut;
    }
    return mixOut * (1.0f / 6.0f);   // headroom across 6 channels
}

void YM2612::clock(float* buf, u32 count) {
    for (u32 i = 0; i < count; i++) {
        const float s = renderSample();
        buf[i * 2u]     = s;
        buf[i * 2u + 1] = s;
    }
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
// PSG master clock — Genesis PSG (SN76489-derived) runs at the same rate
// as the YM2612's /7 output, ~3579545 Hz on NTSC. Close enough on PAL too
// for v1 purposes (region-specific PSG clock deferred alongside FM work).
static constexpr double PSG_CLOCK = 3579545.0;

// Standard SN76489 4-bit attenuation table, ~2dB per step, 15 = silence.
static const float kPsgVolTable[16] = {
    1.0000f, 0.7943f, 0.6310f, 0.5012f, 0.3981f, 0.3162f, 0.2512f, 0.1995f,
    0.1585f, 0.1259f, 0.1000f, 0.0794f, 0.0631f, 0.0501f, 0.0398f, 0.0000f
};

SN76489::SN76489() {
    std::memset(regs, 0, sizeof(regs));
    reset();
}

void SN76489::reset() {
    std::memset(regs, 0, sizeof(regs));
    latch = 0;
    freq[0] = freq[1] = freq[2] = 0;
    // All channels silent at startup (max attenuation = 0xF)
    atten[0] = atten[1] = atten[2] = atten[3] = 0x0Fu;
    noiseCtrl = 0;
    tonePhase[0] = tonePhase[1] = tonePhase[2] = 0.0;
    noisePhase = 0.0;
    noiseShift = 0x8000u;   // real hardware reset state
}

void SN76489::write(u8 val) {
    if (val & 0x80u) {
        // Latch byte: selects channel and register type
        latch = val;
        const u32 ch  = (val >> 5) & 3u;
        const u32 typ = (val >> 4) & 1u;   // 0 = tone/noise, 1 = volume
        if (typ) {
            atten[ch] = val & 0x0Fu;
        } else if (ch == 3u) {
            noiseCtrl = val & 0x07u;
            noiseShift = 0x8000u;   // writing noise control resets the LFSR
        } else {
            freq[ch] = static_cast<u16>((freq[ch] & 0x3F0u) | (val & 0x0Fu));
        }
    } else {
        // Data byte: updates the register selected by latch
        const u32 ch  = (latch >> 5) & 3u;
        const u32 typ = (latch >> 4) & 1u;
        if (typ) {
            atten[ch] = val & 0x0Fu;
        } else if (ch != 3u) {
            // Tone frequency: high 6 bits go into upper part of the 10-bit reg
            freq[ch] = static_cast<u16>((freq[ch] & 0x0Fu) | ((val & 0x3Fu) << 4));
        }
        // Noise (ch==3) has no data-byte-only path beyond the latch write above
    }
}

float SN76489::renderSample() {
    float mix = 0.0f;

    // Tone channels 0-2: continuous phase accumulator per channel.
    // freq==0 is treated as "off" rather than the hardware's ultra-high
    // pitch, to avoid aliasing artifacts at audio rate — acceptable v1
    // simplification.
    for (u32 ch = 0; ch < 3u; ch++) {
        if (freq[ch] == 0u) continue;
        const double hz = PSG_CLOCK / (32.0 * static_cast<double>(freq[ch]));
        tonePhase[ch] += hz / static_cast<double>(GEN_AUDIO_RATE);
        if (tonePhase[ch] >= 1.0) tonePhase[ch] -= 1.0;
        const float out = (tonePhase[ch] < 0.5) ? 1.0f : -1.0f;
        mix += out * kPsgVolTable[atten[ch]];
    }

    // Noise channel: rate select 0-2 = fixed dividers, 3 = tone channel 2's
    // frequency. FB bit selects white (tapped bit0^bit3) vs periodic (bit0)
    // feedback into a 16-bit LFSR, matching documented SN76489 behavior.
    {
        const u32 rateSel = noiseCtrl & 0x03u;
        double noiseHz;
        if (rateSel == 3u) {
            noiseHz = (freq[2] != 0u)
                ? PSG_CLOCK / (32.0 * static_cast<double>(freq[2]))
                : 0.0;
        } else {
            const double divisor = 16.0 * static_cast<double>(1u << rateSel);
            noiseHz = PSG_CLOCK / divisor;
        }

        if (noiseHz > 0.0) {
            noisePhase += noiseHz / static_cast<double>(GEN_AUDIO_RATE);
            while (noisePhase >= 1.0) {
                noisePhase -= 1.0;
                const bool white = (noiseCtrl & 0x04u) != 0u;
                const u16 fb = white
                    ? static_cast<u16>((noiseShift & 1u) ^ ((noiseShift >> 3) & 1u))
                    : static_cast<u16>(noiseShift & 1u);
                noiseShift = static_cast<u16>((noiseShift >> 1) | (fb << 15));
            }
        }

        const float nOut = (noiseShift & 1u) ? 1.0f : -1.0f;
        mix += nOut * kPsgVolTable[atten[3]];
    }

    return mix * 0.25f;   // headroom across up to 4 simultaneous channels
}

void SN76489::clock(float* buf, u32 count) {
    for (u32 i = 0; i < count; i++) {
        const float s = renderSample();
        buf[i * 2u]     = s;
        buf[i * 2u + 1] = s;
    }
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
    // Both engines now produce real audio. FM dominates the mix slightly
    // since most games lean on it for music; PSG usually carries SFX.
    // Balance/levels are a tuning pass, not a correctness concern.
    for (u32 i = 0; i < samplesNeeded; i++) {
        const float ym = ym2612.renderSample();
        const float ps = psg.renderSample();
        float mixed = ym * 0.6f + ps * 0.4f;
        if (mixed > 1.0f) mixed = 1.0f;
        if (mixed < -1.0f) mixed = -1.0f;
        const float s[2] = { mixed, mixed };
        ding_audio_write_sample(&audioBuf, s);
    }
}
