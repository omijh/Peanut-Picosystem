# Peanut-GB for Pimoroni PicoSystem

A fast, lightweight Game Boy (DMG) emulator ported to the **Pimoroni PicoSystem** (RP2040) powered by **Peanut-GB**.

Features **edge-to-edge fullscreen 240×240 scaling**, **full Save State persistence with auto-resume**, multiple color palettes, and locked 60 FPS performance.

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
- **3 Color Palettes**:
  - Classic DMG Olive Green, Pocket B&W (Grayscale), and Game Boy Light (Teal backlight).
- **Zero External Storage Needed**:
  - ROMs are embedded directly into Flash via `embed_rom.py`.

---

## Controls & Hotkeys

| Action | Button Combination | Description |
| :--- | :--- | :--- |
| **D-Pad** | **D-Pad** | Directional movement |
| **A / B** | **A / B** | Primary / Secondary actions |
| **START** | **X** | Start / In-game pause |
| **SELECT** | **Y** | Select |
| **Quick Save State** | Hold **X + Y** (1.0s) | Saves complete snapshot to Flash. Green LED blinks & shows `"STATE SAVED!"` |
| **Quick Load State** | Hold **A + B** (1.0s) | Immediately reloads saved snapshot. Shows `"STATE LOADED!"` |
| **Auto-Resume on Boot** | *Turn power ON* | Automatically restores and continues from your last save state! |
| **Fresh Boot (Skip State)** | Hold **Y** on power-up | Skips saved state and boots from the game's title screen |
| **Cycle Palette** | Hold **X + Y** and tap **UP** | Cycles Classic DMG ⇄ Pocket Grayscale ⇄ Teal Light |
| **Cycle Display Mode** | Hold **X + Y** and tap **DOWN** | Cycles Fullscreen (240×240) ⇄ Aspect-Fit (240×216) ⇄ 1:1 Centered |

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
