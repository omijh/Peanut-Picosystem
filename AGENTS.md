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

## 3. Display Scaling Engine (240×240)

The Game Boy PPU generates 160×144 scanlines, while PicoSystem has a native 240×240 IPS LCD.
The emulator supports three modes (switched with `X + Y + DOWN`):

1. **`DISPLAY_FULLSCREEN` (Default - 240×240)**:
   - **Horizontal (160 → 240)**: Scaled using precomputed LUT `scale_x_lut[x] = (x * 2) / 3`.
   - **Vertical (144 → 240)**: Integer scaling where each Game Boy scanline $L$ spans from $Y = (L \times 5) / 3$ to $Y_{next} = ((L + 1) \times 5) / 3$. Every 3 GB lines cleanly produce 5 screen scanlines ($48 \times 5 = 240$ lines).
   - Zero black borders.
2. **`DISPLAY_ASPECT_FIT` (240×216)**:
   - Horizontal scaled to 240 pixels.
   - Vertical scaled to 216 pixels ($144 \times 1.5$) with 12px letterbox bars top and bottom, preserving the exact 10:9 aspect ratio.
3. **`DISPLAY_PIXEL_PERFECT` (160×144)**:
   - Centered 1:1 original resolution with a classic dark bezel and Game Boy logo.

---

## 4. Flash Persistence Architecture

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

## 5. Key Files & Responsibilities

- **`main.cpp`**:
  - `init()`: Sets up LUT tables, reads existing Flash save data into `cart_ram`, initializes Peanut-GB, and sets up the LCD line renderer.
  - `update(uint32_t tick)`: Polls PicoSystem hardware buttons, maps them to `gb.direct.joypad_bits`, manages the auto-save write timer, handles hotkeys (palette switch, display mode switch, force save), and calls `gb_run_frame(&gb)`.
  - `draw(uint32_t tick)`: Renders bezels (if not in fullscreen) and on-screen save notifications.
  - `lcd_draw_line()`: Scanline callback from Peanut-GB. Implements real-time scaling (Fullscreen 240×240, Aspect Fit 240×216, or 1:1 centered).
- **`peanut_gb.h`**:
  - Upstream single-header Game Boy DMG emulator core.
- **`embed_rom.py`**:
  - Converts a `.gb` binary into `rom.h` and touches `main.cpp` to ensure automatic recompilation.
- **`build.sh`**:
  - Detects the local ARM GNU toolchain, Pico SDK, and pioasm install paths, checks ROM freshness, runs CMake, and executes parallel compilation.

---

## 6. Development & Testing Workflow

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
