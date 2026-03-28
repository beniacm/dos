/*
 * RSETUP.C  -  Shared Radeon demo init / cleanup
 *
 * Extracted from RADEON.C main() boilerplate so every standalone demo
 * can initialise the Radeon hardware with a single function call.
 */

#include "RSETUP.H"

/* ---------------------------------------------------------------- */

int radeon_init(const char *title, int vram_pages)
{
    unsigned long bar0, bar2, bar3, pci_cmd;
    unsigned long lfb_sz;
    unsigned long rbbm_val;

    printf("%s\n", title);
    {
        int i, n = (int)strlen(title);
        for (i = 0; i < n; i++) putchar('=');
        printf("\n\n");
    }

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

    /* --- Map MMIO registers (BAR2 on RV515 PCIe) --- */
    bar0 = pci_rd32(g_pci_bus, g_pci_dev, g_pci_func, 0x10);
    bar2 = pci_rd32(g_pci_bus, g_pci_dev, g_pci_func, 0x18);
    g_mmio_phys = bar2 & 0xFFFFFFF0UL;

    printf("  BAR0 (VRAM): 0x%08lX %s\n", bar0,
           (bar0 & 0x08) ? "(pref)" : "(non-pref)");

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
                 pci_cmd | 0x06);
        printf("  (Enabled PCI memory access + bus-master)\n");
    }

    /* --- Validate MMIO mapping --- */
    g_vram_mb = rreg(R_CONFIG_MEMSIZE) / (1024UL * 1024UL);
    rbbm_val = rreg(R_RBBM_STATUS);
    printf("  Video RAM : %lu MB\n", g_vram_mb);
    printf("  RBBM_STS  : 0x%08lX  (FIFO=%lu, %s)\n",
           rbbm_val, rbbm_val & RBBM_FIFOCNT_MASK,
           (rbbm_val & RBBM_ACTIVE) ? "BUSY" : "idle");

    if (g_vram_mb == 0 || g_vram_mb > 1024) {
        printf("WARNING: VRAM size %lu MB looks wrong.\n", g_vram_mb);
        printf("  CONFIG_MEMSIZE=0x%08lX\n", rreg(R_CONFIG_MEMSIZE));
        printf("  BAR2 may not be MMIO. Run RDIAG for analysis.\n");
    }

    /* Print all BARs */
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

    /* --- GPU framebuffer base address --- */
    {
        unsigned long hdp_fb, mc_fb;
        hdp_fb = rreg(R_HDP_FB_LOCATION);
        mc_fb  = mc_rreg(RV515_MC_FB_LOCATION);
        g_fb_location = (hdp_fb & 0xFFFFUL) << 16;
        printf("\n  HDP_FB_LOC: 0x%08lX  (FB base = 0x%08lX)\n",
               hdp_fb, g_fb_location);
        printf("  MC_FB_LOC : 0x%08lX  (indirect MC reg 0x01)\n", mc_fb);
        if (g_fb_location != 0)
            printf("  NOTE: Non-zero FB base - 2D engine offsets adjusted\n");
    }

    /* --- GPU state diagnostics --- */
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
    printf("  VESA mem  : %lu KB (%lu MB)\n",
           g_vesa_mem_kb, g_vesa_mem_kb / 1024);

    /* Align pitch to 64 bytes (Radeon 2D engine PITCH_OFFSET uses 64-byte units) */
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

    /* Compute 4KB-aligned page stride for AVIVO surface registers */
    g_page_stride = g_yres;
    while (((long)g_page_stride * g_pitch) & 4095L)
        g_page_stride++;
    printf("  Page stride: %d rows (%ld bytes, 4KB-aligned)\n",
           g_page_stride, (long)g_page_stride * g_pitch);

    printf("\nPress any key to start graphics...\n");
    getch();

    /* --- Set graphics mode --- */
    g_font = get_font();

    if (!vbe_set_mode(g_vmode)) {
        printf("ERROR: Cannot set VESA mode.\n");
        dpmi_unmap((void *)g_mmio);
        dpmi_free();
        return 1;
    }

    /* Map LFB for requested pages */
    lfb_sz = (unsigned long)g_pitch * g_page_stride * vram_pages;
    if (g_vram_mb > 0 && lfb_sz > g_vram_mb * 1024UL * 1024UL)
        lfb_sz = g_vram_mb * 1024UL * 1024UL;
    lfb_sz = (lfb_sz + 4095UL) & ~4095UL;
    g_lfb = (unsigned char *)dpmi_map(g_lfb_phys, lfb_sz);
    if (!g_lfb) {
        vbe_text_mode();
        printf("ERROR: Cannot map LFB.\n");
        dpmi_unmap((void *)g_mmio);
        dpmi_free();
        return 1;
    }

    setup_palette();
    gpu_init_2d();
    detect_flip_mode();

    /* If using VGA scanout and pitch was aligned, update CRTC offset */
    if (!g_avivo_flip && (g_pitch != g_xres)) {
        unsigned char cr_val = (unsigned char)(g_pitch / 8);
        outp(0x3D4, 0x13);
        outp(0x3D5, cr_val);
    }

    return 0;   /* success */
}

/* ---------------------------------------------------------------- */

void radeon_cleanup(void)
{
    vbe_text_mode();

    printf("Card: %s  |  VRAM: %lu MB  |  Mode: %dx%d  |  Pipes: %d\n",
           g_card_name, g_vram_mb, g_xres, g_yres, g_num_gb_pipes);

    dpmi_unmap(g_lfb);
    dpmi_unmap((void *)g_mmio);
    dpmi_free();
}
