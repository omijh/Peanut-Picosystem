#include <algorithm>
#include <cstdio>
#include <cstring>
#include "picosystem.hpp"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#define ENABLE_LCD 1
#define ENABLE_SOUND 1
#define PEANUT_GB_12_COLOUR 0
uint8_t audio_read(const uint16_t addr);
void audio_write(const uint16_t addr, const uint8_t val);
#include "peanut_gb.h"
#include "gb_audio.hpp"

// Check if user provided a custom ROM in rom.h, otherwise fall back to test ROM
#if __has_include("rom.h")
    #include "rom.h"
    #define ROM_DATA custom_rom_data
    #define ROM_SIZE custom_rom_len
#else
    #include "dmg-acid2.gb.h"
    #define ROM_DATA dmg_acid2_gb
    #define ROM_SIZE sizeof(dmg_acid2_gb)
#endif

using namespace picosystem;

// Emulator context
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

// 32 KB Cartridge RAM (Save data for MBC1, MBC2, MBC3, etc. - covers Pokemon Gen 1 & 2)
#define CART_RAM_MAX_SIZE (32 * 1024)
static uint8_t cart_ram[CART_RAM_MAX_SIZE];
static bool cart_ram_dirty = false;
static uint32_t last_cart_write_ms = 0;

// On-screen notification banners (~40 Hz PicoSystem VSYNC)
static constexpr uint32_t BANNER_DURATION_FRAMES = 60; // ~1.5s at 40 Hz
static constexpr uint32_t BANNER_SHORT_FRAMES = 45;    // ~1.1s at 40 Hz
static uint32_t banner_timer = 0;
static const char *banner_text = "";
static color_t banner_bg_color = rgb(0, 12, 0);

// Hold timers for button combos (~40 Hz PicoSystem VSYNC)
static constexpr uint32_t COMBO_HOLD_FRAMES = 40; // ~1.0s at 40 Hz
static constexpr uint32_t COMBO_HINT_FRAMES = 10; // ~0.25s at 40 Hz
static uint32_t save_hold_timer = 0;
static uint32_t load_hold_timer = 0;
static bool save_combo_consumed = false;
static bool load_combo_consumed = false;

// Quick Settings Menu & Brightness
static uint8_t brightness_pct = 30; // 10..100%
static constexpr int MENU_ITEM_COUNT = 5;
static bool menu_open = false;
static bool menu_exit_mask = false;
static bool menu_ignore_left_hold = false;
static int menu_cursor = 0;
static uint32_t menu_repeat_timer = 0;

// ============================================================================
// Display Palettes & Scaling
// ============================================================================

static uint8_t palette_idx = 2; // Default: Game Boy Light (Teal)
static const color_t palettes[3][4] = {
    // 0: Classic Game Boy DMG (Olive green)
    { rgb(13, 15, 6), rgb(9, 13, 4), rgb(4, 8, 3), rgb(1, 3, 1) },
    // 1: Game Boy Pocket (B&W Grayscale)
    { rgb(15, 15, 15), rgb(10, 10, 10), rgb(5, 5, 5), rgb(0, 0, 0) },
    // 2: Game Boy Light (Teal backlight)
    { rgb(11, 15, 14), rgb(6, 12, 11), rgb(3, 7, 7), rgb(1, 3, 3) }
};

enum DisplayMode {
    DISPLAY_FULLSCREEN = 0, // 240x240 Edge-to-Edge (Fills entire display)
    DISPLAY_ASPECT_FIT = 1, // 240x216 (Full width, preserves 10:9 ratio, 12px letterbox)
    DISPLAY_PIXEL_PERFECT = 2 // 160x144 Centered with retro bezel
};
static DisplayMode display_mode = DISPLAY_FULLSCREEN;
static uint8_t scale_x_lut[240];

// ============================================================================
// Flash Save State Persistence Architecture
// ============================================================================
// PicoSystem has 16MB of Flash. We reserve 64 KB at the 15 MB mark (0x00F00000).
#define FLASH_STATE_OFFSET    (15 * 1024 * 1024)
#define FLASH_STATE_MAX_SIZE  (64 * 1024)
#define FLASH_STATE_READ_ADDR ((const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + FLASH_STATE_OFFSET))
#define STATE_MAGIC           0x53544154u // "STAT"
#define STATE_VERSION         2u
static constexpr uint8_t SETTINGS_MAGIC_BYTE = 0xA5u;

#ifdef PICO_FLASH_SIZE_BYTES
static_assert(PICO_FLASH_SIZE_BYTES >= FLASH_STATE_OFFSET + FLASH_STATE_MAX_SIZE,
              "PICO_FLASH_SIZE_BYTES too small for save state; build with PICO_BOARD=pimoroni_picosystem");
#endif

struct StateHeader {
    uint32_t magic;          // STATE_MAGIC ("STAT")
    uint32_t version;        // STATE_VERSION
    uint32_t gb_size;        // sizeof(struct gb_s)
    uint32_t ram_size;       // sizeof(cart_ram)
    uint32_t checksum;       // FNV-1a 32-bit hash of gb + cart_ram
    uint32_t rom_hash;       // FNV-1a 32-bit hash of ROM header + ROM_SIZE
    uint8_t  settings_magic; // SETTINGS_MAGIC_BYTE when settings fields are present
    uint8_t  volume_pct;     // 0..100
    uint8_t  brightness_pct; // 10..100
    uint8_t  audio_mode;     // AudioMode (0..2)
    uint8_t  palette_idx;    // 0..2
    uint8_t  display_mode;   // DisplayMode (0..2)
    uint8_t  reserved[2];
};

static_assert(sizeof(StateHeader) == 32, "StateHeader must remain 32 bytes");

static_assert(sizeof(StateHeader) + sizeof(gb) + sizeof(cart_ram) <= FLASH_STATE_MAX_SIZE,
              "Save state exceeds 64 KB Flash reservation");

// Streaming flash page writer (uses only 256 bytes of RAM!)
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

static bool verify_flash_state() {
    StateHeader hdr;
    memcpy(&hdr, FLASH_STATE_READ_ADDR, sizeof(hdr));
    if (hdr.magic != STATE_MAGIC || hdr.version != STATE_VERSION ||
        hdr.gb_size != sizeof(gb) || hdr.ram_size != sizeof(cart_ram) ||
        hdr.rom_hash != calc_rom_hash()) {
        return false;
    }

    const uint8_t *saved_gb = FLASH_STATE_READ_ADDR + sizeof(StateHeader);
    const uint8_t *saved_ram = saved_gb + sizeof(gb);
    return calc_state_checksum(saved_gb, saved_ram) == hdr.checksum;
}

// Forward declarations for callbacks
uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr);
uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr);
void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val);
void gb_error(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr);
void lcd_draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line);

// Save full state (CPU + WRAM + VRAM + Cart RAM) to Flash
bool save_state_to_flash() {
    // Wait for any active screen DMA transfer and silence audio before freezing XIP
    while (_is_flipping()) {}
    gb_audio.pause();

    // Sync live APU registers (including CH1 sweep shifts) and Wave RAM into HRAM I/O
    gb.hram_io[0x26] = gb_audio.apu.power ? 0x80u : 0x00u;
    memcpy(gb.hram_io + 0x10, gb_audio.apu.regs, sizeof(gb_audio.apu.regs));
    memcpy(gb.hram_io + 0x30, gb_audio.apu.wave_ram, sizeof(gb_audio.apu.wave_ram));

    StateHeader hdr;
    hdr.magic = STATE_MAGIC;
    hdr.version = STATE_VERSION;
    hdr.gb_size = sizeof(gb);
    hdr.ram_size = sizeof(cart_ram);
    hdr.checksum = calc_state_checksum((const uint8_t *)&gb, cart_ram);
    hdr.rom_hash = calc_rom_hash();
    hdr.settings_magic = SETTINGS_MAGIC_BYTE;
    hdr.volume_pct     = gb_audio.get_volume();
    hdr.brightness_pct = brightness_pct;
    hdr.audio_mode     = (uint8_t)gb_audio.mode;
    hdr.palette_idx    = palette_idx;
    hdr.display_mode   = (uint8_t)display_mode;
    hdr.reserved[0]    = 0;
    hdr.reserved[1]    = 0;

    uint32_t total_payload = sizeof(StateHeader) + sizeof(gb) + sizeof(cart_ram);
    uint32_t erase_bytes = ((total_payload + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;

    // Visual feedback: green LED
    led(0, 80, 0);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_STATE_OFFSET, erase_bytes);

    FlashWriter writer(FLASH_STATE_OFFSET);
    writer.write_bytes((const uint8_t *)&hdr, sizeof(hdr));
    writer.write_bytes((const uint8_t *)&gb, sizeof(gb));
    writer.write_bytes(cart_ram, sizeof(cart_ram));
    writer.flush();

    restore_interrupts(ints);

    gb_audio.resume();
    led(0, 0, 0);

    bool ok = verify_flash_state();
    if (ok) {
        cart_ram_dirty = false;
        banner_text = "STATE SAVED!";
        banner_bg_color = rgb(0, 12, 0);
        banner_timer = BANNER_DURATION_FRAMES;
    } else {
        banner_text = "SAVE FAILED!";
        banner_bg_color = rgb(14, 0, 0);
        banner_timer = BANNER_DURATION_FRAMES;
    }
    return ok;
}

// Restore full state from Flash
bool load_state_from_flash() {
    if (!verify_flash_state()) {
        return false;
    }

    StateHeader hdr;
    memcpy(&hdr, FLASH_STATE_READ_ADDR, sizeof(hdr));

    const uint8_t *saved_gb = FLASH_STATE_READ_ADDR + sizeof(StateHeader);
    const uint8_t *saved_ram = saved_gb + sizeof(gb);

    // Restore emulator state
    memcpy(&gb, saved_gb, sizeof(gb));
    memcpy(cart_ram, saved_ram, sizeof(cart_ram));

    // Reconnect runtime callback pointers to current firmware code addresses
    gb.gb_rom_read = gb_rom_read;
    gb.gb_cart_ram_read = gb_cart_ram_read;
    gb.gb_cart_ram_write = gb_cart_ram_write;
    gb.gb_error = gb_error;
    gb.gb_serial_tx = nullptr;
    gb.gb_serial_rx = nullptr;
    gb.gb_bootrom_read = nullptr;
    gb.display.lcd_draw_line = lcd_draw_line;
    gb.direct.priv = (void *)ROM_DATA;

    // Release any joypad button latch and clear dirty flag
    gb.direct.joypad = 0xFF;
    cart_ram_dirty = false;

    // Restore user settings if saved in header
    if (hdr.settings_magic == SETTINGS_MAGIC_BYTE) {
        gb_audio.set_volume(std::min<uint8_t>(hdr.volume_pct, 100u));
        brightness_pct = std::clamp<uint8_t>(hdr.brightness_pct, 10u, 100u);
        backlight(brightness_pct);
        if (hdr.audio_mode <= 2u) {
            gb_audio.set_mode((AudioMode)hdr.audio_mode);
        }
        if (hdr.palette_idx <= 2u) {
            palette_idx = hdr.palette_idx;
        }
        if (hdr.display_mode <= 2u) {
            display_mode = (DisplayMode)hdr.display_mode;
        }
    }

    // Synchronize APU registers and Wave RAM from restored IO memory
    gb_audio.restore_from_hram(gb.hram_io);

    return true;
}

// Peanut-GB Callbacks
uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr) {
    if (addr < ROM_SIZE) {
        const uint8_t *rom = (const uint8_t *)gb->direct.priv;
        return rom[addr];
    }
    return 0xFF;
}

uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
    if (addr < CART_RAM_MAX_SIZE) {
        return cart_ram[addr];
    }
    return 0xFF;
}

void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
    if (addr < CART_RAM_MAX_SIZE) {
        if (cart_ram[addr] != val) {
            cart_ram[addr] = val;
            cart_ram_dirty = true;
            last_cart_write_ms = time();
        }
    }
}

void gb_error(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr) {
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
    (void)gb;
    (void)err;
    (void)addr;
#endif
}

static void clear_screen_safe() {
    while (_is_flipping()) {}
    pen(0, 0, 0);
    clear();
}

// LCD Draw Line Callback (Real-time scanline scaling)
void lcd_draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
    if (line >= 144 || SCREEN == nullptr || SCREEN->data == nullptr) {
        return;
    }

    const color_t *pal = palettes[palette_idx];

    if (display_mode == DISPLAY_FULLSCREEN) {
        color_t line_buf[240];
        for (int x = 0; x < 240; ++x) {
            line_buf[x] = pal[pixels[scale_x_lut[x]] & 3];
        }

        int y_start = (line * 5) / 3;
        int y_end   = ((line + 1) * 5) / 3;
        for (int y = y_start; y < y_end; ++y) {
            memcpy(SCREEN->data + (y * 240), line_buf, sizeof(line_buf));
        }
    } else if (display_mode == DISPLAY_ASPECT_FIT) {
        color_t line_buf[240];
        for (int x = 0; x < 240; ++x) {
            line_buf[x] = pal[pixels[scale_x_lut[x]] & 3];
        }

        int y_start = 12 + (line * 3) / 2;
        int y_end   = 12 + ((line + 1) * 3) / 2;
        for (int y = y_start; y < y_end; ++y) {
            memcpy(SCREEN->data + (y * 240), line_buf, sizeof(line_buf));
        }
    } else {
        color_t *dest = SCREEN->data + ((line + 48) * 240 + 40);
        for (int x = 0; x < 160; ++x) {
            dest[x] = pal[pixels[x] & 3];
        }
    }
}

// ============================================================================
// Lifecycle Hooks
// ============================================================================

void init() {
    // Take ownership of PicoSystem piezo PWM before Peanut-GB reset writes NR52
    gb_audio.init();
    backlight(brightness_pct);

    // Precompute horizontal scale LUT: x_gb = (x * 2) / 3
    for (int x = 0; x < 240; ++x) {
        scale_x_lut[x] = (x * 2) / 3;
    }

    // Default init for Peanut-GB
    enum gb_init_error_e init_status = gb_init(
        &gb,
        gb_rom_read,
        gb_cart_ram_read,
        gb_cart_ram_write,
        gb_error,
        (void *)ROM_DATA
    );

    if (init_status != GB_INIT_NO_ERROR) {
        led(100, 0, 0);
    }
    gb_init_lcd(&gb, lcd_draw_line);

    // Initialize DMG post-boot APU master routing defaults before any auto-resume load
    audio_write(0xFF24u, 0x77u);
    audio_write(0xFF25u, 0xF3u);

    // PicoSystem populates _io after init(); buttons are active-low with pull-ups.
    // If 'Y' is held during power-on/boot, start clean from Title Screen.
    bool fresh_boot = !gpio_get(button::Y);

    if (!fresh_boot) {
        // Attempt to auto-resume from saved state
        if (load_state_from_flash()) {
            banner_text = "RESUMED!";
            banner_bg_color = rgb(0, 10, 14); // Cyan
            banner_timer = BANNER_DURATION_FRAMES;
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
        if (menu_repeat_timer >= 14u && ((menu_repeat_timer - 14u) % 4u == 0u)) {
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
            case 3:
                palette_idx = (uint8_t)(((int)palette_idx + delta + 3) % 3);
                break;
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
    if (menu_open) {
        update_menu();
        return;
    }

    static const char * const palette_banners[3] = {
        "PALETTE: DMG GREEN", "PALETTE: POCKET B&W", "PALETTE: TEAL LIGHT"
    };
    static const char * const display_banners[3] = {
        "DISPLAY: FULLSCREEN", "DISPLAY: ASPECT FIT", "DISPLAY: 1:1 CENTER"
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
            save_combo_consumed = true;
            save_hold_timer = 0;
            gb_audio.pause();
            return;
        }
        if (pressed(button::UP)) {
            palette_idx = (palette_idx + 1) % 3;
            banner_text = palette_banners[palette_idx];
            banner_bg_color = rgb(2, 6, 8);
            banner_timer = BANNER_SHORT_FRAMES;
            save_combo_consumed = true;
            save_hold_timer = 0;
        }
        if (pressed(button::DOWN)) {
            display_mode = (DisplayMode)(((int)display_mode + 1) % 3);
            clear_screen_safe();
            banner_text = display_banners[(int)display_mode];
            banner_bg_color = rgb(2, 5, 10);
            banner_timer = BANNER_SHORT_FRAMES;
            save_combo_consumed = true;
            save_hold_timer = 0;
        }
        if (pressed(button::RIGHT)) {
            gb_audio.cycle_mode();
            banner_text = gb_audio.mode_label();
            banner_bg_color = rgb(6, 2, 12);
            banner_timer = BANNER_SHORT_FRAMES;
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
            if (save_hold_timer >= COMBO_HOLD_FRAMES) { // ~1.0s at 40 Hz VSYNC
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
            if (load_hold_timer >= COMBO_HOLD_FRAMES) { // ~1.0s at 40 Hz VSYNC
                if (load_state_from_flash()) {
                    clear_screen_safe();
                    banner_text = "STATE LOADED!";
                    banner_bg_color = rgb(0, 10, 14); // Cyan
                    banner_timer = BANNER_DURATION_FRAMES;
                } else {
                    banner_text = "NO SAVE FOUND!";
                    banner_bg_color = rgb(14, 0, 0); // Red
                    banner_timer = BANNER_SHORT_FRAMES;
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

    // Run 1 frame of emulation and advance audio synthesis
    gb_run_frame(&gb);
    gb_audio.update_frame();

    if (banner_timer > 0) {
        banner_timer--;
    }
}

static void draw_hold_banner(const char *label, uint32_t timer, color_t bg, color_t track_bg) {
    pen(bg);
    frect(54, 214, 132, 20);
    pen(track_bg);
    frect(58, 228, 124, 3);
    int32_t fill_w = (int32_t)((timer * 124) / COMBO_HOLD_FRAMES);
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
    static const char *labels[MENU_ITEM_COUNT] = {
        "VOLUME", "BRIGHT", "AUDIO", "PALETTE", "DISPLAY"
    };
    static const char *audio_names[3] = {
        "< 4-CH PWM >", "< PIEZO BEEP >", "< MUTED >"
    };
    static const char *pal_names[3] = {
        "< DMG GREEN >", "< POCKET B&W >", "< TEAL LIGHT >"
    };
    static const char *disp_names[3] = {
        "< FULLSCREEN >", "< ASPECT FIT >", "< 1:1 CENTER >"
    };

    pen(1, 2, 3);
    frect(17, 45, 206, 150);
    pen(10, 12, 14);
    rect(16, 44, 208, 152);

    pen(2, 5, 8);
    frect(17, 45, 206, 18);
    int32_t tw = 0, th = 0;
    measure("SETTINGS", tw, th);
    pen(15, 15, 15);
    text("SETTINGS", 120 - tw / 2, 50);

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
                text(pal_names[palette_idx], 94, row_y + 5);
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
    } else if (save_hold_timer >= COMBO_HINT_FRAMES) {
        draw_hold_banner("HOLD TO SAVE...", save_hold_timer, rgb(12, 8, 0), rgb(4, 3, 0));
    } else if (load_hold_timer >= COMBO_HINT_FRAMES) {
        draw_hold_banner("HOLD TO LOAD...", load_hold_timer, rgb(0, 8, 12), rgb(0, 3, 5));
    } else if (banner_timer > 0) {
        // Active notification banner
        int32_t tw = 0, th = 0;
        measure(banner_text, tw, th);
        int32_t box_w = std::max<int32_t>(120, tw + 16);
        pen(banner_bg_color);
        frect(120 - box_w / 2, 216, box_w, 16);
        pen(15, 15, 15);
        text(banner_text, 120 - tw / 2, 220);
    }
}
