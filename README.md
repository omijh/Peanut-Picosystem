# Peanut-GB / Walnut-CGB for Pimoroni PicoSystem

A fast, full-color **Game Boy Color (GBC)** & **Game Boy (DMG)** emulator for the **Pimoroni PicoSystem** (RP2040) powered by **Walnut-CGB**.

Features **full Game Boy Color palettes & color filters**, **blended edge-to-edge 240×240 scaling**, **60 Hz ST7789 tear-free display unlock**, **DMA-paced 12-bit audio & piezo beeper modes**, and **power-loss-safe A/B Flash save states with auto-resume**.

---

## Highlights

- **Full Game Boy Color (CGB) + Classic DMG Support**:
  - Automatically boots `.gbc` and dual-mode ROMs in full color (`32 KB WRAM`, `16 KB VRAM`, CGB double-speed, HDMA, and 64-entry RGB565 palettes) while supporting classic `.gb` DMG games with Teal default or 12-color GBC-style palettes.
- **9 Color Modes & Authentic GBC Filter Pipeline**:
  - Switch live between **GBC LCD** (authentic cross-channel GBC LCD bleed + shadow lift on GBC; Teal Light on DMG, default), **RAW COLOR** (unfiltered GBC / 12-color DMG), **VIVID**, **PASTEL**, **TEAL LIGHT**, **DMG GREEN**, **POCKET B&W**, **SEPIA**, and **NIGHT**.
- **60 Hz ST7789 Unlock & Blended 240×240 Display**:
  - Reconfigures the ST7789 panel to **~60 Hz** (`FRCTRL2`) with TE synced to off-glass line 240 (`STE`) for full-speed tear-free frame pacing.
  - Uses branchless 50/50 RGBA4444 pixel averaging (`blend_avg`) for smooth **FULLSCREEN** (240×240, default) and **ASPECT FIT** (240×216) scaling, plus **1:1 CENTER** (160×144).
- **Power-Loss-Safe A/B Ping-Pong Flash Save States**:
  - Alternates between two 128 KB Flash slots (`15.0 MB` and `15.125 MB`), writing the `StateHeader` commit record **last** so a sudden power cut during saving never corrupts your previous save.
  - Saves full CGB CPU, 32 KB WRAM, 16 KB VRAM, palettes, 32 KB Cartridge RAM, and user settings, with automatic resume on boot.
- **4-Channel DMA Audio & Piezo Beeper Mode**:
  - Defaults to **Piezo Beeper Mode** (`AUDIO_PIEZO_BEEP`) converting melodies, frequency sweeps, and percussion into crisp piezo tones.
  - **4-CH PWM** mode uses **12 mA fast-slew pad drive**, a **12-bit 61 kHz ultrasonic PWM carrier** pushed via **RP2040 DMA Channel 1** paced by the measured display rate, a **200 Hz high-pass filter**, **~5.2 kHz low-pass filter**, and a **cascaded soft-knee limiter** for clear, loud polyphonic audio with minimal CPU overhead.
- **Quick Settings Menu (`X + Y + LEFT`)**:
  - Pauses emulation to adjust **Volume (`0–100%`)**, **Brightness (`10–100%`)**, **Audio Mode**, **Color Mode**, and **Display Mode**, with a live **Battery & USB Charging indicator (`CHG`)** and `GBC`/`DMG` status badge in the menu header.
  - Both Volume and Brightness default low at **30%** on boot.
- **Zero External Storage Needed**:
  - ROMs (`.gb` / `.gbc`) are embedded directly into Flash via `embed_rom.py`.

---

## Controls & Hotkeys

### Gameplay Controls
| PicoSystem Button | Game Boy Button | Action |
| :--- | :--- | :--- |
| **D-Pad** | **D-Pad** | Directional movement |
| **A / B** | **A / B** | Primary / Secondary actions |
| **X** | **START** | Start / In-game pause |
| **Y** | **SELECT** | Select |

### Save States & Auto-Resume
| Action | Button Combination | How It Works |
| :--- | :--- | :--- |
| **Quick Save State** | Hold **X + Y** (~1.0s) | Keep holding while the `"HOLD TO SAVE..."` bar fills until the green LED blinks and `"STATE SAVED!"` appears. Writes safely to the inactive A/B Flash slot along with your settings. |
| **Quick Load State** | Hold **A + B** (~1.0s, without D-Pad) | Keep holding while the `"HOLD TO LOAD..."` bar fills until `"STATE LOADED!"` appears (restores newest valid A/B save slot and settings). |
| **Auto-Resume on Boot** | *Power ON normally* | Automatically restores the newest valid save state and settings, displaying `"RESUMED!"`. |
| **Fresh Boot (Skip Save)** | Hold **Y** while powering ON | Skips auto-resume and boots cleanly from the title screen. |

> [!TIP]
> Games that write to battery-backed cartridge SRAM also auto-save to the inactive A/B Flash slot ~2.5 seconds after writing save data.

### Quick Settings Menu (`X + Y + LEFT`)
1. **Open**: Hold **X + Y** (right thumb) and tap **LEFT** (left thumb). Emulation and audio pause automatically.
2. **Navigate**: Press **UP / DOWN** to select a row, and **LEFT / RIGHT** to adjust values (hold **LEFT** or **RIGHT** to slide Volume/Brightness smoothly).
3. **Resume Game**: Press **B**, **X**, or **X + Y + LEFT** to return to gameplay.

| Setting | Range / Options | Default | Notes |
| :--- | :--- | :--- | :--- |
| **VOLUME** | `0%` – `100%` (10% steps) | **30%** | Master speaker level (`0%` silences output cleanly). |
| **BRIGHT** | `10%` – `100%` (10% steps) | **30%** | Screen backlight level (clamped at `10%` minimum). |
| **AUDIO** | `4-CH PWM` ⇄ `PIEZO BEEP` ⇄ `MUTED` | **PIEZO BEEP** | Switch between direct piezo tones (default), 12-bit DMA 61 kHz polyphonic PWM audio, or mute. |
| **COLOR** | `GBC LCD` ⇄ `RAW COLOR` ⇄ `VIVID` ⇄ `PASTEL` ⇄ `TEAL LIGHT` ⇄ `DMG GREEN` ⇄ `POCKET B&W` ⇄ `SEPIA` ⇄ `NIGHT` | **GBC LCD** | Authentic GBC color bleed/lift on CGB games (Teal on DMG) plus filters & 12-color DMG mode. |
| **DISPLAY** | `FULLSCREEN` ⇄ `ASPECT FIT` ⇄ `1:1 CENTER` | **FULLSCREEN** | Blended 240×240 fullscreen, blended 10:9 letterboxed 240×216, or 1:1 160×144 bezel. |

> [!TIP]
> Settings adjusted in the Quick Settings Menu or via hotkeys are saved to Flash whenever you Quick Save with **X + Y** and restored on Quick Load and boot.

### Quick Shortcuts (No Menu Needed)
Hold **X + Y** and tap a D-Pad direction to toggle modes mid-game without pausing:

| Action | Button Combination | Description |
| :--- | :--- | :--- |
| **Cycle Color Mode** | Hold **X + Y** + tap **UP** | Cycles `GBC LCD` → `RAW COLOR` → `VIVID` → `PASTEL` → `TEAL LIGHT` → `DMG GREEN` → `POCKET B&W` → `SEPIA` → `NIGHT` |
| **Cycle Display Mode** | Hold **X + Y** + tap **DOWN** | Cycles `FULLSCREEN` (240×240) ⇄ `ASPECT FIT` (240×216) ⇄ `1:1 CENTER` (160×144) |
| **Cycle Audio Mode** | Hold **X + Y** + tap **RIGHT** | Cycles `4-CH PWM` (12-bit DMA polyphonic music) ⇄ `PIEZO BEEP` (crisp piezo tones) ⇄ `MUTED` |
| **Open Settings Menu** | Hold **X + Y** + tap **LEFT** | Opens the paused **Quick Settings Menu** with battery/charging status |

---

## Quick Start Guide

### 1. Prerequisites (macOS / Linux)

**macOS (Homebrew):**
```bash
brew install cmake arm-none-eabi-gcc picotool
```

**Ubuntu / Debian:**
```bash
sudo apt update
sudo apt install cmake gcc-arm-none-eabi libnewlib-arm-none-eabi build-essential
```

### 2. Clone the Repository

```bash
git clone https://github.com/omijh/Peanut-Picosystem.git
cd Peanut-Picosystem
```

### 3. Embed Your Game Boy / Game Boy Color ROM

```bash
python3 embed_rom.py <your_game.gb_or_gbc>
```
*(Generates `rom.h` and updates `main.cpp`'s timestamp for recompilation)*

### 4. Build the Firmware

```bash
./build.sh
```

The compiled UF2 file will be at:
```text
build/peanut_picosystem.uf2
```

### 5. Flash to Your PicoSystem

1. Turn off your PicoSystem.
2. Hold down **X** while flipping the power switch on.
3. The device mounts as a USB drive named **`RPI-RP2`**.
4. Drag and drop `build/peanut_picosystem.uf2` onto `RPI-RP2`.

---

## Project Structure

```text
├── main.cpp                    # Walnut-CGB glue, GBC color filters, blended scaler, ST7789 60 Hz setup, A/B save engine
├── gb_audio.hpp                # 4-channel DMG APU, 200 Hz HPF + soft-knee limiter, 12-bit DMA PWM & piezo beeper driver
├── walnut_cgb.h                # MIT-licensed Game Boy & Game Boy Color emulator core (Walnut-CGB)
├── dmg-acid2.gb.h              # Fallback graphical test ROM (by Matt Currie)
├── embed_rom.py                # Python utility to convert .gb/.gbc ROMs into rom.h
├── build.sh                    # One-click build script
├── CMakeLists.txt              # CMake build configuration (PICO_BOARD=pimoroni_picosystem)
├── pico_sdk_import.cmake       # Raspberry Pi Pico SDK loader
├── picosystem_sdk_import.cmake # Pimoroni PicoSystem SDK loader
├── AGENTS.md                   # Technical architecture & constraints for AI coding agents
└── README.md                   # Project documentation
```

---

## Credits & License

- **Walnut-CGB / Peanut-GB**: Walnut-CGB by Mr. Paul, Peanut-GB by [Mahyar Koshkouei](https://github.com/deltabeard/Peanut-GB), with palette components from [SameBoy](https://github.com/LIJI32/SameBoy) (MIT License).
- **PicoCrystal-GBC**: [Wyatt-Grant/PicoCrystal-GBC](https://github.com/Wyatt-Grant/PicoCrystal-GBC) — inspiration & techniques for 12-bit DMA piezo audio (200 Hz HPF + soft-knee limiter + 12 mA drive), ST7789 60 Hz TE unlock, matrix/curve GBC color filters, `blend_avg` scaling, power-loss-safe A/B flash slots, and RP2040 SRAM placement.
- **PicoSystem**: Hardware and SDK by [Pimoroni](https://github.com/pimoroni/picosystem).
- **dmg-acid2**: Hardware test ROM by [Matt Currie](https://github.com/mattcurrie/dmg-acid2).
