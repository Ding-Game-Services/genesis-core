#include "genesis.h"
#include "ding_core.h"
#include "ding_audio.h"
#include "ding_md5.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

// ── Emscripten keepalive / native export ──────────────────────────────────────
// DING_EXPORT ensures the linker never strips these functions.
// CMakeLists.txt also lists every function in EXPORTED_FUNCTIONS as a second
// line of defence for the WASM build.
#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#  define DING_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#  define DING_EXPORT
#endif

// ── Single core instance ──────────────────────────────────────────────────────
// One Genesis object per loaded module.  WASM modules are isolated, so this
// global is safe; native callers must not load the same .dll twice in one
// process without calling ding_destroy() between uses.
static Genesis* s_gen = nullptr;

// ─────────────────────────────────────────────────────────────────────────────
// Input descriptor table
// Lists the 12 buttons that exist on every Genesis pad.
// The `index` field maps 1:1 to GenBtn enum values (0–11).
// ding_set_button(port, index, pressed) uses the same indices, so no
// translation is needed at runtime.
// ─────────────────────────────────────────────────────────────────────────────
static const struct { const char* name; u8 idx; } s_inputs[] = {
    { "Up",    GEN_BTN_UP    }, { "Down",  GEN_BTN_DOWN  },
    { "Left",  GEN_BTN_LEFT  }, { "Right", GEN_BTN_RIGHT },
    { "A",     GEN_BTN_A     }, { "B",     GEN_BTN_B     },
    { "C",     GEN_BTN_C     }, { "Start", GEN_BTN_START },
    { "X",     GEN_BTN_X     }, { "Y",     GEN_BTN_Y     },
    { "Z",     GEN_BTN_Z     }, { "Mode",  GEN_BTN_MODE  },
};
static constexpr u32 INPUT_COUNT = GEN_BTN_COUNT;   // 12

// ─────────────────────────────────────────────────────────────────────────────
// Memory regions exposed to Cockpit and the achievement engine
//
//   0  WRAM    64 KB  0xFF0000  — the region ding-engine.js addresses
//   1  Z80_RAM  8 KB  0xA00000  — sound driver working RAM
//   2  VRAM    64 KB  0x000000  — VDP-internal; useful for tile inspection
//
// All three use DING_MEM_DIRECT so consumers can read them without callbacks.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr u32 REGION_COUNT = 3;

// ─────────────────────────────────────────────────────────────────────────────
// Max save-state buffer size
// Actual size varies by game (SRAM presence) but stays well under 256 KB.
// ─────────────────────────────────────────────────────────────────────────────
static constexpr u32 SAVE_MAX = 256u * 1024u;

// =============================================================================
// Public API — all functions are extern "C" so the linker and Emscripten see
// plain C symbol names (no C++ name mangling).
// =============================================================================
extern "C" {

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────
DING_EXPORT void ding_init() {
    if (!s_gen) s_gen = new Genesis();
}

DING_EXPORT void ding_destroy() {
    delete s_gen;
    s_gen = nullptr;
}

DING_EXPORT void ding_reset() {
    if (s_gen) s_gen->reset();
}

// ─────────────────────────────────────────────────────────────────────────────
// Loading
// ─────────────────────────────────────────────────────────────────────────────
DING_EXPORT ding_bool ding_load_rom(const u8* data, u32 len) {
    if (!s_gen) return DING_FALSE;
    return s_gen->loadROM(data, len) ? DING_TRUE : DING_FALSE;
}

DING_EXPORT ding_bool ding_load_disc(DingDiscImage* /*disc*/) {
    return DING_FALSE;   // Genesis is cartridge-only
}

DING_EXPORT ding_bool ding_load_bios(u32 /*index*/, const u8* /*data*/, u32 /*size*/) {
    // The Genesis TMSS register accepts any write — no BIOS file required.
    return DING_TRUE;
}

DING_EXPORT ding_bool ding_swap_disc(DingDiscImage* /*disc*/) {
    return DING_FALSE;
}

// ─────────────────────────────────────────────────────────────────────────────
// Run
// ─────────────────────────────────────────────────────────────────────────────
DING_EXPORT void ding_run_frame() {
    if (s_gen) s_gen->runFrame();
}

// ─────────────────────────────────────────────────────────────────────────────
// Capability queries
// ─────────────────────────────────────────────────────────────────────────────
DING_EXPORT void ding_get_core_info(DingCoreInfo* out) {
    if (!out) return;
    out->name     = "genesis-core";
    out->platform = "Sega Genesis / Mega Drive";
    out->version  = "0.1.0";
}

DING_EXPORT void ding_get_video_info(DingVideoInfo* out) {
    if (!out) return;
    out->base_width  = GEN_W;
    out->base_height = GEN_H_NTSC;   // default; actual height is dynamic
    out->max_width   = GEN_W;
    out->max_height  = GEN_H_MAX;    // 240 — covers both NTSC (224) and PAL (240)
    out->format      = DING_PIXEL_RGBA8;
    out->dynamic     = DING_TRUE;    // height changes when region is PAL
}

DING_EXPORT void ding_get_audio_info(DingAudioInfo* out) {
    if (!out) return;
    out->sample_rate = GEN_AUDIO_RATE;
    out->channels    = static_cast<u8>(GEN_AUDIO_CHANNELS);
}

DING_EXPORT void ding_get_rom_identity(DingRomIdentity* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    out->method = DING_ID_MD5_FULL;
    if (s_gen && !s_gen->bus.rom.empty())
        ding_md5(s_gen->bus.rom.data(),
                 static_cast<u32>(s_gen->bus.rom.size()),
                 out->hash);
}

DING_EXPORT void ding_get_savestate_info(DingSaveStateInfo* out) {
    if (!out) return;
    out->method    = DING_SAVE_FULL;
    out->max_size  = SAVE_MAX;
    out->supported = DING_TRUE;
}

DING_EXPORT u32 ding_get_memory_region_count() {
    return REGION_COUNT;
}

DING_EXPORT void ding_get_memory_region(u32 index, DingMemoryRegion* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    if (!s_gen || index >= REGION_COUNT) return;

    switch (index) {
        case 0:
            // Main working RAM — this is the address space ding-engine.js
            // uses for achievements (keys "0xFF0000"–"0xFFFFFF").
            out->name      = "WRAM";
            out->base_addr = 0xFF0000u;
            out->size      = GEN_WRAM_SIZE;
            out->ptr       = s_gen->bus.wram;
            out->writable  = 1u;
            out->direct    = 1u;
            break;
        case 1:
            // Z80 sound driver RAM
            out->name      = "Z80_RAM";
            out->base_addr = 0xA00000u;
            out->size      = GEN_Z80RAM_SIZE;
            out->ptr       = s_gen->z80.ram;
            out->writable  = 1u;
            out->direct    = 1u;
            break;
        case 2:
            // VDP video RAM — useful in Cockpit for tile/nametable inspection.
            // Base address 0 is VDP-internal (not a 68K bus address).
            out->name      = "VRAM";
            out->base_addr = 0x000000u;
            out->size      = GEN_VRAM_SIZE;
            out->ptr       = s_gen->vdp.vram;
            out->writable  = 0u;
            out->direct    = 1u;
            break;
        default: break;
    }
}

DING_EXPORT u32 ding_get_bios_count() {
    return 0u;   // no BIOS required
}

DING_EXPORT void ding_get_bios_descriptor(u32 /*index*/, DingBiosDescriptor* out) {
    if (out) std::memset(out, 0, sizeof(*out));
}

DING_EXPORT u32 ding_get_input_descriptor_count() {
    return INPUT_COUNT;
}

DING_EXPORT void ding_get_input_descriptor(u32 index, DingInputDescriptor* out) {
    if (!out) return;
    std::memset(out, 0, sizeof(*out));
    if (index >= INPUT_COUNT) return;
    out->name  = s_inputs[index].name;
    out->type  = DING_INPUT_BUTTON;
    out->index = s_inputs[index].idx;
}

// ─────────────────────────────────────────────────────────────────────────────
// Video
// ─────────────────────────────────────────────────────────────────────────────
DING_EXPORT const u8* ding_get_framebuffer() {
    return s_gen ? s_gen->vdp.framebuf : nullptr;
}

DING_EXPORT void ding_get_current_dimensions(u32* w, u32* h) {
    if (!w || !h) return;
    *w = GEN_W;
    *h = (s_gen && s_gen->isPAL) ? GEN_H_PAL : GEN_H_NTSC;
}

// ─────────────────────────────────────────────────────────────────────────────
// Audio
// The frontend polls ding_get_audio_sample_count() each frame and reads
// whatever is available into its own buffer for Web Audio / SDL output.
// ─────────────────────────────────────────────────────────────────────────────
DING_EXPORT u32 ding_get_audio_sample_count() {
    if (!s_gen) return 0u;
    return ding_audio_available(&s_gen->apu.audioBuf);
}

DING_EXPORT u32 ding_read_audio_samples(float* buf, u32 count) {
    if (!s_gen || !buf || !count) return 0u;
    // ding_audio_read() silence-pads if the ring buffer runs dry.
    ding_audio_read(&s_gen->apu.audioBuf, buf, count);
    return count;
}

// ─────────────────────────────────────────────────────────────────────────────
// Input
// ─────────────────────────────────────────────────────────────────────────────
DING_EXPORT void ding_set_button(u32 port, u32 index, u8 pressed) {
    // index maps 1:1 to GenBtn (0=Up … 11=Mode) — no translation needed.
    if (s_gen && index < GEN_BTN_COUNT)
        s_gen->pressButton(port, index, pressed != 0u);
}

DING_EXPORT void ding_set_axis(u32 /*port*/, u32 /*index*/, float /*value*/) {
    // Genesis controllers have no analog axes.
}

// ─────────────────────────────────────────────────────────────────────────────
// Save states
// ─────────────────────────────────────────────────────────────────────────────
DING_EXPORT ding_bool ding_save_state(u8* buf, u32 size, u32* out_size) {
    if (!s_gen) return DING_FALSE;
    return s_gen->saveState(buf, size, out_size) ? DING_TRUE : DING_FALSE;
}

DING_EXPORT ding_bool ding_load_state(const u8* buf, u32 size) {
    if (!s_gen) return DING_FALSE;
    return s_gen->loadState(buf, size) ? DING_TRUE : DING_FALSE;
}

// ─────────────────────────────────────────────────────────────────────────────
// Diagnostics
// ─────────────────────────────────────────────────────────────────────────────
DING_EXPORT void ding_diag_cpu_state(char* out, u32 size) {
    if      (s_gen) s_gen->diagCPU(out, size);
    else if (out && size) out[0] = '\0';
}

DING_EXPORT void ding_diag_video_state(char* out, u32 size) {
    if      (s_gen) s_gen->diagVideo(out, size);
    else if (out && size) out[0] = '\0';
}

DING_EXPORT ding_bool ding_has_error() {
    return (s_gen && s_gen->errorFlag) ? DING_TRUE : DING_FALSE;
}

DING_EXPORT void ding_diag_last_error(char* out, u32 size) {
    if (!out || !size) return;
    if (s_gen && s_gen->errorFlag)
        std::snprintf(out, size, "%s", s_gen->errorMsg);
    else
        out[0] = '\0';
}

DING_EXPORT ding_bool ding_is_disc_swap_pending() {
    return DING_FALSE;   // cartridge system
}

}  // extern "C"
