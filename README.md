# Peanut-GB for Pimoroni PicoSystem

A fast, lightweight Game Boy (DMG) emulator ported to the **Pimoroni PicoSystem** (RP2040) powered by **Peanut-GB**.

Features **edge-to-edge fullscreen 240×240 scaling**, **full Save State persistence with auto-resume**, multiple color palettes, and smooth hardware-synced performance.

---

## Highlights

- **Full Save State & Auto-Resume**:
  - Save your game at **any exact moment** (CPU registers, RAM, VRAM, and cartridge state).
  - Perfect for games without in-game save menus (action platformers, arcade games, homebrews like *Karpe Diem*).
  - **Auto-Resume on Startup**: When you power on your PicoSystem, it automatically resumes right from your last save state!
  - Manual quick-load also supported (instantly reload a tricky section without restarting).
- **Edge-to-Edge 240×240 Display**:
  - Scales Game Boy graphics to take up the **entire 240×240 IPS display** with zero black borders by default!
  - 3 selectable display modes: Fullscreen (240×240), Aspect Fit (240×216), and Pixel-Perfect (160×144).
- **Persistent Non-Volatile Storage**:
  - Saves are streamed in 256-byte pages directly to the 16 MB onboard Flash (at offset 15 MB).
  - Survives power-off, reboots, and dead batteries.
- **4-Channel Game Boy Audio & Piezo Beeper Mode**:
  - Defaults to **Piezo Beeper Mode**, converting live APU melodies, SFX sweeps, and percussion triggers into crisp direct-frequency piezo tones.
  - Switch anytime to **4-CH PWM** for polyphonic 4-channel DMG synthesis (Pulse 1 + Sweep, Pulse 2, Wave RAM, LFSR Noise mixed at 32,768 Hz over a 61 kHz ultrasonic PWM carrier with soft-clipped volume boost) or **Muted**.
- **Quick Settings Menu (`X + Y + LEFT`)**:
  - Pauses emulation to adjust **Master Volume (0%–100%)**, **Screen Brightness (10%–100%)**, **Audio Mode**, **Color Palette**, and **Display Mode** with D-Pad sliders.
  - Both Volume and Brightness start low at **30%** on boot for comfortable eyes and ears.
- **3 Color Palettes**:
  - Classic DMG Olive Green, Pocket B&W (Grayscale), and Game Boy Light (Teal backlight).
- **Zero External Storage Needed**:
  - ROMs are embedded directly into Flash via `embed_rom.py`.

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
| **Quick Save State** | Hold **X + Y** (~1.0s) | Keep holding while the `"HOLD TO SAVE..."` bar fills until the green LED blinks and `"STATE SAVED!"` appears. Also saves your current settings (Volume, Brightness, Audio, Palette, Display). |
| **Quick Load State** | Hold **A + B** (~1.0s, without D-Pad) | Keep holding while the `"HOLD TO LOAD..."` bar fills until `"STATE LOADED!"` appears (restores gameplay and saved settings). |
| **Auto-Resume on Boot** | *Power ON normally* | Automatically restores your last saved game state and settings, displaying `"RESUMED!"`. |
| **Fresh Boot (Skip Save)** | Hold **Y** while powering ON | Skips the saved state and boots cleanly from the game's title screen. |

> [!TIP]
> Games that save to cartridge battery RAM (such as Pokémon) also auto-save to Flash ~2.5 seconds after writing save data.

### Quick Settings Menu (`X + Y + LEFT`)
1. **Open**: Hold **X + Y** (right thumb) and tap **LEFT** (left thumb). Emulation and audio pause automatically.
2. **Navigate**: Press **UP / DOWN** to select a row, and **LEFT / RIGHT** to adjust values (hold **LEFT** or **RIGHT** to slide Volume/Brightness smoothly).
3. **Resume Game**: Press **B**, **X**, or **X + Y + LEFT** to return to gameplay.

| Setting | Range / Options | Default | Notes |
| :--- | :--- | :--- | :--- |
| **VOLUME** | `0%` – `100%` (10% steps) | **30%** | Master speaker level (`0%` silences output cleanly). |
| **BRIGHT** | `10%` – `100%` (10% steps) | **30%** | Screen backlight level (clamped at `10%` minimum so the menu stays visible). |
| **AUDIO** | `4-CH PWM` ⇄ `PIEZO BEEP` ⇄ `MUTED` | **PIEZO BEEP** | Switch between direct piezo tones (default), 61 kHz ultrasonic polyphonic PWM audio, or mute. |
| **PALETTE** | `DMG GREEN` ⇄ `POCKET B&W` ⇄ `TEAL LIGHT` | **TEAL LIGHT** | Game Boy Light teal (default), classic olive green, or monochrome Pocket grayscale. |
| **DISPLAY** | `FULLSCREEN` ⇄ `ASPECT FIT` ⇄ `1:1 CENTER` | **FULLSCREEN** | Edge-to-edge 240×240, 10:9 letterboxed 240×216, or crisp 160×144 bezel. |

> [!TIP]
> Settings adjusted in the Quick Settings Menu or via hotkeys are saved to Flash whenever you Quick Save with **X + Y** and automatically restored on Quick Load and boot.

### Quick Shortcuts (No Menu Needed)
Hold **X + Y** and tap a D-Pad direction to toggle modes mid-game without pausing:

| Action | Button Combination | Description |
| :--- | :--- | :--- |
| **Cycle Palette** | Hold **X + Y** + tap **UP** | Cycles `DMG GREEN` ⇄ `POCKET B&W` ⇄ `TEAL LIGHT` |
| **Cycle Display Mode** | Hold **X + Y** + tap **DOWN** | Cycles `FULLSCREEN` (240×240) ⇄ `ASPECT FIT` (240×216) ⇄ `1:1 CENTER` (160×144) |
| **Cycle Audio Mode** | Hold **X + Y** + tap **RIGHT** | Cycles `4-CH PWM` (full polyphonic music) ⇄ `PIEZO BEEP` (louder classic piezo tones) ⇄ `MUTED` |
| **Open Settings Menu** | Hold **X + Y** + tap **LEFT** | Opens the paused **Quick Settings Menu** for Volume, Brightness, Audio, Palette, & Display |

---

## Quick Start Guide

### 1. Prerequisites (macOS / Linux)

Ensure you have CMake and the ARM cross-compiler installed:

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

### 3. Load Your Own Game Boy ROM

1. Place your `.gb` file in the project root:
   ```bash
   python3 embed_rom.py <your_game>.gb
   ```
   *(This generates `rom.h` and automatically marks `main.cpp` for recompilation)*

### 4. Build the Firmware

Run the build script:
```bash
./build.sh
```

The compiled UF2 file will be located at:
```text
build/peanut_picosystem.uf2
```

### 5. Flash to Your PicoSystem

1. Turn off your PicoSystem.
2. Hold down the **X** button while flipping the power switch on.
3. The device will mount as a USB mass-storage drive named **`RPI-RP2`**.
4. Drag and drop `build/peanut_picosystem.uf2` onto the `RPI-RP2` drive.
5. The device will automatically flash and launch your game!

---

## How the Save State System Works

Instead of relying solely on cartridge battery saves (which many arcade and homebrew games lack), the emulator serializes a complete snapshot of the system:

1. **State Payload**: Includes `struct gb_s` (CPU registers, PC, SP, timers, flags, 8 KB WRAM, 8 KB VRAM, OAM, HRAM) and the 32 KB Cartridge RAM (~50 KB total).
2. **Flash Streaming**: Writes data in 256-byte hardware pages to a dedicated 64 KB sector at offset 15 MB in Flash, protected by a magic header (`STAT`) and a 32-bit checksum.
3. **Auto-Resume**: On boot, `init()` checks for a valid state in Flash. If verified, it restores all memory and registers, re-hooks the native C++ display/read callbacks, and seamlessly resumes gameplay.

---

## Project Structure

```text
├── main.cpp                  # Emulator glue code, scaling renderer, inputs, Save State engine
├── gb_audio.hpp              # 4-channel Game Boy DMG APU & PicoSystem PWM/piezo audio driver
├── peanut_gb.h               # Single-file Game Boy emulator core (by Mahyar Koshkouei)
├── dmg-acid2.gb.h            # Default graphical test ROM (by Matt Currie)
├── embed_rom.py              # Python utility to convert .gb ROMs into rom.h
├── build.sh                  # One-click build script
├── CMakeLists.txt            # CMake build configuration
├── pico_sdk_import.cmake     # Raspberry Pi Pico SDK loader
├── picosystem_sdk_import.cmake # Pimoroni PicoSystem SDK loader
├── .gitignore                # Protects against committing build artifacts & ROMs
├── AGENTS.md                 # Technical architecture & instructions for AI agents
└── README.md                 # Project documentation
```

---

## Credits & License

- **Peanut-GB**: Developed by [Mahyar Koshkouei](https://github.com/deltabeard/Peanut-GB) (MIT License).
- **PicoSystem**: Hardware and SDK by [Pimoroni](https://github.com/pimoroni/picosystem).
- **dmg-acid2**: Hardware test ROM by [Matt Currie](https://github.com/mattcurrie/dmg-acid2).
