/*
 * WAVE.H — Reusable four-wave interference engine
 *
 * Provides the classic plasma sine-interference pattern as a shared
 * height field.  Both the plasma colour demo and the water surface
 * demo build on this same wave engine.
 *
 * Tables (sine LUT + radial dx² + cosine LUT + radial ddx) are
 * initialised once via wave_init().
 *
 * wave_fill_row()      — height only (for plasma colour mapping)
 * wave_fill_row_grad() — height + analytical x/y gradients (for
 *                         water surface lighting without finite diffs)
 *
 * Pure C89 — compiles under DJGPP/GCC and OpenWatcom.
 */
#ifndef WAVE_H
#define WAVE_H

#define WAVE_WIDTH   1024
#define WAVE_HEIGHT  768

/* Shared lookup tables — read-only after wave_init(). */
extern unsigned char  wave_sintab[256];           /* 0..255 unsigned    */
extern signed char    wave_costab[256];           /* -127..+127 signed  */
extern unsigned short wave_dx2[WAVE_WIDTH];       /* radial distance²   */
extern short          wave_ddx[WAVE_WIDTH];       /* d(dist)/dx × 256   */

/* Initialise all tables.  Call once at startup. */
void wave_init(void);

/*
 * Fill one scanline of the four-wave height field.
 *
 *   row   — output: WAVE_WIDTH shorts, each in range 0..765
 *   y     — screen row (0..WAVE_HEIGHT-1)
 *   t1..t4 — time offsets (caller picks any speed scheme)
 *
 * The four waves are:
 *   1. Vertical    — sin( (y>>1) + t1 )
 *   2. Horizontal  — sin( (x>>1) + t2 )
 *   3. Diagonal    — sin( (x>>1) + (y>>1) + t3 )
 *   4. Radial      — sin( dist(x,y) + t4 )
 */
void wave_fill_row(short *row, int y,
                   unsigned int t1, unsigned int t2,
                   unsigned int t3, unsigned int t4);

/*
 * Fill one scanline with height AND analytical gradients.
 *
 *   row_h  — output: heights, WAVE_WIDTH shorts, range 0..765
 *   row_nx — output: dh/dx,   WAVE_WIDTH shorts (signed)
 *   row_ny — output: dh/dy,   WAVE_WIDTH shorts (signed)
 *
 * Gradients are computed via the chain rule from wave_costab[],
 * eliminating the need for a multi-row ring buffer and finite
 * difference stencil.
 */
void wave_fill_row_grad(short *row_h, short *row_nx, short *row_ny,
                        int y, unsigned int t1, unsigned int t2,
                        unsigned int t3, unsigned int t4);

#endif /* WAVE_H */
