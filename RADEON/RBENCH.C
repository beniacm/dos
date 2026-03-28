/*
 * RBENCH.C  -  CPU vs GPU Fill Benchmark
 *
 * Standalone Radeon demo: compares CPU memset vs GPU solid fill
 * throughput, plus GPU small-rect flood benchmarks (3-reg, 2-reg,
 * batched).  Uses RDTSC for microsecond-accurate timing.
 */

#include "RSETUP.H"

int main(void)
{
    unsigned long t0lo, t0hi, t1lo, t1hi;
    double cpu_ms, gpu_ms, rect_ms, rect_ms2, rect_ms3;
    int iters = 100;
    long rect_iters, rect_iters2, rect_iters3;
    int i;
    unsigned char col;
    char buf[120];

    if (radeon_init("RBENCH - CPU vs GPU Fill Benchmark", 2))
        return 1;

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

    /* --- GPU small-rect throughput benchmark --- */
    {
        int rw = 32, rh = 32;
        int cols = g_xres / rw;
        int rows = (g_yres - 20) / rh;
        long total_rects = (long)cols * rows;
        int pass;

        gpu_fill(0, 0, g_xres, g_yres, 0);
        gpu_wait_idle();

        /* 3-reg path */
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

        /* 2-reg path */
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

        /* Batched 2-reg path */
        gpu_fill_setup();
        gpu_fill_set_color(0xAA);
        rect_iters3 = 0;

        rdtsc_read(&t0lo, &t0hi);
        for (pass = 0; pass < 200; pass++) {
            int rx, ry;
            for (ry = 0; ry < rows; ry++)
                for (rx = 0; rx < cols; rx++)
                    gpu_fill_batch_rect(rx * rw, ry * rh, rw, rh);
            gpu_fill_batch_flush();
            rect_iters3 += total_rects;
        }
        gpu_wait_idle();
        rdtsc_read(&t1lo, &t1hi);
        rect_ms3 = tsc_to_ms(t1lo, t1hi, t0lo, t0hi);
    }

    /* Display results */
    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();

    {
        double total = (double)g_xres * g_yres * iters / (1024.0*1024.0);
        double cpu_rate = (cpu_ms > 0) ? total / (cpu_ms/1000.0) : 0;
        double gpu_rate = (gpu_ms > 0) ? total / (gpu_ms/1000.0) : 0;
        double speedup  = (gpu_ms > 0) ? cpu_ms / gpu_ms : 0;
        double rps3     = (rect_ms > 0) ?
            (double)rect_iters / (rect_ms / 1000.0) : 0;
        double rpix3    = (rect_ms > 0) ?
            (double)rect_iters * 32 * 32 / (rect_ms / 1000.0) / 1e6 : 0;
        double rps2     = (rect_ms2 > 0) ?
            (double)rect_iters2 / (rect_ms2 / 1000.0) : 0;
        double rpix2    = (rect_ms2 > 0) ?
            (double)rect_iters2 * 32 * 32 / (rect_ms2 / 1000.0) / 1e6 : 0;
        double rpsB     = (rect_ms3 > 0) ?
            (double)rect_iters3 / (rect_ms3 / 1000.0) : 0;
        double rpixB    = (rect_ms3 > 0) ?
            (double)rect_iters3 * 32 * 32 / (rect_ms3 / 1000.0) / 1e6 : 0;

        cpu_str_c(10, "=== CPU vs GPU Fill Benchmark ===", 255, 2);

        sprintf(buf, "%d x full-screen fill (%dx%d, 8bpp)",
                iters, g_xres, g_yres);
        cpu_str_c(36, buf, 253, 1);

        sprintf(buf, "CPU: %7.1f ms  (%5.1f MB/s)", cpu_ms, cpu_rate);
        cpu_str(40, 60, buf, 251, 2);
        if (cpu_rate < 100.0)
            cpu_str(40, 76,
                    "(Run WCINIT.EXE first for ~30x CPU rate)", 249, 1);

        sprintf(buf, "GPU: %7.1f ms  (%5.1f MB/s)", gpu_ms, gpu_rate);
        cpu_str(40, 86, buf, 250, 2);

        if (gpu_ms > 0) {
            sprintf(buf, "GPU speedup: %.1fx", speedup);
            cpu_str_c(128, buf, 254, 3);
        }

        sprintf(buf, "32x32 (3-reg):    %6.0f Krect/s  %5.0f Mpix/s",
                rps3/1000.0, rpix3);
        cpu_str(20, 166, buf, 250, 1);

        sprintf(buf, "32x32 (2-reg):    %6.0f Krect/s  %5.0f Mpix/s",
                rps2/1000.0, rpix2);
        cpu_str(20, 186, buf, 254, 1);

        sprintf(buf, "32x32 (batch-32): %6.0f Krect/s  %5.0f Mpix/s",
                rpsB/1000.0, rpixB);
        cpu_str(20, 206, buf, 255, 1);

        /* Bar chart */
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
            gpu_fill(50, bar_y, cpu_bar, 24, 251);
            bar_y += 40;
            cpu_str(10, bar_y - 12, "GPU", 250, 1);
            gpu_fill(50, bar_y, gpu_bar, 24, 250);
            gpu_wait_idle();
        }

        sprintf(buf, "Flip: %s   Timer: RDTSC @ %.0f MHz",
                g_avivo_flip ? "AVIVO hwflip" : "VBE+VGA vsync",
                g_rdtsc_mhz);
        cpu_str_c(350, buf, 253, 1);
        cpu_str_c(g_yres - 20, "Press any key to exit", 253, 1);
    }

    getch();
    radeon_cleanup();
    return 0;
}
