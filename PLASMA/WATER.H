/*
 * WATER.H — Water surface renderer
 *
 * Public API for the water surface effect.  Uses the shared WAVE library
 * (WAVE.H/WAVE.C) for the underlying four-wave interference pattern,
 * then applies height-based shading with directional light and specular
 * highlights to create a 3-D water surface appearance.
 *
 * On DJGPP/GCC with SSE2 the shading inner loop is vectorised (4 pixels
 * per iteration).  On Watcom or DOSBox-safe builds, a pure-C89 scalar
 * fallback is used.
 */
#ifndef WATER_H
#define WATER_H

#define WATER_WIDTH   1024
#define WATER_HEIGHT  768

/* Call once before render_water().  Builds internal sine + dx² tables. */
void water_init(void);

/* Render one frame into `buf` (8-bpp, pitch bytes per row). */
void water_render(unsigned char *buf, int pitch, unsigned int t);

/* Build the 256-entry water palette into `pal` (4 bytes per entry: R,G,B,0). */
void water_build_palette(unsigned char *pal);

#endif /* WATER_H */
