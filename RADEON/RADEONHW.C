/*
 * RADEONHW.C  -  Shared hardware-layer for ATI Radeon RV515/RV516
 *
 * Radeon-specific functionality that builds on DOSLIB:
 *   - PCI device detection (pci_find_radeon)
 *   - Radeon MMIO register access (rreg/wreg/mc_rreg/pll_*)
 *   - GPU engine control (reset, 2D init, wait, flush)
 *   - AVIVO page-flip / VGA fallback
 *   - Radeon demo palettes (setup_palette, setup_dune_palette)
 *   - VESA mode selection wrapper (find_mode → find_vbe_mode)
 *
 * Generic functions (DPMI, VBE, PCI config, palette, font, text,
 * timing, vsync) are provided by DOSLIB.C.
 *
 * DJGPP compatibility handled by DOSLIB.
 */

#include "RADEONHW.H"
#include <time.h>

/* =============================================================== */
/*  RV515/RV516 device-ID table (Radeon X1300 family)              */
/* =============================================================== */

const DevEntry g_devtab[] = {
    { 0x7100, "RV515"            }, { 0x7101, "RV515 Sec"       },
    { 0x7102, "RV515 HyperMem"   }, { 0x7109, "RV515"           },
    { 0x710A, "RV515"            }, { 0x7140, "RV515 PRO"       },
    { 0x7141, "RV515 PRO Sec"    }, { 0x7142, "RV515 X1300"     },
    { 0x7143, "RV505 X1300/X1550"}, { 0x7146, "RV515 X1300 XT"  },
    { 0x714E, "RV515 X1300"      },
    { 0x7181, "RV516 X1300/X1550"}, { 0x7183, "RV516 X1300 PRO" },
    { 0x7186, "RV516 Mobile"     }, { 0x7187, "RV516 X1300 Sec" },
    { 0x718A, "RV516 Mobile"     }, { 0x718B, "RV516 Mobile"    },
    { 0x718C, "RV516 Mobile"     }, { 0x718D, "RV516 Mobile"    },
    { 0x719F, "RV516 X1550"      },
    { 0, NULL }
};

/* =============================================================== */
/*  Radeon-specific global state                                    */
/*  (DPMI, VBE, font, DAC, timing globals are in DOSLIB.C)         */
/* =============================================================== */

volatile unsigned long *g_mmio = NULL;
unsigned long  g_mmio_phys  = 0;
unsigned long  g_vram_mb    = 0;
unsigned long  g_fb_location = 0;
int g_page_stride = 0;
unsigned long g_default_po = 0;
int g_fifo_free = 0;
int g_avivo_flip = 0;
int g_num_gb_pipes = 1;

int  g_pci_bus = 0, g_pci_dev = 0, g_pci_func = 0;
unsigned short g_pci_did = 0;
const char    *g_card_name = "Unknown";

/* =============================================================== */
/*  Radeon PCI device scan                                          */
/* =============================================================== */
int pci_find_radeon(void)
{
    int b, d, fn, i;
    unsigned long id;
    unsigned short vid, did;
    unsigned char hdr;

    for (b = 0; b < 256; b++) {
        for (d = 0; d < 32; d++) {
            for (fn = 0; fn < 8; fn++) {
                id = pci_rd32(b, d, fn, 0x00);
                vid = (unsigned short)(id & 0xFFFF);
                did = (unsigned short)(id >> 16);
                if (vid == 0xFFFF || vid == 0) goto next_dev;

                if (vid == ATI_VID) {
                    for (i = 0; g_devtab[i].name; i++) {
                        if (g_devtab[i].did == did) {
                            g_pci_bus   = b;
                            g_pci_dev   = d;
                            g_pci_func  = fn;
                            g_pci_did   = did;
                            g_card_name = g_devtab[i].name;
                            return 1;
                        }
                    }
                }
                if (fn == 0) {
                    hdr = (unsigned char)(pci_rd32(b,d,0,0x0C) >> 16);
                    if (!(hdr & 0x80)) break;   /* single-function device */
                }
                continue;
next_dev:       if (fn == 0) break;
            }
        }
    }
    return 0;
}

/* =============================================================== */
/*  Radeon MMIO register access                                     */
void setup_palette(void)
{
    unsigned char p[256 * 4];
    int i;

    init_dac();
    memset(p, 0, sizeof p);

    for (i = 0; i < 32; i++) {
        unsigned char v = (unsigned char)(i * 255 / 31);
        p[( 1+i)*4+0]=0; p[( 1+i)*4+1]=0; p[( 1+i)*4+2]=v;   /* 1-32 blue */
        p[(33+i)*4+0]=0; p[(33+i)*4+1]=v; p[(33+i)*4+2]=0;    /* 33-64 green */
        p[(65+i)*4+0]=v; p[(65+i)*4+1]=0; p[(65+i)*4+2]=0;    /* 65-96 red */
        p[(97+i)*4+0]=0; p[(97+i)*4+1]=v; p[(97+i)*4+2]=v;    /* 97-128 cyan */
        p[(129+i)*4+0]=v; p[(129+i)*4+1]=v; p[(129+i)*4+2]=0; /* 129-160 yellow */
        p[(161+i)*4+0]=v; p[(161+i)*4+1]=0; p[(161+i)*4+2]=v; /* 161-192 magenta */
        p[(193+i)*4+0]=v; p[(193+i)*4+1]=v; p[(193+i)*4+2]=v; /* 193-224 grey */
    }
    pal_set(p, 248,  40, 40, 40);
    pal_set(p, 249,  80, 80, 80);
    pal_set(p, 250,   0,255,  0);
    pal_set(p, 251, 255,  0,  0);
    pal_set(p, 252,  64, 64, 64);
    pal_set(p, 253,   0,255,255);
    pal_set(p, 254, 255,255,  0);
    pal_set(p, 255, 255,255,255);

    vbe_set_palette(0, 256, p);
}

/* Mountain/dune-themed palette for the dune-chase demo.
   Carefully chosen ranges so each layer group gets a distinctive colour band:
     1-32:  sky (indigo → pale horizon)
     33-48: far mountains (blue-grey / lavender)
     49-64: mid mountains (purple / deep blue)
     65-80: mid-near terrain (forest green)
     81-96: near terrain (bright emerald)
     97-112: warm brown / sienna
     113-128: rich dark brown
     129-144: bright ochre / orange
     145-160: deep red / crimson
     200-215: worm body (cycling 16-colour rainbow, wraps via (si%16))
     248-255: fixed UI colours */
void setup_pac_palette(void)
{
    unsigned char p[256 * 4];

    init_dac();
    memset(p, 0, sizeof p);

    pal_lerp(p,  1,  5,   0,  0, 40,   0, 40,140);
    pal_lerp(p,  6, 10,   0, 40,140,  20,110,255);
    pal_lerp(p, 11, 15,  20,110,255, 180,220,255);

    pal_lerp(p, 16, 19, 180,180,180, 255,255,255);
    pal_lerp(p, 20, 23, 180,150,  0, 255,255, 80);

    pal_lerp(p, 24, 27, 120,  0,  0, 255, 80, 80);
    pal_lerp(p, 28, 31, 120, 40, 80, 255,160,220);
    pal_lerp(p, 32, 35,   0, 90,120, 120,255,255);
    pal_lerp(p, 36, 39, 150, 70,  0, 255,180, 60);

    pal_lerp(p, 40, 43,  20, 40,110, 120,180,255);
    pal_lerp(p, 44, 47, 220,220,255, 255,255,255);

    pal_lerp(p, 48, 51, 180,  0,  0, 255,120,120);
    pal_lerp(p, 52, 55,  0,140,  0, 180,255,120);

    pal_set(p, 248,  40,  40,  40);
    pal_set(p, 249,  90,  90,  90);
    pal_set(p, 250,   0, 255,   0);
    pal_set(p, 251, 255,   0,   0);
    pal_set(p, 252,  70,  70,  70);
    pal_set(p, 253,   0, 255, 255);
    pal_set(p, 254, 255, 255,   0);
    pal_set(p, 255, 255, 255, 255);

    vbe_set_palette(0, 256, p);
}

void setup_dune_palette(void)
{
    unsigned char p[256 * 4];
    int i;
    static const unsigned char rain[16][3] = {
        {255, 30, 30},{255,100, 10},{255,180, 0},{255,255, 0},
        {180,255, 0},{ 30,255, 30},{  0,255,150},{  0,255,255},
        {  0,150,255},{  0, 80,255},{100, 40,255},{180,  0,255},
        {255,  0,200},{255,  0,100},{255, 60, 60},{255,130, 30}
    };

    init_dac();
    memset(p, 0, sizeof p);

    /* Sky: 1-32 deep indigo → rich blue → pale horizon */
    pal_lerp(p,  1,  8,  10, 5, 40,   20, 15, 80);
    pal_lerp(p,  9, 20,  20, 15, 80,  60, 80,200);
    pal_lerp(p, 21, 24,  60, 80,200, 140,180,240);
    pal_lerp(p, 25, 32, 140,180,240, 220,235,255);

    pal_lerp(p, 33, 40, 120,130,170, 150,150,190);
    pal_lerp(p, 41, 48, 150,150,190, 180,175,210);

    pal_lerp(p, 49, 56,  80, 60,140, 100, 80,180);
    pal_lerp(p, 57, 64, 100, 80,180, 130,100,200);

    pal_lerp(p, 65, 72,  20, 80, 40,  40,120, 50);
    pal_lerp(p, 73, 80,  40,120, 50,  60,150, 60);

    pal_lerp(p, 81, 88,  30,160, 50,  50,200, 70);
    pal_lerp(p, 89, 96,  50,200, 70,  80,230, 90);

    pal_lerp(p,  97,104, 140, 90, 40, 170,110, 50);
    pal_lerp(p, 105,112, 170,110, 50, 200,130, 60);

    pal_lerp(p, 113,120, 100, 60, 30, 130, 75, 35);
    pal_lerp(p, 121,128, 130, 75, 35, 160, 90, 40);

    pal_lerp(p, 129,136, 200,150, 40, 230,170, 30);
    pal_lerp(p, 137,144, 230,170, 30, 255,190, 20);

    pal_lerp(p, 145,152, 180, 40, 30, 210, 50, 35);
    pal_lerp(p, 153,160, 210, 50, 35, 240, 60, 40);

    /* Worm body — cycling bright rainbow (16 colours, index wraps on each frame) */
    for (i = 0; i < 16; i++)
        pal_set(p, 200 + i, rain[i][0], rain[i][1], rain[i][2]);

    pal_set(p, 248,  40, 40, 40);
    pal_set(p, 249,  80, 80, 80);
    pal_set(p, 250,   0,255,  0);
    pal_set(p, 251, 255,  0,  0);
    pal_set(p, 252,  64, 64, 64);
    pal_set(p, 253,   0,255,255);
    pal_set(p, 254, 255,255,  0);
    pal_set(p, 255, 255,255,255);

    vbe_set_palette(0, 256, p);
}

/* =============================================================== */
/*  Radeon MMIO register access                                     */
/*                                                                  */
/*  rreg/wreg use g_mmio (the mapped 64KB MMIO window from BAR2).   */
/*  Register offsets are byte addresses; the pointer is 32-bit wide */
/*  so we index by off/4.                                           */
/* =============================================================== */

unsigned long rreg(unsigned long off)
{ return g_mmio[off >> 2]; }

void wreg(unsigned long off, unsigned long val)
{ g_mmio[off >> 2] = val; }

/* Read RV515 indirect MC register via MC_IND_INDEX/MC_IND_DATA.
   Bit mask 0x7F0000 in the index write enables read access. */
unsigned long mc_rreg(unsigned long reg)
{
    unsigned long r;
    wreg(R_MC_IND_INDEX, 0x7F0000UL | (reg & 0xFFFF));
    r = rreg(R_MC_IND_DATA);
    wreg(R_MC_IND_INDEX, 0);   /* deassert index to prevent stale reads */
    return r;
}

/* Read PLL register via CLOCK_CNTL_INDEX/DATA.
   Only the lower 6 bits of the index are used; PLL_WR_EN must NOT
   be set for reads (only set it when writing). */
unsigned long pll_rreg(unsigned long reg)
{
    unsigned long r;
    wreg(R_CLOCK_CNTL_INDEX, reg & 0x3FUL);
    r = rreg(R_CLOCK_CNTL_DATA);
    return r;
}

/* Write PLL register — must set PLL_WR_EN (bit 7) in the index. */
void pll_wreg(unsigned long reg, unsigned long val)
{
    wreg(R_CLOCK_CNTL_INDEX, (reg & 0x3FUL) | PLL_WR_EN);
    wreg(R_CLOCK_CNTL_DATA, val);
}

/* =============================================================== */
/*  GPU engine control helpers                                      */
/* =============================================================== */

/* Mask VGA vertical status control bits — stops the VGA block from
   scanning out and interfering with the 2D engine.
   Equivalent to Linux rv515_vga_render_disable(). */
void gpu_vga_render_disable(void)
{
    wreg(R_VGA_RENDER_CONTROL,
         rreg(R_VGA_RENDER_CONTROL) & ~VGA_VSTATUS_CNTL_MASK);
}

/* Spin-wait for the RV515 memory controller idle flag.
   MC_STATUS bit 4 = MC_STATUS_IDLE.  Returns 1 on idle, 0 on timeout.
   Used before GPU re-init to ensure no DMA is in flight. */
int gpu_mc_wait_idle(void)
{
    unsigned long timeout = 2000000UL;
    while (timeout--) {
        if (mc_rreg(RV515_MC_STATUS) & MC_STATUS_IDLE)
            return 1;
    }
    return 0;
}

/* Wait for n free FIFO entries.
   g_fifo_free tracks entries already known to be free from the last
   RBBM_STATUS read, avoiding expensive MMIO reads in tight loops. */
void gpu_wait_fifo(int n)
{
    unsigned long timeout;
    int avail;
    if (g_fifo_free >= n) {
        g_fifo_free -= n;
        return;
    }
    timeout = 2000000UL;
    while (timeout--) {
        avail = (int)(rreg(R_RBBM_STATUS) & RBBM_FIFOCNT_MASK);
        if (avail >= n) {
            g_fifo_free = avail - n;
            return;
        }
    }
}

/* Flush the R300+/R500 2D destination cache.
   Must be called after all GPU writes before the CPU reads VRAM back.
   RB2D_DC_FLUSH_ALL triggers a full cache writeback; we spin on
   RB2D_DC_BUSY until the flush completes. */
void gpu_engine_flush(void)
{
    unsigned long timeout = 2000000UL;
    wreg(R_RB2D_DSTCACHE_CTLSTAT, RB2D_DC_FLUSH_ALL);
    while (timeout--) {
        if (!(rreg(R_RB2D_DSTCACHE_CTLSTAT) & RB2D_DC_BUSY))
            return;
    }
}

/* Wait for the 2D/3D engine to go fully idle.
   First drain the FIFO (wait for 64 free entries = empty), then wait
   for RBBM_ACTIVE to clear, then flush the destination cache.
   After this returns, all GPU work is visible in VRAM. */
void gpu_wait_idle(void)
{
    unsigned long timeout = 2000000UL;
    gpu_wait_fifo(64);
    while (timeout--) {
        if (!(rreg(R_RBBM_STATUS) & RBBM_ACTIVE))
            break;
    }
    gpu_engine_flush();
    g_fifo_free = 64;   /* engine idle → FIFO fully drained, all 64 slots free */
}

/* =============================================================== */
/*  GPU engine reset (RADEON.C full sequence)                       */
/*                                                                  */
/*  Derived from xf86-video-ati RADEONEngineReset and Linux r100.c  */
/*  For R300+/R500 the soft-reset sequence resets only CP, HI, E2   */
/*  — leaving SE/RE/PP/RB alone prevents lockups.                   */
/* =============================================================== */

void gpu_engine_reset(void)
{
    unsigned long rbbm_soft_reset;
    unsigned long host_path_cntl;
    unsigned long clock_cntl_index;

    /* Save CLOCK_CNTL_INDEX to avoid disturbing PLL access state */
    clock_cntl_index = rreg(R_CLOCK_CNTL_INDEX);

    /*
     * R300+/R500 soft reset sequence:
     * Assert reset on CP, HI and E2 only — SE/RE/PP/RB must NOT be
     * reset here on R300+, doing so causes a GPU lockup.
     * Write zero to deassert ALL reset bits (xf86 style, not per-bit).
     * The flush read after each write ensures the write is posted
     * to the hardware before proceeding.
     */
    rbbm_soft_reset = rreg(R_RBBM_SOFT_RESET);
    wreg(R_RBBM_SOFT_RESET, rbbm_soft_reset |
         SOFT_RESET_CP | SOFT_RESET_HI | SOFT_RESET_E2);
    (void)rreg(R_RBBM_SOFT_RESET);   /* flush posted write */
    wreg(R_RBBM_SOFT_RESET, 0);      /* clear ALL reset bits (xf86 style) */
    (void)rreg(R_RBBM_SOFT_RESET);   /* flush */

    /* Enable RB3D dst-cache autoflush (bit 17) — must be set after
       soft-reset for R300+ (from xf86-video-ati IS_R300/IS_AVIVO path) */
    wreg(R_RB3D_DSTCACHE_MODE, rreg(R_RB3D_DSTCACHE_MODE) | (1UL << 17));

    /*
     * Reset HDP via HOST_PATH_CNTL toggle instead of RBBM_SOFT_RESET.
     * Direct RBBM reset of HDP can cause issues on some ASICs — the
     * Linux and xf86 drivers use the HOST_PATH_CNTL path instead.
     * (from Linux kernel r100.c, xf86-video-ati RADEONEngineReset)
     */
    host_path_cntl = rreg(R_HOST_PATH_CNTL);
    wreg(R_HOST_PATH_CNTL, host_path_cntl | HDP_SOFT_RESET | HDP_APER_CNTL);
    (void)rreg(R_HOST_PATH_CNTL);
    wreg(R_HOST_PATH_CNTL, host_path_cntl | HDP_APER_CNTL);
    (void)rreg(R_HOST_PATH_CNTL);

    /* Enable 2D destination cache autoflush (R300+/R500) */
    wreg(R_RB2D_DSTCACHE_MODE, rreg(R_RB2D_DSTCACHE_MODE) |
         (1UL << 2) | (1UL << 15));  /* DC_AUTOFLUSH | DC_DISABLE_IGNORE_PE */

    /* Restore CLOCK_CNTL_INDEX */
    wreg(R_CLOCK_CNTL_INDEX, clock_cntl_index);
}

/* =============================================================== */
/*  Radeon R500 2D engine initialization                            */
/*                                                                  */
/*  Full sequence derived from Linux rv515.c, r100.c and           */
/*  xf86-video-ati RADEONEngineInit/EngineRestore.                 */
/*  Order matters: pipe init → tile config → cache → reset → ring. */
/* =============================================================== */

void gpu_init_2d(void)
{
    unsigned long pitch64;
    unsigned long pitch_offset;
    unsigned long gb_pipe_sel;
    unsigned long gb_tile_config;
    unsigned long pipe_sel_current;
    unsigned long dst_pipe_val;
    unsigned long pll_tmp;

    /* ---- Phase 1: GPU init (from Linux rv515_gpu_init) ---- */

    gpu_wait_idle();

    /* Disable VGA rendering to prevent interference with 2D engine */
    gpu_vga_render_disable();

    /* Detect number of GB pipes from GB_PIPE_SELECT register.
       Bits [13:12] contain (num_pipes - 1).
       (from xf86-video-ati RADEONEngineInit IS_R500_3D + Linux r420_pipes_init) */
    gb_pipe_sel   = rreg(R_GB_PIPE_SELECT);
    g_num_gb_pipes = (int)((gb_pipe_sel >> 12) & 0x3) + 1;

    /* R500 pipe memory power configuration.
       Bits [11:8] of GB_PIPE_SELECT = active pipe mask.
       Bit 0 enables dynamic power gating. */
    pll_tmp = (1UL | (((gb_pipe_sel >> 8) & 0xFUL) << 4));
    pll_wreg(R500_DYN_SCLK_PWMEM_PIPE, pll_tmp);

    /* Combine current DST_PIPE_CONFIG selection with the active pipe mask.
       (from Linux rv515_gpu_init after r420_pipes_init) */
    dst_pipe_val      = rreg(R_DST_PIPE_CONFIG);
    pipe_sel_current  = (dst_pipe_val >> 2) & 3;
    pll_wreg(R500_DYN_SCLK_PWMEM_PIPE,
             (1UL << pipe_sel_current) |
             (((gb_pipe_sel >> 8) & 0xFUL) << 4));

    gpu_wait_idle();
    gpu_mc_wait_idle();

    /* ---- Phase 2: Tile and cache config (done BEFORE engine reset,
       per xf86-video-ati RADEONEngineInit IS_R300/IS_R500 ordering) ---- */

    gb_tile_config = GB_TILE_ENABLE | GB_TILE_SIZE_16;
    switch (g_num_gb_pipes) {
    case 2:  gb_tile_config |= (1UL << 1); break;  /* R300_PIPE_COUNT_R300 */
    case 3:  gb_tile_config |= (2UL << 1); break;  /* R300_PIPE_COUNT_R420_3P */
    case 4:  gb_tile_config |= (3UL << 1); break;  /* R300_PIPE_COUNT_R420 */
    default: gb_tile_config |= GB_PIPE_COUNT_RV350; break;  /* single pipe */
    }
    wreg(R_GB_TILE_CONFIG, gb_tile_config);

    wreg(R_WAIT_UNTIL, WAIT_2D_IDLECLEAN | WAIT_3D_IDLECLEAN);

    /* Enable auto-configuration of destination pipe routing */
    wreg(R_DST_PIPE_CONFIG, rreg(R_DST_PIPE_CONFIG) | PIPE_AUTO_CONFIG);

    /* Enable RB2D cache autoflush BEFORE engine reset (order matters here) */
    wreg(R_RB2D_DSTCACHE_MODE, rreg(R_RB2D_DSTCACHE_MODE) |
         (1UL << 2) | (1UL << 15));

    /* ---- Phase 3: Engine reset ---- */
    gpu_engine_reset();

    /* ---- Phase 4: Ring-start-equivalent initialization.
       Normally submitted via CP ring; we write directly via MMIO for DOS.
       (from Linux rv515_ring_start) ---- */

    gpu_wait_fifo(12);

    /* 2D/3D sync: stall 2D when 3D active and vice versa */
    wreg(R_ISYNC_CNTL,
         ISYNC_ANY2D_IDLE3D | ISYNC_ANY3D_IDLE2D |
         ISYNC_WAIT_IDLEGUI | ISYNC_CPSCRATCH_IDLEGUI);

    wreg(R_WAIT_UNTIL, WAIT_2D_IDLECLEAN | WAIT_3D_IDLECLEAN);
    wreg(R_DST_PIPE_CONFIG, rreg(R_DST_PIPE_CONFIG) | PIPE_AUTO_CONFIG);

    wreg(R_GB_SELECT, 0);
    wreg(R_GB_ENABLE, 0);

    /* R500: SU_REG_DEST = bitmask of active pipes */
    wreg(R_SU_REG_DEST, (1UL << g_num_gb_pipes) - 1);
    wreg(R_VAP_INDEX_OFFSET, 0);

    /* Flush RB3D and ZB caches */
    wreg(R_RB3D_DSTCACHE_CTLSTAT_RS, RB3D_DC_FLUSH_RS | RB3D_DC_FREE_RS);
    wreg(R_ZB_ZCACHE_CTLSTAT, ZC_FLUSH | ZC_FREE);
    wreg(R_WAIT_UNTIL, WAIT_2D_IDLECLEAN | WAIT_3D_IDLECLEAN);

    wreg(R_GB_AA_CONFIG, 0);   /* anti-aliasing off */

    /* Flush again after AA config change */
    wreg(R_RB3D_DSTCACHE_CTLSTAT_RS, RB3D_DC_FLUSH_RS | RB3D_DC_FREE_RS);
    wreg(R_ZB_ZCACHE_CTLSTAT, ZC_FLUSH | ZC_FREE);

    gpu_wait_fifo(4);

    /* Geometry arbiter: deadlock and fastsync prevention */
    wreg(R_GA_ENHANCE, GA_DEADLOCK_CNTL | GA_FASTSYNC_CNTL);

    /* RBBM GUI control: byte-swap = none (x86 is little-endian) */
    wreg(R_RBBM_GUICNTL, 0);

    gpu_wait_idle();
    gpu_engine_flush();

    /* ---- Phase 5: 2D engine setup (from xf86-video-ati RADEONEngineRestore) ---- */

    /* Encode pitch/offset for the combined register.
       Pitch field is in 64-byte units at bits [29:22].
       Offset field is in 1024-byte units at bits [21:0].
       Offset encodes the GPU internal FB base from HDP_FB_LOCATION. */
    pitch64      = ((unsigned long)g_pitch + 63) / 64;
    pitch_offset = (pitch64 << 22) | ((g_fb_location >> 10) & 0x003FFFFFUL);
    g_default_po = pitch_offset;   /* save for PITCH_OFFSET blits in demo code */

    gpu_wait_fifo(18);

    /* Combined pitch/offset (FIFO-queued on R300+/R500; do not rely on readback) */
    wreg(R_DEFAULT_PITCH_OFFSET, pitch_offset);
    wreg(R_DST_PITCH_OFFSET,     pitch_offset);
    wreg(R_SRC_PITCH_OFFSET,     pitch_offset);

    /* Separate pitch/offset registers — used when PITCH_OFFSET_CNTL bits are
       NOT set in DP_GUI_MASTER_CNTL.  Setting both ensures a known-good state
       regardless of which path the GPU chooses for a given operation. */
    wreg(R_DST_OFFSET, g_fb_location);
    wreg(R_DST_PITCH,  (unsigned long)g_pitch);
    wreg(R_SRC_OFFSET, g_fb_location);
    wreg(R_SRC_PITCH,  (unsigned long)g_pitch);

    /* Full scissor: 0x1FFF clamps at 8191 for the default (very large) rect;
       the per-operation scissor is set tightly in the 2D primitives. */
    wreg(R_DEFAULT_SC_BOTTOM_RIGHT, (0x1FFF << 16) | 0x1FFF);
    wreg(R_SC_TOP_LEFT, 0);
    wreg(R_SC_BOTTOM_RIGHT,
         ((unsigned long)g_yres << 16) | (unsigned long)g_xres);

    wreg(R_DP_WRITE_MASK, 0xFFFFFFFF);
    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);

    /* Initialize brush and source colours to known state
       (from xf86-video-ati RADEONEngineRestore — avoids undefined register
       state that can produce garbage on first operation after reset) */
    wreg(R_DP_BRUSH_FRGD_CLR, 0xFFFFFFFF);
    wreg(R_DP_BRUSH_BKGD_CLR, 0x00000000);
    wreg(R_DP_SRC_FRGD_CLR,   0xFFFFFFFF);
    wreg(R_DP_SRC_BKGD_CLR,   0x00000000);

    /* Surface byte-order: no swap on x86 little-endian */
    wreg(R_SURFACE_CNTL, 0);

    /* Disable colour compare (clean state for first blit operation) */
    wreg(R_CLR_CMP_CNTL, 0);

    gpu_wait_idle();
}

/* =============================================================== */
/*  Page-flip helpers                                               */
/*                                                                  */
/*  AVIVO path: hardware D1GRPH surface-address update with vsync   */
/*              lock/unlock protocol.                               */
/*  VGA path:   poll port 0x3DA for retrace, then call VBE 4F07h.  */
/* =============================================================== */

/* Detect whether AVIVO D1GRPH controls the display.
   After VBE mode set, ATOMBIOS typically configures D1GRPH registers.
   If D1GRPH_PRIMARY_SURFACE_ADDRESS matches our VRAM range AND the
   VGA block is (or can be) disabled, AVIVO hardware flip is available.

   Note: when D1VGA_MODE_ENABLE is set after VBE mode set, the VGA
   block generates scanout and D1GRPH surface register changes are
   ignored by the CRTC.  We attempt to disable it here. */
void detect_flip_mode(void)
{
    unsigned long cur_surf = rreg(R_D1GRPH_PRIMARY_SURFACE_ADDRESS);
    unsigned long d1vga   = rreg(R_D1VGA_CONTROL);

    g_avivo_flip = 0;  /* default: VGA path (always works as fallback) */

    /* Surface address check: must be within our VRAM aperture */
    if ((cur_surf & 0xFFF00000UL) != (g_fb_location & 0xFFF00000UL))
        return;

    if (!(d1vga & D1VGA_MODE_ENABLE)) {
        /* VGA already disabled — AVIVO D1GRPH is driving the display */
        g_avivo_flip = 1;
        return;
    }

    /* VGA mode active.  Disable it so AVIVO D1GRPH takes over.
       ATOMBIOS sets D1GRPH registers during mode set, so this is safe. */
    wreg(R_D1VGA_CONTROL, d1vga & ~D1VGA_MODE_ENABLE);

    /* Program display window — ATOMBIOS may set X_END = pitch (e.g. 832)
       instead of xres (800), causing a dark 32-pixel strip on the right.
       Set X_END = xres explicitly to avoid this artefact. */
    wreg(R_D1GRPH_PITCH,   (unsigned long)g_pitch);
    wreg(R_D1GRPH_X_START, 0);
    wreg(R_D1GRPH_Y_START, 0);
    wreg(R_D1GRPH_X_END,   (unsigned long)g_xres);
    wreg(R_D1GRPH_Y_END,   (unsigned long)g_yres);

    /* Brief spin to let the display controller latch the new configuration */
    { volatile int d; for (d = 0; d < 50000; d++) {} }

    if (!(rreg(R_D1VGA_CONTROL) & D1VGA_MODE_ENABLE)) {
        g_avivo_flip = 1;
    }
    /* If VGA disable didn't stick, g_avivo_flip remains 0 → VGA fallback */
}

/* Wait for AVIVO CRTC to enter vertical blank.
   First exits any ongoing vblank, then waits for the next one. */
void avivo_wait_vblank(void)
{
    int i;
    for (i = 0; i < 500000; i++)
        if (!(rreg(R_D1CRTC_STATUS) & D1CRTC_V_BLANK)) break;
    for (i = 0; i < 500000; i++)
        if (rreg(R_D1CRTC_STATUS) & D1CRTC_V_BLANK) break;
}

/* AVIVO hardware page flip.
   Protocol (per Linux rs600_page_flip):
     1. Lock surface update: prevents latching a partial address
     2. Request flip at vsync (not hsync) via FLIP_CONTROL
     3. Write new surface address (both primary and secondary)
     4. Unlock: arms the flip — hardware applies at next vsync
     5. Wait for SURFACE_UPDATE_PENDING high→low (flip applied)
        Fallback: wait for CRTC vblank if pending bit doesn't signal
                  (observed on RV515 X1300 DevID 7142) */
void hw_page_flip(unsigned long surface_addr)
{
    unsigned long tmp;
    int i;

    tmp = rreg(R_D1GRPH_UPDATE);
    wreg(R_D1GRPH_UPDATE, tmp | D1GRPH_SURFACE_UPDATE_LOCK);

    wreg(R_D1GRPH_FLIP_CONTROL, 0);   /* flip at vsync, not hsync */

    wreg(R_D1GRPH_SECONDARY_SURFACE_ADDRESS, surface_addr);
    wreg(R_D1GRPH_PRIMARY_SURFACE_ADDRESS,   surface_addr);

    /* Unlock: this arms the flip — hardware latches the new address
       at the next vertical retrace */
    tmp = rreg(R_D1GRPH_UPDATE);
    wreg(R_D1GRPH_UPDATE, tmp & ~D1GRPH_SURFACE_UPDATE_LOCK);

    /* Wait for the pending bit to go high (flip is queued) */
    for (i = 0; i < 4000; i++) {
        if (rreg(R_D1GRPH_UPDATE) & D1GRPH_SURFACE_UPDATE_PENDING)
            break;
    }

    if (rreg(R_D1GRPH_UPDATE) & D1GRPH_SURFACE_UPDATE_PENDING) {
        /* Pending is high — wait for it to clear (flip applied at vsync) */
        for (i = 0; i < 300000; i++) {
            if (!(rreg(R_D1GRPH_UPDATE) & D1GRPH_SURFACE_UPDATE_PENDING))
                return;
        }
    } else {
        /* Pending did not assert — fall back to CRTC vblank.
           This is observed on some RV515 X1300 parts (DevID 7142). */
        avivo_wait_vblank();
    }
}

/* =============================================================== */
/*  VESA mode selection: find best 8bpp LFB mode ≤ 1024×768        */
/*  Thin wrapper around DOSLIB's find_vbe_mode().                   */
/* =============================================================== */

int find_mode(void)
{
    return find_vbe_mode(800, 600, 8, 1024, 768);
}
/* Unified page flip: selects AVIVO or VGA path based on g_avivo_flip.
   back_page = 0 or 1 (double-buffer index). */
void flip_page(int back_page)
{
    if (g_avivo_flip) {
        /* AVIVO hardware flip blocks until the new surface is live */
        hw_page_flip(g_fb_location +
                     (unsigned long)back_page * g_pitch * g_page_stride);
    } else {
        /* VGA mode: synchronise manually then update display start address */
        RMI rm;
        wait_vsync();
        memset(&rm, 0, sizeof rm);
        rm.eax = 0x4F07;
        rm.ebx = 0x0000;     /* BH=00 = set immediately (retrace already waited) */
        rm.ecx = 0;
        rm.edx = (unsigned long)(back_page * g_page_stride);
        dpmi_rmint(0x10, &rm);
    }
}

/* Restore display to page 0 using the current flip mode */
void flip_restore_page0(void)
{
    if (g_avivo_flip) {
        hw_page_flip(g_fb_location);
    } else {
        RMI rm;
        memset(&rm, 0, sizeof rm);
        rm.eax = 0x4F07;
        rm.ebx = 0x0000;
        rm.ecx = 0;
        rm.edx = 0;
        dpmi_rmint(0x10, &rm);
    }
}

/* =============================================================== */
/*  GPU 2D drawing primitives                                       */
/* =============================================================== */

/* Hardware-accelerated solid rectangle fill */
void gpu_fill(int x, int y, int w, int h, unsigned char color)
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
void gpu_fill_setup(void)
{
    gpu_wait_fifo(3);
    wreg(R_DP_GUI_MASTER_CNTL,
         GMC_BRUSH_SOLID | GMC_DST_8BPP | GMC_SRC_DATATYPE_COLOR |
         ROP3_PATCOPY | GMC_CLR_CMP_DIS | GMC_WR_MSK_DIS);
    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
    wreg(R_CLR_CMP_CNTL, 0);
}

/* Fast solid fill — only 3 register writes (assumes gpu_fill_setup). */
void gpu_fill_fast(int x, int y, int w, int h, unsigned char color)
{
    gpu_wait_fifo(3);
    wreg(R_DP_BRUSH_FRGD_CLR, (unsigned long)color);
    wreg(R_DST_Y_X, ((unsigned long)y << 16) | (unsigned long)x);
    wreg(R_DST_HEIGHT_WIDTH, ((unsigned long)h << 16) | (unsigned long)w);
}

/* Ultra-fast fill — 2 register writes, same color as previous fill. */
void gpu_fill_rect(int x, int y, int w, int h)
{
    gpu_wait_fifo(2);
    wreg(R_DST_Y_X, ((unsigned long)y << 16) | (unsigned long)x);
    wreg(R_DST_HEIGHT_WIDTH, ((unsigned long)h << 16) | (unsigned long)w);
}

/* Set fill color once for a batch of gpu_fill_rect() calls. */
void gpu_fill_set_color(unsigned char color)
{
    gpu_wait_fifo(1);
    wreg(R_DP_BRUSH_FRGD_CLR, (unsigned long)color);
}

/* Hardware-accelerated screen-to-screen blit */
void gpu_blit(int sx, int sy, int dx, int dy, int w, int h)
{
    unsigned long dp = 0;
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

/* Forward blit (no overlap detection) */
void gpu_blit_fwd(int sx, int sy, int dx, int dy, int w, int h)
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

/* Forward blit with source color-key transparency */
void gpu_blit_key(int sx, int sy, int dx, int dy, int w, int h,
                  unsigned char key)
{
    gpu_wait_fifo(8);
    wreg(R_CLR_CMP_CLR_SRC, (unsigned long)key);
    wreg(R_CLR_CMP_MASK,    0x000000FFUL);
    wreg(R_CLR_CMP_CNTL,    CLR_CMP_SRC_SOURCE | CLR_CMP_FCN_NE);
    wreg(R_DP_GUI_MASTER_CNTL,
         GMC_BRUSH_NONE | GMC_DST_8BPP | GMC_SRC_DATATYPE_COLOR |
         ROP3_SRCCOPY | GMC_DP_SRC_MEMORY | GMC_WR_MSK_DIS);
    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
    wreg(R_SRC_Y_X, ((unsigned long)sy << 16) | (unsigned long)(sx & 0xFFFF));
    wreg(R_DST_Y_X, ((unsigned long)dy << 16) | (unsigned long)(dx & 0xFFFF));
    wreg(R_DST_HEIGHT_WIDTH, ((unsigned long)h << 16) | (unsigned long)w);
}

/* Hardware-accelerated line drawing */
void gpu_line(int x1, int y1, int x2, int y2, unsigned char color)
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

/* Build a PITCH_OFFSET register value for a given VRAM byte offset */
unsigned long make_pitch_offset(unsigned long vram_byte_off)
{
    unsigned long pitch64  = (unsigned long)g_pitch / 64;
    unsigned long gpu_addr = g_fb_location + vram_byte_off;
    return (pitch64 << 22) | ((gpu_addr >> 10) & 0x003FFFFFUL);
}

/* Forward blit with explicit per-surface PITCH_OFFSET values */
void gpu_blit_po(unsigned long src_po, int sx, int sy,
                 unsigned long dst_po, int dx, int dy, int w, int h)
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

/* Keyed blit with explicit per-surface PITCH_OFFSET values */
void gpu_blit_po_key(unsigned long src_po, int sx, int sy,
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

/* Parallax blit with scroll wrapping */
void plax_blit_wrap_po(unsigned long src_po, int scroll_x,
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

/* Flush 2D destination cache */
void gpu_flush_2d_cache(void)
{
    gpu_wait_fifo(2);
    wreg(R_DSTCACHE_CTLSTAT, R300_RB2D_DC_FLUSH_ALL);
    wreg(R_WAIT_UNTIL, WAIT_2D_IDLECLEAN | WAIT_DMA_GUI_IDLE);
}

/* =============================================================== */
/*  GPU state helpers                                                */
/* =============================================================== */

/* Widen scissor to maximum for offscreen VRAM operations */
void gpu_scissor_max(void)
{
    gpu_wait_fifo(1);
    wreg(R_SC_BOTTOM_RIGHT, (0x3FFFUL << 16) | (unsigned long)g_xres);
}

/* Restore scissor to current display dimensions */
void gpu_scissor_default(void)
{
    gpu_wait_fifo(1);
    wreg(R_SC_BOTTOM_RIGHT,
         ((unsigned long)g_yres << 16) | (unsigned long)g_xres);
}

/* Disable color compare (needed after keyed blits) */
void gpu_color_compare_off(void)
{
    gpu_wait_fifo(1);
    wreg(R_CLR_CMP_CNTL, 0);
}

/* Reset DST and SRC PITCH_OFFSET to the default framebuffer */
void gpu_pitch_offset_reset(void)
{
    gpu_wait_fifo(2);
    wreg(R_DST_PITCH_OFFSET, g_default_po);
    wreg(R_SRC_PITCH_OFFSET, g_default_po);
}

/* =============================================================== */
/*  Batched fill with internal FIFO management                      */
/* =============================================================== */

/* MMIO array indices for direct register writes */
#define MMIO_DST_Y_X           (R_DST_Y_X >> 2)
#define MMIO_DST_HEIGHT_WIDTH  (R_DST_HEIGHT_WIDTH >> 2)
#define MMIO_DP_BRUSH_FRGD_CLR (R_DP_BRUSH_FRGD_CLR >> 2)

#define BATCH2_SIZE  32   /* max rects per 2-reg batch (64 FIFO entries) */
#define BATCH3_SIZE  21   /* max rects per 3-reg batch (63 FIFO entries) */

static int g_batch_pending = 0;

/* Batched 2-reg fill (position + size only, color set via gpu_fill_set_color).
   Call gpu_fill_setup() + gpu_fill_set_color() before the batch loop. */
void gpu_fill_batch_rect(int x, int y, int w, int h)
{
    if (g_batch_pending == 0)
        gpu_wait_fifo(BATCH2_SIZE * 2);
    g_mmio[MMIO_DST_Y_X] =
        ((unsigned long)y << 16) | (unsigned long)x;
    g_mmio[MMIO_DST_HEIGHT_WIDTH] =
        ((unsigned long)h << 16) | (unsigned long)w;
    if (++g_batch_pending >= BATCH2_SIZE) {
        g_fifo_free = 0;
        g_batch_pending = 0;
    }
}

/* Batched 3-reg fill (color + position + size per rect).
   Call gpu_fill_setup() before the batch loop. */
void gpu_fill_batch_color(int x, int y, int w, int h, unsigned char color)
{
    if (g_batch_pending == 0)
        gpu_wait_fifo(BATCH3_SIZE * 3);
    g_mmio[MMIO_DP_BRUSH_FRGD_CLR] = (unsigned long)color;
    g_mmio[MMIO_DST_Y_X] =
        ((unsigned long)y << 16) | (unsigned long)x;
    g_mmio[MMIO_DST_HEIGHT_WIDTH] =
        ((unsigned long)h << 16) | (unsigned long)w;
    if (++g_batch_pending >= BATCH3_SIZE) {
        g_fifo_free = 0;
        g_batch_pending = 0;
    }
}

/* Flush batch state — call after a batch loop completes */
void gpu_fill_batch_flush(void)
{
    g_fifo_free = 0;
    g_batch_pending = 0;
}
