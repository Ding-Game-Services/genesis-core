#include "genesis.h"
#include "ding_core.h"
#include "ding_audio.h"
#include "ding_md5.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>

#ifdef __EMSCRIPTEN__
#  include <emscripten.h>
#  define DING_EXPORT EMSCRIPTEN_KEEPALIVE
#else
#  define DING_EXPORT
#endif

// All exported symbols must have C linkage so Emscripten can find them
// by name via ccall/cwrap without C++ name mangling.
extern "C" {

static Genesis* s_gen = nullptr;

// SDK pattern: The SDK expects pointers to static data for capabilities
static DingCoreInfo    s_core_info;
static DingVideoInfo    s_video_info;
static DingAudioInfo    s_audio_info;
static DingSaveStateInfo s_save_info;
static DingRomIdentity  s_rom_id;

static const struct { const char* name; u8 idx; } s_inputs[] = {
    { "Up",    GEN_BTN_UP    }, { "Down",  GEN_BTN_DOWN  },
    { "Left",  GEN_BTN_LEFT  }, { "Right", GEN_BTN_RIGHT },
    { "A",     GEN_BTN_A     }, { "B",     GEN_BTN_B     },
    { "C",     GEN_BTN_C     }, { "Start", GEN_BTN_START },
    { "X",     GEN_BTN_X     }, { "Y",     GEN_BTN_Y     },
    { "Z",     GEN_BTN_Z     }, { "Mode",  GEN_BTN_MODE  },
};

static constexpr u32 INPUT_COUNT = GEN_BTN_COUNT; 
static constexpr u32 REGION_COUNT = 3;
static constexpr u32 SAVE_MAX = 256u * 1024u;

DING_EXPORT void ding_init() {
    if (!s_gen) s_gen = new Genesis();
    
    // Initialize static info structs
    s_core_info.core_name = "genesis-core";
    s_core_info.platform_name = "Sega Genesis / Mega Drive";
    s_core_info.version = "0.1.0";
    s_core_info.api_version_major = DING_CORE_API_VERSION_MAJOR;
    s_core_info.api_version_minor = DING_CORE_API_VERSION_MINOR;

    s_video_info.base_width = GEN_W;
    s_video_info.base_height = GEN_H_NTSC;
    s_video_info.max_width = GEN_W;
    s_video_info.max_height = GEN_H_MAX;
    s_video_info.format = DING_PIXFMT_RGBA8;
    s_video_info.dynamic = 1;

    s_audio_info.sample_rate = GEN_AUDIO_RATE;
    s_audio_info.channels = (u8)GEN_AUDIO_CHANNELS;

    s_save_info.method = DING_SAVE_FULL;
    s_save_info.max_size = SAVE_MAX;
    s_save_info.supported = 1;
}

DING_EXPORT void ding_destroy() {
    delete s_gen;
    s_gen = nullptr;
}

DING_EXPORT void ding_reset() {
    if (s_gen) s_gen->reset();
}

DING_EXPORT DingResult ding_load_rom(const u8* data, size_t len) {
    if (!s_gen) return DING_ERR_GENERIC;
    return s_gen->loadROM(data, (u32)len) ? DING_OK : DING_ERR_BAD_ROM;
}

DING_EXPORT DingResult ding_load_disc(DingDiscImage* /*disc*/) {
    return DING_ERR_NO_DISC; 
}

DING_EXPORT DingResult ding_load_bios(u32 /*index*/, const u8* /*data*/, size_t /*len*/) {
    return DING_OK;
}

DING_EXPORT u8 ding_is_disc_swap_pending() {
    return 0;
}

DING_EXPORT void ding_swap_disc(DingDiscImage* /*disc*/) {
    // No-op
}

DING_EXPORT void ding_set_region(const char* region) {
    if (!s_gen || !region) return;
    s_gen->setRegion(region[0] == 'P' || region[0] == 'p');
}

DING_EXPORT void ding_run_frame() {
    if (!s_gen) return;
    s_gen->runFrame();

    static int dbgCount = 0;
    if (++dbgCount == 60) {
        char buf[1024];
        s_gen->diagCPU(buf, sizeof(buf));
        printf("%s\n", buf);
		printf(
    "PC=%08X SR=%04X D0=%08X A0=%08X\n",
    s_gen->cpu.pc,
    s_gen->cpu.sr,
    s_gen->cpu.d[0],
    s_gen->cpu.a[0]
);

        // What instruction is at the stuck PC?
u16 w0 = s_gen->bus.read16(0x1200);
u16 w1 = s_gen->bus.read16(0x1202);
u16 w2 = s_gen->bus.read16(0x1204);
u16 w3 = s_gen->bus.read16(0x1206);
u16 w4 = s_gen->bus.read16(0x1208);
u16 w5 = s_gen->bus.read16(0x120A);
u16 w6 = s_gen->bus.read16(0x120C);
u16 w7 = s_gen->bus.read16(0x120E);
u16 w8 = s_gen->bus.read16(0x1210);
u16 w9 = s_gen->bus.read16(0x1212);
u16 wa = s_gen->bus.read16(0x1214);
u16 wb = s_gen->bus.read16(0x1216);
printf("1200: %04X %04X %04X %04X %04X %04X %04X %04X %04X %04X %04X %04X\n",
    w0,w1,w2,w3,w4,w5,w6,w7,w8,w9,wa,wb);
    }
}

DING_EXPORT const DingCoreInfo* ding_get_core_info() {
    return &s_core_info;
}

DING_EXPORT const DingVideoInfo* ding_get_video_info() {
    return &s_video_info;
}

DING_EXPORT const DingAudioInfo* ding_get_audio_info() {
    return &s_audio_info;
}

DING_EXPORT const DingRomIdentity* ding_get_rom_identity() {
    if (s_gen && !s_gen->bus.rom.empty()) {
        std::memset(&s_rom_id, 0, sizeof(s_rom_id));
        s_rom_id.method = DING_ID_MD5_FULL;
        ding_md5(s_gen->bus.rom.data(), (u32)s_gen->bus.rom.size(), s_rom_id.hash);
    }
    return &s_rom_id;
}

DING_EXPORT const DingSaveStateInfo* ding_get_savestate_info() {
    return &s_save_info;
}

DING_EXPORT u32 ding_get_memory_region_count() {
    u32 n = REGION_COUNT;
    if (s_gen && s_gen->bus.hasSRAM && s_gen->bus.sramSize > 0) n++;
    return n;
}

DING_EXPORT void ding_get_memory_region(u32 index, DingMemoryRegion* out) {
    if (!out || !s_gen || index >= ding_get_memory_region_count()) return;
    std::memset(out, 0, sizeof(*out));

    switch (index) {
        case 0:
            out->name = "WRAM";
            out->base_addr = 0xFF0000u;
            out->size = GEN_WRAM_SIZE;
            out->ptr = s_gen->bus.wram;
            out->access = DING_MEM_DIRECT;
            out->writable = 1;
            break;
        case 1:
            out->name = "Z80_RAM";
            out->base_addr = 0xA00000u;
            out->size = GEN_Z80RAM_SIZE;
            out->ptr = s_gen->bus.z80Ram;
            out->access = DING_MEM_DIRECT;
            out->writable = 1;
            break;
case 2:
            out->name = "VRAM";
            out->base_addr = 0x000000u;
            out->size = GEN_VRAM_SIZE;
            out->ptr = s_gen->vdp.vram;
            out->access = DING_MEM_DIRECT;
            out->writable = 0;
            break;
        case 3:
            out->name = "SRAM";
            out->base_addr = 0x200000u;
            out->size = s_gen->bus.sramSize;
            out->ptr = s_gen->bus.sramData;
            out->access = DING_MEM_DIRECT;
            out->writable = 1;
            break;
    }
}

DING_EXPORT u32 ding_get_bios_count() { return 0; }
DING_EXPORT void ding_get_bios_descriptor(u32 idx, DingBiosDescriptor* out) {
    if (out) std::memset(out, 0, sizeof(*out));
}

DING_EXPORT u32 ding_get_input_descriptor_count() { return INPUT_COUNT; }
DING_EXPORT void ding_get_input_descriptor(u32 index, DingInputDescriptor* out) {
    if (!out || index >= INPUT_COUNT) return;
    std::memset(out, 0, sizeof(*out));
    out->name = s_inputs[index].name;
    out->type = DING_INPUT_BUTTON;
    out->index = s_inputs[index].idx;
}

DING_EXPORT const u8* ding_get_framebuffer() {
    return s_gen ? s_gen->vdp.framebuf : nullptr;
}

DING_EXPORT void ding_get_current_dimensions(u32* w, u32* h) {
    if (!w || !h) return;
    *w = GEN_W;
    *h = (s_gen && s_gen->isPAL) ? GEN_H_PAL : GEN_H_NTSC;
}

DING_EXPORT u32 ding_get_audio_sample_count() {
    return s_gen ? ding_audio_available(&s_gen->apu.audioBuf) : 0;
}

DING_EXPORT u32 ding_read_audio_samples(float* buf, u32 count) {
    if (!s_gen || !buf) return 0;
    ding_audio_read(&s_gen->apu.audioBuf, buf, count);
    return count;
}

DING_EXPORT void ding_set_button(u8 port, u8 index, u8 pressed) {
    if (s_gen && index < GEN_BTN_COUNT)
        s_gen->pressButton(port, index, pressed != 0);
}

DING_EXPORT void ding_set_axis(u8 port, u8 index, int16_t value) {}

DING_EXPORT size_t ding_save_state(u8* buf, size_t size) {
    if (!s_gen || !buf) return 0;
    u32 outSize = 0;
    if (s_gen->saveState(buf, (u32)size, &outSize)) return (size_t)outSize;
    return 0;
}

DING_EXPORT DingResult ding_load_state(const u8* buf, size_t len) {
    if (!s_gen || !buf) return DING_ERR_BAD_STATE;
    return s_gen->loadState(buf, (u32)len) ? DING_OK : DING_ERR_BAD_STATE;
}

DING_EXPORT size_t ding_diag_cpu_state(char* buf, size_t size) {
    if (!s_gen || !buf) return 0;
    s_gen->diagCPU(buf, (u32)size);
    return (size_t)std::strlen(buf);
}

DING_EXPORT size_t ding_diag_video_state(char* buf, size_t size) {
    if (!s_gen || !buf) return 0;
    s_gen->diagVideo(buf, (u32)size);
    return (size_t)std::strlen(buf);
}

DING_EXPORT u8 ding_has_error() {
    return (s_gen && s_gen->errorFlag) ? 1 : 0;
}

// ── Memory/SRAM helpers for JS frontend ──────────────────────────────────────
DING_EXPORT u8*  ding_get_wram()          { return s_gen ? s_gen->bus.wram : nullptr; }
DING_EXPORT u32  ding_get_wram_size()     { return GEN_WRAM_SIZE; }
DING_EXPORT u8*  ding_get_sram()          { return (s_gen && s_gen->bus.hasSRAM) ? s_gen->bus.sramData : nullptr; }
DING_EXPORT u32  ding_get_sram_size()     { return (s_gen && s_gen->bus.hasSRAM) ? s_gen->bus.sramSize : 0u; }
DING_EXPORT u8   ding_sram_has()          { return (s_gen && s_gen->bus.hasSRAM && s_gen->bus.sramSize > 0) ? 1 : 0; }
DING_EXPORT u8   ding_sram_dirty()        { return (s_gen && s_gen->bus.sramDirty) ? 1 : 0; }
DING_EXPORT void ding_sram_clear_dirty()  { if (s_gen) s_gen->bus.sramDirty = false; }
DING_EXPORT void ding_write8(u32 addr, u8 val) { if (s_gen) s_gen->bus.write8(addr, val); }

DING_EXPORT const char* ding_diag_last_error() {
    return (s_gen && s_gen->errorFlag) ? s_gen->errorMsg : nullptr;
}

} // extern "C"
