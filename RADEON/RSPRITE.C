/*
 * RSPRITE.C  -  GPU Blit Bounce Demo
 *
 * Standalone Radeon demo: bouncing rainbow sprite with GPU blit,
 * double-buffered with vsync page flipping.  Sprite source is
 * generated in offscreen VRAM (page 2).
 */

#include "RSETUP.H"
#include <time.h>

int main(void)
{
    int bw = 120, bh = 90;
    int bx, by, dx, dy;
    int i;
    int sprite_y;
    int back_page, back_y;
    long fps_count;
    clock_t fps_t0;
    double fps;
    char buf[80];

    /* Need 3 pages: 2 display + 1 offscreen sprite */
    if (radeon_init("RSPRITE - GPU Blit Bounce Demo", 3))
        return 1;

    sprite_y = g_page_stride * 2;

    /* Widen scissor for offscreen ops */
    gpu_scissor_max();

    /* Generate rainbow-stripe sprite in offscreen VRAM */
    gpu_fill(0, sprite_y, bw, bh, 0);
    for (i = 0; i < 7; i++) {
        int y0 = sprite_y + i * (bh / 7);
        int y1 = y0 + bh / 7 - 1;
        unsigned char base = (unsigned char)(1 + i * 32);
        int x;
        for (x = 0; x < bw; x++) {
            unsigned char c = base + (unsigned char)(x * 31 / bw);
            gpu_fill(x, y0, 1, y1 - y0 + 1, c);
        }
    }
    gpu_wait_idle();

    /* Clear both display pages */
    gpu_fill(0, 0, g_xres, g_page_stride * 2, 0);
    gpu_wait_idle();

    /* Disable color compare */
    gpu_color_compare_off();

    bx = 0; by = 0; dx = 3; dy = 2;
    back_page = 1;
    fps_count = 0;
    fps       = 0.0;
    fps_t0    = clock();

    while (!kbhit()) {
        int nx = bx + dx;
        int ny = by + dy;

        if (nx < 0 || nx + bw > g_xres) { dx = -dx; nx = bx + dx; }
        if (ny < 0 || ny + bh > g_yres - 16) { dy = -dy; ny = by + dy; }

        back_y = back_page * g_page_stride;

        /* Clear back buffer */
        gpu_fill(0, back_y, g_xres, g_yres, 0);
        /* Blit sprite */
        gpu_blit_fwd(0, sprite_y, nx, back_y + ny, bw, bh);
        gpu_wait_idle();

        /* FPS counter */
        fps_count++;
        {
            clock_t now = clock();
            double elapsed = (double)(now - fps_t0) / CLOCKS_PER_SEC;
            if (elapsed >= 1.0) {
                fps = (double)fps_count / elapsed;
                fps_count = 0;
                fps_t0 = now;
            }
        }

        sprintf(buf, "GPU Blit Bounce  %.1f FPS  [ESC quit]", fps);
        cpu_str(4, back_y + g_yres - 14, buf, 253, 1);

        flip_page(back_page);

        back_page ^= 1;
        bx = nx; by = ny;
    }
    getch();

    /* Restore */
    flip_restore_page0();
    gpu_scissor_default();

    radeon_cleanup();
    return 0;
}
