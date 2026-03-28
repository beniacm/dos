/*
 * RSETUP.H  -  Shared Radeon demo init/cleanup
 *
 * Provides radeon_init() and radeon_cleanup() which handle all the
 * boilerplate that every Radeon demo program needs:
 *   - RDTSC calibration
 *   - DPMI buffer allocation
 *   - PCI detection + MMIO mapping
 *   - VESA mode find + set
 *   - LFB mapping (with multi-page VRAM support)
 *   - Palette + GPU 2D engine init
 *   - AVIVO / VGA flip-mode detection
 *
 * Usage:
 *   if (radeon_init("My Demo", 6)) return 1;
 *   // ... demo code ...
 *   radeon_cleanup();
 */

#ifndef RSETUP_H
#define RSETUP_H

#include "RADEONHW.H"

/* Initialise everything needed for a Radeon GPU demo.
   title:      printed as the banner line.
   vram_pages: total VRAM pages to map (display + offscreen).
   Returns 0 on success, non-zero on error. */
int radeon_init(const char *title, int vram_pages);

/* Restore text mode and release all resources. */
void radeon_cleanup(void);

#endif /* RSETUP_H */
