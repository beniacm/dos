/*
 * RADEON.C  -  ATI Radeon X1300 Pro (RV515/RV516) DOS Demo
 * For OpenWatcom 2.0, 32-bit DOS (DOS/4GW)
 *
 * Demonstrates direct hardware access to the ATI Radeon from DOS:
 *   - PCI bus scanning and card identification
 *   - Memory-mapped GPU register I/O via DPMI
 *   - Hardware-accelerated 2D solid-fill rectangles
 *   - Hardware-accelerated screen-to-screen blit
 *   - CPU vs GPU framebuffer fill benchmark
 *   - Animated GPU rectangle flood with throughput counter
 *   - GPU parallax scrolling (4 layers, color-key transparency)
 *
 * Requires actual Radeon RV515/RV516 hardware (or compatible).
 * Build: wcl386 -bt=dos -l=dos4g -ox radeon.c
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <i86.h>
#include <time.h>

/* =============================================================== */
/*  Packed structures                                               */
/* =============================================================== */

#pragma pack(1)

typedef struct {
    char           sig[4];
    unsigned short ver;
    unsigned long  oem_str;
    unsigned long  caps;
    unsigned long  mode_ptr;
    unsigned short tot_mem;
    unsigned char  _pad[482];
} VBEInfo;

typedef struct {
    unsigned short attr;
    unsigned char  wina, winb;
    unsigned short gran, winsz, sega, segb;
    unsigned long  winfn;
    unsigned short pitch;
    unsigned short xres, yres;
    unsigned char  xch, ych, planes, bpp, banks, model, bksz, pages, r1;
    unsigned char  rmsk, rpos, gmsk, gpos, bmsk, bpos, amsk, apos, dcm;
    unsigned long  lfb_phys;
    unsigned char  _pad[212];
} VBEMode;

typedef struct {
    unsigned long  edi, esi, ebp, _rz;
    unsigned long  ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs;
    unsigned short ip, cs, sp, ss;
} RMI;

#pragma pack()

/* =============================================================== */
/*  ATI Radeon register offsets (R300-R500 compatible 2D engine)    */
/*  Reference: Linux kernel radeon_reg.h, xf86-video-ati           */
/* =============================================================== */

/* Bus / config registers */
#define R_MC_IND_INDEX             0x0070
#define R_MC_IND_DATA              0x0074
#define R_RBBM_SOFT_RESET          0x00F0
#define   SOFT_RESET_CP            (1UL << 0)
#define   SOFT_RESET_HI            (1UL << 1)
#define   SOFT_RESET_SE            (1UL << 2)
#define   SOFT_RESET_RE            (1UL << 3)
#define   SOFT_RESET_PP            (1UL << 4)
#define   SOFT_RESET_E2            (1UL << 5)
#define   SOFT_RESET_RB            (1UL << 6)
#define   SOFT_RESET_HDP           (1UL << 7)
#define R_CONFIG_MEMSIZE           0x00F8
#define R_HOST_PATH_CNTL           0x0130
#define R_HDP_FB_LOCATION          0x0134
#define R_SURFACE_CNTL             0x0B00

/* RV515 indirect MC register numbers (via MC_IND_INDEX/MC_IND_DATA) */
#define RV515_MC_FB_LOCATION       0x0001

#define R_RBBM_STATUS              0x0E40
#define   RBBM_FIFOCNT_MASK        0x007F
#define   RBBM_ACTIVE              (1UL << 31)

/* 2D engine registers */
/* NOTE: R_DSTCACHE_CTLSTAT (0x1714) is the R100/R200 cache flush register.
   For R300+/R500, use R_RB2D_DSTCACHE_CTLSTAT (0x342C) instead. */

#define R_WAIT_UNTIL               0x1720
#define   WAIT_2D_IDLE             (1UL << 14)
#define   WAIT_3D_IDLE             (1UL << 15)
#define   WAIT_2D_IDLECLEAN        (1UL << 16)
#define   WAIT_3D_IDLECLEAN        (1UL << 17)
#define R_RBBM_GUICNTL             0x172C

#define R_DST_OFFSET               0x1404
#define R_DST_PITCH                0x1408
#define R_DST_WIDTH                0x140C
#define R_DST_HEIGHT               0x1410
#define R_DST_Y_X                  0x1438
#define R_DST_HEIGHT_WIDTH         0x143C

#define R_SRC_OFFSET               0x15AC
#define R_SRC_PITCH                0x15B0
#define R_SRC_Y_X                  0x1434

#define R_DST_PITCH_OFFSET         0x142C
#define R_SRC_PITCH_OFFSET         0x1428

#define R_DP_GUI_MASTER_CNTL       0x146C
#define   GMC_BRUSH_SOLID          (13UL << 4)
#define   GMC_BRUSH_NONE           (15UL << 4)
#define   GMC_DST_8BPP             (2UL << 8)
#define   GMC_DST_16BPP            (4UL << 8)
#define   GMC_DST_32BPP            (6UL << 8)
#define   GMC_SRC_DATATYPE_COLOR   (3UL << 12)
#define   GMC_DP_SRC_MEMORY        (2UL << 24)
#define   GMC_CLR_CMP_DIS          (1UL << 28)
#define   GMC_WR_MSK_DIS           (1UL << 30)
#define   GMC_SRC_PITCH_OFFSET_CNTL (1UL << 0)
#define   GMC_DST_PITCH_OFFSET_CNTL (1UL << 1)
#define   ROP3_PATCOPY             (0xF0UL << 16)
#define   ROP3_SRCCOPY             (0xCCUL << 16)

#define R_DP_BRUSH_FRGD_CLR        0x147C
#define R_DP_BRUSH_BKGD_CLR        0x1478
#define R_DP_SRC_FRGD_CLR          0x15D8
#define R_DP_SRC_BKGD_CLR          0x15DC
#define R_DP_CNTL                  0x16C0
#define   DST_X_LEFT_TO_RIGHT      1UL
#define   DST_Y_TOP_TO_BOTTOM      2UL
#define R_DP_DATATYPE              0x16C4
#define R_DP_MIX                   0x16C8
#define R_DP_WRITE_MASK            0x16CC
#define R_DEFAULT_PITCH_OFFSET     0x16E0

#define R_DEFAULT_SC_BOTTOM_RIGHT  0x16E8
#define R_SC_TOP_LEFT              0x16EC
#define R_SC_BOTTOM_RIGHT          0x16F0

#define R_DST_LINE_START           0x1600
#define R_DST_LINE_END             0x1604
#define R_DST_LINE_PATCOUNT        0x1608

/* R300+ specific registers */
#define R_DSTCACHE_CTLSTAT         0x1714
#define   R300_DC_FLUSH_2D         (1UL << 0)
#define   R300_DC_FREE_2D          (1UL << 2)
#define   R300_RB2D_DC_FLUSH_ALL   0x05UL
#define R_WAIT_UNTIL               0x1720
#define   WAIT_2D_IDLECLEAN        (1UL << 16)
#define   WAIT_DMA_GUI_IDLE        (1UL << 9)
#define R_RB3D_DSTCACHE_MODE       0x3258
#define R_RB2D_DSTCACHE_MODE       0x3428
#define R_RB2D_DSTCACHE_CTLSTAT    0x342C
#define   RB2D_DC_FLUSH            (3UL << 0)
#define   RB2D_DC_FREE             (3UL << 2)
#define   RB2D_DC_FLUSH_ALL        0x0FUL
#define   RB2D_DC_BUSY             (1UL << 31)
#define R_GB_TILE_CONFIG           0x4018
#define   GB_TILE_ENABLE           (1UL << 0)
#define   GB_TILE_SIZE_16          (1UL << 4)
#define   GB_PIPE_COUNT_RV350      (0UL << 1)
#define R_DST_PIPE_CONFIG          0x170C
#define   PIPE_AUTO_CONFIG         (1UL << 31)
#define R_GB_PIPE_SELECT           0x402C

/* Color compare registers (for transparency keying) */
#define R_CLR_CMP_CNTL            0x15C0
#define R_CLR_CMP_CLR_SRC         0x15C4
#define R_CLR_CMP_MASK            0x15CC
/* FCN values hardware-confirmed by RBLIT.EXE VRAM readback on RV515 X1300:
   FCN=4 draws pixels where src != key (xf86: RADEON_SRC_CMP_EQ_COLOR).
   FCN=5 draws pixels where src == key (xf86: RADEON_SRC_CMP_NEQ_COLOR).
   Despite the confusing xf86 names, EQ_COLOR=4 is the correct value for
   transparent blits (skip equal/key pixels, draw the rest). */
#define   CLR_CMP_FCN_NE          4UL           /* skip-if-equal  → draw when src != key */
#define   CLR_CMP_FCN_EQ          5UL           /* skip-if-neq    → draw when src == key */
#define   CLR_CMP_SRC_SOURCE      (1UL << 24)   /* compare source pixels */

/* AVIVO display controller registers (R500/RV515)
   Used for tear-free hardware page flipping via surface update lock.
   Reference: Linux kernel rs600.c rs600_page_flip() */
#define R_D1GRPH_PRIMARY_SURFACE_ADDRESS   0x6110
#define R_D1GRPH_SECONDARY_SURFACE_ADDRESS 0x6118
#define R_D1GRPH_PITCH                     0x6120
#define R_D1GRPH_X_START                   0x612C  /* left edge of display window */
#define R_D1GRPH_Y_START                   0x6130  /* top edge of display window */
#define R_D1GRPH_X_END                     0x6134  /* right edge (exclusive) — must equal xres */
#define R_D1GRPH_Y_END                     0x6138  /* bottom edge (exclusive) — must equal yres */
#define R_D1GRPH_UPDATE                    0x6144
#define   D1GRPH_SURFACE_UPDATE_PENDING    (1UL << 2)
#define   D1GRPH_SURFACE_UPDATE_LOCK       (1UL << 16)
#define R_D1GRPH_FLIP_CONTROL              0x6148
#define   D1GRPH_SURFACE_UPDATE_H_RETRACE_EN (1UL << 0)

/* VGA control — when D1VGA_MODE_ENABLE is set, the VGA block generates
   scanout and D1GRPH surface registers are ignored for display. */
#define R_D1VGA_CONTROL                    0x0330
#define   D1VGA_MODE_ENABLE                (1UL << 0)

/* AVIVO CRTC status — for AVIVO-based vblank detection */
#define R_D1CRTC_CONTROL                   0x6080
#define   AVIVO_CRTC_EN                    (1UL << 0)
#define R_D1CRTC_STATUS                    0x609C
#define   D1CRTC_V_BLANK                   (1UL << 0)

/* ---------------------------------------------------------------
 *  Additional registers from Linux kernel radeon driver (rv515d.h,
 *  radeon_reg.h, r300_reg.h) needed for proper RV515 initialization.
 *  Reference: torvalds/linux  drivers/gpu/drm/radeon/
 * --------------------------------------------------------------- */

/* PLL register access (via MMIO, not PCI config) */
#define R_CLOCK_CNTL_INDEX         0x0008
#define R_CLOCK_CNTL_DATA          0x000C
#define   PLL_WR_EN                (1UL << 7)

/* VGA render control — disable VGA to avoid 2D engine conflicts */
#define R_VGA_RENDER_CONTROL       0x0300
#define   VGA_VSTATUS_CNTL_MASK    0x00070000UL

/* HOST_PATH_CNTL bits for HDP reset */
#define   HDP_SOFT_RESET           (1UL << 26)
#define   HDP_APER_CNTL            (1UL << 23)

/* 2D/3D synchronization (rv515d.h) */
#define R_ISYNC_CNTL               0x1724
#define   ISYNC_ANY2D_IDLE3D       (1UL << 0)
#define   ISYNC_ANY3D_IDLE2D       (1UL << 1)
#define   ISYNC_WAIT_IDLEGUI       (1UL << 4)
#define   ISYNC_CPSCRATCH_IDLEGUI  (1UL << 5)

/* Graphics block registers */
#define R_GB_ENABLE                0x4008
#define R_GB_MSPOS0                0x4010
#define R_GB_MSPOS1                0x4014
#define R_GB_SELECT                0x401C
#define R_GB_AA_CONFIG             0x4020

/* R500 shader unit register destination (pipe mask) */
#define R_SU_REG_DEST              0x42C8

/* Vertex array processing */
#define R_VAP_INDEX_OFFSET         0x208C

/* Geometry arbiter — deadlock / fastsync prevention */
#define R_GA_ENHANCE               0x4274
#define   GA_DEADLOCK_CNTL         (1UL << 0)
#define   GA_FASTSYNC_CNTL         (1UL << 1)

/* RB3D dst cache (used during ring start for 3D cache flush) */
#define R_RB3D_DSTCACHE_CTLSTAT_RS 0x4E4C    /* ring-start variant */
#define   RB3D_DC_FLUSH_RS         (2UL << 0)
#define   RB3D_DC_FREE_RS          (2UL << 2)

/* ZB z-cache control/status */
#define R_ZB_ZCACHE_CTLSTAT        0x4F18
#define   ZC_FLUSH                 (1UL << 0)
#define   ZC_FREE                  (1UL << 1)

/* RV515 MC indirect register: MC_STATUS */
#define RV515_MC_STATUS            0x0000
#define   MC_STATUS_IDLE           (1UL << 4)

/* RV515 MC indirect register: MC_CNTL */
#define RV515_MC_CNTL              0x0005

/* R500 PLL register for pipe memory power (indirect PLL register) */
#define R500_DYN_SCLK_PWMEM_PIPE  0x000D

/* =============================================================== */
/*  RV515 / RV516 device-ID table (Radeon X1300 family)            */
/* =============================================================== */

#define ATI_VID  0x1002

typedef struct { unsigned short did; const char *name; } DevEntry;

static const DevEntry g_devtab[] = {
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
/*  Global state                                                    */
/* =============================================================== */

static unsigned short g_dseg = 0, g_dsel = 0;     /* DOS buffer    */
static volatile unsigned long *g_mmio = NULL;      /* MMIO base     */
static unsigned long  g_mmio_phys = 0;
static unsigned long  g_vram_mb   = 0;
static unsigned long  g_vesa_mem_kb = 0;   /* VESA-reported total memory (KB) */
static unsigned char *g_lfb       = NULL;          /* LFB pointer   */
static unsigned long  g_lfb_phys  = 0;
static unsigned long  g_fb_location = 0;   /* GPU internal FB base address */
static int g_xres, g_yres, g_pitch;
static int g_page_stride;  /* rows per page, chosen so stride*pitch is 4KB-aligned */
static unsigned long g_default_po;   /* cached default PITCH_OFFSET (set in gpu_init_2d) */
static unsigned short g_vmode;
static unsigned char *g_font = NULL;
static int g_fifo_free = 0;  /* tracked free FIFO entries (avoids MMIO reads) */
static int g_avivo_flip = 0; /* 1=AVIVO D1GRPH flip, 0=VBE+port 0x3DA */

static int  g_pci_bus, g_pci_dev, g_pci_func;
static unsigned short g_pci_did;
static const char    *g_card_name = "Unknown";

/* =============================================================== */
/*  DPMI helpers  (same pattern as VESADEMO)                        */
/* =============================================================== */

static int dpmi_alloc(unsigned short para)
{
    union REGS r;
    memset(&r, 0, sizeof r);
    r.x.eax = 0x0100;  r.x.ebx = para;
    int386(0x31, &r, &r);
    if (r.x.cflag) return 0;
    g_dseg = (unsigned short)r.x.eax;
    g_dsel = (unsigned short)r.x.edx;
    return 1;
}
static void dpmi_free(void)
{
    union REGS r;
    if (!g_dsel) return;
    memset(&r, 0, sizeof r);
    r.x.eax = 0x0101;  r.x.edx = g_dsel;
    int386(0x31, &r, &r);
    g_dsel = g_dseg = 0;
}
static int dpmi_rmint(unsigned char n, RMI *p)
{
    union REGS r;  struct SREGS sr;
    segread(&sr);
    memset(&r, 0, sizeof r);
    r.x.eax = 0x0300;  r.x.ebx = n;  r.x.ecx = 0;
    r.x.edi = (unsigned int)p;
    int386x(0x31, &r, &r, &sr);
    return !(r.x.cflag);
}
static void *dpmi_map(unsigned long phys, unsigned long sz)
{
    union REGS r;
    memset(&r, 0, sizeof r);
    r.x.eax = 0x0800;
    r.x.ebx = (phys >> 16) & 0xFFFF;  r.x.ecx = phys & 0xFFFF;
    r.x.esi = (sz   >> 16) & 0xFFFF;  r.x.edi = sz   & 0xFFFF;
    int386(0x31, &r, &r);
    if (r.x.cflag) return NULL;
    return (void *)(((r.x.ebx & 0xFFFF) << 16) | (r.x.ecx & 0xFFFF));
}
static void dpmi_unmap(void *p)
{
    union REGS r;
    unsigned long a = (unsigned long)p;
    memset(&r, 0, sizeof r);
    r.x.eax = 0x0801;
    r.x.ebx = (a >> 16) & 0xFFFF;  r.x.ecx = a & 0xFFFF;
    int386(0x31, &r, &r);
}

/* =============================================================== */
/*  PCI configuration via Mechanism 1 (port I/O)                    */
/* =============================================================== */

static unsigned long pci_rd32(int b, int d, int f, int reg)
{
    outpd(0xCF8, 0x80000000UL | ((unsigned long)b<<16) |
          ((unsigned long)d<<11) | ((unsigned long)f<<8) | (reg & 0xFC));
    return inpd(0xCFC);
}
static void pci_wr32(int b, int d, int f, int reg, unsigned long v)
{
    outpd(0xCF8, 0x80000000UL | ((unsigned long)b<<16) |
          ((unsigned long)d<<11) | ((unsigned long)f<<8) | (reg & 0xFC));
    outpd(0xCFC, v);
}
static unsigned short pci_rd16(int b, int d, int f, int reg)
{
    unsigned long v = pci_rd32(b, d, f, reg & ~3);
    return (unsigned short)(v >> ((reg & 2) * 8));
}

/* Scan PCI for an ATI RV515/RV516 device */
static int pci_find_radeon(void)
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
                            g_pci_bus  = b;
                            g_pci_dev  = d;
                            g_pci_func = fn;
                            g_pci_did  = did;
                            g_card_name = g_devtab[i].name;
                            return 1;
                        }
                    }
                }
                if (fn == 0) {
                    hdr = (unsigned char)(pci_rd32(b,d,0,0x0C) >> 16);
                    if (!(hdr & 0x80)) break;   /* single-function */
                }
                continue;
next_dev:       if (fn == 0) break;
            }
        }
    }
    return 0;
}

/* =============================================================== */
/*  VBE helpers                                                     */
/* =============================================================== */

static unsigned char *dosbuf(void)
{ return (unsigned char *)((unsigned long)g_dseg << 4); }

static int vbe_get_info(VBEInfo *out)
{
    RMI rm;
    memset(&rm, 0, sizeof rm);
    memset(dosbuf(), 0, 512);
    memcpy(dosbuf(), "VBE2", 4);
    rm.eax = 0x4F00;  rm.es = g_dseg;  rm.edi = 0;
    if (!dpmi_rmint(0x10, &rm)) return 0;
    if ((rm.eax & 0xFFFF) != 0x004F) return 0;
    memcpy(out, dosbuf(), sizeof(VBEInfo));
    return 1;
}
static int vbe_get_mode(unsigned short m, VBEMode *out)
{
    RMI rm;
    memset(&rm, 0, sizeof rm);
    memset(dosbuf(), 0, 256);
    rm.eax = 0x4F01;  rm.ecx = m;  rm.es = g_dseg;  rm.edi = 0;
    if (!dpmi_rmint(0x10, &rm)) return 0;
    if ((rm.eax & 0xFFFF) != 0x004F) return 0;
    memcpy(out, dosbuf(), sizeof(VBEMode));
    return 1;
}
static int vbe_set(unsigned short m)
{
    RMI rm;
    memset(&rm, 0, sizeof rm);
    rm.eax = 0x4F02;  rm.ebx = (unsigned long)m | 0x4000;
    if (!dpmi_rmint(0x10, &rm)) return 0;
    return ((rm.eax & 0xFFFF) == 0x004F);
}
static void vbe_text(void)
{
    RMI rm;
    memset(&rm, 0, sizeof rm);
    rm.eax = 0x0003;
    dpmi_rmint(0x10, &rm);
}

/* =============================================================== */
/*  BIOS 8x8 font                                                  */
/* =============================================================== */

static unsigned char *get_font(void)
{
    RMI rm;
    memset(&rm, 0, sizeof rm);
    rm.eax = 0x1130;  rm.ebx = 0x0300;
    dpmi_rmint(0x10, &rm);
    return (unsigned char *)(((unsigned long)rm.es << 4) + (rm.ebp & 0xFFFF));
}

/* =============================================================== */
/*  CPU drawing primitives (for text overlay on LFB)                */
/* =============================================================== */

static void cpu_char(int x, int y, char ch, unsigned char c, int sc)
{
    unsigned char *g = g_font + (unsigned char)ch * 8;
    int r, cl, sy, sx;
    for (r = 0; r < 8; r++) {
        unsigned char b = g[r];
        for (cl = 0; cl < 8; cl++) {
            if (b & (0x80 >> cl)) {
                for (sy = 0; sy < sc; sy++)
                    for (sx = 0; sx < sc; sx++)
                        g_lfb[(y+r*sc+sy)*g_pitch + x+cl*sc+sx] = c;
            }
        }
    }
}
static void cpu_str(int x, int y, const char *s, unsigned char c, int sc)
{
    for (; *s; s++, x += 8*sc)
        cpu_char(x, y, *s, c, sc);
}
static void cpu_str_c(int y, const char *s, unsigned char c, int sc)
{
    int x = (g_xres - (int)strlen(s) * 8 * sc) / 2;
    if (x < 0) x = 0;
    cpu_str(x, y, s, c, sc);
}

/* =============================================================== */
/*  VGA palette via VBE 4F09h                                       */
/* =============================================================== */

static int g_dac_bits = 6;

static void init_dac(void)
{
    RMI rm;
    int got;

    memset(&rm, 0, sizeof rm);
    rm.eax = 0x4F08;  rm.ebx = 0x0800;
    dpmi_rmint(0x10, &rm);

    memset(&rm, 0, sizeof rm);
    rm.eax = 0x4F08;  rm.ebx = 0x0100;
    if (dpmi_rmint(0x10, &rm) && (rm.eax & 0xFFFF) == 0x004F)
        got = (int)((rm.ebx >> 8) & 0xFF);
    else
        got = 6;
    if (got < 6) got = 6;
    if (got > 8) got = 8;
    g_dac_bits = got;
}

/*
 * Set palette entries via VBE 4F09h.
 * `p` has `count` entries of 4 bytes each: R, G, B, pad (8-bit values).
 * Scaled to DAC width and reordered to BIOS format (B, G, R, pad).
 */
static void vbe_set_palette(int start, int count, const unsigned char *p)
{
    RMI rm;
    unsigned char *buf = dosbuf();
    int i, shift;

    shift = 8 - g_dac_bits;
    for (i = 0; i < count; i++) {
        buf[i*4+0] = p[i*4+2] >> shift;   /* B */
        buf[i*4+1] = p[i*4+1] >> shift;   /* G */
        buf[i*4+2] = p[i*4+0] >> shift;   /* R */
        buf[i*4+3] = 0;
    }

    memset(&rm, 0, sizeof rm);
    rm.eax = 0x4F09;
    rm.ebx = 0x0000;
    rm.ecx = (unsigned long)count;
    rm.edx = (unsigned long)start;
    rm.es  = g_dseg;
    rm.edi = 0;
    dpmi_rmint(0x10, &rm);
}

static void setup_palette(void)
{
    unsigned char p[256 * 4];
    int i;

    init_dac();
    memset(p, 0, sizeof p);

    for (i = 0; i < 32; i++) {
        unsigned char v = (unsigned char)(i * 255 / 31);
        p[( 1+i)*4+0]=0; p[( 1+i)*4+1]=0; p[( 1+i)*4+2]=v;
        p[(33+i)*4+0]=0; p[(33+i)*4+1]=v; p[(33+i)*4+2]=0;
        p[(65+i)*4+0]=v; p[(65+i)*4+1]=0; p[(65+i)*4+2]=0;
        p[(97+i)*4+0]=0; p[(97+i)*4+1]=v; p[(97+i)*4+2]=v;
        p[(129+i)*4+0]=v; p[(129+i)*4+1]=v; p[(129+i)*4+2]=0;
        p[(161+i)*4+0]=v; p[(161+i)*4+1]=0; p[(161+i)*4+2]=v;
        p[(193+i)*4+0]=v; p[(193+i)*4+1]=v; p[(193+i)*4+2]=v;
    }
    p[248*4+0]= 40; p[248*4+1]= 40; p[248*4+2]= 40;
    p[249*4+0]= 80; p[249*4+1]= 80; p[249*4+2]= 80;
    p[250*4+0]=  0; p[250*4+1]=255; p[250*4+2]=  0;
    p[251*4+0]=255; p[251*4+1]=  0; p[251*4+2]=  0;
    p[252*4+0]= 64; p[252*4+1]= 64; p[252*4+2]= 64;
    p[253*4+0]=  0; p[253*4+1]=255; p[253*4+2]=255;
    p[254*4+0]=255; p[254*4+1]=255; p[254*4+2]=  0;
    p[255*4+0]=255; p[255*4+1]=255; p[255*4+2]=255;

    vbe_set_palette(0, 256, p);
}

/* =============================================================== */
/*  Radeon MMIO register access                                     */
/* =============================================================== */

static unsigned long rreg(unsigned long off)
{ return g_mmio[off >> 2]; }

static void wreg(unsigned long off, unsigned long val)
{ g_mmio[off >> 2] = val; }

/* Read RV515 indirect MC register (via MC_IND_INDEX/MC_IND_DATA) */
static unsigned long mc_rreg(unsigned long reg)
{
    unsigned long r;
    wreg(R_MC_IND_INDEX, 0x7F0000UL | (reg & 0xFFFF));
    r = rreg(R_MC_IND_DATA);
    wreg(R_MC_IND_INDEX, 0);
    return r;
}

/* Read/write PLL registers via CLOCK_CNTL_INDEX/DATA
   (from Linux kernel radeon driver: radeon_reg.h, r100.c) */
static unsigned long pll_rreg(unsigned long reg)
{
    unsigned long r;
    wreg(R_CLOCK_CNTL_INDEX, reg & 0x3FUL);
    r = rreg(R_CLOCK_CNTL_DATA);
    return r;
}
static void pll_wreg(unsigned long reg, unsigned long val)
{
    wreg(R_CLOCK_CNTL_INDEX, (reg & 0x3FUL) | PLL_WR_EN);
    wreg(R_CLOCK_CNTL_DATA, val);
}

/* Disable VGA rendering — prevents VGA block from interfering with
   the 2D engine.  (from Linux: rv515_vga_render_disable) */
static void gpu_vga_render_disable(void)
{
    wreg(R_VGA_RENDER_CONTROL,
         rreg(R_VGA_RENDER_CONTROL) & ~VGA_VSTATUS_CNTL_MASK);
}

/* Wait for memory controller idle (from Linux: rv515_mc_wait_for_idle) */
static int gpu_mc_wait_idle(void)
{
    unsigned long timeout = 2000000UL;
    while (timeout--) {
        if (mc_rreg(RV515_MC_STATUS) & MC_STATUS_IDLE)
            return 1;
    }
    return 0;
}

/* Wait for at least `n` free FIFO entries.
   Uses a local counter to avoid MMIO reads when enough space is known. */
static void gpu_wait_fifo(int n)
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

/* Flush R300+/R500 2D destination cache and wait until done */
static void gpu_engine_flush(void)
{
    unsigned long timeout = 2000000UL;
    wreg(R_RB2D_DSTCACHE_CTLSTAT, RB2D_DC_FLUSH_ALL);
    while (timeout--) {
        if (!(rreg(R_RB2D_DSTCACHE_CTLSTAT) & RB2D_DC_BUSY))
            return;
    }
}

/* Wait for the 2D/3D engine to go idle */
static void gpu_wait_idle(void)
{
    unsigned long timeout = 2000000UL;
    gpu_wait_fifo(64);
    while (timeout--) {
        if (!(rreg(R_RBBM_STATUS) & RBBM_ACTIVE))
            break;
    }
    gpu_engine_flush();
    g_fifo_free = 64;   /* engine idle → FIFO fully drained */
}

/* =============================================================== */
/*  Radeon engine reset (from xf86-video-ati RADEONEngineReset)     */
/*  Improved with full R300+/R500 sequence from Linux sources.      */
/* =============================================================== */

static void gpu_engine_reset(void)
{
    unsigned long rbbm_soft_reset;
    unsigned long host_path_cntl;
    unsigned long clock_cntl_index;

    /* Save CLOCK_CNTL_INDEX to avoid disturbing PLL access state
       (from xf86-video-ati RADEONEngineReset) */
    clock_cntl_index = rreg(R_CLOCK_CNTL_INDEX);

    /*
     * R300+/R500 soft reset sequence (from xf86-video-ati):
     *   For R300+/AVIVO: reset only CP, HI, E2 — leave SE/RE/PP/RB
     *   alone to avoid lockup.  Then write 0 to clear all reset bits.
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
     * Reset HDP via HOST_PATH_CNTL toggle.
     * RBBM_SOFT_RESET of HDP can cause problems on some ASICs — the
     * Linux and xf86 drivers toggle HDP_SOFT_RESET through HOST_PATH_CNTL
     * instead.  (from Linux kernel r100.c, xf86-video-ati)
     */
    host_path_cntl = rreg(R_HOST_PATH_CNTL);
    wreg(R_HOST_PATH_CNTL, host_path_cntl | HDP_SOFT_RESET | HDP_APER_CNTL);
    (void)rreg(R_HOST_PATH_CNTL);
    wreg(R_HOST_PATH_CNTL, host_path_cntl | HDP_APER_CNTL);
    (void)rreg(R_HOST_PATH_CNTL);

    /* Enable destination cache autoflush (R300+/R500 2D cache) */
    wreg(R_RB2D_DSTCACHE_MODE, rreg(R_RB2D_DSTCACHE_MODE) |
         (1UL << 2) | (1UL << 15));  /* DC_AUTOFLUSH | DC_DISABLE_IGNORE_PE */

    /* Restore CLOCK_CNTL_INDEX */
    wreg(R_CLOCK_CNTL_INDEX, clock_cntl_index);
}

/* =============================================================== */
/*  Radeon R500 2D engine initialization                            */
/*  Full sequence derived from Linux kernel rv515.c, r100.c and     */
/*  xf86-video-ati radeon_accel.c RADEONEngineInit/EngineRestore.   */
/* =============================================================== */

static int g_num_gb_pipes = 1;

static void gpu_init_2d(void)
{
    unsigned long pitch64;
    unsigned long pitch_offset;
    unsigned long gb_pipe_sel;
    unsigned long gb_tile_config;
    unsigned long pipe_sel_current;
    unsigned long dst_pipe_val;
    unsigned long pll_tmp;

    /* ---- Phase 1: GPU init (from Linux rv515_gpu_init) ---- */

    /* Wait for GUI idle before any reset */
    gpu_wait_idle();

    /* Disable VGA rendering to prevent interference with 2D engine
       (from Linux rv515_vga_render_disable) */
    gpu_vga_render_disable();

    /* Detect number of GB pipes
       (from xf86-video-ati RADEONEngineInit IS_R500_3D path +
        Linux rv515_gpu_init → r420_pipes_init) */
    gb_pipe_sel = rreg(R_GB_PIPE_SELECT);
    g_num_gb_pipes = (int)((gb_pipe_sel >> 12) & 0x3) + 1;

    /* R500 pipe memory power configuration
       (from xf86-video-ati RADEONEngineInit IS_R500_3D path) */
    pll_tmp = (1UL | (((gb_pipe_sel >> 8) & 0xFUL) << 4));
    pll_wreg(R500_DYN_SCLK_PWMEM_PIPE, pll_tmp);

    /* Write PLL reg 0x000D with pipe config
       (from Linux rv515_gpu_init after r420_pipes_init) */
    dst_pipe_val = rreg(R_DST_PIPE_CONFIG);
    pipe_sel_current = (dst_pipe_val >> 2) & 3;
    pll_wreg(R500_DYN_SCLK_PWMEM_PIPE,
             (1UL << pipe_sel_current) |
             (((gb_pipe_sel >> 8) & 0xFUL) << 4));

    /* Wait for GUI and MC idle after pipe init
       (from Linux rv515_gpu_init) */
    gpu_wait_idle();
    gpu_mc_wait_idle();

    /* ---- Phase 2: Configure R300+/R500 tile and cache
       (from xf86-video-ati RADEONEngineInit IS_R300/IS_R500 path,
        done BEFORE engine reset per xf86 order) ---- */

    /* GB_TILE_CONFIG based on detected pipe count
       (from xf86-video-ati RADEONEngineInit) */
    gb_tile_config = GB_TILE_ENABLE | GB_TILE_SIZE_16;
    switch (g_num_gb_pipes) {
    case 2:  gb_tile_config |= (1UL << 1); break;  /* R300_PIPE_COUNT_R300 */
    case 3:  gb_tile_config |= (2UL << 1); break;  /* R300_PIPE_COUNT_R420_3P */
    case 4:  gb_tile_config |= (3UL << 1); break;  /* R300_PIPE_COUNT_R420 */
    default: gb_tile_config |= GB_PIPE_COUNT_RV350; break;  /* 1 pipe */
    }
    wreg(R_GB_TILE_CONFIG, gb_tile_config);

    wreg(R_WAIT_UNTIL, WAIT_2D_IDLECLEAN | WAIT_3D_IDLECLEAN);

    /* R420+/R500: auto-configure destination pipe */
    wreg(R_DST_PIPE_CONFIG, rreg(R_DST_PIPE_CONFIG) | PIPE_AUTO_CONFIG);

    /* Enable RB2D cache autoflush BEFORE engine reset
       (from xf86-video-ati RADEONEngineInit — order matters) */
    wreg(R_RB2D_DSTCACHE_MODE, rreg(R_RB2D_DSTCACHE_MODE) |
         (1UL << 2) | (1UL << 15));

    /* ---- Phase 3: Engine reset ---- */
    gpu_engine_reset();

    /* ---- Phase 4: Ring-start-equivalent initialization
       (from Linux rv515_ring_start — these are normally submitted via
        the CP ring, but we write them directly via MMIO for DOS) ---- */

    gpu_wait_fifo(12);

    /* 2D/3D synchronization control (rv515_ring_start) */
    wreg(R_ISYNC_CNTL,
         ISYNC_ANY2D_IDLE3D | ISYNC_ANY3D_IDLE2D |
         ISYNC_WAIT_IDLEGUI | ISYNC_CPSCRATCH_IDLEGUI);

    wreg(R_WAIT_UNTIL, WAIT_2D_IDLECLEAN | WAIT_3D_IDLECLEAN);
    wreg(R_DST_PIPE_CONFIG, rreg(R_DST_PIPE_CONFIG) | PIPE_AUTO_CONFIG);

    wreg(R_GB_SELECT, 0);
    wreg(R_GB_ENABLE, 0);

    /* R500 shader-unit register destination = pipe mask */
    wreg(R_SU_REG_DEST, (1UL << g_num_gb_pipes) - 1);
    wreg(R_VAP_INDEX_OFFSET, 0);

    /* Flush RB3D and ZB caches (rv515_ring_start) */
    wreg(R_RB3D_DSTCACHE_CTLSTAT_RS, RB3D_DC_FLUSH_RS | RB3D_DC_FREE_RS);
    wreg(R_ZB_ZCACHE_CTLSTAT, ZC_FLUSH | ZC_FREE);
    wreg(R_WAIT_UNTIL, WAIT_2D_IDLECLEAN | WAIT_3D_IDLECLEAN);

    /* Anti-aliasing off */
    wreg(R_GB_AA_CONFIG, 0);

    /* Flush again */
    wreg(R_RB3D_DSTCACHE_CTLSTAT_RS, RB3D_DC_FLUSH_RS | RB3D_DC_FREE_RS);
    wreg(R_ZB_ZCACHE_CTLSTAT, ZC_FLUSH | ZC_FREE);

    gpu_wait_fifo(4);

    /* Geometry arbiter: deadlock and fastsync prevention
       (from rv515_ring_start) */
    wreg(R_GA_ENHANCE, GA_DEADLOCK_CNTL | GA_FASTSYNC_CNTL);

    /* Set RBBM GUI control — no byte swap (x86 is little-endian) */
    wreg(R_RBBM_GUICNTL, 0);

    /* Wait for full idle */
    gpu_wait_idle();

    /* Flush 2D destination cache */
    gpu_engine_flush();

    /* ---- Phase 5: 2D engine setup
       (from xf86-video-ati RADEONEngineRestore) ---- */

    /* Encode pitch/offset for the combined register.
       Pitch field is in 64-byte units at bits [29:22],
       offset in 1024-byte units at bits [21:0].
       Offset includes GPU internal FB base address from HDP_FB_LOCATION. */
    pitch64 = ((unsigned long)g_pitch + 63) / 64;
    pitch_offset = (pitch64 << 22) | ((g_fb_location >> 10) & 0x003FFFFFUL);
    g_default_po = pitch_offset;

    gpu_wait_fifo(18);

    /* Combined pitch/offset registers (used when GMC_DST_PITCH_OFFSET_CNTL
       is set in DP_GUI_MASTER_CNTL, per xf86-video-ati) */
    wreg(R_DEFAULT_PITCH_OFFSET, pitch_offset);
    wreg(R_DST_PITCH_OFFSET, pitch_offset);
    wreg(R_SRC_PITCH_OFFSET, pitch_offset);

    /* Also write the separate pitch/offset registers — the GPU may use
       these when PITCH_OFFSET_CNTL bits are not set in DP_GUI_MASTER_CNTL.
       On RV515, the combined register is FIFO-queued and may not read back;
       setting the separate registers ensures a known-good state. */
    wreg(R_DST_OFFSET, g_fb_location);
    wreg(R_DST_PITCH,  (unsigned long)g_pitch);
    wreg(R_SRC_OFFSET, g_fb_location);
    wreg(R_SRC_PITCH,  (unsigned long)g_pitch);

    wreg(R_DEFAULT_SC_BOTTOM_RIGHT, (0x1FFF << 16) | 0x1FFF);
    wreg(R_SC_TOP_LEFT, 0);
    wreg(R_SC_BOTTOM_RIGHT,
         ((unsigned long)g_yres << 16) | (unsigned long)g_xres);

    wreg(R_DP_WRITE_MASK, 0xFFFFFFFF);
    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);

    /* Initialize brush and source colors to known state
       (from xf86-video-ati RADEONEngineRestore — avoids
        undefined register state on real hardware) */
    wreg(R_DP_BRUSH_FRGD_CLR, 0xFFFFFFFF);
    wreg(R_DP_BRUSH_BKGD_CLR, 0x00000000);
    wreg(R_DP_SRC_FRGD_CLR, 0xFFFFFFFF);
    wreg(R_DP_SRC_BKGD_CLR, 0x00000000);

    /* Set surface byte-order control (no swap for x86) */
    wreg(R_SURFACE_CNTL, 0);

    /* Disable color compare (clean state for first operation) */
    wreg(R_CLR_CMP_CNTL, 0);

    gpu_wait_idle();
}

/* =============================================================== */
/*  Page flipping: AVIVO hardware flip with VGA fallback            */
/*                                                                  */
/*  AVIVO path: D1GRPH_UPDATE_LOCK → write surface → wait pending   */
/*              → unlock.  Matches Linux kernel rs600_page_flip().   */
/*                                                                  */
/*  VGA path:   port 0x3DA vsync wait + VBE 4F07h display start.    */
/*  Used when D1VGA_MODE_ENABLE is set (VBE modes use VGA scanout). */
/* =============================================================== */

/* Detect whether AVIVO D1GRPH controls the display.
   Reads D1GRPH_PRIMARY_SURFACE_ADDRESS after VBE mode set:
   if it contains our FB base (VRAM range), AVIVO may be usable.
   Also checks D1VGA_CONTROL — when VGA mode is active, D1GRPH
   registers are ignored for scanout and AVIVO page flip has no effect.
   Matches the verification approach used by PLASMA.C -hwflip. */
static void detect_flip_mode(void)
{
    unsigned long cur_surf = rreg(R_D1GRPH_PRIMARY_SURFACE_ADDRESS);
    unsigned long d1vga   = rreg(R_D1VGA_CONTROL);
    unsigned long d1crtc  = rreg(R_D1CRTC_CONTROL);

    g_avivo_flip = 0;  /* default: VGA path (safe fallback) */

    /* Primary check: does D1GRPH surface addr match our VRAM aperture?
       After ATOMBIOS VBE mode set, it should contain g_fb_location. */
    if ((cur_surf & 0xFFF00000UL) != (g_fb_location & 0xFFF00000UL))
        return;  /* D1GRPH not configured for our FB — VGA path */

    /* D1GRPH matches our FB.  Check if VGA mode is generating scanout.
       When D1VGA_MODE_ENABLE is set, the VGA block drives the display
       and D1GRPH surface address writes are ignored by the CRTC.
       ATOMBIOS typically leaves VGA mode enabled after VBE mode set. */
    if (!(d1vga & D1VGA_MODE_ENABLE)) {
        /* VGA already disabled — AVIVO D1GRPH is active */
        g_avivo_flip = 1;
        return;
    }

    /* VGA mode is active.  ATOMBIOS configured D1GRPH registers during
       mode set, so we can transition to AVIVO scanout by disabling VGA.
       This enables hardware page flipping for tear-free animation.
       (Mirrors Linux radeon driver behaviour after mode set.) */
    wreg(R_D1VGA_CONTROL, d1vga & ~D1VGA_MODE_ENABLE);

    /* Set D1GRPH stride and display window.
       ATOMBIOS may have set X_END = pitch (832) instead of xres (800),
       which causes the controller to scan 832 pixels per line and show
       the 32-byte padding area as a dark vertical stripe on the right.
       X_START/Y_START = 0, X_END = xres, Y_END = yres. */
    wreg(R_D1GRPH_PITCH,   (unsigned long)g_pitch);
    wreg(R_D1GRPH_X_START, 0);
    wreg(R_D1GRPH_Y_START, 0);
    wreg(R_D1GRPH_X_END,   (unsigned long)g_xres);
    wreg(R_D1GRPH_Y_END,   (unsigned long)g_yres);

    /* Brief spin to let the display controller pick up new state */
    { volatile int d; for (d = 0; d < 50000; d++) {} }

    /* Verify VGA was actually disabled */
    if (!(rreg(R_D1VGA_CONTROL) & D1VGA_MODE_ENABLE)) {
        g_avivo_flip = 1;
    }
    /* If still VGA, g_avivo_flip stays 0 → VGA vsync fallback */
}

/* Wait for the AVIVO CRTC to enter vblank.
   Used as a fallback when SURFACE_UPDATE_PENDING does not signal. */
static void avivo_wait_vblank(void)
{
    int i;
    /* If already in vblank, wait for it to end first */
    for (i = 0; i < 500000; i++)
        if (!(rreg(R_D1CRTC_STATUS) & D1CRTC_V_BLANK)) break;
    /* Wait for next vblank to start */
    for (i = 0; i < 500000; i++)
        if (rreg(R_D1CRTC_STATUS) & D1CRTC_V_BLANK) break;
}

/* AVIVO hardware page flip.  Sequence per Linux kernel rs600_page_flip():
     lock → write address → unlock (arms flip) → wait for vsync.
   Blocks until the new surface is live so the caller can safely write
   to the old (now-hidden) surface.

   Primary sync: SURFACE_UPDATE_PENDING high→low (pending armed then applied).
   Fallback:     AVIVO D1CRTC_V_BLANK via D1CRTC_STATUS when pending does not
                 signal (observed on RV515 X1300 DevID 7142). */
static void hw_page_flip(unsigned long surface_addr)
{
    unsigned long tmp;
    int i;

    /* Lock: prevents the controller latching a partial address update */
    tmp = rreg(R_D1GRPH_UPDATE);
    wreg(R_D1GRPH_UPDATE, tmp | D1GRPH_SURFACE_UPDATE_LOCK);

    /* Flip at vsync, not hsync */
    wreg(R_D1GRPH_FLIP_CONTROL, 0);

    /* Write new surface address (primary and secondary must match) */
    wreg(R_D1GRPH_SECONDARY_SURFACE_ADDRESS, surface_addr);
    wreg(R_D1GRPH_PRIMARY_SURFACE_ADDRESS, surface_addr);

    /* Unlock: arms the flip — hardware applies it at next vsync */
    tmp = rreg(R_D1GRPH_UPDATE);
    wreg(R_D1GRPH_UPDATE, tmp & ~D1GRPH_SURFACE_UPDATE_LOCK);

    /* Wait for SURFACE_UPDATE_PENDING to go high (flip armed) */
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
        /* Pending bit did not signal — use CRTC vblank as fallback.
           This is the observed behaviour on RV515 X1300 (DevID 7142). */
        avivo_wait_vblank();
    }
}

/* VGA retrace wait via Input Status Register 1 (port 0x3DA). */
static void wait_vsync(void)
{
    while (inp(0x3DA) & 0x08);
    while (!(inp(0x3DA) & 0x08));
}

/* Unified page flip: auto-selects AVIVO or VGA path.
   back_page = page index (0 or 1). */
static void flip_page(int back_page)
{
    if (g_avivo_flip) {
        /* hw_page_flip blocks until the flip is applied at vsync */
        hw_page_flip(g_fb_location +
            (unsigned long)back_page * g_pitch * g_page_stride);
    } else {
        /* VGA mode: vsync wait + VBE display start */
        RMI rm;
        wait_vsync();
        memset(&rm, 0, sizeof rm);
        rm.eax = 0x4F07;
        rm.ebx = 0x0000;   /* immediate set (vsync already waited) */
        rm.ecx = 0;
        rm.edx = (unsigned long)(back_page * g_page_stride);
        dpmi_rmint(0x10, &rm);
    }
}

/* Restore display to page 0 using current flip mode. */
static void flip_restore_page0(void)
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
/*  Radeon 2D operations                                            */
/* =============================================================== */

/* Hardware-accelerated solid rectangle fill */
static void gpu_fill(int x, int y, int w, int h, unsigned char color)
{
    gpu_wait_fifo(6);

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
/*  VESA mode selection: find best 8bpp LFB mode                    */
/* =============================================================== */

static int find_mode(void)
{
    VBEInfo vi;
    VBEMode mi;
    unsigned short *ml, modelist[512];
    unsigned long seg, off;
    int i, cnt;
    unsigned long best_pixels = 0;

    if (!vbe_get_info(&vi)) return 0;
    if (vi.ver < 0x0200) return 0;

    g_vesa_mem_kb = (unsigned long)vi.tot_mem * 64;  /* tot_mem is in 64KB blocks */

    seg = (vi.mode_ptr >> 16) & 0xFFFF;
    off = vi.mode_ptr & 0xFFFF;
    ml  = (unsigned short *)((seg << 4) + off);
    for (cnt = 0; cnt < 511 && ml[cnt] != 0xFFFF; cnt++)
        modelist[cnt] = ml[cnt];
    modelist[cnt] = 0xFFFF;

    g_vmode = 0;
    for (i = 0; i < cnt; i++) {
        unsigned long px;
        if (!vbe_get_mode(modelist[i], &mi)) continue;
        if (mi.bpp != 8) continue;
        if (!(mi.attr & 0x01)) continue;
        if (!(mi.attr & 0x10)) continue;
        if (!(mi.attr & 0x80)) continue;
        if (mi.model != 4) continue;
        if (mi.lfb_phys == 0) continue;
        if (mi.xres > 1024 || mi.yres > 768) continue;

        px = (unsigned long)mi.xres * mi.yres;
        /* Prefer 800x600 if available, else biggest */
        if (mi.xres == 800 && mi.yres == 600) {
            g_vmode    = modelist[i];
            g_xres     = mi.xres;
            g_yres     = mi.yres;
            g_pitch    = mi.pitch;
            g_lfb_phys = mi.lfb_phys;
            break;
        }
        if (px > best_pixels) {
            best_pixels = px;
            g_vmode     = modelist[i];
            g_xres      = mi.xres;
            g_yres      = mi.yres;
            g_pitch     = mi.pitch;
            g_lfb_phys  = mi.lfb_phys;
        }
    }
    return g_vmode != 0;
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

/* CPU vs GPU fill benchmark */
static void demo_benchmark(void)
{
    clock_t t0, t1;
    double cpu_ms, gpu_ms, rect_ms, rect_ms2;
    int iters = 100;
    long rect_iters, rect_iters2;
    int i;
    unsigned char col;
    char buf[80];

    /* Warm up */
    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();

    /* --- CPU benchmark --- */
    t0 = clock();
    for (i = 0; i < iters; i++) {
        int y;
        col = (unsigned char)(i & 0xFF);
        for (y = 0; y < g_yres; y++)
            memset(g_lfb + y * g_pitch, col, g_xres);
    }
    t1 = clock();
    cpu_ms = (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC;

    /* --- GPU full-screen fill benchmark --- */
    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();

    t0 = clock();
    for (i = 0; i < iters; i++) {
        col = (unsigned char)(i & 0xFF);
        gpu_fill(0, 0, g_xres, g_yres, col);
    }
    gpu_wait_idle();
    t1 = clock();
    gpu_ms = (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC;

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
        t0 = clock();
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
        t1 = clock();
        rect_ms = (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC;

        /* 2-reg path (pos + size only, same color — peak throughput) */
        gpu_fill_setup();
        gpu_fill_set_color(0x55);
        rect_iters2 = 0;
        t0 = clock();
        for (pass = 0; pass < 200; pass++) {
            int rx, ry;
            for (ry = 0; ry < rows; ry++)
                for (rx = 0; rx < cols; rx++)
                    gpu_fill_rect(rx * rw, ry * rh, rw, rh);
            rect_iters2 += total_rects;
        }
        gpu_wait_idle();
        t1 = clock();
        rect_ms2 = (double)(t1 - t0) * 1000.0 / CLOCKS_PER_SEC;
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

        cpu_str_c(10, "=== CPU vs GPU Fill Benchmark ===", 255, 2);

        sprintf(buf, "%d x full-screen fill (%dx%d, 8bpp)", iters, g_xres, g_yres);
        cpu_str_c(40, buf, 253, 1);

        sprintf(buf, "CPU: %7.1f ms  (%5.1f MB/s)", cpu_ms, cpu_rate);
        cpu_str(40, 70, buf, 251, 2);

        sprintf(buf, "GPU: %7.1f ms  (%5.1f MB/s)", gpu_ms, gpu_rate);
        cpu_str(40, 100, buf, 250, 2);

        if (gpu_ms > 0) {
            sprintf(buf, "GPU speedup: %.1fx", speedup);
            cpu_str_c(150, buf, 254, 3);
        }

        /* Small rect throughput — 3 regs (color varies) */
        sprintf(buf, "32x32 (3-reg): %6.0f Krect/s  %5.0f Mpix/s", rps3/1000.0, rpix3);
        cpu_str_c(180, buf, 250, 1);

        /* 2-reg same-color peak throughput */
        sprintf(buf, "32x32 (2-reg): %6.0f Krect/s  %5.0f Mpix/s", rps2/1000.0, rpix2);
        cpu_str_c(200, buf, 254, 1);

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

        sprintf(buf, "Flip: %s", g_avivo_flip ? "AVIVO hwflip" : "VBE+VGA vsync");
        cpu_str_c(350, buf, 253, 1);
        cpu_str_c(g_yres - 20, "Press any key for GPU flood demo, ESC to quit", 253, 1);
    }
}

/* Animated random GPU rectangles with throughput counter.
   Uses fast fill path (3 regs per rect) + FIFO tracking for max throughput. */
static void demo_flood(void)
{
    clock_t t0, last_upd;
    long count = 0, fps_count = 0;
    long long total_pixels = 0;
    char buf[80];
    int ch, loop_count = 0;

    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();

    cpu_str_c(4, "GPU Rectangle Flood - press any key to stop", 255, 1);

    srand((unsigned int)(*(volatile unsigned long *)0x46CUL));
    t0 = clock();
    last_upd = t0;

    /* Set up 2D engine for batch fills (invariant regs written once) */
    gpu_fill_setup();

    while (1) {
        int rx = rand() % g_xres;
        int ry = rand() % g_yres + 14;
        int rw = rand() % (g_xres / 4) + 4;
        int rh = rand() % (g_yres / 4) + 4;
        unsigned char rc = (unsigned char)(rand() & 0xFF);

        if (rx + rw > g_xres) rw = g_xres - rx;
        if (ry + rh > g_yres) rh = g_yres - ry;
        if (rw < 1 || rh < 1) continue;

        gpu_fill_fast(rx, ry, rw, rh, rc);
        count++;
        fps_count++;
        total_pixels += (long long)rw * rh;

        /* Check keyboard less frequently (every 64 rects) */
        if (++loop_count >= 64) {
            loop_count = 0;
            if (kbhit()) break;
        }

        /* Update counter every ~4000 rects (reduces stall overhead) */
        if (fps_count >= 4000) {
            clock_t now = clock();
            double elapsed = (double)(now - last_upd) / CLOCKS_PER_SEC;
            double rps, mpps;
            if (elapsed <= 0) elapsed = 0.001;
            rps = fps_count / elapsed;
            mpps = (double)total_pixels / elapsed / 1000000.0;

            gpu_wait_idle();
            gpu_fill(0, 0, g_xres, 13, 0);
            gpu_wait_idle();
            sprintf(buf, "Rects: %ld  %-.0f rects/s  %-.0f Mpix/s",
                    count, rps, mpps);
            cpu_str(4, 4, buf, 254, 1);

            /* Re-setup fast fill state after gpu_fill in header clear */
            gpu_fill_setup();

            fps_count = 0;
            total_pixels = 0;
            last_upd = now;
        }
    }
    ch = getch();
    (void)ch;

    gpu_wait_idle();
    {
        double total_sec = (double)(clock() - t0) / CLOCKS_PER_SEC;
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
#define PLAX_TRANSP    0    /* palette index = transparent */

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
/*  Dune Chase: 16-layer desert parallax + 16 bouncing UFO sprites */
/* =============================================================== */

#define DUNE_NLAYERS   16
#define DUNE_NUFOS     16
#define UFO_W          32
#define UFO_H          16

/* ----- Generate desert sky gradient (layer 0, opaque) ------------ */
static void gen_desert_sky(int base)
{
    int x, y, n, i;

    /* Top = deep blue (index 1), fading to pale sandy blue at horizon */
    for (y = 0; y < g_yres; y++) {
        unsigned char c;
        if (y < g_yres / 2) {
            /* Upper half: blue gradient (1-32) */
            c = (unsigned char)(1 + y * 31 / (g_yres / 2));
            if (c > 32) c = 32;
        } else {
            /* Lower half: warm yellow/sand wash (129-145) */
            c = (unsigned char)(129 + (y - g_yres / 2) * 16 / (g_yres / 2));
            if (c > 145) c = 145;
        }
        memset(g_lfb + (long)(base + y) * g_pitch, c, g_xres);
    }

    /* Sparse bright sun spots near top */
    srand(12345);
    n = g_xres / 8;
    for (i = 0; i < n; i++) {
        x = rand() % g_xres;
        y = rand() % (g_yres / 4);
        g_lfb[(long)(base + y) * g_pitch + x] = 254;  /* bright yellow dot */
    }
}

/* ----- Generate a dune layer (layers 1-15, transparent above) ---- */
static void gen_dune_layer(int base, int layer_idx)
{
    int x, y, h, top;
    int base_h, amp1, amp2, freq1, freq2, phase1, phase2;
    unsigned char c_top, c_bot;

    /* Clear to transparent */
    for (y = 0; y < g_yres; y++)
        memset(g_lfb + (long)(base + y) * g_pitch, PLAX_TRANSP, g_xres);

    /* Farther layers (lower idx) are shorter and paler.
       Closer layers (higher idx) are taller and more saturated. */
    base_h = g_yres * (4 + layer_idx * 2) / 100;
    amp1   = g_yres * (3 + layer_idx) / 100;
    amp2   = g_yres * (1 + layer_idx / 2) / 100;
    freq1  = g_xres * (3 - layer_idx % 3) + layer_idx * 31;
    freq2  = g_xres * (2 - layer_idx % 2) + layer_idx * 53 + 100;
    if (freq1 < 80) freq1 = 80;
    if (freq2 < 60) freq2 = 60;
    phase1 = layer_idx * 137;
    phase2 = layer_idx * 263 + 50;

    /* Color range: blend from yellow (129+) toward red/brown (65+)
       as layers get closer */
    if (layer_idx < 8) {
        c_top = (unsigned char)(129 + layer_idx * 3);     /* yellow range */
        c_bot = (unsigned char)(129 + layer_idx * 3 + 12);
        if (c_top > 158) c_top = 158;
        if (c_bot > 160) c_bot = 160;
    } else {
        c_top = (unsigned char)(65 + (layer_idx - 8) * 3);   /* red/brown */
        c_bot = (unsigned char)(65 + (layer_idx - 8) * 3 + 15);
        if (c_top > 92)  c_top = 92;
        if (c_bot > 96)  c_bot = 96;
    }

    for (x = 0; x < g_xres; x++) {
        h = base_h
            + tri_wave(x + phase1, freq1, amp1)
            + tri_wave(x * 2 + phase2, freq2, amp2);
        if (h < 8) h = 8;
        if (h > g_yres * 3 / 4) h = g_yres * 3 / 4;
        top = g_yres - h;

        for (y = top; y < g_yres; y++) {
            int shade;
            shade = (int)c_top + (y - top) * ((int)c_bot - (int)c_top) / h;
            g_lfb[(long)(base + y) * g_pitch + x] = (unsigned char)shade;
        }
    }
}

/* ----- Generate UFO sprite at page 18 ----------------------------- */
static void gen_ufo_sprite(int base)
{
    int x, y, cx, cy, dx, dy;

    /* Clear sprite area to transparent */
    for (y = 0; y < UFO_H; y++)
        memset(g_lfb + (long)(base + y) * g_pitch, PLAX_TRANSP, UFO_W);

    cx = UFO_W / 2;
    cy = UFO_H / 2;

    /* Dome: upper half-ellipse (rows 2..6), bright cyan */
    for (y = 2; y <= 6; y++) {
        int hw;
        hw = 6 * (6 - (y - 2)) / 5;
        if (hw < 1) hw = 1;
        for (x = cx - hw; x <= cx + hw; x++)
            if (x >= 0 && x < UFO_W)
                g_lfb[(long)(base + y) * g_pitch + x] = 253;  /* bright cyan */
    }

    /* Body disc: rows 7..10, wide ellipse, bright white/yellow */
    for (y = 7; y <= 10; y++) {
        int hw;
        hw = 14 - (y - 7) * 2;
        if (hw < 6) hw = 6;
        for (x = cx - hw; x <= cx + hw; x++) {
            if (x >= 0 && x < UFO_W) {
                unsigned char bc;
                dx = x - cx;
                dy = y - 8;
                bc = (unsigned char)(254 - ((dx * dx + dy * dy) > 80 ? 6 : 0));
                g_lfb[(long)(base + y) * g_pitch + x] = bc;
            }
        }
    }

    /* Landing lights: 3 dots at bottom */
    for (x = cx - 6; x <= cx + 6; x += 6) {
        for (dy = 11; dy <= 12; dy++) {
            if (x >= 0 && x < UFO_W && dy < UFO_H)
                g_lfb[(long)(base + dy) * g_pitch + x] = 255;  /* white */
        }
    }
}

static void demo_dune_chase(void)
{
    int layer_base[DUNE_NLAYERS];
    unsigned long layer_off[DUNE_NLAYERS];  /* VRAM byte offset per layer */
    unsigned long layer_po[DUNE_NLAYERS];   /* pre-computed PITCH_OFFSET */
    int sprite_base;
    unsigned long sprite_off;               /* VRAM byte offset for sprite */
    unsigned long sprite_po_val;            /* PITCH_OFFSET for sprite */
    unsigned long stage_off;                /* staging area byte offset */
    unsigned long stage_po;                 /* PITCH_OFFSET for staging */
    int back_page, back_y, scroll;
    long fps_count;
    clock_t fps_t0, t_render_start, t_render_end, t_frame_end;
    double fps, cpu_pct, cpu_acc;
    long cpu_samples;
    char buf[120];
    int i;
    unsigned long need;

    /* UFO state arrays */
    int ux[DUNE_NUFOS], uy[DUNE_NUFOS];
    int udx[DUNE_NUFOS], udy[DUNE_NUFOS];

    need = (unsigned long)g_pitch * g_page_stride * 20;
    if (g_vram_mb > 0 && need > g_vram_mb * 1024UL * 1024UL) {
        gpu_fill(0, 0, g_xres, g_yres, 0);
        gpu_wait_idle();
        cpu_str_c(g_yres / 2, "Not enough VRAM for dune demo", 251, 2);
        cpu_str_c(g_yres / 2 + 30, "Press any key...", 253, 1);
        getch();
        return;
    }

    /* Compute layer offscreen row bases and VRAM byte offsets.
       Pre-compute PITCH_OFFSET values so each blit carries its own
       surface address — matching the xf86-video-ati driver pattern. */
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

    /* Widen scissor so GPU ops can reach offscreen VRAM.
       With per-surface offsets, coordinates stay within 0..g_yres,
       but we still need room for the 2-page display fill. */
    gpu_wait_fifo(1);
    wreg(R_SC_BOTTOM_RIGHT, (0x3FFFUL << 16) | (unsigned long)g_xres);

    /* Show generation message */
    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();
    cpu_str_c(g_yres / 2 - 20, "Generating 16-layer desert landscape...", 255, 2);
    cpu_str_c(g_yres / 2 + 10, "Dune Chase: 16 layers + 16 UFOs", 253, 1);

    /* Generate all 16 layers via staging through page 1.
       CPU writes each layer into page 1 (within LFB range), then
       GPU blits from page 1 to the final offscreen page.
       Both src and dst PITCH_OFFSET + GMC control bits are written
       in the SAME FIFO batch per blit (xf86 driver pattern). */

    /* Layer 0: sky (opaque) */
    gen_desert_sky(g_page_stride);
    gpu_blit_po(stage_po, 0, 0, layer_po[0], 0, 0, g_xres, g_yres);
    gpu_wait_idle();
    gpu_flush_2d_cache();

    /* Layers 1-15: dunes */
    for (i = 1; i < DUNE_NLAYERS; i++) {
        gen_dune_layer(g_page_stride, i);
        gpu_blit_po(stage_po, 0, 0, layer_po[i], 0, 0, g_xres, g_yres);
        gpu_wait_idle();
        gpu_flush_2d_cache();
    }

    /* UFO sprite */
    gen_ufo_sprite(g_page_stride);
    gpu_blit_po(stage_po, 0, 0, sprite_po_val, 0, 0, UFO_W, UFO_H);
    gpu_wait_idle();
    gpu_flush_2d_cache();

    /* Reset PITCH_OFFSET to default for gpu_fill (which lacks GMC bits) */
    gpu_wait_fifo(2);
    wreg(R_DST_PITCH_OFFSET, g_default_po);
    wreg(R_SRC_PITCH_OFFSET, g_default_po);

    /* Initialize UFO positions and velocities */
    srand(9999);
    for (i = 0; i < DUNE_NUFOS; i++) {
        ux[i]  = rand() % (g_xres - UFO_W);
        uy[i]  = 20 + rand() % (g_yres - UFO_H - 40);
        udx[i] = (rand() % 3) + 1;
        udy[i] = (rand() % 3) + 1;
        if (rand() % 2) udx[i] = -udx[i];
        if (rand() % 2) udy[i] = -udy[i];
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

        /* Composite 16 layers back-to-front into back buffer.
           Layer 0 at 1/16 speed, layer 15 at full speed.
           Each blit writes SRC_PITCH_OFFSET, DST_PITCH_OFFSET, and
           DP_GUI_MASTER_CNTL with GMC_*_PITCH_OFFSET_CNTL bits in a
           single FIFO batch — ensuring the GPU reads per-surface offsets. */
        plax_blit_wrap_po(layer_po[0], scroll / 16,
                          g_default_po, back_y, g_yres, 0);
        for (i = 1; i < DUNE_NLAYERS; i++) {
            plax_blit_wrap_po(layer_po[i], scroll * (i + 1) / 16,
                              g_default_po, back_y, g_yres, 1);
        }

        /* Draw 16 UFO sprites with color-key transparency */
        for (i = 0; i < DUNE_NUFOS; i++) {
            gpu_blit_po_key(sprite_po_val, 0, 0,
                            g_default_po, ux[i], back_y + uy[i],
                            UFO_W, UFO_H, PLAX_TRANSP);
        }

        gpu_wait_idle();

        t_render_end = clock();

        /* Update UFO positions (bounce off edges) */
        for (i = 0; i < DUNE_NUFOS; i++) {
            ux[i] += udx[i];
            uy[i] += udy[i];
            if (ux[i] < 0)               { ux[i] = 0;               udx[i] = -udx[i]; }
            if (ux[i] > g_xres - UFO_W)  { ux[i] = g_xres - UFO_W; udx[i] = -udx[i]; }
            if (uy[i] < 16)              { uy[i] = 16;              udy[i] = -udy[i]; }
            if (uy[i] > g_yres - UFO_H)  { uy[i] = g_yres - UFO_H; udy[i] = -udy[i]; }
        }

        /* FPS counter (update every ~1 second) */
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

        /* HUD text (CPU writes directly to back buffer in LFB) */
        sprintf(buf, "Dune Chase  16 layers + 16 UFOs  %.1f FPS  CPU:%d%%  [ESC quit]",
                fps, (int)(cpu_pct + 0.5));
        cpu_str(4, back_y + 4, buf, 255, 1);

        /* Page flip: atomic, tear-free */
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
        if (scroll >= g_xres * 1000) scroll = 0;
    }
    getch();

    /* Restore display to page 0 */
    flip_restore_page0();

    /* Reset PITCH_OFFSET to default and restore scissor for other demos */
    gpu_wait_fifo(3);
    wreg(R_DST_PITCH_OFFSET, g_default_po);
    wreg(R_SRC_PITCH_OFFSET, g_default_po);
    wreg(R_SC_BOTTOM_RIGHT,
         ((unsigned long)g_yres << 16) | (unsigned long)g_xres);
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
