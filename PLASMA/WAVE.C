/*
 * WAVE.C — Reusable four-wave interference engine
 *
 * See WAVE.H for the public interface.
 *
 * Pure C89 — compiles under DJGPP/GCC and OpenWatcom.
 * No floating point at runtime (sin() used only in wave_init).
 */

#include <math.h>
#include "WAVE.H"

/* ======================================================================== *
 *  Public tables                                                            *
 * ======================================================================== */

unsigned char  wave_sintab[256];           /* 256 B — sine LUT              */
unsigned short wave_dx2[WAVE_WIDTH];       /* 2 KB  — precomputed dx² term  */

/* ======================================================================== *
 *  Module-private state (set once by wave_init)                             *
 * ======================================================================== */

static int w_icy;      /* centre-Y = WAVE_HEIGHT / 2 */
static int w_maxd2;    /* icx² + icy²                */

/* ======================================================================== *
 *  Initialisation                                                           *
 * ======================================================================== */

void wave_init(void)
{
    int x;
    int icx = WAVE_WIDTH  / 2;
    w_icy   = WAVE_HEIGHT / 2;
    w_maxd2 = icx * icx + w_icy * w_icy;

    for (x = 0; x < 256; x++)
        wave_sintab[x] = (unsigned char)
            ((sin(x * 6.283185307 / 256.0) + 1.0) * 127.5);

    for (x = 0; x < WAVE_WIDTH; x++) {
        int dx = x - icx;
        wave_dx2[x] = (unsigned short)((dx * dx * 255) / w_maxd2);
    }
}

/* ======================================================================== *
 *  Per-row height computation                                               *
 * ======================================================================== */

void wave_fill_row(short *row, int y,
                   unsigned int t1, unsigned int t2,
                   unsigned int t3, unsigned int t4)
{
    int x;
    int dbase = (y >> 1) & 0xFF;
    int dy    = y - w_icy;
    int dy2   = (dy * dy * 255) / w_maxd2;
    int vy    = (int)wave_sintab[((unsigned int)(y >> 1) + t1) & 0xFF];
    unsigned short xwave[256];
    unsigned char  radial[WAVE_WIDTH];

    /* Horizontal + diagonal fold */
    for (x = 0; x < 256; x++)
        xwave[x] = wave_sintab[(x + t2) & 0xFF]
                  + wave_sintab[(x + dbase + t3) & 0xFF];

    /* Radial ripple from centre */
    for (x = 0; x < WAVE_WIDTH; x++) {
        unsigned int d = wave_dx2[x] + dy2;
        if (d > 255) d = 255;
        radial[x] = wave_sintab[(d + t4) & 0xFF];
    }

    /* Combine: 3 waves × 0..255 ⇒ 0..765 */
    for (x = 0; x < WAVE_WIDTH; x++) {
        int v = (int)xwave[(x >> 1) & 0xFF] + vy + (int)radial[x];
        row[x] = (short)v;
    }
}
