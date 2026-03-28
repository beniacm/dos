/*
 * RPLAX.C  -  GPU Parallax Scrolling Demo
 *
 * Standalone Radeon demo combining two parallax modes:
 *   1) Diagnostic grid parallax (4 colour-coded grid layers)
 *   2) Scenic parallax (sky, mountains, hills, cityscape)
 *
 * All layer compositing is GPU-accelerated using keyed blits.
 * Double-buffered with AVIVO hardware page flip or VBE fallback.
 */

#include "RSETUP.H"
#include <time.h>

#define PLAX_NLAYERS   4

/* ---- Integer triangle wave ---- */
static int tri_wave(int t, int period, int amp)
{
    int q, phase;
    if (period <= 0 || amp == 0) return 0;
    q = period / 4;
    if (q <= 0) return 0;
    phase = ((t % period) + period) % period;
    if (phase < q)      return  phase * amp / q;
    if (phase < 3 * q)  return (2 * q - phase) * amp / q;
    return (phase - period) * amp / q;
}

/* ---- Non-PO blit with horizontal wrapping ---- */
static void plax_blit_wrap(int layer_y, int scroll_x,
                           int dst_y, int h, int use_key)
{
    int sx, w1, w2;
    sx = scroll_x % g_xres;
    if (sx < 0) sx += g_xres;
    w1 = g_xres - sx;
    w2 = sx;

    if (use_key) {
        gpu_blit_key(sx, layer_y, 0, dst_y, w1, h, PLAX_TRANSP);
        if (w2 > 0)
            gpu_blit_key(0, layer_y, w1, dst_y, w2, h, PLAX_TRANSP);
    } else {
        gpu_blit_fwd(sx, layer_y, 0, dst_y, w1, h);
        if (w2 > 0)
            gpu_blit_fwd(0, layer_y, w1, dst_y, w2, h);
    }
}

/* =============================================================== */
/*  Diagnostic grid layer generator                                 */
/* =============================================================== */

static void gen_diag_layer(int base, int cell_w, int cell_h,
                           unsigned char border_c, unsigned char label_c,
                           const char *tag)
{
    int x, y, cx, cy;

    for (y = 0; y < g_yres; y++)
        memset(g_lfb + (long)(base + y) * g_pitch, PLAX_TRANSP, g_xres);

    for (cx = 0; cx * cell_w < g_xres; cx++) {
        int x0 = cx * cell_w;
        int x1 = x0 + cell_w - 1;
        if (x1 >= g_xres) x1 = g_xres - 1;

        for (cy = 0; cy * cell_h < g_yres; cy++) {
            int y0 = cy * cell_h;
            int y1 = y0 + cell_h - 1;
            if (y1 >= g_yres) y1 = g_yres - 1;

            /* Top and bottom edges (2px thick) */
            for (x = x0; x <= x1; x++) {
                g_lfb[(long)(base + y0) * g_pitch + x] = border_c;
                if (y0 + 1 < g_yres)
                    g_lfb[(long)(base + y0 + 1) * g_pitch + x] = border_c;
                g_lfb[(long)(base + y1) * g_pitch + x] = border_c;
                if (y1 - 1 > y0 + 1)
                    g_lfb[(long)(base + y1 - 1) * g_pitch + x] = border_c;
            }
            /* Left and right edges (2px thick) */
            for (y = y0; y <= y1; y++) {
                g_lfb[(long)(base + y) * g_pitch + x0] = border_c;
                if (x0 + 1 <= x1)
                    g_lfb[(long)(base + y) * g_pitch + x0 + 1] = border_c;
                g_lfb[(long)(base + y) * g_pitch + x1] = border_c;
                if (x1 - 1 > x0 + 1)
                    g_lfb[(long)(base + y) * g_pitch + x1 - 1] = border_c;
            }

            /* Label inside cell */
            if (cell_w >= 24 && cell_h >= 16) {
                char lbl[8];
                int lx, li;
                sprintf(lbl, "%s%d", tag, (cx + cy * 10) % 100);
                lx = x0 + 4;
                for (li = 0; lbl[li] && lx + 8 <= x1; li++, lx += 8)
                    cpu_char(lx, base + y0 + 4, lbl[li], label_c, 1);
            }
        }
    }
}

/* =============================================================== */
/*  Scenic layer generators                                         */
/* =============================================================== */

static void gen_sky(int base)
{
    int x, y, i, n;

    for (y = 0; y < g_yres; y++) {
        unsigned char c = (unsigned char)(1 + y * 16 / g_yres);
        memset(g_lfb + (long)(base + y) * g_pitch, c, g_xres);
    }
    srand(42);
    n = g_xres * g_yres / 50;
    for (i = 0; i < n; i++) {
        x = rand() % g_xres;
        y = rand() % (g_yres * 3 / 4);
        g_lfb[(long)(base + y) * g_pitch + x] =
            (unsigned char)((rand() % 4 == 0) ? 255 : 193 + rand() % 20);
    }
}

static void gen_mountains(int base)
{
    int x, y, h, top, shade;

    for (y = 0; y < g_yres; y++)
        memset(g_lfb + (long)(base + y) * g_pitch, PLAX_TRANSP, g_xres);

    for (x = 0; x < g_xres; x++) {
        h = g_yres * 35 / 100
            + tri_wave(x,             g_xres * 2, g_yres / 4)
            + tri_wave(x * 3 + 80,   g_xres,     g_yres / 6)
            + tri_wave(x * 7 + 200,  g_xres,     g_yres / 12);
        if (h < 40)                h = 40;
        if (h > g_yres * 7 / 10)  h = g_yres * 7 / 10;
        top = g_yres - h;

        for (y = top; y < g_yres; y++) {
            shade = 195 + (y - top) * 22 / h;
            if (shade > 218) shade = 218;
            if (y < top + h / 10 && h > g_yres / 3)
                shade = 255;
            g_lfb[(long)(base + y) * g_pitch + x] = (unsigned char)shade;
        }
    }
}

static void gen_hills(int base)
{
    int x, y, h, top, ci;

    for (y = 0; y < g_yres; y++)
        memset(g_lfb + (long)(base + y) * g_pitch, PLAX_TRANSP, g_xres);

    for (x = 0; x < g_xres; x++) {
        h = g_yres * 22 / 100
            + tri_wave(x * 2,        g_xres * 2, g_yres / 7)
            + tri_wave(x * 5 + 50,   g_xres,     g_yres / 10);
        if (h < 20)            h = 20;
        if (h > g_yres / 2)   h = g_yres / 2;
        top = g_yres - h;

        for (y = top; y < g_yres; y++) {
            ci = 33 + (y - top) * 26 / h;
            if (ci > 58) ci = 58;
            g_lfb[(long)(base + y) * g_pitch + x] = (unsigned char)ci;
        }

        /* Trees every ~50 pixels on taller hills */
        if ((x % 50) < 2 && h > g_yres / 5) {
            int th = h / 4;
            for (y = top - th; y < top; y++)
                if (y >= 0)
                    g_lfb[(long)(base + y) * g_pitch + x] = 38;
            {
                int cx, cy;
                for (cy = top - th - 10; cy < top - th + 2; cy++)
                    for (cx = x - 4; cx <= x + 4; cx++)
                        if (cy >= 0 && cx >= 0 && cx < g_xres)
                            g_lfb[(long)(base + cy) * g_pitch + cx] = 42;
            }
        }
    }
}

static void gen_cityscape(int base)
{
    int x, y, ground_h, bx, by;

    for (y = 0; y < g_yres; y++)
        memset(g_lfb + (long)(base + y) * g_pitch, PLAX_TRANSP, g_xres);

    ground_h = g_yres / 10;

    for (y = g_yres - ground_h; y < g_yres; y++) {
        int ci = 70 + (y - (g_yres - ground_h)) * 20 / ground_h;
        if (ci > 90) ci = 90;
        memset(g_lfb + (long)(base + y) * g_pitch, (unsigned char)ci, g_xres);
    }

    srand(7777);
    x = 2;
    while (x < g_xres) {
        int bw = 12 + rand() % 20;
        int bh = ground_h + 20 + rand() % (g_yres / 4);
        int top_y = g_yres - bh;
        unsigned char bc = (unsigned char)(129 + rand() % 28);

        if (top_y < g_yres / 3) top_y = g_yres / 3;

        for (bx = x; bx < x + bw && bx < g_xres; bx++)
            for (by = top_y; by < g_yres - ground_h; by++)
                g_lfb[(long)(base + by) * g_pitch + bx] = bc;

        for (bx = x; bx < x + bw && bx < g_xres; bx++)
            if (top_y > 0)
                g_lfb[(long)(base + top_y) * g_pitch + bx] =
                    (unsigned char)(bc + 5 < 160 ? bc + 5 : 159);

        for (by = top_y + 5; by < g_yres - ground_h - 5; by += 8)
            for (bx = x + 3; bx < x + bw - 3; bx += 5) {
                int wy, wx;
                unsigned char wc;
                if (bx + 2 >= g_xres) continue;
                wc = (unsigned char)((rand() % 3) ? 254 : 248);
                for (wy = by; wy < by + 3; wy++)
                    for (wx = bx; wx < bx + 2; wx++)
                        g_lfb[(long)(base + wy) * g_pitch + wx] = wc;
            }

        x += bw + 4 + rand() % 15;
    }
}

/* =============================================================== */
/*  Diagnostic parallax loop                                        */
/* =============================================================== */

static void demo_parallax_diag(void)
{
    int layer_base[PLAX_NLAYERS];
    int back_page, back_y, scroll;
    long fps_count;
    clock_t fps_t0;
    double fps;
    char buf[80];
    int i;
    unsigned long need;

    need = (unsigned long)g_pitch * g_page_stride * 6;
    if (g_vram_mb > 0 && need > g_vram_mb * 1024UL * 1024UL) {
        gpu_fill(0, 0, g_xres, g_yres, 0);
        gpu_wait_idle();
        cpu_str_c(g_yres / 2, "Not enough VRAM for diag demo", 251, 2);
        cpu_str_c(g_yres / 2 + 30, "Press any key...", 253, 1);
        getch();
        return;
    }

    for (i = 0; i < PLAX_NLAYERS; i++)
        layer_base[i] = g_page_stride * (2 + i);

    gpu_scissor_max();

    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();
    cpu_str_c(g_yres / 2 - 20,
              "Generating DIAGNOSTIC parallax layers...", 255, 2);
    cpu_str_c(g_yres / 2 + 10,
              "Layer 0=Blue  1=Red  2=Green  3=Yellow  grids", 253, 1);

    /* Layer 0: blue grid (opaque background) */
    {
        int ly;
        for (ly = 0; ly < g_yres; ly++)
            memset(g_lfb + (long)(layer_base[0] + ly) * g_pitch, 2, g_xres);
    }
    {
        int lx, ly;
        for (ly = 0; ly < g_yres; ly += 100)
            for (lx = 0; lx < g_xres; lx++)
                g_lfb[(long)(layer_base[0] + ly) * g_pitch + lx] = 20;
        for (lx = 0; lx < g_xres; lx += 100)
            for (ly = 0; ly < g_yres; ly++)
                g_lfb[(long)(layer_base[0] + ly) * g_pitch + lx] = 20;
        for (ly = 0; ly < g_yres; ly += 100) {
            for (lx = 0; lx < g_xres; lx += 100) {
                char lbl[8];
                sprintf(lbl, "B%d", ((lx/100) + (ly/100)*10) % 100);
                if (lx + 24 < g_xres && ly + 12 < g_yres) {
                    int ci;
                    for (ci = 0; lbl[ci]; ci++)
                        cpu_char(lx + 4 + ci * 8, layer_base[0] + ly + 4,
                                 lbl[ci], 28, 1);
                }
            }
        }
    }

    gen_diag_layer(layer_base[1], 80, 80, 90, 75, "R");
    gen_diag_layer(layer_base[2], 60, 60, 55, 45, "G");
    gen_diag_layer(layer_base[3], 40, 40, 155, 140, "Y");

    gpu_fill(0, 0, g_xres, g_page_stride * 2, 0);
    gpu_wait_idle();

    gpu_color_compare_off();

    back_page = 1;
    scroll    = 0;
    fps_count = 0;
    fps       = 0.0;
    fps_t0    = clock();

    while (!kbhit()) {
        back_y = back_page * g_page_stride;

        plax_blit_wrap(layer_base[0], scroll / 8,
                       back_y, g_yres, 0);
        plax_blit_wrap(layer_base[1], scroll / 4,
                       back_y, g_yres, 1);
        plax_blit_wrap(layer_base[2], scroll / 2,
                       back_y, g_yres, 1);
        plax_blit_wrap(layer_base[3], scroll,
                       back_y, g_yres, 1);

        gpu_wait_idle();

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

        sprintf(buf, "DIAG Parallax  %.1f FPS  scroll=%d  [ESC quit]",
                fps, scroll);
        cpu_str(4, back_y + 4, buf, 255, 1);

        cpu_str(4,   back_y + g_yres - 42,
                "Blue=L0(1/8x) Red=L1(1/4x) Green=L2(1/2x) Yellow=L3(1x)",
                253, 1);
        sprintf(buf, "page=%d back_y=%d stride=%d pitch=%d",
                back_page, back_y, g_page_stride, g_pitch);
        cpu_str(4, back_y + g_yres - 28, buf, 254, 1);
        sprintf(buf, "base[0]=%d [1]=%d [2]=%d [3]=%d",
                layer_base[0], layer_base[1], layer_base[2], layer_base[3]);
        cpu_str(4, back_y + g_yres - 14, buf, 254, 1);

        flip_page(back_page);

        back_page ^= 1;
        scroll++;
        if (scroll >= g_xres * 1000) scroll = 0;
    }
    getch();

    flip_restore_page0();
    gpu_scissor_default();
}

/* =============================================================== */
/*  Scenic parallax loop                                            */
/* =============================================================== */

static void demo_parallax(void)
{
    int layer_base[PLAX_NLAYERS];
    int back_page, back_y, scroll;
    long fps_count;
    clock_t fps_t0, t_render_start, t_render_end, t_frame_end;
    double fps, cpu_pct, cpu_acc;
    long cpu_samples;
    char buf[120];
    int i;
    unsigned long need;

    need = (unsigned long)g_pitch * g_page_stride * 6;
    if (g_vram_mb > 0 && need > g_vram_mb * 1024UL * 1024UL) {
        gpu_fill(0, 0, g_xres, g_yres, 0);
        gpu_wait_idle();
        cpu_str_c(g_yres / 2, "Not enough VRAM for parallax demo", 251, 2);
        cpu_str_c(g_yres / 2 + 30, "Press any key...", 253, 1);
        getch();
        return;
    }

    for (i = 0; i < PLAX_NLAYERS; i++)
        layer_base[i] = g_page_stride * (2 + i);

    gpu_scissor_max();

    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();
    cpu_str_c(g_yres / 2 - 20, "Generating parallax layers...", 255, 2);
    cpu_str_c(g_yres / 2 + 10, "Sky - Mountains - Hills - City", 253, 1);

    gen_sky(layer_base[0]);
    gen_mountains(layer_base[1]);
    gen_hills(layer_base[2]);
    gen_cityscape(layer_base[3]);

    gpu_fill(0, 0, g_xres, g_page_stride * 2, 0);
    gpu_wait_idle();

    gpu_color_compare_off();

    back_page   = 1;
    scroll      = 0;
    fps_count   = 0;
    fps         = 0.0;
    cpu_pct     = 0.0;
    cpu_acc     = 0.0;
    cpu_samples = 0;
    fps_t0      = clock();

    while (!kbhit()) {
        back_y = back_page * g_page_stride;

        t_render_start = clock();

        plax_blit_wrap(layer_base[0], scroll / 8,
                       back_y, g_yres, 0);
        plax_blit_wrap(layer_base[1], scroll / 4,
                       back_y, g_yres, 1);
        plax_blit_wrap(layer_base[2], scroll / 2,
                       back_y, g_yres, 1);
        plax_blit_wrap(layer_base[3], scroll,
                       back_y, g_yres, 1);

        gpu_wait_idle();

        t_render_end = clock();

        fps_count++;
        {
            clock_t now = clock();
            double elapsed = (double)(now - fps_t0) / CLOCKS_PER_SEC;
            if (elapsed >= 1.0) {
                fps = (double)fps_count / elapsed;
                fps_count = 0;
                fps_t0 = now;
                if (cpu_samples > 0)
                    cpu_pct = cpu_acc / cpu_samples;
                cpu_acc = 0.0;
                cpu_samples = 0;
            }
        }

        sprintf(buf, "GPU Parallax  %d layers  %.1f FPS  CPU:%d%%  [ESC quit]",
                PLAX_NLAYERS, fps, (int)(cpu_pct + 0.5));
        cpu_str(4, back_y + 4, buf, 255, 1);

        flip_page(back_page);

        t_frame_end = clock();

        {
            double render_t = (double)(t_render_end - t_render_start);
            double frame_t  = (double)(t_frame_end  - t_render_start);
            if (frame_t > 0.0) {
                cpu_acc += render_t * 100.0 / frame_t;
                cpu_samples++;
            }
        }

        back_page ^= 1;
        scroll++;
        if (scroll >= g_xres * 1000) scroll = 0;
    }
    getch();

    flip_restore_page0();
    gpu_scissor_default();
}

/* =============================================================== */

int main(void)
{
    int ch;

    /* 6 pages: 2 display + 4 layer buffers */
    if (radeon_init("RPLAX - GPU Parallax Scrolling Demo", 6))
        return 1;

    /* Run diagnostic parallax first */
    demo_parallax_diag();

    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();
    cpu_str_c(g_yres / 2,
              "Press any key for scenic parallax, ESC to quit", 253, 1);
    ch = getch();

    if (ch != 27) {
        /* Run scenic parallax */
        demo_parallax();
    }

    radeon_cleanup();
    return 0;
}
