#!/usr/bin/env bash
#!/usr/bin/env python3
import sys
import os
import time

def main():
    if len(sys.argv) < 2:
        print("Usage: python3 embed_rom.py <path_to_game.gb> [output_file.h]")
        sys.exit(1)

    rom_path = sys.argv[1]
    out_path = sys.argv[2] if len(sys.argv) > 2 else "rom.h"

    if not os.path.exists(rom_path):
        print(f"Error: File '{rom_path}' not found!")
        sys.exit(1)

    with open(rom_path, "rb") as f:
        data = f.read()

    size = len(data)
    print(f"Embedding '{rom_path}' ({size} bytes, {size / 1024:.1f} KB) -> '{out_path}'...")

    with open(out_path, "w") as out:
        out.write("/* Auto-generated ROM file for Peanut-GB PicoSystem */\n")
        out.write("#pragma once\n\n")
        out.write(f"// Original file: {os.path.basename(rom_path)}\n")
        out.write(f"// Size: {size} bytes\n")
        out.write(f"const unsigned int custom_rom_len = {size};\n")
        out.write("const unsigned char custom_rom_data[] = {\n  ")

        for i, byte in enumerate(data):
            out.write(f"0x{byte:02x}, ")
            if (i + 1) % 16 == 0 and i + 1 < size:
                out.write("\n  ")

        out.write("\n};\n")

    # Touch main.cpp so build systems immediately trigger a recompile of main.cpp
    script_dir = os.path.dirname(os.path.abspath(__file__))
    main_cpp = os.path.join(script_dir, "main.cpp")
    if os.path.exists(main_cpp):
        os.utime(main_cpp, None)

    print("Done! Updated main.cpp timestamp. Run ./build.sh to compile.")

if __name__ == "__main__":
    main()
