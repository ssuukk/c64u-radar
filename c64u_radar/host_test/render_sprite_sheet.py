"""Render numbered markers with uniform stems for eight compass directions."""
from PIL import Image, ImageDraw

DIRECTIONS = ("N", "NE", "E", "SE", "S", "SW", "W", "NW")
SCALE = 6
CELL_W, CELL_H = 176, 164
GREEN = (154, 210, 132)

patterns = open("sprite_sheet.bin", "rb").read()
if len(patterns) != 512:
    raise SystemExit("sprite_sheet.bin must contain eight 64-byte patterns")

image = Image.new("RGB", (CELL_W * 4, CELL_H * 2), (0, 0, 0))
draw = ImageDraw.Draw(image)
for index, label in enumerate(DIRECTIONS):
    ox = (index % 4) * CELL_W + 16
    oy = (index // 4) * CELL_H + 22
    pattern = patterns[index * 64:(index + 1) * 64]
    draw.text((ox + 62, oy - 18), label, fill=GREEN)
    for y in range(21):
        for x in range(24):
            byte = pattern[y * 3 + (x >> 3)]
            if byte & (0x80 >> (x & 7)):
                draw.rectangle((ox + x * SCALE, oy + y * SCALE,
                                ox + (x + 1) * SCALE - 1,
                                oy + (y + 1) * SCALE - 1), fill=GREEN)

image.save("direction_sprite_sheet.png")
print("wrote direction_sprite_sheet.png")
