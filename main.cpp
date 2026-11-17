#include <cstdio>
#include <cstring>
#include "picosystem.hpp"
#include "hardware/flash.h"
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
static uint32_t save_hold_timer = 0;
static uint32_t load_hold_timer = 0;

// ============================================================================
// Flash Save State Persistence Architecture
// ============================================================================
// PicoSystem has 16MB of Flash. We reserve 64 KB at the 15 MB mark (0x00F00000).
#define FLASH_STATE_OFFSET (15 * 1024 * 1024)
#define FLASH_STATE_ADDR   ((const uint8_t *)(XIP_BASE + FLASH_STATE_OFFSET))
#define STATE_MAGIC        0x53544154 // "STAT"

struct StateHeader {
    uint32_t magic;      // STATE_MAGIC
    uint32_t version;    // 1
    uint32_t gb_size;    // sizeof(struct gb_s)
    uint32_t ram_size;   // sizeof(cart_ram)
    uint32_t checksum;   // additive checksum of gb + cart_ram
    uint32_t reserved[3];
};

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

static uint32_t calc_state_checksum() {
    uint32_t chk = 0;
    const uint8_t *p_gb = (const uint8_t *)&gb;
    for (size_t i = 0; i < sizeof(gb); ++i) {
        chk += p_gb[i];
    }
    for (size_t i = 0; i < sizeof(cart_ram); ++i) {
        chk += cart_ram[i];
    }
    return chk;
}

// Forward declarations for callbacks
uint8_t gb_rom_read(struct gb_s *gb, const uint_fast32_t addr);
uint8_t gb_cart_ram_read(struct gb_s *gb, const uint_fast32_t addr);
void gb_cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val);
void gb_error(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr);
void lcd_draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line);

// Save full state (CPU + WRAM + VRAM + Cart RAM) to Flash
void save_state_to_flash() {
    StateHeader hdr;
    hdr.magic = STATE_MAGIC;
    hdr.version = 1;
    hdr.gb_size = sizeof(gb);
    hdr.ram_size = sizeof(cart_ram);
    hdr.checksum = calc_state_checksum();
    for (int i = 0; i < 3; ++i) hdr.reserved[i] = 0;

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

    led(0, 0, 0);
    cart_ram_dirty = false;

    // Show onscreen banner
    banner_text = "STATE SAVED!";
    banner_bg_color = rgb(0, 12, 0);
    banner_timer = 90; // ~1.5s at 60 FPS
}

// Restore full state from Flash
bool load_state_from_flash() {
    const StateHeader *hdr = (const StateHeader *)FLASH_STATE_ADDR;
    if (hdr->magic != STATE_MAGIC || hdr->version != 1 ||
        hdr->gb_size != sizeof(gb) || hdr->ram_size != sizeof(cart_ram)) {
        return false;
    }

    const uint8_t *ptr = FLASH_STATE_ADDR + sizeof(StateHeader);
    const uint8_t *saved_gb = ptr;
    ptr += sizeof(gb);
    const uint8_t *saved_ram = ptr;

    // Verify checksum
    uint32_t chk = 0;
    for (size_t i = 0; i < sizeof(gb); ++i) chk += saved_gb[i];
    for (size_t i = 0; i < sizeof(cart_ram); ++i) chk += saved_ram[i];

    if (chk != hdr->checksum) {
        return false;
    }

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

    // Release any joypad button latch
    gb.direct.joypad = 0xFF;

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

    // If 'Y' is held during power-on/boot, start clean from Title Screen
    bool fresh_boot = button(button::Y);

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
    // QUICK SAVE STATE: Hold X + Y for 1.0 second
    // ------------------------------------------------------------------------
    if (button(button::X) && button(button::Y) && !button(button::UP) && !button(button::DOWN)) {
        save_hold_timer++;
        if (save_hold_timer > 60) { // 1.0s at 60 FPS
            save_state_to_flash();
            save_hold_timer = 0;
        }
    } else {
        save_hold_timer = 0;
    }

    // ------------------------------------------------------------------------
    // QUICK LOAD STATE: Hold A + B for 1.0 second
    // ------------------------------------------------------------------------
    if (button(button::A) && button(button::B) && !button(button::X) && !button(button::Y)) {
        load_hold_timer++;
        if (load_hold_timer > 60) { // 1.0s at 60 FPS
            if (load_state_from_flash()) {
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
        }
    } else {
        load_hold_timer = 0;
    }

    // Auto-save battery RAM (Pokemon in-game saves)
    if (cart_ram_dirty && (time() - last_cart_write_ms > 2500)) {
        save_state_to_flash();
    }

    // Shortcut: Tap UP while holding X + Y to cycle color palettes
    if (button(button::X) && button(button::Y) && pressed(button::UP)) {
        palette_idx = (palette_idx + 1) % 3;
    }

    // Shortcut: Tap DOWN while holding X + Y to cycle display modes
    if (button(button::X) && button(button::Y) && pressed(button::DOWN)) {
        display_mode = (DisplayMode)(((int)display_mode + 1) % 3);
        clear();
    }

    // Run 1 frame of emulation
    gb_run_frame(&gb);

    if (banner_timer > 0) {
        banner_timer--;
    }
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
    if (save_hold_timer > 20) {
        pen(12, 8, 0); // Amber
        frect(60, 216, 120, 16);
        pen(15, 15, 15);
        text("SAVING STATE...", 68, 220);
    } else if (load_hold_timer > 20) {
        pen(0, 8, 12); // Blue
        frect(60, 216, 120, 16);
        pen(15, 15, 15);
        text("LOADING STATE...", 65, 220);
    } else if (banner_timer > 0) {
        // Active notification banner
        pen(banner_bg_color);
        frect(65, 216, 110, 16);
        pen(15, 15, 15);
        text(banner_text, 75, 220);
    }
}
