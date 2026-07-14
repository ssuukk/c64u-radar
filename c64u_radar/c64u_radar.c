/* ==========================================================================
 * C64U RADAR -- public ADS-B scope for the Commodore 64
 *
 * Direct HTTPS to adsb.fi OpenData API v3 via Meatloaf IEC device.
 * No Python server, no Ultimate-II+ UCI registers needed.
 *
 *   feed:   opens an HTTPS connection to opendata.adsb.fi, issues GET
 *           /api/v3/lat/.../lon/.../dist/..., and extracts JSON fields
 *           through Meatloaf's built-in JSON Pointer ("j") command.
 *   blob:   builds the in-memory LD wire format so render_targets()
 *           can stamp sprites and the side panel unchanged.
 *
 * Hardware requirements:
 *   - Commodore 64
 *   - Meatloaf (or compatible IEC device with full-mode HTTP + TLS)
 *   - Device 8, secondary address 2 (read-write full HTTP mode)
 *
 * Display: hires bitmap 320x200, VIC bank 1
 *   $5A00-$5BFF eight independent 64-byte sprite patterns
 *   $5C00 screen matrix (color cells, $D0 = light green on black)
 *   $6000 bitmap
 *   sprite pointers $68-$6F at $5FF8
 *   scope = left 200x200 px, center (100,100), 9nm ring r=95
 *   table = char columns 26..39
 *
 * Build:  make            (cl65 -t c64)
 * ========================================================================== */

#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <peekpoke.h>
#ifndef HOST_TEST
#include <conio.h>
#include <c64.h>
#include <cbm.h>
#endif
#include "sincos.h"
#include "airports.h"

/* ---- configuration ------------------------------------------------------ */
#define ADSB_BASE       "https://opendata.adsb.fi/api/v3"
#define DEFAULT_RANGE   9                   /* nautical miles                  */
#define POLL_JIF        600                 /* 10 s between polls (jiffies)   */
#define REPLY_JIF       1200                /* 20 s network timeout            */
#define VERSION_STRING  "V0.2"
#define ML_CH           2                   /* Meatloaf logical channel        */
#define ML_DEV          8                   /* Meatloaf IEC device number      */
#define ML_SA           2                   /* secondary address = full mode   */

/* ---- video map ----------------------------------------------------------- */
#define MATRIX      0x5C00
#define BITMAP      0x6000
#define SPR_DATA    0x5A00
#define SPR_PTRVAL  0x68              /* ($5A00-$4000)/64                   */
#define COL_GREEN_BLACK 0xD0
#define COL_RED_BLACK   0x20
#define SPR_GREEN   13

#define SCOPE_C     100               /* scope center px                    */
#define RING_PX     95                /* 9 nm ring                          */
#define TBL_COL     26                /* table starts at char column 26    */
#define TBL_W       14

#define MAGIC0      0x4C              /* 'L' */
#define MAGIC1      0x44              /* 'D' */
#define REC_SZ      28
#define MAX_AC      8

/* link states */
enum { ST_OK, ST_STALE, ST_DOWN, ST_BAD, ST_WAIT, ST_LOCATION, ST_EXIT };

/* Host-test hook: build with -DHOST_TEST (see host_test/)                */
#ifdef HOST_TEST
unsigned char host_ram[65536];
#  define MEM(a) (host_ram + (a))
#else
#  define MEM(a) ((unsigned char*)(unsigned int)(a))
#endif

static unsigned char* const bmp = MEM(BITMAP);
static unsigned char* const mtx = MEM(MATRIX);

/* Upper RAM workspace                                                       */
static unsigned char* const charset = MEM(0x8000);
static unsigned int* const rowbase = (unsigned int*)MEM(0x8800);
static const unsigned char bmask[8] =
    { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 };

/* Blob buffer (LD wire format, max 232 bytes) at 0x8900                    */
static unsigned char* const blob = MEM(0x8900);

/* URL buffer at 0x8A00 (max 96 bytes for adsb.fi v3 URL).  Used only during
 * cbm_open(); after the connection opens its content is dead and the space
 * is free to reuse.                                                          */
static char* const url_buf = (char*)MEM(0x8A00);

/* Display labels — live for the duration of the scope loop, must not overlap
 * url_buf.  Placed at +0x60 to leave url_buf a full 96-byte safety margin.   */
static char* const scope_label1 = (char*)MEM(0x8A60);
static char* const scope_label2 = (char*)MEM(0x8A70);

/* JSON value read buffer size (must match j_val area below)                */
#define JVAL_SZ 36

/* JSON Pointer construction + response buffers (only used inside fetch())
 * j_ptr at $8A80 fits "/ac/15/alt_baro\0" (max ~18 bytes for 2-digit idx).
 * j_val at $8AC0 (32 bytes, fits any single JSON field the API returns).   */
static char* const j_ptr = (char*)MEM(0x8A80);
static char* const j_val = (char*)MEM(0x8AC0);

static unsigned char link_down_displayed;
static unsigned char current_range = DEFAULT_RANGE;

/* ---- ASCII → screen-code (from original -- these are PETSCII-safe) ------ */
static unsigned char asc2sc(char c)
{
    unsigned char o = (unsigned char)c;
    if (o >= 0xC1 && o <= 0xDA) return o & 0x1F;
    if (o >= 0x41 && o <= 0x5A) return o - 0x40;
    if (o >= 0x61 && o <= 0x7A) return o - 0x60;
    if (o >= 0x20 && o <= 0x3F) return o;
    return 46;                                    /* '.' */
}

/* ==========================================================================
 * string helpers (no reliance on large stdlib routines)
 * ========================================================================== */

static void str_cpy(char* dst, const char* src, unsigned char maxlen)
{
    unsigned char i = 0;
    while (*src && i < maxlen - 1) { dst[i++] = *src++; }
    dst[i] = 0;
}

/* ---------------------------------------------------------------------------
 * adsb.fi JSON field extraction via Meatloaf "j" commands
 *
 * After "m get" + "s" is sent, the response is buffered inside Meatloaf.
 * Each "j /path" command extracts one field from the cached JSON without
 * re-fetching the URL. We read the result byte-by-byte until EOI.
 * ---------------------------------------------------------------------------
 */

/* Read one response value into buf (max len-1 chars, NUL-terminated).
 * Returns 1 on success (at least one byte read), 0 on EOI/error.          */
static unsigned char ml_read_val(unsigned char ch, char* buf,
                                 unsigned char maxlen)
{
    unsigned char i = 0;
    while (i < maxlen - 1) {
        if (cbm_read(ch, (unsigned char*)(buf + i), 1) == 0) break;
        if (buf[i] == '\r' || buf[i] == '\n') break;
        ++i;
    }
    buf[i] = 0;
    return i > 0;
}

/* Query a JSON Pointer and read the result as a string into buf.
 * Returns 1 on success, 0 if the field is absent.                         */
static unsigned char j_str(unsigned char ch, const char* pointer,
                           char* buf, unsigned char maxlen)
{
    cbm_write(ch, "j ", 2);
    cbm_write(ch, pointer, (unsigned char)strlen(pointer));
    cbm_write(ch, "\r\n", 2);
    return ml_read_val(ch, buf, maxlen);
}

/* Query a JSON Pointer and read the result as an integer.
 * Returns 1 on success, 0 on missing field.                               */
static unsigned char j_int(unsigned char ch, const char* pointer, int* val)
{
    char buf[20];
    if (!j_str(ch, pointer, buf, sizeof(buf))) return 0;
    *val = atoi(buf);
    return 1;
}

/* Parse a decimal string in tenths (e.g. "4.5" → 45).
 * Returns 1 on success, 0 on parse failure.                                */
static unsigned char parse_tenths(const char* s, int* out)
{
    int val = 0, frac = 0, sign = 1;
    if (!s || !*s) return 0;
    if (*s == '-') { sign = -1; ++s; }
    if (*s == '+') { ++s; }
    if (!(*s >= '0' && *s <= '9')) return 0;
    while (*s >= '0' && *s <= '9') {
        val = val * 10 + (*s - '0');
        ++s;
    }
    if (*s == '.') {
        ++s;
        if (*s >= '0' && *s <= '9') { frac = *s - '0'; ++s; }
        while (*s >= '0' && *s <= '9') ++s;  /* consume rest of fraction */
    }
    if (*s) return 0;
    *out = sign * (val * 10 + frac);
    return 1;
}

/* Check if a string equals "ground" (case-insensitive)                     */
static unsigned char is_ground(const char* s)
{
    return (s[0] == 'g' || s[0] == 'G') &&
           (s[1] == 'r' || s[1] == 'R') &&
           (s[2] == 'o' || s[2] == 'O') &&
           (s[3] == 'u' || s[3] == 'U') &&
           (s[4] == 'n' || s[4] == 'N') &&
           (s[5] == 'd' || s[5] == 'D') && !s[6];
}

/* Validate 4-letter ICAO code */ /* (basic range check — used at compile time) */
static unsigned char is_icao(const char* code)
{
    unsigned char i, c;
    for (i = 0; i < 4; ++i) {
        c = (unsigned char)code[i];
        /* Accept high PETSCII (0xC1-0xDA) or ASCII (0x41-0x5A) uppercase */
        if ((c >= 0xC1 && c <= 0xDA) || (c >= 0x41 && c <= 0x5A))
            continue;
        return 0;
    }
    return code[4] == 0;
}

/* Pack 4-char ICAO code into uint32 big-endian.
 * Accepts high PETSCII bytes (0xC1-0xDA) or ASCII (0x41-0x5A).             */
static unsigned long code_to_u32(const char* code)
{
    unsigned char b[4], c;
    unsigned char i;
    for (i = 0; i < 4; ++i) {
        c = (unsigned char)code[i];
        /* Normalize PETSCII → ASCII: 0xD0 (0x50|0x80) → 0x50, etc. */
        b[i] = (c & 0x80) ? (c & 0x7F) : c;
    }
    return ((unsigned long)b[0] << 24) |
           ((unsigned long)b[1] << 16) |
           ((unsigned long)b[2] <<  8) |
            (unsigned long)b[3];
}

/* Binary-search the airport table for an ICAO code.
 * Returns 1 and sets lat/lon on success.                                   */
static unsigned char find_airport(const char* code,
                                  int32_t* lat, int32_t* lon)
{
    unsigned long target = code_to_u32(code);
    int lo = 0, hi = AIRPORT_COUNT - 1, mid;
    while (lo <= hi) {
        mid = (lo + hi) >> 1;
        if (airports[mid].code < target)      lo = mid + 1;
        else if (airports[mid].code > target) hi = mid - 1;
        else { *lat = airports[mid].lat; *lon = airports[mid].lon; return 1; }
    }
    return 0;
}

/* ==========================================================================
 * startup location request
 * ========================================================================== */

/* Validate a decimal coordinate without linking the C64 floating-point lib */
static unsigned char valid_coordinate(const char* text, unsigned int limit)
{
    unsigned int whole = 0;
    unsigned char digits = 0, frac_digits = 0, frac_nonzero = 0;
    const char* p = text;
    if (*p == '+' || *p == '-') ++p;
    while (*p >= '0' && *p <= '9') {
        if (digits == 3) return 0;
        whole = whole * 10 + (unsigned int)(*p - '0');
        ++digits; ++p;
    }
    if (!digits) return 0;
    if (*p == '.') {
        ++p;
        while (*p >= '0' && *p <= '9') {
            if (*p != '0') frac_nonzero = 1;
            ++frac_digits; ++p;
        }
        if (!frac_digits) return 0;
    }
    if (*p) return 0;
    return whole < limit || (whole == limit && !frac_nonzero);
}

/* Set lat/lon from user-entered coordinates.  Builds the URL for adsb.fi. */
static unsigned char set_position_request(const char* latitude,
                                          const char* longitude)
{
    if (!valid_coordinate(latitude, 90) || !valid_coordinate(longitude, 180))
        return 0;
    /* Build adsb.fi v3 URL */
    sprintf(url_buf, "%s/lat/%s/lon/%s/dist/%d",
            ADSB_BASE, latitude, longitude, current_range + 1);
    str_cpy(scope_label1, latitude, 14);
    str_cpy(scope_label2, longitude, 14);
    return 1;
}

/* Set lat/lon from ICAO airport code lookup.                              */
static unsigned char set_icao_request(const char* code)
{
    int32_t icao_lat, icao_lon;
    char lat_str[16], lon_str[16];
    unsigned char neg;
    if (!is_icao(code)) return 0;
    if (!find_airport(code, &icao_lat, &icao_lon)) return 0;
    /* Convert integer micro-degrees back to signed decimal text */
    neg = icao_lat < 0;
    if (neg) icao_lat = -icao_lat;
    sprintf(lat_str, "%s%ld.%06ld",
            neg ? "-" : "",
            (long)(icao_lat / 1000000),
            (long)(icao_lat % 1000000));
    neg = icao_lon < 0;
    if (neg) icao_lon = -icao_lon;
    sprintf(lon_str, "%s%ld.%06ld",
            neg ? "-" : "",
            (long)(icao_lon / 1000000),
            (long)(icao_lon % 1000000));
    sprintf(url_buf, "%s/lat/%s/lon/%s/dist/%d",
            ADSB_BASE, lat_str, lon_str, current_range + 1);
    str_cpy(scope_label1, code, 14);
    scope_label2[0] = 0;
    return 1;
}

static unsigned char key_to_petscii(unsigned char c)
{
    if (c >= 0x41 && c <= 0x5A) c |= 0x80;
    else if (c >= 0x61 && c <= 0x7A)
        c = (unsigned char)((c - 0x20) | 0x80);
    return c;
}

#ifndef HOST_TEST
static void menu_putsxy(unsigned char x, unsigned char y, const char* text)
{
    unsigned char c;
    gotoxy(x, y);
    while (*text) {
        c = (unsigned char)*text++;
        cputc((char)c);
    }
}

static unsigned char read_input(char* result, unsigned char maximum)
{
    unsigned char length = 0, c;
    unsigned char start_x = wherex(), start_y = wherey();
    cursor(1);
    for (;;) {
        c = (unsigned char)cgetc();
        if (c == CH_ENTER) break;
        if (c == CH_DEL || c == 8) {
            if (length) {
                --length;
                gotoxy((unsigned char)(start_x + length), start_y);
                cputc(' ');
                gotoxy((unsigned char)(start_x + length), start_y);
            }
            continue;
        }
        c = key_to_petscii(c);
        if (((c >= 0x20 && c <= 0x7E) || (c >= 0xC1 && c <= 0xDA))
                && length < maximum) {
            result[length++] = (char)c;
            cputc((char)c);
        }
    }
    cursor(0);
    result[length] = 0;
    return length;
}

static void setup_location(void)
{
    char latitude[17], longitude[17], icao[5];
    unsigned char choice;
    textcolor(COLOR_LIGHTGREEN);
    bgcolor(COLOR_BLACK);
    bordercolor(COLOR_BLACK);
    url_buf[0] = 0;
    scope_label1[0] = 0;
    scope_label2[0] = 0;

    for (;;) {
        clrscr();
        menu_putsxy(13, 2, "C64U RADAR " VERSION_STRING);
        menu_putsxy(1, 4, "Choose an option to center your scope:");
        menu_putsxy(4, 6, "1. CENTER ON LAT/LONG");
        menu_putsxy(4, 8, "2. CENTER ON ICAO AIRPORT CODE");
        menu_putsxy(6, 14, "Traffic data source: adsb.fi");
        menu_putsxy(6, 15, "via Meatloaf HTTPS");
        menu_putsxy(13, 23, "levimaaia.com");
        menu_putsxy(9, 24, "youtube.com/@levimaaia");

        choice = (unsigned char)cgetc();
        choice = key_to_petscii(choice);

        if (choice == '1' || choice == 'P') {
            clrscr();
            menu_putsxy(7, 1, "ENTER LATITUDE / LONGITUDE");
            menu_putsxy(5, 3, "FORMAT: SIGNED DECIMAL DEGREES");
            menu_putsxy(5, 4, "LATITUDE:  -90 TO 90");
            menu_putsxy(5, 5, "LONGITUDE: -180 TO 180");
            menu_putsxy(6, 6, "EX: 39.117210  -94.635600");
            menu_putsxy(3, 9, "LATITUDE : ");
            read_input(latitude, 15);
            menu_putsxy(3, 12, "LONGITUDE: ");
            read_input(longitude, 16);
            if (set_position_request(latitude, longitude)) return;
            menu_putsxy(3, 16, "INVALID POSITION");
            menu_putsxy(3, 18, "PRESS A KEY TO RETRY");
            cgetc();
            continue;
        }
        if (choice == '2' || choice == 'I') {
            clrscr();
            menu_putsxy(7, 4, "ENTER ICAO CODE");
            menu_putsxy(12, 8, "CODE: ");
            read_input(icao, 4);
            if (set_icao_request(icao)) return;
            menu_putsxy(8, 12, "INVALID ICAO CODE");
            menu_putsxy(8, 14, "PRESS A KEY");
            cgetc();
            continue;
        }
    }
}
#endif

/* 3x5 digit masks for 1..8 (unchanged)                                      */
static const unsigned char digit_rows[8][5] = {
    { 2, 6, 2, 2, 7 },
    { 7, 1, 7, 4, 7 },
    { 7, 1, 7, 1, 7 },
    { 5, 5, 7, 1, 1 },
    { 7, 4, 7, 1, 7 },
    { 7, 4, 7, 5, 7 },
    { 7, 1, 2, 2, 2 },
    { 7, 5, 7, 5, 7 }
};

static const unsigned char dir_x[8] = { 11, 18, 21, 18, 11, 4, 1, 4 };
static const unsigned char dir_y[8] = {  0,  3, 10, 17, 20,17,10, 3 };

/* ==========================================================================
 * low-level drawing (unchanged from original)
 * ========================================================================== */

static void plot(int x, int y)
{
    if ((unsigned)x > 199 || (unsigned)y > 199) return;
    bmp[rowbase[y >> 3] + (y & 7) + (x & 0xF8)] |= bmask[x & 7];
}

static void hline(int x0, int x1, int y)
{
    int x;
    for (x = x0; x <= x1; ++x) plot(x, y);
}

static void vline(int y0, int y1, int x)
{
    int y;
    for (y = y0; y <= y1; ++y) plot(x, y);
}

static void spr_plot(unsigned char* p, unsigned char x, unsigned char y)
{
    if (x < 24 && y < 21)
        p[(unsigned int)y * 3 + (x >> 3)] |= bmask[x & 7];
}

static void spr_unplot(unsigned char* p, unsigned char x, unsigned char y)
{
    if (x < 24 && y < 21)
        p[(unsigned int)y * 3 + (x >> 3)] &= (unsigned char)~bmask[x & 7];
}

static void spr_line(unsigned char* p, int x0, int y0, int x1, int y1)
{
    int dx = x1 > x0 ? x1 - x0 : x0 - x1;
    int dy = y1 > y0 ? y1 - y0 : y0 - y1;
    int sx = x0 < x1 ? 1 : -1;
    int sy = y0 < y1 ? 1 : -1;
    int err = dx - dy, e2;
    for (;;) {
        spr_plot(p, (unsigned char)x0, (unsigned char)y0);
        if (x0 == x1 && y0 == y1) break;
        e2 = err << 1;
        if (e2 > -dy) { err -= dy; x0 += sx; }
        if (e2 <  dx) { err += dx; y0 += sy; }
    }
}

static void build_sprite(unsigned char slot, unsigned char track,
                         unsigned char track_unknown)
{
    unsigned char* p = MEM(SPR_DATA + (unsigned int)slot * 64);
    unsigned char sector, x, y, half, row;
    memset(p, 0, 64);

    if (!track_unknown) {
        sector = (unsigned char)((((unsigned int)track + 16) >> 5) & 7);
        spr_line(p, 11, 10, dir_x[sector], dir_y[sector]);
    }

    for (y = 5; y <= 15; ++y) {
        half = (unsigned char)(5 - (y > 10 ? y - 10 : 10 - y));
        for (x = (unsigned char)(11 - half); x <= (unsigned char)(11 + half); ++x)
            spr_plot(p, x, y);
    }

    for (y = 0; y < 5; ++y) {
        row = digit_rows[slot][y];
        for (x = 0; x < 3; ++x)
            if (row & (4 >> x))
                spr_unplot(p, (unsigned char)(10 + x), (unsigned char)(8 + y));
    }
}

static void circle(int cx, int cy, int r)
{
    int x = r, y = 0, err = 1 - r;
    while (x >= y) {
        plot(cx + x, cy + y); plot(cx - x, cy + y);
        plot(cx + x, cy - y); plot(cx - x, cy - y);
        plot(cx + y, cy + x); plot(cx - y, cy + x);
        plot(cx + y, cy - x); plot(cx - y, cy - x);
        ++y;
        if (err < 0) err += (y << 1) + 1;
        else { --x; err += ((y - x) << 1) + 1; }
    }
}

static void draw_sc(unsigned char col, unsigned char row,
                    const unsigned char* sc, unsigned char len)
{
    unsigned char i, k;
    unsigned char* dst = bmp + rowbase[row] + (col << 3);
    const unsigned char* g;
    for (i = 0; i < len; ++i) {
        g = charset + ((unsigned int)sc[i] << 3);
        for (k = 0; k < 8; ++k) dst[k] = g[k];
        dst += 8;
    }
}

static void draw_sc_reverse(unsigned char col, unsigned char row,
                            unsigned char sc)
{
    unsigned char k;
    unsigned char* dst = bmp + rowbase[row] + (col << 3);
    const unsigned char* g = charset + ((unsigned int)sc << 3);
    for (k = 0; k < 8; ++k) dst[k] = (unsigned char)~g[k];
}

static void draw_ascii(unsigned char col, unsigned char row,
                       const char* s, unsigned char width)
{
    unsigned char buf[40];
    unsigned char i = 0;
    while (*s && i < width) buf[i++] = asc2sc(*s++);
    while (i < width) buf[i++] = 0x20;
    draw_sc(col, row, buf, width);
}

static void draw_centered(unsigned char col, unsigned char row,
                          const char* s, unsigned char width)
{
    char buf[15];
    unsigned char n = (unsigned char)strlen(s), left;
    if (n > width) n = width;
    left = (unsigned char)((width - n) >> 1);
    memset(buf, ' ', width);
    memcpy(buf + left, s, n);
    buf[width] = 0;
    draw_ascii(col, row, buf, width);
}

static void draw_reverse_ascii(unsigned char col, unsigned char row,
                               const char* s, unsigned char width)
{
    unsigned char i;
    memset(mtx + (unsigned int)row * 40 + col, COL_RED_BLACK, width);
    for (i = 0; i < width; ++i)
        draw_sc_reverse((unsigned char)(col + i), row,
                        asc2sc(s[i] ? s[i] : ' '));
}

/* ==========================================================================
 * projection: polar (dst NM, dir degrees) → pixel (x, y)
 * ========================================================================== */

/* Convert (dst_tenths, dir_degrees, range_nm) to 0..199 pixel coords.
 * Uses fixed-point lookup tables for sin/cos (scaled by 127).             */
static void project(int dst_tenths, unsigned int dir,
                    unsigned int range_nm,
                    unsigned char* out_x, unsigned char* out_y)
{
    int pixel_dist, dx, dy, x, y;

    /* pixel_dist = (dst_nm / range_nm) * SCOPE_RADIUS_PX
     * dst_nm = dst_tenths / 10,  so:
     * pixel_dist = dst_tenths * SCOPE_RADIUS_PX / (range_nm * 10)         */
    if (range_nm == 0) range_nm = 1;
    pixel_dist = (dst_tenths * RING_PX) / ((int)range_nm * 10);
    if (pixel_dist > 199) pixel_dist = 199;

    dir %= 360;
    dx = (pixel_dist * (int)sin127[dir]) / 127;
    dy = (pixel_dist * (int)cos127[dir]) / 127;

    x = SCOPE_C + dx;
    y = SCOPE_C - dy;             /* screen Y axis is inverted             */

    if (x < 0) x = 0;  if (x > 199) x = 199;
    if (y < 0) y = 0;  if (y > 199) y = 199;

    *out_x = (unsigned char)x;
    *out_y = (unsigned char)y;
}

/* ==========================================================================
 * display setup (unchanged)
 * ========================================================================== */

#ifndef HOST_TEST
static void copy_charset(void)
{
    unsigned char p;
    __asm__("sei");
    p = PEEK(0x0001);
    POKE(0x0001, p & 0xFB);
    memcpy(charset, (void*)0xD000, 2048);
    POKE(0x0001, p);
    __asm__("cli");
}
#endif

static void init_video(void)
{
    unsigned char i;
    unsigned int r;
    for (i = 0; i < 25; ++i) {
        r = i; rowbase[i] = r * 320;
    }
    memset(bmp, 0, 8000);
    memset(mtx, COL_GREEN_BLACK, 1000);
    POKE(0xDD00, (PEEK(0xDD00) & 0xFC) | 0x02);   /* VIC bank 1 ($4000)    */
    POKE(0xD018, 0x78);                           /* matrix $5C00, bmp $6000 */
    POKE(0xD011, 0x3B);                           /* hires bitmap on       */
    POKE(0xD016, 0xC8);
    POKE(0xD020, 0); POKE(0xD021, 0);
}

static void init_sprites(void)
{
    unsigned char i;
    memset(MEM(SPR_DATA), 0, 512);
    for (i = 0; i < 8; ++i) {
        build_sprite(i, 0, 1);
        POKE(MATRIX + 0x3F8 + i, SPR_PTRVAL + i);
        POKE(0xD027 + i, SPR_GREEN);
    }
    POKE(0xD015, 0);
    POKE(0xD010, 0); POKE(0xD017, 0); POKE(0xD01D, 0);
    POKE(0xD01B, 0); POKE(0xD01C, 0);
}

static void draw_static_scope(void)
{
    static const unsigned char lbl3 = 0x33, lbl6 = 0x36, lbl9 = 0x39;

    hline(0, 199, 0);  hline(0, 199, 199);
    vline(0, 199, 0);  vline(0, 199, 199);
    circle(SCOPE_C, SCOPE_C, 32);
    circle(SCOPE_C, SCOPE_C, 63);
    circle(SCOPE_C, SCOPE_C, RING_PX);
    hline(98, 102, 100); vline(98, 102, 100);

    draw_sc(13, 8, &lbl3, 1);
    draw_sc(13, 4, &lbl6, 1);
    draw_sc(13, 0, &lbl9, 1);

    draw_centered(TBL_COL, 0, "C64U RADAR", TBL_W);
    memset(bmp + rowbase[1] + (TBL_COL << 3), 0, TBL_W * 8);
    {
        unsigned char bar[TBL_W];
        memset(bar, 0x40, TBL_W);
        draw_sc(TBL_COL, 1, bar, TBL_W);
        draw_sc(TBL_COL, 20, bar, TBL_W);
    }
    draw_ascii(TBL_COL, 23, "MAIN MENU: F1", TBL_W);
}

/* ==========================================================================
 * table + sprites from the in-memory LD blob
 * ========================================================================== */

static void show_status(unsigned char st)
{
    static const char* txt[] =
        { "LINK OK", "LINK STALE", "LINK DOWN", "BAD DATA", "CONNECTING",
          "BAD LOCATION" };
    memset(mtx + 21 * 40 + TBL_COL,
           (st == ST_STALE || st == ST_DOWN || st == ST_BAD || st == ST_LOCATION)
               ? COL_RED_BLACK : COL_GREEN_BLACK,
           TBL_W);
    draw_ascii(TBL_COL, 21, txt[st], TBL_W);
}

static void show_link_down(void)
{
    unsigned char row;
    POKE(0xD015, 0);
    for (row = 2; row <= 19; ++row)
        draw_ascii(TBL_COL, row, "", TBL_W);
    draw_ascii(TBL_COL, 21, "", TBL_W);
    draw_ascii(TBL_COL, 22, "", TBL_W);
    draw_reverse_ascii(6, 12, "  LINK DOWN  ", 13);
    link_down_displayed = 1;
}

static void render_targets(void)
{
    unsigned char count = blob[4];
    unsigned char total = blob[5];
    unsigned char age   = blob[6];
    unsigned char i, x, y;
    const unsigned char* r;
    unsigned char lineA[TBL_W], lineB[TBL_W];
    char tmp[16];

    if (count > MAX_AC) count = MAX_AC;

    draw_centered(TBL_COL, 2, scope_label1, TBL_W);
    draw_centered(TBL_COL, 3, scope_label2, TBL_W);
    if (blob[3] & 0x01) sprintf(tmp, "AGE %3u RNG %3u", age, total);
    else sprintf(tmp, "IN RANGE %3u", total);
    draw_ascii(TBL_COL, 22, tmp, TBL_W);

    for (i = 0; i < count; ++i) {
        r = blob + 8 + (unsigned int)i * REC_SZ;
        x = r[0]; y = r[1];
        build_sprite(i, r[4], r[3] & 0x04);
        POKE(0xD000 + (i << 1), 24 + x - 11);
        POKE(0xD001 + (i << 1), 50 + y - 10);
        POKE(0xD027 + i, SPR_GREEN);
    }
    POKE(0xD015, count ? (unsigned char)((1 << count) - 1) : 0);

    for (i = 0; i < MAX_AC; ++i) {
        memset(lineA, 0x20, TBL_W);
        memset(lineB, 0x20, TBL_W);
        if (i < count) {
            r = blob + 8 + (unsigned int)i * REC_SZ;
            lineA[0] = (unsigned char)('1' + i);
            memcpy(lineA + 1,  r + 6, 8);
            memcpy(lineA + 10, r + 14, 4);
            memcpy(lineB + 1,  r + 18, 5);
            memcpy(lineB + 7,  r + 23, 4);
            lineB[11] = 0x0B;                     /* K */
            lineB[12] = 0x14;                     /* T */
        } else if (i == 0) {
            draw_ascii(TBL_COL, 4, "NO TRAFFIC", TBL_W);
            draw_sc(TBL_COL, 5, lineB, TBL_W);
            continue;
        }
        draw_sc(TBL_COL, 4 + (i << 1), lineA, TBL_W);
        if (i < count)
            draw_sc_reverse(TBL_COL, 4 + (i << 1), lineA[0]);
        draw_sc(TBL_COL, 5 + (i << 1), lineB, TBL_W);
    }
}

/* ==========================================================================
 * adsb.fi data fetch via Meatloaf HTTPS
 * ========================================================================== */

static unsigned char fetch(void)
{
    unsigned char ch = ML_CH;
    int total, i, dst_tenths, dir_int, alt_val, gs_val, track_val;
    int http_status;
    char alt_str[6], gs_str[5];
    unsigned char px, py, flags, track_byte, speed_byte;
    unsigned char valid_count = 0, j;

    /* ---- open Meatloaf connection ---- */
    if (cbm_open(ch, ML_DEV, ML_SA, url_buf) != 0) {
        return ST_DOWN;
    }

    /* ---- build and send the GET request ---- */
    cbm_write(ch, "h user-agent: c64u-radar/1.0\r\n", 31);
    cbm_write(ch, "m get\r\n", 7);
    cbm_write(ch, "s\r\n", 3);

    /* ---- check HTTP response status ---- */
    cbm_write(ch, "status\r\n", 8);
    if (!ml_read_val(ch, j_val, JVAL_SZ)) {
        cbm_close(ch); return ST_DOWN;
    }
    http_status = atoi(j_val);
    if (http_status != 200) {
        cbm_close(ch);
        return (http_status == -1 || http_status == -2) ? ST_DOWN : ST_BAD;
    }

    /* ---- total aircraft count ---- */
    if (!j_int(ch, "/total", &total)) {
        cbm_close(ch); return ST_BAD;
    }
    if (total <= 0) total = 0;
    if (total > MAX_AC) total = MAX_AC;

    blob[0] = MAGIC0; blob[1] = MAGIC1;
    blob[2] = 1;      /* wire version */
    blob[3] = (total > MAX_AC) ? 0x02 : 0;  /* truncated flag if >8 a/c  */
    blob[4] = 0;      /* count -- set after extraction */
    blob[5] = total > 255 ? 255 : (unsigned char)total;
    blob[6] = 0;      /* age */
    blob[7] = 0;

    /* ---- extract each aircraft via JSON Pointer ---- */
    for (i = 0; i < total; ++i) {
        unsigned char* r = blob + 8 + (unsigned int)valid_count * REC_SZ;

        /* Filter: skip ground traffic (alt == "ground" or gs < 40 kt) */
        sprintf(j_ptr, "/ac/%d/alt_baro", i);
        if (j_str(ch, j_ptr, j_val, JVAL_SZ) && is_ground(j_val))
            continue;
        sprintf(j_ptr, "/ac/%d/gs", i);
        if (j_int(ch, j_ptr, &gs_val) && gs_val >= 0 && gs_val < 40)
            continue;

        /* Distance from center (required for position) */
        sprintf(j_ptr, "/ac/%d/dst", i);
        if (!j_str(ch, j_ptr, j_val, JVAL_SZ) ||
            !parse_tenths(j_val, &dst_tenths)) {
            continue;
        }

        /* Bearing from center */
        sprintf(j_ptr, "/ac/%d/dir", i);
        if (!j_int(ch, j_ptr, &dir_int)) continue;
        dir_int %= 360;
        if (dir_int < 0) dir_int += 360;

        /* Project to pixel coords */
        project(dst_tenths, (unsigned int)dir_int, current_range, &px, &py);
        r[0] = px; r[1] = py; r[2] = 3;
        flags = 0;

        /* Track heading */
        sprintf(j_ptr, "/ac/%d/track", i);
        if (j_int(ch, j_ptr, &track_val)) {
            track_byte = (unsigned char)(((track_val % 360) * 256u) / 360);
        } else {
            flags |= 0x04; track_byte = 0;
        }
        r[3] = flags; r[4] = track_byte;

        /* Ground speed text */
        if (gs_val >= 0) {
            speed_byte = (gs_val > 255) ? 255 : (unsigned char)gs_val;
            sprintf(gs_str, "%4d", (gs_val > 9999) ? 9999 : gs_val);
        } else {
            flags |= 0x02; speed_byte = 0;
            gs_str[0] = '-'; gs_str[1] = '-';
            gs_str[2] = ' '; gs_str[3] = ' '; gs_str[4] = 0;
        }
        r[5] = speed_byte;
        r[3] = flags;

        /* Altitude text (fresh read) */
        sprintf(j_ptr, "/ac/%d/alt_baro", i);
        if (!j_str(ch, j_ptr, j_val, JVAL_SZ)) {
            alt_str[0] = '-'; alt_str[1] = '-';
            alt_str[2] = ' '; alt_str[3] = ' '; alt_str[4] = ' '; alt_str[5] = 0;
        } else {
            alt_val = atoi(j_val);
            if (alt_val >= 18000)
                sprintf(alt_str, "FL%03d", alt_val / 100);
            else
                sprintf(alt_str, "%5d", alt_val);
        }
        r[3] = flags;

        /* callsign (8 chars, space-padded, converted to screen code) */
        sprintf(j_ptr, "/ac/%d/flight", i);
        if (!j_str(ch, j_ptr, j_val, JVAL_SZ)) {
            for (j = 0; j < 8; ++j) r[6 + j] = 0x20;
        } else {
            for (j = 0; j < 8; ++j)
                r[6 + j] = j_val[j] ? asc2sc(j_val[j]) : 0x20;
        }

        /* aircraft type code (4 chars, e.g. "B738", scrn code) */
        sprintf(j_ptr, "/ac/%d/t", i);
        if (!j_str(ch, j_ptr, j_val, JVAL_SZ)) {
            for (j = 0; j < 4; ++j) r[14 + j] = 0x20;
        } else {
            for (j = 0; j < 4; ++j)
                r[14 + j] = j_val[j] ? asc2sc(j_val[j]) : 0x20;
        }

        /* altitude text (5 bytes, screen-coded) */
        for (j = 0; j < 5; ++j)
            r[18 + j] = asc2sc(alt_str[j]);

        /* ground speed text (4 bytes, screen-coded) */
        for (j = 0; j < 4; ++j)
            r[23 + j] = asc2sc(gs_str[j]);

        r[27] = 0;                       /* pad byte */
        ++valid_count;
    }

    blob[4] = valid_count;

    cbm_close(ch);

    if (!link_down_displayed) render_targets();
    return ST_OK;
}

#ifndef HOST_TEST
static unsigned char wait_jiffies(unsigned int j)
{
    clock_t t0 = clock();
    while ((unsigned int)(clock() - t0) < j) {
        if (kbhit() && (unsigned char)cgetc() == CH_F1) return 1;
    }
    return 0;
}

static void init_text_video(void)
{
    POKE(0xD015, 0);
    POKE(0xD011, 0x1B);
    POKE(0xD016, 0xC8);
    POKE(0xDD00, (PEEK(0xDD00) & 0xFC) | 0x03);
    POKE(0xD018, 0x17);
    POKE(0xD020, 0); POKE(0xD021, 0);
}

/* ========================================================================== */

int main(void)
{
    copy_charset();
    for (;;) {
        unsigned char status;
        init_text_video();
        setup_location();
        init_video();
        init_sprites();
        draw_static_scope();
        link_down_displayed = 0;
        show_status(ST_WAIT);
        for (;;) {
            status = fetch();
            if (status == ST_EXIT) break;
            if (status == ST_DOWN) {
                if (!link_down_displayed) show_link_down();
            } else {
                if (link_down_displayed) {
                    init_video();
                    init_sprites();
                    draw_static_scope();
                    render_targets();
                    link_down_displayed = 0;
                }
                show_status(status);
            }
            if (wait_jiffies(POLL_JIF)) break;
        }
    }
    return 0;
}
#endif /* !HOST_TEST */
