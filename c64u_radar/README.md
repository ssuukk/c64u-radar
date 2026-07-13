# C64U Radar — C64 side

C64 ADS-B radar scope using **Meatloaf HTTPS** directly to adsb.fi.
No Python server, no Ultimate Cartridge UCI registers needed.

## Menu

```text
C64U RADAR V0.2
Choose an option to center your scope:
1. CENTER ON LAT/LONG
2. CENTER ON ICAO AIRPORT CODE
```

- **Option 1** — enter signed decimal latitude/longitude (e.g. `51.4775` `-0.4614`).
- **Option 2** — enter a 4-letter ICAO airport code (e.g. `EGLL` for Heathrow).
  The code is resolved from a built-in table of ~280 major airports.

The C64 opens a Meatloaf full-mode HTTP connection to `opendata.adsb.fi/api/v3`
and extracts aircraft fields via JSON Pointer commands.  No PC helper needed.

## Build

```sh
make clean all        # requires cc65 on PATH
make host_test        # builds native harness (no C64 needed)
```

Output: `c64u_radar.prg`.  Build fails if program/data reaches the fixed
sprite block at `$5A00` (checked by `check_map.py`).

## Memory layout

Upper RAM worksheet:
- `$8000` charset copy (2 KB)
- `$8800` rowbase (100 B)
- `$8900` blob buffer (232 B)
- `$8A00` URL buffer (96 B)
- `$8A60` scope labels (32 B)
- `$8A80` JSON Pointer scratch (32 B)
- `$8AC0` JSON value buffer (36 B)

The old `$8AC0` mailbox (`MR2M` magic, server-address push) is no longer used
— the adsb.fi URL is always built from user input.
