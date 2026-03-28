/*
 * PLASMA.C  -  8-bit VESA plasma effect, 1024x768, vsync demonstration
 * OpenWatcom 2.0, 32-bit DOS/4GW
 *
 * Demonstrates:
 *   - Classic 4-component plasma (horizontal, vertical, diagonal, radial waves)
 *   - VBE 2.0+ linear frame buffer in 1024x768 8bpp
 *   - VBE 4F09h palette programming (works on Radeon in VESA modes)
 *   - VBE 2.0 hardware page flip via INT 10h AX=4F07h — triple-buffered:
 *     sysram render buffer + 2 VRAM pages (render→sysram, blit→back, flip)
 *   - VBE 3.0 Protected Mode Interface (PMI) for faster page flip via
 *     DPMI 0301h direct procedure call (bypasses INT 10h dispatch)
 *   - Falls back to double-buffer (sysram+blit, no flip) if VRAM < 2 pages
 *     or 4F07h fails
 *   - Vertical retrace sync to eliminate frame tearing
 *
 * Performance note:
 *   Rendering is always done into cached system RAM, then blitted to VRAM.
 *   Direct VRAM writes are extremely slow (~100ns/byte uncached), which
 *   limited frame rate to ~13 FPS on real hardware.  The system RAM + blit
 *   approach uses fast cached writes and WC-friendly sequential memcpy.
 *
 * Controls:
 *   V     = toggle vsync on/off  (watch for tear line when OFF)
 *   ESC   = quit
 *
 * Options:
 *   -vbe2       = force VBE 2.0 mode (disable VBE 3.0 features)
 *   -pmi        = enable VBE 3.0 Protected Mode Interface
 *   -nopmi      = disable PMI (default)
 *   -nomtrr     = skip MTRR write-combining setup
 *   -mtrrinfo   = show detailed MTRR/PAT/PTE diagnostic dump
 *   -directvram = render directly to VRAM (slow, for benchmarking)
 *   -hwflip     = direct GPU register page flip (tear-free, bypasses BIOS)
 *
 * Build:
 *   wcc386 -bt=dos -3r -ox -s -zq PLASMA.C
 *   wlink system dos4g name PLASMA file PLASMA option quiet
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <conio.h>
#include "VGA.H"
#include "WAVE.H"

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

#define WIDTH   1024
#define HEIGHT  768
#define PIXELS  ((unsigned long)WIDTH * HEIGHT)


/* --------------------------------------------------------------------------
 * Plasma engine
 *
 * Uses four overlapping sine waves:
 *   wave 1: horizontal (x)
 *   wave 2: vertical   (y)
 *   wave 3: diagonal   (x+y)
 *   wave 4: radial     (distance from centre)
 *
 * Each wave is sampled from a precomputed 256-entry sine table.
 * The four 8-bit values are averaged to a single palette index.
 * -------------------------------------------------------------------------- */


static void init_plasma_tables(void)
{
    wave_init();
}

/*
 * render_plasma: fill `buf` (WIDTH x HEIGHT, pitch = WIDTH) for frame `t`.
 * Each component uses a slightly different speed multiplier so the waves
 * drift independently and produce interesting interference patterns.
 */
static void render_plasma(unsigned char *buf, int pitch, unsigned int t)
{
    int x, y;
    unsigned int t1 = (t)         & 0xFF;   /* base speed                  */
    unsigned int t2 = (t + t/2)   & 0xFF;   /* 1.5x                        */
    unsigned int t3 = (t*2)       & 0xFF;   /* 2x – diagonal               */
    unsigned int t4 = (t*3)       & 0xFF;   /* 3x – radial (fast ripple)   */
    short hrow[WIDTH];

    for (y = 0; y < HEIGHT; y++) {
        unsigned char *dst = buf + (unsigned long)y * pitch;
        wave_fill_row(hrow, y, t1, t2, t3, t4);
        for (x = 0; x < WIDTH; x++)
            dst[x] = (unsigned char)(hrow[x] >> 2);
    }
}

/* --------------------------------------------------------------------------
 * Plasma palette: 256-entry smooth triple-phase RGB cycle
 * -------------------------------------------------------------------------- */

static void build_plasma_palette(unsigned char *pal)
{
    int i;
    double t;

    for (i = 0; i < 256; i++) {
        t = i * 6.283185307 / 256.0;
        pal[i*4+0] = (unsigned char)((sin(t)                + 1.0) * 127.5); /* R */
        pal[i*4+1] = (unsigned char)((sin(t + 2.094395102)  + 1.0) * 127.5); /* G */
        pal[i*4+2] = (unsigned char)((sin(t + 4.188790205)  + 1.0) * 127.5); /* B */
        pal[i*4+3] = 0;
    }
}


/* --------------------------------------------------------------------------
 * Startup performance benchmark
 *
 * Runs before the demo loop to show where the time goes.
 * Called after LFB is mapped and MTRR WC is set up.
 * Measures render, blit, and render+blit in isolation.
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

    /* Warm up */
    render_plasma(tmp, WIDTH, 0);
    blit_to_lfb(lfb, lfb_pitch, tmp);

    /* Render only */
    rdtsc_read(&lo0, &hi0);
    for (i = 0; i < BENCH_FRAMES; i++)
        render_plasma(tmp, WIDTH, (unsigned int)i * 3);
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
        render_plasma(tmp, WIDTH, (unsigned int)i * 3);
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
    unsigned char *frame_buf = NULL; /* system-RAM render buffer          */
    unsigned char  pal[256*4];
    unsigned char *font;
    char           msg[96];
    unsigned int   t;
    int            vsync_on, running;
    int            use_doublebuf;   /* 1 = VBE 4F07h page flip active   */
    int            back_page;       /* 0 or 1                           */
    unsigned long  page_size;       /* bytes per VRAM page              */
    unsigned long  frame_count, last_ticks, now, elapsed;
    float          fps;
    volatile unsigned long *bios_ticks;
    int            i;

    /* BIOS timer-tick counter at flat address 0x46C  (~18.2 ticks/sec) */
#ifdef __DJGPP__
    bios_ticks = (volatile unsigned long *)(0x0046CUL + __djgpp_conventional_base);
#else
    bios_ticks = (volatile unsigned long *)0x46CUL;
#endif

    /* Parse command-line flags */
    for (i = 1; i < argc; i++) {
        if (stricmp(argv[i], "-vbe2") == 0 ||
            stricmp(argv[i], "/vbe2") == 0) {
            g_force_vbe2 = 1;
        }
        if (stricmp(argv[i], "-pmi") == 0 ||
            stricmp(argv[i], "/pmi") == 0) {
            g_no_pmi = 0;
        }
        if (stricmp(argv[i], "-nopmi") == 0 ||
            stricmp(argv[i], "/nopmi") == 0) {
            g_no_pmi = 1;
        }
        if (stricmp(argv[i], "-nomtrr") == 0 ||
            stricmp(argv[i], "/nomtrr") == 0) {
            g_no_mtrr = 1;
        }
        if (stricmp(argv[i], "-directvram") == 0 ||
            stricmp(argv[i], "/directvram") == 0) {
            g_direct_vram = 1;
        }
        if (stricmp(argv[i], "-hwflip") == 0 ||
            stricmp(argv[i], "/hwflip") == 0) {
            g_hw_flip = 1;
        }
        if (stricmp(argv[i], "-mtrrinfo") == 0 ||
            stricmp(argv[i], "/mtrrinfo") == 0) {
            g_mtrr_info = 1;
        }
        if (stricmp(argv[i], "-sched") == 0 ||
            stricmp(argv[i], "/sched") == 0) {
            g_sched_req = 1;
        }
    }

    /* Conventional DOS memory:
     *   512 B VBEInfo  + 256 B ModeInfo  + 1024 B palette = 1792 B
     *   128 paragraphs = 2048 B - enough with room to spare              */
#ifdef __DJGPP__
    /* Enable near pointer access for direct physical memory mapping */
    if (!__djgpp_nearptr_enable()) {
        printf("Failed to enable near pointer access\n");
        return 1;
    }
    /* nearptr_enable expands DS/ES/SS to 4 GB but leaves CS at program
     * size.  PMI calls jump into BIOS ROM code outside our binary, so
     * expand CS to cover the full 4 GB address space as well.
     * Try DPMI 0008h first (works on PMODE/DJ), fall back to direct
     * GDT/LDT patching (needed for CWSDPR0). */
    if ((_get_cs() & 3) == 0) {
        if (expand_cs_to_4gb()) {
            printf("[0] CS limit expanded to 4 GB\n");
        } else {
            /* CS expansion failed: PMI calls would jump into BIOS ROM code
             * at addresses above CS.limit and immediately #GP.  Force PMI
             * off so the demo falls back to the safe VBE INT 10h path.   */
            g_no_pmi = 1;
            printf("Warning: could not expand CS limit -- PMI disabled, using VBE INT 10h\n");
        }
    }
    printf("[0] DJGPP nearptr enabled, SSE init...\n");
    djgpp_enable_sse();
#endif
    if (!dpmi_alloc_dos(128)) {
        printf("DPMI: cannot allocate conventional memory\n");
        return 1;
    }

    /* Calibrate RDTSC frequency (~220ms, text mode, safe) */
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

    /* VBE 3.0: query Protected Mode Interface
     * PMI code does direct port I/O — requires ring 0 (PMODE/W). */
    if (g_vbe3 && !g_no_pmi) {
        if ((_get_cs() & 3) != 0) {
            printf("PMI        : skipped (ring 3 — needs ring 0 for direct I/O)\n");
        } else {
            g_pmi_ok = query_pmi();
            if (g_pmi_ok) {
                unsigned long entry = (unsigned long)g_pmi_rm_seg * 16
                                    + g_pmi_rm_off + g_pmi_setds_off;
                printf("PMI        : available, entry at 0x%08lX (linear)\n", entry);
            } else {
                printf("PMI        : not available\n");
            }
        }
    } else if (g_no_pmi) {
        printf("PMI        : disabled (use -pmi to enable)\n");
    }

    {
        /* Copy mode list to a local array BEFORE any vbe_get_mode_info calls.
         * The mode list pointer often points into our DOS transfer buffer; each
         * vbe_get_mode_info call zeros the first 256 bytes of that buffer,
         * corrupting the list on cards (e.g. Radeon X1300) that store it there. */
        unsigned short  mode_list[512];
        unsigned short *src;
        int             i, count;
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
            if (!(vbmi.mode_attributes & 0x01))    continue;  /* supported  */
            if (!(vbmi.mode_attributes & 0x10))    continue;  /* graphics   */
            if (!(vbmi.mode_attributes & 0x80))    continue;  /* LFB        */
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

    /* ---- Check VRAM for page-flipping --------------------------------- */
    page_size     = (unsigned long)lfb_pitch * HEIGHT;
    use_doublebuf = 0;
    back_page     = 0;

    {
        unsigned long total_vram = (unsigned long)vbi.total_memory * 65536UL;
        printf("VRAM: %luKB  page: %luKB  ",
               total_vram / 1024UL, page_size / 1024UL);
        if (total_vram >= page_size * 2UL) {
            /* sysram render + 2 VRAM pages = triple-buffer */
            printf("-> triple-buffer candidate (sysram + 2x VRAM)\n");
            use_doublebuf = 1;   /* confirmed after 4F07h test below    */
        } else {
            /* sysram render + 1 VRAM page = double-buffer */
            printf("-> double-buffer (sysram + 1x VRAM, insufficient for page flip)\n");
        }
    }

    /* ---- Allocate system-RAM frame buffer (skipped with -directvram) --- */
    /*
     * Rendering plasma directly into VRAM is extremely slow because LFB
     * writes are uncached (UC) or write-combining (WC).  Individual byte
     * writes at ~100ns each × 786K pixels ≈ 78ms/frame → ~13 FPS.
     * Instead, render into fast cached system RAM, then blit to VRAM in
     * one sequential burst (WC-friendly memcpy).
     * Use -directvram to measure the difference on real hardware.
     */
    if (!g_direct_vram) {
        frame_buf = (unsigned char *)malloc(PIXELS);
        if (!frame_buf) {
            dpmi_free_dos();
            printf("Out of memory (frame buffer)\n");
            return 1;
        }
    }
    printf("Render     : %s\n", g_direct_vram ? "direct-to-VRAM (-directvram)" : "system-RAM + blit");

    init_plasma_tables();

    /* ---- Map LFB (1 page for single-buf, 2 pages for double-buf) ------ */
    {
        unsigned long map_size = use_doublebuf ? page_size * 2UL : page_size;
        lfb = (unsigned char *)dpmi_map_physical(lfb_phys, map_size);
    }
    if (!lfb) {
        free(frame_buf);
        dpmi_free_dos();
        printf("Cannot map LFB at 0x%08lX\n", lfb_phys);
        return 1;
    }

    /* ---- MTRR Write-Combining for LFB --------------------------------- */
    if (!g_no_mtrr) {
        unsigned long total_vram = (unsigned long)vbi.total_memory * 65536UL;
        int wc = setup_mtrr_wc(lfb_phys, total_vram);
        if (wc == 1) {
            if (g_mtrr_replaced_uc)
                printf("MTRR WC    : enabled (slot %d, replaced BIOS UC, %luMB at 0x%08lX)\n",
                       g_mtrr_slot, next_pow2(total_vram) >> 20, lfb_phys);
            else
                printf("MTRR WC    : enabled (slot %d, new entry, %luMB at 0x%08lX)\n",
                       g_mtrr_slot, next_pow2(total_vram) >> 20, lfb_phys);
        } else if (wc == -1)
            printf("MTRR WC    : already active (BIOS/chipset)\n");
        else if ((_get_cs() & 3) != 0)
            printf("MTRR WC    : skipped (ring 3 — use CWSDPR0 for ring 0)\n");
        else
            printf("MTRR WC    : not available\n");
    } else {
        printf("MTRR WC    : disabled by -nomtrr\n");
    }

    /* ---- PAT fix: entry 3 UC->UC- for WC passthrough ------------------- */
    if (g_mtrr_wc >= 1) {
        int pat_ok = setup_pat_uc_minus();
        if (pat_ok == 1)
            printf("PAT fix    : entry 3 UC->UC- (WC passthrough enabled)\n");
        else if (pat_ok == -1)
            printf("PAT fix    : not needed (entry 3 already UC- or WC)\n");
        else
            printf("PAT fix    : not available\n");
    }

    /* ---- MTRR diagnostic dump (with -mtrrinfo flag) -------------------- */
    if (g_mtrr_info) {
        unsigned long map_size = use_doublebuf ? page_size * 2UL : page_size;
        dump_mtrr_info((unsigned long)lfb, map_size);
    }

    /* ---- Brief mode set to test page-flip features --------------------- */
    if (!vbe_set_mode(target_mode)) {
        dpmi_unmap_physical(lfb);
        if (frame_buf) free(frame_buf);
        dpmi_free_dos();
        printf("Set mode 0x%03X failed\n", target_mode);
        return 1;
    }

    /* Test DAC width (needs VBE mode active) */
    init_dac();

    /* Test VBE 4F07h page-flip support (needs VBE mode active) */
    if (use_doublebuf) {
        if (!vbe_set_display_start(0, 0, 0)) {
            use_doublebuf = 0;
        } else {
            /* Test PMI SetDisplayStart if available */
            if (g_pmi_ok) {
                if (!pmi_set_display_start(0, 0, 0)) {
                    g_pmi_ok = 0;
                    vbe_set_display_start(0, 0, 0);
                }
            }
            /* Test VBE 3.0 scheduled flip (BL=02h).
             * Opt-in via -sched: ATI ATOMBIOS crashes (exception 0Dh)
             * on unsupported BL=02h subfunction. */
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
                    /* Wait for test flip, then reset to page 0 */
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

    /* Test direct GPU register access for -hwflip */
    if (g_hw_flip && use_doublebuf) {
        if ((_get_cs() & 3) != 0) {
            printf("HW flip    : skipped (ring 3 — use CWSDPR0 for ring 0)\n");
            g_hw_flip = 0;
        } else if (!detect_gpu_iobase(lfb_phys)) {
            printf("HW flip    : GPU I/O base not found\n");
            g_hw_flip = 0;
        } else {
            /* Verify by reading D1GRPH_PRIMARY_SURFACE_ADDRESS.
             * After mode set, it should contain our LFB physical base. */
            unsigned long cur = gpu_reg_read(R5_D1GRPH_PRIMARY_SURFACE_ADDRESS);
            if ((cur & 0xFFF00000UL) != (lfb_phys & 0xFFF00000UL)) {
                printf("HW flip    : verification failed "
                       "(D1GRPH=0x%08lX, LFB=0x%08lX)\n", cur, lfb_phys);
                g_hw_flip = 0;
            } else {
                g_fb_phys = lfb_phys;
                printf("HW flip    : active (I/O=0x%04X, "
                       "D1GRPH=0x%08lX)\n",
                       g_gpu_iobase, cur);
            }
        }
    } else if (g_hw_flip) {
        printf("HW flip    : disabled (no page flip support)\n");
        g_hw_flip = 0;
    }

    /* ---- Startup benchmark (while in graphics mode — safe VRAM writes) - */
    run_benchmark(lfb, lfb_pitch);

    /* Return to text mode for feature summary */
    vbe_set_text_mode();

    /* ---- Feature Summary ----------------------------------------------- */
    {
        const char *ring_str  = ((_get_cs() & 3) == 0) ?
                                "0 (ring 0)" : "3 (ring 3 — use CWSDPR0)";
        const char *pmi_str, *mtrr_str, *flip_str;
        const char *sched_str, *buf_str, *render_str;
        const char *hwflip_str;

        if (g_pmi_ok)
            pmi_str = "available";
        else if (g_no_pmi)
            pmi_str = "disabled (use -pmi to enable)";
        else if (g_force_vbe2)
            pmi_str = "disabled (-vbe2)";
        else if ((_get_cs() & 3) != 0)
            pmi_str = "skipped (ring 3)";
        else
            pmi_str = "not available";

        if (g_mtrr_wc == 1)
            mtrr_str = g_mtrr_replaced_uc ? "enabled (replaced BIOS UC)"
                                           : "enabled (new slot)";
        else if (g_mtrr_wc == 2)
            mtrr_str = "already active (BIOS)";
        else if (g_no_mtrr)
            mtrr_str = "disabled (-nomtrr)";
        else if ((_get_cs() & 3) != 0)
            mtrr_str = "skipped (ring 3 — use CWSDPR0 for ring 0)";
        else
            mtrr_str = "not available";

        flip_str = use_doublebuf ? "yes (4F07h)" : "no";

        if (g_sched_flip)
            sched_str = "yes (BL=02h)";
        else if (!g_vbe3)
            sched_str = "n/a (VBE 2.0)";
        else if (!use_doublebuf)
            sched_str = "n/a (no page flip)";
        else
            sched_str = "not supported";

        if (g_hw_flip)
            hwflip_str = "active (GPU register + update lock)";
        else
            hwflip_str = "off (use -hwflip to enable)";

        if (g_direct_vram)
            buf_str = use_doublebuf ? "double (2x VRAM)" :
                                      "single (1x VRAM)";
        else
            buf_str = use_doublebuf ? "triple (sysram + 2x VRAM)" :
                                      "double (sysram + 1x VRAM)";

        render_str = g_direct_vram ? "direct-to-VRAM (-directvram)" :
                                     "system-RAM + blit";

        printf("\n");
        printf("==========================================\n");
        printf(" PLASMA - Feature Summary\n");
        printf("==========================================\n");
        printf(" CPU          : %.0f MHz%s\n", g_rdtsc_mhz,
               (g_rdtsc_mhz < 100.0 || g_rdtsc_mhz > 10000.0)
                   ? " (emulator - timing unreliable)" : "");
        printf(" VBE version  : %d.%d%s\n",
               g_vbe_version >> 8, g_vbe_version & 0xFF,
               g_force_vbe2 ? " (forced VBE 2.0)" : "");
        printf(" Video memory : %u KB\n", (unsigned)vbi.total_memory * 64);
        printf(" Mode         : 0x%03X  %dx%d %dbpp\n",
               target_mode, WIDTH, HEIGHT, 8);
        printf(" LFB          : 0x%08lX  pitch=%d\n",
               lfb_phys, lfb_pitch);
        printf(" DAC          : %d-bit\n", g_dac_bits);
        printf(" Ring         : %s\n", ring_str);
        printf(" PMI          : %s\n", pmi_str);
        printf(" MTRR WC      : %s\n", mtrr_str);
        printf(" PAT fix      : %s\n",
               g_pat_modified ? "entry 3 UC->UC- (WC passthrough)" :
               g_mtrr_wc ? "not needed" : "n/a");
        printf(" Page flip    : %s\n", flip_str);
        printf(" Sched flip   : %s\n", sched_str);
        printf(" HW flip      : %s\n", hwflip_str);
        printf(" Buffering    : %s\n", buf_str);
        printf(" Render       : %s\n", render_str);
        printf("==========================================\n");
        /* Benchmark results */
        if (g_bench_combined_ms > 0.0) {
            double r_fps  = g_bench_render_ms   > 0.0 ? 1000.0 / g_bench_render_ms   : 0.0;
            double b_fps  = g_bench_blit_ms     > 0.0 ? 1000.0 / g_bench_blit_ms     : 0.0;
            double c_fps  = g_bench_combined_ms > 0.0 ? 1000.0 / g_bench_combined_ms : 0.0;
            double r_pct  = g_bench_render_ms   / g_bench_combined_ms * 100.0;
            double b_pct  = g_bench_blit_ms     / g_bench_combined_ms * 100.0;
            unsigned long blit_mbs = g_bench_blit_ms > 0.0
                ? (unsigned long)(PIXELS / 1024.0 / 1024.0 / (g_bench_blit_ms / 1000.0))
                : 0UL;
            const char *bn = (r_pct >= 60.0) ? "RENDER" :
                             (b_pct >= 60.0) ? "BLIT"   : "balanced";
            printf("==========================================\n");
            printf(" Benchmark (%d frames, sysram+blit)\n", BENCH_FRAMES);
            printf("  Render : %8.3f ms  %7.2f FPS\n",
                   g_bench_render_ms, r_fps);
            if (g_bench_blit_ms > 0.0)
                printf("  Blit   : %8.3f ms  %7.2f FPS  %lu MB/s\n",
                       g_bench_blit_ms, b_fps, blit_mbs);
            else
                printf("  Blit   : <0.001 ms  (sub-us, fast emulator RAM)\n");
            printf("  Total  : %8.3f ms  %7.2f FPS  bottleneck: %s\n",
                   g_bench_combined_ms, c_fps, bn);
            printf("  Share  : render %3.0f%%  blit %3.0f%%\n",
                   r_pct > 0.0 ? r_pct : 0.0,
                   b_pct > 0.0 ? b_pct : 0.0);
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

    /* Re-initialize DAC and display start for the demo */
    init_dac();
    if (use_doublebuf) {
        vbe_set_display_start(0, 0, 0);
        back_page = 1;
    }

    /* ---- Palette ------------------------------------------------------- */
    build_plasma_palette(pal);
    find_hud_colours(pal);
    vbe_set_palette(0, 256, pal);

    /* ---- BIOS font ----------------------------------------------------- */
    font = get_bios_font_8x8();

    /* ---- Palette debug grid -------------------------------------------- */
    show_palette_grid(lfb, lfb_pitch, font);
    while (!kbhit()) {}          /* wait for keypress                      */
    getch();                     /* consume it                             */

    /* ---- Main loop ----------------------------------------------------- */
    t           = 0;
    vsync_on    = 1;
    running     = 1;
    frame_count = 0;
    last_ticks  = *bios_ticks;
    fps         = 0.0f;

#ifdef __DJGPP__
    /* PMODETSR reverts CS.limit to the original program size after every
     * DPMI real-mode simulation (INT 31h AX=0300h saves/restores the PM
     * descriptor state on each real-mode switch).  expand_cs_to_4gb()
     * during startup succeeded, but the last VBE INT 10h call (palette,
     * mode set, font) reset CS.limit back to 0x17ffff before the loop.
     * Re-expand now — no more DPMI real-mode calls in the render loop,
     * so the 4 GB limit stays in effect for all PMI page-flip calls.   */
    if (g_pmi_ok) {
        if (!expand_cs_to_4gb()) {
            g_pmi_ok = 0;
            printf("PMI: CS limit was reverted by DPMI server; PMI disabled\n");
        }
    }
#endif

    while (running) {

        /* --- Keyboard --------------------------------------------------- */
        while (kbhit()) {
            int k = getch();
            if (k == 27) {
                running = 0;
            } else if (k == 'v' || k == 'V') {
                vsync_on = !vsync_on;
            }
        }

        /* --- Render plasma ------------------------------------------------- */
        /* With -directvram: render straight into VRAM (slow, for comparison).
         * Normal:           render into system RAM, then blit to VRAM.       */
        if (g_direct_vram) {
            unsigned char *dst = lfb + (use_doublebuf ?
                                  (unsigned long)back_page * page_size : 0UL);
            if (vsync_on) wait_vsync();
            render_plasma(dst, lfb_pitch, t);
            /* No HUD in direct-VRAM mode (would need a scratch buffer) */
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
            render_plasma(frame_buf, WIDTH, t);

            /* --- HUD overlay (into system RAM) -------------------------- */
            {
                const char *sync_str = vsync_on ?
                    (g_sched_flip ? "SCH" : "ON ") : "OFF";
                const char *buf_str  = g_direct_vram ?
                                       (use_doublebuf ? "DBL" : "SGL") :
                                       (use_doublebuf ? "TRP" : "DBL");
                const char *pmi_str  = g_pmi_ok ? "YES" : "NO ";
                const char *wc_str   = g_mtrr_wc ? "YES" : "NO ";
                const char *hwf_str  = g_hw_flip ? "YES" : "NO ";
                sprintf(msg, "VSYNC:%s BUF:%s PMI:%s HWF:%s WC:%s FPS:%5.1f  [V] [ESC]",
                        sync_str, buf_str, pmi_str, hwf_str, wc_str, fps);
                draw_str_bg(frame_buf, WIDTH, 4, 4, msg,
                            g_hud_bright, g_hud_dark, font);
            }

            if (vsync_on && g_hw_flip) {
                draw_str_2x(frame_buf, WIDTH, 4, 16,
                            "HW FLIP - GPU locked, tear-free  ",
                            g_hud_bright, g_hud_dark, font);
            } else if (!vsync_on && g_hw_flip) {
                draw_str_2x(frame_buf, WIDTH, 4, 16,
                            "HW FLIP - tear-free, no throttle ",
                            g_hud_bright, g_hud_dark, font);
            } else if (vsync_on && g_sched_flip) {
                draw_str_2x(frame_buf, WIDTH, 4, 16,
                            "VSYNC SCHED - non-blocking flip  ",
                            g_hud_bright, g_hud_dark, font);
            } else if (vsync_on) {
                draw_str_2x(frame_buf, WIDTH, 4, 16,
                            "VSYNC ON  - tearing suppressed   ",
                            g_hud_bright, g_hud_dark, font);
            } else {
                draw_str_2x(frame_buf, WIDTH, 4, 16,
                            "VSYNC OFF - watch for tear line! ",
                            g_hud_bright, g_hud_dark, font);
            }

            /* --- Present frame ------------------------------------------ */
            if (use_doublebuf) {
                if (g_hw_flip) {
                    /* Direct GPU register flip with update lock.
                     * The lock buffers the surface address write; on
                     * unlock the hardware applies it atomically at the
                     * next vsync — guaranteed tear-free, non-blocking.
                     *
                     * For vsync throttling we wait for the pending bit
                     * to clear (i.e. hardware applied previous flip). */
                    if (vsync_on)
                        while (!hw_is_flip_done()) {}

                    blit_to_lfb(lfb + (unsigned long)back_page * page_size,
                                lfb_pitch, frame_buf);

                    hw_page_flip(g_fb_phys +
                        (unsigned long)back_page * page_size);

                } else if (g_sched_flip) {
                    /* Wait for previous scheduled flip to complete before
                     * overwriting the back page that may still be displayed. */
                    if (g_pmi_ok) {
                        while (!pmi_is_flip_complete()) {}
                    } else {
                        while (!vbe_is_flip_complete()) {}
                    }

                    blit_to_lfb(lfb + (unsigned long)back_page * page_size,
                                lfb_pitch, frame_buf);

                    if (vsync_on) {
                        /* VBE 3.0 non-blocking scheduled flip */
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
                    /* Standard BIOS flip: always BL=00h (immediate).
                     *
                     * BL=80h (BIOS-managed vsync wait) is unreliable on
                     * modern GPUs: ATI ATOMBIOS writes registers AFTER
                     * blanking ends, causing tearing at the top of screen.
                     *
                     * For vsync throttling we wait_vsync() AFTER the flip. */
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

        /* --- FPS counter (updated ~once per second) --------------------- */
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

    printf("PLASMA done.  mode=0x%03X  DAC=%d-bit  buf:%s  PMI:%s  HWF:%s  WC:%s  PAT:%s  sched:%s  render:%s\n",
           target_mode, g_dac_bits,
           g_direct_vram ? (use_doublebuf ? "double" : "single") :
                           (use_doublebuf ? "triple" : "double"),
           g_pmi_ok ? "yes" : "no",
           g_hw_flip ? "yes" : "no",
           g_mtrr_wc == 1 ? "mtrr" : g_mtrr_wc == 2 ? "bios" : "no",
           g_pat_modified ? "fix" : "no",
           g_sched_flip ? "yes" : "no",
           g_direct_vram ? "direct" : "sysram");
    return 0;
}
