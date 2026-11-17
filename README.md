# Peanut-GB for Pimoroni PicoSystem

A fast, lightweight Game Boy (DMG) emulator ported to the **Pimoroni PicoSystem** (RP2040) powered by **Peanut-GB**.

Features **edge-to-edge fullscreen 240×240 scaling**, **persistent Flash battery saves**, multiple color palettes, and full 60 FPS performance.

---

## Highlights

- **Edge-to-Edge 240×240 Display**:
  - Scales Game Boy graphics to take up the **entire 240×240 IPS display** with zero black borders by default!
  - Supports 3 selectable display modes:
    1. **Fullscreen (240×240)**: Fills the entire screen edge-to-edge.
    2. **Aspect Fit (240×216)**: Full horizontal width preserving exact 10:9 ratio with tiny 12px top/bottom letterbox.
    3. **Pixel-Perfect (160×144)**: Centered 1:1 original resolution with retro bezel.
- **Persistent Flash Battery Saves**: 
  - Supports MBC1, MBC2, and MBC3 cartridge save RAM (up to 32 KB, fully supporting *Pokémon Red, Blue, Yellow, Gold, Silver*).
  - Automatically commits battery saves to onboard non-volatile QSPI Flash memory (at the 15 MB sector mark).
  - Data survives power-off, resets, and dead battery.
  - On-screen `"SAVED!"` banner and green LED indicator when saving.
- **3 Color Palettes**:
  - Classic DMG Olive Green
  - Pocket B&W (Crisp grayscale)
  - Game Boy Light (Teal backlight)
- **Zero External Storage Needed**:
  - ROMs are embedded directly into the 16 MB onboard Flash via `embed_rom.py`.
- **Tested Out-of-the-Box**:
  - Includes Matt Currie's renowned `dmg-acid2` graphical test ROM by default so you can build and flash right away.

---

## Controls

| Game Boy Button | PicoSystem Button | Action |
| :--- | :--- | :--- |
| **D-Pad** | **D-Pad** | Directional controls |
| **A** | **A** | Primary action |
| **B** | **B** | Secondary action / Cancel |
| **START** | **X** | Start / In-game menu |
| **SELECT** | **Y** | Select |
| **Force Save to Flash** | Hold **X + Y** (1.5s) | Manually commits SRAM save data to Flash |
| **Cycle Palette** | Hold **X + Y** and tap **UP** | Cycles through DMG, Pocket, and Teal palettes |
| **Cycle Display Mode** | Hold **X + Y** and tap **DOWN** | Cycles Fullscreen (240×240) ⇄ Aspect-Fit ⇄ 1:1 Centered |

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

*(If you didn't download the Pico SDK or PicoSystem SDK into the repository root, set the environment variables: `export PICO_SDK_PATH=/path/to/pico-sdk` and `export PICOSYSTEM_DIR=/path/to/picosystem`)*

### 3. Load Your Own Game Boy ROM (e.g. Pokémon)

1. Place your `.gb` file in the project root:
   ```bash
   python3 embed_rom.py <your_game>.gb
   ```
   *(This generates `rom.h`, automatically updating `main.cpp` for recompilation)*

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
5. The device will automatically flash and launch your game in edge-to-edge fullscreen!

---

## How the Save System Works

Game Boy cartridges (like Pokémon) store save games in Cartridge RAM (`cart_ram`). Because the RP2040's SRAM is volatile, the emulator mirrors this memory to the 16 MB QSPI Flash:

1. **Auto-Save Detection**: Whenever the game writes to Cartridge RAM to save (e.g., saving through Pokémon's in-game START menu), a dirty flag is set. Once writes settle for 2.5 seconds, the emulator writes the 32 KB block into the 15 MB Flash sector.
2. **Manual Save Combo**: You can hold **X + Y** for 1.5 seconds at any time to force an immediate Flash write.
3. **Boot-Up Restore**: On startup, `init()` reads the 15 MB Flash sector, validates a magic header (`PGBS`) and checksum, and restores the save data back into memory.

---

## Project Structure

```text
├── main.cpp                  # Emulator glue code, scaling renderer, inputs, Flash saves
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
