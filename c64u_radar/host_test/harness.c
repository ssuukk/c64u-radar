/* Builds c64u_radar.c natively against fake RAM + a mocked Meatloaf IEC
 * device, drives one init + fetch cycle with synthetic adsb.fi JSON data,
 * and dumps the bitmap, sprite block and VIC registers for render_preview.py.
 *
 *   cc -DHOST_TEST -I. -I.. -o harness harness.c   (from host_test/)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward declarations of mocked Meatloaf IEC functions */
unsigned char cbm_open(unsigned char lfn, unsigned char device,
                       unsigned char secaddr, const char* name);
unsigned char cbm_close(unsigned char lfn);
unsigned char cbm_write(unsigned char lfn, const void* buffer,
                        unsigned char size);
unsigned char cbm_read(unsigned char lfn, void* buffer, unsigned char size);

#include "../c64u_radar.c"

/* ---- fake RAM + registers ---- */
void host_poke(unsigned addr, unsigned char v) { host_ram[addr & 0xFFFF] = v; }
unsigned char host_peek(unsigned addr)         { return host_ram[addr & 0xFFFF]; }

/* ---- fake jiffies ---- */
static long jiffies = 0;
clock_t host_clock(void) { return (clock_t)jiffies++; }

/* =========================================================================
 * Mocked Meatloaf IEC device
 *
 * We intercept cbm_open/write/read/close to simulate a Meatloaf doing
 * HTTPS to adsb.fi.  The mock processes commands the same way the real
 * firmware would, returning JSON Pointer results from a synthetic dataset.
 * ========================================================================= */

/* Synthentic aircraft dataset (matches original preview_blob's 5 AC) */
#define MOCK_AC_COUNT 5
static const struct {
    const char*  flight;       /* callsign               */
    const char*  ac_type;      /* ICAO type code          */
    int          alt_baro;     /* feet; -1 = "ground"     */
    int          gs;           /* knots; -1 = missing      */
    int          track;         /* deg true; -1 = missing   */
    int          dst_tenths;   /* NM * 10                  */
    int          dir;          /* deg true bearing          */
} mock_aircraft[MOCK_AC_COUNT] = {
    { "FIN50F ", "A359", 37000, 490, 273, 57, 310 },
    { "BAW217 ", "B772", 35000, 455, 280, 63, 305 },
    { "SAS987 ", "A20N", 31000, 420, 285, 71, 300 },
    { "DLH456 ", "B748", 34000, 440, 290, 78, 295 },
    { "RYR12  ", "B738", 28000, 380, 295, 85, 290 },
};

/* Mock command-response state */
#define MOCK_BUF_SZ 256
static char mock_cmd_buf[MOCK_BUF_SZ];    /* last command received     */
static unsigned char mock_cmd_len;        /* bytes in cmd buffer       */
static char mock_resp_buf[MOCK_BUF_SZ];   /* pending response data     */
static unsigned char mock_resp_len;       /* bytes ready to read        */
static unsigned char mock_resp_pos;       /* read position              */
static int mock_http_status;              /* the "status" we'll return */
static unsigned char mock_initialized;    /* "s" command was sent       */
static char mock_last_url[128];           /* URL from cbm_open          */

unsigned char cbm_open(unsigned char lfn, unsigned char device,
                       unsigned char secaddr, const char* name)
{
    (void)lfn; (void)device; (void)secaddr;
    strncpy(mock_last_url, name, sizeof(mock_last_url) - 1);
    mock_last_url[sizeof(mock_last_url) - 1] = 0;
    mock_cmd_len = 0;
    mock_resp_len = 0;
    mock_resp_pos = 0;
    mock_http_status = 200;
    mock_initialized = 0;
    return 0;   /* success */
}

unsigned char cbm_close(unsigned char lfn)
{
    (void)lfn;
    return 0;
}

/* Prepare a response string for cbm_read to serve back. */
static void mock_queue_resp(const char* s)
{
    unsigned char len = (unsigned char)strlen(s);
    if (len > sizeof(mock_resp_buf) - 1) len = sizeof(mock_resp_buf) - 1;
    memcpy(mock_resp_buf, s, len);
    mock_resp_len = len;
    mock_resp_pos = 0;
}

/* Process a complete command line that arrived via cbm_write. */
static void mock_process_command(const char* cmd, unsigned char len)
{
    if (len == 0) return;

    /* "m get" — just acknowledge */
    if (!strncmp(cmd, "m get", 5) && (len == 5 || cmd[5]=='\r'))
        return;

    /* "s" — send request, JSON body is now ready for "j" queries */
    if (!strncmp(cmd, "s", 1) && (len == 1 || cmd[1]=='\r')) {
        mock_initialized = 1;
        return;
    }

    /* "status" — respond with HTTP status code */
    if (!strncmp(cmd, "status", 6)) {
        char tmp[8];
        sprintf(tmp, "%d\r", mock_http_status);
        mock_queue_resp(tmp);
        return;
    }

    /* "h ..." — header, ignore */
    if (!strncmp(cmd, "h ", 2))
        return;

    /* "j /total" — total aircraft count */
    if (!strncmp(cmd, "j /total", 8)) {
        char tmp[8];
        sprintf(tmp, "%d\r", MOCK_AC_COUNT);
        mock_queue_resp(tmp);
        return;
    }

    /* "j /ac/N/dst" — distance in NM */
    if (!strncmp(cmd, "j /ac/", 6)) {
        int idx = atoi(cmd + 6);
        const char* field_end = cmd + 6;
        /* find the field name after the number */
        while (*field_end && *field_end >= '0' && *field_end <= '9') ++field_end;
        if (*field_end == '/') ++field_end;  /* skip slash */

        if (idx >= 0 && idx < MOCK_AC_COUNT) {
            if (!strncmp(field_end, "dst", 3)) {
                char tmp[8];
                sprintf(tmp, "%d.%d\r",
                        mock_aircraft[idx].dst_tenths / 10,
                        mock_aircraft[idx].dst_tenths % 10);
                mock_queue_resp(tmp);
                return;
            }
            if (!strncmp(field_end, "dir", 3)) {
                char tmp[8];
                sprintf(tmp, "%d\r", mock_aircraft[idx].dir);
                mock_queue_resp(tmp);
                return;
            }
            if (!strncmp(field_end, "track", 5)) {
                char tmp[8];
                sprintf(tmp, "%d\r", mock_aircraft[idx].track);
                mock_queue_resp(tmp);
                return;
            }
            if (!strncmp(field_end, "gs", 2)) {
                char tmp[8];
                sprintf(tmp, "%d\r", mock_aircraft[idx].gs);
                mock_queue_resp(tmp);
                return;
            }
            if (!strncmp(field_end, "alt_baro", 8)) {
                if (mock_aircraft[idx].alt_baro < 0) {
                    mock_queue_resp("ground\r");
                } else {
                    char tmp[16];
                    sprintf(tmp, "%d\r", mock_aircraft[idx].alt_baro);
                    mock_queue_resp(tmp);
                }
                return;
            }
            if (!strncmp(field_end, "flight", 6)) {
                char tmp[12];
                /* return the raw flight text, space-padded, without CR --
                 * but ml_read_val stops on \r, so include it. */
                sprintf(tmp, "%s\r", mock_aircraft[idx].flight);
                mock_queue_resp(tmp);
                return;
            }
            if (!strncmp(field_end, "t", 1)) {
                char tmp[8];
                sprintf(tmp, "%s\r", mock_aircraft[idx].ac_type);
                mock_queue_resp(tmp);
                return;
            }
        }
    }

    /* Unrecognized command — return "0\r" as a safe default */
    mock_queue_resp("0\r");
}

unsigned char cbm_write(unsigned char lfn, const void* buffer,
                        unsigned char size)
{
    unsigned char i;
    const char* data = (const char*)buffer;
    (void)lfn;

    for (i = 0; i < size; ++i) {
        if (data[i] == '\r' || data[i] == '\n') {
            if (mock_cmd_len > 0) {
                mock_cmd_buf[mock_cmd_len] = 0;
                mock_process_command(mock_cmd_buf, mock_cmd_len);
                mock_cmd_len = 0;
            }
        } else {
            if (mock_cmd_len < sizeof(mock_cmd_buf) - 1)
                mock_cmd_buf[mock_cmd_len++] = data[i];
        }
    }
    return size;
}

unsigned char cbm_read(unsigned char lfn, void* buffer, unsigned char size)
{
    unsigned char i = 0;
    char* dst = (char*)buffer;
    (void)lfn;

    while (i < size && mock_resp_pos < mock_resp_len) {
        dst[i++] = mock_resp_buf[mock_resp_pos++];
    }
    return i;
}

/* =========================================================================
 * host copy_charset: load the real 901225-01 ROM, uppercase set
 * ========================================================================= */
static void copy_charset(void)
{
    unsigned char p, y;
    FILE* f = fopen("../chargen.bin", "rb");
    if (!f) {
        memset(charset, 0, 2048);
        for (p = 0; p < 8; ++p) {
            for (y = 0; y < 5; ++y)
                charset[((unsigned int)('1' + p) << 3) + 1 + y]
                    = (unsigned char)(digit_rows[p][y] << 3);
        }
        fprintf(stderr, "chargen.bin missing; using synthetic digit glyphs\n");
        return;
    }
    if (fread(charset, 1, 2048, f) != 2048) {
        fprintf(stderr, "chargen.bin truncated\n"); exit(1);
    }
    fclose(f);
}

/* =========================================================================
 * Helper: check a sprite pixel at a known offset within slot's 64-byte block
 * ========================================================================= */
static int sprite_pixel(unsigned char slot, unsigned char x, unsigned char y)
{
    unsigned int off = SPR_DATA + (unsigned int)slot * 64
                     + (unsigned int)y * 3 + (x >> 3);
    return (host_ram[off] & (0x80 >> (x & 7))) != 0;
}

static unsigned long sprite_hash(unsigned char slot)
{
    unsigned int i;
    unsigned long h = 2166136261UL;
    for (i = 0; i < 64; ++i) {
        h ^= host_ram[SPR_DATA + (unsigned int)slot * 64 + i];
        h *= 16777619UL;
    }
    return h;
}

/* =========================================================================
 * main test harness
 * ========================================================================= */
int main(void)
{
    unsigned char st, i, d, other, k, px, py;
    unsigned int stem_pixels;
    int mdx, mdy;
    unsigned char* cell;
    unsigned char* glyph;
    unsigned long digit_hash[8];
    FILE* f;

    /* ---- static tests (no networking needed) ---- */

    /* PETSCII / screen code conversions (same as original) */
    if (key_to_petscii(0x4B) != 0xCB || key_to_petscii(0x53) != 0xD3 ||
        key_to_petscii(0x42) != 0xC2 || key_to_petscii(0x41) != 0xC1 ||
        asc2sc((char)0xC1) != 1 || asc2sc((char)0xDA) != 26) {
        fprintf(stderr, "PETSCII keyboard normalization failed\n"); return 1;
    }

    /* Coordinate validation (no floating point needed) */
    /* Coordinate validation */
    /* These must succeed: */
    if (!valid_coordinate("39.117210", 90) || !valid_coordinate("0", 180) ||
        !valid_coordinate("-33.867", 90) || !valid_coordinate("151.2093", 180) ||
        !valid_coordinate("51.4775", 90) || !valid_coordinate("-0.4614", 180)) {
        fprintf(stderr, "valid coordinate rejected\n"); return 1;
    }
    /* These must fail: */
    if (valid_coordinate("90.1", 90) || valid_coordinate("999999999999999", 90) ||
        valid_coordinate("91", 90) || valid_coordinate("-91", 90) ||
        valid_coordinate("180.01", 180) || valid_coordinate("-180.001", 180) ||
        valid_coordinate("N34", 90) || valid_coordinate("", 90)) {
        fprintf(stderr, "coordinate validation failed\n"); return 1;
    }

    /* ICAO validation and airport lookup */
    if (is_icao("")) return 1;
    if (is_icao("EGL")) return 1;
    if (is_icao("EGLLL")) return 1;
    if (!is_icao("EGLL")) return 1;
    if (!is_icao("KJFK")) return 1;

    /* Known airport lookup */
    {
        int32_t alat, alon;
        if (!find_airport("EGLL", &alat, &alon)) {
            fprintf(stderr, "EGLL not found in airports table\n"); return 1;
        }
        /* Heathrow is roughly 51.48N, -0.46W ± 0.01° from OurAirports data */
        if (alat < 51400000 || alat > 51550000 ||
            alon < -470000 || alon > -450000) {
            fprintf(stderr, "EGLL lookup gave wrong coords: alat=%+ld alon=%+ld\n",
                    (long)alat, (long)alon); return 1;
        }
        if (!find_airport("KJFK", &alat, &alon)) {
            fprintf(stderr, "KJFK not found in airports table\n"); return 1;
        }
        if (find_airport("ZZZZ", &alat, &alon)) {
            fprintf(stderr, "bogus code should not be found\n"); return 1;
        }
    }

    /* str_cpy */
    {
        char buf[20];
        memset(buf, 'X', 20);
        str_cpy(buf, "hello", 20);
        if (strcmp(buf, "hello")) {
            fprintf(stderr, "str_cpy failed: %s\n", buf); return 1;
        }
        str_cpy(buf, "this is a very long string that exceeds buffer", 10);
        if (strncmp(buf, "this is a", 9) || buf[9] != 0) {
            fprintf(stderr, "str_cpy truncation failed: %s\n", buf); return 1;
        }
    }

    /* set_position_request builds correct URL */
    scope_label1[0] = 0; scope_label2[0] = 0; url_buf[0] = 0;
    if (!set_position_request("39.117210", "-94.635600")) {
        fprintf(stderr, "valid position rejected\n"); return 1;
    }
    if (strcmp(scope_label1, "39.117210")) {
        fprintf(stderr, "scope label1 mismatch: %s\n", scope_label1); return 1;
    }
    if (strcmp(scope_label2, "-94.635600")) {
        fprintf(stderr, "scope label2 mismatch: %s\n", scope_label2); return 1;
    }
    if (strncmp(url_buf, ADSB_BASE "/lat/39.117210/lon/-94.635600/dist/", 47)) {
        fprintf(stderr, "url_buf incorrect: %s\n", url_buf); return 1;
    }

    /* set_icao_request builds correct URL */
    if (!set_icao_request("EGLL")) {
        fprintf(stderr, "EGLL icao lookup failed\n"); return 1;
    }
    if (strcmp(scope_label1, "EGLL")) {
        fprintf(stderr, "ICAO label mismatch: %s\n", scope_label1); return 1;
    }
    if (scope_label2[0]) {
        fprintf(stderr, "ICAO scope_label2 should be empty\n"); return 1;
    }
    if (strncmp(url_buf, ADSB_BASE "/lat/51.47", 30)) {
        fprintf(stderr, "EGLL URL mismatch: %s\n", url_buf); return 1;
    }

    /* Invalid positions must be rejected */
    if (set_position_request("90.1", "0") ||
        set_position_request("0", "-180.01") ||
        set_position_request("999999999999999", "0") ||
        set_position_request("N34", "-119") ||
        set_icao_request("JFK")) {    /* 3-letter code */
        fprintf(stderr, "invalid location accepted\n"); return 1;
    }

    /* parse_tenths */
    {
        int val;
        if (!parse_tenths("4.5", &val) || val != 45) {
            fprintf(stderr, "parse_tenths 4.5 failed: %d\n", val); return 1;
        }
        if (!parse_tenths("0.0", &val) || val != 0) {
            fprintf(stderr, "parse_tenths 0.0 failed\n"); return 1;
        }
        if (!parse_tenths("12", &val) || val != 120) {
            fprintf(stderr, "parse_tenths 12 failed: %d\n", val); return 1;
        }
        if (parse_tenths("abc", &val)) {
            fprintf(stderr, "parse_tenths should fail on abc\n"); return 1;
        }
    }

    /* is_ground */
    if (!is_ground("ground") || !is_ground("GROUND") ||
        is_ground("not") || is_ground("")) {
        fprintf(stderr, "is_ground failed\n"); return 1;
    }

    /* ---- initialise display, then run fetch ---- */

    if (!set_position_request("39.117210", "-94.635600")) return 1;
    copy_charset();
    init_video();
    init_sprites();
    draw_static_scope();
    show_status(ST_WAIT);
    st = fetch();
    show_status(st);

    printf("fetch status=%u (0=OK)  count=%u\n", st, blob[4]);
    if (st != ST_OK) return 1;

    /* ---- validate blob structure ---- */
    if (blob[0] != MAGIC0 || blob[1] != MAGIC1 || blob[2] != 1) {
        fprintf(stderr, "bad blob header\n"); return 1;
    }
    if (blob[4] != MOCK_AC_COUNT) {
        fprintf(stderr, "expected %d ac, got %u\n", MOCK_AC_COUNT, blob[4]);
        return 1;
    }
    /* Check that the URL used matches what we set */
    if (strncmp(mock_last_url, ADSB_BASE, sizeof(ADSB_BASE) - 1)) {
        fprintf(stderr, "URL sent to meatloaf incorrect: %s\n", mock_last_url);
        return 1;
    }

    /* ---- sprite pattern tests (same as original) ---- */

    /* Every displayed list number must be the reverse of its digit glyph. */
    for (i = 0; i < blob[4]; ++i) {
        cell = host_ram + BITMAP + rowbase[4 + (i << 1)] + (TBL_COL << 3);
        glyph = charset + ((unsigned int)('1' + i) << 3);
        for (k = 0; k < 8; ++k) {
            if (cell[k] != (unsigned char)~glyph[k]) {
                fprintf(stderr, "bad reverse table number %u row %u\n", i + 1, k);
                return 1;
            }
        }
    }

    /* ---- dump display state ---- */
    f = fopen("bitmap.bin", "wb");
    fwrite(host_ram + BITMAP, 1, 8000, f); fclose(f);
    f = fopen("sprblock.bin", "wb");
    fwrite(host_ram + SPR_DATA, 1, 512, f); fclose(f);
    f = fopen("sprites.txt", "w");
    fprintf(f, "%u\n", host_ram[0xD015]);
    for (i = 0; i < 8; ++i)
        fprintf(f, "%u %u %u\n", host_ram[0xD000 + (i << 1)],
                host_ram[0xD001 + (i << 1)], host_ram[0xD027 + i] & 15);
    fclose(f);

    /* ---- sprite pointer validation ---- */
    for (i = 0; i < 8; ++i) {
        if (host_ram[MATRIX + 0x3F8 + i] != SPR_PTRVAL + i) {
            fprintf(stderr, "bad sprite pointer %u\n", i); return 1;
        }
        if (host_ram[SPR_DATA + (unsigned int)i * 64 + 63] != 0) {
            fprintf(stderr, "bad sprite pad byte %u\n", i); return 1;
        }
    }

    /* ---- digit uniqueness and direction stem tests (same as original) ---- */
    for (i = 0; i < 8; ++i) {
        build_sprite(i, 0, 1);
        digit_hash[i] = sprite_hash(i);
        for (other = 0; other < i; ++other) {
            if (digit_hash[i] == digit_hash[other]) {
                fprintf(stderr, "duplicate digit patterns %u/%u\n", other + 1, i + 1);
                return 1;
            }
        }
        for (d = 0; d < 8; ++d) {
            build_sprite(i, (unsigned char)(d << 5), 0);
            if (!sprite_pixel(i, dir_x[d], dir_y[d])) {
                fprintf(stderr, "missing direction endpoint digit=%u dir=%u\n", i + 1, d);
                return 1;
            }
            stem_pixels = 0;
            for (py = 0; py < 21; ++py) {
                for (px = 0; px < 24; ++px) {
                    mdx = px > 11 ? px - 11 : 11 - px;
                    mdy = py > 10 ? py - 10 : 10 - py;
                    if (mdx + mdy > 5 && sprite_pixel(i, px, py))
                        ++stem_pixels;
                }
            }
            if (stem_pixels != 5) {
                fprintf(stderr, "nonuniform stem digit=%u dir=%u pixels=%u\n",
                        i + 1, d, stem_pixels);
                return 1;
            }
        }
        build_sprite(i, 91, 1);
        stem_pixels = 0;
        for (py = 0; py < 21; ++py) {
            for (px = 0; px < 24; ++px) {
                mdx = px > 11 ? px - 11 : 11 - px;
                mdy = py > 10 ? py - 10 : 10 - py;
                if (mdx + mdy > 5 && sprite_pixel(i, px, py))
                    ++stem_pixels;
            }
        }
        if (stem_pixels != 0) {
            fprintf(stderr, "unknown track drew %u outside pixels for digit=%u\n",
                    stem_pixels, i + 1);
            return 1;
        }
        for (d = 0; d < 8; ++d) {
            if (sprite_pixel(i, dir_x[d], dir_y[d])) {
                fprintf(stderr, "unknown track drew arrow digit=%u dir=%u\n", i + 1, d);
                return 1;
            }
        }
    }
    for (i = 0; i < 8; ++i)
        build_sprite(i, (unsigned char)(i << 5), 0);
    f = fopen("sprite_sheet.bin", "wb");
    fwrite(host_ram + SPR_DATA, 1, 512, f); fclose(f);

    /* ---- link-down display test ---- */
    show_link_down();
    if (host_ram[0xD015] || !link_down_displayed ||
        host_ram[MATRIX + 12 * 40 + 6] != COL_RED_BLACK ||
        host_ram[BITMAP + rowbase[12] + (6 << 3)] != 0xFF) {
        fprintf(stderr, "link-down banner/clearing failed\n"); return 1;
    }

    puts("verified PETSCII and screen-code conversions");
    puts("verified coordinate, ICAO, and string function validation");
    puts("verified EGLL and KJFK airport lookup via binary search");
    puts("verified position and ICAO request URL construction");
    puts("verified Meatloaf mock: adsb.fi JSON Pointer extraction");
    puts("verified 8 sprite pointers, 8 unique digits, direction stems, unknown track");
    puts("verified link-down display and clearing");
    puts("dumped bitmap.bin sprblock.bin sprites.txt sprite_sheet.bin");
    return 0;
}
