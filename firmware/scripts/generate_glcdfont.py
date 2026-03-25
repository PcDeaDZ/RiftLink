# -*- coding: utf-8 -*-
"""
Generate improved glcdfont.c with better Cyrillic glyphs.

Font format: 5 columns x 8 rows per glyph, column-major.
Each byte = 1 column, bit 0 = top row, bit 7 = bottom row.

Layout in font array (each char = 5 bytes):
  0-31:   control chars (symbols/icons)
  32-127: standard ASCII
  128-143: lowercase р-я  (16 chars)
  144-175: uppercase А-Я  (32 chars)
  176:     empty (padding)
  177-192: lowercase а-п  (16 chars)
  193:     Ё (uppercase)
  194:     ё (lowercase)
"""
import sys, io
sys.stdout = io.TextIOWrapper(sys.stdout.buffer, encoding='utf-8', errors='replace')

def glyph_from_grid(grid_str):
    """Convert a visual grid to 5 column bytes.
    Grid: 5 chars wide, up to 8 rows. '#'=pixel on, ' '/'.'=off.
    """
    rows = []
    for line in grid_str.strip().split('\n'):
        line = line.strip()
        if not line:
            continue
        row = []
        for ch in line[:5]:
            row.append(1 if ch == '#' else 0)
        while len(row) < 5:
            row.append(0)
        rows.append(row)
    while len(rows) < 8:
        rows.append([0]*5)

    cols = [0]*5
    for col in range(5):
        val = 0
        for row in range(8):
            if rows[row][col]:
                val |= (1 << row)
        cols[col] = val
    return cols

# ============================================================
# UPPERCASE А-Я (32 chars, positions 144-175)
# ============================================================

upper_glyphs = {}

upper_glyphs['A'] = """
.####
#...#
#...#
#####
#...#
#...#
#...#
.....
"""

upper_glyphs['B'] = """
#####
#....
#....
####.
#...#
#...#
####.
.....
"""

upper_glyphs['V'] = """
####.
#...#
#...#
####.
#...#
#...#
####.
.....
"""

upper_glyphs['G'] = """
#####
#....
#....
#....
#....
#....
#....
.....
"""

upper_glyphs['D'] = """
.####
.#.#.
.#.#.
.#.#.
.#.#.
#...#
#####
#...#
"""

upper_glyphs['E'] = """
#####
#....
#....
###..
#....
#....
#####
.....
"""

upper_glyphs['Zh'] = """
#.#.#
#.#.#
.###.
..#..
.###.
#.#.#
#.#.#
.....
"""

upper_glyphs['Z'] = """
.###.
#...#
....#
..##.
....#
#...#
.###.
.....
"""

upper_glyphs['I'] = """
#...#
#..##
#.#.#
#.#.#
##..#
#...#
#...#
.....
"""

upper_glyphs['J'] = """
..#..
#...#
#..##
#.#.#
##..#
#...#
#...#
.....
"""

upper_glyphs['K'] = """
#...#
#..#.
#.#..
##...
#.#..
#..#.
#...#
.....
"""

upper_glyphs['L'] = """
..###
..#.#
..#.#
..#.#
..#.#
.#..#
#...#
.....
"""

upper_glyphs['M'] = """
#...#
##.##
#.#.#
#.#.#
#...#
#...#
#...#
.....
"""

upper_glyphs['N'] = """
#...#
#...#
#...#
#####
#...#
#...#
#...#
.....
"""

upper_glyphs['O'] = """
.###.
#...#
#...#
#...#
#...#
#...#
.###.
.....
"""

upper_glyphs['P'] = """
#####
#...#
#...#
#...#
#...#
#...#
#...#
.....
"""

upper_glyphs['R'] = """
####.
#...#
#...#
####.
#....
#....
#....
.....
"""

upper_glyphs['S'] = """
.###.
#...#
#....
#....
#....
#...#
.###.
.....
"""

upper_glyphs['T'] = """
#####
..#..
..#..
..#..
..#..
..#..
..#..
.....
"""

upper_glyphs['U'] = """
#...#
#...#
#...#
.####
....#
....#
.###.
.....
"""

upper_glyphs['F'] = """
.###.
#.#.#
#.#.#
#.#.#
.###.
..#..
..#..
.....
"""

upper_glyphs['Kh'] = """
#...#
#...#
.#.#.
..#..
.#.#.
#...#
#...#
.....
"""

upper_glyphs['Ts'] = """
#...#
#...#
#...#
#...#
#...#
#...#
#####
....#
"""

upper_glyphs['Ch'] = """
#...#
#...#
#...#
.####
....#
....#
....#
.....
"""

upper_glyphs['Sh'] = """
#...#
#...#
#.#.#
#.#.#
#.#.#
#.#.#
#####
.....
"""

upper_glyphs['Shch'] = """
#...#
#...#
#.#.#
#.#.#
#.#.#
#.#.#
#####
....#
"""

upper_glyphs['Hard'] = """
##...
.#...
.#...
.###.
.#..#
.#..#
.###.
.....
"""

upper_glyphs['Y'] = """
#...#
#...#
#...#
##..#
#.#.#
#.#.#
##..#
.....
"""

upper_glyphs['Soft'] = """
#....
#....
#....
###..
#..#.
#..#.
###..
.....
"""

upper_glyphs['Eh'] = """
.###.
#...#
....#
.####
....#
#...#
.###.
.....
"""

upper_glyphs['Yu'] = """
#.##.
#.#.#
#.#.#
###.#
#.#.#
#.#.#
#.##.
.....
"""

upper_glyphs['Ya'] = """
.####
#...#
#...#
.####
..#.#
.#..#
#...#
.....
"""

# ============================================================
# LOWERCASE а-п (16 chars, positions 177-192)
# ============================================================

lower_ap_glyphs = {}

lower_ap_glyphs['a'] = """
.....
.....
.###.
....#
.####
#...#
.####
.....
"""

lower_ap_glyphs['b'] = """
..###
.#...
#....
####.
#...#
#...#
.###.
.....
"""

lower_ap_glyphs['v'] = """
.....
.....
####.
#...#
####.
#...#
####.
.....
"""

lower_ap_glyphs['g'] = """
.....
.....
#####
#....
#....
#....
#....
.....
"""

lower_ap_glyphs['d'] = """
.....
.....
.###.
.#.#.
.#.#.
#...#
#####
#...#
"""

lower_ap_glyphs['e'] = """
.....
.....
.###.
#...#
#####
#....
.###.
.....
"""

lower_ap_glyphs['zh'] = """
.....
.....
#.#.#
#.#.#
.###.
#.#.#
#.#.#
.....
"""

lower_ap_glyphs['z'] = """
.....
.....
.###.
....#
..##.
....#
.###.
.....
"""

lower_ap_glyphs['i'] = """
.....
.....
#..##
#.#.#
#.#.#
##..#
#...#
.....
"""

lower_ap_glyphs['j'] = """
.#.#.
.....
#..##
#.#.#
#.#.#
##..#
#...#
.....
"""

lower_ap_glyphs['k'] = """
.....
.....
#..#.
#.#..
##...
#.#..
#..#.
.....
"""

lower_ap_glyphs['l'] = """
.....
.....
..###
..#.#
..#.#
.#..#
#...#
.....
"""

lower_ap_glyphs['m'] = """
.....
.....
#...#
##.##
#.#.#
#...#
#...#
.....
"""

lower_ap_glyphs['n'] = """
.....
.....
#...#
#...#
#####
#...#
#...#
.....
"""

lower_ap_glyphs['o'] = """
.....
.....
.###.
#...#
#...#
#...#
.###.
.....
"""

lower_ap_glyphs['p'] = """
.....
.....
#####
#...#
#...#
#...#
#...#
.....
"""

# ============================================================
# LOWERCASE р-я (16 chars, positions 128-143)
# ============================================================

lower_rya_glyphs = {}

lower_rya_glyphs['r'] = """
.....
.....
####.
#...#
#...#
####.
#....
#....
"""

lower_rya_glyphs['s'] = """
.....
.....
.###.
#...#
#....
#...#
.###.
.....
"""

lower_rya_glyphs['t'] = """
.....
.....
#####
..#..
..#..
..#..
..#..
.....
"""

lower_rya_glyphs['u'] = """
.....
.....
#...#
#...#
#...#
.####
....#
.###.
"""

lower_rya_glyphs['f'] = """
.....
.....
.###.
#.#.#
#.#.#
.###.
..#..
..#..
"""

lower_rya_glyphs['kh'] = """
.....
.....
#...#
.#.#.
..#..
.#.#.
#...#
.....
"""

lower_rya_glyphs['ts'] = """
.....
.....
#..#.
#..#.
#..#.
#..#.
#####
....#
"""

lower_rya_glyphs['ch'] = """
.....
.....
#...#
#...#
.####
....#
....#
.....
"""

lower_rya_glyphs['sh'] = """
.....
.....
#...#
#.#.#
#.#.#
#.#.#
#####
.....
"""

lower_rya_glyphs['shch'] = """
.....
.....
#...#
#.#.#
#.#.#
#.#.#
#####
....#
"""

lower_rya_glyphs['hard'] = """
.....
.....
##...
.###.
.#..#
.#..#
.###.
.....
"""

lower_rya_glyphs['y'] = """
.....
.....
#...#
##..#
#.#.#
#.#.#
##..#
.....
"""

lower_rya_glyphs['soft'] = """
.....
.....
#....
###..
#..#.
#..#.
###..
.....
"""

lower_rya_glyphs['eh'] = """
.....
.....
.###.
....#
.####
....#
.###.
.....
"""

lower_rya_glyphs['yu'] = """
.....
.....
#.##.
#.#.#
###.#
#.#.#
#.##.
.....
"""

lower_rya_glyphs['ya'] = """
.....
.....
.####
#...#
.####
.#..#
#...#
.....
"""

# ============================================================
# Ё/ё (positions 193, 194)
# ============================================================

yo_upper = """
.#.#.
.....
#####
#....
###..
#....
#####
.....
"""

yo_lower = """
.#.#.
.....
.###.
#...#
#####
#....
.###.
.....
"""

# ============================================================
# Build the complete font array
# ============================================================

# Original ASCII part (positions 0-127) — keep from existing font
original_ascii = [
0, 0, 0, 0, 0, 62, 91, 79, 91, 62, 62, 107, 79, 107, 62, 28, 62, 124, 62, 28,
24, 60, 126, 60, 24, 28, 87, 125, 87, 28, 28, 94, 127, 94, 28, 24, 60, 24, 0, 0,
255, 231, 195, 231, 255, 24, 36, 24, 0, 0, 255, 231, 219, 231, 255, 48, 72, 58, 6, 14,
38, 41, 121, 41, 38, 64, 127, 5, 5, 7, 64, 127, 5, 37, 63, 90, 60, 231, 60, 90,
127, 62, 28, 28, 8, 8, 28, 28, 62, 127, 20, 34, 127, 34, 20, 95, 95, 0, 95, 95,
6, 9, 127, 1, 127, 102, 137, 149, 106, 0, 96, 96, 96, 96, 96, 148, 162, 255, 162, 148,
8, 4, 126, 4, 8, 16, 32, 126, 32, 16, 8, 8, 42, 28, 8, 8, 28, 42, 8, 8,
30, 16, 16, 16, 16, 12, 30, 12, 30, 12, 48, 56, 62, 56, 48, 6, 14, 62, 14, 6,
0, 0, 0, 0, 0, 95, 0, 0, 0, 0, 3, 0, 3, 0, 0, 20, 62, 20, 62, 20,
36, 106, 43, 18, 0, 99, 19, 8, 100, 99, 54, 73, 86, 32, 80, 3, 0, 0, 0, 0,
28, 34, 65, 0, 0, 65, 34, 28, 0, 0, 40, 24, 14, 24, 40, 8, 8, 62, 8, 8,
176, 112, 0, 0, 0, 8, 8, 8, 8, 0, 96, 96, 0, 0, 0, 96, 24, 6, 1, 0,
62, 65, 65, 62, 0, 66, 127, 64, 0, 0, 98, 81, 73, 70, 0, 34, 65, 73, 54, 0,
24, 20, 18, 127, 0, 39, 69, 69, 57, 0, 62, 73, 73, 48, 0, 97, 17, 9, 7, 0,
54, 73, 73, 54, 0, 6, 73, 73, 62, 0, 80, 0, 0, 0, 0, 128, 80, 0, 0, 0,
16, 40, 68, 0, 0, 20, 20, 20, 0, 0, 68, 40, 16, 0, 0, 2, 89, 9, 6, 0,
62, 73, 85, 93, 14, 126, 17, 17, 126, 0, 127, 73, 73, 54, 0, 62, 65, 65, 34, 0,
127, 65, 65, 62, 0, 127, 73, 73, 65, 0, 127, 9, 9, 1, 0, 62, 65, 73, 122, 0,
127, 8, 8, 127, 0, 65, 127, 65, 0, 0, 48, 64, 65, 63, 0, 127, 8, 20, 99, 0,
127, 64, 64, 64, 0, 127, 2, 12, 2, 127, 127, 4, 8, 16, 127, 62, 65, 65, 62, 0,
127, 9, 9, 6, 0, 62, 65, 65, 190, 0, 127, 9, 9, 118, 0, 70, 73, 73, 50, 0,
1, 1, 127, 1, 1, 63, 64, 64, 63, 0, 15, 48, 64, 48, 15, 63, 64, 56, 64, 63,
99, 20, 8, 20, 99, 7, 8, 112, 8, 7, 97, 81, 73, 71, 0, 127, 65, 0, 0, 0,
1, 6, 24, 96, 0, 65, 127, 0, 0, 0, 2, 1, 2, 0, 0, 64, 64, 64, 64, 0,
1, 2, 0, 0, 0, 32, 84, 84, 120, 0, 127, 68, 68, 56, 0, 56, 68, 68, 40, 0,
56, 68, 68, 127, 0, 56, 84, 84, 24, 0, 4, 126, 5, 0, 0, 152, 164, 164, 120, 0,
127, 4, 4, 120, 0, 68, 125, 64, 0, 0, 64, 128, 132, 125, 0, 127, 16, 40, 68, 0,
65, 127, 64, 0, 0, 124, 4, 124, 4, 120, 124, 4, 4, 120, 0, 56, 68, 68, 56, 0,
252, 36, 36, 24, 0, 24, 36, 36, 252, 0, 124, 8, 4, 4, 0, 72, 84, 84, 36, 0,
4, 63, 68, 0, 0, 60, 64, 64, 124, 0, 28, 32, 64, 32, 28, 60, 64, 60, 64, 60,
68, 40, 16, 40, 68, 156, 160, 160, 124, 0, 100, 84, 76, 0, 0, 8, 54, 65, 0, 0,
127, 0, 0, 0, 0, 65, 54, 8, 0, 0, 8, 4, 8, 4, 0, 0, 0, 0, 0, 0,
]

assert len(original_ascii) == 128 * 5, f"Expected {128*5} bytes for ASCII, got {len(original_ascii)}"

# Build Cyrillic glyphs
upper_order = ['A','B','V','G','D','E','Zh','Z','I','J','K','L','M','N','O','P',
               'R','S','T','U','F','Kh','Ts','Ch','Sh','Shch','Hard','Y','Soft','Eh','Yu','Ya']

lower_ap_order = ['a','b','v','g','d','e','zh','z','i','j','k','l','m','n','o','p']

lower_rya_order = ['r','s','t','u','f','kh','ts','ch','sh','shch','hard','y','soft','eh','yu','ya']

# positions 128-143: lowercase р-я
rya_bytes = []
for name in lower_rya_order:
    rya_bytes.extend(glyph_from_grid(lower_rya_glyphs[name]))

# positions 144-175: uppercase А-Я
upper_bytes = []
for name in upper_order:
    upper_bytes.extend(glyph_from_grid(upper_glyphs[name]))

# position 176: empty
empty_bytes = [0, 0, 0, 0, 0]

# positions 177-192: lowercase а-п
ap_bytes = []
for name in lower_ap_order:
    ap_bytes.extend(glyph_from_grid(lower_ap_glyphs[name]))

# position 193: Ё
yo_upper_bytes = glyph_from_grid(yo_upper)

# position 194: ё
yo_lower_bytes = glyph_from_grid(yo_lower)

# Padding to fill up to 256 chars (remaining are zeros)
total_cyrillic = rya_bytes + upper_bytes + empty_bytes + ap_bytes + yo_upper_bytes + yo_lower_bytes
# positions 128-194 = 67 chars = 335 bytes

remaining = (256 - 195) * 5  # positions 195-255
padding = [0] * remaining

all_data = original_ascii + total_cyrillic + padding

assert len(all_data) == 256 * 5, f"Expected {256*5}, got {len(all_data)}"

# Format output
print("// Improved RiftLink glcdfont — Cyrillic 5x7 bitmap font")
print("// Layout: 128-143=lowercase r-ya, 144-175=uppercase A-Ya,")
print("//         176=empty, 177-192=lowercase a-p, 193=Yo, 194=yo")
print("// Generated by generate_glcdfont.py")
print("")
print("#ifndef FONT5X7_H")
print("#define FONT5X7_H")
print("")
print("#ifdef __AVR__")
print("#include <avr/io.h>")
print("#include <avr/pgmspace.h>")
print("#elif defined(ESP8266)")
print("#include <pgmspace.h>")
print("#else")
print("#define PROGMEM")
print("#endif")
print("")
print("static const unsigned char font[] PROGMEM = {")

# Print 20 values per line (4 chars worth)
for i in range(0, len(all_data), 20):
    chunk = all_data[i:i+20]
    line = ", ".join(str(b) for b in chunk)
    if i + 20 < len(all_data):
        line += ","
    print(line)

print("};")
print("")
print("#endif")

# Also visualize for verification
print("\n\n// ===== VERIFICATION =====")

def render_glyph_from_bytes(cols):
    lines = []
    for row in range(8):
        line = ""
        for col in range(5):
            if cols[col] & (1 << row):
                line += "##"
            else:
                line += "  "
        lines.append(line)
    return lines

cyrillic_labels_upper = list("ABVGDEZHZIJKLMNOP") + ["R","S","T","U","F","Kh","Ts","Ch","Sh","Shch","Hard","Y","Soft","Eh","Yu","Ya"]
# fix: need proper 32 labels
cyrillic_labels_upper = upper_order

print("\n// UPPERCASE A-Ya (144-175):")
for i, name in enumerate(upper_order):
    idx = 144 + i
    off = idx * 5
    cols = all_data[off:off+5]
    glyph = render_glyph_from_bytes(cols)
    print(f"// [{idx}] {name}:")
    for line in glyph:
        print(f"//   |{line}|")
    print()

print("\n// LOWERCASE a-p (177-192):")
for i, name in enumerate(lower_ap_order):
    idx = 177 + i
    off = idx * 5
    cols = all_data[off:off+5]
    glyph = render_glyph_from_bytes(cols)
    print(f"// [{idx}] {name}:")
    for line in glyph:
        print(f"//   |{line}|")
    print()

print("\n// LOWERCASE r-ya (128-143):")
for i, name in enumerate(lower_rya_order):
    idx = 128 + i
    off = idx * 5
    cols = all_data[off:off+5]
    glyph = render_glyph_from_bytes(cols)
    print(f"// [{idx}] {name}:")
    for line in glyph:
        print(f"//   |{line}|")
    print()

print("\n// Yo/yo (193-194):")
for idx, name in [(193, "Yo"), (194, "yo")]:
    off = idx * 5
    cols = all_data[off:off+5]
    glyph = render_glyph_from_bytes(cols)
    print(f"// [{idx}] {name}:")
    for line in glyph:
        print(f"//   |{line}|")
    print()
