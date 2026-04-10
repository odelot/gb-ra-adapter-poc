# GB crc-md5-mapper.py
#
# Generates a CRC32→MD5 lookup table for Game Boy ROMs.
#
# For each ROM the script:
#   - Computes CRC32 of the first 512 bytes (at offset 0x0000, no header to skip).
#   - Computes MD5 of the full ROM content (rcheevos GB hashing = plain MD5 of entire file).
#
# Output format (one line per game):
#   XXXXXXXX=<md5>
#
# Usage:
#   pip install py7zr
#   python crc-md5-mapper.py > ../../gb-esp-firmware/data/games.txt
#
# The games.txt is loaded into the ESP32's LittleFS and used to resolve the
# CRC32 fingerprint sent by the Pico (CART_CRC=XXXXXXXX) into an RA MD5 hash.
#
# Romset: No-Intro "Nintendo - Game Boy"
#   Place all .7z files (or .gb / .gbc files) under ./games/

import hashlib
import binascii
import os
import sys
import tempfile

try:
    import py7zr
except ImportError:
    print("ERROR: py7zr not installed. Run: pip install py7zr", file=sys.stderr)
    sys.exit(1)


def read_rom_bytes(file_path):
    """Return the raw bytes of a ROM, extracting from .7z if necessary."""
    ext = os.path.splitext(file_path)[1].lower()
    if ext == '.7z':
        with py7zr.SevenZipFile(file_path, mode='r') as z:
            names = z.getnames()
            # Pick the first .gb or .gbc file inside the archive
            rom_name = next(
                (n for n in names if os.path.splitext(n)[1].lower() in ('.gb', '.gbc')),
                None
            )
            if rom_name is None:
                return None
            with tempfile.TemporaryDirectory() as tmpdir:
                z.extract(path=tmpdir, targets=[rom_name])
                extracted = os.path.join(tmpdir, rom_name)
                with open(extracted, 'rb') as f:
                    return f.read()
    elif ext in ('.gb', '.gbc'):
        with open(file_path, 'rb') as f:
            return f.read()
    return None


def calculate_md5(rom_bytes):
    """MD5 of the full ROM (rcheevos GB hashing = plain MD5 of entire file)."""
    return hashlib.md5(rom_bytes).hexdigest()


def calculate_crc32_first_512(rom_bytes):
    """CRC32 of the first 512 bytes."""
    sample = rom_bytes[:512]
    crc = binascii.crc32(sample) & 0xFFFFFFFF
    return format(crc, '08X')


def list_files(root_dir):
    paths = []
    for dirpath, _, filenames in os.walk(root_dir):
        for filename in filenames:
            paths.append(os.path.join(dirpath, filename))
    return paths


def region_priority(path):
    """Lower number = higher priority in collision resolution."""
    p = path.replace('\\', '/')
    if '(World)' in p:
        return 0
    if '(USA, Europe)' in p or '(USA, Australia)' in p:
        return 1
    if '(USA)' in p:
        return 2
    if '(Europe)' in p:
        return 3
    if '(Japan)' in p:
        return 4
    return 5


def is_unwanted(path):
    """Skip beta, proto, pirate, unlicensed, and BIOS files."""
    p = path.replace('\\', '/')
    keywords = ['(Beta', '(Proto', '(Pirate', '(Unl)', '(Unl,', '[BIOS]', '(Sample)']
    return any(k in p for k in keywords)


# ── main ────────────────────────────────────────────────────────────────────

root_games_dir = './games'
output_file    = './games.txt'   # written directly to avoid PowerShell stdout encoding issues

game_list = list_files(root_games_dir)

# Keep only .7z / .gb / .gbc files
game_list = [f for f in game_list
             if os.path.splitext(f)[1].lower() in ('.7z', '.gb', '.gbc')]

# Remove unwanted variants
game_list = [f for f in game_list if not is_unwanted(f)]

# Sort by region priority (best region first), then alphabetically
game_list.sort(key=lambda x: (region_priority(x), x.lower()))

seen_crcs = set()   # collision detection: first canonical version wins

written = 0
with open(output_file, 'w', encoding='ascii', newline='\n') as out:
    for game_path in game_list:
        rom_bytes = read_rom_bytes(game_path)
        if rom_bytes is None or len(rom_bytes) < 512:
            print(f"SKIP (too small or unreadable): {game_path}", file=sys.stderr)
            continue

        crc32 = calculate_crc32_first_512(rom_bytes)
        md5   = calculate_md5(rom_bytes)

        if crc32 in seen_crcs:
            continue  # keep first (highest-priority) version
        seen_crcs.add(crc32)

        out.write(f"{crc32}={md5}\n")
        written += 1

print(f"Done: {written} entries written to {output_file}", file=sys.stderr)
