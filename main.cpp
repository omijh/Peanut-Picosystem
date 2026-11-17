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
static bool save_dirty = false;
static uint32_t last_save_write_ms = 0;
static uint32_t save_banner_frames = 0;
static uint32_t save_hold_timer = 0;

// Flash save persistence
// PicoSystem has 16MB of Flash. We reserve sector at 15MB mark (0x00F00000).
#define FLASH_SAVE_OFFSET (15 * 1024 * 1024)
#define FLASH_SAVE_ADDR   ((const uint8_t *)(XIP_BASE + FLASH_SAVE_OFFSET))
#define SAVE_MAGIC        0x50474253 // "PGBS" (Peanut-GB Save)

struct SaveHeader {
    uint32_t magic;
    uint32_t size;
    uint32_t checksum;
    uint32_t reserved;
};

// 3 Selectable Palettes
static uint8_t palette_idx = 0;
static const color_t palettes[3][4] = {
    // 0: Classic Game Boy DMG (Olive green)
    { rgb(13, 15, 6), rgb(9, 13, 4), rgb(4, 8, 3), rgb(1, 3, 1) },
    // 1: Game Boy Pocket (B&W Grayscale)
    { rgb(15, 15, 15), rgb(10, 10, 10), rgb(5, 5, 5), rgb(0, 0, 0) },
    // 2: Game Boy Light (Teal backlight)
    { rgb(11, 15, 14), rgb(6, 12, 11), rgb(3, 7, 7), rgb(1, 3, 3) }
};

// Display Scaling Modes
enum DisplayMode {
    DISPLAY_FULLSCREEN = 0, // 240x240 Edge-to-Edge (Fills entire display)
    DISPLAY_ASPECT_FIT = 1, // 240x216 (Full width, preserves 10:9 ratio, tiny 12px bar)
    DISPLAY_PIXEL_PERFECT = 2 // 160x144 Centered with retro bezel
};
static DisplayMode display_mode = DISPLAY_FULLSCREEN;
static uint8_t scale_x_lut[240];

// Save Cartridge RAM to non-volatile Flash
void save_to_flash() {
    uint32_t chk = 0;
    for (uint32_t i = 0; i < CART_RAM_MAX_SIZE; ++i) {
        chk += cart_ram[i];
    }

    SaveHeader hdr;
    hdr.magic = SAVE_MAGIC;
    hdr.size = CART_RAM_MAX_SIZE;
    hdr.checksum = chk;
    hdr.reserved = 0;

    // Buffer aligned to 4096-byte flash sectors (36 KB total)
    static uint8_t write_buf[36864];
    memset(write_buf, 0xFF, sizeof(write_buf));
    memcpy(write_buf, &hdr, sizeof(SaveHeader));
    memcpy(write_buf + sizeof(SaveHeader), cart_ram, CART_RAM_MAX_SIZE);

    // Visual feedback: green LED
    led(0, 50, 0);

    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(FLASH_SAVE_OFFSET, sizeof(write_buf));
    flash_range_program(FLASH_SAVE_OFFSET, write_buf, sizeof(write_buf));
    restore_interrupts(ints);

    led(0, 0, 0);
    save_dirty = false;
    save_banner_frames = 60; // Display "SAVED!" banner for ~1 second
}

// Load Cartridge RAM from non-volatile Flash
void load_from_flash() {
    const SaveHeader *hdr = (const SaveHeader *)FLASH_SAVE_ADDR;
    if (hdr->magic == SAVE_MAGIC && hdr->size <= CART_RAM_MAX_SIZE) {
        const uint8_t *data = FLASH_SAVE_ADDR + sizeof(SaveHeader);
        uint32_t chk = 0;
        for (uint32_t i = 0; i < hdr->size; ++i) {
            chk += data[i];
        }
        if (chk == hdr->checksum) {
            memcpy(cart_ram, data, hdr->size);
        }
    }
}

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
            save_dirty = true;
            last_save_write_ms = time();
        }
    }
}

void gb_error(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr) {
    // Flash red LED on error
    led(100, 0, 0);
}

// LCD Draw Line Callback (Scanline rendering with scaling)
void lcd_draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
    if (line >= 144 || SCREEN == nullptr || SCREEN->data == nullptr) {
        return;
    }

    const color_t *pal = palettes[palette_idx];

    if (display_mode == DISPLAY_FULLSCREEN) {
        // Mode 0: 240x240 Edge-to-Edge
        // X: 160 -> 240 (1.5x)
        // Y: 144 -> 240 (5/3x)
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
        // Mode 1: 240x216 (Full width, exact 10:9 aspect ratio, 12px letterbox top/bottom)
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
        // Mode 2: 160x144 Pixel-Perfect Centered with bezel
        color_t *dest = SCREEN->data + ((line + 48) * 240 + 40);
        for (int x = 0; x < 160; ++x) {
            dest[x] = pal[pixels[x] & 3];
        }
    }
}

// --- PicoSystem Lifecycle Hooks ---

void init() {
    // Precalculate horizontal 160 -> 240 scale lookup table: x_gb = (x * 2) / 3
    for (int x = 0; x < 240; ++x) {
        scale_x_lut[x] = (x * 2) / 3;
    }

    // Load any existing persistent battery save from Flash
    load_from_flash();

    // Initialize Peanut-GB
    enum gb_init_error_e init_status = gb_init(
        &gb,
        gb_rom_read,
        gb_cart_ram_read,
        gb_cart_ram_write,
        gb_error,
        (void *)ROM_DATA
    );

    if (init_status != GB_INIT_NO_ERROR) {
        led(100, 0, 0); // Red LED if ROM header error
    }

    // Set LCD scanline callback
    gb_init_lcd(&gb, lcd_draw_line);
}

void update(uint32_t tick) {
    // Map PicoSystem buttons to Game Boy Joypad (0 = pressed, 1 = released)
    gb.direct.joypad_bits.up     = !button(button::UP);
    gb.direct.joypad_bits.down   = !button(button::DOWN);
    gb.direct.joypad_bits.left   = !button(button::LEFT);
    gb.direct.joypad_bits.right  = !button(button::RIGHT);
    gb.direct.joypad_bits.a      = !button(button::A);
    gb.direct.joypad_bits.b      = !button(button::B);
    gb.direct.joypad_bits.start  = !button(button::X);
    gb.direct.joypad_bits.select = !button(button::Y);

    // Shortcut: Hold X + Y for 1.5 seconds to force save
    if (button(button::X) && button(button::Y)) {
        save_hold_timer++;
        if (save_hold_timer > 90) { // ~1.5s at 60 FPS
            save_to_flash();
            save_hold_timer = 0;
        }
    } else {
        save_hold_timer = 0;
    }

    // Auto-save: Flush to Flash 2.5 seconds after game stops writing to cartridge RAM
    if (save_dirty && (time() - last_save_write_ms > 2500)) {
        save_to_flash();
    }

    // Shortcut: Tap UP while holding X + Y to cycle color palettes
    if (button(button::X) && button(button::Y) && pressed(button::UP)) {
        palette_idx = (palette_idx + 1) % 3;
    }

    // Shortcut: Tap DOWN while holding X + Y to cycle display modes (Fullscreen, Aspect-Fit, 1:1)
    if (button(button::X) && button(button::Y) && pressed(button::DOWN)) {
        display_mode = (DisplayMode)(((int)display_mode + 1) % 3);
        // Clear screen to wipe any old bezel graphics when switching modes
        clear();
    }

    // Run 1 frame of emulation
    gb_run_frame(&gb);

    if (save_banner_frames > 0) {
        save_banner_frames--;
    }
}

void draw(uint32_t tick) {
    // Only render borders in non-fullscreen modes
    if (display_mode == DISPLAY_PIXEL_PERFECT) {
        pen(1, 1, 1); // Dark charcoal bezel
        frect(0, 0, 240, 48);
        frect(0, 192, 240, 48);
        frect(0, 48, 40, 144);
        frect(200, 48, 40, 144);

        pen(6, 6, 6);
        text("GAME BOY", 92, 28);
    } else if (display_mode == DISPLAY_ASPECT_FIT) {
        pen(0, 0, 0); // Black 12px letterbox bars
        frect(0, 0, 240, 12);
        frect(0, 228, 240, 12);
    }

    // Save notification banner (draws compactly on any mode)
    if (save_banner_frames > 0) {
        pen(0, 12, 0); // Green
        frect(80, 216, 80, 16);
        pen(15, 15, 15);
        text("SAVED!", 100, 220);
    } else if (save_hold_timer > 30) {
        // Holding save combo feedback
        pen(12, 8, 0);
        frect(70, 216, 100, 16);
        pen(15, 15, 15);
        text("SAVING...", 92, 220);
    }
}
