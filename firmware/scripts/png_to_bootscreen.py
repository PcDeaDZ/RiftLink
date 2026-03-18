#!/usr/bin/env python3
"""
Конвертация PNG в C-массивы для бутскрина RiftLink.
Использование: python png_to_bootscreen.py <source.png>
Создаёт: bootscreen_oled.h (128x64), bootscreen_paper.h (250x122)
Требуется: pip install Pillow
"""

import sys
from pathlib import Path

try:
    from PIL import Image
except ImportError:
    print("Требуется Pillow: pip install Pillow")
    sys.exit(1)

# Размеры экранов
OLED_W, OLED_H = 128, 64
PAPER_W, PAPER_H = 250, 122

# Порог: пиксели ярче этого -> 1 (белый), иначе 0. Ниже = жирнее.
THRESHOLD = 70

# Утолщение: 0 = без расширения, 1 = линии толще на 1 пиксель
BOLD_DILATE = 0


def _dilate_bits(bits: list[list[int]], w: int, h: int) -> list[list[int]]:
    """Морфологическое расширение: белый пиксель «расплывается» на соседей."""
    out = [[0] * w for _ in range(h)]
    for y in range(h):
        for x in range(w):
            if bits[y][x]:
                for dy in (-1, 0, 1):
                    for dx in (-1, 0, 1):
                        ny, nx = y + dy, x + dx
                        if 0 <= ny < h and 0 <= nx < w:
                            out[ny][nx] = 1
    return out


def image_to_bitmap(img: Image.Image, width: int, height: int) -> bytes:
    """Конвертирует изображение в 1-bit bitmap (MSB first, row-major)."""
    gray = img.resize((width, height), Image.Resampling.LANCZOS).convert("L")
    pixels = gray.load()
    bits = [[1 if pixels[x, y] > THRESHOLD else 0 for x in range(width)] for y in range(height)]

    for _ in range(BOLD_DILATE):
        bits = _dilate_bits(bits, width, height)

    bytes_per_row = (width + 7) // 8
    result = bytearray(bytes_per_row * height)
    for y in range(height):
        for x in range(width):
            bit = bits[y][x]
            byte_idx = y * bytes_per_row + x // 8
            bit_idx = 7 - (x % 8)
            result[byte_idx] |= bit << bit_idx
    return bytes(result)


def bytes_to_c_array(data: bytes, name: str) -> str:
    """Форматирует байты как C массив."""
    lines = []
    for i in range(0, len(data), 16):
        chunk = data[i : i + 16]
        hex_str = ", ".join(f"0x{b:02X}" for b in chunk)
        lines.append(f"  {hex_str},")
    return f"static const uint8_t {name}[] = {{\n" + "\n".join(lines) + "\n};"


def main():
    if len(sys.argv) < 2:
        print("Usage: python png_to_bootscreen.py <source.png>")
        sys.exit(1)

    src = Path(sys.argv[1])
    if not src.exists():
        print(f"File not found: {src}")
        sys.exit(1)

    script_dir = Path(__file__).parent
    out_dir = script_dir.parent / "include"

    img = Image.open(src).convert("RGB")
    # Инвертируем: исходник тёмный фон, светлый текст — для OLED/E-Ink нужен светлый на тёмном
    # Оставляем как есть: тёмный фон -> 0, светлый текст/меш -> 1

    # OLED 128x64
    oled_data = image_to_bitmap(img, OLED_W, OLED_H)
    oled_h = out_dir / "bootscreen_oled.h"
    out_dir.mkdir(parents=True, exist_ok=True)
    with open(oled_h, "w", encoding="utf-8") as f:
        f.write("// RiftLink boot screen — OLED 128x64 (V3/V4)\n")
        f.write("// Auto-generated from app_icon_source.png\n\n")
        f.write("#ifndef RIFTLINK_BOOTSCREEN_OLED_H\n#define RIFTLINK_BOOTSCREEN_OLED_H\n\n")
        f.write(f"#define BOOTSCREEN_OLED_W {OLED_W}\n#define BOOTSCREEN_OLED_H {OLED_H}\n\n")
        f.write(bytes_to_c_array(oled_data, "bootscreen_oled"))
        f.write("\n\n#endif\n")
    print(f"Created {oled_h}")

    # Paper 250x122
    paper_data = image_to_bitmap(img, PAPER_W, PAPER_H)
    paper_h = out_dir / "bootscreen_paper.h"
    with open(paper_h, "w", encoding="utf-8") as f:
        f.write("// RiftLink boot screen — E-Paper 250x122 (Paper)\n")
        f.write("// Auto-generated from app_icon_source.png\n\n")
        f.write("#ifndef RIFTLINK_BOOTSCREEN_PAPER_H\n#define RIFTLINK_BOOTSCREEN_PAPER_H\n\n")
        f.write(f"#define BOOTSCREEN_PAPER_W {PAPER_W}\n#define BOOTSCREEN_PAPER_H {PAPER_H}\n\n")
        f.write(bytes_to_c_array(paper_data, "bootscreen_paper"))
        f.write("\n\n#endif\n")
    print(f"Created {paper_h}")


if __name__ == "__main__":
    main()
