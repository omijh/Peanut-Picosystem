#!/usr/bin/env python3
import os
import sys

MIN_GB_ROM_BYTES = 0x150
MAX_GB_ROM_BYTES = 12 * 1024 * 1024


def read_rom_mode(data: bytes) -> str:
    cgb_flag = data[0x0143]
    if cgb_flag == 0xC0:
        return "GBC"
    if (cgb_flag & 0x80) != 0:
        return "GBC+DMG"
    return "DMG"


def read_rom_title(data: bytes) -> str:
    end = 0x0143 if (data[0x0143] & 0x80) != 0 else 0x0144
    raw = data[0x0134:end]
    chars = []
    for b in raw:
        if b == 0:
            break
        if 32 <= b <= 126:
            chars.append(chr(b))
    return "".join(chars).strip()


def main():
    if len(sys.argv) < 2:
        print("Usage: python3 embed_rom.py <path_to_game.gb|.gbc> [output_file.h]")
        sys.exit(1)

    script_dir = os.path.dirname(os.path.abspath(__file__))
    rom_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else os.path.join(script_dir, "rom.h")

    if not os.path.exists(rom_path):
        print(f"Error: File '{rom_path}' not found!", file=sys.stderr)
        sys.exit(1)

    with open(rom_path, "rb") as f:
        data = f.read()

    size = len(data)
    if size < MIN_GB_ROM_BYTES:
        print(
            f"Error: '{rom_path}' is too small ({size} bytes; minimum valid GB/GBC ROM header is {MIN_GB_ROM_BYTES} bytes).",
            file=sys.stderr,
        )
        sys.exit(1)
    if size > MAX_GB_ROM_BYTES:
        print(
            f"Error: '{rom_path}' is too large ({size / (1024 * 1024):.2f} MB; max supported is {MAX_GB_ROM_BYTES // (1024 * 1024)} MB).",
            file=sys.stderr,
        )
        sys.exit(1)

    title = read_rom_title(data)
    mode = read_rom_mode(data)
    meta = f" [{title} | {mode}]" if title else f" [{mode}]"
    print(f"Embedding '{rom_path}'{meta} ({size} bytes, {size / 1024:.1f} KB) -> '{out_path}'...")

    with open(out_path, "w") as out:
        out.write("/* Auto-generated ROM file for Walnut-CGB PicoSystem */\n")
        out.write("#pragma once\n\n")
        out.write(f"// Original file: {os.path.basename(rom_path)}\n")
        if title:
            out.write(f"// Title: {title}\n")
        out.write(f"// Mode: {mode}\n")
        out.write(f"// Size: {size} bytes\n")
        out.write(f"const unsigned int custom_rom_len = {size};\n")
        out.write("const unsigned char custom_rom_data[] = {\n")

        for offset in range(0, size, 16):
            chunk = data[offset:offset + 16]
            line = ", ".join(f"0x{b:02x}" for b in chunk)
            suffix = "," if offset + 16 < size else ""
            out.write(f"  {line}{suffix}\n")

        out.write("};\n")

    # Touch main.cpp so build systems immediately trigger a recompile of main.cpp
    main_cpp = os.path.join(script_dir, "main.cpp")
    if os.path.exists(main_cpp):
        os.utime(main_cpp, None)

    print("Done! Updated main.cpp timestamp. Run ./build.sh to compile.")


if __name__ == "__main__":
    main()
