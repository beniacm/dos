/*
 * WATER.C — Water surface renderer: lit heightfield from plasma waves
 *
 * Uses the shared WAVE library for the underlying four-wave interference
 * pattern.  The height value itself provides the base palette index
 * (preserving the recognisable plasma pattern).  Finite differences
 * (stencil S=4) yield surface normals; a directional shade offset +
 * n^4 specular highlights create the illusion of a 3-D water surface
 * lit from the upper left.
 *
 * Compiler paths:
 *   DJGPP/GCC + __SSE2__  — shading inner loop processes 4 pixels via
 *                            128-bit SSE2 integer intrinsics.
 *   Everything else        — pure C89 scalar fallback (Watcom-safe).
 *
 * Build:
 *   DJGPP: i586-pc-msdosdjgpp-gcc -march=prescott -mfpmath=sse -O3 ...
 *   Watcom: wcc386 -bt=dos -6r -ox -oh -on -zq WATER.C
 */

#include <string.h>
#include "WATER.H"
#include "WAVE.H"

#if defined(__DJGPP__) && defined(__SSE2__)
#include <emmintrin.h>
#define WATER_USE_SSE2  1
#else
#define WATER_USE_SSE2  0
#endif

void water_init(void)
{
    wave_init();
}

/* ======================================================================== *
 *  Shading constants                                                        *
 * ======================================================================== */

#define WATER_S     4
#define WATER_ROWS  (2 * WATER_S + 1)
#define WATER_SCALE 3
#define WATER_FLAT  80

/* ======================================================================== *
 *  Scalar shading fallback — pure C89, Watcom + DJGPP                     *
 *                                                                          *
 *  Uses the height value itself as the base palette index (preserving the  *
 *  plasma interference pattern), then shifts it with a directional shade   *
 *  offset and adds specular highlights for water sparkle.                  *
 * ======================================================================== */

static void shade_row_scalar(unsigned char *dst,
                             const short *row_c,
                             const short *row_u,
                             const short *row_d,
                             int width)
{
    /* Light from upper-left at 45-degree elevation */
    const int LX = -64;
    const int LY = -64;
    const int LZ =  90;
    int x;

    for (x = 0; x < width; x++) {
        int xl = x > WATER_S       ? x - WATER_S : 0;
        int xr = x < width-WATER_S ? x + WATER_S : width - 1;
        int base, nx, ny, dir, shade, dot_raw, rz, sp, idx;

        /* Base brightness from height — keeps plasma pattern visible */
        base = row_c[x] / 3;   /* 0..255 (height range 0..765) */

        nx = (row_c[xl] - row_c[xr]) * WATER_SCALE;
        ny = (row_u[x]  - row_d[x])  * WATER_SCALE;

        /* Directional shade: dot(N_horizontal, L_horizontal) / 256 */
        dir = nx * LX + ny * LY;
        shade = dir / 256;

        /* Specular: Rz = 2 * dot_full / FLAT - LZ */
        dot_raw = dir + WATER_FLAT * LZ;
        rz = (2 * dot_raw) / WATER_FLAT - LZ;
        if (rz < 0)   rz = 0;
        if (rz > 255) rz = 255;
        sp = (rz * rz) >> 8;
        sp = (sp * sp) >> 8;

        idx = base + shade + (sp >> 2);
        if (idx < 0)   idx = 0;
        if (idx > 255) idx = 255;

        dst[x] = (unsigned char)idx;
    }
}

/* ======================================================================== *
 *  SSE2 shading — 4 pixels per iteration                                  *
 *                                                                          *
 *  Pentium D has SSE2/SSE3 only — no _mm_mullo_epi32 (SSE4.1) or          *
 *  _mm_min_epi32 (SSE4.1).  We emulate 32-bit multiply via _mm_mul_epu32  *
 *  and 32-bit min/max via _mm_cmpgt_epi32.                                *
 * ======================================================================== */

#if WATER_USE_SSE2

/* 32-bit lane multiply emulation for SSE2.
 * _mm_mul_epu32 multiplies lanes 0,2 to 64-bit; we shuffle to get 1,3. */
static __inline__ __m128i sse2_mullo_epi32(__m128i a, __m128i b)
{
    __m128i p02 = _mm_mul_epu32(a, b);
    __m128i p13 = _mm_mul_epu32(_mm_shuffle_epi32(a, _MM_SHUFFLE(2,3,0,1)),
                                _mm_shuffle_epi32(b, _MM_SHUFFLE(2,3,0,1)));
    __m128i lo02 = _mm_shuffle_epi32(p02, _MM_SHUFFLE(0,0,2,0));
    __m128i lo13 = _mm_shuffle_epi32(p13, _MM_SHUFFLE(0,0,2,0));
    return _mm_unpacklo_epi32(lo02, lo13);
}

/* Signed 32-bit max/min emulation for SSE2. */
static __inline__ __m128i sse2_max_epi32(__m128i a, __m128i b)
{
    __m128i gt = _mm_cmpgt_epi32(a, b);
    return _mm_or_si128(_mm_and_si128(gt, a), _mm_andnot_si128(gt, b));
}
static __inline__ __m128i sse2_min_epi32(__m128i a, __m128i b)
{
    __m128i gt = _mm_cmpgt_epi32(a, b);
    return _mm_or_si128(_mm_and_si128(gt, b), _mm_andnot_si128(gt, a));
}

static void shade_row_sse2(unsigned char *dst,
                           const short *row_c,
                           const short *row_u,
                           const short *row_d,
                           int width)
{
    const __m128i vLX      = _mm_set1_epi32(-64);
    const __m128i vLY      = _mm_set1_epi32(-64);
    const __m128i vLZ      = _mm_set1_epi32(90);
    const __m128i vSCALE   = _mm_set1_epi32(WATER_SCALE);
    const __m128i vDIV3    = _mm_set1_epi32(43691); /* /3 ≈ *43691 >> 17 */
    const __m128i vRCP     = _mm_set1_epi32(205);   /* /80 ≈ *205 >> 14  */
    const __m128i v2       = _mm_set1_epi32(2);
    const __m128i vZero    = _mm_setzero_si128();
    const __m128i v255     = _mm_set1_epi32(255);
    const __m128i vFLAT_LZ = _mm_set1_epi32(WATER_FLAT * 90);
    int x;

    /* Process 4 pixels at a time; handle edges with scalar */
    if (WATER_S > 0)
        shade_row_scalar(dst, row_c, row_u, row_d, WATER_S);

    for (x = WATER_S; x <= width - WATER_S - 4; x += 4) {
        __m128i vnx, vny, vdir, vshade, vbase;
        __m128i vdot_raw, vrz, vsp, vidx, vout;

        /* Gather base height values: row_c[x] / 3 */
        vbase = _mm_set_epi32(row_c[x+3], row_c[x+2],
                              row_c[x+1], row_c[x+0]);
        vbase = _mm_srai_epi32(sse2_mullo_epi32(vbase, vDIV3), 17);

        /* Gather 4 nx values: (row_c[x-S] - row_c[x+S]) * SCALE */
        vnx = _mm_set_epi32(
            row_c[x+3-WATER_S] - row_c[x+3+WATER_S],
            row_c[x+2-WATER_S] - row_c[x+2+WATER_S],
            row_c[x+1-WATER_S] - row_c[x+1+WATER_S],
            row_c[x+0-WATER_S] - row_c[x+0+WATER_S]);
        vnx = sse2_mullo_epi32(vnx, vSCALE);

        /* Gather 4 ny values: (row_u[x] - row_d[x]) * SCALE */
        vny = _mm_set_epi32(
            row_u[x+3] - row_d[x+3],
            row_u[x+2] - row_d[x+2],
            row_u[x+1] - row_d[x+1],
            row_u[x+0] - row_d[x+0]);
        vny = sse2_mullo_epi32(vny, vSCALE);

        /* dir = nx*LX + ny*LY (directional component only) */
        vdir = _mm_add_epi32(sse2_mullo_epi32(vnx, vLX),
                             sse2_mullo_epi32(vny, vLY));

        /* shade = dir / 256 (arithmetic shift) */
        vshade = _mm_srai_epi32(vdir, 8);

        /* Specular: dot_raw = dir + FLAT*LZ, rz = 2*dot_raw/FLAT - LZ */
        vdot_raw = _mm_add_epi32(vdir, vFLAT_LZ);
        vrz = _mm_sub_epi32(
            _mm_srai_epi32(
                sse2_mullo_epi32(sse2_mullo_epi32(vdot_raw, v2), vRCP),
                14),
            vLZ);
        vrz = sse2_max_epi32(vrz, vZero);
        vrz = sse2_min_epi32(vrz, v255);

        /* sp = (rz*rz >> 8)^2 >> 8 — values ≤255 so products ≤65025 */
        vsp = _mm_srli_epi32(sse2_mullo_epi32(vrz, vrz), 8);
        vsp = _mm_srli_epi32(sse2_mullo_epi32(vsp, vsp), 8);

        /* idx = base + shade + (sp >> 2), clamped 0..255 */
        vidx = _mm_add_epi32(vbase,
            _mm_add_epi32(vshade, _mm_srli_epi32(vsp, 2)));
        vidx = sse2_max_epi32(vidx, vZero);
        vidx = sse2_min_epi32(vidx, v255);

        /* Pack 4×int32 → 4 bytes into dst */
        vout = _mm_packs_epi32(vidx, vZero);   /* 4×int32 → 8×int16 */
        vout = _mm_packus_epi16(vout, vZero);   /* 8×int16 → 16×uint8 */
        *((int *)(dst + x)) = _mm_cvtsi128_si32(vout);
    }

    /* Handle remaining pixels with scalar */
    if (x < width)
        shade_row_scalar(dst + x, row_c + x, row_u + x, row_d + x,
                         width - x);
}

#endif /* WATER_USE_SSE2 */

/* ======================================================================== *
 *  Main render entry point                                                 *
 * ======================================================================== */

void water_render(unsigned char *buf, int pitch, unsigned int t)
{
    int y;
    /* Coprime speed multipliers — waves evolve independently instead of
     * scrolling as a rigid unit.  Offsets break initial phase alignment. */
    unsigned int t1 = (t * 3)         & 0xFF;
    unsigned int t2 = (t * 4 + 97)    & 0xFF;
    unsigned int t3 = (t * 7 + 151)   & 0xFF;
    unsigned int t4 = (t * 11 + 53)   & 0xFF;

    short hring[WATER_ROWS][WATER_WIDTH];
    int   ring_base = 0;

    /* Pre-fill ring buffer */
    for (y = 0; y < WATER_ROWS; y++) {
        int sy = y - WATER_S;
        if (sy < 0) sy = 0;
        wave_fill_row(hring[y], sy, t1, t2, t3, t4);
    }

    for (y = 0; y < WATER_HEIGHT; y++) {
        unsigned char *dst = buf + (unsigned long)y * pitch;
        int ri_c = (y - ring_base + WATER_S) % WATER_ROWS;
        int ri_u = (ri_c - WATER_S + WATER_ROWS) % WATER_ROWS;
        int ri_d = (ri_c + WATER_S) % WATER_ROWS;

#if WATER_USE_SSE2
        shade_row_sse2(dst, hring[ri_c], hring[ri_u], hring[ri_d],
                       WATER_WIDTH);
#else
        shade_row_scalar(dst, hring[ri_c], hring[ri_u], hring[ri_d],
                         WATER_WIDTH);
#endif

        /* Advance ring buffer */
        if (y + WATER_S + 1 < WATER_HEIGHT) {
            int slot = (y - ring_base) % WATER_ROWS;
            wave_fill_row(hring[slot], y + WATER_S + 1,
                      t1, t2, t3, t4);
        }
    }
}

/* ======================================================================== *
 *  Water palette: deep ocean blue -> turquoise -> white foam               *
 * ======================================================================== */

void water_build_palette(unsigned char *pal)
{
    int i;
    for (i = 0; i < 256; i++) {
        double f  = i / 255.0;
        double f2 = f * f;
        int r, g, b;

        r = (int)(5.0 + 250.0 * f2);
        g = (int)(20.0 + 100.0 * f + 135.0 * f2);
        b = (int)(80.0 + 120.0 * f + 55.0 * f2);

        if (r > 255) r = 255;
        if (g > 255) g = 255;
        if (b > 255) b = 255;

        pal[i*4+0] = (unsigned char)r;
        pal[i*4+1] = (unsigned char)g;
        pal[i*4+2] = (unsigned char)b;
        pal[i*4+3] = 0;
    }
}
