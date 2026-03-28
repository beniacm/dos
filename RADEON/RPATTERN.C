/*
 * RPATTERN.C  -  GPU Pattern + Starburst Lines Demo
 *
 * Standalone Radeon demo: GPU-accelerated solid fills and lines.
 * Uses RSETUP for hardware init/cleanup.
 */

#include "RSETUP.H"

int main(void)
{
    int i, bw, bh, x, y, cols, rows;
    unsigned char c;
    char fbuf[80];

    if (radeon_init("RPATTERN - GPU Pattern + Lines Demo", 2))
        return 1;

    /* Clear screen */
    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();

    /* Grid of coloured blocks */
    cols = 16;  rows = 8;
    bw = (g_xres - 4) / cols;
    bh = (g_yres / 2 - 20) / rows;
    if (bw < 4) bw = 4;
    if (bh < 4) bh = 4;

    for (i = 0; i < cols * rows && i < 128; i++) {
        x = 2 + (i % cols) * bw;
        y = 30 + (i / cols) * bh;
        c = (unsigned char)(i * 2);
        gpu_fill(x, y, bw - 1, bh - 1, c);
    }

    /* Starburst of lines from centre */
    {
        int cx = g_xres / 2;
        int cy = g_yres * 3 / 4;
        int step = g_xres / 30;
        if (step < 10) step = 10;

        for (x = 0; x < g_xres; x += step)
            gpu_line(cx, cy, x, g_yres / 2 + 10,
                     (unsigned char)(97 + (x*31/g_xres)));
        for (y = g_yres/2 + 10; y < g_yres - 2; y += step/2)
            gpu_line(cx, cy, g_xres - 3, y,
                     (unsigned char)(129 + (y*31/g_yres)));
        for (x = g_xres - 1; x >= 0; x -= step)
            gpu_line(cx, cy, x, g_yres - 3,
                     (unsigned char)(161 + (x*31/g_xres)));
        for (y = g_yres - 1; y >= g_yres/2 + 10; y -= step/2)
            gpu_line(cx, cy, 2, y,
                     (unsigned char)(1 + (y*31/g_yres)));
    }

    gpu_wait_idle();

    /* HUD */
    cpu_str_c(4, "GPU-Drawn Pattern (fill + line)", 255, 1);
    sprintf(fbuf, "Flip: %s    Radeon 2D engine drew all shapes.  Press key...",
            g_avivo_flip ? "AVIVO hwflip" : "VBE+VGA vsync");
    cpu_str_c(g_yres - 14, fbuf, 253, 1);

    getch();

    radeon_cleanup();
    return 0;
}
