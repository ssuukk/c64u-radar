/* ==========================================================================
 * C64U RADAR -- public ADS-B scope for the Commodore 64 Ultimate
 *
 * Companion to the standalone C64 Ultimate Radar Python server. The server
 * TLS, JSON, trig and projection; this program opens one TCP socket per
 * poll through the Ultimate Command Interface network target, reads a
 * <=232-byte fixed-width blob, and moves sprites.
 *
 *   feed:   connect FEED_HOST:6464, optionally send MR2 location request,
 *           then read until remote close
 *   blob:   8-byte header "LD",ver,flags,count,total,age,0
 *           + count * 28-byte records (see ldv_c64_feed.py docstring)
 *   text fields arrive as C64 screen codes -- blitted straight to bitmap
 *
 * Display: hires bitmap 320x200, VIC bank 1
 *   $5A00-$5BFF eight independent 64-byte sprite patterns
 *   $5C00 screen matrix (color cells, $D0 = light green on black)
 *   $6000 bitmap
 *   sprite pointers $68-$6F at $5FF8
 *   scope = left 200x200 px, center (100,100), 9nm ring r=95
 *   table = char columns 26..39
 *
 * Build:  make            (cl65 -t c64, vendored ultimateii-dos-lib, GPL-3)
 * Run:    select the PRG in the Ultimate file browser and run it
 * Needs:  "Command Interface" enabled in the Ultimate menu, and the C64U
 *         on the same LAN as the server computer. The startup menu can
 *         replace the initial FEED_HOST value without rebuilding.
 *
 * Known v1 limits: no sweep animation, no dead reckoning between polls
 * (track/gs bytes are already in the record for it), blips are all one
 * color until the feed starts banding altitudes.
 * ========================================================================== */

#include <string.h>
#include <stdio.h>
#include <time.h>
#include <peekpoke.h>
#ifndef HOST_TEST
#include <conio.h>
#include <c64.h>
#include <cbm.h>
#endif
#include "ultimate_lib.h"

/* ---- configuration ------------------------------------------------------ */
#define FEED_HOST   "0.0.0.0"         /* public build requires menu setup   */
#define FEED_PORT   6464
#define POLL_JIF    600               /* 10 s between polls (jiffies)       */
#define REPLY_JIF   1200              /* 20 s: first new location may fetch */
#define VERSION_STRING "V0.1"         /* main menu only; scope title has no
                                          room for it (14-char column)      */

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
#define FEED_REQ_SZ 48
#define BLOB_SZ     (8 + MAX_AC * REC_SZ)

/* link states */
enum { ST_OK, ST_STALE, ST_DOWN, ST_BAD, ST_WAIT, ST_LOCATION, ST_EXIT };

/* main-menu server-address label: where the current feed_host came from */
enum { SERVER_UNSET, SERVER_AUTO, SERVER_MANUAL };

/* Host-test hook: build with -DHOST_TEST (see host_test/) to compile the
 * drawing/parsing logic natively against a fake 64K RAM and render a
 * pixel-true preview PNG. The C64 build is the #else path, unchanged.   */
#ifdef HOST_TEST
unsigned char host_ram[65536];
#  define MEM(a) (host_ram + (a))
#else
#  define MEM(a) ((unsigned char*)(unsigned int)(a))
#endif

static unsigned char* const bmp = MEM(BITMAP);
static unsigned char* const mtx = MEM(MATRIX);

/* Upper RAM workspace. Keeping it out of the loaded program leaves the
 * fixed $5A00 sprite block untouched as menu/network features grow.        */
static unsigned char* const charset = MEM(0x8000);
static unsigned int* const rowbase = (unsigned int*)MEM(0x8800);
static const unsigned char bmask[8] =
    { 0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01 };

static unsigned char* const blob = MEM(0x8900);
static unsigned char sock;
static char* const feed_request = (char*)MEM(0x8A00);
static char* const feed_host = (char*)MEM(0x8A30);
static char* const scope_label1 = (char*)MEM(0x8A40);
static char* const scope_label2 = (char*)MEM(0x8A50);
static unsigned char link_down_displayed;

/* Server-address mailbox shared with the radar Python server through the
 * Ultimate REST API (machine:readmem / machine:writemem). The server only
 * writes offsets +5..+22 and only after verifying the magic, so it can never
 * touch a machine that is not running this program. The block also survives
 * reset/relaunch, which restores the last IP without the server.
 *   +0..3 magic "MR2M" (ASCII bytes, kept numeric: cc65 letter literals are
 *   PETSCII)  +4 version  +5 ip length  +6..21 ip text  +22 checksum        */
#define MAILBOX_ADDR 0x8AC0
static unsigned char* const mailbox = MEM(MAILBOX_ADDR);
static unsigned char server_source;      /* SERVER_UNSET/AUTO/MANUAL        */
static unsigned char cs_hotkey;          /* raw byte for Commodore+S, or 0  */

static unsigned char set_feed_host(const char* text);
static unsigned char find_commodore_key(unsigned char unshifted_code);

static unsigned char mailbox_checksum(void)
{
    unsigned char sum = 0xA5, i, len = mailbox[5];
    sum ^= len;
    for (i = 0; i < len && i < 15; ++i) sum ^= mailbox[6 + i];
    return sum;
}

static unsigned char mailbox_magic_ok(void)
{
    return mailbox[0] == 0x4D && mailbox[1] == 0x52 &&
           mailbox[2] == 0x32 && mailbox[3] == 0x4D && mailbox[4] == 1;
}

/* Mirror the active server IP so the server sees the radar running and the
 * address survives reset.                                                   */
static void mailbox_store(const char* ip)
{
    unsigned char i, len = (unsigned char)strlen(ip);
    mailbox[0] = 0x4D; mailbox[1] = 0x52;
    mailbox[2] = 0x32; mailbox[3] = 0x4D;
    mailbox[4] = 1;
    mailbox[5] = len;
    for (i = 0; i < 16; ++i)
        mailbox[6 + i] = i < len ? (unsigned char)ip[i] : 0;
    mailbox[22] = mailbox_checksum();
}

/* Adopt a valid mailbox IP that differs from the active one. Returns 1 when
 * feed_host changed. Torn server writes fail the checksum and are skipped.
 * A manually entered address is sticky for the rest of this run: the server
 * would otherwise re-push its own address over the user's deliberate choice
 * every poll cycle.                                                        */
static unsigned char mailbox_poll(void)
{
    char ip[16];
    unsigned char i, len = mailbox[5];
    if (server_source == SERVER_MANUAL) return 0;
    if (!mailbox_magic_ok()) return 0;
    if (len < 7 || len > 15) return 0;
    if (mailbox[22] != mailbox_checksum()) return 0;
    for (i = 0; i < len; ++i) ip[i] = (char)mailbox[6 + i];
    ip[len] = 0;
    if (!strcmp(ip, feed_host)) return 0;
    if (!set_feed_host(ip)) return 0;
    server_source = SERVER_AUTO;
    return 1;
}

static void init_config(void)
{
    strcpy(feed_host, FEED_HOST);
    feed_request[0] = 0;
    scope_label1[0] = 0;
    scope_label2[0] = 0;
    /* A mailbox that survived reset restores the last server IP (mailbox_poll
       sets server_source = SERVER_AUTO on success); otherwise plant a fresh
       mailbox so the server knows the radar is running.                    */
    if (!mailbox_poll())
        mailbox_store(feed_host);
    /* 0x53 is the numeric low-PETSCII code the KERNAL reports for the
       unshifted S key -- NOT the char literal 'S', which cc65 compiles to
       high PETSCII for on-screen display and would never match here. */
    cs_hotkey = find_commodore_key(0x53);
    POKE(0x0291, 128);   /* disable the SHIFT+Commodore charset toggle:    */
                         /* C=+S is now an app hotkey, not a display switch */
}

/* ==========================================================================
 * startup location request
 * ========================================================================== */


/* Validate a decimal coordinate without linking the C64 floating-point
 * library. Values at the exact pole/date-line limit may only have zeroes
 * after the decimal point.                                                  */
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

static unsigned char set_position_request(const char* latitude,
                                          const char* longitude)
{
    if (!valid_coordinate(latitude, 90) || !valid_coordinate(longitude, 180))
        return 0;
    if (strlen(latitude) + strlen(longitude) + 13 > FEED_REQ_SZ)
        return 0;
    /* Lowercase source letters compile to the ASCII-compatible PETSCII
       bytes required by the network protocol. */
    sprintf(feed_request, "mr2 pos %s %s 9\n", latitude, longitude);
    strncpy(scope_label1, latitude, 14); scope_label1[14] = 0;
    strncpy(scope_label2, longitude, 14); scope_label2[14] = 0;
    return 1;
}

static unsigned char set_icao_request(const char* code)
{
    unsigned char i;
    if (strlen(code) != 4) return 0;
    for (i = 0; i < 4; ++i)
        if (code[i] < 'A' || code[i] > 'Z') return 0;
    sprintf(feed_request, "mr2 icao %c%c%c%c 9\n",
            code[0] & 0x7F, code[1] & 0x7F,
            code[2] & 0x7F, code[3] & 0x7F);
    strcpy(scope_label1, code);
    scope_label2[0] = 0;
    return 1;
}

static unsigned char set_feed_host(const char* text)
{
    unsigned char groups = 0, digits = 0;
    unsigned int value = 0;
    const char* p = text;
    for (;;) {
        if (*p >= '0' && *p <= '9') {
            if (++digits > 3) return 0;
            value = value * 10 + (unsigned int)(*p++ - '0');
            if (value > 255) return 0;
        } else if (*p == '.' || !*p) {
            if (!digits || ++groups > 4) return 0;
            if (!*p) break;
            digits = 0; value = 0; ++p;
        } else return 0;
    }
    if (groups != 4 || strlen(text) > 15) return 0;
    strcpy(feed_host, text);
    return 1;
}

static unsigned char key_to_petscii(unsigned char c)
{
    /* KERNAL keyboard input uses low PETSCII for unshifted letters, while
       cc65 uppercase C literals use high PETSCII. Normalize to the latter. */
    if (c >= 0x41 && c <= 0x5A) c |= 0x80;
    else if (c >= 0x61 && c <= 0x7A)
        c = (unsigned char)((c - 0x20) | 0x80);
    return c;
}

/* KERNAL ROM is banked in at $E000-$FFFF by default (this program never
 * hides it, unlike the brief CHAREN toggle in copy_charset()), so its fixed
 * keyboard decode tables can be read directly instead of guessing a byte
 * value for a modified key. Locate the physical key whose *unshifted* code
 * is `unshifted_code` in the $EB81 table, then return the same key's entry
 * in the $EC03 Commodore-key table. This works across KERNAL ROM revisions.
 * The lookup is a plain PEEK scan, so it also runs correctly against the
 * harness's fake RAM when a test plants matching bytes there.               */
#define KEYTAB_UNSHIFT   0xEB81
#define KEYTAB_COMMODORE 0xEC03
#define KEYTAB_KEYS      64
static unsigned char find_commodore_key(unsigned char unshifted_code)
{
    unsigned char i;
    for (i = 0; i < KEYTAB_KEYS; ++i)
        if (PEEK(KEYTAB_UNSHIFT + i) == unshifted_code)
            return PEEK(KEYTAB_COMMODORE + i);
    return 0;
}

#ifndef HOST_TEST
/* The menu uses the lowercase/uppercase character set. cc65's high-PETSCII
 * uppercase and low-PETSCII lowercase literals therefore display directly. */
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

/* Draw whichever of the three server-address states currently applies.
 * Label and value sit on separate rows because "AUTO DISCOVERED SERVER AT:"
 * plus a worst-case 15-character IP would overflow the 40-column screen and
 * wrap onto the next line if crammed onto one.                             */
static void draw_server_status(void)
{
    switch (server_source) {
        case SERVER_MANUAL:
            menu_putsxy(4, 12, "USER ENTERED SERVER IP:");
            menu_putsxy(4, 13, feed_host);
            break;
        case SERVER_AUTO:
            menu_putsxy(4, 12, "Auto discovered server at:");
            menu_putsxy(4, 13, feed_host);
            break;
        default:
            menu_putsxy(4, 12, "SEARCHING FOR SERVER...");
            break;
    }
}

static void setup_location(void)
{
    char latitude[17], longitude[17], icao[5], address[16];
    unsigned char choice, raw;
    textcolor(COLOR_LIGHTGREEN);
    bgcolor(COLOR_BLACK);
    bordercolor(COLOR_BLACK);
    feed_request[0] = 0;
    scope_label1[0] = 0;
    scope_label2[0] = 0;

    for (;;) {
        clrscr();
        menu_putsxy(13, 2, "C64U RADAR " VERSION_STRING);
        menu_putsxy(1, 4, "Choose an option to center your scope:");
        menu_putsxy(4, 6, "1. CENTER ON LAT/LONG");
        menu_putsxy(4, 8, "2. CENTER ON ICAO AIRPORT CODE");
        draw_server_status();
        menu_putsxy(4, 15, "C= + S CHANGES SERVER ADDRESS");
        menu_putsxy(6, 20, "Traffic data source: adsb.fi");
        menu_putsxy(13, 23, "levimaaia.com");
        menu_putsxy(9, 24, "youtube.com/@levimaaia");
        /* Wait for a key while watching the mailbox: the server pushes its
           IP through the Ultimate REST API while this menu idles. The raw
           byte is checked against the Commodore+S hotkey before any PETSCII
           normalization, since normalizing could shift it into a different
           code and hide the match.                                         */
        raw = 0;
        for (;;) {
            if (kbhit()) { raw = (unsigned char)cgetc(); break; }
            if (mailbox_poll()) break;    /* redraw with the new server line */
        }
        if (!raw) continue;

        if (cs_hotkey && raw == cs_hotkey) {
            clrscr();
            menu_putsxy(6, 4, "ENTER SERVER IP ADDRESS");
            menu_putsxy(4, 8, "IP: ");
            read_input(address, 15);
            if (set_feed_host(address)) {
                server_source = SERVER_MANUAL;
                mailbox_store(feed_host);
            } else {
                menu_putsxy(8, 12, "INVALID IP ADDRESS");
                menu_putsxy(8, 14, "PRESS A KEY");
                cgetc();
            }
            continue;
        }

        choice = key_to_petscii(raw);
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

/* 3x5 digit masks for 1..8. The bits are cut out of a solid diamond.      */
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

/* Stem endpoints for N, NE, E, SE, S, SW, W, NW. Each direction exposes
 * five pixels beyond the radius-five diamond.                              */
static const unsigned char dir_x[8] = { 11, 18, 21, 18, 11, 4, 1, 4 };
static const unsigned char dir_y[8] = {  0,  3, 10, 17, 20,17,10, 3 };

/* ==========================================================================
 * low-level drawing
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


/* Sprite-local drawing. A pattern is 3 bytes x 21 rows plus byte 63.      */
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

/* Build one numbered marker. Track-byte sectors are 32 units (45 degrees),
 * rounded to the nearest of the eight compass directions.                 */
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

    /* Solid radius-five diamond centered at the target position. */
    for (y = 5; y <= 15; ++y) {
        half = (unsigned char)(5 - (y > 10 ? y - 10 : 10 - y));
        for (x = (unsigned char)(11 - half); x <= (unsigned char)(11 + half); ++x)
            spr_plot(p, x, y);
    }

    /* Cut the target number out in black for CRT-readable contrast. */
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

/* blit screen-code glyphs into the bitmap at char cell (col,row)           */
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

/* One reverse-video screen-code cell in bitmap mode: green field, black
 * glyph. There is no VIC reverse flag in hires bitmap mode.                */
static void draw_sc_reverse(unsigned char col, unsigned char row,
                            unsigned char sc)
{
    unsigned char k;
    unsigned char* dst = bmp + rowbase[row] + (col << 3);
    const unsigned char* g = charset + ((unsigned int)sc << 3);
    for (k = 0; k < 8; ++k) dst[k] = (unsigned char)~g[k];
}

static unsigned char asc2sc(char c)
{
    unsigned char o = (unsigned char)c;
    if (o >= 0xC1 && o <= 0xDA) return o & 0x1F;
    if (o >= 0x41 && o <= 0x5A) return o - 0x40;
    if (o >= 0x61 && o <= 0x7A) return o - 0x60;
    if (o >= 0x20 && o <= 0x3F) return o;
    return 46;                                    /* '.' */
}

/* draw PETSCII/ASCII text, space-padded to `width` screen-code cells       */
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
 * display setup
 * ========================================================================== */

#ifndef HOST_TEST   /* host harness loads the chargen ROM file instead */
static void copy_charset(void)
{
    unsigned char p;
    __asm__("sei");
    p = PEEK(0x0001);
    POKE(0x0001, p & 0xFB);                       /* char ROM in at $D000  */
    memcpy(charset, (void*)0xD000, 2048);         /* uppercase/gfx set     */
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
    POKE(0xD015, 0);                              /* all off until data    */
    POKE(0xD010, 0); POKE(0xD017, 0); POKE(0xD01D, 0);
    POKE(0xD01B, 0); POKE(0xD01C, 0);
}

static void draw_static_scope(void)
{
    static const unsigned char lbl3 = 0x33, lbl6 = 0x36, lbl9 = 0x39;

    /* bezel + rings (3/6/9 nm) + center cross */
    hline(0, 199, 0);  hline(0, 199, 199);
    vline(0, 199, 0);  vline(0, 199, 199);
    circle(SCOPE_C, SCOPE_C, 32);
    circle(SCOPE_C, SCOPE_C, 63);
    circle(SCOPE_C, SCOPE_C, RING_PX);
    hline(98, 102, 100); vline(98, 102, 100);

    /* ring labels just right of 12 o'clock, like the panel */
    draw_sc(13, 8, &lbl3, 1);
    draw_sc(13, 4, &lbl6, 1);
    draw_sc(13, 0, &lbl9, 1);

    /* right column chrome */
    draw_centered(TBL_COL, 0, "C64U RADAR", TBL_W);
    memset(bmp + rowbase[1] + (TBL_COL << 3), 0, TBL_W * 8);
    {
        unsigned char bar[TBL_W];
        memset(bar, 0x40, TBL_W);                 /* horizontal line char  */
        draw_sc(TBL_COL, 1, bar, TBL_W);
        draw_sc(TBL_COL, 20, bar, TBL_W);
    }
    draw_ascii(TBL_COL, 23, "MAIN MENU: F1", TBL_W);
}

/* ==========================================================================
 * table + sprites from a parsed blob
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

    /* Keep the previous frame visible while positions/patterns are updated. */
    for (i = 0; i < count; ++i) {
        r = blob + 8 + (unsigned int)i * REC_SZ;
        x = r[0]; y = r[1];
        build_sprite(i, r[4], r[3] & 0x04);
        POKE(0xD000 + (i << 1), 24 + x - 11);     /* center local 11,10    */
        POKE(0xD001 + (i << 1), 50 + y - 10);
        POKE(0xD027 + i, SPR_GREEN);
    }
    POKE(0xD015, count ? (unsigned char)((1 << count) - 1) : 0);

    /* table rows: two per target */
    for (i = 0; i < MAX_AC; ++i) {
        memset(lineA, 0x20, TBL_W);
        memset(lineB, 0x20, TBL_W);
        if (i < count) {
            r = blob + 8 + (unsigned int)i * REC_SZ;
            lineA[0] = (unsigned char)('1' + i);  /* sprite/list number    */
            memcpy(lineA + 1,  r + 6, 8);         /* callsign              */
            memcpy(lineA + 10, r + 14, 4);        /* type, col 9 is space  */
            memcpy(lineB + 1,  r + 18, 5);        /* alt                   */
            memcpy(lineB + 7,  r + 23, 4);        /* gs                    */
            lineB[11] = 0x0B;                     /* K                     */
            lineB[12] = 0x14;                     /* T                     */
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
 * network
 * ========================================================================== */

/* read until the server closes (return 0) or we have the whole blob.
 * uii_socketread: 0 = closed, -1 = nothing yet, >0 = bytes at uii_data[2] */
static int read_blob(void)
{
    unsigned int total = 0, needed = 8;
    int n;
    clock_t t0 = clock();

    while (total < needed) {
#ifndef HOST_TEST
        if (kbhit() && (unsigned char)cgetc() == CH_F1) return -3;
#endif
        n = uii_socketread(sock, 512);
        if (n == 0) break;
        if (n > 0) {
            if (total + n > BLOB_SZ) n = BLOB_SZ - total;
            if (n > 0) {
                memcpy(blob + total, uii_data + 2, (size_t)n);
                total += n;
            }
            if (total >= 8) {
                if (blob[0] != MAGIC0 || blob[1] != MAGIC1 || blob[4] > MAX_AC)
                    return -2;
                needed = 8 + (unsigned int)blob[4] * REC_SZ;
            }
        }
        if ((unsigned int)(clock() - t0) > REPLY_JIF) return -1;
    }
    return (total >= needed) ? (int)total : -1;
}

static unsigned char fetch(void)
{
    int r;
    sock = uii_tcpconnect(feed_host, FEED_PORT);
    if (!uii_success()) return ST_DOWN;
    if (feed_request[0]) {
        uii_socketwrite(sock, feed_request);
        if (!uii_success()) {
            uii_socketclose(sock);
            return ST_DOWN;
        }
    }
    r = read_blob();
    uii_socketclose(sock);
    if (r == -3) return ST_EXIT;
    if (r == -2) return ST_BAD;
    if (r < 0)  return ST_DOWN;
    if (blob[3] & 0x04) return ST_LOCATION;
    if (!link_down_displayed) render_targets();
    return (blob[3] & 0x01) ? ST_STALE : ST_OK;
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
    POKE(0xD018, 0x17);                           /* lowercase/uppercase ROM */
    POKE(0xD020, 0); POKE(0xD021, 0);
}

/* ========================================================================== */

int main(void)
{
    init_config();
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
