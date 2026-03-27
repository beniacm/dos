/*
 * RADEON.C  -  ATI Radeon X1300 Pro (RV515/RV516) DOS Demo
 *
 * Hardware-layer code (DPMI, PCI, VBE, MMIO, GPU engine, palette, flip)
 * lives in RADEONHW.C / RADEONHW.H.  This file contains only the demo
 * code: mode selection, 2D-engine operations, scenes and main().
 */

#include "RADEONHW.H"
#include <conio.h>
#include <time.h>

/* g_vesa_mem_kb is shared global declared in RADEONHW.H */

/* =============================================================== */
/*  Radeon 2D operations                                            */
/* =============================================================== */

/* Hardware-accelerated solid rectangle fill */
static void gpu_fill(int x, int y, int w, int h, unsigned char color)
{
    gpu_wait_fifo(5);

    wreg(R_DP_GUI_MASTER_CNTL,
         GMC_BRUSH_SOLID | GMC_DST_8BPP | GMC_SRC_DATATYPE_COLOR |
         ROP3_PATCOPY | GMC_CLR_CMP_DIS | GMC_WR_MSK_DIS);

    wreg(R_DP_BRUSH_FRGD_CLR, (unsigned long)color);
    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
    wreg(R_DST_Y_X, ((unsigned long)y << 16) | (unsigned long)x);
    wreg(R_DST_HEIGHT_WIDTH, ((unsigned long)h << 16) | (unsigned long)w);
}

/* Set up 2D engine state for a batch of solid fills.
   Call once before a sequence of gpu_fill_fast() calls. */
static void gpu_fill_setup(void)
{
    gpu_wait_fifo(3);
    wreg(R_DP_GUI_MASTER_CNTL,
         GMC_BRUSH_SOLID | GMC_DST_8BPP | GMC_SRC_DATATYPE_COLOR |
         ROP3_PATCOPY | GMC_CLR_CMP_DIS | GMC_WR_MSK_DIS);
    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
    wreg(R_CLR_CMP_CNTL, 0);
}

/* Fast solid fill — only 3 register writes (assumes gpu_fill_setup). */
static void gpu_fill_fast(int x, int y, int w, int h, unsigned char color)
{
    gpu_wait_fifo(3);
    wreg(R_DP_BRUSH_FRGD_CLR, (unsigned long)color);
    wreg(R_DST_Y_X, ((unsigned long)y << 16) | (unsigned long)x);
    wreg(R_DST_HEIGHT_WIDTH, ((unsigned long)h << 16) | (unsigned long)w);
}

/* Ultra-fast fill — 2 register writes, same color as previous fill.
   Matches xf86-video-ati RADEONSolid(): only position + size. */
static void gpu_fill_rect(int x, int y, int w, int h)
{
    gpu_wait_fifo(2);
    wreg(R_DST_Y_X, ((unsigned long)y << 16) | (unsigned long)x);
    wreg(R_DST_HEIGHT_WIDTH, ((unsigned long)h << 16) | (unsigned long)w);
}

/* Set fill color once for a batch of gpu_fill_rect() calls. */
static void gpu_fill_set_color(unsigned char color)
{
    gpu_wait_fifo(1);
    wreg(R_DP_BRUSH_FRGD_CLR, (unsigned long)color);
}

/* ---- Batched FIFO access for peak rect throughput ---- */

/* FIFO depth for batching: 32 rects × 2 regs = 64 entries.
   We wait once for 64 entries, then blast 32 rects directly. */
#define FILL_BATCH  32

/* Inlined register write indices for tight loops */
#define MMIO_DST_Y_X          (R_DST_Y_X >> 2)
#define MMIO_DST_HEIGHT_WIDTH  (R_DST_HEIGHT_WIDTH >> 2)
#define MMIO_DP_BRUSH_FRGD_CLR (R_DP_BRUSH_FRGD_CLR >> 2)

/* Fast XOR-shift PRNG — no modulo needed for power-of-2 masks */
static unsigned long g_xor_state = 2463534242UL;
static unsigned long xorshift(void)
{
    g_xor_state ^= g_xor_state << 13;
    g_xor_state ^= g_xor_state >> 17;
    g_xor_state ^= g_xor_state << 5;
    return g_xor_state;
}

/* Hardware-accelerated screen-to-screen blit */
static void gpu_blit(int sx, int sy, int dx, int dy, int w, int h)
{
    unsigned long dp = 0;

    /* Handle overlapping regions */
    if (sx <= dx) { sx += w - 1; dx += w - 1; }
    else          { dp |= DST_X_LEFT_TO_RIGHT; }
    if (sy <= dy) { sy += h - 1; dy += h - 1; }
    else          { dp |= DST_Y_TOP_TO_BOTTOM; }

    gpu_wait_fifo(6);

    wreg(R_DP_GUI_MASTER_CNTL,
         GMC_BRUSH_NONE | GMC_DST_8BPP | GMC_SRC_DATATYPE_COLOR |
         ROP3_SRCCOPY | GMC_DP_SRC_MEMORY | GMC_CLR_CMP_DIS |
         GMC_WR_MSK_DIS);

    wreg(R_DP_CNTL, dp);
    wreg(R_SRC_Y_X, ((unsigned long)sy << 16) | (unsigned long)(sx & 0xFFFF));
    wreg(R_DST_Y_X, ((unsigned long)dy << 16) | (unsigned long)(dx & 0xFFFF));
    wreg(R_DST_HEIGHT_WIDTH, ((unsigned long)h << 16) | (unsigned long)w);
}

/* Hardware-accelerated line drawing */
static void gpu_line(int x1, int y1, int x2, int y2, unsigned char color)
{
    gpu_wait_fifo(5);

    wreg(R_DP_GUI_MASTER_CNTL,
         GMC_BRUSH_SOLID | GMC_DST_8BPP | GMC_SRC_DATATYPE_COLOR |
         ROP3_PATCOPY | GMC_CLR_CMP_DIS | GMC_WR_MSK_DIS);

    wreg(R_DP_BRUSH_FRGD_CLR, (unsigned long)color);
    wreg(R_DST_LINE_START,
         ((unsigned long)y1 << 16) | ((unsigned long)x1 & 0xFFFF));
    wreg(R_DST_LINE_END,
         ((unsigned long)y2 << 16) | ((unsigned long)x2 & 0xFFFF));
    wreg(R_DST_LINE_PATCOUNT, 0x55UL << 16);
}

/* Forward blit (no overlap detection — for non-overlapping regions) */
static void gpu_blit_fwd(int sx, int sy, int dx, int dy, int w, int h)
{
    gpu_wait_fifo(5);

    wreg(R_DP_GUI_MASTER_CNTL,
         GMC_BRUSH_NONE | GMC_DST_8BPP | GMC_SRC_DATATYPE_COLOR |
         ROP3_SRCCOPY | GMC_DP_SRC_MEMORY | GMC_CLR_CMP_DIS |
         GMC_WR_MSK_DIS);

    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
    wreg(R_SRC_Y_X, ((unsigned long)sy << 16) | (unsigned long)(sx & 0xFFFF));
    wreg(R_DST_Y_X, ((unsigned long)dy << 16) | (unsigned long)(dx & 0xFFFF));
    wreg(R_DST_HEIGHT_WIDTH, ((unsigned long)h << 16) | (unsigned long)w);
}

/* Forward blit with source color-key transparency.
   Pixels in the source matching `key` are NOT drawn. */
static void gpu_blit_key(int sx, int sy, int dx, int dy, int w, int h,
                         unsigned char key)
{
    gpu_wait_fifo(8);

    wreg(R_CLR_CMP_CLR_SRC, (unsigned long)key);
    wreg(R_CLR_CMP_MASK,    0x000000FFUL);
    wreg(R_CLR_CMP_CNTL,    CLR_CMP_SRC_SOURCE | CLR_CMP_FCN_NE);

    wreg(R_DP_GUI_MASTER_CNTL,
         GMC_BRUSH_NONE | GMC_DST_8BPP | GMC_SRC_DATATYPE_COLOR |
         ROP3_SRCCOPY | GMC_DP_SRC_MEMORY | GMC_WR_MSK_DIS);
         /* Note: no GMC_CLR_CMP_DIS — color compare is active */

    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
    wreg(R_SRC_Y_X, ((unsigned long)sy << 16) | (unsigned long)(sx & 0xFFFF));
    wreg(R_DST_Y_X, ((unsigned long)dy << 16) | (unsigned long)(dx & 0xFFFF));
    wreg(R_DST_HEIGHT_WIDTH, ((unsigned long)h << 16) | (unsigned long)w);
}

#define PLAX_TRANSP    0    /* palette index = transparent */

/* ---------------------------------------------------------------
   Per-surface PITCH_OFFSET blit functions.
   The R300-R500 2D engine has a 13-bit Y coordinate limit (max 8191).
   For offscreen surfaces beyond row 8191, we must encode the surface
   base into SRC/DST_PITCH_OFFSET and keep Y within 0..8191.

   These functions write PITCH_OFFSET registers AND DP_GUI_MASTER_CNTL
   with GMC_*_PITCH_OFFSET_CNTL bits in a single FIFO batch, matching
   the xf86-video-ati driver pattern.  This guarantees the GPU reads
   the per-surface offsets for each blit.
   ---------------------------------------------------------------*/

/* Build a PITCH_OFFSET register value for a given VRAM byte offset */
static unsigned long make_pitch_offset(unsigned long vram_byte_off)
{
    unsigned long pitch64  = (unsigned long)g_pitch / 64;
    unsigned long gpu_addr = g_fb_location + vram_byte_off;
    return (pitch64 << 22) | ((gpu_addr >> 10) & 0x003FFFFFUL);
}

/* Forward blit with explicit per-surface PITCH_OFFSET values */
static void gpu_blit_po(unsigned long src_po, int sx, int sy,
                        unsigned long dst_po, int dx, int dy,
                        int w, int h)
{
    gpu_wait_fifo(7);
    wreg(R_DST_PITCH_OFFSET, dst_po);
    wreg(R_SRC_PITCH_OFFSET, src_po);
    wreg(R_DP_GUI_MASTER_CNTL,
         GMC_DST_PITCH_OFFSET_CNTL | GMC_SRC_PITCH_OFFSET_CNTL |
         GMC_BRUSH_NONE | GMC_DST_8BPP | GMC_SRC_DATATYPE_COLOR |
         ROP3_SRCCOPY | GMC_DP_SRC_MEMORY | GMC_CLR_CMP_DIS |
         GMC_WR_MSK_DIS);
    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
    wreg(R_SRC_Y_X, ((unsigned long)sy << 16) | (unsigned long)(sx & 0xFFFF));
    wreg(R_DST_Y_X, ((unsigned long)dy << 16) | (unsigned long)(dx & 0xFFFF));
    wreg(R_DST_HEIGHT_WIDTH, ((unsigned long)h << 16) | (unsigned long)w);
}

/* Keyed blit with explicit per-surface PITCH_OFFSET values.
   Pixels matching `key` in the source are NOT drawn. */
static void gpu_blit_po_key(unsigned long src_po, int sx, int sy,
                            unsigned long dst_po, int dx, int dy,
                            int w, int h, unsigned char key)
{
    gpu_wait_fifo(10);
    wreg(R_CLR_CMP_CLR_SRC, (unsigned long)key);
    wreg(R_CLR_CMP_MASK,    0x000000FFUL);
    wreg(R_CLR_CMP_CNTL,    CLR_CMP_SRC_SOURCE | CLR_CMP_FCN_NE);
    wreg(R_DST_PITCH_OFFSET, dst_po);
    wreg(R_SRC_PITCH_OFFSET, src_po);
    wreg(R_DP_GUI_MASTER_CNTL,
         GMC_DST_PITCH_OFFSET_CNTL | GMC_SRC_PITCH_OFFSET_CNTL |
         GMC_BRUSH_NONE | GMC_DST_8BPP | GMC_SRC_DATATYPE_COLOR |
         ROP3_SRCCOPY | GMC_DP_SRC_MEMORY | GMC_WR_MSK_DIS);
    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
    wreg(R_SRC_Y_X, ((unsigned long)sy << 16) | (unsigned long)(sx & 0xFFFF));
    wreg(R_DST_Y_X, ((unsigned long)dy << 16) | (unsigned long)(dx & 0xFFFF));
    wreg(R_DST_HEIGHT_WIDTH, ((unsigned long)h << 16) | (unsigned long)w);
}

/* Parallax blit with scroll wrapping — uses per-surface PITCH_OFFSET */
static void plax_blit_wrap_po(unsigned long src_po, int scroll_x,
                              unsigned long dst_po, int dst_y,
                              int h, int use_key)
{
    int sx, w1, w2;
    sx = scroll_x % g_xres;
    if (sx < 0) sx += g_xres;
    w1 = g_xres - sx;
    w2 = sx;

    if (use_key) {
        gpu_blit_po_key(src_po, sx, 0, dst_po, 0, dst_y, w1, h, PLAX_TRANSP);
        if (w2 > 0)
            gpu_blit_po_key(src_po, 0, 0, dst_po, w1, dst_y, w2, h, PLAX_TRANSP);
    } else {
        gpu_blit_po(src_po, sx, 0, dst_po, 0, dst_y, w1, h);
        if (w2 > 0)
            gpu_blit_po(src_po, 0, 0, dst_po, w1, dst_y, w2, h);
    }
}

/* Flush 2D destination cache (after GPU writes, before next read) */
static void gpu_flush_2d_cache(void)
{
    gpu_wait_fifo(2);
    wreg(R_DSTCACHE_CTLSTAT, R300_RB2D_DC_FLUSH_ALL);
    wreg(R_WAIT_UNTIL, WAIT_2D_IDLECLEAN | WAIT_DMA_GUI_IDLE);
}

/* =============================================================== */
/*  Demo / benchmark routines                                       */
/* =============================================================== */

/* Draw a colour test pattern with GPU fills */
static void demo_pattern(void)
{
    int i, bw, bh, x, y, cols, rows;
    unsigned char c;

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
            gpu_line(cx, cy, x, g_yres / 2 + 10, (unsigned char)(97 + (x*31/g_xres)));
        for (y = g_yres/2 + 10; y < g_yres - 2; y += step/2)
            gpu_line(cx, cy, g_xres - 3, y, (unsigned char)(129 + (y*31/g_yres)));
        for (x = g_xres - 1; x >= 0; x -= step)
            gpu_line(cx, cy, x, g_yres - 3, (unsigned char)(161 + (x*31/g_xres)));
        for (y = g_yres - 1; y >= g_yres/2 + 10; y -= step/2)
            gpu_line(cx, cy, 2, y, (unsigned char)(1 + (y*31/g_yres)));
    }

    gpu_wait_idle();
}

/* CPU vs GPU fill benchmark — uses RDTSC for microsecond-accurate timing */
static void demo_benchmark(void)
{
    unsigned long t0lo, t0hi, t1lo, t1hi;
    double cpu_ms, gpu_ms, rect_ms, rect_ms2, rect_ms3;
    int iters = 100;
    long rect_iters, rect_iters2, rect_iters3;
    int i;
    unsigned char col;
    char buf[120];

    /* Warm up */
    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();

    /* --- CPU benchmark --- */
    rdtsc_read(&t0lo, &t0hi);
    for (i = 0; i < iters; i++) {
        int y;
        col = (unsigned char)(i & 0xFF);
        for (y = 0; y < g_yres; y++)
            memset(g_lfb + y * g_pitch, col, g_xres);
    }
    rdtsc_read(&t1lo, &t1hi);
    cpu_ms = tsc_to_ms(t1lo, t1hi, t0lo, t0hi);

    /* --- GPU full-screen fill benchmark --- */
    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();

    rdtsc_read(&t0lo, &t0hi);
    for (i = 0; i < iters; i++) {
        col = (unsigned char)(i & 0xFF);
        gpu_fill(0, 0, g_xres, g_yres, col);
    }
    gpu_wait_idle();
    rdtsc_read(&t1lo, &t1hi);
    gpu_ms = tsc_to_ms(t1lo, t1hi, t0lo, t0hi);

    /* --- GPU small-rect throughput benchmark (fast path) --- */
    {
        int rw = 32, rh = 32;
        int cols = g_xres / rw;
        int rows = (g_yres - 20) / rh;
        long total_rects = (long)cols * rows;
        int pass;

        gpu_fill(0, 0, g_xres, g_yres, 0);
        gpu_wait_idle();

        /* 3-reg path (color + pos + size per rect) */
        gpu_fill_setup();
        rect_iters = 0;
        rdtsc_read(&t0lo, &t0hi);
        for (pass = 0; pass < 200; pass++) {
            int rx, ry;
            col = (unsigned char)(pass & 0xFF);
            for (ry = 0; ry < rows; ry++)
                for (rx = 0; rx < cols; rx++)
                    gpu_fill_fast(rx * rw, ry * rh, rw, rh,
                                  (unsigned char)(col + rx + ry));
            rect_iters += total_rects;
        }
        gpu_wait_idle();
        rdtsc_read(&t1lo, &t1hi);
        rect_ms = tsc_to_ms(t1lo, t1hi, t0lo, t0hi);

        /* 2-reg path (pos + size only, same color — peak throughput) */
        gpu_fill_setup();
        gpu_fill_set_color(0x55);
        rect_iters2 = 0;
        rdtsc_read(&t0lo, &t0hi);
        for (pass = 0; pass < 200; pass++) {
            int rx, ry;
            for (ry = 0; ry < rows; ry++)
                for (rx = 0; rx < cols; rx++)
                    gpu_fill_rect(rx * rw, ry * rh, rw, rh);
            rect_iters2 += total_rects;
        }
        gpu_wait_idle();
        rdtsc_read(&t1lo, &t1hi);
        rect_ms2 = tsc_to_ms(t1lo, t1hi, t0lo, t0hi);

        /* Batched 2-reg path — one FIFO check per 32 rects, direct MMIO */
        gpu_fill_setup();
        gpu_fill_set_color(0xAA);
        rect_iters3 = 0;
        {
            unsigned long hw_packed = ((unsigned long)rh << 16) | (unsigned long)rw;

            rdtsc_read(&t0lo, &t0hi);
            for (pass = 0; pass < 200; pass++) {
                int rx, ry, pending;
                pending = 0;
                for (ry = 0; ry < rows; ry++) {
                    for (rx = 0; rx < cols; rx++) {
                        if (pending == 0)
                            gpu_wait_fifo(FILL_BATCH * 2);
                        g_mmio[MMIO_DST_Y_X] =
                            ((unsigned long)(ry * rh) << 16) | (unsigned long)(rx * rw);
                        g_mmio[MMIO_DST_HEIGHT_WIDTH] = hw_packed;
                        if (++pending >= FILL_BATCH) {
                            g_fifo_free = 0;
                            pending = 0;
                        }
                    }
                }
                g_fifo_free = 0;
                rect_iters3 += total_rects;
            }
            gpu_wait_idle();
            rdtsc_read(&t1lo, &t1hi);
            rect_ms3 = tsc_to_ms(t1lo, t1hi, t0lo, t0hi);
        }
    }

    /* Display results */
    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();

    {
        double total = (double)g_xres * g_yres * iters / (1024.0*1024.0);
        double cpu_rate = (cpu_ms > 0) ? total / (cpu_ms/1000.0) : 0;
        double gpu_rate = (gpu_ms > 0) ? total / (gpu_ms/1000.0) : 0;
        double speedup  = (gpu_ms > 0) ? cpu_ms / gpu_ms : 0;
        double rps3     = (rect_ms > 0) ? (double)rect_iters / (rect_ms / 1000.0) : 0;
        double rpix3    = (rect_ms > 0) ? (double)rect_iters * 32 * 32 /
                          (rect_ms / 1000.0) / 1000000.0 : 0;
        double rps2     = (rect_ms2 > 0) ? (double)rect_iters2 / (rect_ms2 / 1000.0) : 0;
        double rpix2    = (rect_ms2 > 0) ? (double)rect_iters2 * 32 * 32 /
                          (rect_ms2 / 1000.0) / 1000000.0 : 0;
        double rpsB     = (rect_ms3 > 0) ? (double)rect_iters3 / (rect_ms3 / 1000.0) : 0;
        double rpixB    = (rect_ms3 > 0) ? (double)rect_iters3 * 32 * 32 /
                          (rect_ms3 / 1000.0) / 1000000.0 : 0;

        cpu_str_c(10, "=== CPU vs GPU Fill Benchmark ===", 255, 2);

        sprintf(buf, "%d x full-screen fill (%dx%d, 8bpp)", iters, g_xres, g_yres);
        cpu_str_c(36, buf, 253, 1);

        sprintf(buf, "CPU: %7.1f ms  (%5.1f MB/s)", cpu_ms, cpu_rate);
        cpu_str(40, 60, buf, 251, 2);
        if (cpu_rate < 100.0)
            cpu_str(40, 76, "(Run WCINIT.EXE first for ~30x CPU rate)", 249, 1);

        sprintf(buf, "GPU: %7.1f ms  (%5.1f MB/s)", gpu_ms, gpu_rate);
        cpu_str(40, 86, buf, 250, 2);

        if (gpu_ms > 0) {
            sprintf(buf, "GPU speedup: %.1fx", speedup);
            cpu_str_c(128, buf, 254, 3);
        }

        /* Small rect throughput — 3 regs (color varies) */
        sprintf(buf, "32x32 (3-reg):    %6.0f Krect/s  %5.0f Mpix/s",
                rps3/1000.0, rpix3);
        cpu_str(20, 166, buf, 250, 1);

        /* 2-reg same-color */
        sprintf(buf, "32x32 (2-reg):    %6.0f Krect/s  %5.0f Mpix/s",
                rps2/1000.0, rpix2);
        cpu_str(20, 186, buf, 254, 1);

        /* Batched 2-reg (peak) */
        sprintf(buf, "32x32 (batch-32): %6.0f Krect/s  %5.0f Mpix/s",
                rpsB/1000.0, rpixB);
        cpu_str(20, 206, buf, 255, 1);

        /* Draw bar chart */
        {
            int bar_max = g_xres - 100;
            int cpu_bar, gpu_bar, bar_y;
            double mx = cpu_ms;
            if (gpu_ms > mx) mx = gpu_ms;
            if (mx <= 0) mx = 1;

            cpu_bar = (int)(cpu_ms / mx * bar_max);
            gpu_bar = (int)(gpu_ms / mx * bar_max);
            if (cpu_bar < 1) cpu_bar = 1;
            if (gpu_bar < 1) gpu_bar = 1;

            bar_y = 240;
            cpu_str(10, bar_y - 12, "CPU", 251, 1);
            gpu_fill(50, bar_y, cpu_bar, 24, 251 /* red */);
            bar_y += 40;
            cpu_str(10, bar_y - 12, "GPU", 250, 1);
            gpu_fill(50, bar_y, gpu_bar, 24, 250 /* green */);
            gpu_wait_idle();
        }

        sprintf(buf, "Flip: %s   Timer: RDTSC @ %.0f MHz",
                g_avivo_flip ? "AVIVO hwflip" : "VBE+VGA vsync", g_rdtsc_mhz);
        cpu_str_c(350, buf, 253, 1);
        cpu_str_c(g_yres - 20, "Press any key for GPU flood demo, ESC to quit", 253, 1);
    }
}

/* Animated random GPU rectangles with throughput counter.
   Uses batched FIFO + XOR-shift PRNG for maximum throughput. */
static void demo_flood(void)
{
    unsigned long t0lo, t0hi, now_lo, now_hi;
    long count = 0, fps_count = 0;
    long long total_pixels = 0;
    char buf[80];
    int ch, loop_count = 0;
    int pending;

    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();

    cpu_str_c(4, "GPU Rectangle Flood - press any key to stop", 255, 1);

    /* Read BIOS timer tick at 0x40:0x6C for XOR-shift seed.
     * Suppress false GCC array-bounds warning from aggressive inlining. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Warray-bounds"
    g_xor_state = *(volatile unsigned long *)0x46CUL;
#pragma GCC diagnostic pop
    if (g_xor_state == 0) g_xor_state = 1;
    rdtsc_read(&t0lo, &t0hi);

    /* Set up 2D engine for batch fills (invariant regs written once) */
    gpu_fill_setup();
    pending = 0;

    while (1) {
        unsigned long rnd;
        int rx, ry, rw, rh;
        unsigned char rc;

        rnd = xorshift();
        rx = (int)(rnd & 0x3FF) % g_xres;
        ry = (int)((rnd >> 10) & 0x3FF) % g_yres + 14;
        rnd = xorshift();
        rw = (int)(rnd % 200) + 4;          /* 4-203, avg ~102 */
        rh = (int)((rnd >> 16) % 150) + 4;  /* 4-153, avg ~79 */
        rc = (unsigned char)(rnd >> 16);

        if (rx + rw > g_xres) rw = g_xres - rx;
        if (ry + rh > g_yres) rh = g_yres - ry;
        if (rw < 1 || rh < 1) continue;

        /* Batched FIFO: check once per 21 rects (21×3 = 63 entries) */
        if (pending == 0)
            gpu_wait_fifo(63);
        g_mmio[MMIO_DP_BRUSH_FRGD_CLR] = (unsigned long)rc;
        g_mmio[MMIO_DST_Y_X] = ((unsigned long)ry << 16) | (unsigned long)rx;
        g_mmio[MMIO_DST_HEIGHT_WIDTH] = ((unsigned long)rh << 16) | (unsigned long)rw;
        if (++pending >= 21) {
            g_fifo_free = 0;
            pending = 0;
        }

        count++;
        fps_count++;
        total_pixels += (long long)rw * rh;

        /* Check keyboard less frequently (every 128 rects) */
        if (++loop_count >= 128) {
            loop_count = 0;
            if (kbhit()) break;
        }

        /* Update counter every ~8000 rects */
        if (fps_count >= 8000) {
            double elapsed;
            double rps, mpps;
            g_fifo_free = 0;
            pending = 0;
            rdtsc_read(&now_lo, &now_hi);
            elapsed = tsc_to_ms(now_lo, now_hi, t0lo, t0hi) / 1000.0;
            if (elapsed <= 0) elapsed = 0.001;
            rps = count / elapsed;
            mpps = (double)total_pixels / elapsed / 1000000.0;

            gpu_wait_idle();
            gpu_fill(0, 0, g_xres, 13, 0);
            gpu_wait_idle();
            sprintf(buf, "Rects: %ld  %-.0f rects/s  %-.0f Mpix/s",
                    count, rps, mpps);
            cpu_str(4, 4, buf, 254, 1);

            /* Re-setup fast fill state after gpu_fill in header clear */
            gpu_fill_setup();
            pending = 0;

            fps_count = 0;
        }
    }
    ch = getch();
    (void)ch;
    g_fifo_free = 0;

    gpu_wait_idle();
    {
        double total_sec;
        rdtsc_read(&now_lo, &now_hi);
        total_sec = tsc_to_ms(now_lo, now_hi, t0lo, t0hi) / 1000.0;
        if (total_sec <= 0) total_sec = 0.001;
        gpu_fill(0, 0, g_xres, g_yres, 0);
        gpu_wait_idle();
        sprintf(buf, "Total: %ld rects in %.1f sec (%.0f rects/sec)",
                count, total_sec, count / total_sec);
        cpu_str_c(g_yres/2 - 20, buf, 254, 2);
        cpu_str_c(g_yres/2 + 20, "Press any key for blit demo, ESC to quit", 253, 1);
    }
}

/* GPU blit demo: bounce a block around the screen.
   Double-buffered with vsync page flipping for tear-free animation.
   Sprite source is kept in offscreen VRAM (page 2). */
static void demo_blit(void)
{
    int bw = 120, bh = 90;
    int bx, by, dx, dy;
    int i;
    int sprite_y;       /* offscreen row where sprite pattern lives */
    int back_page, back_y;
    long fps_count;
    clock_t fps_t0;
    double fps;
    char buf[80];

    /* Sprite source stored on page 2 (offscreen, beyond display pages) */
    sprite_y = g_page_stride * 2;

    /* Widen scissor so GPU fill/blit can reach offscreen rows */
    gpu_wait_fifo(1);
    wreg(R_SC_BOTTOM_RIGHT, (0x3FFFUL << 16) | (unsigned long)g_xres);

    /* Generate the rainbow-stripe sprite in offscreen VRAM */
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

    /* Ensure color compare is disabled (previous demo may leave state) */
    gpu_wait_fifo(1);
    wreg(R_CLR_CMP_CNTL, 0);

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

        /* Clear the back buffer (GPU fill is very fast) */
        gpu_fill(0, back_y, g_xres, g_yres, 0);

        /* Blit sprite from offscreen source to new position on back buffer */
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

        /* HUD text on back buffer */
        sprintf(buf, "GPU Blit Bounce  %.1f FPS  [ESC quit]", fps);
        cpu_str(4, back_y + g_yres - 14, buf, 253, 1);

        /* Wait for previous flip to complete, then issue new flip */
        flip_page(back_page);

        back_page ^= 1;
        bx = nx; by = ny;
    }
    getch();

    /* Restore display to page 0, reset scissor */
    flip_restore_page0();

    gpu_wait_fifo(1);
    wreg(R_SC_BOTTOM_RIGHT,
         ((unsigned long)g_yres << 16) | (unsigned long)g_xres);
}

/* =============================================================== */
/*  Demo 5: GPU parallax scrolling with color-key transparency      */
/*                                                                  */
/*  4 layers at different scroll speeds, composited each frame      */
/*  using GPU BLT.  Upper layers use source color-key (index 0) so */
/*  lower layers show through transparent regions.                  */
/*  Double-buffered via AVIVO D1GRPH hardware page flip.            */
/* =============================================================== */

#define PLAX_NLAYERS   4

/* Integer triangle wave.  t = input, period > 0, returns [-amp..+amp]. */
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

/* Blit one layer with horizontal wrapping (two BLTs when scroll
   crosses the edge).  use_key: 0=opaque, 1=color-key transparency. */
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

/* ----- Layer 0: star field / night sky (opaque, full screen) ----- */
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

/* ----- Layer 1: far mountains (gray, transparent above) ---------- */
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
                shade = 255;   /* snow cap on tallest peaks */
            g_lfb[(long)(base + y) * g_pitch + x] = (unsigned char)shade;
        }
    }
}

/* ----- Layer 2: green hills with trees (transparent above) ------- */
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

        /* Tree trunks every ~50 pixels on taller hills */
        if ((x % 50) < 2 && h > g_yres / 5) {
            int th = h / 4;
            for (y = top - th; y < top; y++)
                if (y >= 0)
                    g_lfb[(long)(base + y) * g_pitch + x] = 38;
            /* Canopy */
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

/* ----- Layer 3: city skyline (transparent above) ----------------- */
static void gen_cityscape(int base)
{
    int x, y, ground_h, bx, by;

    for (y = 0; y < g_yres; y++)
        memset(g_lfb + (long)(base + y) * g_pitch, PLAX_TRANSP, g_xres);

    ground_h = g_yres / 10;

    /* Ground strip */
    for (y = g_yres - ground_h; y < g_yres; y++) {
        int ci = 70 + (y - (g_yres - ground_h)) * 20 / ground_h;
        if (ci > 90) ci = 90;
        memset(g_lfb + (long)(base + y) * g_pitch, (unsigned char)ci, g_xres);
    }

    /* Buildings */
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

        /* Roof highlight */
        for (bx = x; bx < x + bw && bx < g_xres; bx++)
            if (top_y > 0)
                g_lfb[(long)(base + top_y) * g_pitch + bx] =
                    (unsigned char)(bc + 5 < 160 ? bc + 5 : 159);

        /* Windows (some lit, some dark) */
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

/* ----- Main parallax loop ---------------------------------------- */

/* Diagnostic grid layer generators.  Each draws a grid of hollow
   rectangles in a distinctive color on a transparent background.
   The grid spacing increases with depth (perspective cue) and each
   cell contains a label so misaligned layers are immediately obvious. */

static void gen_diag_layer(int base, int cell_w, int cell_h,
                           unsigned char border_c, unsigned char label_c,
                           const char *tag)
{
    int x, y, cx, cy;

    /* Fill entire layer with transparency */
    for (y = 0; y < g_yres; y++)
        memset(g_lfb + (long)(base + y) * g_pitch, PLAX_TRANSP, g_xres);

    /* Draw grid of hollow rectangles */
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

            /* Label inside cell: "A0" "A1" etc. (tag char + cell number) */
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
/*  Dune Chase: 16-layer mountain parallax + 128-segment worm      */
/* =============================================================== */

#define DUNE_NLAYERS   16
#define WORM_NSEGS    128
#define WORM_W         12
#define WORM_H         12
#define WORM_COLS      10
#define WORM_HIST     512

/* ----- Generate bright sky gradient (layer 0, opaque) ------------ */
static void gen_desert_sky(int base)
{
    int x, y, n, i, half;

    half = g_yres / 2;

    for (y = 0; y < g_yres; y++) {
        unsigned char c;
        if (y < half) {
            /* Upper half: deep indigo(1) -> rich blue(20) */
            c = (unsigned char)(1 + y * 19 / half);
            if (c > 20) c = 20;
        } else {
            /* Lower half: blue(20) -> pale horizon white(32) */
            c = (unsigned char)(20 + (y - half) * 12 / half);
            if (c > 32) c = 32;
        }
        memset(g_lfb + (long)(base + y) * g_pitch, c, g_xres);
    }

    /* Sparse stars in upper quarter */
    srand(12345);
    n = g_xres / 4;
    for (i = 0; i < n; i++) {
        x = rand() % g_xres;
        y = rand() % (g_yres / 5);
        g_lfb[(long)(base + y) * g_pitch + x] = 255;  /* white star */
    }
    /* A few bright yellow stars */
    for (i = 0; i < n / 3; i++) {
        x = rand() % g_xres;
        y = rand() % (g_yres / 4);
        g_lfb[(long)(base + y) * g_pitch + x] = 254;  /* yellow */
    }
}

/* ----- Generate a fractal mountain layer (layers 1-15, transparent above) */
static void gen_dune_layer(int base, int layer_idx)
{
    int x, y, h, top, step, seg, half, left, right, mid_x;
    int base_h, rough;
    unsigned char c_top, c_bot;
    /* height map for each column */
    static int hmap[832]; /* >= g_xres = 800, use g_pitch=832 max */

    /* Clear to transparent */
    for (y = 0; y < g_yres; y++)
        memset(g_lfb + (long)(base + y) * g_pitch, PLAX_TRANSP, g_xres);

    /* --- Fractal midpoint displacement for the skyline --- */
    srand((unsigned)(layer_idx * 7919 + 3571));

    /* Base height and roughness scale with layer index.
       layer 1 = farthest (tall, smooth peaks), layer 15 = closest (short, jagged) */
    base_h = g_yres * (50 - layer_idx * 2) / 100;
    rough  = g_yres * (2 + layer_idx) / 80;

    /* Initialize endpoints */
    for (x = 0; x < g_xres; x++)
        hmap[x] = base_h;

    /* Seed a few anchors */
    step = 128;
    if (step > g_xres) step = g_xres;
    for (x = 0; x < g_xres; x += step)
        hmap[x] = base_h + (rand() % (rough * 2 + 1)) - rough;
    hmap[0] = base_h + (rand() % (rough + 1));
    hmap[g_xres - 1] = base_h + (rand() % (rough + 1));

    /* Midpoint displacement passes */
    for (seg = step; seg >= 2; seg /= 2) {
        half = seg / 2;
        for (x = 0; x + seg < g_xres; x += seg) {
            left  = hmap[x];
            right = hmap[x + seg];
            mid_x = x + half;
            if (mid_x < g_xres) {
                int disp;
                disp = (rough * half) / step;
                if (disp < 1) disp = 1;
                hmap[mid_x] = (left + right) / 2
                             + (rand() % (disp * 2 + 1)) - disp;
            }
        }
    }

    /* Interpolate any remaining gaps (linear between set points) */
    for (x = 0; x < g_xres - 1; x++) {
        if (hmap[x] == base_h && x > 0) {
            /* already filled by displacement, keep */
        }
    }

    /* Clamp heights */
    for (x = 0; x < g_xres; x++) {
        if (hmap[x] < 8) hmap[x] = 8;
        if (hmap[x] > g_yres * 3 / 4) hmap[x] = g_yres * 3 / 4;
    }

    /* Color ranges per layer group */
    if (layer_idx <= 4) {
        /* Far: blue-grey / lavender (33-48) to purple (49-64) */
        c_top = (unsigned char)(33 + (layer_idx - 1) * 4);
        c_bot = (unsigned char)(c_top + 12);
        if (c_bot > 64) c_bot = 64;
    } else if (layer_idx <= 7) {
        /* Mid: purple(49-64) to forest green(65-80) */
        c_top = (unsigned char)(49 + (layer_idx - 5) * 5);
        c_bot = (unsigned char)(c_top + 14);
        if (c_bot > 80) c_bot = 80;
    } else if (layer_idx <= 11) {
        /* Near: green(65-80) to emerald(81-96) to brown(97-112) */
        c_top = (unsigned char)(65 + (layer_idx - 8) * 8);
        c_bot = (unsigned char)(c_top + 14);
        if (c_bot > 112) c_bot = 112;
    } else {
        /* Front: brown(113-128), ochre(129-144), crimson(145-160) */
        c_top = (unsigned char)(113 + (layer_idx - 12) * 12);
        c_bot = (unsigned char)(c_top + 14);
        if (c_bot > 160) c_bot = 160;
    }

    /* Draw columns from mountain skyline downward with gradient */
    for (x = 0; x < g_xres; x++) {
        h = hmap[x];
        top = g_yres - h;
        for (y = top; y < g_yres; y++) {
            int shade;
            shade = (int)c_top + (y - top) * ((int)c_bot - (int)c_top) / (h > 0 ? h : 1);
            if (shade < (int)c_top) shade = (int)c_top;
            if (shade > (int)c_bot) shade = (int)c_bot;
            g_lfb[(long)(base + y) * g_pitch + x] = (unsigned char)shade;
        }
    }
}

/* ----- Generate 128 worm ball sprites at staging page ------------- */
static void gen_worm_sprites(int base)
{
    int si, row, col, sx, sy, x, y, cx, cy, r2, dx, dy;
    unsigned char c;
    int rows_needed;

    /* Clear entire sprite area (WORM_COLS * WORM_W wide,
       ceil(WORM_NSEGS/WORM_COLS) * WORM_H tall) */
    rows_needed = ((WORM_NSEGS + WORM_COLS - 1) / WORM_COLS) * WORM_H;
    for (y = 0; y < rows_needed && (base + y) < g_page_stride * 20; y++)
        memset(g_lfb + (long)(base + y) * g_pitch, PLAX_TRANSP,
               WORM_COLS * WORM_W);

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
                    /* Head segment: add eyes */
                    if (si == 0) {
                        /* Eyes: two dark dots */
                        if ((dx == -2 && dy == -1) || (dx == 2 && dy == -1))
                            g_lfb[(long)(base + sy + y) * g_pitch + sx + x] = 0;
                        else if (dx * dx + dy * dy <= (r2 * 2 / 3))
                            g_lfb[(long)(base + sy + y) * g_pitch + sx + x] = 255;
                        else
                            g_lfb[(long)(base + sy + y) * g_pitch + sx + x] = 254;
                    } else {
                        /* Bright center, darker edge */
                        if (dx * dx + dy * dy <= r2 / 3)
                            g_lfb[(long)(base + sy + y) * g_pitch + sx + x] = 255;
                        else
                            g_lfb[(long)(base + sy + y) * g_pitch + sx + x] = c;
                    }
                }
            }
        }
    }
}

static void demo_dune_chase(void)
{
    int layer_base[DUNE_NLAYERS];
    unsigned long layer_off[DUNE_NLAYERS];
    unsigned long layer_po[DUNE_NLAYERS];
    int sprite_base;
    unsigned long sprite_off;
    unsigned long sprite_po_val;
    unsigned long stage_off;
    unsigned long stage_po;
    int back_page, back_y, scroll;
    long fps_count;
    clock_t fps_t0, t_render_start, t_render_end, t_frame_end;
    double fps, cpu_pct, cpu_acc;
    long cpu_samples;
    char buf[120];
    int i;
    unsigned long need;
    int sprite_blit_w, sprite_blit_h;

    /* Worm state */
    int worm_hx[WORM_HIST], worm_hy[WORM_HIST];
    int worm_head;
    int head_dx, head_dy;
    int scroll_x;
    int seg_row, seg_col, seg_sx, seg_sy;

    setup_dune_palette();

    need = (unsigned long)g_pitch * g_page_stride * 20;
    if (g_vram_mb > 0 && need > g_vram_mb * 1024UL * 1024UL) {
        gpu_fill(0, 0, g_xres, g_yres, 0);
        gpu_wait_idle();
        cpu_str_c(g_yres / 2, "Not enough VRAM for dune demo", 251, 2);
        cpu_str_c(g_yres / 2 + 30, "Press any key...", 253, 1);
        getch();
        setup_palette();
        return;
    }

    /* Compute layer offscreen row bases and VRAM byte offsets */
    for (i = 0; i < DUNE_NLAYERS; i++) {
        layer_base[i] = g_page_stride * (2 + i);
        layer_off[i]  = (unsigned long)layer_base[i] * g_pitch;
        layer_po[i]   = make_pitch_offset(layer_off[i]);
    }

    /* Sprite sheet at page 18 */
    sprite_base   = g_page_stride * 18;
    sprite_off    = (unsigned long)sprite_base * g_pitch;
    sprite_po_val = make_pitch_offset(sprite_off);

    /* Staging area = page 1 */
    stage_off = (unsigned long)g_page_stride * g_pitch;
    stage_po  = make_pitch_offset(stage_off);

    /* Widen scissor for offscreen ops */
    gpu_wait_fifo(1);
    wreg(R_SC_BOTTOM_RIGHT, (0x3FFFUL << 16) | (unsigned long)g_xres);

    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();
    cpu_str_c(g_yres / 2 - 20, "Generating 16-layer mountain landscape...", 255, 2);
    cpu_str_c(g_yres / 2 + 10, "Mountain Chase: 16 layers + 128-segment worm", 253, 1);

    /* Layer 0: sky (opaque) */
    gen_desert_sky(g_page_stride);
    gpu_blit_po(stage_po, 0, 0, layer_po[0], 0, 0, g_xres, g_yres);
    gpu_wait_idle();
    gpu_flush_2d_cache();

    /* Layers 1-15: fractal mountains */
    for (i = 1; i < DUNE_NLAYERS; i++) {
        gen_dune_layer(g_page_stride, i);
        gpu_blit_po(stage_po, 0, 0, layer_po[i], 0, 0, g_xres, g_yres);
        gpu_wait_idle();
        gpu_flush_2d_cache();
    }

    /* Worm sprites -- blit the entire sprite area */
    gen_worm_sprites(g_page_stride);
    sprite_blit_w = WORM_COLS * WORM_W;
    sprite_blit_h = ((WORM_NSEGS + WORM_COLS - 1) / WORM_COLS) * WORM_H;
    gpu_blit_po(stage_po, 0, 0, sprite_po_val, 0, 0,
                sprite_blit_w, sprite_blit_h);
    gpu_wait_idle();
    gpu_flush_2d_cache();

    /* Reset PITCH_OFFSET to default */
    gpu_wait_fifo(2);
    wreg(R_DST_PITCH_OFFSET, g_default_po);
    wreg(R_SRC_PITCH_OFFSET, g_default_po);

    /* Initialize worm: head starts mid-screen, moving diagonally */
    srand(9999);
    worm_head = 0;
    head_dx = 5;
    head_dy = 3;
    worm_hx[0] = g_xres / 2;
    worm_hy[0] = g_yres / 2;
    /* Pre-fill history so all segments start at same spot */
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

    while (!kbhit()) {
        back_y = back_page * g_page_stride;

        t_render_start = clock();

        /* Composite 16 layers with exponential scroll speeds.
           Layer 0 (far sky): very slow.  Layer 15 (front): ~8x faster. */
        plax_blit_wrap_po(layer_po[0], scroll / 32,
                          g_default_po, back_y, g_yres, 0);
        for (i = 1; i < DUNE_NLAYERS; i++) {
            scroll_x = (int)((long)scroll * (1 + (long)i * i) / 64);
            plax_blit_wrap_po(layer_po[i], scroll_x,
                              g_default_po, back_y, g_yres, 1);
        }

        /* Draw 128 worm segments (tail first so head draws on top) */
        for (i = WORM_NSEGS - 1; i >= 0; i--) {
            int hi, wx, wy;
            hi = (worm_head - i * 2 + WORM_HIST * 256) % WORM_HIST;
            wx = worm_hx[hi];
            wy = worm_hy[hi];
            seg_row = i / WORM_COLS;
            seg_col = i % WORM_COLS;
            seg_sx  = seg_col * WORM_W;
            seg_sy  = seg_row * WORM_H;
            gpu_blit_po_key(sprite_po_val, seg_sx, seg_sy,
                            g_default_po, wx, back_y + wy,
                            WORM_W, WORM_H, PLAX_TRANSP);
        }

        gpu_wait_idle();

        t_render_end = clock();

        /* Advance worm head */
        {
            int nx, ny;
            nx = worm_hx[worm_head] + head_dx;
            ny = worm_hy[worm_head] + head_dy;
            if (nx < 0)              { nx = 0;              head_dx = -head_dx; }
            if (nx > g_xres - WORM_W){ nx = g_xres - WORM_W; head_dx = -head_dx; }
            if (ny < 16)             { ny = 16;             head_dy = -head_dy; }
            if (ny > g_yres - WORM_H){ ny = g_yres - WORM_H; head_dy = -head_dy; }
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
        sprintf(buf, "Mountain Chase  16 layers + 128-seg worm  %.1f FPS  CPU:%d%%  [ESC quit]",
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
    getch();

    flip_restore_page0();

    gpu_wait_fifo(3);
    wreg(R_DST_PITCH_OFFSET, g_default_po);
    wreg(R_SRC_PITCH_OFFSET, g_default_po);
    wreg(R_SC_BOTTOM_RIGHT,
         ((unsigned long)g_yres << 16) | (unsigned long)g_xres);

    setup_palette();  /* restore standard palette after dune demo */
}

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

    gpu_wait_fifo(1);
    wreg(R_SC_BOTTOM_RIGHT, (0x3FFFUL << 16) | (unsigned long)g_xres);

    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();
    cpu_str_c(g_yres / 2 - 20, "Generating DIAGNOSTIC parallax layers...", 255, 2);
    cpu_str_c(g_yres / 2 + 10,
              "Layer 0=Blue  1=Red  2=Green  3=Yellow  grids", 253, 1);

    /* Layer 0 (sky/back): blue grid, large cells — opaque background */
    /* Fill with solid dark blue first (not transparent) */
    {
        int ly;
        for (ly = 0; ly < g_yres; ly++)
            memset(g_lfb + (long)(layer_base[0] + ly) * g_pitch, 2, g_xres);
    }
    /* Draw large grid on top */
    {
        int lx, ly;
        for (ly = 0; ly < g_yres; ly += 100)
            for (lx = 0; lx < g_xres; lx++)
                g_lfb[(long)(layer_base[0] + ly) * g_pitch + lx] = 20;
        for (lx = 0; lx < g_xres; lx += 100)
            for (ly = 0; ly < g_yres; ly++)
                g_lfb[(long)(layer_base[0] + ly) * g_pitch + lx] = 20;
        /* Label cells */
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

    /* Layer 1 (mountains): red hollow grid, 80px cells */
    gen_diag_layer(layer_base[1], 80, 80, 90, 75, "R");

    /* Layer 2 (hills): green hollow grid, 60px cells */
    gen_diag_layer(layer_base[2], 60, 60, 55, 45, "G");

    /* Layer 3 (city/front): yellow hollow grid, 40px cells */
    gen_diag_layer(layer_base[3], 40, 40, 155, 140, "Y");

    /* Clear both display pages */
    gpu_fill(0, 0, g_xres, g_page_stride * 2, 0);
    gpu_wait_idle();

    gpu_wait_fifo(1);
    wreg(R_CLR_CMP_CNTL, 0);

    back_page = 1;
    scroll    = 0;
    fps_count = 0;
    fps       = 0.0;
    fps_t0    = clock();

    while (!kbhit()) {
        back_y = back_page * g_page_stride;

        /* Same scroll speeds as scenic parallax */
        plax_blit_wrap(layer_base[0], scroll / 8,
                       back_y, g_yres, 0);  /* blue bg: opaque */
        plax_blit_wrap(layer_base[1], scroll / 4,
                       back_y, g_yres, 1);  /* red grid: keyed */
        plax_blit_wrap(layer_base[2], scroll / 2,
                       back_y, g_yres, 1);  /* green grid: keyed */
        plax_blit_wrap(layer_base[3], scroll,
                       back_y, g_yres, 1);  /* yellow grid: keyed */

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

        /* Legend at bottom */
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

    gpu_wait_fifo(1);
    wreg(R_SC_BOTTOM_RIGHT,
         ((unsigned long)g_yres << 16) | (unsigned long)g_xres);
}

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

    /* Verify VRAM can hold 2 display pages + 4 layer pages */
    need = (unsigned long)g_pitch * g_page_stride * 6;
    if (g_vram_mb > 0 && need > g_vram_mb * 1024UL * 1024UL) {
        gpu_fill(0, 0, g_xres, g_yres, 0);
        gpu_wait_idle();
        cpu_str_c(g_yres / 2, "Not enough VRAM for parallax demo", 251, 2);
        cpu_str_c(g_yres / 2 + 30, "Press any key...", 253, 1);
        getch();
        return;
    }

    /* Layer offscreen rows: page 2..5 (pages 0-1 are display buffers) */
    for (i = 0; i < PLAX_NLAYERS; i++)
        layer_base[i] = g_page_stride * (2 + i);

    /* Widen scissor so GPU ops can reach offscreen VRAM */
    gpu_wait_fifo(1);
    wreg(R_SC_BOTTOM_RIGHT, (0x3FFFUL << 16) | (unsigned long)g_xres);

    /* Show generation message on current visible page */
    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();
    cpu_str_c(g_yres / 2 - 20, "Generating parallax layers...", 255, 2);
    cpu_str_c(g_yres / 2 + 10, "Sky - Mountains - Hills - City", 253, 1);

    /* Procedurally generate 4 layers into offscreen VRAM */
    gen_sky(layer_base[0]);
    gen_mountains(layer_base[1]);
    gen_hills(layer_base[2]);
    gen_cityscape(layer_base[3]);

    /* Clear both display pages */
    gpu_fill(0, 0, g_xres, g_page_stride * 2, 0);
    gpu_wait_idle();

    /* Reset color compare to clean state before keyed blits */
    gpu_wait_fifo(1);
    wreg(R_CLR_CMP_CNTL, 0);

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

        /* Composite layers back-to-front into back buffer.
           Scroll speeds: sky 1/8, mountains 1/4, hills 1/2, city 1x */
        plax_blit_wrap(layer_base[0], scroll / 8,
                       back_y, g_yres, 0);  /* sky: opaque */
        plax_blit_wrap(layer_base[1], scroll / 4,
                       back_y, g_yres, 1);  /* mountains: keyed */
        plax_blit_wrap(layer_base[2], scroll / 2,
                       back_y, g_yres, 1);  /* hills: keyed */
        plax_blit_wrap(layer_base[3], scroll,
                       back_y, g_yres, 1);  /* city: keyed */

        gpu_wait_idle();

        t_render_end = clock();

        /* FPS counter (update every ~1 second) */
        fps_count++;
        {
            clock_t now = clock();
            double elapsed = (double)(now - fps_t0) / CLOCKS_PER_SEC;
            if (elapsed >= 1.0) {
                fps = (double)fps_count / elapsed;
                fps_count = 0;
                fps_t0 = now;
                /* Update CPU% average over this interval */
                if (cpu_samples > 0)
                    cpu_pct = cpu_acc / cpu_samples;
                cpu_acc = 0.0;
                cpu_samples = 0;
            }
        }

        /* HUD text (CPU writes directly to back buffer in LFB) */
        sprintf(buf, "GPU Parallax  %d layers  %.1f FPS  CPU:%d%%  [ESC quit]",
                PLAX_NLAYERS, fps, (int)(cpu_pct + 0.5));
        cpu_str(4, back_y + 4, buf, 255, 1);

        /* Page flip: atomic, tear-free (AVIVO hw or VBE fallback) */
        flip_page(back_page);

        t_frame_end = clock();

        /* Accumulate CPU utilization sample for this frame */
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
        if (scroll >= g_xres * 1000) scroll = 0;  /* prevent overflow */
    }
    getch();

    /* Restore display to page 0 */
    flip_restore_page0();

    /* Restore scissor */
    gpu_wait_fifo(1);
    wreg(R_SC_BOTTOM_RIGHT,
         ((unsigned long)g_yres << 16) | (unsigned long)g_xres);
}
/* =============================================================== */

int main(void)
{
    unsigned long bar0, bar2, bar3, pci_cmd;
    unsigned long lfb_sz;
    unsigned long rbbm_val;
    int ch;

    printf("RADEON.EXE - ATI Radeon X1300 Pro DOS Hardware Demo\n");
    printf("===================================================\n\n");

    /* Calibrate RDTSC for high-resolution timing (~220ms) */
    printf("Calibrating RDTSC timer...");
    calibrate_rdtsc();
    printf(" %.1f MHz\n", g_rdtsc_mhz);

    /* Allocate DOS transfer buffer */
    if (!dpmi_alloc(64)) {
        printf("ERROR: Cannot allocate DOS memory.\n");
        return 1;
    }

    /* --- PCI detection --- */
    printf("Scanning PCI bus for ATI RV515/RV516...\n");
    if (!pci_find_radeon()) {
        printf("ERROR: No Radeon X1300 (RV515/RV516) found.\n");
        printf("This demo requires actual Radeon hardware.\n");
        dpmi_free();
        return 1;
    }

    printf("\n  Card      : %s (PCI %02X:%02X.%X)\n",
           g_card_name, g_pci_bus, g_pci_dev, g_pci_func);
    printf("  Vendor/Dev: %04X:%04X\n", ATI_VID, g_pci_did);
    printf("  Revision  : %02X\n",
           (unsigned)(pci_rd32(g_pci_bus,g_pci_dev,g_pci_func,0x08) & 0xFF));
    printf("  Subsystem : %04X:%04X\n",
           pci_rd16(g_pci_bus,g_pci_dev,g_pci_func,0x2C),
           pci_rd16(g_pci_bus,g_pci_dev,g_pci_func,0x2E));

    /* --- Map MMIO registers (BAR2 on RV515 PCIe) ---
       RV515 PCIe BAR layout:
         BAR0 (0x10): VRAM aperture (256MB, 64-bit, prefetchable)
         BAR2 (0x18): MMIO registers (64KB, 64-bit, non-prefetchable)
         BAR4 (0x20): I/O ports (256 bytes)
       BAR0 is 64-bit and occupies PCI offsets 0x10+0x14,
       so BAR2 starts at PCI offset 0x18. */
    bar0 = pci_rd32(g_pci_bus, g_pci_dev, g_pci_func, 0x10);
    bar2 = pci_rd32(g_pci_bus, g_pci_dev, g_pci_func, 0x18);
    g_mmio_phys = bar2 & 0xFFFFFFF0UL;

    printf("  BAR0 (VRAM): 0x%08lX %s\n", bar0,
           (bar0 & 0x08) ? "(pref)" : "(non-pref)");

    /* Check if BAR2 is 64-bit (type field bits [2:1] == 10b) */
    if ((bar2 & 0x06) == 0x04) {
        bar3 = pci_rd32(g_pci_bus, g_pci_dev, g_pci_func, 0x1C);
        printf("  BAR2 (MMIO): 64-bit, lower=0x%08lX upper=0x%08lX\n",
               bar2, bar3);
        if (bar3 != 0) {
            printf("ERROR: MMIO mapped above 4GB (0x%08lX%08lX).\n",
                   bar3, g_mmio_phys);
            printf("Cannot access from 32-bit DOS extender.\n");
            dpmi_free();
            return 1;
        }
    } else {
        printf("  BAR2 (MMIO): 32-bit = 0x%08lX\n", bar2);
    }

    printf("  MMIO phys : 0x%08lX %s\n", g_mmio_phys,
           (bar2 & 0x08) ? "(prefetchable)" : "(non-prefetchable)");

    /* Map 64KB of MMIO (RV515 BAR2 register space) */
    g_mmio = (volatile unsigned long *)dpmi_map(g_mmio_phys, 0x10000);
    if (!g_mmio) {
        printf("ERROR: Cannot map MMIO registers.\n");
        dpmi_free();
        return 1;
    }
    printf("  MMIO lin  : 0x%08lX (mapped 64KB)\n",
           (unsigned long)g_mmio);

    /* Ensure PCI bus-master + memory access are enabled */
    pci_cmd = pci_rd32(g_pci_bus, g_pci_dev, g_pci_func, 0x04);
    if ((pci_cmd & 0x06) != 0x06) {
        pci_wr32(g_pci_bus, g_pci_dev, g_pci_func, 0x04,
                 pci_cmd | 0x06);  /* memory + bus-master */
        printf("  (Enabled PCI memory access + bus-master)\n");
    }

    /* --- Validate MMIO mapping by reading known registers --- */
    g_vram_mb = rreg(R_CONFIG_MEMSIZE) / (1024UL * 1024UL);
    rbbm_val = rreg(R_RBBM_STATUS);
    printf("  Video RAM : %lu MB\n", g_vram_mb);
    printf("  RBBM_STS  : 0x%08lX  (FIFO=%lu, %s)\n",
           rbbm_val, rbbm_val & RBBM_FIFOCNT_MASK,
           (rbbm_val & RBBM_ACTIVE) ? "BUSY" : "idle");

    /* Sanity check: VRAM should be 64-512 MB for X1300 */
    if (g_vram_mb == 0 || g_vram_mb > 1024) {
        printf("WARNING: VRAM size %lu MB looks wrong.\n", g_vram_mb);
        printf("  CONFIG_MEMSIZE=0x%08lX\n", rreg(R_CONFIG_MEMSIZE));
        printf("  BAR2 may not be MMIO. Run RDIAG for analysis.\n");
    }

    /* Print all BARs for diagnostics */
    {
        int i;
        printf("\n  PCI BARs:\n");
        for (i = 0; i < 6; i++) {
            unsigned long b = pci_rd32(g_pci_bus, g_pci_dev,
                                       g_pci_func, 0x10 + i*4);
            printf("    BAR%d: 0x%08lX", i, b);
            if (b & 1) printf(" (I/O)");
            else {
                printf(" (Mem, %s, %s)",
                       (b & 0x06)==0x04 ? "64-bit" : "32-bit",
                       (b & 0x08) ? "pref" : "non-pref");
            }
            printf("\n");
        }
    }

    /* --- Read GPU framebuffer base address (for 2D engine offsets) --- */
    {
        unsigned long hdp_fb, mc_fb;
        hdp_fb = rreg(R_HDP_FB_LOCATION);
        mc_fb  = mc_rreg(RV515_MC_FB_LOCATION);
        g_fb_location = (hdp_fb & 0xFFFFUL) << 16;
        printf("\n  HDP_FB_LOC: 0x%08lX  (FB base = 0x%08lX)\n",
               hdp_fb, g_fb_location);
        printf("  MC_FB_LOC : 0x%08lX  (indirect MC reg 0x01)\n", mc_fb);
        if (g_fb_location != 0)
            printf("  NOTE: Non-zero FB base — 2D engine offsets adjusted\n");
    }

    /* --- Additional GPU state diagnostics (from Linux rv515 debugfs) --- */
    {
        unsigned long gb_ps, gb_tc, dpc, vga_rc, isync, mc_st;
        int npipes;

        gb_ps  = rreg(R_GB_PIPE_SELECT);
        gb_tc  = rreg(R_GB_TILE_CONFIG);
        dpc    = rreg(R_DST_PIPE_CONFIG);
        vga_rc = rreg(R_VGA_RENDER_CONTROL);
        isync  = rreg(R_ISYNC_CNTL);
        mc_st  = mc_rreg(RV515_MC_STATUS);
        npipes = (int)((gb_ps >> 12) & 0x3) + 1;

        printf("\n  GPU State (pre-init):\n");
        printf("    GB_PIPE_SELECT : 0x%08lX  (%d pipe%s)\n",
               gb_ps, npipes, npipes > 1 ? "s" : "");
        printf("    GB_TILE_CONFIG : 0x%08lX\n", gb_tc);
        printf("    DST_PIPE_CONFIG: 0x%08lX\n", dpc);
        printf("    VGA_RENDER_CTL : 0x%08lX\n", vga_rc);
        printf("    ISYNC_CNTL     : 0x%08lX\n", isync);
        printf("    MC_STATUS      : 0x%08lX  (%s)\n",
               mc_st, (mc_st & MC_STATUS_IDLE) ? "idle" : "BUSY");
    }

    /* --- Find VESA mode --- */
    printf("\nLooking for 800x600 8bpp VESA mode...\n");
    if (!find_mode()) {
        printf("ERROR: No suitable VESA 8bpp LFB mode found.\n");
        dpmi_unmap((void *)g_mmio);
        dpmi_free();
        return 1;
    }
    printf("  Mode 0x%03X: %dx%d pitch=%d  LFB=0x%08lX\n",
           g_vmode, g_xres, g_yres, g_pitch, g_lfb_phys);
    printf("  VESA mem  : %lu KB (%lu MB)\n", g_vesa_mem_kb, g_vesa_mem_kb / 1024);

    /* Align pitch to 64 bytes — the Radeon 2D engine PITCH_OFFSET register
       encodes pitch in 64-byte units. A non-aligned VBE pitch (e.g. 800)
       causes GPU operations to use a different stride than the CRTC/CPU,
       producing sheared/shredded rendering on all GPU blits. */
    if (g_pitch & 63) {
        int old_pitch = g_pitch;
        g_pitch = (g_pitch + 63) & ~63;
        printf("  Pitch aligned: %d -> %d (64-byte GPU requirement)\n",
               old_pitch, g_pitch);
    }

    {
        unsigned long pitch64 = ((unsigned long)g_pitch + 63) / 64;
        unsigned long po = (pitch64 << 22) |
                           ((g_fb_location >> 10) & 0x003FFFFFUL);
        printf("  PITCH_OFFSET: 0x%08lX  (pitch64=%lu, offset=0x%lX)\n",
               po, pitch64, (g_fb_location >> 10) & 0x003FFFFFUL);
    }

    /* Compute page stride: smallest row count >= g_yres where
       stride * pitch is 4KB-aligned.  Required for AVIVO surface
       address register which needs 4KB alignment on RV515. */
    g_page_stride = g_yres;
    while (((long)g_page_stride * g_pitch) & 4095L)
        g_page_stride++;
    printf("  Page stride: %d rows (%ld bytes, 4KB-aligned)\n",
           g_page_stride, (long)g_page_stride * g_pitch);

    printf("\nPress any key to start graphics demo...\n");
    getch();

    /* --- Set graphics mode --- */
    g_font = get_font();

    if (!vbe_set(g_vmode)) {
        printf("ERROR: Cannot set VESA mode.\n");
        dpmi_unmap((void *)g_mmio);
        dpmi_free();
        return 1;
    }

    /* Map LFB for 2 display + 4 offscreen pages (6x); dune demo stages
       layers through page 1 and GPU-blits to higher offscreen pages. */
    lfb_sz = (unsigned long)g_pitch * g_page_stride * 6;
    if (g_vram_mb > 0 && lfb_sz > g_vram_mb * 1024UL * 1024UL)
        lfb_sz = g_vram_mb * 1024UL * 1024UL;
    lfb_sz = (lfb_sz + 4095UL) & ~4095UL;
    g_lfb = (unsigned char *)dpmi_map(g_lfb_phys, lfb_sz);
    if (!g_lfb) {
        vbe_text();
        printf("ERROR: Cannot map LFB.\n");
        dpmi_unmap((void *)g_mmio);
        dpmi_free();
        return 1;
    }

    setup_palette();
    gpu_init_2d();
    detect_flip_mode();

    /* If using VGA scanout (not AVIVO) and pitch was aligned, update
       the VGA CRTC offset register so display pitch matches GPU pitch.
       CRTC offset (CR13) = pitch / 8 in 256-color byte-mode. */
    if (!g_avivo_flip && (g_pitch != g_xres)) {
        unsigned char cr_val = (unsigned char)(g_pitch / 8);
        outp(0x3D4, 0x13);
        outp(0x3D5, cr_val);
    }

    /* Verify GPU 2D engine: read back PITCH_OFFSET and do a test fill */
    {
        unsigned long po = rreg(R_DST_PITCH_OFFSET);
        unsigned long rbbm;
        unsigned char before, after;

        /* Read back one pixel before GPU fill */
        before = g_lfb[0];
        /* GPU fill a single pixel at (0,0) with color 0xAA */
        gpu_fill(0, 0, 1, 1, 0xAA);
        gpu_wait_idle();
        after = g_lfb[0];

        /* If GPU wrote correctly, after should be 0xAA */
        if (after != 0xAA) {
            /* GPU fill didn't reach visible VRAM — show diagnostics */
            rbbm = rreg(R_RBBM_STATUS);
            vbe_text();
            printf("GPU 2D Engine Diagnostic:\n");
            printf("  DST_PITCH_OFFSET: 0x%08lX\n", po);
            printf("    pitch field = %lu (x64 = %lu bytes)\n",
                   (po >> 22) & 0xFF, ((po >> 22) & 0xFF) * 64UL);
            printf("    offset field = %lu (x1024 = 0x%08lX)\n",
                   po & 0x3FFFFFUL,
                   (po & 0x3FFFFFUL) * 1024UL);
            printf("  FB base       : 0x%08lX\n", g_fb_location);
            printf("  RBBM_STATUS   : 0x%08lX (FIFO=%lu, %s)\n",
                   rbbm, rbbm & RBBM_FIFOCNT_MASK,
                   (rbbm & RBBM_ACTIVE) ? "BUSY" : "idle");
            printf("  LFB pixel test: before=0x%02X after=0x%02X "
                   "(expected 0xAA)\n", before, after);
            printf("  GB_PIPE_SELECT: 0x%08lX  (%d pipes)\n",
                   rreg(R_GB_PIPE_SELECT), g_num_gb_pipes);
            printf("  GB_TILE_CONFIG: 0x%08lX\n", rreg(R_GB_TILE_CONFIG));
            printf("  ISYNC_CNTL    : 0x%08lX\n", rreg(R_ISYNC_CNTL));
            printf("  VGA_RENDER_CTL: 0x%08lX\n",
                   rreg(R_VGA_RENDER_CONTROL));
            printf("  MC_STATUS     : 0x%08lX\n",
                   mc_rreg(RV515_MC_STATUS));
            printf("\n  GPU 2D engine may not be functional.\n");
            printf("  Press any key to continue anyway, ESC to quit.\n");
            ch = getch();
            if (ch == 27) {
                dpmi_unmap(g_lfb);
                dpmi_unmap((void *)g_mmio);
                dpmi_free();
                return 1;
            }
            /* Re-enter graphics mode */
            if (!vbe_set(g_vmode)) {
                printf("ERROR: Cannot re-set VESA mode.\n");
                dpmi_unmap(g_lfb);
                dpmi_unmap((void *)g_mmio);
                dpmi_free();
                return 1;
            }
            setup_palette();
            gpu_init_2d();
            detect_flip_mode();
            if (!g_avivo_flip && (g_pitch != g_xres)) {
                unsigned char cr_val = (unsigned char)(g_pitch / 8);
                outp(0x3D4, 0x13);
                outp(0x3D5, cr_val);
            }
        }
    }

    /* === Demo 1: GPU-accelerated pattern === */
    demo_pattern();
    cpu_str_c(4, "GPU-Drawn Pattern (fill + line)", 255, 1);
    {
        char fbuf[80];
        sprintf(fbuf, "Flip: %s    Radeon 2D engine drew all shapes.  Press key...",
                g_avivo_flip ? "AVIVO hwflip" : "VBE+VGA vsync");
        cpu_str_c(g_yres - 14, fbuf, 253, 1);
    }
    ch = getch();

    if (ch != 27) {
        /* === Demo 2: Benchmark === */
        demo_benchmark();
        ch = getch();
    }

    if (ch != 27) {
        /* === Demo 3: GPU flood === */
        demo_flood();
        ch = getch();
    }

    if (ch != 27) {
        /* === Demo 4: Blit bounce === */
        demo_blit();
    }

    if (ch != 27) {
        ch = getch();
    }

    if (ch != 27) {
        /* === Demo 5: Dune Chase (16 layers + 16 UFOs) === */
        demo_dune_chase();
    }

    if (ch != 27) {
        /* === Demo 6: Diagnostic parallax (colored grids) === */
        demo_parallax_diag();
    }

    if (ch != 27) {
        /* === Demo 7: GPU parallax scrolling (scenic) === */
        demo_parallax();
    }

    /* --- Restore text mode, cleanup --- */
    vbe_text();

    printf("Radeon demo complete.\n");
    printf("Card: %s  |  VRAM: %lu MB  |  Mode: %dx%d  |  Pipes: %d\n",
           g_card_name, g_vram_mb, g_xres, g_yres, g_num_gb_pipes);

    dpmi_unmap(g_lfb);
    dpmi_unmap((void *)g_mmio);
    dpmi_free();

    return 0;
}
