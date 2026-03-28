/*
 * RFLOOD.C  -  GPU Rectangle Flood Demo
 *
 * Standalone Radeon demo: animated random rectangle flood with
 * batched FIFO access and XOR-shift PRNG for maximum throughput.
 */

#include "RSETUP.H"

/* Fast XOR-shift PRNG */
static unsigned long g_xor_state = 2463534242UL;
static unsigned long xorshift(void)
{
    g_xor_state ^= g_xor_state << 13;
    g_xor_state ^= g_xor_state >> 17;
    g_xor_state ^= g_xor_state << 5;
    return g_xor_state;
}

int main(void)
{
    unsigned long t0lo, t0hi, now_lo, now_hi;
    long count = 0, fps_count = 0;
    long long total_pixels = 0;
    char buf[80];
    int loop_count = 0;
    int pending;

    if (radeon_init("RFLOOD - GPU Rectangle Flood Demo", 2))
        return 1;

    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();

    cpu_str_c(4, "GPU Rectangle Flood - press any key to stop", 255, 1);

    /* Seed PRNG from RDTSC */
    rdtsc_read(&g_xor_state, &t0hi);
    if (g_xor_state == 0) g_xor_state = 1;
    rdtsc_read(&t0lo, &t0hi);

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
        rw = (int)(rnd % 200) + 4;
        rh = (int)((rnd >> 16) % 150) + 4;
        rc = (unsigned char)(rnd >> 16);

        if (rx + rw > g_xres) rw = g_xres - rx;
        if (ry + rh > g_yres) rh = g_yres - ry;
        if (rw < 1 || rh < 1) continue;

        /* Batched FIFO: check once per 21 rects (21x3 = 63 entries) */
        if (pending == 0)
            gpu_wait_fifo(63);
        g_mmio[MMIO_DP_BRUSH_FRGD_CLR] = (unsigned long)rc;
        g_mmio[MMIO_DST_Y_X] =
            ((unsigned long)ry << 16) | (unsigned long)rx;
        g_mmio[MMIO_DST_HEIGHT_WIDTH] =
            ((unsigned long)rh << 16) | (unsigned long)rw;
        if (++pending >= 21) {
            g_fifo_free = 0;
            pending = 0;
        }

        count++;
        fps_count++;
        total_pixels += (long long)rw * rh;

        if (++loop_count >= 128) {
            loop_count = 0;
            if (kbhit()) break;
        }

        /* Update counter every ~8000 rects */
        if (fps_count >= 8000) {
            double elapsed, rps, mpps;
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

            gpu_fill_setup();
            pending = 0;
            fps_count = 0;
        }
    }
    getch();
    g_fifo_free = 0;

    /* Summary */
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
        cpu_str_c(g_yres / 2 - 10, buf, 254, 2);
        cpu_str_c(g_yres / 2 + 20, "Press any key to exit", 253, 1);
    }

    getch();
    radeon_cleanup();
    return 0;
}
