/*
 * WATERDRP.C — Water drop ripple simulation, 1024x768, top-down view
 *
 * Simulates concentric ripples on a still water surface using the 2D
 * wave equation with damping.  Camera looks straight down; a directional
 * light from the north-west at 45 degrees illuminates the surface.
 *
 * Drops fall periodically at random positions.  Press SPACE for a manual
 * drop, V to toggle vsync, ESC to quit.
 *
 * Uses VGA.H library for all VBE/DPMI/MTRR infrastructure.
 * Pure C89 — compiles under DJGPP/GCC and OpenWatcom.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <conio.h>
#include "VGA.H"

/* ======================================================================== *
 *  Constants                                                                *
 * ======================================================================== */

#define WIDTH   VGA_WIDTH
#define HEIGHT  VGA_HEIGHT
#define PIXELS  VGA_PIXELS

#define HEIGHT_SCALE    8       /* height -> palette divisor              */
#define SHADE_DIV       512     /* directional shade attenuation          */
#define SPEC_FLAT       80      /* specular FLAT parameter                */
#define DAMP_SHIFT      6       /* damping: lose 1/64 per frame (~1.5%)  */

/* Light from upper-left (NW) at 45-degree elevation */
#define LX  (-64)
#define LY  (-64)
#define LZ  ( 90)

/* Auto-drop timing */
#define DROP_INTERVAL   70      /* frames between auto-drops             */
#define DROP_FIRST      30      /* first drop after this many frames     */

/* ======================================================================== *
 *  Height field — two buffers for ping-pong wave equation                  *
 * ======================================================================== */

static short hbuf[2][HEIGHT][WIDTH];    /* 3 MB BSS */
static int   g_cur = 0;                /* current buffer index */

/* Simple PRNG (LCG) seeded from BIOS ticks */
static unsigned long g_rng = 12345;
static unsigned long rng_next(void)
{
    g_rng = g_rng * 1103515245UL + 12345UL;
    return (g_rng >> 16) & 0x7FFF;
}

/* ======================================================================== *
 *  Water drop: parabolic impulse in a circular area                        *
 * ======================================================================== */

static void water_drop(int cx, int cy, int radius, int strength)
{
    int dx, dy;
    int r2 = radius * radius;
    short (*h)[WIDTH] = hbuf[g_cur];

    for (dy = -radius; dy <= radius; dy++) {
        int py = cy + dy;
        if (py < 1 || py >= HEIGHT - 1) continue;
        for (dx = -radius; dx <= radius; dx++) {
            int px = cx + dx;
            int d2 = dx * dx + dy * dy;
            if (px < 1 || px >= WIDTH - 1) continue;
            if (d2 < r2) {
                /* Smooth parabolic profile: max at center, zero at edge */
                h[py][px] -= (short)(strength * (r2 - d2) / r2);
            }
        }
    }
}

/* ======================================================================== *
 *  Wave equation step with damping                                         *
 *                                                                          *
 *  new[x,y] = avg(4 neighbors)/2 - prev[x,y]                              *
 *  new[x,y] -= new[x,y] >> DAMP_SHIFT          (energy loss)              *
 *                                                                          *
 *  After the step, swap current and previous buffers.                      *
 * ======================================================================== */

static void wave_step(void)
{
    int x, y;
    short (*cur)[WIDTH]  = hbuf[g_cur];
    short (*prv)[WIDTH]  = hbuf[1 - g_cur];

    for (y = 1; y < HEIGHT - 1; y++) {
        for (x = 1; x < WIDTH - 1; x++) {
            int val = ((int)cur[y][x-1] + cur[y][x+1]
                     + cur[y-1][x]  + cur[y+1][x]) / 2
                    - prv[y][x];
            val -= val >> DAMP_SHIFT;
            prv[y][x] = (short)val;
        }
    }
    g_cur = 1 - g_cur;
}

/* ======================================================================== *
 *  Shade the height field to an 8-bpp frame buffer                         *
 *                                                                          *
 *  base  = 128 + h/HEIGHT_SCALE   (centred, ripples are ±)                 *
 *  shade = dot(normal_xy, light_xy) / SHADE_DIV                            *
 *  spec  = Rz^4 Phong specular                                             *
 *  idx   = clamp(base + shade + spec>>2, 0, 255)                           *
 * ======================================================================== */

static void shade_frame(unsigned char *buf, int pitch)
{
    int x, y;
    short (*h)[WIDTH] = hbuf[g_cur];

    for (y = 1; y < HEIGHT - 1; y++) {
        unsigned char *dst = buf + (unsigned long)y * pitch;
        for (x = 1; x < WIDTH - 1; x++) {
            int base, nx, ny, dir, shade, dot_raw, rz, sp, idx;

            base = 128 + h[y][x] / HEIGHT_SCALE;
            if (base < 0)   base = 0;
            if (base > 255) base = 255;

            /* Surface normal from finite differences (1-pixel stencil) */
            nx = (int)h[y][x-1] - h[y][x+1];
            ny = (int)h[y-1][x] - h[y+1][x];

            /* Directional shade */
            dir = nx * LX + ny * LY;
            shade = dir / SHADE_DIV;

            /* Specular: Rz = 2 * dot_full / FLAT - LZ */
            dot_raw = dir + SPEC_FLAT * LZ;
            rz = (2 * dot_raw) / SPEC_FLAT - LZ;
            if (rz < 0)   rz = 0;
            if (rz > 255) rz = 255;
            sp = (rz * rz) >> 8;
            sp = (sp * sp) >> 8;

            idx = base + shade + (sp >> 2);
            if (idx < 0)   idx = 0;
            if (idx > 255) idx = 255;

            dst[x] = (unsigned char)idx;
        }
        /* Border pixels: copy nearest interior pixel */
        dst[0] = dst[1];
        dst[WIDTH - 1] = dst[WIDTH - 2];
    }
    /* Top and bottom border rows */
    memcpy(buf, buf + pitch, WIDTH);
    memcpy(buf + (unsigned long)(HEIGHT - 1) * pitch,
           buf + (unsigned long)(HEIGHT - 2) * pitch, WIDTH);
}

/* ======================================================================== *
 *  Water palette: deep ocean blue → turquoise → white foam                 *
 * ======================================================================== */

static void build_drop_palette(unsigned char *pal)
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

/* ======================================================================== *
 *  Main                                                                     *
 * ======================================================================== */

int main(int argc, char *argv[])
{
    VBEInfo     vbi;
    VBEModeInfo vbmi;
    unsigned short target_mode;
    int            lfb_pitch;
    unsigned long  lfb_phys;
    unsigned char *lfb;
    unsigned char *frame_buf = NULL;
    unsigned char  pal[256*4];
    unsigned char *font;
    char           msg[96];
    unsigned int   t;
    int            vsync_on, running;
    int            use_doublebuf;
    int            back_page;
    unsigned long  page_size;
    unsigned long  frame_count, last_ticks, now, elapsed;
    float          fps;
    volatile unsigned long *bios_ticks;
    int            i;

#ifdef __DJGPP__
    bios_ticks = (volatile unsigned long *)(0x0046CUL + __djgpp_conventional_base);
#else
    bios_ticks = (volatile unsigned long *)0x46CUL;
#endif

    /* Parse command-line flags */
    for (i = 1; i < argc; i++) {
        if (stricmp(argv[i], "-vbe2") == 0 ||
            stricmp(argv[i], "/vbe2") == 0)   g_force_vbe2 = 1;
        if (stricmp(argv[i], "-pmi") == 0 ||
            stricmp(argv[i], "/pmi") == 0)    g_no_pmi = 0;
        if (stricmp(argv[i], "-nomtrr") == 0 ||
            stricmp(argv[i], "/nomtrr") == 0) g_no_mtrr = 1;
        if (stricmp(argv[i], "-directvram") == 0 ||
            stricmp(argv[i], "/directvram") == 0) g_direct_vram = 1;
        if (stricmp(argv[i], "-hwflip") == 0 ||
            stricmp(argv[i], "/hwflip") == 0) g_hw_flip = 1;
        if (stricmp(argv[i], "-mtrrinfo") == 0 ||
            stricmp(argv[i], "/mtrrinfo") == 0) g_mtrr_info = 1;
        if (stricmp(argv[i], "-sched") == 0 ||
            stricmp(argv[i], "/sched") == 0)  g_sched_req = 1;
    }

    /* ---- DJGPP-specific setup ------------------------------------------ */
#ifdef __DJGPP__
    if (!__djgpp_nearptr_enable()) {
        printf("Failed to enable near pointer access\n");
        return 1;
    }
    if ((_get_cs() & 3) == 0)
        expand_cs_to_4gb();
    djgpp_enable_sse();
#endif
    if (!dpmi_alloc_dos(128)) {
        printf("DPMI: cannot allocate conventional memory\n");
        return 1;
    }

    printf("Calibrating CPU frequency...\n");
    calibrate_rdtsc();
    printf("CPU: %.0f MHz\n", g_rdtsc_mhz);

    /* ---- Locate 1024x768 8bpp LFB mode -------------------------------- */
    if (!vbe_get_info(&vbi)) {
        dpmi_free_dos();
        printf("VBE info call failed\n");
        return 1;
    }
    if (vbi.vbe_version < 0x0200) {
        dpmi_free_dos();
        printf("VBE 2.0+ required (detected %04X)\n", vbi.vbe_version);
        return 1;
    }

    g_vbe_version = vbi.vbe_version;
    g_vbe3        = (vbi.vbe_version >= 0x0300);
    if (g_force_vbe2) g_vbe3 = 0;

    if (g_vbe3 && !g_no_pmi) {
        if ((_get_cs() & 3) == 0)
            g_pmi_ok = query_pmi();
    }

    {
        unsigned short  mode_list[512];
        unsigned short *src;
        int             count;
        unsigned long   ml_seg = (vbi.video_mode_ptr >> 16) & 0xFFFF;
        unsigned long   ml_off =  vbi.video_mode_ptr        & 0xFFFF;

#ifdef __DJGPP__
        src = (unsigned short *)(ml_seg * 16 + ml_off + __djgpp_conventional_base);
#else
        src = (unsigned short *)(ml_seg * 16 + ml_off);
#endif
        for (count = 0; count < 511 && src[count] != 0xFFFF; count++)
            mode_list[count] = src[count];
        mode_list[count] = 0xFFFF;

        target_mode = 0xFFFF;
        lfb_phys    = 0;
        lfb_pitch   = WIDTH;

        for (i = 0; i < count && target_mode == 0xFFFF; i++) {
            if (!vbe_get_mode_info(mode_list[i], &vbmi)) continue;
            if (vbmi.x_resolution       != WIDTH)  continue;
            if (vbmi.y_resolution       != HEIGHT) continue;
            if (vbmi.bits_per_pixel     != 8)      continue;
            if (!(vbmi.mode_attributes & 0x01))    continue;
            if (!(vbmi.mode_attributes & 0x10))    continue;
            if (!(vbmi.mode_attributes & 0x80))    continue;
            if (!vbmi.phys_base_ptr)               continue;
            target_mode = mode_list[i];
            lfb_phys    = vbmi.phys_base_ptr;
            lfb_pitch   = vbmi.bytes_per_scan_line;
        }
    }

    if (target_mode == 0xFFFF) {
        dpmi_free_dos();
        printf("1024x768 8bpp LFB mode not found\n");
        return 1;
    }
    printf("Mode 0x%03X  LFB 0x%08lX  pitch=%d\n",
           target_mode, lfb_phys, lfb_pitch);

    /* ---- Buffering ----------------------------------------------------- */
    page_size     = (unsigned long)lfb_pitch * HEIGHT;
    use_doublebuf = 0;
    back_page     = 0;

    {
        unsigned long total_vram = (unsigned long)vbi.total_memory * 65536UL;
        if (total_vram >= page_size * 2UL)
            use_doublebuf = 1;
    }

    /* ---- Frame buffer -------------------------------------------------- */
    if (!g_direct_vram) {
        frame_buf = (unsigned char *)malloc(PIXELS);
        if (!frame_buf) {
            dpmi_free_dos();
            printf("Out of memory\n");
            return 1;
        }
    }

    /* ---- Map LFB ------------------------------------------------------- */
    {
        unsigned long map_size = use_doublebuf ? page_size * 2UL : page_size;
        lfb = (unsigned char *)dpmi_map_physical(lfb_phys, map_size);
    }
    if (!lfb) {
        if (frame_buf) free(frame_buf);
        dpmi_free_dos();
        printf("Cannot map LFB\n");
        return 1;
    }

    /* ---- MTRR WC + PAT ------------------------------------------------ */
    if (!g_no_mtrr) {
        unsigned long total_vram = (unsigned long)vbi.total_memory * 65536UL;
        setup_mtrr_wc(lfb_phys, total_vram);
    }
    if (g_mtrr_wc >= 1)
        setup_pat_uc_minus();
    if (g_mtrr_info) {
        unsigned long map_size = use_doublebuf ? page_size * 2UL : page_size;
        dump_mtrr_info((unsigned long)lfb, map_size);
    }

    /* ---- Set mode + feature test --------------------------------------- */
    if (!vbe_set_mode(target_mode)) {
        dpmi_unmap_physical(lfb);
        if (frame_buf) free(frame_buf);
        dpmi_free_dos();
        printf("Set mode failed\n");
        return 1;
    }
    init_dac();

    if (use_doublebuf) {
        if (!vbe_set_display_start(0, 0, 0)) {
            use_doublebuf = 0;
        } else {
            if (g_pmi_ok && !pmi_set_display_start(0, 0, 0))
                g_pmi_ok = 0;
            if (g_vbe3 && g_sched_req) {
                int sok = g_pmi_ok
                    ? pmi_schedule_display_start(0, (unsigned short)HEIGHT)
                    : vbe_schedule_display_start(0, (unsigned short)HEIGHT);
                if (sok) {
                    g_sched_flip = 1;
                    if (g_pmi_ok) {
                        while (!pmi_is_flip_complete()) {}
                        pmi_set_display_start(0, 0, 0);
                    } else {
                        while (!vbe_is_flip_complete()) {}
                        vbe_set_display_start(0, 0, 0);
                    }
                }
            }
        }
    }

    if (g_hw_flip && use_doublebuf) {
        if ((_get_cs() & 3) != 0 || !detect_gpu_iobase(lfb_phys)) {
            g_hw_flip = 0;
        } else {
            unsigned long cur = gpu_reg_read(R5_D1GRPH_PRIMARY_SURFACE_ADDRESS);
            if ((cur & 0xFFF00000UL) != (lfb_phys & 0xFFF00000UL))
                g_hw_flip = 0;
            else
                g_fb_phys = lfb_phys;
        }
    } else {
        g_hw_flip = 0;
    }

    /* Return to text for status */
    vbe_set_text_mode();
    printf("\n=== WATERDRP — Water Drop Ripple Demo ===\n");
    printf("CPU: %.0f MHz  VBE: %d.%d  VRAM: %uKB\n",
           g_rdtsc_mhz,
           g_vbe_version >> 8, g_vbe_version & 0xFF,
           (unsigned)vbi.total_memory * 64);
    printf("Mode: 0x%03X  Buf: %s  PMI: %s  WC: %s\n",
           target_mode,
           use_doublebuf ? "triple" : "double",
           g_pmi_ok ? "yes" : "no",
           g_mtrr_wc ? "yes" : "no");
    printf("Controls: [SPACE] drop  [V] vsync  [ESC] quit\n");
    printf("Press any key to start...");
    getch();

    /* ---- Set video mode for demo --------------------------------------- */
    if (!vbe_set_mode(target_mode)) {
        dpmi_unmap_physical(lfb);
        if (frame_buf) free(frame_buf);
        dpmi_free_dos();
        return 1;
    }
    init_dac();
    if (use_doublebuf) {
        vbe_set_display_start(0, 0, 0);
        back_page = 1;
    }

    build_drop_palette(pal);
    find_hud_colours(pal);
    vbe_set_palette(0, 256, pal);
    font = get_bios_font_8x8();

    /* Seed RNG from BIOS ticks */
    g_rng = *bios_ticks;

    /* Clear height buffers */
    memset(hbuf, 0, sizeof(hbuf));

    /* ---- Main loop ----------------------------------------------------- */
    t           = 0;
    vsync_on    = 1;
    running     = 1;
    frame_count = 0;
    last_ticks  = *bios_ticks;
    fps         = 0.0f;

    while (running) {
        int manual_drop = 0;

        /* --- Keyboard --------------------------------------------------- */
        while (kbhit()) {
            int k = getch();
            if (k == 27)
                running = 0;
            else if (k == 'v' || k == 'V')
                vsync_on = !vsync_on;
            else if (k == ' ')
                manual_drop = 1;
        }

        /* --- Auto-drops at random positions ----------------------------- */
        if (manual_drop) {
            int cx = 80 + (int)(rng_next() % (WIDTH - 160));
            int cy = 80 + (int)(rng_next() % (HEIGHT - 160));
            int r  = 10 + (int)(rng_next() % 12);
            int s  = 400 + (int)(rng_next() % 400);
            water_drop(cx, cy, r, s);
        }

        if (t == DROP_FIRST) {
            /* First drop at centre */
            water_drop(WIDTH / 2, HEIGHT / 2, 16, 600);
        } else if (t > DROP_FIRST && ((t - DROP_FIRST) % DROP_INTERVAL == 0)) {
            int cx = 100 + (int)(rng_next() % (WIDTH - 200));
            int cy = 100 + (int)(rng_next() % (HEIGHT - 200));
            int r  = 8 + (int)(rng_next() % 14);
            int s  = 300 + (int)(rng_next() % 500);
            water_drop(cx, cy, r, s);
        }
        /* Second staggered drop stream for overlapping interference */
        if (t > DROP_FIRST + DROP_INTERVAL / 2
            && ((t - DROP_FIRST - DROP_INTERVAL / 2) % DROP_INTERVAL == 0)) {
            int cx = 150 + (int)(rng_next() % (WIDTH - 300));
            int cy = 150 + (int)(rng_next() % (HEIGHT - 300));
            int r  = 6 + (int)(rng_next() % 10);
            int s  = 200 + (int)(rng_next() % 400);
            water_drop(cx, cy, r, s);
        }

        /* --- Physics step ----------------------------------------------- */
        wave_step();

        /* --- Render ----------------------------------------------------- */
        if (g_direct_vram) {
            unsigned char *dst = lfb + (use_doublebuf ?
                (unsigned long)back_page * page_size : 0UL);
            if (vsync_on) wait_vsync();
            shade_frame(dst, lfb_pitch);
            if (use_doublebuf) {
                if (g_hw_flip)
                    hw_page_flip(g_fb_phys +
                        (unsigned long)back_page * page_size);
                else if (g_pmi_ok)
                    pmi_set_display_start(0,
                        (unsigned short)(back_page * HEIGHT), 0);
                else
                    vbe_set_display_start(0,
                        (unsigned short)(back_page * HEIGHT), 0);
                back_page = 1 - back_page;
            }
        } else {
            shade_frame(frame_buf, WIDTH);

            /* HUD */
            sprintf(msg, "WATERDRP  VSYNC:%s  FPS:%5.1f  [SPACE] [V] [ESC]",
                    vsync_on ? "ON " : "OFF", fps);
            draw_str_bg(frame_buf, WIDTH, 4, 4, msg,
                        g_hud_bright, g_hud_dark, font);

            /* Present */
            if (use_doublebuf) {
                if (g_hw_flip) {
                    if (vsync_on)
                        while (!hw_is_flip_done()) {}
                    blit_to_lfb(lfb + (unsigned long)back_page * page_size,
                                lfb_pitch, frame_buf);
                    hw_page_flip(g_fb_phys +
                        (unsigned long)back_page * page_size);
                } else if (g_sched_flip) {
                    if (g_pmi_ok)
                        while (!pmi_is_flip_complete()) {}
                    else
                        while (!vbe_is_flip_complete()) {}
                    blit_to_lfb(lfb + (unsigned long)back_page * page_size,
                                lfb_pitch, frame_buf);
                    if (vsync_on) {
                        if (g_pmi_ok)
                            pmi_schedule_display_start(0,
                                (unsigned short)(back_page * HEIGHT));
                        else
                            vbe_schedule_display_start(0,
                                (unsigned short)(back_page * HEIGHT));
                    } else {
                        if (g_pmi_ok)
                            pmi_set_display_start(0,
                                (unsigned short)(back_page * HEIGHT), 0);
                        else
                            vbe_set_display_start(0,
                                (unsigned short)(back_page * HEIGHT), 0);
                    }
                } else {
                    blit_to_lfb(lfb + (unsigned long)back_page * page_size,
                                lfb_pitch, frame_buf);
                    if (g_pmi_ok)
                        pmi_set_display_start(0,
                            (unsigned short)(back_page * HEIGHT), 0);
                    else
                        vbe_set_display_start(0,
                            (unsigned short)(back_page * HEIGHT), 0);
                    if (vsync_on)
                        wait_vsync();
                }
                back_page = 1 - back_page;
            } else {
                if (vsync_on)
                    wait_vsync();
                blit_to_lfb(lfb, lfb_pitch, frame_buf);
            }
        }

        t++;
        frame_count++;

        now     = *bios_ticks;
        elapsed = now - last_ticks;
        if (elapsed >= 18UL) {
            fps         = (float)frame_count * 18.2f / (float)elapsed;
            frame_count = 0;
            last_ticks  = now;
        }
    }

    /* ---- Cleanup ------------------------------------------------------- */
    vbe_set_text_mode();
    restore_pat();
    restore_mtrr();
    dpmi_unmap_physical(lfb);
    if (frame_buf) free(frame_buf);
    dpmi_free_dos();
#ifdef __DJGPP__
    __djgpp_nearptr_disable();
#endif

    printf("WATERDRP done.\n");
    return 0;
}
