# AGENTS.md — Technical Architecture & Agent Guidelines

This file provides architectural context, memory constraints, and development instructions for AI coding agents (and human contributors) extending or maintaining this repository.

---

## 1. Project Overview & Target Hardware

This project ports **Peanut-GB** (a header-only Game Boy DMG emulator) to the **Pimoroni PicoSystem** handheld gaming console.

### Hardware Specifications
- **MCU**: Raspberry Pi RP2040 (Dual ARM Cortex-M0+ running at 250 MHz by default).
- **RAM**: 264 KB internal SRAM (in 6 banks).
- **Flash Storage**: 16 MB QSPI NOR Flash (Winbond W25Q128JV or equivalent, memory-mapped to `0x10000000`).
- **Display**: 1.54" ST7789 IPS LCD (240×240 resolution, 16-bit color / 4096 palette mode).
- **Inputs**: 8 tactile push buttons: D-Pad (Up, Down, Left, Right), A, B, X, Y.
- **Audio**: Single piezo buzzer driven via PWM.

---

## 2. Memory Budget & Constraints

Because the RP2040 has 264 KB of SRAM, memory layout must be managed strictly:

| Memory Consumer | Location | Size | Notes |
| :--- | :--- | :--- | :--- |
| **PicoSystem Framebuffer (`SCREEN`)** | SRAM | **115.2 KB** | 240 × 240 × 2 bytes. Retained between frames. |
| **Peanut-GB CPU & State (`gb_s`)** | SRAM | **~16 KB** | Includes 8 KB WRAM, 8 KB VRAM, OAM, HRAM, and CPU registers. |
| **Cartridge Battery RAM (`cart_ram`)** | SRAM | **32 KB** | Accommodates 4 banks of 8 KB (sufficient for MBC1, MBC2, and MBC3 / Pokémon Gen 1 & 2). |
| **Free SRAM Headroom** | SRAM | **~100 KB** | Available for stack, heap, and runtime. |
| **Game Boy ROM (`custom_rom_data`)** | **Flash** | **Up to ~14 MB** | Marked `const` so it lives in Flash and executes in place via XIP cache. Never copy ROMs into RAM. |
| **Battery Save Sector** | **Flash** | **36 KB** | Reserved at offset `15 MB` (`0x00F00000`) for non-volatile cartridge SRAM saves. |

> [!CAUTION]
> **Do not allocate large static buffers in SRAM.** All textures, lookup tables, and ROM images must be declared `const` to reside in Flash memory.

---

## 3. Flash Persistence Architecture

Writing to RP2040 Flash memory requires strict precautions:

1. **Bus Stall during Flash operations**:
   - The RP2040 executes code from Flash (XIP). When `flash_range_erase` or `flash_range_program` is invoked, Flash cache reads are disabled.
   - **Always disable interrupts** using `uint32_t ints = save_and_disable_interrupts();` before touching Flash, and re-enable with `restore_interrupts(ints);`.
2. **Alignment Rules**:
   - `flash_range_erase` must be aligned to `FLASH_SECTOR_SIZE` (4096 bytes).
   - `flash_range_program` must be aligned to `FLASH_PAGE_SIZE` (256 bytes).
3. **Data Verification**:
   - Saves are prefixed with `struct SaveHeader` containing `magic = 0x50474253` (`PGBS`), size, and a 32-bit additive checksum to prevent restoring corrupted data.

---

## 4. Key Files & Responsibilities

- **`main.cpp`**:
  - `init()`: Reads existing Flash save data into `cart_ram`, initializes Peanut-GB with callbacks, and sets up the LCD line renderer.
  - `update(uint32_t tick)`: Polls PicoSystem hardware buttons, maps them to `gb.direct.joypad_bits`, manages the auto-save write timer, and calls `gb_run_frame(&gb)`.
  - `draw(uint32_t tick)`: Renders the outer bezel and on-screen save notifications.
  - `lcd_draw_line()`: Scanline callback from Peanut-GB. Translates 160 pixels into 16-bit RGB values and centers them at `(x + 40, line + 48)` in `SCREEN->data`.
- **`peanut_gb.h`**:
  - Upstream single-header Game Boy DMG emulator core. Configured with:
    - `#define ENABLE_LCD 1`
    - `#define ENABLE_SOUND 0` (for initial stability)
    - `#define PEANUT_GB_12_COLOUR 0`
- **`embed_rom.py`**:
  - Converts a `.gb` binary into `rom.h` (`custom_rom_data` byte array).
- **`build.sh`**:
  - Detects the local ARM GNU toolchain, Pico SDK, and pioasm install paths, runs CMake, and executes parallel compilation.

---

## 5. Development & Testing Workflow

### Building the Project
```bash
./build.sh
```

### Changing or Adding a ROM
```bash
python3 embed_rom.py <path/to/game.gb>
./build.sh
```

### Clean Rebuild
```bash
rm -rf build
./build.sh
```

---

## 6. Guidelines for Agents Modifying Code

- **Adding Sound Support**:
  - Peanut-GB supports audio hooks (`audio_read`, `audio_write`).
  - PicoSystem provides `play(voice, freq, duration)`. For full chiptune APU synthesis, route square-wave and noise registers to a PWM audio interrupt on the secondary core (Core 1).
- **Palette Enhancements**:
  - Color palettes are defined in `palettes[3][4]`. New palettes can be added by expanding the array and updating the cycling logic in `update()`.
- **Dual-Core Work**:
  - If utilizing Core 1 (via `multicore_launch_core1`), ensure Core 1 is paused (`multicore_lockout_start_blocking()`) before any Flash erase/program operations.
