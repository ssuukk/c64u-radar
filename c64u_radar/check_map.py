import re, sys
path = sys.argv[1] if len(sys.argv) > 1 else "c64u_radar.map"
seg = re.search(r"Segment list:.*?\n\n", open(path).read(), re.S).group(0)
end = max(int(m.group(2), 16) for m in re.finditer(r"^(\w+)\s+[0-9A-F]+\s+([0-9A-F]+)", seg, re.M)
          if m.group(1) != "ZEROPAGE")
print(f"top of program+data: ${end:04X} (must stay below $5A00)")
sys.exit(1 if end >= 0x5A00 else 0)
