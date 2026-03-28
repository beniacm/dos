/*
 * RDUNE.C  -  Mountain Chase: 16-Layer Parallax + 128-Segment Worm
 *
 * Standalone Radeon demo: fractal mountain landscape with 16 depth
 * layers scrolling at exponentially increasing speeds, plus a
 * 128-segment worm bouncing across the scene.
 *
 * Two compositing modes (toggle with SPACE):
 *   GPU: Radeon 2D engine PITCH_OFFSET blits (~150 blits/frame)
 *   CPU: Software compositing in system RAM + REP MOVSD final blit
 *
 * The CPU% counter shows the load difference between GPU offload
 * and CPU-based compositing.
 */

#include "RSETUP.H"
#include <time.h>

#define DUNE_NLAYERS   16
#define WORM_NSEGS    128
#define WORM_W         12
#define WORM_H         12
#define WORM_COLS      10
#define WORM_HIST     512

/* ---- System RAM buffers for CPU compositing -------------------- */
static unsigned char *g_ram_layer[DUNE_NLAYERS];
static unsigned char *g_ram_sprites;
static unsigned char *g_ram_frame;
static int g_ram_spr_pitch;
static int g_ram_spr_rows;

static int sw_alloc(void)
{
    int layer_sz = g_xres * g_yres;
    int spr_rows = ((WORM_NSEGS + WORM_COLS - 1) / WORM_COLS) * WORM_H;
    int spr_sz   = (WORM_COLS * WORM_W) * spr_rows;
    int i;

    g_ram_frame = (unsigned char *)malloc(layer_sz);
    if (!g_ram_frame) return 0;

    for (i = 0; i < DUNE_NLAYERS; i++) {
        g_ram_layer[i] = (unsigned char *)malloc(layer_sz);
        if (!g_ram_layer[i]) return 0;
    }

    g_ram_sprites   = (unsigned char *)malloc(spr_sz);
    g_ram_spr_pitch = WORM_COLS * WORM_W;
    g_ram_spr_rows  = spr_rows;
    return g_ram_sprites != NULL;
}

static void sw_free(void)
{
    int i;
    if (g_ram_frame)   { free(g_ram_frame);   g_ram_frame = NULL; }
    for (i = 0; i < DUNE_NLAYERS; i++)
        if (g_ram_layer[i]) { free(g_ram_layer[i]); g_ram_layer[i] = NULL; }
    if (g_ram_sprites) { free(g_ram_sprites); g_ram_sprites = NULL; }
}

/* ---- Generate bright sky gradient (layer 0, opaque) ------------- */
static void gen_desert_sky(unsigned char *buf, int pitch)
{
    int x, y, n, i, half;

    half = g_yres / 2;
    for (y = 0; y < g_yres; y++) {
        unsigned char c;
        if (y < half) {
            c = (unsigned char)(1 + y * 19 / half);
            if (c > 20) c = 20;
        } else {
            c = (unsigned char)(20 + (y - half) * 12 / half);
            if (c > 32) c = 32;
        }
        memset(buf + y * pitch, c, g_xres);
    }

    /* Sparse stars in upper quarter */
    srand(12345);
    n = g_xres / 4;
    for (i = 0; i < n; i++) {
        x = rand() % g_xres;
        y = rand() % (g_yres / 5);
        buf[y * pitch + x] = 255;
    }
    for (i = 0; i < n / 3; i++) {
        x = rand() % g_xres;
        y = rand() % (g_yres / 4);
        buf[y * pitch + x] = 254;
    }
}

/* ---- Generate a fractal mountain layer (layers 1-15) ------------ */
static void gen_dune_layer(unsigned char *buf, int pitch, int layer_idx)
{
    int x, y, h, top, step, seg, half, left, right, mid_x;
    int base_h, rough;
    unsigned char c_top, c_bot;
    static int hmap[832];

    for (y = 0; y < g_yres; y++)
        memset(buf + y * pitch, PLAX_TRANSP, g_xres);

    srand((unsigned)(layer_idx * 7919 + 3571));

    base_h = g_yres * (50 - layer_idx * 2) / 100;
    rough  = g_yres * (2 + layer_idx) / 80;

    for (x = 0; x < g_xres; x++)
        hmap[x] = base_h;

    step = 128;
    if (step > g_xres) step = g_xres;
    for (x = 0; x < g_xres; x += step)
        hmap[x] = base_h + (rand() % (rough * 2 + 1)) - rough;
    hmap[0] = base_h + (rand() % (rough + 1));
    hmap[g_xres - 1] = base_h + (rand() % (rough + 1));

    /* Midpoint displacement */
    for (seg = step; seg >= 2; seg /= 2) {
        half = seg / 2;
        for (x = 0; x + seg < g_xres; x += seg) {
            left  = hmap[x];
            right = hmap[x + seg];
            mid_x = x + half;
            if (mid_x < g_xres) {
                int disp = (rough * half) / step;
                if (disp < 1) disp = 1;
                hmap[mid_x] = (left + right) / 2
                             + (rand() % (disp * 2 + 1)) - disp;
            }
        }
    }

    for (x = 0; x < g_xres; x++) {
        if (hmap[x] < 8) hmap[x] = 8;
        if (hmap[x] > g_yres * 3 / 4) hmap[x] = g_yres * 3 / 4;
    }

    /* Color ranges per layer group */
    if (layer_idx <= 4) {
        c_top = (unsigned char)(33 + (layer_idx - 1) * 4);
        c_bot = (unsigned char)(c_top + 12);
        if (c_bot > 64) c_bot = 64;
    } else if (layer_idx <= 7) {
        c_top = (unsigned char)(49 + (layer_idx - 5) * 5);
        c_bot = (unsigned char)(c_top + 14);
        if (c_bot > 80) c_bot = 80;
    } else if (layer_idx <= 11) {
        c_top = (unsigned char)(65 + (layer_idx - 8) * 8);
        c_bot = (unsigned char)(c_top + 14);
        if (c_bot > 112) c_bot = 112;
    } else {
        c_top = (unsigned char)(113 + (layer_idx - 12) * 12);
        c_bot = (unsigned char)(c_top + 14);
        if (c_bot > 160) c_bot = 160;
    }

    for (x = 0; x < g_xres; x++) {
        h = hmap[x];
        top = g_yres - h;
        for (y = top; y < g_yres; y++) {
            int shade;
            shade = (int)c_top + (y - top) * ((int)c_bot - (int)c_top) /
                    (h > 0 ? h : 1);
            if (shade < (int)c_top) shade = (int)c_top;
            if (shade > (int)c_bot) shade = (int)c_bot;
            buf[y * pitch + x] = (unsigned char)shade;
        }
    }
}

/* ---- Generate 128 worm ball sprites ----------------------------- */
static void gen_worm_sprites(unsigned char *buf, int pitch)
{
    int si, row, col, sx, sy, x, y, cx, cy, r2, dx, dy;
    unsigned char c;
    int rows_needed;

    rows_needed = ((WORM_NSEGS + WORM_COLS - 1) / WORM_COLS) * WORM_H;
    for (y = 0; y < rows_needed; y++)
        memset(buf + y * pitch, PLAX_TRANSP, WORM_COLS * WORM_W);

    for (si = 0; si < WORM_NSEGS; si++) {
        row = si / WORM_COLS;
        col = si % WORM_COLS;
        sx = col * WORM_W;
        sy = row * WORM_H;
        cx = WORM_W / 2;
        cy = WORM_H / 2;
        r2 = (WORM_W / 2 - 1) * (WORM_W / 2 - 1);
        c  = (unsigned char)(200 + (si % 16));

        for (y = 0; y < WORM_H; y++) {
            for (x = 0; x < WORM_W; x++) {
                dx = x - cx;
                dy = y - cy;
                if (dx * dx + dy * dy <= r2) {
                    if (si == 0) {
                        if ((dx == -2 && dy == -1) || (dx == 2 && dy == -1))
                            buf[(sy + y) * pitch + sx + x] = 0;
                        else if (dx * dx + dy * dy <= (r2 * 2 / 3))
                            buf[(sy + y) * pitch + sx + x] = 255;
                        else
                            buf[(sy + y) * pitch + sx + x] = 254;
                    } else {
                        if (dx * dx + dy * dy <= r2 / 3)
                            buf[(sy + y) * pitch + sx + x] = 255;
                        else
                            buf[(sy + y) * pitch + sx + x] = c;
                    }
                }
            }
        }
    }
}

/* ---- Copy sys RAM buffer to VRAM staging area ------------------- */
static void ram_to_vram_stage(const unsigned char *buf, int w, int h)
{
    int y;
    for (y = 0; y < h; y++)
        memcpy(g_lfb + (long)(g_page_stride + y) * g_pitch, buf + y * w, w);
}

/* =============================================================== */
/*  Software (CPU) compositing functions                            */
/* =============================================================== */

/* Scroll-wrapped layer blit into sys RAM frame buffer */
static void sw_plax_blit(const unsigned char *layer, int scroll_x,
                         unsigned char *frame, int use_key)
{
    int y, x, sx, w1, w2;
    sx = scroll_x % g_xres;
    if (sx < 0) sx += g_xres;
    w1 = g_xres - sx;
    w2 = sx;

    for (y = 0; y < g_yres; y++) {
        unsigned char *dst = frame + y * g_xres;
        const unsigned char *src = layer + y * g_xres;

        if (!use_key) {
            memcpy(dst, src + sx, w1);
            if (w2 > 0)
                memcpy(dst + w1, src, w2);
        } else {
            /* First segment: from sx onward */
            for (x = 0; x < w1; x++) {
                unsigned char c = src[sx + x];
                if (c != PLAX_TRANSP) dst[x] = c;
            }
            /* Wrapped segment: from 0 */
            for (x = 0; x < w2; x++) {
                unsigned char c = src[x];
                if (c != PLAX_TRANSP) dst[w1 + x] = c;
            }
        }
    }
}

/* Keyed sprite blit from sprite sheet to sys RAM frame buffer */
static void sw_sprite_key(const unsigned char *spr, int spr_pitch,
                          int sx, int sy,
                          unsigned char *frame, int dx, int dy,
                          int w, int h)
{
    int y, x;
    for (y = 0; y < h; y++) {
        int fy = dy + y;
        if (fy < 0 || fy >= g_yres) continue;
        for (x = 0; x < w; x++) {
            int fx = dx + x;
            unsigned char c;
            if (fx < 0 || fx >= g_xres) continue;
            c = spr[(sy + y) * spr_pitch + sx + x];
            if (c != PLAX_TRANSP) frame[fy * g_xres + fx] = c;
        }
    }
}

/* Blit completed frame from sys RAM to VRAM back page using WC path */
static void
#ifdef __DJGPP__
__attribute__((noinline, optimize("O2")))
#endif
sw_blit_to_vram(const unsigned char *frame, int back_y)
{
    int y;
    for (y = 0; y < g_yres; y++)
        wc_memcpy(g_lfb + (long)(back_y + y) * g_pitch,
                  frame + y * g_xres, (unsigned long)g_xres);
}

/* ================================================================= */

int main(void)
{
    unsigned long layer_po[DUNE_NLAYERS];
    unsigned long sprite_po_val;
    unsigned long stage_po;
    int back_page, back_y, scroll;
    long fps_count;
    clock_t fps_t0, t_render_start, t_render_end, t_frame_end;
    double fps, cpu_pct, cpu_acc;
    long cpu_samples;
    char buf[160];
    int i;
    unsigned long need;
    int sprite_blit_w, sprite_blit_h;
    int gpu_mode = 1;

    /* Worm state */
    int worm_hx[WORM_HIST], worm_hy[WORM_HIST];
    int worm_head;
    int head_dx, head_dy;
    int scroll_x;

    /* 20 pages: 2 display + 16 layers + 1 sprite sheet + 1 staging */
    if (radeon_init("RDUNE - Mountain Chase Demo", 20))
        return 1;

    setup_dune_palette();

    need = (unsigned long)g_pitch * g_page_stride * 20;
    if (g_vram_mb > 0 && need > g_vram_mb * 1024UL * 1024UL) {
        gpu_fill(0, 0, g_xres, g_yres, 0);
        gpu_wait_idle();
        cpu_str_c(g_yres / 2, "Not enough VRAM for dune demo", 251, 2);
        cpu_str_c(g_yres / 2 + 30, "Press any key...", 253, 1);
        getch();
        radeon_cleanup();
        return 1;
    }

    /* Allocate system RAM buffers for CPU compositing */
    if (!sw_alloc()) {
        gpu_fill(0, 0, g_xres, g_yres, 0);
        gpu_wait_idle();
        cpu_str_c(g_yres / 2, "Not enough RAM for SW buffers", 251, 2);
        getch();
        radeon_cleanup();
        return 1;
    }

    /* Compute VRAM offsets and PITCH_OFFSET values for GPU mode */
    for (i = 0; i < DUNE_NLAYERS; i++)
        layer_po[i] = make_pitch_offset(
            (unsigned long)g_page_stride * (2 + i) * g_pitch);

    sprite_po_val = make_pitch_offset(
        (unsigned long)g_page_stride * 18 * g_pitch);
    stage_po = make_pitch_offset(
        (unsigned long)g_page_stride * g_pitch);

    /* Widen scissor for offscreen ops */
    gpu_scissor_max();

    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();
    cpu_str_c(g_yres / 2 - 20,
              "Generating 16-layer mountain landscape...", 255, 2);
    cpu_str_c(g_yres / 2 + 10,
              "Mountain Chase: 16 layers + 128-segment worm", 253, 1);

    /* Generate layers into system RAM, copy to VRAM for GPU mode */
    gen_desert_sky(g_ram_layer[0], g_xres);
    ram_to_vram_stage(g_ram_layer[0], g_xres, g_yres);
    gpu_blit_po(stage_po, 0, 0, layer_po[0], 0, 0, g_xres, g_yres);
    gpu_wait_idle();
    gpu_flush_2d_cache();

    for (i = 1; i < DUNE_NLAYERS; i++) {
        gen_dune_layer(g_ram_layer[i], g_xres, i);
        ram_to_vram_stage(g_ram_layer[i], g_xres, g_yres);
        gpu_blit_po(stage_po, 0, 0, layer_po[i], 0, 0, g_xres, g_yres);
        gpu_wait_idle();
        gpu_flush_2d_cache();
    }

    /* Sprites */
    gen_worm_sprites(g_ram_sprites, g_ram_spr_pitch);
    sprite_blit_w = g_ram_spr_pitch;
    sprite_blit_h = g_ram_spr_rows;
    ram_to_vram_stage(g_ram_sprites, sprite_blit_w, sprite_blit_h);
    gpu_blit_po(stage_po, 0, 0, sprite_po_val, 0, 0,
                sprite_blit_w, sprite_blit_h);
    gpu_wait_idle();
    gpu_flush_2d_cache();

    /* Reset GPU state */
    gpu_pitch_offset_reset();

    /* Initialize worm */
    srand(9999);
    worm_head = 0;
    head_dx = 5;
    head_dy = 3;
    worm_hx[0] = g_xres / 2;
    worm_hy[0] = g_yres / 2;
    for (i = 1; i < WORM_HIST; i++) {
        worm_hx[i] = worm_hx[0];
        worm_hy[i] = worm_hy[0];
    }

    /* Clear both display pages */
    gpu_fill(0, 0, g_xres, g_page_stride * 2, 0);
    gpu_wait_idle();

    back_page   = 1;
    scroll      = 0;
    fps_count   = 0;
    fps         = 0.0;
    cpu_pct     = 0.0;
    cpu_acc     = 0.0;
    cpu_samples = 0;
    fps_t0      = clock();

    while (1) {
        /* Check keys: ESC=quit, SPACE=toggle GPU/CPU */
        if (kbhit()) {
            int ch = getch();
            if (ch == 27) break;
            if (ch == ' ') gpu_mode = !gpu_mode;
        }

        back_y = back_page * g_page_stride;
        t_render_start = clock();

        if (gpu_mode) {
            /* ---- GPU compositing ---- */
            plax_blit_wrap_po(layer_po[0], scroll / 32,
                              g_default_po, back_y, g_yres, 0);
            for (i = 1; i < DUNE_NLAYERS; i++) {
                scroll_x = (int)((long)scroll * (1 + (long)i * i) / 64);
                plax_blit_wrap_po(layer_po[i], scroll_x,
                                  g_default_po, back_y, g_yres, 1);
            }

            for (i = WORM_NSEGS - 1; i >= 0; i--) {
                int hi, wx, wy, sr, sc;
                hi = (worm_head - i * 2 + WORM_HIST * 256) % WORM_HIST;
                wx = worm_hx[hi];
                wy = worm_hy[hi];
                sr = i / WORM_COLS;
                sc = i % WORM_COLS;
                gpu_blit_po_key(sprite_po_val, sc * WORM_W, sr * WORM_H,
                                g_default_po, wx, back_y + wy,
                                WORM_W, WORM_H, PLAX_TRANSP);
            }
            gpu_wait_idle();

        } else {
            /* ---- CPU compositing ---- */
            /* Composite layers into sys RAM frame buffer */
            sw_plax_blit(g_ram_layer[0], scroll / 32, g_ram_frame, 0);
            for (i = 1; i < DUNE_NLAYERS; i++) {
                scroll_x = (int)((long)scroll * (1 + (long)i * i) / 64);
                sw_plax_blit(g_ram_layer[i], scroll_x, g_ram_frame, 1);
            }

            /* Draw worm sprites into sys RAM frame */
            for (i = WORM_NSEGS - 1; i >= 0; i--) {
                int hi, wx, wy, sr, sc;
                hi = (worm_head - i * 2 + WORM_HIST * 256) % WORM_HIST;
                wx = worm_hx[hi];
                wy = worm_hy[hi];
                sr = i / WORM_COLS;
                sc = i % WORM_COLS;
                sw_sprite_key(g_ram_sprites, g_ram_spr_pitch,
                              sc * WORM_W, sr * WORM_H,
                              g_ram_frame, wx, wy, WORM_W, WORM_H);
            }

            /* Final blit: sys RAM -> VRAM back page (WC-optimal) */
            sw_blit_to_vram(g_ram_frame, back_y);
        }

        t_render_end = clock();

        /* Advance worm head */
        {
            int nx, ny;
            nx = worm_hx[worm_head] + head_dx;
            ny = worm_hy[worm_head] + head_dy;
            if (nx < 0)               { nx = 0;               head_dx = -head_dx; }
            if (nx > g_xres - WORM_W) { nx = g_xres - WORM_W; head_dx = -head_dx; }
            if (ny < 16)              { ny = 16;              head_dy = -head_dy; }
            if (ny > g_yres - WORM_H) { ny = g_yres - WORM_H; head_dy = -head_dy; }
            worm_head = (worm_head + 1) % WORM_HIST;
            worm_hx[worm_head] = nx;
            worm_hy[worm_head] = ny;
        }

        /* FPS counter */
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

        /* HUD */
        sprintf(buf,
                "[%s] Mountain Chase  16L+128seg  "
                "%.1f FPS  CPU:%d%%  [SPACE=toggle ESC=quit]",
                gpu_mode ? "GPU" : "CPU",
                fps, (int)(cpu_pct + 0.5));
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
        scroll += 3;
        if (scroll >= g_xres * 1000) scroll = 0;
    }

    flip_restore_page0();
    gpu_pitch_offset_reset();
    gpu_scissor_default();

    sw_free();
    radeon_cleanup();
    return 0;
}
