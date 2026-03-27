/*
 * WAVE.C — Reusable four-wave interference engine
 *
 * See WAVE.H for the public interface.
 *
 * Pure C89 — compiles under DJGPP/GCC and OpenWatcom.
 * No floating point at runtime (sin/cos used only in wave_init).
 */

#include <math.h>
#include "WAVE.H"

/* ======================================================================== *
 *  Public tables                                                            *
 * ======================================================================== */

unsigned char  wave_sintab[256];           /* 256 B — sine LUT              */
signed char    wave_costab[256];           /* 256 B — cosine LUT (signed)   */
unsigned short wave_dx2[WAVE_WIDTH];       /* 2 KB  — precomputed dx² term  */
short          wave_ddx[WAVE_WIDTH];       /* 2 KB  — d(dist)/dx × 256      */

/* ======================================================================== *
 *  Module-private state (set once by wave_init)                             *
 * ======================================================================== */

static int w_icy;      /* centre-Y = WAVE_HEIGHT / 2 */
static int w_maxd2;    /* icx² + icy²                */

/* ======================================================================== *
 *  Initialisation                                                           *
 * ======================================================================== */

/* wave_init writes to wave_dx2[] and wave_ddx[] — global BSS arrays whose
 * addresses may be only 4-byte aligned after COFF link-time merging.
 * GCC's auto-vectoriser generates movdqa (16-byte aligned store) for these
 * arrays; if they land on an 8-byte boundary the first movdqa faults with
 * a General Protection Fault.
 * Disabling tree-vectorisation for this one-shot init function is safe:
 * the per-frame wave_fill_row/wave_fill_row_grad paths are unaffected and
 * those loops operate on stack arrays that are always properly aligned.   */
#ifdef __GNUC__
__attribute__((noinline, optimize("O2,no-tree-vectorize")))
#endif
void wave_init(void)
{
    int x;
    int icx = WAVE_WIDTH  / 2;
    w_icy   = WAVE_HEIGHT / 2;
    w_maxd2 = icx * icx + w_icy * w_icy;

    for (x = 0; x < 256; x++) {
        double a = x * 6.283185307 / 256.0;
        wave_sintab[x] = (unsigned char)((sin(a) + 1.0) * 127.5);
        wave_costab[x] = (signed char)(cos(a) * 127.0);
    }

    for (x = 0; x < WAVE_WIDTH; x++) {
        int dx = x - icx;
        wave_dx2[x] = (unsigned short)((dx * dx * 255) / w_maxd2);
        /* d(dist)/dx in 8.8 fixed point: 2*dx*255*256 / maxd² */
        wave_ddx[x] = (short)((long)dx * 130560L / w_maxd2);
    }
}

/* ======================================================================== *
 *  Per-row height computation (height only — used by plasma)               *
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

/* ======================================================================== *
 *  Per-row height + analytical gradient (for water lighting)               *
 *                                                                          *
 *  h(x,y) = sintab[f1(y)] + sintab[f2(x)] + sintab[f3(x,y)]              *
 *          + sintab[f4(x,y)]                                               *
 *                                                                          *
 *  dh/dx = costab[f2] · df2/dx  +  costab[f3] · df3/dx                    *
 *        + costab[f4] · df4/dx                                             *
 *                                                                          *
 *  Chain rule factors:                                                     *
 *    f2 = (x>>1) + t2        →  df2/dx = 1/2                              *
 *    f3 = (x>>1) + (y>>1) + t3  →  df3/dx = 1/2,  df3/dy = 1/2           *
 *    f4 = dist(x,y) + t4    →  df4/dx = ddx[x],   df4/dy = ddy           *
 *    f1 = (y>>1) + t1        →  df1/dy = 1/2                              *
 * ======================================================================== */

void wave_fill_row_grad(short *row_h, short *row_nx, short *row_ny,
                        int y, unsigned int t1, unsigned int t2,
                        unsigned int t3, unsigned int t4)
{
    int x;
    int dbase = (y >> 1) & 0xFF;
    int dy    = y - w_icy;
    int dy2   = (dy * dy * 255) / w_maxd2;

    /* Wave 1 (vertical): height and y-derivative, constant per row */
    int vy  = (int)wave_sintab[((unsigned int)(y >> 1) + t1) & 0xFF];
    int dvy = (int)wave_costab[((unsigned int)(y >> 1) + t1) & 0xFF];

    /* Radial y-derivative factor (8.8 fixed point, constant per row) */
    int ddy = (int)((long)dy * 130560L / w_maxd2);

    /* Precompute horizontal + diagonal: value and derivatives */
    unsigned short xwave[256];
    short          xwave_dx[256];
    short          xwave_dy[256];

    for (x = 0; x < 256; x++) {
        unsigned int arg2 = (x + t2) & 0xFF;
        unsigned int arg3 = (x + dbase + t3) & 0xFF;
        xwave[x]    = (unsigned short)
            ((int)wave_sintab[arg2] + wave_sintab[arg3]);
        xwave_dx[x] = (short)
            ((int)wave_costab[arg2] + wave_costab[arg3]);
        xwave_dy[x] = wave_costab[arg3];
    }

    /* Main loop: height + gradient per pixel */
    for (x = 0; x < WAVE_WIDTH; x++) {
        int xidx = (x >> 1) & 0xFF;
        unsigned int d = wave_dx2[x] + dy2;
        int rad_h, rad_cos, nx, ny;

        if (d > 255) {
            /* Clamped region: flat radial, zero radial gradient */
            rad_h   = (int)wave_sintab[(255 + t4) & 0xFF];
            rad_cos = 0;
        } else {
            unsigned int arg4 = (d + t4) & 0xFF;
            rad_h   = (int)wave_sintab[arg4];
            rad_cos = (int)wave_costab[arg4];
        }

        row_h[x] = (short)((int)xwave[xidx] + vy + rad_h);

        /* x-gradient: (costab[f2]+costab[f3])/2 + costab[f4]*ddx[x]>>8 */
        nx = xwave_dx[xidx] / 2
           + (rad_cos * (int)wave_ddx[x]) / 256;
        row_nx[x] = (short)nx;

        /* y-gradient: (costab[f1]+costab[f3])/2 + costab[f4]*ddy>>8 */
        ny = (dvy + (int)xwave_dy[xidx]) / 2
           + (rad_cos * ddy) / 256;
        row_ny[x] = (short)ny;
    }
}
