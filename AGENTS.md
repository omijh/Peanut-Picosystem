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
| **Peanut-GB CPU & State (`gb_s`)** | SRAM | **~17.1 KB** | Includes 8 KB WRAM, 8 KB VRAM, OAM, HRAM, and CPU registers. |
| **Cartridge Battery RAM (`cart_ram`)** | SRAM | **32 KB** | Accommodates 4 banks of 8 KB (sufficient for MBC1, MBC2, and MBC3 / Pokémon Gen 1 & 2). |
| **Free SRAM Headroom** | SRAM | **~99 KB** | Available for stack, heap, and runtime. |
| **Game Boy ROM (`custom_rom_data`)** | **Flash** | **Up to ~14 MB** | Marked `const` so it lives in Flash and executes in place via XIP cache. Never copy ROMs into RAM. |
| **Save State Sector** | **Flash** | **64 KB** | Reserved at offset `15 MB` (`0x00F00000`) for full emulator snapshot + SRAM. |

> [!CAUTION]
> **Do not allocate large static buffers in SRAM.** All textures, lookup tables, and ROM images must be declared `const` to reside in Flash memory.

---

## 3. Save State & Persistence Architecture

Because many action and homebrew Game Boy games (such as *Karpe Diem*) do not have in-game battery save menus, the emulator implements a **Full Save State Engine**:

1. **State Snapshot Payload**:
   - `StateHeader` (magic `0x53544154` / "STAT", version, payload sizes, 32-bit checksum).
   - Entire `struct gb_s` (~17.1 KB): CPU registers (PC, SP, A, F, BC, DE, HL), timers, flags, 8 KB WRAM, 8 KB VRAM, OAM, HRAM.
   - `cart_ram` (32 KB): Cartridge SRAM.
   - Total payload: ~49.9 KB.
2. **Streaming Page Writer (`FlashWriter`)**:
   - Instead of allocating a 64 KB intermediate buffer in SRAM (which would starve RAM), `FlashWriter` uses a single **256-byte** page buffer on the stack.
   - Data is streamed in 256-byte chunks directly to `flash_range_program()`.
3. **Interrupt Safety**:
   - `save_and_disable_interrupts()` must precede all flash operations, and `restore_interrupts()` must follow.
4. **Pointer Re-hooking on Restore**:
   - When `load_state_from_flash()` overwrites `gb`, it immediately re-assigns all native C++ callbacks (`gb_rom_read`, `gb_cart_ram_read`, `gb_cart_ram_write`, `gb_error`, `lcd_draw_line`, `direct.priv`) to ensure pointers remain valid across firmware builds.
   - `gb.direct.joypad` is reset to `0xFF` to prevent stuck button states.

---

## 4. Display Scaling Engine (240×240)

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

## 5. Key Files & Responsibilities

- **`main.cpp`**:
  - `init()`: Sets up LUT tables, checks Flash for saved state (with auto-resume), initializes Peanut-GB, and sets up LCD line renderer.
  - `update(uint32_t tick)`: Polls buttons, checks hold combos (Quick Save: `X + Y`, Quick Load: `A + B`, Hotkeys: `X+Y+UP` / `X+Y+DOWN`), runs `gb_run_frame(&gb)`.
  - `draw(uint32_t tick)`: Renders bezels (if in letterbox/centered mode) and onscreen notification banners.
  - `save_state_to_flash()` / `load_state_from_flash()`: Serializes and restores the full emulator state.
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
