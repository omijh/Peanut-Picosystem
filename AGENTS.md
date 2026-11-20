# AGENTS.md — Technical Architecture & Agent Guidelines

This file provides architectural context, memory constraints, and development instructions for AI coding agents (and human contributors) extending or maintaining this repository.

---

## 1. Project Overview & Target Hardware

This project ports **Walnut-CGB** (a Game Boy / Game Boy Color fork of Peanut-GB) to the **Pimoroni PicoSystem** handheld gaming console.

### Hardware Specifications
- **MCU**: Raspberry Pi RP2040 (Dual ARM Cortex-M0+ overclocked to 250 MHz by PicoSystem SDK).
- **RAM**: 256 KB main SRAM (`0x20000000..0x20040000`) + two 4 KB Scratch banks (`SCRATCH_X`, `SCRATCH_Y` for Core stacks).
- **Flash Storage**: 16 MB QSPI NOR Flash (Winbond W25Q128JV, memory-mapped at `0x10000000`; first 12 MB usable by code + embedded ROMs per `memmap_picosystem.ld`).
- **Display**: 1.54" ST7789 IPS LCD (240×240 resolution, 12-bit 4444 framebuffer, unlocked to ~60 Hz with TE synced to line 240 at startup).
- **Inputs**: 8 tactile push buttons (`UP`, `DOWN`, `LEFT`, `RIGHT`, `A`, `B`, `X`, `Y`).
- **Audio & Power**: Piezo buzzer on `GPIO 11` (PWM Slice 5B), USB VBUS detect on `GPIO 2`, Charger STAT on `GPIO 24`.

---

## 2. Memory Budget & Constraints

RP2040 main SRAM is `262,144` bytes (with stacks in `SCRATCH_X`/`SCRATCH_Y`):

| Memory Consumer | Location | Approx Size | Notes |
| :--- | :--- | :--- | :--- |
| **PicoSystem Framebuffer (`SCREEN`)** | SRAM (`.bss`) | **115.2 KB** | 240 × 240 × 2 bytes (`RGBA4444`). |
| **Walnut-CGB CPU & GBC State (`gb_s`)** | SRAM (`.bss`) | **~50.5 KB** | 32 KB GBC WRAM (8 banks), 16 KB GBC VRAM (2 banks), OAM, HRAM, 64 B BG/OAM palettes, `fixPalette`, CPU/MBC state. |
| **Cartridge Battery RAM (`cart_ram`)** | SRAM (`.bss`) | **32.0 KB** | Covers up to 4 banks of 8 KB (MBC1, MBC2, MBC3, MBC5). |
| **Game Boy Audio (`gb_audio`)** | SRAM (`.bss`) | **~4.7 KB** | `GbApu` (~270 B) + `uint32_t duty_buf[2][548]` (4,384 B DMA ping-pong buffer). |
| **Filter LUTs & Scanline Row (`_cf_*`, `_pal_*`, `prev_row`)** | SRAM (`.bss`) | **~1.7 KB** | Color filter pipeline tables, 64-entry RGBA4444 palette cache, 240-px blend buffer. |
| **SRAM-Resident Code (`__not_in_flash_func` + tables)** | SRAM (`.data`) | **~41.7 KB** | `gb_run_frame_dualfetch`, `__gb_step_cpu`, `__gb_read*`/`__gb_write*`, `__gb_render_scanline`, `lcd_draw_line`, `gb_audio_dma_irq_handler`, `TAC_CYCLES`, `op_cycles`. |
| **Reserved Heap + Remaining Headroom** | SRAM | **2 KB + ~15 KB free** | Ends well below `0x20040000` (`__HeapLimit ~0x2003c434`). |
| **Game Boy ROM (`custom_rom_data`)** | **Flash** | **Up to ~11.5 MB** | Marked `const` in Flash XIP. Never copy whole ROMs into RAM. |
| **A/B Ping-Pong Save Slots** | **Flash** | **2 × 128 KB** | Reserved at `15.0 MB` (`0x00F00000`) and `15.125 MB` (`0x00F20000`). |

> [!CAUTION]
> **Watch SRAM tightly.** Only ~15 KB of free main SRAM remains below `0x20040000`. Keep large assets/tables `const` in Flash and always check `arm-none-eabi-objdump -h` after changing `.data`/`.bss` allocations or adding `__not_in_flash_func`.

---

## 3. A/B Ping-Pong Flash Save State Architecture

Because both Game Boy / Game Boy Color action games and battery-backed RPGs run here, the emulator implements a **Power-Loss-Safe A/B Save State Engine** across two 128 KB Flash slots:

1. **Alternating Commit Order**:
   - Slot A sits at `0x00F00000` and Slot B sits at `0x00F20000`.
   - `save_state_to_flash()` finds the currently newest valid slot, targets the **inactive** slot with `seq = newest_seq + 1`, streams `struct gb_s` (~50.5 KB) + `cart_ram` (32 KB) starting at `slot_off + 256`, and programs **Page 0 (`StateHeader`, `40` bytes)** **LAST** so an interrupted erase/program leaves the old slot intact.
2. **State Snapshot Contents**:
   - `StateHeader`: magic `0x53544154` (`"STAT"`), `STATE_VERSION = 3`, `seq`, payload sizes, 32-bit FNV-1a checksum (`gb + cart_ram`), `rom_hash`, and packed settings (`settings_magic = 0xA5`, `volume_pct`, `brightness_pct`, `audio_mode`, `color_mode`, `display_mode`).
   - Live APU registers (`power`, `regs[0..0x15]`, `wave_ram[0..0x0F]`) are synced into `gb.hram_io` prior to hashing/writing.
3. **Interrupt & DMA Safety**:
   - Waits for `while (_is_flipping()) {}`, pauses audio DMA (`gb_audio.pause()`), disables interrupts (`save_and_disable_interrupts()`), erases/programs Flash, restores interrupts, and resumes audio.
4. **Pointer Re-hooking on Restore**:
   - `load_state_from_flash()` picks the highest-`seq` valid slot, restores `gb` and `cart_ram`, re-hooks all Walnut-CGB callbacks (`gb_rom_read`, `gb_rom_read_16bit`, `gb_rom_read_32bit`, `gb_cart_ram_read`, `gb_cart_ram_write`, `gb_error`, `display.lcd_draw_line`, `direct.priv`), resets `gb.direct.joypad = 0xFF`, reapplies color tables, marks `fixPaletteDirty = 1`, and syncs `gb_audio.restore_from_hram(gb.hram_io)`.

---

## 4. Color Pipeline & Blended Display Scaling (240×240)

- **Color Filter Pipeline (`cf_build_tables` / `_pal_lut` / `_dmg_lut`)**:
  - Converts changed CGB palette entries (`gb.cgb.fixPalette[0x40]`) through a 3×3 color matrix + 256-entry per-channel output curves on palette changes (`fixPaletteDirty || pal_lut_stale`), never per pixel.
  - Supports 9 `ColorMode`s: `COLOR_GBC_LCD` (default; GBC bleed + lift on CGB, Teal Light on DMG), `COLOR_RAW` (unfiltered GBC / 12-color DMG), `COLOR_VIVID`, `COLOR_PASTEL`, `COLOR_TEAL`, `COLOR_DMG_GREEN`, `COLOR_POCKET_BW`, `COLOR_SEPIA`, and `COLOR_NIGHT`.
- **Blended Scaling (`blend_avg`)**:
  - Uses branchless packed RGBA4444 channel averaging `(a & b) + (((a ^ b) & 0xEEEEu) >> 1)` horizontally (`160 → 240` as `(c0, avg(c0,c1), c1)`) and vertically (`prev_row`) in **`DISPLAY_FULLSCREEN` (240×240, 5:3 blended)** and **`DISPLAY_ASPECT_FIT` (240×216, 3:2 blended)**, alongside **`DISPLAY_PIXEL_PERFECT` (160×144 centered)**.

---

## 5. Key Files & Responsibilities

- **`main.cpp`**:
  - Walnut-CGB direct ROM/Cart-RAM hooks (`WGB_ROM_READ*`, `WGB_CART_RAM_*`, `WGB_RAM_DATA`), `init()` (GPIO 2/24 charge sense, `configure_st7789_60hz()`, `measure_te_period_us()`, DMA frame-period calibration, DMG post-boot APU routing, boot auto-resume), `update()` (`gb_run_frame_dualfetch`, hold combos with D-Pad guards, paused Settings Menu), `draw()`, and A/B Flash save/load.
- **`gb_audio.hpp`**:
  - `GbApu`: 4-channel DMG APU with 200 Hz 1-pole HPF, ~5.2 kHz 1-pole LPF, shift-only cascaded soft-knee limiter (`soft_limit_s16`), `0..100%` volume (default `30%`), and piezo pitch/envelope extraction.
  - `GbAudioDriver`: Defaults to `AUDIO_PIEZO_BEEP`; drives GPIO 11 (12 mA, fast slew) via direct frequency beeper (`AUDIO_PIEZO_BEEP`), 12-bit 61.035 kHz ultrasonic PWM double-buffered on **DMA Channel 1** (`DMA_IRQ_1`) paced by a DMA DREQ timer tuned to `frame_period_us` (`AUDIO_PWM_APU`), or mute (`AUDIO_OFF`).
- **`walnut_cgb.h`**:
  - Upstream/adapted single-header GBC + DMG core (`WALNUT_FULL_GBC_SUPPORT = 1`).
- **`embed_rom.py`**:
  - Converts `.gb`/`.gbc` binaries (up to 12 MB) into `rom.h`, detects GBC/DMG mode, and updates `main.cpp` mtime.
- **`build.sh`**:
  - Detects ARM toolchain, Pico SDK, and pioasm, enforces `PICO_BOARD=pimoroni_picosystem`, and compiles `./build/peanut_picosystem.uf2`.

---

## 6. Development & Testing Workflow

### Building the Project
```bash
./build.sh
```

### Changing or Adding a ROM
```bash
python3 embed_rom.py <path/to/game.gb_or_gbc>
./build.sh
```

### Clean Rebuild
```bash
rm -rf build
./build.sh
```
