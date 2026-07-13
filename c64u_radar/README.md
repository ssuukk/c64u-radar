# C64U Radar

C64 Ultimate ADS-B radar scope. This source tree contains no baked-in
coordinates or LAN address — the compiled server address starts at `0.0.0.0`
and the user always picks a real center on the C64.

The visible menu is:

```text
C64U RADAR V0.1
Choose an option to center your scope:
1. CENTER ON LAT/LONG
2. CENTER ON ICAO AIRPORT CODE
```

The version string is on the main menu title only — the bitmap scope
screen's own title has no room for it (14-character column). Bump
`VERSION_STRING` in `c64u_radar.c` for future releases.

Users choose a latitude/longitude or four-letter ICAO airport center. There is
no numbered menu option for the server IP; the address normally fills itself
in. While the menu idles, the program watches a mailbox at `$8AC0` that the
Python server fills over the C64 Ultimate REST API (see
`../server/README.md`), and the menu shows one of three states:

- `SEARCHING FOR SERVER...` — no mailbox value adopted yet.
- `AUTO DISCOVERED SERVER AT:` — the server found and pushed its address.
- `USER ENTERED SERVER IP:` — the user overrode it manually (below).

Pressing Commodore+S (`C= + S`) opens manual IP entry. A manually entered
address is sticky for the rest of the run: it is mirrored into the mailbox,
but the program stops auto-adopting further server pushes until the next
relaunch/reset, so a background server on a different address can't silently
overwrite a deliberate manual choice. Either way — pushed or manually
entered — the address survives reset/relaunch, because the mailbox lives in
RAM outside the loaded program and is only lost on power-off.

Mailbox layout at `$8AC0` (23 bytes): `MR2M` magic, version 1, IP length,
16-byte IP text field, XOR checksum (seed `$A5`). The program only adopts a
mailbox IP whose checksum validates and which parses as dotted IPv4.

The Commodore+S hotkey is detected by scanning the KERNAL's own keyboard
decode tables at `$EB81` (unshifted) and `$EC03` (Commodore) at startup for
the physical S key, rather than hardcoding a PETSCII byte for the
Commodore-modified key. `POKE 657,128` disables the KERNAL's automatic
SHIFT+Commodore charset toggle, since Commodore is now an application hotkey
modifier and the program owns a fixed lowercase/uppercase charset choice.

Build with cc65 on `PATH`:

```sh
make clean all
```

Output: `c64u_radar.prg`. The build fails if program/data reaches the fixed
sprite block at `$5A00`.

Run the native harness from `host_test/` with:

```sh
cc -DHOST_TEST -I. -I.. -o harness harness.c
./harness
```

The companion Python server lives in this repo's `server/` folder.
