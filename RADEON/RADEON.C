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
#define   CLR_CMP_FCN_NE          5UL           /* draw when src != key */
#define   CLR_CMP_SRC_SOURCE      (1UL << 24)   /* compare source pixels */

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
static unsigned char *g_lfb       = NULL;          /* LFB pointer   */
static unsigned long  g_lfb_phys  = 0;
static unsigned long  g_fb_location = 0;   /* GPU internal FB base address */
static int g_xres, g_yres, g_pitch;
static unsigned short g_vmode;
static unsigned char *g_font = NULL;

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

/* Wait for at least `n` free FIFO entries */
static void gpu_wait_fifo(int n)
{
    unsigned long timeout = 2000000UL;
    while (timeout--) {
        if ((int)(rreg(R_RBBM_STATUS) & RBBM_FIFOCNT_MASK) >= n)
            return;
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

    gpu_wait_fifo(14);
    wreg(R_DEFAULT_PITCH_OFFSET, pitch_offset);
    wreg(R_DST_PITCH_OFFSET, pitch_offset);
    wreg(R_SRC_PITCH_OFFSET, pitch_offset);

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
    double cpu_ms, gpu_ms;
    int iters = 100;
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

    /* --- GPU benchmark --- */
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

    /* Display results */
    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();

    {
        double total = (double)g_xres * g_yres * iters / (1024.0*1024.0);
        double cpu_rate = (cpu_ms > 0) ? total / (cpu_ms/1000.0) : 0;
        double gpu_rate = (gpu_ms > 0) ? total / (gpu_ms/1000.0) : 0;
        double speedup  = (gpu_ms > 0) ? cpu_ms / gpu_ms : 0;

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

            bar_y = 220;
            cpu_str(10, bar_y - 12, "CPU", 251, 1);
            gpu_fill(50, bar_y, cpu_bar, 24, 251 /* red */);
            bar_y += 40;
            cpu_str(10, bar_y - 12, "GPU", 250, 1);
            gpu_fill(50, bar_y, gpu_bar, 24, 250 /* green */);
            gpu_wait_idle();
        }

        cpu_str_c(g_yres - 20, "Press any key for GPU flood demo, ESC to quit", 253, 1);
    }
}

/* Animated random GPU rectangles with throughput counter */
static void demo_flood(void)
{
    clock_t t0, last_upd;
    long count = 0, fps_count = 0;
    char buf[60];
    int ch;

    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();

    cpu_str_c(4, "GPU Rectangle Flood - press any key to stop", 255, 1);

    srand((unsigned int)(*(volatile unsigned long *)0x46CUL));
    t0 = clock();
    last_upd = t0;

    while (!kbhit()) {
        int rx = rand() % g_xres;
        int ry = rand() % g_yres + 14;
        int rw = rand() % (g_xres / 4) + 4;
        int rh = rand() % (g_yres / 4) + 4;
        unsigned char rc = (unsigned char)(rand() & 0xFF);

        if (rx + rw > g_xres) rw = g_xres - rx;
        if (ry + rh > g_yres) rh = g_yres - ry;
        if (rw < 1 || rh < 1) continue;

        gpu_fill(rx, ry, rw, rh, rc);
        count++;
        fps_count++;

        /* Update counter every ~500 rects */
        if (fps_count >= 500) {
            clock_t now = clock();
            double elapsed = (double)(now - last_upd) / CLOCKS_PER_SEC;
            double rps;
            if (elapsed <= 0) elapsed = 0.001;
            rps = fps_count / elapsed;

            gpu_wait_idle();
            gpu_fill(0, 0, g_xres, 13, 0);
            gpu_wait_idle();
            sprintf(buf, "Rects: %ld  Rate: %.0f rects/sec", count, rps);
            cpu_str(4, 4, buf, 254, 1);

            fps_count = 0;
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

/* GPU blit demo: bounce a block around the screen */
static void demo_blit(void)
{
    int bw = 120, bh = 90;
    int bx, by, dx, dy;
    int i;

    gpu_fill(0, 0, g_xres, g_yres, 0);
    gpu_wait_idle();

    /* Draw a small pattern to blit around */
    for (i = 0; i < 7; i++) {
        int y0 = i * (bh / 7);
        int y1 = y0 + bh / 7 - 1;
        unsigned char base = (unsigned char)(1 + i * 32);
        int x;
        for (x = 0; x < bw; x++) {
            unsigned char c = base + (unsigned char)(x * 31 / bw);
            gpu_fill(x, y0, 1, y1 - y0 + 1, c);
        }
    }
    gpu_wait_idle();

    cpu_str_c(g_yres - 14, "GPU Blit Bounce - press any key to stop", 253, 1);

    bx = 0; by = 0; dx = 3; dy = 2;

    while (!kbhit()) {
        int nx = bx + dx;
        int ny = by + dy;

        if (nx < 0 || nx + bw > g_xres) { dx = -dx; nx = bx + dx; }
        if (ny < 0 || ny + bh > g_yres - 16) { dy = -dy; ny = by + dy; }

        gpu_blit(bx, by, nx, ny, bw, bh);

        /* Erase trail edges */
        if (dx > 0) gpu_fill(bx, by, dx, bh, 0);
        else        gpu_fill(nx + bw, by, -dx, bh, 0);
        if (dy > 0) gpu_fill(bx, by, bw, dy, 0);
        else        gpu_fill(bx, ny + bh, bw, -dy, 0);

        bx = nx; by = ny;

        /* Small delay for visible motion */
        gpu_wait_idle();
    }
    getch();
}

/* =============================================================== */
/*  Demo 5: GPU parallax scrolling with color-key transparency      */
/*                                                                  */
/*  4 layers at different scroll speeds, composited each frame      */
/*  using GPU BLT.  Upper layers use source color-key (index 0) so */
/*  lower layers show through transparent regions.                  */
/*  Double-buffered via VBE 4F07h page flip.                        */
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
            + tri_wave(x * 7,        g_xres * 2, g_yres / 5)
            + tri_wave(x * 13 + 80,  g_xres * 3, g_yres / 7)
            + tri_wave(x * 29 + 200, g_xres,     g_yres / 10);
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
            + tri_wave(x * 11,       g_xres * 2, g_yres / 8)
            + tri_wave(x * 23 + 50,  g_xres,     g_yres / 12);
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
static void demo_parallax(void)
{
    int layer_base[PLAX_NLAYERS];
    int back_page, back_y, scroll;
    long fps_count;
    clock_t fps_t0;
    double fps;
    char buf[80];
    int i;
    unsigned long need;
    RMI rm;

    /* Verify VRAM can hold 2 display pages + 4 layer pages */
    need = (unsigned long)g_pitch * g_yres * 6;
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
        layer_base[i] = g_yres * (2 + i);

    /* Widen scissor so GPU ops can reach offscreen VRAM */
    gpu_wait_fifo(1);
    wreg(R_SC_BOTTOM_RIGHT, (0x1FFFUL << 16) | (unsigned long)g_xres);

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
    gpu_fill(0, 0, g_xres, g_yres * 2, 0);
    gpu_wait_idle();

    back_page = 1;
    scroll    = 0;
    fps_count = 0;
    fps       = 0.0;
    fps_t0    = clock();

    while (!kbhit()) {
        back_y = back_page * g_yres;

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

        /* FPS counter (update every ~1 second) */
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

        /* HUD text (CPU writes directly to back buffer in LFB) */
        sprintf(buf, "GPU Parallax  %d layers  %.1f FPS  [ESC quit]",
                PLAX_NLAYERS, fps);
        cpu_str(4, back_y + 4, buf, 255, 1);

        /* VBE 4F07h: flip display to back buffer (vsync) */
        memset(&rm, 0, sizeof rm);
        rm.eax = 0x4F07;
        rm.ebx = 0x0080;   /* set during vertical retrace */
        rm.ecx = 0;
        rm.edx = (unsigned long)back_y;
        dpmi_rmint(0x10, &rm);

        back_page ^= 1;
        scroll++;
        if (scroll >= g_xres * 1000) scroll = 0;  /* prevent overflow */
    }
    getch();

    /* Restore display to page 0 */
    memset(&rm, 0, sizeof rm);
    rm.eax = 0x4F07;
    rm.ebx = 0x0000;
    rm.ecx = 0;
    rm.edx = 0;
    dpmi_rmint(0x10, &rm);

    /* Restore scissor */
    gpu_wait_fifo(1);
    wreg(R_SC_BOTTOM_RIGHT,
         ((unsigned long)g_yres << 16) | (unsigned long)g_xres);
}
/* =============================================================== */

int main(void)
{
    unsigned long bar0, bar1, pci_cmd;
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

    /* --- Map MMIO registers (BAR0 — may be 64-bit on PCIe) --- */
    bar0 = pci_rd32(g_pci_bus, g_pci_dev, g_pci_func, 0x10);
    g_mmio_phys = bar0 & 0xFFFFFFF0UL;

    /* Check if BAR0 is 64-bit (type field bits [2:1] == 10b) */
    if ((bar0 & 0x06) == 0x04) {
        bar1 = pci_rd32(g_pci_bus, g_pci_dev, g_pci_func, 0x14);
        printf("  BAR0      : 64-bit, lower=0x%08lX upper=0x%08lX\n",
               bar0, bar1);
        if (bar1 != 0) {
            printf("ERROR: MMIO mapped above 4GB (0x%08lX%08lX).\n",
                   bar1, g_mmio_phys);
            printf("Cannot access from 32-bit DOS extender.\n");
            dpmi_free();
            return 1;
        }
    } else {
        printf("  BAR0      : 32-bit = 0x%08lX\n", bar0);
    }

    printf("  MMIO phys : 0x%08lX %s\n", g_mmio_phys,
           (bar0 & 0x08) ? "(prefetchable)" : "(non-prefetchable)");

    /* Map 128KB of MMIO (RV515 register space) */
    g_mmio = (volatile unsigned long *)dpmi_map(g_mmio_phys, 0x20000);
    if (!g_mmio) {
        printf("ERROR: Cannot map MMIO registers.\n");
        dpmi_free();
        return 1;
    }
    printf("  MMIO lin  : 0x%08lX (mapped 128KB)\n",
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
        printf("  BAR0 may not be MMIO. Check lspci output.\n");
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
    {
        unsigned long pitch64 = ((unsigned long)g_pitch + 63) / 64;
        unsigned long po = (pitch64 << 22) |
                           ((g_fb_location >> 10) & 0x003FFFFFUL);
        printf("  PITCH_OFFSET: 0x%08lX  (pitch64=%lu, offset=0x%lX)\n",
               po, pitch64, (g_fb_location >> 10) & 0x003FFFFFUL);
    }

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

    /* Map enough LFB for parallax demo: 2 display + 4 layer pages (6×) */
    lfb_sz = (unsigned long)g_pitch * g_yres * 6;
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
        }
    }

    /* === Demo 1: GPU-accelerated pattern === */
    demo_pattern();
    cpu_str_c(4, "GPU-Drawn Pattern (fill + line)", 255, 1);
    cpu_str_c(g_yres - 14,
              "Radeon 2D engine drew all shapes.  Press key...", 253, 1);
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
        /* === Demo 5: GPU parallax scrolling === */
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
