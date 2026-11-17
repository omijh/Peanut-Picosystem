#!/usr/bin/env bash
set -e

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# Detect local or environment toolchain and SDK paths
if [ -d "$SCRIPT_DIR/bin" ]; then
  export PATH="$SCRIPT_DIR/bin:$PATH"
fi

if [ -z "$PICO_SDK_PATH" ]; then
  if [ -d "$SCRIPT_DIR/pico-sdk" ]; then
    export PICO_SDK_PATH="$SCRIPT_DIR/pico-sdk"
  else
    echo "Warning: PICO_SDK_PATH not set. CMake will attempt to fetch or find it."
  fi
fi

if [ -z "$PICO_TOOLCHAIN_PATH" ] && [ -d "$SCRIPT_DIR/toolchain" ]; then
  export PICO_TOOLCHAIN_PATH="$SCRIPT_DIR/toolchain"
fi

CMAKE_EXTRA_ARGS=()
if [ -d "$SCRIPT_DIR/pioasm-install/lib/cmake/pioasm" ]; then
  CMAKE_EXTRA_ARGS+=("-Dpioasm_DIR=$SCRIPT_DIR/pioasm-install/lib/cmake/pioasm")
fi

if [ -d "$SCRIPT_DIR/picosystem" ]; then
  CMAKE_EXTRA_ARGS+=("-DPICOSYSTEM_DIR=$SCRIPT_DIR/picosystem")
fi

# Ensure main.cpp is rebuilt if rom.h is newer than the built UF2
if [ -f "$SCRIPT_DIR/rom.h" ] && [ -f "$SCRIPT_DIR/build/peanut_picosystem.uf2" ]; then
  if [ "$SCRIPT_DIR/rom.h" -nt "$SCRIPT_DIR/build/peanut_picosystem.uf2" ]; then
    touch "$SCRIPT_DIR/main.cpp"
  fi
fi

mkdir -p "$SCRIPT_DIR/build"
cd "$SCRIPT_DIR/build"

cmake .. "${CMAKE_EXTRA_ARGS[@]}"

NUM_JOBS=$(sysctl -n hw.ncpu 2>/dev/null || nproc 2>/dev/null || echo 4)
make -j"$NUM_JOBS"

echo ""
echo "=========================================================="
echo "SUCCESS! Firmware built:"
echo "$SCRIPT_DIR/build/peanut_picosystem.uf2"
echo "=========================================================="
echo "To flash to your PicoSystem:"
echo "1. Hold the 'X' button and turn on the power switch."
echo "2. Drag and drop 'peanut_picosystem.uf2' onto the RPI-RP2 drive."
