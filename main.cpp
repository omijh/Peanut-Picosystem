#include <cstdio>
#include <cstring>
#include "picosystem.hpp"
#include "hardware/flash.h"
#include "hardware/gpio.h"
#include "hardware/sync.h"

#define ENABLE_LCD 1
#define ENABLE_SOUND 0
#define PEANUT_GB_12_COLOUR 0
#include "peanut_gb.h"

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

// 32 KB Cartridge RAM (Save data for MBC1, MBC2, MBC3, etc. - covers Pokemon Gen 1 & 2)
#define CART_RAM_MAX_SIZE (32 * 1024)
static uint8_t cart_ram[CART_RAM_MAX_SIZE];
static bool cart_ram_dirty = false;
static uint32_t last_cart_write_ms = 0;

// On-screen notification banners
static uint32_t banner_timer = 0;
static const char *banner_text = "";
static color_t banner_bg_color = rgb(0, 12, 0);

// Hold timers for button combos
static constexpr uint32_t COMBO_HOLD_FRAMES = 60;
static constexpr uint32_t COMBO_HINT_FRAMES = 15;
static uint32_t save_hold_timer = 0;
static uint32_t load_hold_timer = 0;
static bool save_combo_consumed = false;
static bool load_combo_consumed = false;

// ============================================================================
// Flash Save State Persistence Architecture
// ============================================================================
// PicoSystem has 16MB of Flash. We reserve 64 KB at the 15 MB mark (0x00F00000).
#define FLASH_STATE_OFFSET    (15 * 1024 * 1024)
#define FLASH_STATE_MAX_SIZE  (64 * 1024)
#define FLASH_STATE_READ_ADDR ((const uint8_t *)(XIP_NOCACHE_NOALLOC_BASE + FLASH_STATE_OFFSET))
#define STATE_MAGIC           0x53544154u // "STAT"
#define STATE_VERSION         2u

#ifdef PICO_FLASH_SIZE_BYTES
static_assert(PICO_FLASH_SIZE_BYTES >= FLASH_STATE_OFFSET + FLASH_STATE_MAX_SIZE,
              "PICO_FLASH_SIZE_BYTES too small for save state; build with PICO_BOARD=pimoroni_picosystem");
#endif

struct StateHeader {
    uint32_t magic;      // STATE_MAGIC ("STAT")
    uint32_t version;    // STATE_VERSION
    uint32_t gb_size;    // sizeof(struct gb_s)
    uint32_t ram_size;   // sizeof(cart_ram)
    uint32_t checksum;   // FNV-1a 32-bit hash of gb + cart_ram
    uint32_t rom_hash;   // FNV-1a 32-bit hash of ROM header + ROM_SIZE
    uint32_t reserved[2];
};

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
    StateHeader hdr;
    hdr.magic = STATE_MAGIC;
    hdr.version = STATE_VERSION;
    hdr.gb_size = sizeof(gb);
    hdr.ram_size = sizeof(cart_ram);
    hdr.checksum = calc_state_checksum((const uint8_t *)&gb, cart_ram);
    hdr.rom_hash = calc_rom_hash();
    hdr.reserved[0] = 0;
    hdr.reserved[1] = 0;

    uint32_t total_payload = sizeof(StateHeader) + sizeof(gb) + sizeof(cart_ram);
    uint32_t erase_bytes = ((total_payload + FLASH_SECTOR_SIZE - 1) / FLASH_SECTOR_SIZE) * FLASH_SECTOR_SIZE;

    // Wait for any active screen DMA transfer before halting interrupts for Flash I/O
    while (_is_flipping()) {}

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

    led(0, 0, 0);

    bool ok = verify_flash_state();
    if (ok) {
        cart_ram_dirty = false;
        banner_text = "STATE SAVED!";
        banner_bg_color = rgb(0, 12, 0);
        banner_timer = 90; // ~1.5s at 60 FPS
    } else {
        banner_text = "SAVE FAILED!";
        banner_bg_color = rgb(14, 0, 0);
        banner_timer = 90;
    }
    return ok;
}

// Restore full state from Flash
bool load_state_from_flash() {
    if (!verify_flash_state()) {
        return false;
    }

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

    return true;
}

// ============================================================================
// Display Palettes & Scaling
// ============================================================================

static uint8_t palette_idx = 0;
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

// Peanut-GB Callbacks
uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr) {
    const uint8_t *rom = (const uint8_t *)gb->direct.priv;
    return rom[addr];
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
    led(100, 0, 0);
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

    // PicoSystem populates _io after init(); buttons are active-low with pull-ups.
    // If 'Y' is held during power-on/boot, start clean from Title Screen.
    bool fresh_boot = !gpio_get(button::Y);

    if (!fresh_boot) {
        // Attempt to auto-resume from saved state
        if (load_state_from_flash()) {
            banner_text = "RESUMED!";
            banner_bg_color = rgb(0, 10, 14); // Cyan
            banner_timer = 90;
        }
    }
}

void update(uint32_t tick) {
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
    // Shortcuts: Hold X + Y and tap UP (palette) or DOWN (display mode)
    // ------------------------------------------------------------------------
    if (button(button::X) && button(button::Y)) {
        if (pressed(button::UP)) {
            palette_idx = (palette_idx + 1) % 3;
            save_combo_consumed = true;
            save_hold_timer = 0;
        }
        if (pressed(button::DOWN)) {
            display_mode = (DisplayMode)(((int)display_mode + 1) % 3);
            pen(0, 0, 0);
            clear();
            save_combo_consumed = true;
            save_hold_timer = 0;
        }
    } else {
        save_hold_timer = 0;
        save_combo_consumed = false;
    }

    // ------------------------------------------------------------------------
    // QUICK SAVE STATE: Hold X + Y for 1.0 second
    // ------------------------------------------------------------------------
    if (button(button::X) && button(button::Y) && !button(button::UP) && !button(button::DOWN)) {
        if (!save_combo_consumed) {
            save_hold_timer++;
            if (save_hold_timer >= COMBO_HOLD_FRAMES) { // 1.0s at 60 FPS
                save_state_to_flash();
                save_hold_timer = 0;
                save_combo_consumed = true;
            }
        }
    } else {
        save_hold_timer = 0;
    }

    // ------------------------------------------------------------------------
    // QUICK LOAD STATE: Hold A + B for 1.0 second
    // ------------------------------------------------------------------------
    if (button(button::A) && button(button::B) && !button(button::X) && !button(button::Y)) {
        if (!load_combo_consumed) {
            load_hold_timer++;
            if (load_hold_timer >= COMBO_HOLD_FRAMES) { // 1.0s at 60 FPS
                if (load_state_from_flash()) {
                    pen(0, 0, 0);
                    clear();
                    banner_text = "STATE LOADED!";
                    banner_bg_color = rgb(0, 10, 14); // Cyan
                    banner_timer = 90;
                } else {
                    banner_text = "NO SAVE FOUND!";
                    banner_bg_color = rgb(14, 0, 0); // Red
                    banner_timer = 60;
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

    // Auto-save battery RAM (Pokemon in-game saves)
    if (cart_ram_dirty && (time() - last_cart_write_ms > 2500)) {
        if (!save_state_to_flash()) {
            last_cart_write_ms = time();
        }
    }

    // Run 1 frame of emulation
    gb_run_frame(&gb);

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

void draw(uint32_t tick) {
    // Non-fullscreen bezels
    if (display_mode == DISPLAY_PIXEL_PERFECT) {
        pen(1, 1, 1);
        frect(0, 0, 240, 48);
        frect(0, 192, 240, 48);
        frect(0, 48, 40, 144);
        frect(200, 48, 40, 144);

        pen(6, 6, 6);
        text("GAME BOY", 92, 28);
    } else if (display_mode == DISPLAY_ASPECT_FIT) {
        pen(0, 0, 0);
        frect(0, 0, 240, 12);
        frect(0, 228, 240, 12);
    }

    // On-screen feedback when holding Save or Load combos
    if (save_hold_timer >= COMBO_HINT_FRAMES) {
        draw_hold_banner("HOLD TO SAVE...", save_hold_timer, rgb(12, 8, 0), rgb(4, 3, 0));
    } else if (load_hold_timer >= COMBO_HINT_FRAMES) {
        draw_hold_banner("HOLD TO LOAD...", load_hold_timer, rgb(0, 8, 12), rgb(0, 3, 5));
    } else if (banner_timer > 0) {
        // Active notification banner
        pen(banner_bg_color);
        frect(60, 216, 120, 16);
        pen(15, 15, 15);
        int32_t tw = 0, th = 0;
        measure(banner_text, tw, th);
        text(banner_text, 120 - tw / 2, 220);
    }
}
