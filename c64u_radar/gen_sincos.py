#!/usr/bin/env python3
"""Generate sincos.h with 360-entry sin/cos lookup tables scaled by 127."""
import math

lines = [
    "#ifndef SINCOS_H",
    "#define SINCOS_H",
    "",
    "/* Sin/Cos lookup tables scaled by 127 for fixed-point radar projection */",
    "/* Generated: 360 entries, 1-degree resolution */",
    "",
    "static const signed char sin127[360] = {",
]

for i in range(360):
    val = int(round(math.sin(math.radians(i)) * 127))
    if i % 10 == 0:
        lines.append("    " + ", ".join(f"{int(round(math.sin(math.radians(j)) * 127)):4d}" for j in range(i, min(i + 10, 360))) + ",")

lines[-1] = lines[-1].rstrip(",")
lines.append("};")
lines.append("")
lines.append("static const signed char cos127[360] = {")

for i in range(360):
    if i % 10 == 0:
        end = min(i + 10, 360)
        vals = [int(round(math.cos(math.radians(j)) * 127)) for j in range(i, end)]
        lines.append("    " + ", ".join(f"{v:4d}" for v in vals) + ",")

lines[-1] = lines[-1].rstrip(",")
lines.append("};")
lines.append("")
lines.append("#endif")

with open("sincos.h", "w") as f:
    f.write("\n".join(lines) + "\n")

print("Generated sincos.h")
