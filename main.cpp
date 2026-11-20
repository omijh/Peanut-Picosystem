#include <algorithm>
#include <cstdio>
#include <cstring>
#include "picosystem.hpp"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#ifndef GB_AUDIO_HOST_TEST
#include "hardware/pio.h"
#include "hardware/spi.h"
#include "pico/platform.h"
#include "pico/time.h"
#else
#ifndef __not_in_flash_func
#define __not_in_flash_func(fn) fn
#endif
#endif

// ============================================================================
// ROM & Cartridge RAM Storage + Devirtualized Walnut-CGB Hooks
// ============================================================================

#if __has_include("rom.h")
    #include "rom.h"
    #define ROM_DATA custom_rom_data
    #define ROM_SIZE custom_rom_len
#else
    #include "dmg-acid2.gb.h"
    #define ROM_DATA dmg_acid2_gb
    #define ROM_SIZE sizeof(dmg_acid2_gb)
#endif

// 32 KB Cartridge RAM (covers MBC1, MBC2, MBC3, MBC5 / Pokemon Gen 1 & 2)
#define CART_RAM_MAX_SIZE (32 * 1024)
static uint8_t cart_ram[CART_RAM_MAX_SIZE];
static bool cart_ram_dirty = false;
static uint32_t last_cart_write_ms = 0;

static inline uint8_t wgb_fast_rom_read(uint_fast32_t addr) {
    return (addr < ROM_SIZE) ? ((const uint8_t *)ROM_DATA)[addr] : 0xFFu;
}

static inline uint16_t wgb_fast_rom_read16(uint_fast32_t addr) {
    if (addr + 1u < ROM_SIZE) {
        const uint8_t *p = (const uint8_t *)ROM_DATA + addr;
        return (uint16_t)p[0] | ((uint16_t)p[1] << 8);
    }
    return (uint16_t)wgb_fast_rom_read(addr) | ((uint16_t)wgb_fast_rom_read(addr + 1u) << 8);
}

static inline uint32_t wgb_fast_rom_read32(uint_fast32_t addr) {
    if (addr + 3u < ROM_SIZE) {
        const uint8_t *p = (const uint8_t *)ROM_DATA + addr;
        return (uint32_t)p[0]
             | ((uint32_t)p[1] << 8)
             | ((uint32_t)p[2] << 16)
             | ((uint32_t)p[3] << 24);
    }
    return (uint32_t)wgb_fast_rom_read(addr)
         | ((uint32_t)wgb_fast_rom_read(addr + 1u) << 8)
         | ((uint32_t)wgb_fast_rom_read(addr + 2u) << 16)
         | ((uint32_t)wgb_fast_rom_read(addr + 3u) << 24);
}

static inline uint8_t wgb_fast_cart_ram_read(uint_fast32_t addr) {
    return (addr < CART_RAM_MAX_SIZE) ? cart_ram[addr] : 0xFFu;
}

static inline void wgb_fast_cart_ram_write(uint_fast32_t addr, uint8_t val) {
    if (addr < CART_RAM_MAX_SIZE && cart_ram[addr] != val) {
        cart_ram[addr] = val;
        cart_ram_dirty = true;
        last_cart_write_ms = picosystem::time();
    }
}

#define WGB_ROM_READ(gb_ptr, a)          wgb_fast_rom_read(a)
#define WGB_ROM_READ16(gb_ptr, a)        wgb_fast_rom_read16(a)
#define WGB_ROM_READ32(gb_ptr, a)        wgb_fast_rom_read32(a)
#define WGB_CART_RAM_READ(gb_ptr, a)     wgb_fast_cart_ram_read(a)
#define WGB_CART_RAM_WRITE(gb_ptr, a, v) wgb_fast_cart_ram_write((a), (v))

#ifndef GB_AUDIO_HOST_TEST
#define WGB_RAM_DATA __not_in_flash("wgb_tables")
#else
#define WGB_RAM_DATA
#endif

#define ENABLE_LCD 1
#define ENABLE_SOUND 1
#define WALNUT_GB_12_COLOUR 1
#define WALNUT_FULL_GBC_SUPPORT 1
uint8_t audio_read(const uint16_t addr);
void audio_write(const uint16_t addr, const uint8_t val);
#include "walnut_cgb.h"
#include "gb_audio.hpp"

using namespace picosystem;

// Emulator & audio context
static struct gb_s gb;
static GbAudioDriver gb_audio;

uint8_t audio_read(const uint16_t addr) {
    return gb_audio.apu.read(addr);
}

void audio_write(const uint16_t addr, const uint8_t val) {
    if (addr >= 0xFF10u && addr <= 0xFF3Fu) {
        gb_audio.apu.write(addr, val);
        if (addr >= 0xFF30u) {
            gb.hram_io[addr - 0xFF00u] = val;
        } else if (addr == 0xFF26u) {
            gb.hram_io[0x26] = val & 0x80u;
            if ((val & 0x80u) == 0u) {
                memset(gb.hram_io + 0x10, 0, 0x16);
            }
        } else if (gb_audio.apu.power) {
            gb.hram_io[addr - 0xFF00u] = val;
        }
    }
}

// ============================================================================
// UI State, Dynamic Frame Timings & Battery Status
// ============================================================================

static uint32_t combo_hold_frames = 60;      // ~1.0s (recalibrated by ST7789 TE period)
static uint32_t combo_hint_frames = 15;      // ~0.25s
static uint32_t banner_duration_frames = 90; // ~1.5s
static uint32_t banner_short_frames = 60;    // ~1.0s

static uint32_t banner_timer = 0;
static const char *banner_text = "";
static color_t banner_bg_color = rgb(0, 12, 0);

static uint32_t save_hold_timer = 0;
static uint32_t load_hold_timer = 0;
static bool save_combo_consumed = false;
static bool load_combo_consumed = false;

static uint8_t brightness_pct = 30; // 10..100%
static constexpr int MENU_ITEM_COUNT = 5;
static bool menu_open = false;
static bool menu_exit_mask = false;
static bool menu_ignore_left_hold = false;
static int menu_cursor = 0;
static uint32_t menu_repeat_timer = 0;

static constexpr uint32_t CHARGE_STAT_PIN = 24u; // open-drain STAT, active low
static constexpr uint32_t CHARGE_VBUS_PIN = 2u;  // VBUS detect, active high
static bool cached_charging = false;
static uint32_t cached_battery_pct = 100u;
static uint32_t menu_batt_timer = 0;

static void poll_battery_status() {
    cached_battery_pct = std::min<uint32_t>(battery(), 100u);
#ifndef GB_AUDIO_HOST_TEST
    cached_charging = gpio_get(CHARGE_VBUS_PIN) && !gpio_get(CHARGE_STAT_PIN);
#endif
}

// ============================================================================
// Color Modes, Matrix/Curve Filter Pipeline & Blended Display Scaling
// ============================================================================

enum ColorMode : uint8_t {
    COLOR_GBC_LCD   = 0, // CGB: GBC LCD bleed+lift; DMG: Teal Light (default)
    COLOR_RAW       = 1, // CGB: Unfiltered native color; DMG: 12-Color GBC-style palette
    COLOR_VIVID     = 2, // CGB & 12-Color DMG: Saturation boost
    COLOR_PASTEL    = 3, // CGB & 12-Color DMG: Soft high-key pastel
    COLOR_TEAL      = 4, // 4-shade Game Boy Light Teal
    COLOR_DMG_GREEN = 5, // 4-shade Classic DMG Olive Green
    COLOR_POCKET_BW = 6, // 4-shade Game Boy Pocket Monochrome
    COLOR_SEPIA     = 7, // Warm lifted sepia monochrome
    COLOR_NIGHT     = 8, // Low-blue amber night mode
    COLOR_MODE_COUNT
};
static ColorMode color_mode = COLOR_GBC_LCD;

enum DisplayMode {
    DISPLAY_FULLSCREEN = 0,    // 240x240 Edge-to-Edge blended 5:3 vertical / 3:2 horizontal
    DISPLAY_ASPECT_FIT = 1,    // 240x216 Exact 10:9 aspect ratio with 1.5x blended scaling
    DISPLAY_PIXEL_PERFECT = 2  // 160x144 Centered 1:1 with retro bezel
};
static DisplayMode display_mode = DISPLAY_FULLSCREEN;

// Filter tables & converted CGB / DMG palettes (in SRAM for fast scanlines)
static uint8_t _cf_x5[32];
static int32_t _cf_mat[9];
static uint8_t _cf_out[3][256];

static uint16_t _pal_src[0x40];
static color_t  _pal_lut[0x40];
static bool     pal_lut_stale = true;

// [layer: 0=OBJ0, 1=OBJ1, 2=BG][shade: 0..3]
static color_t _dmg_lut[3][4];
// Previous horizontally-scaled scanline for vertical blend_avg smoothing
static color_t prev_row[240];

static constexpr int32_t CF_LR = 77, CF_LG = 150, CF_LB = 29;

static inline constexpr color_t rgb565_to_color(uint16_t c) {
    uint8_t r4 = (c >> 12) & 0xFu;
    uint8_t g4 = (c >> 7)  & 0xFu;
    uint8_t b4 = (c >> 1)  & 0xFu;
    return (color_t)(r4 | (0xFu << 4) | (b4 << 8) | (g4 << 12));
}

static inline int32_t cf_clamp8(int32_t v) {
    v &= ~(v >> 31);
    const int32_t t = 255 - v;
    return 255 - (t & ~(t >> 31));
}

static int32_t cf_lift(int32_t v, int32_t amount) {
    const int32_t iv = 255 - v;
    return v + ((((255 - ((iv * iv) >> 8)) - v) * amount) >> 8);
}

static void cf_sat_matrix(int32_t s) {
    static constexpr int32_t L[3] = { CF_LR, CF_LG, CF_LB };
    for (int32_t i = 0; i < 3; ++i) {
        int32_t row = 0;
        for (int32_t j = 0; j < 3; ++j) {
            if (i == j) {
                continue;
            }
            const int32_t v = ((256 - s) * L[j] + 128) >> 8;
            _cf_mat[i * 3 + j] = v;
            row += v;
        }
        _cf_mat[i * 3 + i] = 256 - row;
    }
}

static const uint8_t CF_RAMP_TEAL[4][3] = {
    { 0x10, 0x30, 0x30 }, { 0x30, 0x70, 0x70 },
    { 0x60, 0xC0, 0xB0 }, { 0xB0, 0xF0, 0xE0 }
};
static const uint8_t CF_RAMP_GREEN[4][3] = {
    { 0x10, 0x30, 0x10 }, { 0x40, 0x80, 0x30 },
    { 0x90, 0xD0, 0x40 }, { 0xD0, 0xF0, 0x60 }
};
static const uint8_t CF_RAMP_POCKET[4][3] = {
    { 0x00, 0x00, 0x00 }, { 0x50, 0x50, 0x50 },
    { 0xA0, 0xA0, 0xA0 }, { 0xF0, 0xF0, 0xF0 }
};

static void cf_build_tables(ColorMode mode) {
    for (int32_t v = 0; v < 32; ++v) {
        _cf_x5[v] = (uint8_t)((v * 255) / 31);
    }

    switch (mode) {
        case COLOR_GBC_LCD: {
            static constexpr int32_t GBC_BLEED[9] = {
                200,  40,  16,
                 32, 184,  40,
                 32,  48, 176
            };
            memcpy(_cf_mat, GBC_BLEED, sizeof(_cf_mat));
            break;
        }
        case COLOR_VIVID:  cf_sat_matrix(352); break; // 1.375x saturation
        case COLOR_PASTEL: cf_sat_matrix(141); break; // 0.55x desaturation
        case COLOR_NIGHT:  cf_sat_matrix(192); break; // 0.75x saturation
        case COLOR_RAW:    cf_sat_matrix(256); break; // Identity
        default:           cf_sat_matrix(0);   break; // Luma monochrome / 4-shade ramp
    }

    for (int32_t v = 0; v < 256; ++v) {
        int32_t c[3];
        switch (mode) {
            case COLOR_GBC_LCD:
                c[0] = c[1] = c[2] = cf_lift(v, 128);
                break;
            case COLOR_VIVID:
                c[0] = c[1] = c[2] = 128 + (((v - 128) * 9) >> 3);
                break;
            case COLOR_PASTEL:
                c[0] = c[1] = c[2] = cf_lift(v, 190);
                break;
            case COLOR_SEPIA: {
                const int32_t l = cf_lift(v, 64);
                c[0] = (l * 275) >> 8;
                c[1] = (l * 238) >> 8;
                c[2] = (l * 184) >> 8;
                break;
            }
            case COLOR_NIGHT: {
                const int32_t l = cf_lift(v, 96);
                c[0] = (l * 272) >> 8;
                c[1] = (l * 230) >> 8;
                c[2] = (l * 140) >> 8;
                break;
            }
            case COLOR_TEAL:
            case COLOR_DMG_GREEN:
            case COLOR_POCKET_BW: {
                const uint8_t (*ramp)[3] = (mode == COLOR_DMG_GREEN) ? CF_RAMP_GREEN
                                         : (mode == COLOR_POCKET_BW) ? CF_RAMP_POCKET
                                                                     : CF_RAMP_TEAL;
                const int32_t shade = v >> 6; // 0 (darkest) .. 3 (lightest)
                c[0] = ramp[shade][0];
                c[1] = ramp[shade][1];
                c[2] = ramp[shade][2];
                break;
            }
            default:
                c[0] = c[1] = c[2] = v;
                break;
        }
        for (int32_t i = 0; i < 3; ++i) {
            _cf_out[i][v] = (uint8_t)(cf_clamp8(c[i]) >> 4);
        }
    }
}

static color_t __not_in_flash_func(rgb565_to_color_filtered)(uint16_t c) {
    const int32_t r = _cf_x5[(c >> 11) & 0x1Fu];
    const int32_t g = _cf_x5[(c >> 6)  & 0x1Fu];
    const int32_t b = _cf_x5[c         & 0x1Fu];

    const uint32_t nr = _cf_out[0][cf_clamp8((r * _cf_mat[0] + g * _cf_mat[1] + b * _cf_mat[2]) >> 8)];
    const uint32_t ng = _cf_out[1][cf_clamp8((r * _cf_mat[3] + g * _cf_mat[4] + b * _cf_mat[5]) >> 8)];
    const uint32_t nb = _cf_out[2][cf_clamp8((r * _cf_mat[6] + g * _cf_mat[7] + b * _cf_mat[8]) >> 8)];

    return (color_t)(nr | (0xFu << 4) | (nb << 8) | (ng << 12));
}

static void apply_color_mode(ColorMode next_mode) {
    color_mode = next_mode;
    cf_build_tables(next_mode);

    // 4-shade neutral ramp in RGB565 (lightest -> darkest)
    static constexpr uint16_t DMG_GREY565[4] = { 0xFFFFu, 0xAD55u, 0x52AAu, 0x0000u };
    // GBC-style 12-color DMG auto-palette (OBJ0 = warm red/orange, OBJ1 = ocean blue, BG = crisp cream/sage/forest)
    static constexpr uint16_t DMG_12C_565[3][4] = {
        { 0xFFFEu, 0xFB46u, 0xC164u, 0x2082u }, // 0: OBJ0
        { 0xFFFEu, 0x65FFu, 0x1B98u, 0x08E8u }, // 1: OBJ1
        { 0xFFFEu, 0x8F4Eu, 0x35C9u, 0x10C5u }  // 2: BG
    };

    if (next_mode == COLOR_GBC_LCD) {
        // Classic DMG games keep user's Teal default under COLOR_GBC_LCD while CGB gets LCD bleed+lift
        for (int layer = 0; layer < 3; ++layer) {
            for (int s = 0; s < 4; ++s) {
                const uint8_t *rgb8 = CF_RAMP_TEAL[3 - s];
                _dmg_lut[layer][s] = rgb(rgb8[0] >> 4, rgb8[1] >> 4, rgb8[2] >> 4);
            }
        }
    } else if (next_mode == COLOR_RAW || next_mode == COLOR_VIVID || next_mode == COLOR_PASTEL) {
        for (int layer = 0; layer < 3; ++layer) {
            for (int s = 0; s < 4; ++s) {
                uint16_t c = DMG_12C_565[layer][s];
                _dmg_lut[layer][s] = (next_mode == COLOR_RAW) ? rgb565_to_color(c)
                                                              : rgb565_to_color_filtered(c);
            }
        }
    } else {
        for (int layer = 0; layer < 3; ++layer) {
            for (int s = 0; s < 4; ++s) {
                _dmg_lut[layer][s] = rgb565_to_color_filtered(DMG_GREY565[s]);
            }
        }
    }

    pal_lut_stale = true;
    gb.cgb.fixPaletteDirty = 1;
}

// Packed RGBA4444 channel-wise 50/50 average without unpacking (branchless)
static inline color_t blend_avg(color_t a, color_t b) {
    return (color_t)((a & b) + (((a ^ b) & 0xEEEEu) >> 1));
}

// ============================================================================
// Power-Loss-Safe A/B Ping-Pong Flash Save State Engine
// ============================================================================
// PicoSystem has 16 MB Flash. Reserve two 128 KB ping-pong slots at 15.0 MB and 15.125 MB.
#define FLASH_STATE_SLOT_A    (15u * 1024u * 1024u)
#define FLASH_STATE_SLOT_B    (15u * 1024u * 1024u + 128u * 1024u)
#define FLASH_STATE_SLOT_SIZE (128u * 1024u)
#define STATE_MAGIC           0x53544154u // "STAT"
#define STATE_VERSION         3u
static constexpr uint8_t SETTINGS_MAGIC_BYTE = 0xA5u;

#ifdef PICO_FLASH_SIZE_BYTES
static_assert(PICO_FLASH_SIZE_BYTES >= FLASH_STATE_SLOT_B + FLASH_STATE_SLOT_SIZE,
              "PICO_FLASH_SIZE_BYTES too small for A/B save slots; build with PICO_BOARD=pimoroni_picosystem");
#endif

struct StateHeader {
    uint32_t magic;          // STATE_MAGIC ("STAT")
    uint32_t version;        // STATE_VERSION (3)
    uint32_t seq;            // Monotonic sequence number for A/B slot ordering
    uint32_t gb_size;        // sizeof(struct gb_s)
    uint32_t ram_size;       // sizeof(cart_ram)
    uint32_t checksum;       // FNV-1a 32-bit hash of gb + cart_ram
    uint32_t rom_hash;       // FNV-1a 32-bit hash of ROM header + ROM_SIZE
    uint8_t  settings_magic; // SETTINGS_MAGIC_BYTE when settings fields are valid
    uint8_t  volume_pct;     // 0..100
    uint8_t  brightness_pct; // 10..100
    uint8_t  audio_mode;     // AudioMode (0..2)
    uint8_t  color_mode;     // ColorMode (0..COLOR_MODE_COUNT-1)
    uint8_t  display_mode;   // DisplayMode (0..2)
    uint8_t  reserved[6];
};

static_assert(sizeof(StateHeader) == 40, "StateHeader must remain 40 bytes");
static_assert(FLASH_PAGE_SIZE + sizeof(gb) + sizeof(cart_ram) <= FLASH_STATE_SLOT_SIZE,
              "Save state exceeds 128 KB Flash slot");

static inline uint32_t slot_offset_for_idx(int idx) {
    return (idx == 1) ? FLASH_STATE_SLOT_B : FLASH_STATE_SLOT_A;
}

static inline const uint8_t *flash_nocache_ptr(uint32_t offset) {
    return (const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + offset);
}

// Streaming flash page writer (uses only 256 bytes of stack RAM)
struct FlashWriter {
    uint32_t offset;
    uint8_t page[FLASH_PAGE_SIZE];
    size_t page_idx;

    FlashWriter(uint32_t start_offset) : offset(start_offset), page_idx(0) {
        memset(page, 0xFF, sizeof(page));
    }

    void write_bytes(const uint8_t *src, size_t len) {
        for (size_t i = 0; i < len; ++i) {
            page[page_idx++] = src[i];
            if (page_idx == FLASH_PAGE_SIZE) {
                flash_range_program(offset, page, FLASH_PAGE_SIZE);
                offset += FLASH_PAGE_SIZE;
                page_idx = 0;
                memset(page, 0xFF, sizeof(page));
            }
        }
    }

    void flush() {
        if (page_idx > 0) {
            flash_range_program(offset, page, FLASH_PAGE_SIZE);
            offset += FLASH_PAGE_SIZE;
            page_idx = 0;
        }
    }
};

static uint32_t fnv1a_update(uint32_t h, const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; ++i) {
        h ^= data[i];
        h *= 16777619u;
    }
    return h;
}

static uint32_t calc_rom_hash() {
    uint32_t h = 2166136261u;
    uint32_t rom_size = (uint32_t)ROM_SIZE;
    uint8_t size_bytes[4] = {
        (uint8_t)(rom_size & 0xFFu),
        (uint8_t)((rom_size >> 8) & 0xFFu),
        (uint8_t)((rom_size >> 16) & 0xFFu),
        (uint8_t)((rom_size >> 24) & 0xFFu)
    };
    h = fnv1a_update(h, size_bytes, sizeof(size_bytes));
    if (ROM_SIZE > 0x0100u) {
        size_t hdr_end = ROM_SIZE < 0x0150u ? (size_t)ROM_SIZE : 0x0150u;
        h = fnv1a_update(h, (const uint8_t *)ROM_DATA + 0x0100u, hdr_end - 0x0100u);
    }
    return h;
}

static uint32_t calc_state_checksum(const uint8_t *gb_bytes, const uint8_t *ram_bytes) {
    uint32_t h = 2166136261u;
    h = fnv1a_update(h, gb_bytes, sizeof(gb));
    h = fnv1a_update(h, ram_bytes, sizeof(cart_ram));
    return h;
}

static bool verify_flash_slot(int slot_idx, StateHeader *out_hdr = nullptr) {
    uint32_t slot_off = slot_offset_for_idx(slot_idx);
    const uint8_t *base = flash_nocache_ptr(slot_off);
    StateHeader hdr;
    memcpy(&hdr, base, sizeof(hdr));
    if (hdr.magic != STATE_MAGIC || hdr.version != STATE_VERSION ||
        hdr.gb_size != sizeof(gb) || hdr.ram_size != sizeof(cart_ram) ||
        hdr.rom_hash != calc_rom_hash()) {
        return false;
    }

    const uint8_t *saved_gb = base + FLASH_PAGE_SIZE;
    const uint8_t *saved_ram = saved_gb + sizeof(gb);
    if (calc_state_checksum(saved_gb, saved_ram) != hdr.checksum) {
        return false;
    }
    if (out_hdr) {
        *out_hdr = hdr;
    }
    return true;
}

static int find_newest_valid_slot(StateHeader *out_hdr = nullptr) {
    StateHeader ha, hb;
    bool va = verify_flash_slot(0, &ha);
    bool vb = verify_flash_slot(1, &hb);
    if (!va && !vb) {
        return -1;
    }
    int best = 0;
    if (va && vb) {
        best = ((int32_t)(hb.seq - ha.seq) > 0) ? 1 : 0;
    } else {
        best = vb ? 1 : 0;
    }
    if (out_hdr) {
        *out_hdr = (best == 1) ? hb : ha;
    }
    return best;
}

// Forward declarations for Walnut-CGB init/cold callbacks
uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr);
uint16_t gb_rom_read_16bit(struct gb_s *gb, const uint_fast32_t addr);
uint32_t gb_rom_read_32bit(struct gb_s *gb, const uint_fast32_t addr);
uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr);
void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val);
void gb_error(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr);
void __not_in_flash_func(lcd_draw_line)(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line);

// Save full state (CPU + WRAM + VRAM + CGB palettes + Cart RAM) to inactive A/B Flash slot
bool save_state_to_flash() {
    while (_is_flipping()) {}
    gb_audio.pause();

    StateHeader cur_hdr;
    int cur_slot = find_newest_valid_slot(&cur_hdr);
    int target_slot = (cur_slot == 0) ? 1 : 0;
    uint32_t next_seq = (cur_slot >= 0) ? (cur_hdr.seq + 1u) : 1u;
    uint32_t slot_off = slot_offset_for_idx(target_slot);

    // Sync live APU registers (including CH1 sweep shifts) and Wave RAM into HRAM I/O
    gb.hram_io[0x26] = gb_audio.apu.power ? 0x80u : 0x00u;
    memcpy(gb.hram_io + 0x10, gb_audio.apu.regs, sizeof(gb_audio.apu.regs));
    memcpy(gb.hram_io + 0x30, gb_audio.apu.wave_ram, sizeof(gb_audio.apu.wave_ram));

    StateHeader hdr = {};
    hdr.magic          = STATE_MAGIC;
    hdr.version        = STATE_VERSION;
    hdr.seq            = next_seq;
    hdr.gb_size        = sizeof(gb);
    hdr.ram_size       = sizeof(cart_ram);
    hdr.checksum       = calc_state_checksum((const uint8_t *)&gb, cart_ram);
    hdr.rom_hash       = calc_rom_hash();
    hdr.settings_magic = SETTINGS_MAGIC_BYTE;
    hdr.volume_pct     = gb_audio.get_volume();
    hdr.brightness_pct = brightness_pct;
    hdr.audio_mode     = (uint8_t)gb_audio.mode;
    hdr.color_mode     = (uint8_t)color_mode;
    hdr.display_mode   = (uint8_t)display_mode;

    uint32_t total_stored = FLASH_PAGE_SIZE + sizeof(gb) + sizeof(cart_ram);
    uint32_t erase_bytes = ((total_stored + FLASH_SECTOR_SIZE - 1u) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;

    led(0, 80, 0);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(slot_off, erase_bytes);

    {
        FlashWriter writer(slot_off + FLASH_PAGE_SIZE);
        writer.write_bytes((const uint8_t *)&gb, sizeof(gb));
        writer.write_bytes(cart_ram, sizeof(cart_ram));
        writer.flush();
    }

    // Write Page 0 (StateHeader) LAST as atomic commit marker for power-loss safety
    {
        uint8_t page0[FLASH_PAGE_SIZE];
        memset(page0, 0xFF, sizeof(page0));
        memcpy(page0, &hdr, sizeof(hdr));
        flash_range_program(slot_off, page0, FLASH_PAGE_SIZE);
    }

    restore_interrupts(ints);

    gb_audio.resume();
    led(0, 0, 0);

    bool ok = verify_flash_slot(target_slot);
    if (ok) {
        cart_ram_dirty = false;
        banner_text = "STATE SAVED!";
        banner_bg_color = rgb(0, 12, 0);
        banner_timer = banner_duration_frames;
    } else {
        banner_text = "SAVE FAILED!";
        banner_bg_color = rgb(14, 0, 0);
        banner_timer = banner_duration_frames;
    }
    return ok;
}

// Restore newest valid save state from A/B ping-pong Flash slots
bool load_state_from_flash() {
    StateHeader hdr;
    int slot = find_newest_valid_slot(&hdr);
    if (slot < 0) {
        return false;
    }

    const uint8_t *base = flash_nocache_ptr(slot_offset_for_idx(slot));
    const uint8_t *saved_gb = base + FLASH_PAGE_SIZE;
    const uint8_t *saved_ram = saved_gb + sizeof(gb);

    memcpy(&gb, saved_gb, sizeof(gb));
    memcpy(cart_ram, saved_ram, sizeof(cart_ram));

    // Reconnect runtime callback pointers to current firmware code addresses
    gb.gb_rom_read       = gb_rom_read;
    gb.gb_rom_read_16bit = gb_rom_read_16bit;
    gb.gb_rom_read_32bit = gb_rom_read_32bit;
    gb.gb_cart_ram_read  = gb_cart_ram_read;
    gb.gb_cart_ram_write = gb_cart_ram_write;
    gb.gb_error          = gb_error;
    gb.gb_serial_tx      = nullptr;
    gb.gb_serial_rx      = nullptr;
    gb.gb_bootrom_read   = nullptr;
    gb.display.lcd_draw_line = lcd_draw_line;
    gb.direct.priv       = (void *)ROM_DATA;

    gb.direct.joypad = 0xFF;
    cart_ram_dirty = false;

    if (hdr.settings_magic == SETTINGS_MAGIC_BYTE) {
        gb_audio.set_volume(std::min<uint8_t>(hdr.volume_pct, 100u));
        brightness_pct = std::clamp<uint8_t>(hdr.brightness_pct, 10u, 100u);
        backlight(brightness_pct);
        if (hdr.audio_mode <= 2u) {
            gb_audio.set_mode((AudioMode)hdr.audio_mode);
        }
        if (hdr.color_mode < (uint8_t)COLOR_MODE_COUNT) {
            apply_color_mode((ColorMode)hdr.color_mode);
        }
        if (hdr.display_mode <= 2u) {
            display_mode = (DisplayMode)hdr.display_mode;
        }
    }

    pal_lut_stale = true;
    gb.cgb.fixPaletteDirty = 1;

    gb_audio.restore_from_hram(gb.hram_io);
    return true;
}

// ============================================================================
// Walnut-CGB Callbacks & SRAM Scanline Renderer
// ============================================================================

uint8_t gb_rom_read(struct gb_s *gb_ptr, const uint_fast32_t addr) {
    (void)gb_ptr;
    return wgb_fast_rom_read(addr);
}

uint16_t gb_rom_read_16bit(struct gb_s *gb_ptr, const uint_fast32_t addr) {
    (void)gb_ptr;
    return wgb_fast_rom_read16(addr);
}

uint32_t gb_rom_read_32bit(struct gb_s *gb_ptr, const uint_fast32_t addr) {
    (void)gb_ptr;
    return wgb_fast_rom_read32(addr);
}

uint8_t gb_cart_ram_read(struct gb_s *gb_ptr, const uint_fast32_t addr) {
    (void)gb_ptr;
    return wgb_fast_cart_ram_read(addr);
}

void gb_cart_ram_write(struct gb_s *gb_ptr, const uint_fast32_t addr, const uint8_t val) {
    (void)gb_ptr;
    wgb_fast_cart_ram_write(addr, val);
}

void gb_error(struct gb_s *gb_ptr, const enum gb_error_e err, const uint16_t addr) {
    gb_audio.pause();
    led(100, 0, 0);
#ifndef GB_AUDIO_HOST_TEST
    while (_is_flipping()) {}
    char msg[32];
    snprintf(msg, sizeof(msg), "CPU ERR %d @ %04X", (int)err, (unsigned)addr);
    pen(12, 0, 0);
    frect(20, 108, 200, 24);
    int32_t tw = 0, th = 0;
    measure(msg, tw, th);
    pen(15, 15, 15);
    text(msg, 120 - tw / 2, 116);
    _flip();
    while (true) {
        sleep(100);
    }
#else
    (void)gb_ptr;
    (void)err;
    (void)addr;
#endif
}

static void clear_screen_safe() {
    while (_is_flipping()) {}
    pen(0, 0, 0);
    clear();
}

// Rebuilds changed entries in _pal_lut from gb->cgb.fixPalette (64 entries)
static void __not_in_flash_func(rebuild_cgb_palette)(struct gb_s *gb_ptr) {
    const bool all = pal_lut_stale;
    pal_lut_stale = false;
    gb_ptr->cgb.fixPaletteDirty = 0;

    if (color_mode != COLOR_RAW) {
        for (int i = 0; i < 0x40; ++i) {
            const uint16_t c = gb_ptr->cgb.fixPalette[i];
            if (!all && c == _pal_src[i]) {
                continue;
            }
            _pal_src[i] = c;
            _pal_lut[i] = rgb565_to_color_filtered(c);
        }
    } else {
        for (int i = 0; i < 0x40; ++i) {
            const uint16_t c = gb_ptr->cgb.fixPalette[i];
            if (!all && c == _pal_src[i]) {
                continue;
            }
            _pal_src[i] = c;
            _pal_lut[i] = rgb565_to_color(c);
        }
    }
}

void __not_in_flash_func(lcd_draw_line)(struct gb_s *gb_ptr, const uint8_t *pixels, const uint_fast8_t line) {
    if (line >= 144u || SCREEN == nullptr || SCREEN->data == nullptr) {
        return;
    }

    const bool cgb = (gb_ptr->cgb.cgbMode != 0);
    if (cgb && (gb_ptr->cgb.fixPaletteDirty || pal_lut_stale)) {
        rebuild_cgb_palette(gb_ptr);
    }

    if (display_mode == DISPLAY_PIXEL_PERFECT) {
        color_t *dest = SCREEN->data + ((line + 48u) * 240u + 40u);
        if (cgb) {
            for (int x = 0; x < 160; ++x) {
                dest[x] = _pal_lut[pixels[x] & 0x3Fu];
            }
        } else {
            for (int x = 0; x < 160; ++x) {
                uint8_t p = pixels[x];
                dest[x] = _dmg_lut[(p & 0x30u) >> 4][p & 0x03u];
            }
        }
        return;
    }

    // Horizontal 1.5x blended scale (2 source pixels -> 3 output pixels with midpoint average)
    color_t line_buf[240];
    if (cgb) {
        for (int s = 0, d = 0; s < 160; s += 2, d += 3) {
            color_t c0 = _pal_lut[pixels[s] & 0x3Fu];
            color_t c1 = _pal_lut[pixels[s + 1] & 0x3Fu];
            line_buf[d]     = c0;
            line_buf[d + 1] = blend_avg(c0, c1);
            line_buf[d + 2] = c1;
        }
    } else {
        for (int s = 0, d = 0; s < 160; s += 2, d += 3) {
            uint8_t p0 = pixels[s];
            uint8_t p1 = pixels[s + 1];
            color_t c0 = _dmg_lut[(p0 & 0x30u) >> 4][p0 & 0x03u];
            color_t c1 = _dmg_lut[(p1 & 0x30u) >> 4][p1 & 0x03u];
            line_buf[d]     = c0;
            line_buf[d + 1] = blend_avg(c0, c1);
            line_buf[d + 2] = c1;
        }
    }

    if (display_mode == DISPLAY_FULLSCREEN) {
        // 5:3 vertical blended scale: every 3 GB scanlines -> 5 screen rows (lines 3k+1 and 3k+3 blended)
        const uint32_t g = line / 3u;
        const uint32_t r = line - g * 3u;
        const uint32_t base_y = g * 5u;

        if (r == 0u) {
            memcpy(SCREEN->data + base_y * 240u, line_buf, sizeof(line_buf));
            memcpy(prev_row, line_buf, sizeof(prev_row));
        } else if (r == 1u) {
            color_t *blend_dst = SCREEN->data + (base_y + 1u) * 240u;
            for (int x = 0; x < 240; ++x) {
                blend_dst[x] = blend_avg(prev_row[x], line_buf[x]);
            }
            memcpy(SCREEN->data + (base_y + 2u) * 240u, line_buf, sizeof(line_buf));
            memcpy(prev_row, line_buf, sizeof(prev_row));
        } else {
            color_t *blend_dst = SCREEN->data + (base_y + 3u) * 240u;
            for (int x = 0; x < 240; ++x) {
                blend_dst[x] = blend_avg(prev_row[x], line_buf[x]);
            }
            memcpy(SCREEN->data + (base_y + 4u) * 240u, line_buf, sizeof(line_buf));
        }
    } else {
        // DISPLAY_ASPECT_FIT: Exact 1.5x vertical blended scale into rows 12..227
        const uint32_t k = line >> 1;
        const uint32_t base_y = 12u + k * 3u;

        if ((line & 1u) == 0u) {
            memcpy(SCREEN->data + base_y * 240u, line_buf, sizeof(line_buf));
            memcpy(prev_row, line_buf, sizeof(prev_row));
        } else {
            color_t *blend_dst = SCREEN->data + (base_y + 1u) * 240u;
            for (int x = 0; x < 240; ++x) {
                blend_dst[x] = blend_avg(prev_row[x], line_buf[x]);
            }
            memcpy(SCREEN->data + (base_y + 2u) * 240u, line_buf, sizeof(line_buf));
        }
    }
}

// ============================================================================
// ST7789 60 Hz Unlock & Hardware Setup
// ============================================================================

#ifndef GB_AUDIO_HOST_TEST
static void configure_st7789_60hz() {
    constexpr uint PIN_CS   = 5u;
    constexpr uint PIN_SCK  = 6u;
    constexpr uint PIN_MOSI = 7u;
    constexpr uint PIN_DC   = 9u;
    constexpr uint32_t PIN_MASK = (1u << PIN_SCK) | (1u << PIN_MOSI);

    while (_is_flipping()) {}
    while (!pio_sm_is_tx_fifo_empty(pio0, 0)) {}
    sleep_us(20);
    pio_sm_set_enabled(pio0, 0, false);

    // Deselect ST7789 before switching SCK/MOSI between PIO0 and SPI0
    gpio_put(PIN_CS, 1);
    gpio_set_function(PIN_SCK, GPIO_FUNC_SPI);
    gpio_set_function(PIN_MOSI, GPIO_FUNC_SPI);

    auto send_cmd = [&](uint8_t cmd, const uint8_t *data, size_t len) {
        gpio_put(PIN_CS, 0);
        gpio_put(PIN_DC, 0);
        spi_write_blocking(spi0, &cmd, 1);
        if (len > 0 && data != nullptr) {
            gpio_put(PIN_DC, 1);
            spi_write_blocking(spi0, data, len);
        }
        gpio_put(PIN_CS, 1);
    };

    const uint8_t ste_data[2] = { 0x00, 0xF0 }; // Pulse TE at scanline 240 (~4.5 ms off-glass head start)
    const uint8_t fr_data[1]  = { 0x0F };       // ~60 Hz panel refresh instead of ~39 Hz
    const uint8_t caset[4]    = { 0x00, 0x00, 0x00, 0xEF };
    const uint8_t raset[4]    = { 0x00, 0x00, 0x00, 0xEF };
    send_cmd(0x44, ste_data, 2); // STE
    send_cmd(0xC6, fr_data, 1);  // FRCTRL2
    send_cmd(0x2A, caset, 4);    // CASET
    send_cmd(0x2B, raset, 4);    // RASET
    send_cmd(0x2C, nullptr, 0);  // RAMWR (leaves CS = 1)

    // While CS is still HIGH (deselected), clear PIO0's SCK/MOSI pin latches to 0
    // (screen.pio ends with `side 1`, leaving SCK latched HIGH) and jump back to wrap_target (`pull`).
    const uint wrap_target = (pio0->sm[0].execctrl & PIO_SM0_EXECCTRL_WRAP_BOTTOM_BITS)
                             >> PIO_SM0_EXECCTRL_WRAP_BOTTOM_LSB;
    pio_sm_clear_fifos(pio0, 0);
    pio_sm_restart(pio0, 0);
    pio_sm_clkdiv_restart(pio0, 0);
    pio_sm_set_pins_with_mask(pio0, 0, 0u, PIN_MASK);
    pio_sm_set_pindirs_with_mask(pio0, 0, PIN_MASK, PIN_MASK);
    pio_sm_exec(pio0, 0, pio_encode_jmp(wrap_target));

    // Hand MOSI/SCK back to PIO0 (both SPI0 and PIO0 now drive SCK = 0) and enable SM0
    pio_gpio_init(pio0, PIN_MOSI);
    pio_gpio_init(pio0, PIN_SCK);
    pio_sm_set_enabled(pio0, 0, true);

    // Only now select ST7789 in RAMWR data mode so zero spurious clock edges are sampled
    gpio_put(PIN_DC, 1);
    gpio_put(PIN_CS, 0);
}

static uint32_t measure_te_period_us() {
    constexpr uint PIN_VSYNC = 8u;
    constexpr uint32_t INTERVALS = 8u;
    const uint64_t deadline = time_us_64() + 400000u;

    auto wait_rising = [&]() -> bool {
        while (gpio_get(PIN_VSYNC))  { if (time_us_64() > deadline) return false; }
        while (!gpio_get(PIN_VSYNC)) { if (time_us_64() > deadline) return false; }
        return true;
    };

    if (!wait_rising()) {
        return 16742u;
    }
    const uint64_t t0 = time_us_64();
    for (uint32_t i = 0; i < INTERVALS; ++i) {
        if (!wait_rising()) {
            return 16742u;
        }
    }
    uint32_t avg = (uint32_t)((time_us_64() - t0) / INTERVALS);
    return (avg >= 10000u && avg <= 35000u) ? avg : 16742u;
}
#endif

// ============================================================================
// Lifecycle Hooks
// ============================================================================

void init() {
    uint32_t te_us = 16742u;
#ifndef GB_AUDIO_HOST_TEST
    gpio_init(CHARGE_STAT_PIN);
    gpio_set_dir(CHARGE_STAT_PIN, GPIO_IN);
    gpio_pull_up(CHARGE_STAT_PIN);
    gpio_init(CHARGE_VBUS_PIN);
    gpio_set_dir(CHARGE_VBUS_PIN, GPIO_IN);
    gpio_pull_down(CHARGE_VBUS_PIN);

    configure_st7789_60hz();
    te_us = measure_te_period_us();
#endif

    // Calibrate hold & banner frame counts to actual display TE rate (~60 Hz or fallback ~40 Hz)
    combo_hold_frames      = std::clamp<uint32_t>(1000000u / te_us, 35u, 65u);
    combo_hint_frames      = std::max<uint32_t>(combo_hold_frames / 4u, 8u);
    banner_duration_frames = (combo_hold_frames * 3u) / 2u;
    banner_short_frames    = combo_hold_frames;

    gb_audio.init();
    gb_audio.set_frame_period_us(te_us);
    backlight(brightness_pct);

    enum gb_init_error_e init_status = gb_init(
        &gb,
        gb_rom_read,
        gb_rom_read_16bit,
        gb_rom_read_32bit,
        gb_cart_ram_read,
        gb_cart_ram_write,
        gb_error,
        (void *)ROM_DATA
    );

    if (init_status != GB_INIT_NO_ERROR) {
        led(100, 0, 0);
    }

    // Start CGB palette white before game initializes BCPD/OCPD
    for (int i = 0; i < 0x40; ++i) {
        gb.cgb.fixPalette[i] = 0xFFFFu;
    }
    gb_init_lcd(&gb, lcd_draw_line);
    apply_color_mode(color_mode);

    // Initialize DMG post-boot APU master routing defaults before any auto-resume load
    audio_write(0xFF24u, 0x77u);
    audio_write(0xFF25u, 0xF3u);

    // PicoSystem populates _io after init(); buttons are active-low with pull-ups.
    // If 'Y' is held during power-on/boot, start clean from Title Screen.
    bool fresh_boot = !gpio_get(button::Y);

    if (!fresh_boot) {
        if (load_state_from_flash()) {
            banner_text = "RESUMED!";
            banner_bg_color = rgb(0, 10, 14);
            banner_timer = banner_duration_frames;
        }
    }
}

static void update_menu() {
    if (!button(button::LEFT)) {
        menu_ignore_left_hold = false;
    }

    if (pressed(button::B) ||
        (pressed(button::X) && !button(button::Y)) ||
        (button(button::X) && button(button::Y) && pressed(button::LEFT))) {
        menu_open = false;
        menu_exit_mask = true;
        menu_ignore_left_hold = false;
        menu_repeat_timer = 0;
        save_combo_consumed = true;
        save_hold_timer = 0;
        clear_screen_safe();
        gb_audio.resume();
        return;
    }

    menu_batt_timer++;
    if (menu_batt_timer >= 30u) {
        menu_batt_timer = 0;
        poll_battery_status();
    }

    if (pressed(button::UP)) {
        menu_cursor = (menu_cursor + MENU_ITEM_COUNT - 1) % MENU_ITEM_COUNT;
        menu_repeat_timer = 0;
    } else if (pressed(button::DOWN)) {
        menu_cursor = (menu_cursor + 1) % MENU_ITEM_COUNT;
        menu_repeat_timer = 0;
    }

    int delta = 0;
    bool left_held = button(button::LEFT) && !menu_ignore_left_hold;
    bool right_held = button(button::RIGHT);
    if (pressed(button::LEFT)) {
        delta = -1;
        menu_repeat_timer = 0;
        menu_ignore_left_hold = false;
    } else if (pressed(button::RIGHT)) {
        delta = 1;
        menu_repeat_timer = 0;
    } else if ((left_held != right_held) && menu_cursor <= 1) {
        menu_repeat_timer++;
        if (menu_repeat_timer >= 18u && ((menu_repeat_timer - 18u) % 5u == 0u)) {
            delta = right_held ? 1 : -1;
        }
    } else {
        menu_repeat_timer = 0;
    }

    if (delta != 0) {
        switch (menu_cursor) {
            case 0: {
                int next = std::clamp((int)gb_audio.get_volume() + delta * 10, 0, 100);
                gb_audio.set_volume((uint8_t)next);
                break;
            }
            case 1: {
                int next = std::clamp((int)brightness_pct + delta * 10, 10, 100);
                brightness_pct = (uint8_t)next;
                backlight(brightness_pct);
                break;
            }
            case 2: {
                int next = ((int)gb_audio.mode + delta + 3) % 3;
                gb_audio.set_mode((AudioMode)next);
                break;
            }
            case 3: {
                int count = (int)COLOR_MODE_COUNT;
                int next = ((int)color_mode + delta + count) % count;
                apply_color_mode((ColorMode)next);
                break;
            }
            case 4:
                display_mode = (DisplayMode)(((int)display_mode + delta + 3) % 3);
                clear_screen_safe();
                break;
            default:
                break;
        }
    }
}

void update(uint32_t tick) {
    (void)tick;

    if (menu_open) {
        update_menu();
        return;
    }

    static const char * const color_banners[COLOR_MODE_COUNT] = {
        "COLOR: GBC LCD",
        "COLOR: RAW COLOR",
        "COLOR: VIVID",
        "COLOR: PASTEL",
        "COLOR: TEAL LIGHT",
        "COLOR: DMG GREEN",
        "COLOR: POCKET B&W",
        "COLOR: SEPIA",
        "COLOR: NIGHT"
    };
    static const char * const display_banners[3] = {
        "DISPLAY: FULLSCREEN",
        "DISPLAY: ASPECT FIT",
        "DISPLAY: 1:1 CENTER"
    };

    // Joypad buttons (0 = pressed, 1 = released)
    gb.direct.joypad_bits.up     = !button(button::UP);
    gb.direct.joypad_bits.down   = !button(button::DOWN);
    gb.direct.joypad_bits.left   = !button(button::LEFT);
    gb.direct.joypad_bits.right  = !button(button::RIGHT);
    gb.direct.joypad_bits.a      = !button(button::A);
    gb.direct.joypad_bits.b      = !button(button::B);
    gb.direct.joypad_bits.start  = !button(button::X);
    gb.direct.joypad_bits.select = !button(button::Y);

    // ------------------------------------------------------------------------
    // Shortcuts: Hold X + Y and tap D-Pad (UP/DOWN/LEFT/RIGHT)
    // ------------------------------------------------------------------------
    if (button(button::X) && button(button::Y)) {
        if (pressed(button::LEFT)) {
            menu_open = true;
            menu_ignore_left_hold = true;
            menu_repeat_timer = 0;
            menu_batt_timer = 0;
            poll_battery_status();
            save_combo_consumed = true;
            save_hold_timer = 0;
            gb_audio.pause();
            return;
        }
        if (pressed(button::UP)) {
            int next = ((int)color_mode + 1) % (int)COLOR_MODE_COUNT;
            apply_color_mode((ColorMode)next);
            banner_text = color_banners[next];
            banner_bg_color = rgb(2, 6, 8);
            banner_timer = banner_short_frames;
            save_combo_consumed = true;
            save_hold_timer = 0;
        }
        if (pressed(button::DOWN)) {
            display_mode = (DisplayMode)(((int)display_mode + 1) % 3);
            clear_screen_safe();
            banner_text = display_banners[(int)display_mode];
            banner_bg_color = rgb(2, 5, 10);
            banner_timer = banner_short_frames;
            save_combo_consumed = true;
            save_hold_timer = 0;
        }
        if (pressed(button::RIGHT)) {
            gb_audio.cycle_mode();
            banner_text = gb_audio.mode_label();
            banner_bg_color = rgb(6, 2, 12);
            banner_timer = banner_short_frames;
            save_combo_consumed = true;
            save_hold_timer = 0;
        }
    } else {
        save_hold_timer = 0;
        save_combo_consumed = false;
    }

    // ------------------------------------------------------------------------
    // QUICK SAVE STATE: Hold X + Y for ~1.0 second
    // ------------------------------------------------------------------------
    if (button(button::X) && button(button::Y) &&
        !button(button::UP) && !button(button::DOWN) &&
        !button(button::LEFT) && !button(button::RIGHT)) {
        if (!save_combo_consumed) {
            save_hold_timer++;
            if (save_hold_timer >= combo_hold_frames) {
                save_state_to_flash();
                save_hold_timer = 0;
                save_combo_consumed = true;
            }
        }
    } else {
        save_hold_timer = 0;
    }

    // ------------------------------------------------------------------------
    // QUICK LOAD STATE: Hold A + B (without D-Pad) for ~1.0 second
    // ------------------------------------------------------------------------
    if (button(button::A) && button(button::B) &&
        !button(button::X) && !button(button::Y) &&
        !button(button::UP) && !button(button::DOWN) &&
        !button(button::LEFT) && !button(button::RIGHT)) {
        if (!load_combo_consumed) {
            load_hold_timer++;
            if (load_hold_timer >= combo_hold_frames) {
                if (load_state_from_flash()) {
                    clear_screen_safe();
                    banner_text = "STATE LOADED!";
                    banner_bg_color = rgb(0, 10, 14);
                    banner_timer = banner_duration_frames;
                } else {
                    banner_text = "NO SAVE FOUND!";
                    banner_bg_color = rgb(14, 0, 0);
                    banner_timer = banner_short_frames;
                }
                load_hold_timer = 0;
                load_combo_consumed = true;
            }
        }
    } else {
        load_hold_timer = 0;
        if (!(button(button::A) && button(button::B))) {
            load_combo_consumed = false;
        }
    }

    // Keep A+B masked from the restored frame until the user releases the combo
    if (load_combo_consumed) {
        gb.direct.joypad_bits.a = 1;
        gb.direct.joypad_bits.b = 1;
    }

    // Keep menu exit keys masked from the game until released
    if (menu_exit_mask) {
        if (!button(button::A) && !button(button::B) &&
            !button(button::X) && !button(button::Y) &&
            !button(button::LEFT)) {
            menu_exit_mask = false;
        } else {
            gb.direct.joypad_bits.a      = 1;
            gb.direct.joypad_bits.b      = 1;
            gb.direct.joypad_bits.start  = 1;
            gb.direct.joypad_bits.select = 1;
            gb.direct.joypad_bits.left   = 1;
        }
    }

    // Auto-save battery RAM (Pokemon in-game saves)
    if (cart_ram_dirty && (time() - last_cart_write_ms > 2500)) {
        if (!save_state_to_flash()) {
            last_cart_write_ms = time();
        }
    }

    // Run 1 frame of GBC/DMG emulation (SRAM-resident dispatcher) + advance audio synthesis
    gb_run_frame_dualfetch(&gb);
    gb_audio.update_frame();

    if (banner_timer > 0) {
        banner_timer--;
    }
}

// ============================================================================
// UI Overlays & Menu Rendering
// ============================================================================

static void draw_hold_banner(const char *label, uint32_t timer, color_t bg, color_t track_bg) {
    pen(bg);
    frect(54, 214, 132, 20);
    pen(track_bg);
    frect(58, 228, 124, 3);
    int32_t fill_w = (int32_t)((timer * 124u) / combo_hold_frames);
    if (fill_w > 124) {
        fill_w = 124;
    }
    pen(15, 15, 15);
    if (fill_w > 0) {
        frect(58, 228, fill_w, 3);
    }
    int32_t tw = 0, th = 0;
    measure(label, tw, th);
    text(label, 120 - tw / 2, 217);
}

static void draw_slider(int32_t y, uint8_t pct, color_t fill_col) {
    pen(4, 4, 5);
    frect(94, y + 6, 80, 6);
    int32_t w = ((int32_t)pct * 80) / 100;
    if (w > 0) {
        pen(fill_col);
        frect(94, y + 6, w, 6);
    }
    char buf[8];
    snprintf(buf, sizeof(buf), "%3u%%", pct);
    pen(15, 15, 15);
    text(buf, 180, y + 5);
}

static void draw_settings_menu() {
    static const char * const labels[MENU_ITEM_COUNT] = {
        "VOLUME", "BRIGHT", "AUDIO", "COLOR", "DISPLAY"
    };
    static const char * const audio_names[3] = {
        "< 4-CH PWM >", "< PIEZO BEEP >", "< MUTED >"
    };
    static const char * const color_names[COLOR_MODE_COUNT] = {
        "< GBC LCD >",
        "< RAW COLOR >",
        "< VIVID >",
        "< PASTEL >",
        "< TEAL LIGHT >",
        "< DMG GREEN >",
        "< POCKET B&W >",
        "< SEPIA >",
        "< NIGHT >"
    };
    static const char * const disp_names[3] = {
        "< FULLSCREEN >", "< ASPECT FIT >", "< 1:1 CENTER >"
    };

    pen(1, 2, 3);
    frect(17, 45, 206, 150);
    pen(10, 12, 14);
    rect(16, 44, 208, 152);

    pen(2, 5, 8);
    frect(17, 45, 206, 18);
    int32_t tw = 0, th = 0;

    // Mode tag on header left
    const char *sys_tag = gb.cgb.cgbMode ? "GBC" : "DMG";
    pen(gb.cgb.cgbMode ? rgb(8, 15, 11) : rgb(10, 13, 14));
    text(sys_tag, 23, 50);

    measure("SETTINGS", tw, th);
    pen(15, 15, 15);
    text("SETTINGS", 120 - tw / 2, 50);

    // Battery & charging indicator on header right
    char batt_buf[12];
    if (cached_charging) {
        snprintf(batt_buf, sizeof(batt_buf), "CHG %lu%%", (unsigned long)cached_battery_pct);
        pen(6, 15, 8);
    } else {
        snprintf(batt_buf, sizeof(batt_buf), "%lu%%", (unsigned long)cached_battery_pct);
        if (cached_battery_pct <= 20u) {
            pen(15, 4, 4);
        } else if (cached_battery_pct <= 45u) {
            pen(15, 13, 4);
        } else {
            pen(11, 14, 12);
        }
    }
    measure(batt_buf, tw, th);
    text(batt_buf, 217 - tw, 50);

    for (int i = 0; i < MENU_ITEM_COUNT; ++i) {
        int32_t row_y = 68 + i * 20;
        if (i == menu_cursor) {
            pen(3, 6, 10);
            frect(20, row_y, 200, 18);
            pen(15, 15, 15);
            text(">", 22, row_y + 5);
        } else {
            pen(11, 12, 13);
        }
        text(labels[i], 30, row_y + 5);

        switch (i) {
            case 0: {
                uint8_t vol = gb_audio.get_volume();
                color_t col = (gb_audio.mode == AUDIO_OFF || vol == 0u)
                    ? rgb(6, 6, 6)
                    : rgb(4, 14, 6);
                draw_slider(row_y, vol, col);
                break;
            }
            case 1:
                draw_slider(row_y, brightness_pct, rgb(15, 13, 4));
                break;
            case 2:
                pen(15, 15, 15);
                text(audio_names[(int)gb_audio.mode], 94, row_y + 5);
                break;
            case 3:
                pen(15, 15, 15);
                text(color_names[(int)color_mode], 94, row_y + 5);
                break;
            case 4:
                pen(15, 15, 15);
                text(disp_names[(int)display_mode], 94, row_y + 5);
                break;
            default:
                break;
        }
    }

    pen(4, 5, 7);
    hline(22, 173, 196);
    static const char *hint = "D-PAD:ADJUST  B/X:BACK";
    measure(hint, tw, th);
    pen(9, 10, 11);
    text(hint, 120 - tw / 2, 180);
}

void draw(uint32_t tick) {
    (void)tick;

    // Non-fullscreen bezels
    if (display_mode == DISPLAY_PIXEL_PERFECT) {
        pen(1, 1, 1);
        frect(0, 0, 240, 48);
        frect(0, 192, 240, 48);
        frect(0, 48, 40, 144);
        frect(200, 48, 40, 144);

        int32_t tw = 0, th = 0;
        measure("GAME BOY", tw, th);
        pen(6, 6, 6);
        text("GAME BOY", 120 - tw / 2, 24);
    } else if (display_mode == DISPLAY_ASPECT_FIT) {
        pen(0, 0, 0);
        frect(0, 0, 240, 12);
        frect(0, 228, 240, 12);
    }

    if (menu_open) {
        draw_settings_menu();
    } else if (save_hold_timer >= combo_hint_frames) {
        draw_hold_banner("HOLD TO SAVE...", save_hold_timer, rgb(12, 8, 0), rgb(4, 3, 0));
    } else if (load_hold_timer >= combo_hint_frames) {
        draw_hold_banner("HOLD TO LOAD...", load_hold_timer, rgb(0, 8, 12), rgb(0, 3, 5));
    } else if (banner_timer > 0) {
        int32_t tw = 0, th = 0;
        measure(banner_text, tw, th);
        int32_t box_w = std::max<int32_t>(120, tw + 16);
        pen(banner_bg_color);
        frect(120 - box_w / 2, 216, box_w, 16);
        pen(15, 15, 15);
        text(banner_text, 120 - tw / 2, 220);
    }
}
