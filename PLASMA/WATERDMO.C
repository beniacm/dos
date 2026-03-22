/*
 * WATERDMO.C — Water surface demo: lit plasma waves as 3-D water
 *
 * Renders the same four-wave interference pattern as the plasma demo,
 * but shaded as a water surface with directional NW lighting and
 * specular highlights.  Uses analytical gradients from the WAVE engine
 * (no finite-difference stencil).
 *
 * Uses VGA.H + WAVE.H + WATER.H libraries.
 * Pure C89 — compiles under DJGPP/GCC and OpenWatcom.
 *
 * Controls:
 *   V     = toggle vsync on/off
 *   ESC   = quit
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <conio.h>
#include "VGA.H"
#include "WATER.H"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

#define WIDTH   1024
#define HEIGHT  768
#define PIXELS  ((unsigned long)WIDTH * HEIGHT)

/* --------------------------------------------------------------------------
 * Startup performance benchmark
 * -------------------------------------------------------------------------- */

#define BENCH_FRAMES 100

#ifdef __DJGPP__
static void __attribute__((noinline, optimize("O2,no-unroll-loops")))
run_benchmark(unsigned char *lfb, int lfb_pitch)
#else
static void run_benchmark(unsigned char *lfb, int lfb_pitch)
#endif
{
    unsigned long lo0, hi0, lo1, hi1;
    unsigned char *tmp;
    int      i;

    tmp = (unsigned char *)malloc(PIXELS);
    if (!tmp) return;
    memset(tmp, 0x80, PIXELS);

    water_render(tmp, WIDTH, 0);
    blit_to_lfb(lfb, lfb_pitch, tmp);

    /* Render only */
    rdtsc_read(&lo0, &hi0);
    for (i = 0; i < BENCH_FRAMES; i++)
        water_render(tmp, WIDTH, (unsigned int)i * 3);
    rdtsc_read(&lo1, &hi1);
    g_bench_render_ms = tsc_to_ms(lo1, hi1, lo0, hi0) / BENCH_FRAMES;

    /* Blit only */
    rdtsc_read(&lo0, &hi0);
    for (i = 0; i < BENCH_FRAMES; i++)
        blit_to_lfb(lfb, lfb_pitch, tmp);
    rdtsc_read(&lo1, &hi1);
    g_bench_blit_ms = tsc_to_ms(lo1, hi1, lo0, hi0) / BENCH_FRAMES;

    /* Combined */
    rdtsc_read(&lo0, &hi0);
    for (i = 0; i < BENCH_FRAMES; i++) {
        water_render(tmp, WIDTH, (unsigned int)i * 3);
        blit_to_lfb(lfb, lfb_pitch, tmp);
    }
    rdtsc_read(&lo1, &hi1);
    g_bench_combined_ms = tsc_to_ms(lo1, hi1, lo0, hi0) / BENCH_FRAMES;

    free(tmp);
}

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    VBEInfo    vbi;
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
        if (stricmp(argv[i], "-nopmi") == 0 ||
            stricmp(argv[i], "/nopmi") == 0)  g_no_pmi = 1;
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

#ifdef __DJGPP__
    if (!__djgpp_nearptr_enable()) {
        printf("Failed to enable near pointer access\n");
        return 1;
    }
    if ((_get_cs() & 3) == 0) {
        if (expand_cs_to_4gb())
            printf("[0] CS limit expanded to 4 GB\n");
    }
    printf("[0] DJGPP nearptr enabled, SSE init...\n");
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
    printf("Searching for 1024x768 8bpp LFB mode...\n");

    if (!vbe_get_info(&vbi)) {
        dpmi_free_dos();
        printf("VBE info call failed\n");
        return 1;
    }
    if (vbi.vbe_version < 0x0200) {
        dpmi_free_dos();
        printf("VBE 2.0+ required (detected version %04X)\n", vbi.vbe_version);
        return 1;
    }

    g_vbe_version = vbi.vbe_version;
    g_vbe3        = (vbi.vbe_version >= 0x0300);
    if (g_force_vbe2)
        g_vbe3 = 0;

    printf("VBE version : %d.%d%s\n",
           vbi.vbe_version >> 8, vbi.vbe_version & 0xFF,
           g_vbe3 ? " (VBE 3.0)" :
           g_force_vbe2 ? " (VBE 3.0 disabled by -vbe2)" : "");
    printf("Video memory: %u KB\n", (unsigned)vbi.total_memory * 64);

    if (g_vbe3 && !g_no_pmi) {
        if ((_get_cs() & 3) == 0) {
            g_pmi_ok = query_pmi();
            if (g_pmi_ok)
                printf("PMI        : available\n");
        }
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
    printf("Found mode 0x%03X  LFB at 0x%08lX  pitch=%d\n",
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

    /* ---- Allocate system-RAM frame buffer ------------------------------ */
    if (!g_direct_vram) {
        frame_buf = (unsigned char *)malloc(PIXELS);
        if (!frame_buf) {
            dpmi_free_dos();
            printf("Out of memory (frame buffer)\n");
            return 1;
        }
    }

    water_init();

    /* ---- Map LFB ------------------------------------------------------- */
    {
        unsigned long map_size = use_doublebuf ? page_size * 2UL : page_size;
        lfb = (unsigned char *)dpmi_map_physical(lfb_phys, map_size);
    }
    if (!lfb) {
        if (frame_buf) free(frame_buf);
        dpmi_free_dos();
        printf("Cannot map LFB at 0x%08lX\n", lfb_phys);
        return 1;
    }

    /* ---- MTRR Write-Combining ------------------------------------------ */
    if (!g_no_mtrr) {
        unsigned long total_vram = (unsigned long)vbi.total_memory * 65536UL;
        int wc = setup_mtrr_wc(lfb_phys, total_vram);
        if (wc == 1)
            printf("MTRR WC    : enabled (slot %d)\n", g_mtrr_slot);
        else if (wc == -1)
            printf("MTRR WC    : already active\n");
    }
    if (g_mtrr_wc >= 1)
        setup_pat_uc_minus();
    if (g_mtrr_info) {
        unsigned long map_size = use_doublebuf ? page_size * 2UL : page_size;
        dump_mtrr_info((unsigned long)lfb, map_size);
    }

    /* ---- Set mode + feature tests -------------------------------------- */
    if (!vbe_set_mode(target_mode)) {
        dpmi_unmap_physical(lfb);
        if (frame_buf) free(frame_buf);
        dpmi_free_dos();
        printf("Set mode 0x%03X failed\n", target_mode);
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
                int sched_ok;
                if (g_pmi_ok)
                    sched_ok = pmi_schedule_display_start(0,
                                   (unsigned short)HEIGHT);
                else
                    sched_ok = vbe_schedule_display_start(0,
                                   (unsigned short)HEIGHT);
                if (sched_ok) {
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

    /* ---- Benchmark ----------------------------------------------------- */
    run_benchmark(lfb, lfb_pitch);

    /* Return to text mode for summary */
    vbe_set_text_mode();

    {
        const char *buf_str = g_direct_vram ?
            (use_doublebuf ? "double" : "single") :
            (use_doublebuf ? "triple" : "double");

        printf("\n");
        printf("==========================================\n");
        printf(" WATER — Lit Water Surface Demo\n");
        printf("==========================================\n");
        printf(" CPU          : %.0f MHz\n", g_rdtsc_mhz);
        printf(" VBE          : %d.%d\n",
               g_vbe_version >> 8, g_vbe_version & 0xFF);
        printf(" Video memory : %u KB\n", (unsigned)vbi.total_memory * 64);
        printf(" Mode         : 0x%03X  %dx%d %dbpp\n",
               target_mode, WIDTH, HEIGHT, 8);
        printf(" LFB          : 0x%08lX  pitch=%d\n",
               lfb_phys, lfb_pitch);
        printf(" DAC          : %d-bit\n", g_dac_bits);
        printf(" PMI          : %s\n", g_pmi_ok ? "yes" : "no");
        printf(" MTRR WC      : %s\n", g_mtrr_wc ? "yes" : "no");
        printf(" HW flip      : %s\n", g_hw_flip ? "yes" : "no");
        printf(" Buffering    : %s\n", buf_str);
        printf("==========================================\n");
        if (g_bench_combined_ms > 0.0) {
            double c_fps = 1000.0 / g_bench_combined_ms;
            printf(" Benchmark (%d frames)\n", BENCH_FRAMES);
            printf("  Render : %8.3f ms\n", g_bench_render_ms);
            printf("  Blit   : %8.3f ms\n", g_bench_blit_ms);
            printf("  Total  : %8.3f ms  %7.2f FPS\n",
                   g_bench_combined_ms, c_fps);
        }
        printf("==========================================\n");
        printf(" Controls: [V] toggle vsync  [ESC] quit\n");
        printf("==========================================\n");
        printf("\n Press any key to start...");
    }
    getch();

    /* ---- Set video mode (for the demo) --------------------------------- */
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

    /* ---- Palette ------------------------------------------------------- */
    water_build_palette(pal);
    find_hud_colours(pal);
    vbe_set_palette(0, 256, pal);

    font = get_bios_font_8x8();

    /* ---- Main loop ----------------------------------------------------- */
    t           = 0;
    vsync_on    = 1;
    running     = 1;
    frame_count = 0;
    last_ticks  = *bios_ticks;
    fps         = 0.0f;

    while (running) {

        /* --- Keyboard --------------------------------------------------- */
        while (kbhit()) {
            int k = getch();
            if (k == 27)
                running = 0;
            else if (k == 'v' || k == 'V')
                vsync_on = !vsync_on;
        }

        /* --- Render water ----------------------------------------------- */
        if (g_direct_vram) {
            unsigned char *dst = lfb + (use_doublebuf ?
                                  (unsigned long)back_page * page_size : 0UL);
            if (vsync_on) wait_vsync();
            water_render(dst, lfb_pitch, t);
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
            water_render(frame_buf, WIDTH, t);

            /* --- HUD overlay -------------------------------------------- */
            {
                const char *sync_str = vsync_on ?
                    (g_sched_flip ? "SCH" : "ON ") : "OFF";
                sprintf(msg, "WATER  VSYNC:%s  FPS:%5.1f  [V] [ESC]",
                        sync_str, fps);
                draw_str_bg(frame_buf, WIDTH, 4, 4, msg,
                            g_hud_bright, g_hud_dark, font);
            }

            /* --- Present frame ------------------------------------------ */
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

        /* --- Advance animation ------------------------------------------ */
        t += 2;
        frame_count++;

        /* --- FPS counter ------------------------------------------------ */
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

    printf("WATER done.\n");
    return 0;
}
