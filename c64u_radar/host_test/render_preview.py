"""Compose the harness dumps into a pixel-true preview PNG (Pepto palette, x3)."""
from PIL import Image

PAL = [(0,0,0),(255,255,255),(104,55,43),(112,164,178),(111,61,134),(88,141,67),
       (53,40,121),(184,199,111),(111,79,37),(67,57,0),(154,103,89),(68,68,68),
       (108,108,108),(154,210,132),(108,94,181),(149,149,149)]
FG = PAL[13]                    # light-green cells ($D0) everywhere

bm = open("bitmap.bin","rb").read()
img = Image.new("RGB", (320,200), PAL[0])
px = img.load()
for y in range(200):
    base = (y >> 3) * 320 + (y & 7)
    for xc in range(40):
        b = bm[base + xc*8]
        if b:
            for bit in range(8):
                if b & (0x80 >> bit):
                    px[xc*8 + bit, y] = FG

spr = open("sprblock.bin","rb").read()
lines = open("sprites.txt").read().split()
en = int(lines[0]); vals = list(map(int, lines[1:]))
for i in range(8):
    if not (en & (1 << i)): continue
    sx, sy, col = vals[i*3] - 24, vals[i*3+1] - 50, PAL[vals[i*3+2]]
    for row in range(21):
        for byte in range(3):
            b = spr[i*64 + row*3 + byte]
            for bit in range(8):
                if b & (0x80 >> bit):
                    x, y = sx + byte*8 + bit, sy + row
                    if 0 <= x < 320 and 0 <= y < 200: px[x, y] = col

img.resize((960,600), Image.NEAREST).save("scope_preview.png")
print("wrote scope_preview.png")
