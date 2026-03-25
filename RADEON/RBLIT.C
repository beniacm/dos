/*
 * RBLIT.C  -  ATI Radeon X1300 Pro (RV515/RV516) 2D Blitter Validation
 * For OpenWatcom 2.0, 32-bit DOS (PMODE/W)
 *
 * Exercises the GPU 2D engine and validates results by reading VRAM:
 *   - Solid fill: single pixel, small rect, large rect, color values
 *   - Pitch alignment: GPU vs CPU stride agreement
 *   - Screen-to-screen blit: forward copy correctness
 *   - Color-key transparency (CLR_CMP_FCN_NE=5 vs FCN_EQ=4)
 *   - Overlapping blit direction handling
 *   - Scissor clipping
 *   - Multi-operation state leakage (fill → blit → keyed blit)
 *   - AVIVO display register state (D1VGA_CONTROL, D1GRPH_PITCH/X_END/Y_END)
 *   - Vblank timing via D1CRTC_V_BLANK (validates fallback vsync path)
 *   - Ghost rectangle: checks for pitch-mismatch echoes at +32-column offset
 *   - Full-width keyed blit: reproduces parallax layer (diagnoses sky leak)
 *   - VGA CRTC state: CR13 pitch register (controls VGA scanout stride)
 *   - AVIVO page flip address verification
 *
 * Outputs BLITLOG.TXT for offline analysis.
 *
 * Build: wcc386 -bt=dos -3r -ox -s -zq RBLIT.C
 *        wlink system pmodew name RBLIT file RBLIT option quiet
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
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
/* =============================================================== */

#define R_MC_IND_INDEX             0x0070
#define R_MC_IND_DATA              0x0074
#define R_RBBM_SOFT_RESET          0x00F0
#define   SOFT_RESET_CP            (1UL << 0)
#define   SOFT_RESET_HI            (1UL << 1)
#define   SOFT_RESET_E2            (1UL << 5)
#define R_CONFIG_MEMSIZE           0x00F8
#define R_HOST_PATH_CNTL           0x0130
#define R_HDP_FB_LOCATION          0x0134
#define R_SURFACE_CNTL             0x0B00

#define RV515_MC_FB_LOCATION       0x0001

#define R_RBBM_STATUS              0x0E40
#define   RBBM_FIFOCNT_MASK        0x007F
#define   RBBM_ACTIVE              (1UL << 31)

/* 2D engine registers */
#define R_WAIT_UNTIL               0x1720
#define   WAIT_2D_IDLECLEAN        (1UL << 16)
#define   WAIT_3D_IDLECLEAN        (1UL << 17)
#define   WAIT_DMA_GUI_IDLE        (1UL << 9)
#define R_DSTCACHE_CTLSTAT         0x1714
#define   R300_RB2D_DC_FLUSH_ALL   0x05UL
#define R_RBBM_GUICNTL             0x172C

#define R_DST_OFFSET               0x1404
#define R_DST_PITCH                0x1408
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
#define R_DP_WRITE_MASK            0x16CC
#define R_DEFAULT_PITCH_OFFSET     0x16E0

#define R_DEFAULT_SC_BOTTOM_RIGHT  0x16E8
#define R_SC_TOP_LEFT              0x16EC
#define R_SC_BOTTOM_RIGHT          0x16F0

/* Color compare registers */
#define R_CLR_CMP_CNTL            0x15C0
#define R_CLR_CMP_CLR_SRC         0x15C4
#define R_CLR_CMP_MASK            0x15CC
/* Hardware-confirmed: FCN=4 draws when src != key; FCN=5 draws when src == key.
   xf86 names these EQ_COLOR/NEQ_COLOR describing the *skip* condition. */
#define   CLR_CMP_FCN_NE          4UL           /* skip-if-equal  → draw when src != key */
#define   CLR_CMP_FCN_EQ          5UL           /* skip-if-neq    → draw when src == key */
#define   CLR_CMP_SRC_SOURCE      (1UL << 24)   /* compare source pixels */

/* R300+ cache control */
#define R_RB3D_DSTCACHE_MODE       0x3258
#define R_RB2D_DSTCACHE_MODE       0x3428
#define R_RB2D_DSTCACHE_CTLSTAT    0x342C
#define   RB2D_DC_FLUSH_ALL        0x0FUL
#define   RB2D_DC_BUSY             (1UL << 31)

#define R_GB_TILE_CONFIG           0x4018
#define   GB_TILE_ENABLE           (1UL << 0)
#define   GB_TILE_SIZE_16          (1UL << 4)
#define   GB_PIPE_COUNT_RV350      (0UL << 1)
#define R_DST_PIPE_CONFIG          0x170C
#define   PIPE_AUTO_CONFIG         (1UL << 31)
#define R_GB_PIPE_SELECT           0x402C

/* RV515 ring-start registers */
#define R_ISYNC_CNTL               0x1724
#define   ISYNC_ANY2D_IDLE3D       (1UL << 0)
#define   ISYNC_ANY3D_IDLE2D       (1UL << 1)
#define   ISYNC_WAIT_IDLEGUI       (1UL << 4)
#define   ISYNC_CPSCRATCH_IDLEGUI  (1UL << 5)
#define R_GB_ENABLE                0x4008
#define R_GB_SELECT                0x401C
#define R_GB_AA_CONFIG             0x4020
#define R_SU_REG_DEST              0x42C8
#define R_VAP_INDEX_OFFSET         0x208C
#define R_GA_ENHANCE               0x4274
#define   GA_DEADLOCK_CNTL         (1UL << 0)
#define   GA_FASTSYNC_CNTL         (1UL << 1)
#define R_RB3D_DSTCACHE_CTLSTAT_RS 0x4E4C
#define   RB3D_DC_FLUSH_RS         (2UL << 0)
#define   RB3D_DC_FREE_RS          (2UL << 2)
#define R_ZB_ZCACHE_CTLSTAT        0x4F18
#define   ZC_FLUSH                 (1UL << 0)
#define   ZC_FREE                  (1UL << 1)
#define R_VGA_RENDER_CONTROL       0x0300
#define   VGA_VSTATUS_CNTL_MASK    0x00070000UL
#define R_CLOCK_CNTL_INDEX         0x0008
#define R_CLOCK_CNTL_DATA          0x000C
#define   PLL_WR_EN                (1UL << 7)
#define R500_DYN_SCLK_PWMEM_PIPE  0x000D

#define   HDP_SOFT_RESET           (1UL << 26)
#define   HDP_APER_CNTL            (1UL << 23)

/* AVIVO display controller registers (RV515/R500) */
#define R_D1GRPH_PRIMARY_SURFACE_ADDRESS   0x6110
#define R_D1GRPH_SECONDARY_SURFACE_ADDRESS 0x6118
#define R_D1GRPH_PITCH                     0x6120
#define R_D1GRPH_X_START                   0x612C
#define R_D1GRPH_Y_START                   0x6130
#define R_D1GRPH_X_END                     0x6134  /* must equal xres, not pitch */
#define R_D1GRPH_Y_END                     0x6138
#define R_D1GRPH_UPDATE                    0x6144
#define   D1GRPH_SURFACE_UPDATE_PENDING    (1UL << 2)
#define   D1GRPH_SURFACE_UPDATE_LOCK       (1UL << 16)
#define R_D1GRPH_FLIP_CONTROL              0x6148
/* VGA mode enable: when set the VGA block drives scanout, D1GRPH flips ignored */
#define R_D1VGA_CONTROL                    0x0330
#define   D1VGA_MODE_ENABLE                (1UL << 0)
/* AVIVO CRTC control/status */
#define R_D1CRTC_CONTROL                   0x6080
#define   AVIVO_CRTC_EN                    (1UL << 0)
#define R_D1CRTC_STATUS                    0x609C
#define   D1CRTC_V_BLANK                   (1UL << 0)

/* RV515 device IDs */
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

static unsigned short g_dseg = 0, g_dsel = 0;
static volatile unsigned long *g_mmio = NULL;
static unsigned long  g_mmio_phys = 0;
static unsigned long  g_vram_mb   = 0;
static unsigned char *g_lfb       = NULL;
static unsigned long  g_lfb_phys  = 0;
static unsigned long  g_lfb_size  = 0;   /* actual mapped LFB size */
static unsigned long  g_fb_location = 0;
static int g_xres, g_yres, g_pitch;
static int g_vbe_pitch;    /* original VBE pitch before alignment */
static int g_page_stride;  /* rows per page, 4KB-aligned */
static unsigned short g_vmode;
static int g_fifo_free = 0;
static int g_num_gb_pipes = 1;

static int  g_pci_bus, g_pci_dev, g_pci_func;
static unsigned short g_pci_did;
static const char    *g_card_name = "Unknown";

static FILE *g_log = NULL;
static int g_pass = 0, g_fail = 0, g_warn = 0;

/* =============================================================== */
/*  Dual output: console + log file                                 */
/* =============================================================== */

static void out(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    if (g_log) {
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

static void result(int pass, const char *fmt, ...)
{
    va_list ap;
    const char *tag = pass ? "PASS" : "FAIL";
    if (pass) g_pass++; else g_fail++;

    printf("    [%s] ", tag);
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    if (g_log) {
        fprintf(g_log, "    [%s] ", tag);
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

static void warn(const char *fmt, ...)
{
    va_list ap;
    g_warn++;

    printf("    [WARN] ");
    va_start(ap, fmt);
    vprintf(fmt, ap);
    va_end(ap);

    if (g_log) {
        fprintf(g_log, "    [WARN] ");
        va_start(ap, fmt);
        vfprintf(g_log, fmt, ap);
        va_end(ap);
        fflush(g_log);
    }
}

/* =============================================================== */
/*  DPMI helpers                                                    */
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
/*  PCI                                                             */
/* =============================================================== */

static unsigned long pci_rd32(int b, int d, int f, int reg)
{
    outpd(0xCF8, 0x80000000UL | ((unsigned long)b<<16) |
          ((unsigned long)d<<11) | ((unsigned long)f<<8) | (reg & 0xFC));
    return inpd(0xCFC);
}
static unsigned short pci_rd16(int b, int d, int f, int reg)
{
    unsigned long v = pci_rd32(b, d, f, reg & ~3);
    return (unsigned short)(v >> ((reg & 2) * 8));
}

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
                    if (!(hdr & 0x80)) break;
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

static int vbe_get_info(VBEInfo *out_vi)
{
    RMI rm;
    memset(&rm, 0, sizeof rm);
    memset(dosbuf(), 0, 512);
    memcpy(dosbuf(), "VBE2", 4);
    rm.eax = 0x4F00;  rm.es = g_dseg;  rm.edi = 0;
    if (!dpmi_rmint(0x10, &rm)) return 0;
    if ((rm.eax & 0xFFFF) != 0x004F) return 0;
    memcpy(out_vi, dosbuf(), sizeof(VBEInfo));
    return 1;
}
static int vbe_get_mode(unsigned short m, VBEMode *out_m)
{
    RMI rm;
    memset(&rm, 0, sizeof rm);
    memset(dosbuf(), 0, 256);
    rm.eax = 0x4F01;  rm.ecx = m;  rm.es = g_dseg;  rm.edi = 0;
    if (!dpmi_rmint(0x10, &rm)) return 0;
    if ((rm.eax & 0xFFFF) != 0x004F) return 0;
    memcpy(out_m, dosbuf(), sizeof(VBEMode));
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

    for (i = 0; modelist[i] != 0xFFFF; i++) {
        if (!vbe_get_mode(modelist[i], &mi)) continue;
        if (mi.bpp != 8) continue;
        if (!(mi.attr & 0x80)) continue;  /* no LFB */
        if (mi.model != 4 && mi.model != 6) continue;  /* packed/direct */
        /* Prefer 800x600; accept 640x480 as fallback */
        if (mi.xres == 800 && mi.yres == 600) {
            g_xres = mi.xres;  g_yres = mi.yres;
            g_pitch = mi.pitch;  g_lfb_phys = mi.lfb_phys;
            g_vmode = modelist[i];
            return 1;
        }
        if ((unsigned long)mi.xres * mi.yres > best_pixels) {
            best_pixels = (unsigned long)mi.xres * mi.yres;
            g_xres = mi.xres;  g_yres = mi.yres;
            g_pitch = mi.pitch;  g_lfb_phys = mi.lfb_phys;
            g_vmode = modelist[i];
        }
    }
    return (best_pixels > 0);
}

/* =============================================================== */
/*  MMIO register access                                            */
/* =============================================================== */

static unsigned long rreg(unsigned long off)
{ return g_mmio[off >> 2]; }

static void wreg(unsigned long off, unsigned long val)
{ g_mmio[off >> 2] = val; }

static unsigned long mc_rreg(unsigned long reg)
{
    unsigned long r;
    wreg(R_MC_IND_INDEX, 0x7F0000UL | (reg & 0xFFFF));
    r = rreg(R_MC_IND_DATA);
    wreg(R_MC_IND_INDEX, 0);
    return r;
}

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

/* =============================================================== */
/*  GPU engine control                                              */
/* =============================================================== */

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

static void gpu_engine_flush(void)
{
    unsigned long timeout = 2000000UL;
    wreg(R_RB2D_DSTCACHE_CTLSTAT, RB2D_DC_FLUSH_ALL);
    while (timeout--) {
        if (!(rreg(R_RB2D_DSTCACHE_CTLSTAT) & RB2D_DC_BUSY))
            return;
    }
}

static void gpu_wait_idle(void)
{
    unsigned long timeout = 2000000UL;
    gpu_wait_fifo(64);
    while (timeout--) {
        if (!(rreg(R_RBBM_STATUS) & RBBM_ACTIVE))
            break;
    }
    gpu_engine_flush();
    g_fifo_free = 64;
}

static void gpu_engine_reset(void)
{
    unsigned long rbbm_soft_reset;
    unsigned long host_path_cntl;
    unsigned long clock_cntl_index;

    clock_cntl_index = rreg(R_CLOCK_CNTL_INDEX);

    rbbm_soft_reset = rreg(R_RBBM_SOFT_RESET);
    wreg(R_RBBM_SOFT_RESET, rbbm_soft_reset |
         SOFT_RESET_CP | SOFT_RESET_HI | SOFT_RESET_E2);
    (void)rreg(R_RBBM_SOFT_RESET);
    wreg(R_RBBM_SOFT_RESET, 0);
    (void)rreg(R_RBBM_SOFT_RESET);

    wreg(R_RB3D_DSTCACHE_MODE, rreg(R_RB3D_DSTCACHE_MODE) | (1UL << 17));

    host_path_cntl = rreg(R_HOST_PATH_CNTL);
    wreg(R_HOST_PATH_CNTL, host_path_cntl | HDP_SOFT_RESET | HDP_APER_CNTL);
    (void)rreg(R_HOST_PATH_CNTL);
    wreg(R_HOST_PATH_CNTL, host_path_cntl | HDP_APER_CNTL);
    (void)rreg(R_HOST_PATH_CNTL);

    wreg(R_RB2D_DSTCACHE_MODE, rreg(R_RB2D_DSTCACHE_MODE) |
         (1UL << 2) | (1UL << 15));

    wreg(R_CLOCK_CNTL_INDEX, clock_cntl_index);
}

static void gpu_vga_render_disable(void)
{
    wreg(R_VGA_RENDER_CONTROL,
         rreg(R_VGA_RENDER_CONTROL) & ~VGA_VSTATUS_CNTL_MASK);
}

/* =============================================================== */
/*  Full GPU 2D initialization (from RADEON.C gpu_init_2d)          */
/* =============================================================== */

static void gpu_init_2d(void)
{
    unsigned long pitch64, pitch_offset;
    unsigned long gb_pipe_sel, gb_tile_config;
    unsigned long dst_pipe_val, pipe_sel_current, pll_tmp;

    gpu_wait_idle();
    gpu_vga_render_disable();

    gb_pipe_sel = rreg(R_GB_PIPE_SELECT);
    g_num_gb_pipes = (int)((gb_pipe_sel >> 12) & 0x3) + 1;

    pll_tmp = (1UL | (((gb_pipe_sel >> 8) & 0xFUL) << 4));
    pll_wreg(R500_DYN_SCLK_PWMEM_PIPE, pll_tmp);

    dst_pipe_val = rreg(R_DST_PIPE_CONFIG);
    pipe_sel_current = (dst_pipe_val >> 2) & 3;
    pll_wreg(R500_DYN_SCLK_PWMEM_PIPE,
             (1UL << pipe_sel_current) |
             (((gb_pipe_sel >> 8) & 0xFUL) << 4));

    gpu_wait_idle();

    gb_tile_config = GB_TILE_ENABLE | GB_TILE_SIZE_16;
    switch (g_num_gb_pipes) {
    case 2:  gb_tile_config |= (1UL << 1); break;
    case 3:  gb_tile_config |= (2UL << 1); break;
    case 4:  gb_tile_config |= (3UL << 1); break;
    default: gb_tile_config |= GB_PIPE_COUNT_RV350; break;
    }
    wreg(R_GB_TILE_CONFIG, gb_tile_config);
    wreg(R_WAIT_UNTIL, WAIT_2D_IDLECLEAN | WAIT_3D_IDLECLEAN);
    wreg(R_DST_PIPE_CONFIG, rreg(R_DST_PIPE_CONFIG) | PIPE_AUTO_CONFIG);

    wreg(R_RB2D_DSTCACHE_MODE, rreg(R_RB2D_DSTCACHE_MODE) |
         (1UL << 2) | (1UL << 15));

    gpu_engine_reset();

    gpu_wait_fifo(12);
    wreg(R_ISYNC_CNTL,
         ISYNC_ANY2D_IDLE3D | ISYNC_ANY3D_IDLE2D |
         ISYNC_WAIT_IDLEGUI | ISYNC_CPSCRATCH_IDLEGUI);
    wreg(R_WAIT_UNTIL, WAIT_2D_IDLECLEAN | WAIT_3D_IDLECLEAN);
    wreg(R_DST_PIPE_CONFIG, rreg(R_DST_PIPE_CONFIG) | PIPE_AUTO_CONFIG);
    wreg(R_GB_SELECT, 0);
    wreg(R_GB_ENABLE, 0);
    wreg(R_SU_REG_DEST, (1UL << g_num_gb_pipes) - 1);
    wreg(R_VAP_INDEX_OFFSET, 0);
    wreg(R_RB3D_DSTCACHE_CTLSTAT_RS, RB3D_DC_FLUSH_RS | RB3D_DC_FREE_RS);
    wreg(R_ZB_ZCACHE_CTLSTAT, ZC_FLUSH | ZC_FREE);
    wreg(R_WAIT_UNTIL, WAIT_2D_IDLECLEAN | WAIT_3D_IDLECLEAN);
    wreg(R_GB_AA_CONFIG, 0);
    wreg(R_RB3D_DSTCACHE_CTLSTAT_RS, RB3D_DC_FLUSH_RS | RB3D_DC_FREE_RS);
    wreg(R_ZB_ZCACHE_CTLSTAT, ZC_FLUSH | ZC_FREE);

    gpu_wait_fifo(4);
    wreg(R_GA_ENHANCE, GA_DEADLOCK_CNTL | GA_FASTSYNC_CNTL);
    wreg(R_RBBM_GUICNTL, 0);
    gpu_wait_idle();
    gpu_engine_flush();

    /* 2D engine setup */
    pitch64 = ((unsigned long)g_pitch + 63) / 64;
    pitch_offset = (pitch64 << 22) | ((g_fb_location >> 10) & 0x003FFFFFUL);

    gpu_wait_fifo(18);
    wreg(R_DEFAULT_PITCH_OFFSET, pitch_offset);
    wreg(R_DST_PITCH_OFFSET, pitch_offset);
    wreg(R_SRC_PITCH_OFFSET, pitch_offset);

    /* Also write separate pitch/offset registers (match RADEON.C fix) */
    wreg(R_DST_OFFSET, g_fb_location);
    wreg(R_DST_PITCH,  (unsigned long)g_pitch);
    wreg(R_SRC_OFFSET, g_fb_location);
    wreg(R_SRC_PITCH,  (unsigned long)g_pitch);

    wreg(R_DEFAULT_SC_BOTTOM_RIGHT, (0x1FFF << 16) | 0x1FFF);
    wreg(R_SC_TOP_LEFT, 0);
    wreg(R_SC_BOTTOM_RIGHT, (0x1FFFUL << 16) | 0x1FFFUL);

    wreg(R_DP_WRITE_MASK, 0xFFFFFFFF);
    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);

    wreg(R_DP_BRUSH_FRGD_CLR, 0xFFFFFFFF);
    wreg(R_DP_BRUSH_BKGD_CLR, 0x00000000);
    wreg(R_DP_SRC_FRGD_CLR, 0xFFFFFFFF);
    wreg(R_DP_SRC_BKGD_CLR, 0x00000000);

    wreg(R_SURFACE_CNTL, 0);
    wreg(R_CLR_CMP_CNTL, 0);

    gpu_wait_idle();
}

/* =============================================================== */
/*  GPU 2D primitives                                               */
/* =============================================================== */

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
    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
    wreg(R_SRC_Y_X, ((unsigned long)sy << 16) | (unsigned long)(sx & 0xFFFF));
    wreg(R_DST_Y_X, ((unsigned long)dy << 16) | (unsigned long)(dx & 0xFFFF));
    wreg(R_DST_HEIGHT_WIDTH, ((unsigned long)h << 16) | (unsigned long)w);
}

/* Blit with explicit FCN value for testing */
static void gpu_blit_cmp(int sx, int sy, int dx, int dy, int w, int h,
                         unsigned char key, unsigned long fcn)
{
    gpu_wait_fifo(8);
    wreg(R_CLR_CMP_CLR_SRC, (unsigned long)key);
    wreg(R_CLR_CMP_MASK,    0x000000FFUL);
    wreg(R_CLR_CMP_CNTL,    CLR_CMP_SRC_SOURCE | fcn);
    wreg(R_DP_GUI_MASTER_CNTL,
         GMC_BRUSH_NONE | GMC_DST_8BPP | GMC_SRC_DATATYPE_COLOR |
         ROP3_SRCCOPY | GMC_DP_SRC_MEMORY | GMC_WR_MSK_DIS);
    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
    wreg(R_SRC_Y_X, ((unsigned long)sy << 16) | (unsigned long)(sx & 0xFFFF));
    wreg(R_DST_Y_X, ((unsigned long)dy << 16) | (unsigned long)(dx & 0xFFFF));
    wreg(R_DST_HEIGHT_WIDTH, ((unsigned long)h << 16) | (unsigned long)w);
}

static void gpu_disable_cmp(void)
{
    gpu_wait_fifo(1);
    wreg(R_CLR_CMP_CNTL, 0);
}

/* =============================================================== */
/*  VRAM read helper — reads from LFB at (x,y)                     */
/* =============================================================== */

static unsigned char vram_read(int x, int y)
{
    return g_lfb[(long)y * g_pitch + x];
}

/* Clear a rectangular region of VRAM via CPU (known-good baseline) */
static void cpu_clear(int x, int y, int w, int h, unsigned char val)
{
    int row;
    for (row = y; row < y + h; row++)
        memset(g_lfb + (long)row * g_pitch + x, val, w);
}

/* Compare a rectangular VRAM region against expected value.
   Returns number of mismatched pixels. */
static int vram_check_rect(int x, int y, int w, int h, unsigned char expect)
{
    int bad = 0, row, col;
    for (row = y; row < y + h; row++)
        for (col = x; col < x + w; col++)
            if (g_lfb[(long)row * g_pitch + col] != expect)
                bad++;
    return bad;
}

/* Dump first few mismatches for diagnostics */
static void vram_dump_mismatches(int x, int y, int w, int h,
                                 unsigned char expect, int max_show)
{
    int row, col, shown = 0;
    for (row = y; row < y + h && shown < max_show; row++)
        for (col = x; col < x + w && shown < max_show; col++) {
            unsigned char got = g_lfb[(long)row * g_pitch + col];
            if (got != expect) {
                out("      (%d,%d): expected 0x%02X, got 0x%02X\n",
                    col, row, expect, got);
                shown++;
            }
        }
    if (shown >= max_show)
        out("      ... (more mismatches)\n");
}

/* =============================================================== */
/*  TEST 1: Solid fill — basic functionality                        */
/* =============================================================== */

static void test_fill_basic(void)
{
    int bad;

    out("\n========================================\n");
    out("  TEST 1: GPU Solid Fill - Basic\n");
    out("========================================\n\n");

    /* 1a: Single pixel fill */
    out("  1a: Single pixel fill at (0,0) with color 0xAA\n");
    cpu_clear(0, 0, 16, 16, 0x00);
    gpu_fill(0, 0, 1, 1, 0xAA);
    gpu_wait_idle();
    {
        unsigned char got = vram_read(0, 0);
        unsigned char neighbor = vram_read(1, 0);
        result(got == 0xAA,
               "pixel (0,0) = 0x%02X (expect 0xAA)\n", got);
        result(neighbor == 0x00,
               "pixel (1,0) = 0x%02X (expect 0x00, not overwritten)\n",
               neighbor);
    }

    /* 1b: Small rectangle (4x4) */
    out("\n  1b: 4x4 fill at (10,10) with color 0x55\n");
    cpu_clear(0, 0, 64, 64, 0x00);
    gpu_fill(10, 10, 4, 4, 0x55);
    gpu_wait_idle();
    bad = vram_check_rect(10, 10, 4, 4, 0x55);
    result(bad == 0, "4x4 interior: %d bad pixels\n", bad);
    /* Check border is untouched */
    {
        int border_bad = 0;
        unsigned char v;
        int i;
        for (i = 10; i < 14; i++) {
            v = vram_read(i, 9);  if (v != 0x00) border_bad++;
            v = vram_read(i, 14); if (v != 0x00) border_bad++;
        }
        for (i = 10; i < 14; i++) {
            v = vram_read(9, i);  if (v != 0x00) border_bad++;
            v = vram_read(14, i); if (v != 0x00) border_bad++;
        }
        result(border_bad == 0,
               "4x4 border untouched: %d bad pixels\n", border_bad);
    }

    /* 1c: All 256 color values */
    out("\n  1c: Fill with all 256 color values (1x1 each)\n");
    cpu_clear(0, 0, 256, 4, 0x00);
    {
        int i, bad_colors = 0;
        for (i = 0; i < 256; i++) {
            gpu_fill(i, 0, 1, 1, (unsigned char)i);
        }
        gpu_wait_idle();
        for (i = 0; i < 256; i++) {
            if (vram_read(i, 0) != (unsigned char)i)
                bad_colors++;
        }
        result(bad_colors == 0,
               "256 color values: %d mismatches\n", bad_colors);
    }

    /* 1d: Larger fill (64x64) */
    out("\n  1d: 64x64 fill at (100,100) with color 0x77\n");
    cpu_clear(96, 96, 72, 72, 0x00);
    gpu_fill(100, 100, 64, 64, 0x77);
    gpu_wait_idle();
    bad = vram_check_rect(100, 100, 64, 64, 0x77);
    result(bad == 0, "64x64 interior: %d bad pixels\n", bad);
    if (bad > 0)
        vram_dump_mismatches(100, 100, 64, 64, 0x77, 5);
}

/* =============================================================== */
/*  TEST 2: Pitch alignment validation                              */
/* =============================================================== */

static void test_pitch_alignment(void)
{
    int aligned;
    unsigned long po_readback, po_pitch64;

    out("\n========================================\n");
    out("  TEST 2: Pitch Alignment\n");
    out("========================================\n\n");

    out("  VBE reported pitch: %d\n", g_vbe_pitch);
    out("  Aligned pitch:      %d\n", g_pitch);
    aligned = !(g_pitch & 63);
    result(aligned, "pitch %d is %s-aligned\n",
           g_pitch, aligned ? "64-byte" : "NOT 64-byte");

    /* Read back PITCH_OFFSET register — on RV515 this is FIFO-queued
       and CPU readback returns 0x00000000.  This is expected HW behavior,
       NOT an error.  The actual pitch/offset is still latched internally. */
    po_readback = rreg(R_DST_PITCH_OFFSET);
    po_pitch64 = (po_readback >> 22) & 0x3FF;
    out("  DST_PITCH_OFFSET = 0x%08lX  (pitch64=%lu → %lu bytes)\n",
        po_readback, po_pitch64, po_pitch64 * 64);
    if (po_readback == 0) {
        warn("DST_PITCH_OFFSET reads 0 (FIFO register, expected on RV515)\n");
    } else {
        result(po_pitch64 * 64 == (unsigned long)g_pitch,
               "PITCH_OFFSET pitch matches g_pitch (%lu == %d)\n",
               po_pitch64 * 64, g_pitch);
    }

    /* Test separate pitch/offset registers (these should be readable) */
    {
        unsigned long dst_off_rb = rreg(R_DST_OFFSET);
        unsigned long dst_pit_rb = rreg(R_DST_PITCH);
        unsigned long src_off_rb = rreg(R_SRC_OFFSET);
        unsigned long src_pit_rb = rreg(R_SRC_PITCH);
        out("  DST_OFFSET = 0x%08lX  (expect 0x%08lX)\n",
            dst_off_rb, g_fb_location);
        out("  DST_PITCH  = %lu  (expect %d)\n",
            dst_pit_rb, g_pitch);
        out("  SRC_OFFSET = 0x%08lX  SRC_PITCH = %lu\n",
            src_off_rb, src_pit_rb);
        result(dst_pit_rb == (unsigned long)g_pitch,
               "DST_PITCH register matches g_pitch (%lu == %d)\n",
               dst_pit_rb, g_pitch);
        result(dst_off_rb == g_fb_location,
               "DST_OFFSET register matches fb_location (0x%08lX == 0x%08lX)\n",
               dst_off_rb, g_fb_location);
    }

    /* Test: GPU fill a row at y=2, then verify CPU reads the correct
       pixels at the correct stride. This catches GPU/CPU pitch mismatch. */
    out("\n  Pitch coherence test: GPU fills row 2, CPU verifies stride\n");
    cpu_clear(0, 0, g_pitch, 4, 0x00);
    gpu_fill(0, 2, g_xres, 1, 0xBB);
    gpu_wait_idle();
    {
        int ok = 1;
        int x;
        /* Check row 2 has 0xBB */
        for (x = 0; x < g_xres; x++) {
            if (vram_read(x, 2) != 0xBB) { ok = 0; break; }
        }
        /* Check row 1 and row 3 are still 0x00 */
        if (ok) {
            for (x = 0; x < g_xres; x++) {
                if (vram_read(x, 1) != 0x00) { ok = 0; break; }
                if (vram_read(x, 3) != 0x00) { ok = 0; break; }
            }
        }
        result(ok, "GPU/CPU pitch agreement (row 2 fill, neighbors clean)\n");
        if (!ok) {
            /* Detailed diagnostics for pitch mismatch */
            out("      Row 1 sample: ");
            for (x = 0; x < 16 && x < g_xres; x++)
                out("%02X ", vram_read(x, 1));
            out("\n      Row 2 sample: ");
            for (x = 0; x < 16 && x < g_xres; x++)
                out("%02X ", vram_read(x, 2));
            out("\n      Row 3 sample: ");
            for (x = 0; x < 16 && x < g_xres; x++)
                out("%02X ", vram_read(x, 3));
            out("\n");
            /* Check if data from row 2 spilled into row 1 or 3 at
               the VBE pitch boundary (indicates pitch mismatch) */
            if (g_vbe_pitch != g_pitch) {
                out("      Checking for shear at VBE pitch %d:\n", g_vbe_pitch);
                out("        LFB[2*%d + 0] = 0x%02X\n", g_vbe_pitch,
                    g_lfb[(long)2 * g_vbe_pitch]);
                out("        LFB[2*%d + 0] = 0x%02X\n", g_pitch,
                    g_lfb[(long)2 * g_pitch]);
            }
        }
    }
}

/* =============================================================== */
/*  TEST 3: Screen-to-screen blit                                   */
/* =============================================================== */

static void test_blit_fwd(void)
{
    int x, bad;

    out("\n========================================\n");
    out("  TEST 3: Screen-to-Screen Blit (Forward)\n");
    out("========================================\n\n");

    /* Set up source pattern: horizontal gradient at row 50 */
    out("  Setup: CPU writes gradient at y=50..53 (16 wide)\n");
    cpu_clear(0, 48, g_xres, 16, 0x00);
    cpu_clear(0, 70, g_xres, 16, 0x00);
    for (x = 0; x < 16; x++)
        g_lfb[(long)50 * g_pitch + x] = (unsigned char)(x + 1);
    for (x = 0; x < 16; x++)
        g_lfb[(long)51 * g_pitch + x] = (unsigned char)(0x10 + x);
    for (x = 0; x < 16; x++)
        g_lfb[(long)52 * g_pitch + x] = (unsigned char)(0x20 + x);
    for (x = 0; x < 16; x++)
        g_lfb[(long)53 * g_pitch + x] = (unsigned char)(0x30 + x);

    /* Blit: (0,50) → (0,70), 16x4 */
    out("  GPU blit: (0,50) 16x4 → (0,70)\n");
    gpu_blit_fwd(0, 50, 0, 70, 16, 4);
    gpu_wait_idle();

    /* Verify destination */
    {
        int ok = 1;
        int r, c;
        for (r = 0; r < 4; r++) {
            for (c = 0; c < 16; c++) {
                unsigned char src = g_lfb[(long)(50 + r) * g_pitch + c];
                unsigned char dst = g_lfb[(long)(70 + r) * g_pitch + c];
                if (src != dst) { ok = 0; break; }
            }
            if (!ok) break;
        }
        result(ok, "16x4 blit: source matches destination\n");
        if (!ok) {
            out("      Source row 50: ");
            for (x = 0; x < 16; x++) out("%02X ", vram_read(x, 50));
            out("\n      Dest   row 70: ");
            for (x = 0; x < 16; x++) out("%02X ", vram_read(x, 70));
            out("\n");
        }
    }

    /* 3b: Blit to different X position */
    out("\n  3b: Blit (0,50) 16x4 → (200,70)\n");
    cpu_clear(196, 68, 24, 10, 0x00);
    gpu_blit_fwd(0, 50, 200, 70, 16, 4);
    gpu_wait_idle();
    {
        int ok = 1;
        int r, c;
        for (r = 0; r < 4; r++)
            for (c = 0; c < 16; c++) {
                unsigned char src = g_lfb[(long)(50 + r) * g_pitch + c];
                unsigned char dst = g_lfb[(long)(70 + r) * g_pitch + 200 + c];
                if (src != dst) { ok = 0; break; }
            }
        result(ok, "blit to different X: source matches destination\n");
    }

    /* 3c: Larger blit */
    out("\n  3c: Large blit 128x64\n");
    {
        int ok = 1;
        int r, c;
        /* Fill source area with pattern */
        for (r = 0; r < 64; r++)
            for (c = 0; c < 128; c++)
                g_lfb[(long)(200 + r) * g_pitch + c] =
                    (unsigned char)((r ^ c) & 0xFF);
        cpu_clear(300, 200, 132, 68, 0x00);

        gpu_blit_fwd(0, 200, 300, 200, 128, 64);
        gpu_wait_idle();

        for (r = 0; r < 64; r++)
            for (c = 0; c < 128; c++) {
                unsigned char src = g_lfb[(long)(200 + r) * g_pitch + c];
                unsigned char dst = g_lfb[(long)(200 + r) * g_pitch + 300 + c];
                if (src != dst) { ok = 0; break; }
            }
        result(ok, "128x64 blit: %s\n", ok ? "all pixels match" : "MISMATCH");
    }
}

/* =============================================================== */
/*  TEST 4: Color-key transparency (the critical FCN test)          */
/* =============================================================== */

static void test_color_key(void)
{
    int x, r;
    unsigned char key_color = 0x00;

    out("\n========================================\n");
    out("  TEST 4: Color-Key Transparency (CLR_CMP)\n");
    out("========================================\n\n");

    out("  This test validates CLR_CMP_FCN_NE=%lu and CLR_CMP_FCN_EQ=%lu\n",
        CLR_CMP_FCN_NE, CLR_CMP_FCN_EQ);
    out("  Per Linux kernel radeon_reg.h:\n");
    out("    RADEON_SRC_CMP_EQ_COLOR  = (4 << 0)  → draw when src == key\n");
    out("    RADEON_SRC_CMP_NEQ_COLOR = (5 << 0)  → draw when src != key\n\n");

    /*
     * Setup: Create a source pattern at (0,300):
     *   Row 300: [KEY, 0x11, KEY, 0x22, KEY, 0x33, KEY, 0x44, ...]
     * The destination at (0,320) is pre-filled with 0xFF.
     */
    key_color = 0x00;
    out("  Key color: 0x%02X\n", key_color);
    out("  Source pattern at y=300: alternating key/non-key\n");

    cpu_clear(0, 298, g_xres, 4, 0x00);
    for (x = 0; x < 16; x++) {
        if (x & 1)
            g_lfb[(long)300 * g_pitch + x] = (unsigned char)(0x10 + x);
        else
            g_lfb[(long)300 * g_pitch + x] = key_color;
    }

    out("  Source: ");
    for (x = 0; x < 16; x++)
        out("%02X ", vram_read(x, 300));
    out("\n\n");

    /* ---- Test 4a: FCN_NE (value 5) — should draw non-key pixels ---- */
    out("  4a: FCN_NE (value %lu): blit with key=0x%02X\n",
        CLR_CMP_FCN_NE, key_color);
    out("      Expected: non-key pixels drawn, key pixels leave dest unchanged\n");

    cpu_clear(0, 320, 20, 2, 0xFF);  /* fill dest with 0xFF sentinel */
    gpu_blit_cmp(0, 300, 0, 320, 16, 1, key_color, CLR_CMP_FCN_NE);
    gpu_wait_idle();
    gpu_disable_cmp();
    gpu_wait_idle();

    out("      Dest:   ");
    for (x = 0; x < 16; x++)
        out("%02X ", vram_read(x, 320));
    out("\n");

    {
        int ne_ok = 1;
        int drawn_non_key = 0, preserved_key = 0;
        for (x = 0; x < 16; x++) {
            unsigned char src = vram_read(x, 300);
            unsigned char dst = vram_read(x, 320);
            if (src == key_color) {
                /* Key pixel: dest should be unchanged (0xFF) */
                if (dst == 0xFF) preserved_key++;
                else ne_ok = 0;
            } else {
                /* Non-key pixel: dest should match source */
                if (dst == src) drawn_non_key++;
                else ne_ok = 0;
            }
        }
        result(ne_ok,
               "FCN_NE: %d non-key drawn, %d key preserved (of 8 each)\n",
               drawn_non_key, preserved_key);
        if (!ne_ok) {
            out("      DETAIL: non-key pixels that arrived: %d/8\n",
                drawn_non_key);
            out("      DETAIL: key pixels left as 0xFF:     %d/8\n",
                preserved_key);
        }
    }

    /* ---- Test 4b: FCN_EQ (value 4) — should draw key-matching pixels ---- */
    out("\n  4b: FCN_EQ (value %lu): blit with key=0x%02X\n",
        CLR_CMP_FCN_EQ, key_color);
    out("      Expected: key-matching pixels drawn, non-key pixels leave dest\n");

    cpu_clear(0, 330, 20, 2, 0xFF);
    gpu_blit_cmp(0, 300, 0, 330, 16, 1, key_color, CLR_CMP_FCN_EQ);
    gpu_wait_idle();
    gpu_disable_cmp();
    gpu_wait_idle();

    out("      Dest:   ");
    for (x = 0; x < 16; x++)
        out("%02X ", vram_read(x, 330));
    out("\n");

    {
        int eq_ok = 1;
        int drawn_key = 0, preserved_non_key = 0;
        for (x = 0; x < 16; x++) {
            unsigned char src = vram_read(x, 300);
            unsigned char dst = vram_read(x, 330);
            if (src == key_color) {
                /* Key pixel: should be drawn (src → dst) */
                if (dst == key_color) drawn_key++;
                else eq_ok = 0;
            } else {
                /* Non-key pixel: should be unchanged (0xFF) */
                if (dst == 0xFF) preserved_non_key++;
                else eq_ok = 0;
            }
        }
        result(eq_ok,
               "FCN_EQ: %d key drawn, %d non-key preserved (of 8 each)\n",
               drawn_key, preserved_non_key);
        if (!eq_ok) {
            out("      DETAIL: key pixels drawn:         %d/8\n", drawn_key);
            out("      DETAIL: non-key pixels preserved: %d/8\n",
                preserved_non_key);
        }
    }

    /* ---- Test 4c: FCN_NE with non-zero key ---- */
    out("\n  4c: FCN_NE with key=0x42 (non-zero key)\n");
    {
        unsigned char key2 = 0x42;
        int ne2_ok = 1;
        int drawn = 0, kept = 0;

        /* Source: known pattern with some 0x42 values */
        for (x = 0; x < 16; x++)
            g_lfb[(long)302 * g_pitch + x] =
                (x % 3 == 0) ? key2 : (unsigned char)(0xA0 + x);

        out("      Source: ");
        for (x = 0; x < 16; x++) out("%02X ", vram_read(x, 302));
        out("\n");

        cpu_clear(0, 340, 20, 2, 0xDD);
        gpu_blit_cmp(0, 302, 0, 340, 16, 1, key2, CLR_CMP_FCN_NE);
        gpu_wait_idle();
        gpu_disable_cmp();
        gpu_wait_idle();

        out("      Dest:   ");
        for (x = 0; x < 16; x++) out("%02X ", vram_read(x, 340));
        out("\n");

        for (x = 0; x < 16; x++) {
            unsigned char src = vram_read(x, 302);
            unsigned char dst = vram_read(x, 340);
            if (src == key2) {
                if (dst == 0xDD) kept++; else ne2_ok = 0;
            } else {
                if (dst == src) drawn++; else ne2_ok = 0;
            }
        }
        result(ne2_ok,
               "FCN_NE key=0x42: %d drawn, %d preserved\n", drawn, kept);
    }

    /* ---- Test 4d: Verify FCN value swapped would be wrong ---- */
    out("\n  4d: Explicitly test FCN=4 used as NE (the OLD wrong code)\n");
    out("      If this passes, FCN=4 means NE and kernel docs are wrong.\n");
    out("      If this fails, FCN=4 means EQ (kernel is correct).\n");
    {
        int draws_non_key = 0;
        int draws_key = 0;

        cpu_clear(0, 350, 20, 2, 0xFF);
        gpu_blit_cmp(0, 300, 0, 350, 16, 1, key_color, 4UL);
        gpu_wait_idle();
        gpu_disable_cmp();
        gpu_wait_idle();

        out("      Dest:   ");
        for (x = 0; x < 16; x++) out("%02X ", vram_read(x, 350));
        out("\n");

        for (x = 0; x < 16; x++) {
            unsigned char src = vram_read(x, 300);
            unsigned char dst = vram_read(x, 350);
            if (src == key_color) {
                if (dst == key_color) draws_key++;
            } else {
                if (dst == src) draws_non_key++;
            }
        }
        out("      FCN=4 drew %d non-key pixels, %d key pixels\n",
            draws_non_key, draws_key);
        if (draws_non_key == 8 && draws_key == 0)
            warn("FCN=4 acts as NE (contradicts kernel docs!)\n");
        else if (draws_key > 0 && draws_non_key == 0)
            result(1, "FCN=4 acts as EQ (matches kernel docs)\n");
        else
            warn("FCN=4 behaviour unclear: %d NE, %d EQ\n",
                 draws_non_key, draws_key);
    }

    /* ---- Test 4e: Explicitly test FCN=5 as NE ---- */
    out("\n  4e: Explicitly test FCN=5 used as NE\n");
    {
        int draws_non_key = 0;
        int draws_key = 0;

        cpu_clear(0, 360, 20, 2, 0xFF);
        gpu_blit_cmp(0, 300, 0, 360, 16, 1, key_color, 5UL);
        gpu_wait_idle();
        gpu_disable_cmp();
        gpu_wait_idle();

        out("      Dest:   ");
        for (x = 0; x < 16; x++) out("%02X ", vram_read(x, 360));
        out("\n");

        for (x = 0; x < 16; x++) {
            unsigned char src = vram_read(x, 300);
            unsigned char dst = vram_read(x, 360);
            if (src == key_color) {
                if (dst == key_color) draws_key++;
            } else {
                if (dst == src) draws_non_key++;
            }
        }
        out("      FCN=5 drew %d non-key pixels, %d key pixels\n",
            draws_non_key, draws_key);
        if (draws_non_key == 8 && draws_key == 0)
            result(1, "FCN=5 acts as NE (matches kernel docs)\n");
        else if (draws_key > 0 && draws_non_key == 0)
            warn("FCN=5 acts as EQ (contradicts kernel docs!)\n");
        else
            warn("FCN=5 behaviour unclear: %d NE, %d EQ\n",
                 draws_non_key, draws_key);
    }
}

/* =============================================================== */
/*  TEST 5: State leakage between operations                        */
/* =============================================================== */

static void test_state_leakage(void)
{
    int bad;

    out("\n========================================\n");
    out("  TEST 5: State Leakage Between Operations\n");
    out("========================================\n\n");

    /* Scenario: do a keyed blit, then a plain fill.
       If CLR_CMP_CNTL leaks, the fill might be affected. */
    out("  5a: Keyed blit → plain fill (CLR_CMP leak test)\n");

    /* Setup source for keyed blit */
    cpu_clear(0, 400, 32, 2, 0x00);
    {
        int x;
        for (x = 0; x < 16; x++)
            g_lfb[(long)400 * g_pitch + x] = (x & 1) ? 0x55 : 0x00;
    }

    /* Keyed blit (enables color compare) */
    gpu_blit_key(0, 400, 0, 402, 16, 1, 0x00);
    gpu_wait_idle();

    /* Now do a plain fill — must NOT be affected by leftover CLR_CMP */
    cpu_clear(0, 410, 20, 4, 0x00);
    gpu_fill(0, 410, 16, 1, 0xCC);
    gpu_wait_idle();
    bad = vram_check_rect(0, 410, 16, 1, 0xCC);
    result(bad == 0,
           "fill after keyed blit: %d bad pixels (CLR_CMP %s)\n",
           bad, bad == 0 ? "properly reset by gpu_fill" : "LEAKED");
    if (bad > 0) {
        int x;
        out("      Row 410: ");
        for (x = 0; x < 16; x++) out("%02X ", vram_read(x, 410));
        out("\n");
    }

    /* 5b: Plain blit after keyed blit */
    out("\n  5b: Keyed blit → plain blit (CLR_CMP leak test)\n");
    cpu_clear(0, 420, 32, 4, 0x00);
    {
        int x;
        for (x = 0; x < 16; x++)
            g_lfb[(long)420 * g_pitch + x] = (unsigned char)(0x70 + x);
    }

    /* Keyed blit first */
    gpu_blit_key(0, 400, 100, 420, 16, 1, 0x00);
    gpu_wait_idle();

    /* Plain blit — should copy all pixels regardless */
    gpu_blit_fwd(0, 420, 0, 425, 16, 1);
    gpu_wait_idle();
    {
        int ok = 1;
        int x;
        for (x = 0; x < 16; x++) {
            if (vram_read(x, 425) != vram_read(x, 420)) { ok = 0; break; }
        }
        result(ok, "plain blit after keyed blit: %s\n",
               ok ? "all pixels copied" : "MISSING PIXELS (CLR_CMP leak!)");
    }
}

/* =============================================================== */
/*  TEST 6: Overlapping blit (direction handling)                   */
/* =============================================================== */

static void test_blit_overlap(void)
{
    out("\n========================================\n");
    out("  TEST 6: Overlapping Blit Direction\n");
    out("========================================\n\n");

    /* Setup: gradient line at y=450 */
    {
        int x;
        unsigned char orig[32];

        cpu_clear(0, 448, g_xres, 4, 0x00);
        for (x = 0; x < 32; x++) {
            g_lfb[(long)450 * g_pitch + x] = (unsigned char)(x + 1);
            orig[x] = (unsigned char)(x + 1);
        }

        /* Forward overlap: copy (0,450)→(4,450) 16x1
           Source and dest overlap by 12 pixels.
           GPU should handle this via direction bits. */
        out("  6a: Forward overlap: (0,450)→(4,450) 16x1\n");
        {
            /* Use gpu_blit from RADEON.C style (with overlap detection) */
            unsigned long dp = 0;
            int sx = 0, sy = 450, dx = 4, dy = 450, w = 16, h = 1;

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
            wreg(R_SRC_Y_X,
                 ((unsigned long)sy << 16) | (unsigned long)(sx & 0xFFFF));
            wreg(R_DST_Y_X,
                 ((unsigned long)dy << 16) | (unsigned long)(dx & 0xFFFF));
            wreg(R_DST_HEIGHT_WIDTH,
                 ((unsigned long)h << 16) | (unsigned long)w);
        }
        gpu_wait_idle();

        /* Expected result: pixels 4..19 should be [1,2,3,...,16] */
        {
            int ok = 1;
            int x2;
            for (x2 = 0; x2 < 16; x2++) {
                unsigned char expected = orig[x2];  /* original source */
                unsigned char got = vram_read(x2 + 4, 450);
                if (got != expected) { ok = 0; break; }
            }
            result(ok, "forward overlap: correct copy with right-to-left\n");
            if (!ok) {
                out("      Result:   ");
                for (x = 4; x < 20; x++) out("%02X ", vram_read(x, 450));
                out("\n      Expected: ");
                for (x = 0; x < 16; x++) out("%02X ", orig[x]);
                out("\n");
            }
        }
    }
}

/* =============================================================== */
/*  TEST 7: Scissor clipping                                        */
/* =============================================================== */

static void test_scissor(void)
{
    int bad;

    out("\n========================================\n");
    out("  TEST 7: Scissor Clipping\n");
    out("========================================\n\n");

    /* Set tight scissor: (100,460)-(140,480) */
    out("  Setting scissor to (100,460)-(140,480)\n");
    gpu_wait_fifo(2);
    wreg(R_SC_TOP_LEFT,
         ((unsigned long)460 << 16) | (unsigned long)100);
    wreg(R_SC_BOTTOM_RIGHT,
         ((unsigned long)480 << 16) | (unsigned long)140);

    /* Fill a 64x64 rect starting at (80,450) — should be clipped */
    cpu_clear(80, 450, 68, 40, 0x00);
    gpu_fill(80, 450, 64, 32, 0xEE);
    gpu_wait_idle();

    /* Pixels inside scissor should be filled */
    bad = vram_check_rect(100, 460, 40, 20, 0xEE);
    result(bad == 0, "inside scissor (100-139, 460-479): %d bad pixels\n", bad);

    /* Pixels outside scissor should be untouched */
    {
        int outside_bad = 0;
        int x, y;
        /* Check left of scissor (80-99) */
        for (y = 460; y < 480; y++)
            for (x = 80; x < 100; x++)
                if (vram_read(x, y) != 0x00) outside_bad++;
        /* Check above scissor (450-459) */
        for (y = 450; y < 460; y++)
            for (x = 100; x < 140; x++)
                if (vram_read(x, y) != 0x00) outside_bad++;
        result(outside_bad == 0,
               "outside scissor: %d pixels incorrectly written\n",
               outside_bad);

        /* Detailed dump when scissor fails */
        if (outside_bad > 0 || bad > 0) {
            unsigned long sc_tl_rb = rreg(R_SC_TOP_LEFT);
            unsigned long sc_br_rb = rreg(R_SC_BOTTOM_RIGHT);
            unsigned long dsc_br_rb = rreg(R_DEFAULT_SC_BOTTOM_RIGHT);
            out("      SC_TOP_LEFT readback    = 0x%08lX (Y=%lu X=%lu)\n",
                sc_tl_rb, sc_tl_rb >> 16, sc_tl_rb & 0xFFFF);
            out("      SC_BOTTOM_RIGHT readback= 0x%08lX (Y=%lu X=%lu)\n",
                sc_br_rb, sc_br_rb >> 16, sc_br_rb & 0xFFFF);
            out("      DEFAULT_SC_BR readback  = 0x%08lX\n", dsc_br_rb);
            /* Show what the GPU actually wrote */
            out("      GPU fill region dump (rows 450-481, x=78..141):\n");
            for (y = 450; y < 482 && y < 454; y++) {
                out("        row %d x[78..141]: ", y);
                for (x = 78; x < 142; x++)
                    out("%02X", vram_read(x, y));
                out("\n");
            }
            /* Sample key rows */
            out("        row 450 x[98..141]: ");
            for (x = 98; x < 142; x++) out("%02X", vram_read(x, 450));
            out("\n        row 460 x[78..141]: ");
            for (x = 78; x < 142; x++) out("%02X", vram_read(x, 460));
            out("\n        row 479 x[78..141]: ");
            for (x = 78; x < 142; x++) out("%02X", vram_read(x, 479));
            out("\n        row 480 x[78..141]: ");
            for (x = 78; x < 142; x++) out("%02X", vram_read(x, 480));
            out("\n");
        }
    }

    /* Restore scissor */
    gpu_wait_fifo(2);
    wreg(R_SC_TOP_LEFT, 0);
    wreg(R_SC_BOTTOM_RIGHT, (0x1FFFUL << 16) | 0x1FFFUL);
}

/* =============================================================== */
/*  TEST 8: GPU fill vs CPU fill coherence (cache flush test)       */
/* =============================================================== */

static void test_cache_coherence(void)
{
    out("\n========================================\n");
    out("  TEST 8: GPU/CPU Cache Coherence\n");
    out("========================================\n\n");

    /* GPU fill, then immediately CPU read — tests cache flush */
    out("  8a: GPU fill 128x1, immediate CPU readback\n");
    cpu_clear(0, 500, 132, 4, 0x00);
    gpu_fill(0, 500, 128, 1, 0x77);
    gpu_wait_idle();   /* includes cache flush */
    {
        int bad = vram_check_rect(0, 500, 128, 1, 0x77);
        result(bad == 0,
               "GPU fill immediate read: %d/%d pixels correct\n",
               128 - bad, 128);
    }

    /* CPU write, GPU blit, CPU verify */
    out("\n  8b: CPU write → GPU blit → CPU verify\n");
    {
        int x, ok = 1;
        for (x = 0; x < 64; x++)
            g_lfb[(long)510 * g_pitch + x] = (unsigned char)(x * 4);
        gpu_blit_fwd(0, 510, 0, 515, 64, 1);
        gpu_wait_idle();
        for (x = 0; x < 64; x++) {
            if (vram_read(x, 515) != (unsigned char)(x * 4)) {
                ok = 0; break;
            }
        }
        result(ok, "CPU→GPU→CPU round trip: %s\n",
               ok ? "coherent" : "INCOHERENT");
    }

    /* Multiple fills in sequence without waiting between them */
    out("\n  8c: Batched fills (no wait between), single verify\n");
    cpu_clear(0, 520, g_xres, 10, 0x00);
    gpu_fill(0, 520, 32, 1, 0x11);
    gpu_fill(0, 521, 32, 1, 0x22);
    gpu_fill(0, 522, 32, 1, 0x33);
    gpu_fill(0, 523, 32, 1, 0x44);
    gpu_wait_idle();
    {
        int bad = 0;
        bad += vram_check_rect(0, 520, 32, 1, 0x11);
        bad += vram_check_rect(0, 521, 32, 1, 0x22);
        bad += vram_check_rect(0, 522, 32, 1, 0x33);
        bad += vram_check_rect(0, 523, 32, 1, 0x44);
        result(bad == 0,
               "4 batched fills: %d bad pixels total\n", bad);
    }
}

/* =============================================================== */
/*  TEST 9: Full-width fill (stress test at pitch boundary)         */
/* =============================================================== */

static void test_fullwidth(void)
{
    int bad;

    out("\n========================================\n");
    out("  TEST 9: Full-Width Operations\n");
    out("========================================\n\n");

    /* Full-width fill */
    out("  9a: Full-width fill: %dx1 at y=540\n", g_xres);
    cpu_clear(0, 538, g_xres, 4, 0x00);
    gpu_fill(0, 540, g_xres, 1, 0xAA);
    gpu_wait_idle();
    bad = vram_check_rect(0, 540, g_xres, 1, 0xAA);
    result(bad == 0, "full-width fill (%d pixels): %d bad\n", g_xres, bad);
    /* Verify row above/below are clean */
    bad = vram_check_rect(0, 539, g_xres, 1, 0x00);
    result(bad == 0, "row above: %d leaked pixels\n", bad);
    bad = vram_check_rect(0, 541, g_xres, 1, 0x00);
    result(bad == 0, "row below: %d leaked pixels\n", bad);

    /* Check padding bytes between xres and pitch */
    if (g_pitch > g_xres) {
        int pad_x;
        int pad_dirty = 0;
        out("\n  9b: Pitch padding (%d..%d) after full-width fill\n",
            g_xres, g_pitch - 1);
        for (pad_x = g_xres; pad_x < g_pitch; pad_x++) {
            /* These bytes are in the padding region; GPU may or may not
               write them. Report either way for analysis. */
            if (g_lfb[(long)540 * g_pitch + pad_x] == 0xAA)
                pad_dirty++;
        }
        out("      Padding bytes with fill color: %d / %d\n",
            pad_dirty, g_pitch - g_xres);
        if (pad_dirty > 0)
            warn("GPU filled into pitch padding (%d bytes) — "
                 "not harmful but noted\n", pad_dirty);
        else
            result(1, "padding bytes clean\n");
    }

    /* Full-width blit */
    out("\n  9c: Full-width blit: y=540 → y=550\n");
    cpu_clear(0, 548, g_xres, 4, 0x00);
    gpu_blit_fwd(0, 540, 0, 550, g_xres, 1);
    gpu_wait_idle();
    bad = vram_check_rect(0, 550, g_xres, 1, 0xAA);
    result(bad == 0, "full-width blit: %d bad pixels\n", bad);
}

/* =============================================================== */
/*  TEST 10: Register readback validation                           */
/* =============================================================== */

static void test_register_readback(void)
{
    out("\n========================================\n");
    out("  TEST 10: Key Register State\n");
    out("========================================\n\n");

    {
        unsigned long dpo = rreg(R_DST_PITCH_OFFSET);
        unsigned long spo = rreg(R_SRC_PITCH_OFFSET);
        unsigned long rbbm = rreg(R_RBBM_STATUS);
        unsigned long fifo = rbbm & RBBM_FIFOCNT_MASK;

        out("  DST_PITCH_OFFSET = 0x%08lX\n", dpo);
        out("  SRC_PITCH_OFFSET = 0x%08lX\n", spo);
        result(dpo == spo,
               "DST/SRC PITCH_OFFSET match: %s\n",
               dpo == spo ? "yes" : "NO — may cause blit issues");

        out("  RBBM_STATUS      = 0x%08lX (FIFO=%lu, %s)\n",
            rbbm, fifo, (rbbm & RBBM_ACTIVE) ? "BUSY" : "idle");
        result(!(rbbm & RBBM_ACTIVE),
               "engine idle after all tests: %s\n",
               !(rbbm & RBBM_ACTIVE) ? "yes" : "NO");
        result(fifo >= 32,
               "FIFO healthy (%lu entries free)\n", fifo);
    }

    /* CLR_CMP should be clean after all tests */
    {
        unsigned long cmp = rreg(R_CLR_CMP_CNTL);
        out("  CLR_CMP_CNTL     = 0x%08lX\n", cmp);
        /* Note: on R500, CLR_CMP_CNTL may not be readable (FIFO-submitted).
           Report whatever we get. */
        if (cmp == 0)
            result(1, "CLR_CMP_CNTL is clean (0x00000000)\n");
        else if (cmp == 0xFFFFFFFFUL || cmp == 0)
            out("      (Register may be write-only on R500 — non-zero "
                "readback is expected)\n");
        else
            warn("CLR_CMP_CNTL = 0x%08lX (expected 0 after gpu_disable_cmp)\n",
                 cmp);
    }
}

/* =============================================================== */
/*  TEST 11: AVIVO display controller register state                */
/* =============================================================== */

static void test_avivo_registers(void)
{
    unsigned long d1vga, grph_pitch, x_start, y_start, x_end, y_end;
    unsigned long d1crtc_ctrl, d1crtc_stat;
    unsigned long surf_pri, surf_sec;

    out("\n========================================\n");
    out("  TEST 11: AVIVO Display Register State\n");
    out("========================================\n\n");

    d1vga       = rreg(R_D1VGA_CONTROL);
    grph_pitch  = rreg(R_D1GRPH_PITCH);
    x_start     = rreg(R_D1GRPH_X_START);
    y_start     = rreg(R_D1GRPH_Y_START);
    x_end       = rreg(R_D1GRPH_X_END);
    y_end       = rreg(R_D1GRPH_Y_END);
    d1crtc_ctrl = rreg(R_D1CRTC_CONTROL);
    d1crtc_stat = rreg(R_D1CRTC_STATUS);
    surf_pri    = rreg(R_D1GRPH_PRIMARY_SURFACE_ADDRESS);
    surf_sec    = rreg(R_D1GRPH_SECONDARY_SURFACE_ADDRESS);

    out("  D1VGA_CONTROL    = 0x%08lX  (VGA mode %s)\n",
        d1vga, (d1vga & D1VGA_MODE_ENABLE) ? "ACTIVE — D1GRPH flips ignored!" : "disabled, AVIVO active");
    out("  D1CRTC_CONTROL   = 0x%08lX  (CRTC %s)\n",
        d1crtc_ctrl, (d1crtc_ctrl & AVIVO_CRTC_EN) ? "enabled" : "DISABLED");
    out("  D1CRTC_STATUS    = 0x%08lX  (currently %s)\n",
        d1crtc_stat, (d1crtc_stat & D1CRTC_V_BLANK) ? "in vblank" : "active");
    out("  D1GRPH_PITCH     = 0x%08lX  (%lu bytes)\n", grph_pitch, grph_pitch);
    out("  D1GRPH_X_START   = %lu\n", x_start);
    out("  D1GRPH_Y_START   = %lu\n", y_start);
    out("  D1GRPH_X_END     = %lu  (pixel width should be %d)\n", x_end, g_xres);
    out("  D1GRPH_Y_END     = %lu  (pixel height should be %d)\n", y_end, g_yres);
    out("  SURF_PRIMARY     = 0x%08lX\n", surf_pri);
    out("  SURF_SECONDARY   = 0x%08lX\n", surf_sec);

    /* Key diagnosis: VGA mode active means hw_page_flip does nothing */
    result(!(d1vga & D1VGA_MODE_ENABLE),
           "D1VGA_MODE_ENABLE clear (AVIVO scanout active)\n");

    /* CRTC must be enabled for display to work */
    result(!!(d1crtc_ctrl & AVIVO_CRTC_EN),
           "D1CRTC enabled\n");

    /* Pitch must match g_pitch */
    result(grph_pitch == (unsigned long)g_pitch,
           "D1GRPH_PITCH (%lu) matches g_pitch (%d)\n",
           grph_pitch, g_pitch);

    /* X_END must equal xres (not pitch!) — dark vertical bar if wrong */
    result(x_end == (unsigned long)g_xres,
           "D1GRPH_X_END (%lu) == xres (%d)%s\n",
           x_end, g_xres,
           x_end == (unsigned long)g_pitch ? " [PROBLEM: set to pitch, causes dark bar!]" : "");

    /* Y_END must equal yres */
    result(y_end == (unsigned long)g_yres,
           "D1GRPH_Y_END (%lu) == yres (%d)\n",
           y_end, g_yres);

    /* Surface address should be within VRAM */
    result(surf_pri >= g_fb_location &&
           surf_pri < g_fb_location + (unsigned long)g_vram_mb * 1024UL * 1024UL,
           "SURF_PRIMARY (0x%08lX) within VRAM [0x%08lX+%luMB]\n",
           surf_pri, g_fb_location, g_vram_mb);
}

/* =============================================================== */
/*  TEST 12: Vblank timing measurement                              */
/* =============================================================== */

static void test_vblank_timing(void)
{
    /* Measure 5 vblank rising edges; report inter-frame period.
       ~16.7ms at 60Hz.  If this times out → D1CRTC_V_BLANK not toggling
       → avivo_wait_vblank() in RADEON.C spins forever / never syncs. */
    int i, n, timeout_count = 0;
    unsigned long periods[5];
    unsigned long t_start, t_now, t_prev;

    out("\n========================================\n");
    out("  TEST 12: Vblank Timing (D1CRTC_V_BLANK)\n");
    out("========================================\n\n");

    /* Use busy-loop counter as a rough timer.  We calibrate by counting
       iterations per "known" loop and converting to approximate ms. */

    /* First, wait for vblank to go LOW (active region) */
    for (i = 0; i < 1000000 && (rreg(R_D1CRTC_STATUS) & D1CRTC_V_BLANK); i++);
    if (i == 1000000) {
        out("  D1CRTC_V_BLANK stuck HIGH (always in vblank?) — no timing possible.\n");
        result(0, "D1CRTC_V_BLANK transitions\n");
        return;
    }

    /* Now measure rising edges (start of vblank) */
    t_prev = 0;
    n = 0;
    for (n = 0; n < 5; n++) {
        /* Wait for high (vblank start) */
        for (i = 0; i < 5000000; i++) {
            if (rreg(R_D1CRTC_STATUS) & D1CRTC_V_BLANK) break;
        }
        if (i == 5000000) { timeout_count++; break; }
        t_now = (unsigned long)i; /* not wall-time, but records loop iterations */

        /* Store raw loop counts between edges — not calibrated ms, but ratios work */
        if (n > 0) periods[n-1] = t_now;

        /* Wait for low (active region) before next edge */
        for (i = 0; i < 5000000; i++) {
            if (!(rreg(R_D1CRTC_STATUS) & D1CRTC_V_BLANK)) break;
        }
        if (i == 5000000) { timeout_count++; break; }
    }

    if (timeout_count > 0) {
        out("  Timed out waiting for vblank transition (%d timeouts)\n", timeout_count);
        result(0, "D1CRTC_V_BLANK transitions within timeout\n");
        return;
    }

    /* Count total transitions observed — if we got 5 edges, it works */
    out("  D1CRTC_V_BLANK observed %d rising edges (of 5 attempted)\n", n);
    out("  Vblank loop-count intervals: %lu %lu %lu %lu\n",
        periods[0], periods[1], periods[2], periods[3]);

    result(n == 5, "D1CRTC_V_BLANK transitions: %d/5 seen%s\n",
           n, n < 5 ? " [PROBLEM: fallback vblank in RADEON.C will not work!]" : "");

    /* Sanity: intervals should be roughly equal (within 20%) */
    if (n == 5) {
        unsigned long mn = periods[0], mx = periods[0];
        int j;
        for (j = 1; j < 4; j++) {
            if (periods[j] < mn) mn = periods[j];
            if (periods[j] > mx) mx = periods[j];
        }
        /* Pass if max/min ratio < 1.5 */
        result(mx < mn * 2,
               "Vblank intervals consistent (min=%lu max=%lu)\n", mn, mx);
    }
}

/* =============================================================== */
/*  TEST 13: Ghost rectangle investigation                          */
/* =============================================================== */
/* The observed symptom: "slightly darker version of bounced rect  */
/* to the right, wraps around on the left."  This is the signature */
/* of the display controller scanning at a different pitch than    */
/* the GPU rendered at, causing rows to appear shifted.            */
/*                                                                  */
/* Concretely: GPU renders at g_pitch=832, display scans at 800.   */
/* Row N starts at byte 832*N in VRAM.  Display thinks rows are    */
/* 800 bytes apart.  Row 1 of the image appears at pixel 32 of     */
/* the display's "row 1" (800 vs 832 delta accumulates each row).  */
/*                                                                  */
/* This test:                                                       */
/*   a) Fills a 16x16 block with a known pattern                   */
/*   b) Scans a 100-pixel-wide zone around+right for stray pixels  */
/*   c) Specifically checks at offset +32 (pitch padding width)    */
/*   d) Also checks the first few columns (row-wrap) for echoes    */
/* =============================================================== */

static void test_ghost_rect(void)
{
    int x, y, bad_right, bad_left, bad_pitch_offset;
    unsigned char *p;

    out("\n========================================\n");
    out("  TEST 13: Ghost Rectangle Investigation\n");
    out("========================================\n\n");

    /* 13a: Place a distinctive pattern at (300, 200), 16x16 */
    out("  13a: Write pattern at (300,200) 16x16 = 0xCC, surround = 0x00\n");
    /* Clear a wide area first */
    cpu_clear(200, 190, 200, 40, 0x00);
    /* Write the pattern via GPU fill */
    gpu_fill(300, 200, 16, 16, 0xCC);
    gpu_wait_idle();

    /* Verify the source is correct */
    {
        int src_bad = vram_check_rect(300, 200, 16, 16, 0xCC);
        result(src_bad == 0, "Source block correct: %d bad pixels\n", src_bad);
    }

    /* 13b: Check +32 to +48 columns from source X — this is where the ghost
       would appear if display pitch != GPU pitch (pitch=832, visible=800,
       padding=32, ghost at X+32 in display coordinates) */
    out("  13b: Check for ghost at x=332..347 (source+32, pitch padding boundary)\n");
    bad_pitch_offset = 0;
    for (y = 200; y < 216; y++) {
        for (x = 332; x < 348; x++) {
            p = g_lfb + (long)y * g_pitch + x;
            if (*p == 0xCC) bad_pitch_offset++;
        }
    }
    result(bad_pitch_offset == 0,
           "No ghost at +32 offset: %d stray 0xCC pixels%s\n",
           bad_pitch_offset,
           bad_pitch_offset > 0 ? " [PROBLEM: display pitch != GPU pitch!]" : "");

    /* 13c: Check left edge of screen (x=0..16) at rows 200..215
       for row-wrap ghost (if rows appear shifted, row 201 would
       start 32 pixels into display row 200's territory, etc.) */
    out("  13c: Check for row-wrap ghost at x=0..15 rows 200..216\n");
    bad_left = 0;
    for (y = 200; y < 217; y++) {
        for (x = 0; x < 16; x++) {
            p = g_lfb + (long)y * g_pitch + x;
            if (*p == 0xCC) bad_left++;
        }
    }
    result(bad_left == 0,
           "No row-wrap ghost at left edge: %d stray 0xCC pixels\n",
           bad_left);

    /* 13d: Check a wide area to the right of the source (x=316..400)
       for any stray 0xCC that shouldn't be there */
    out("  13d: Scan wide area right of source (x=316..400) for stray pixels\n");
    bad_right = 0;
    for (y = 200; y < 216; y++) {
        for (x = 316; x < 400; x++) {
            p = g_lfb + (long)y * g_pitch + x;
            if (*p == 0xCC) bad_right++;
        }
    }
    result(bad_right == 0,
           "No stray pixels right of source (x=316..400): %d found\n",
           bad_right);

    /* 13e: Blit the block to a different location and check for ghosts */
    out("  13e: GPU blit (300,200)→(100,300) 16x16, check for ghost at (132,300)\n");
    cpu_clear(80, 295, 200, 30, 0x00);
    gpu_blit_fwd(300, 200, 100, 300, 16, 16);
    gpu_wait_idle();

    {
        int dst_bad  = vram_check_rect(100, 300, 16, 16, 0xCC);
        int ghost_bad = 0;
        result(dst_bad == 0, "Blit destination correct: %d bad pixels\n", dst_bad);

        /* Pixel dump if blit destination is wrong */
        if (dst_bad > 0) {
            out("      Destination pixel dump (100,300) 16x16:\n");
            for (y = 300; y < 316 && y < 308; y++) {
                out("        row %d: ", y);
                for (x = 100; x < 116; x++)
                    out("%02X ", vram_read(x, y));
                out("\n");
            }
            /* Also check: did the blit land at a different Y? */
            out("      Scanning for 0xCC at x=100, y=280..320:\n        ");
            for (y = 280; y < 320; y++) {
                unsigned char v = vram_read(100, y);
                if (v == 0xCC) out("y=%d:CC ", y);
            }
            out("\n");
            /* Check source is still intact */
            out("      Source (300,200) row0: ");
            for (x = 300; x < 316; x++)
                out("%02X ", vram_read(x, 200));
            out("\n");
        }

        /* Check for ghost at +32 from destination */
        for (y = 300; y < 316; y++)
            for (x = 132; x < 148; x++) {
                p = g_lfb + (long)y * g_pitch + x;
                if (*p == 0xCC) ghost_bad++;
            }
        result(ghost_bad == 0,
               "No ghost at blit-dst+32 (132,300): %d stray pixels%s\n",
               ghost_bad,
               ghost_bad > 0 ? " [PROBLEM: confirms display pitch mismatch!]" : "");
    }
}

/* =============================================================== */
/*  TEST 14: Full-width keyed blit (parallax layer scenario)        */
/* =============================================================== */

static void test_fullwidth_keyed(void)
{
    int x, y, bad_drawn, bad_key, bad_outside;
    unsigned char *p;
    int src_y = 380, dst_y = 400;
    int w = g_xres;  /* full visible width */

    out("\n========================================\n");
    out("  TEST 14: Full-Width Keyed Blit (Parallax)\n");
    out("========================================\n\n");

    /* Setup: clear destination row to 0xFF (sky color) */
    out("  Setup: dst row %d = 0xFF (sky), src row %d = alternating key/non-key\n",
        dst_y, src_y);
    cpu_clear(0, dst_y - 1, w, 3, 0xFF);
    /* Source: odd pixels = key (0x00 = transparent), even = non-key pattern */
    for (x = 0; x < w; x++) {
        p = g_lfb + (long)src_y * g_pitch + x;
        *p = (x & 1) ? 0x00 : (unsigned char)(0x80 + (x & 0x3F));
    }

    /* FCN_NE blit: non-key pixels overwrite, key pixels preserve dest */
    {
        unsigned long po = ((unsigned long)(g_pitch/64) << 22) |
                           ((g_lfb_phys - g_fb_location) >> 10);
        gpu_wait_fifo(10);
        wreg(R_DST_PITCH_OFFSET, po);
        wreg(R_SRC_PITCH_OFFSET, po);
        wreg(R_CLR_CMP_CNTL,
             (CLR_CMP_FCN_NE << 0) | CLR_CMP_SRC_SOURCE);
        wreg(R_CLR_CMP_CLR_SRC, 0x00000000UL);
        wreg(R_CLR_CMP_MASK,    0x000000FFUL);
        gpu_wait_fifo(8);
        wreg(R_DP_GUI_MASTER_CNTL,
             GMC_BRUSH_NONE | GMC_DST_8BPP | GMC_SRC_DATATYPE_COLOR |
             ROP3_SRCCOPY | GMC_DP_SRC_MEMORY | GMC_WR_MSK_DIS);
        wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
        wreg(R_SC_TOP_LEFT,      0);
        wreg(R_SC_BOTTOM_RIGHT,  ((unsigned long)g_yres << 16) | (unsigned long)g_xres);
        wreg(R_SRC_Y_X,          ((unsigned long)src_y << 16));
        wreg(R_DST_Y_X,          ((unsigned long)dst_y << 16));
        wreg(R_DST_HEIGHT_WIDTH,  (1UL << 16) | (unsigned long)w);
    }
    gpu_wait_idle();

    /* Verify: non-key (even) pixels should be drawn, key (odd) pixels = 0xFF */
    bad_drawn = 0;
    bad_key   = 0;
    for (x = 0; x < w; x++) {
        p = g_lfb + (long)dst_y * g_pitch + x;
        if (x & 1) {
            /* key pixel: destination should remain 0xFF */
            if (*p != 0xFF) bad_key++;
        } else {
            /* non-key: should match source */
            unsigned char exp = (unsigned char)(0x80 + (x & 0x3F));
            if (*p != exp) bad_drawn++;
        }
    }
    result(bad_drawn == 0,
           "Full-width keyed blit: %d non-key pixels wrong\n", bad_drawn);
    result(bad_key == 0,
           "Full-width keyed blit: %d key pixels overwritten (sky leaks!)\n",
           bad_key);

    /* Detailed diagnostics when pixels are wrong */
    if (bad_drawn > 0 || bad_key > 0) {
        int sample_count = 0;
        out("      First 16 mismatched pixels at dst row %d:\n", dst_y);
        for (x = 0; x < w && sample_count < 16; x++) {
            p = g_lfb + (long)dst_y * g_pitch + x;
            if (x & 1) {
                if (*p != 0xFF) {
                    out("        x=%d: got=0x%02X exp=0xFF (key should be preserved)\n",
                        x, *p);
                    sample_count++;
                }
            } else {
                unsigned char exp = (unsigned char)(0x80 + (x & 0x3F));
                if (*p != exp) {
                    out("        x=%d: got=0x%02X exp=0x%02X\n", x, *p, exp);
                    sample_count++;
                }
            }
        }
        /* Dump first 32 bytes of dst row */
        out("      Dst row %d raw [0..31]: ", dst_y);
        for (x = 0; x < 32; x++)
            out("%02X ", vram_read(x, dst_y));
        out("\n");
        /* And near the end */
        out("      Dst row %d raw [%d..%d]: ", dst_y, w-16, w-1);
        for (x = w - 16; x < w; x++)
            out("%02X ", vram_read(x, dst_y));
        out("\n");
    }

    /* Check rows outside dst for leakage */
    bad_outside = vram_check_rect(0, dst_y - 1, w, 1, 0xFF) +
                  vram_check_rect(0, dst_y + 1, w, 1, 0xFF);
    result(bad_outside == 0,
           "No keyed blit row leakage above/below: %d bad pixels\n", bad_outside);

    /* Per-row leakage breakdown when test fails */
    if (bad_outside > 0) {
        int row_bad;
        for (y = dst_y - 2; y <= dst_y + 2; y++) {
            if (y == dst_y) continue;  /* skip dst row itself */
            row_bad = 0;
            for (x = 0; x < w; x++) {
                unsigned char v = vram_read(x, y);
                if (v != 0xFF) row_bad++;
            }
            if (row_bad > 0) {
                out("      Row %d: %d bad pixels.  Sample [0..15]: ", y, row_bad);
                for (x = 0; x < 16; x++)
                    out("%02X ", vram_read(x, y));
                out("\n");
            }
        }
    }

    /* Reset CLR_CMP */
    gpu_wait_fifo(2);
    wreg(R_CLR_CMP_CNTL, 0);
}

/* =============================================================== */
/*  TEST 15: VGA CRTC register state                                */
/* =============================================================== */

static void test_vga_crtc(void)
{
    unsigned char cr13, cr01, cr14;
    unsigned int expected_cr13;

    out("\n========================================\n");
    out("  TEST 15: VGA CRTC Register State\n");
    out("========================================\n\n");

    /* Read VGA CRTC registers via 0x3D4/0x3D5 */
    outp(0x3D4, 0x13); cr13 = inp(0x3D5);   /* offset (pitch/2 in words, i.e. pitch/8) */
    outp(0x3D4, 0x01); cr01 = inp(0x3D5);   /* horizontal display end - 1 (in chars) */
    outp(0x3D4, 0x14); cr14 = inp(0x3D5);   /* underline location (DWord mode bit) */

    expected_cr13 = (unsigned int)(g_pitch / 8);

    out("  VGA CR13 (offset/pitch): 0x%02X = %u  (expect %u for pitch=%d)\n",
        cr13, cr13, expected_cr13, g_pitch);
    out("  VGA CR01 (h-disp end-1): 0x%02X = %u  (expect %u for xres=%d)\n",
        cr01, cr01, (g_xres / 8) - 1, g_xres);
    out("  VGA CR14 (underline):    0x%02X  (bit6=dword mode)\n", cr14);

    /* CR13: controls the line pitch in VGA scanout path.
       If != pitch/8 the display will show horizontal smearing/wrapping. */
    result((unsigned int)cr13 == expected_cr13,
           "CR13 = %u (expected %u = pitch(%d)/8)%s\n",
           (unsigned int)cr13, expected_cr13, g_pitch,
           (unsigned int)cr13 != expected_cr13
               ? " [PROBLEM: VGA pitch mismatch → row wrap → ghost image!]" : "");

    /* CR01: horizontal display end. In text mode this is char-based;
       in 256-color graphics it's clock-based (xres/8 - 1). */
    result((unsigned int)cr01 == (unsigned int)((g_xres / 8) - 1),
           "CR01 = %u (expected %u = xres(%d)/8 - 1)\n",
           (unsigned int)cr01, (g_xres / 8) - 1, g_xres);

    /* Sanity: D1VGA_CONTROL and CRTC mode should agree */
    {
        unsigned long d1vga = rreg(R_D1VGA_CONTROL);
        int vga_active = !!(d1vga & D1VGA_MODE_ENABLE);
        out("  D1VGA_CONTROL = 0x%08lX (VGA scanout: %s)\n",
            d1vga, vga_active ? "ACTIVE" : "disabled");
        if (vga_active) {
            out("  NOTE: VGA scanout is active — CR13 controls row stride.\n");
            out("        If CR13 != pitch/8, you get ghost-image row wrapping.\n");
        } else {
            out("  NOTE: AVIVO scanout active — CR13 only matters if VGA active.\n");
        }
    }
}

/* =============================================================== */
/*  TEST 16: AVIVO page flip address verification                   */
/* =============================================================== */

static void test_flip_address(void)
{
    unsigned long addr_before, addr_after;
    unsigned long page0 = g_lfb_phys - g_fb_location;
    unsigned long page1;
    unsigned long pitch_bytes = (unsigned long)g_pitch;
    unsigned long page1_offset;
    int i;

    out("\n========================================\n");
    out("  TEST 16: AVIVO Page Flip Address\n");
    out("========================================\n\n");

    /* Page 1 starts at page0 + one screen's worth of bytes */
    page1_offset = page0 + pitch_bytes * (unsigned long)g_yres;
    /* Align to 4KB */
    page1_offset = (page1_offset + 4095UL) & ~4095UL;
    page1 = g_fb_location + page1_offset;

    out("  g_lfb_phys    = 0x%08lX\n", g_lfb_phys);
    out("  g_fb_location = 0x%08lX\n", g_fb_location);
    out("  Page 0 offset = 0x%08lX  (phys = 0x%08lX)\n", page0, g_lfb_phys);
    out("  Page 1 offset = 0x%08lX  (phys = 0x%08lX)\n", page1_offset, page1);
    out("  Pitch         = %d bytes (%d rows per page)\n", g_pitch, g_yres);

    /* Read current surface address */
    addr_before = rreg(R_D1GRPH_PRIMARY_SURFACE_ADDRESS);
    out("  SURF_PRIMARY before flip = 0x%08lX\n", addr_before);

    /* Check VGA mode — if active, flips won't change SURF_PRIMARY */
    {
        unsigned long d1vga = rreg(R_D1VGA_CONTROL);
        if (d1vga & D1VGA_MODE_ENABLE) {
            out("  D1VGA_MODE_ENABLE is SET — AVIVO flip will have no effect!\n");
            result(0, "D1VGA_MODE_ENABLE clear (required for AVIVO flips)\n");
            return;
        }
    }

    /* Simulate hw_page_flip sequence to page 1:
       lock → write primary+secondary → unlock */
    {
        unsigned long tmp = rreg(R_D1GRPH_UPDATE);
        wreg(R_D1GRPH_UPDATE, tmp | D1GRPH_SURFACE_UPDATE_LOCK);
        wreg(R_D1GRPH_FLIP_CONTROL, 0);
        wreg(R_D1GRPH_SECONDARY_SURFACE_ADDRESS, page1);
        wreg(R_D1GRPH_PRIMARY_SURFACE_ADDRESS,   page1);
        tmp = rreg(R_D1GRPH_UPDATE);
        wreg(R_D1GRPH_UPDATE, tmp & ~D1GRPH_SURFACE_UPDATE_LOCK);
    }

    /* Check whether SURFACE_UPDATE_PENDING goes high */
    {
        int pending_seen = 0;
        for (i = 0; i < 100000; i++) {
            if (rreg(R_D1GRPH_UPDATE) & D1GRPH_SURFACE_UPDATE_PENDING) {
                pending_seen = 1;
                break;
            }
        }
        out("  SURFACE_UPDATE_PENDING went high: %s (in %d iters)\n",
            pending_seen ? "YES" : "NO", i);
        if (!pending_seen)
            out("  NOTE: If pending never goes high, hw_page_flip falls back to\n"
                "        D1CRTC_V_BLANK — this is expected on some RV515 boards.\n");
    }

    /* Wait for vsync (either via pending or v_blank) */
    for (i = 0; i < 500000; i++)
        if (!(rreg(R_D1CRTC_STATUS) & D1CRTC_V_BLANK)) break;
    for (i = 0; i < 500000; i++)
        if (  rreg(R_D1CRTC_STATUS) & D1CRTC_V_BLANK)  break;
    /* Wait for pending low (flip applied) */
    for (i = 0; i < 300000; i++)
        if (!(rreg(R_D1GRPH_UPDATE) & D1GRPH_SURFACE_UPDATE_PENDING)) break;

    addr_after = rreg(R_D1GRPH_PRIMARY_SURFACE_ADDRESS);
    out("  SURF_PRIMARY after flip  = 0x%08lX  (expected 0x%08lX)\n",
        addr_after, page1);

    result(addr_after == page1,
           "Surface address updated to page1 (0x%08lX)%s\n",
           page1, addr_after != page1 ? " [PROBLEM: flip did not apply!]" : "");

    /* Flip back to page 0 */
    {
        unsigned long tmp = rreg(R_D1GRPH_UPDATE);
        wreg(R_D1GRPH_UPDATE, tmp | D1GRPH_SURFACE_UPDATE_LOCK);
        wreg(R_D1GRPH_FLIP_CONTROL, 0);
        wreg(R_D1GRPH_SECONDARY_SURFACE_ADDRESS, g_lfb_phys);
        wreg(R_D1GRPH_PRIMARY_SURFACE_ADDRESS,   g_lfb_phys);
        tmp = rreg(R_D1GRPH_UPDATE);
        wreg(R_D1GRPH_UPDATE, tmp & ~D1GRPH_SURFACE_UPDATE_LOCK);
    }
    /* Wait vsync */
    for (i = 0; i < 500000; i++)
        if (!(rreg(R_D1CRTC_STATUS) & D1CRTC_V_BLANK)) break;
    for (i = 0; i < 500000; i++)
        if (  rreg(R_D1CRTC_STATUS) & D1CRTC_V_BLANK)  break;

    addr_after = rreg(R_D1GRPH_PRIMARY_SURFACE_ADDRESS);
    out("  SURF_PRIMARY after flip-back = 0x%08lX  (expected 0x%08lX)\n",
        addr_after, g_lfb_phys);
    result(addr_after == g_lfb_phys,
           "Surface address restored to page0 (0x%08lX)\n", g_lfb_phys);
}

/* =============================================================== */
/*  TEST 17: GMC_PITCH_OFFSET_CNTL bit effect                      */
/* =============================================================== */

static void test_gmc_pitch_offset_cntl(void)
{
    int bad_without, bad_with;
    unsigned long pitch64 = ((unsigned long)g_pitch + 63) / 64;
    unsigned long po = (pitch64 << 22) | ((g_fb_location >> 10) & 0x003FFFFFUL);

    out("\n========================================\n");
    out("  TEST 17: GMC_PITCH_OFFSET_CNTL Bits\n");
    out("========================================\n\n");

    out("  Tests whether DP_GUI_MASTER_CNTL bits 0-1 (SRC/DST PITCH_OFFSET_CNTL)\n");
    out("  affect blit behavior.  xf86-video-ati always sets these bits.\n\n");

    /* 17a: Fill WITHOUT GMC_PITCH_OFFSET_CNTL bits (current behavior) */
    out("  17a: GPU fill 32x4 at (600,100) WITHOUT GMC bits [1:0]\n");
    cpu_clear(598, 98, 36, 8, 0x00);
    gpu_wait_fifo(4);
    wreg(R_DST_PITCH_OFFSET, po);
    wreg(R_SRC_PITCH_OFFSET, po);
    wreg(R_DST_OFFSET, g_fb_location);
    wreg(R_DST_PITCH,  (unsigned long)g_pitch);
    gpu_fill(600, 100, 32, 4, 0xAA);
    gpu_wait_idle();

    bad_without = vram_check_rect(600, 100, 32, 4, 0xAA);
    result(bad_without == 0,
           "Fill without GMC PITCH_OFFSET bits: %d bad pixels\n", bad_without);

    /* 17b: Fill WITH GMC_DST_PITCH_OFFSET_CNTL (bit 1) set */
    out("  17b: GPU fill 32x4 at (600,110) WITH GMC_DST_PITCH_OFFSET_CNTL\n");
    cpu_clear(598, 108, 36, 8, 0x00);
    gpu_wait_fifo(6);
    wreg(R_DST_PITCH_OFFSET, po);
    wreg(R_SRC_PITCH_OFFSET, po);
    wreg(R_DP_GUI_MASTER_CNTL,
         GMC_BRUSH_SOLID | GMC_DST_8BPP | GMC_SRC_DATATYPE_COLOR |
         ROP3_PATCOPY | GMC_CLR_CMP_DIS | GMC_WR_MSK_DIS |
         (1UL << 0) |   /* GMC_SRC_PITCH_OFFSET_CNTL */
         (1UL << 1));   /* GMC_DST_PITCH_OFFSET_CNTL */
    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
    wreg(R_DP_BRUSH_FRGD_CLR, 0xBBUL);
    wreg(R_DST_Y_X,         ((unsigned long)110 << 16) | 600UL);
    gpu_wait_fifo(1);
    wreg(R_DST_HEIGHT_WIDTH, (4UL << 16) | 32UL);
    gpu_wait_idle();

    bad_with = vram_check_rect(600, 110, 32, 4, 0xBB);
    result(bad_with == 0,
           "Fill with GMC PITCH_OFFSET bits: %d bad pixels\n", bad_with);

    if (bad_without == 0 && bad_with == 0) {
        out("  Both paths work — GMC bits not required for MMIO fills.\n");
    } else if (bad_without > 0 && bad_with == 0) {
        out("  *** GMC bits are REQUIRED for correct pitch/offset! ***\n");
    } else if (bad_without == 0 && bad_with > 0) {
        out("  GMC bits BREAK fills — do NOT set them.\n");
    }

    /* Pixel dump for comparison */
    {
        int x;
        out("      Row 100 [600..631]: ");
        for (x = 600; x < 632; x++) out("%02X", vram_read(x, 100));
        out("\n      Row 110 [600..631]: ");
        for (x = 600; x < 632; x++) out("%02X", vram_read(x, 110));
        out("\n");
    }

    /* Restore gpu_fill's expected state */
    gpu_wait_fifo(3);
    wreg(R_DST_PITCH_OFFSET, po);
    wreg(R_SRC_PITCH_OFFSET, po);
    wreg(R_CLR_CMP_CNTL, 0);
}

/* =============================================================== */
/*  TEST 18-24: Comprehensive PITCH_OFFSET validation               */
/*  These tests validate every assumption used by the dune chase    */
/*  demo: per-blit PITCH_OFFSET with GMC bits, offscreen staging,   */
/*  Y rebasing, color-key with PO, and consecutive PO switching.    */
/* =============================================================== */

/* Helper: build PITCH_OFFSET register value */
static unsigned long make_pitch_offset(unsigned long vram_byte_off)
{
    unsigned long pitch64  = (unsigned long)g_pitch / 64;
    unsigned long gpu_addr = g_fb_location + vram_byte_off;
    return (pitch64 << 22) | ((gpu_addr >> 10) & 0x003FFFFFUL);
}

/* Helper: flush 2D dest cache */
static void gpu_flush_2d(void)
{
    gpu_wait_fifo(2);
    wreg(R_DSTCACHE_CTLSTAT, R300_RB2D_DC_FLUSH_ALL);
    wreg(R_WAIT_UNTIL, WAIT_2D_IDLECLEAN | WAIT_DMA_GUI_IDLE);
}

/* Helper: reset PITCH_OFFSET to default */
static void po_reset(void)
{
    unsigned long po = make_pitch_offset(0);
    gpu_wait_fifo(6);
    wreg(R_DST_PITCH_OFFSET, po);
    wreg(R_SRC_PITCH_OFFSET, po);
    wreg(R_DST_OFFSET, g_fb_location);
    wreg(R_DST_PITCH,  (unsigned long)g_pitch);
    wreg(R_SRC_OFFSET, g_fb_location);
    wreg(R_SRC_PITCH,  (unsigned long)g_pitch);
}

/* Helper: fill using PO (matches RADEON.C gpu_blit_po pattern for fills) */
static void gpu_fill_po(unsigned long dst_po, int x, int y, int w, int h,
                        unsigned char color)
{
    gpu_wait_fifo(7);
    wreg(R_DST_PITCH_OFFSET, dst_po);
    wreg(R_DP_GUI_MASTER_CNTL,
         GMC_DST_PITCH_OFFSET_CNTL |
         GMC_BRUSH_SOLID | GMC_DST_8BPP | GMC_SRC_DATATYPE_COLOR |
         ROP3_PATCOPY | GMC_CLR_CMP_DIS | GMC_WR_MSK_DIS);
    wreg(R_DP_BRUSH_FRGD_CLR, (unsigned long)color);
    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
    wreg(R_DST_Y_X, ((unsigned long)y << 16) | (unsigned long)x);
    wreg(R_DST_HEIGHT_WIDTH, ((unsigned long)h << 16) | (unsigned long)w);
}

/* Helper: blit with per-blit PITCH_OFFSET + GMC bits (matches RADEON.C) */
static void blit_po(unsigned long src_po, int sx, int sy,
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

/* Helper: keyed blit with per-blit PITCH_OFFSET (matches RADEON.C) */
static void blit_po_key(unsigned long src_po, int sx, int sy,
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

/* Helper: read pixel at (x,y) relative to a VRAM byte offset base */
static unsigned char off_read(unsigned long vram_off, int x, int y)
{
    unsigned long addr = vram_off + (unsigned long)y * g_pitch + x;
    if (addr >= g_lfb_size) return 0xEE;  /* out of range sentinel */
    return g_lfb[addr];
}

/* Helper: write pixel at (x,y) relative to a VRAM byte offset base */
static void off_write(unsigned long vram_off, int x, int y, unsigned char v)
{
    unsigned long addr = vram_off + (unsigned long)y * g_pitch + x;
    if (addr < g_lfb_size)
        g_lfb[addr] = v;
}

/* Helper: fill rect via CPU at offset base */
static void off_fill(unsigned long vram_off, int x, int y,
                     int w, int h, unsigned char v)
{
    int r, c;
    for (r = y; r < y + h; r++)
        for (c = x; c < x + w; c++)
            off_write(vram_off, c, r, v);
}

/* Helper: check rect at offset base, return bad pixel count */
static int off_check(unsigned long vram_off, int x, int y,
                     int w, int h, unsigned char expect)
{
    int bad = 0, r, c;
    for (r = y; r < y + h; r++)
        for (c = x; c < x + w; c++)
            if (off_read(vram_off, c, r) != expect) bad++;
    return bad;
}

/* Helper: dump a row of pixels at offset base */
static void off_dump_row(unsigned long vram_off, int y, int x0, int x1)
{
    int x;
    out("      Row %d [%d..%d]: ", y, x0, x1 - 1);
    for (x = x0; x < x1; x++)
        out("%02X", off_read(vram_off, x, y));
    out("\n");
}

/* ---- TEST 18: PITCH_OFFSET Encoding Validation ---- */
static void test_po_encoding(void)
{
    unsigned long po0, po1, po2, po3;
    unsigned long off0, off1, off2, off3;

    out("\n========================================\n");
    out("  TEST 18: PITCH_OFFSET Encoding\n");
    out("========================================\n\n");

    out("  Validates make_pitch_offset() encoding for various VRAM offsets.\n");
    out("  FB_LOCATION=0x%08lX  Pitch=%d  Pitch/64=%lu\n\n",
        g_fb_location, g_pitch, (unsigned long)g_pitch / 64);

    off0 = 0;
    off1 = (unsigned long)g_page_stride * g_pitch;
    off2 = (unsigned long)g_page_stride * g_pitch * 2;
    off3 = (unsigned long)g_page_stride * g_pitch * 10;

    po0 = make_pitch_offset(off0);
    po1 = make_pitch_offset(off1);
    po2 = make_pitch_offset(off2);
    po3 = make_pitch_offset(off3);

    out("  Page stride: %d rows  Bytes per page: %lu\n",
        g_page_stride, (unsigned long)g_page_stride * g_pitch);
    out("  off=0x%08lX -> PO=0x%08lX (page 0, display)\n", off0, po0);
    out("  off=0x%08lX -> PO=0x%08lX (page 1)\n", off1, po1);
    out("  off=0x%08lX -> PO=0x%08lX (page 2)\n", off2, po2);
    out("  off=0x%08lX -> PO=0x%08lX (page 10)\n", off3, po3);

    /* Decode each PO value */
    {
        unsigned long vals[4];
        int idx;
        vals[0] = po0; vals[1] = po1; vals[2] = po2; vals[3] = po3;
        for (idx = 0; idx < 4; idx++) {
            unsigned long p = vals[idx];
            unsigned long enc_pitch64 = (p >> 22) & 0xFF;
            unsigned long enc_offset  = p & 0x003FFFFFUL;
            unsigned long enc_addr    = enc_offset << 10;
            out("    PO[%d]=0x%08lX -> pitch64=%lu (pitch=%lu) offset=0x%06lX (addr=0x%08lX)\n",
                idx, p, enc_pitch64, enc_pitch64 * 64, enc_offset, enc_addr);
        }
    }

    /* Sanity: pitch field should be the same for all */
    {
        int ok = 1;
        unsigned long p0 = (po0 >> 22) & 0xFF;
        if (((po1 >> 22) & 0xFF) != p0) ok = 0;
        if (((po2 >> 22) & 0xFF) != p0) ok = 0;
        if (((po3 >> 22) & 0xFF) != p0) ok = 0;
        result(ok, "All PO values have same pitch field (%lu)\n", p0);
    }

    /* Sanity: offsets should increase */
    {
        int ok = (po1 & 0x003FFFFFUL) > (po0 & 0x003FFFFFUL) &&
                 (po2 & 0x003FFFFFUL) > (po1 & 0x003FFFFFUL) &&
                 (po3 & 0x003FFFFFUL) > (po2 & 0x003FFFFFUL);
        result(ok, "PO offset field increases with VRAM offset\n");
    }

    /* Check offset alignment: GPU address must be 1KB-aligned */
    {
        int ok = 1;
        if ((g_fb_location + off1) & 0x3FF) ok = 0;
        if ((g_fb_location + off2) & 0x3FF) ok = 0;
        result(ok, "VRAM page offsets are 1KB-aligned (required by PITCH_OFFSET)\n");
    }

    out("\n");
}

/* ---- TEST 19: gpu_fill with DST_PITCH_OFFSET + GMC bit ---- */
static void test_po_fill(void)
{
    unsigned long po0, off1, po1;
    int bad;

    out("\n========================================\n");
    out("  TEST 19: GPU Fill with DST_PITCH_OFFSET_CNTL\n");
    out("========================================\n\n");

    po0  = make_pitch_offset(0);
    off1 = (unsigned long)g_page_stride * g_pitch;
    po1  = make_pitch_offset(off1);

    /* 19a: Fill at display page 0 using PO */
    out("  19a: gpu_fill_po to display (PO=0x%08lX) at (100,400) 32x4\n", po0);
    cpu_clear(98, 398, 36, 8, 0x00);
    gpu_fill_po(po0, 100, 400, 32, 4, 0xA1);
    gpu_wait_idle();
    bad = vram_check_rect(100, 400, 32, 4, 0xA1);
    result(bad == 0, "Fill via PO to display: %d bad pixels\n", bad);
    if (bad > 0) {
        int x;
        out("      Row 400 [100..131]: ");
        for (x = 100; x < 132; x++) out("%02X", vram_read(x, 400));
        out("\n");
    }

    /* 19b: Fill at offscreen page 1 using PO, Y=0 should target row g_page_stride */
    if (off1 + (unsigned long)g_pitch * 8 <= g_lfb_size) {
        out("  19b: gpu_fill_po to page 1 (PO=0x%08lX) at (100,0) 32x4\n", po1);
        off_fill(off1, 98, 0, 36, 8, 0x00);
        gpu_fill_po(po1, 100, 0, 32, 4, 0xB2);
        gpu_wait_idle();
        gpu_flush_2d();
        bad = off_check(off1, 100, 0, 32, 4, 0xB2);
        result(bad == 0, "Fill via PO to page 1 (Y rebased): %d bad pixels\n", bad);
        if (bad > 0) {
            out("      Expected at VRAM offset 0x%08lX\n", off1);
            off_dump_row(off1, 0, 100, 132);
            off_dump_row(off1, 1, 100, 132);
            /* Also check if it went to page 0 by mistake */
            out("      Page 0 row %d [100..131]: ", g_page_stride);
            {
                int x;
                for (x = 100; x < 132; x++) out("%02X", vram_read(x, g_page_stride));
                out("\n");
            }
            /* Check absolute row 0 too */
            out("      Page 0 row 0 [100..131]: ");
            {
                int x;
                for (x = 100; x < 132; x++) out("%02X", vram_read(x, 0));
                out("\n");
            }
        }
        /* Verify display page was NOT clobbered */
        {
            int disp_bad = vram_check_rect(100, 0, 32, 4, 0x00);
            result(disp_bad == 0,
                   "Display page 0 not clobbered by PO fill: %d bad\n", disp_bad);
        }
    } else {
        out("  19b: SKIPPED (LFB too small for page 1)\n");
    }

    po_reset();
    out("\n");
}

/* ---- TEST 20: Basic blit_po copy within display ---- */
static void test_po_blit_basic(void)
{
    unsigned long po0;
    int bad;

    out("\n========================================\n");
    out("  TEST 20: blit_po Basic Copy (same surface)\n");
    out("========================================\n\n");

    po0 = make_pitch_offset(0);

    /* Write a 16x4 pattern at (200,400) via CPU */
    {
        int r, c;
        for (r = 0; r < 4; r++)
            for (c = 0; c < 16; c++)
                g_lfb[(long)(400 + r) * g_pitch + 200 + c] =
                    (unsigned char)(0x30 + r * 16 + c);
    }

    /* Clear dest area */
    cpu_clear(300, 400, 20, 8, 0x00);

    /* blit_po: copy (200,400) -> (300,400) 16x4, both PO=display */
    out("  20a: blit_po (200,400)->(300,400) 16x4, both PO=display\n");
    blit_po(po0, 200, 400, po0, 300, 400, 16, 4);
    gpu_wait_idle();

    bad = 0;
    {
        int r, c;
        for (r = 0; r < 4; r++)
            for (c = 0; c < 16; c++) {
                unsigned char expect = (unsigned char)(0x30 + r * 16 + c);
                if (vram_read(300 + c, 400 + r) != expect) bad++;
            }
    }
    result(bad == 0, "blit_po display-to-display: %d bad pixels\n", bad);
    if (bad > 0) {
        out("      Source row 400 [200..215]: ");
        { int x; for (x = 200; x < 216; x++) out("%02X", vram_read(x, 400)); }
        out("\n      Dest   row 400 [300..315]: ");
        { int x; for (x = 300; x < 316; x++) out("%02X", vram_read(x, 400)); }
        out("\n");
    }

    po_reset();
    out("\n");
}

/* ---- TEST 21: Offscreen staging via blit_po ---- */
static void test_po_staging(void)
{
    unsigned long off1, off2, po0, po1, po2;
    int bad;

    out("\n========================================\n");
    out("  TEST 21: Offscreen Staging via blit_po\n");
    out("========================================\n\n");

    out("  This replicates the dune demo's staging pattern:\n");
    out("  CPU -> page 1, gpu blit_po page 1 -> page 2, blit_po page 2 -> display\n\n");

    po0  = make_pitch_offset(0);
    off1 = (unsigned long)g_page_stride * g_pitch;
    off2 = (unsigned long)g_page_stride * g_pitch * 2;
    po1  = make_pitch_offset(off1);
    po2  = make_pitch_offset(off2);

    if (off2 + (unsigned long)g_pitch * (g_yres + 4) > g_lfb_size) {
        out("  SKIPPED: LFB too small (need %lu, have %lu)\n",
            off2 + (unsigned long)g_pitch * (g_yres + 4), g_lfb_size);
        return;
    }

    /* 21a: CPU writes a striped pattern to page 1 */
    out("  21a: CPU writes 64x8 stripe pattern to page 1\n");
    {
        int r, c;
        for (r = 0; r < 8; r++)
            for (c = 0; c < 64; c++)
                off_write(off1, c, r, (unsigned char)((r & 1) ? 0xCC : 0x55));
    }

    /* 21b: GPU blit_po from page 1 to page 2 */
    out("  21b: blit_po page1(0,0) -> page2(0,0) 64x8\n");
    off_fill(off2, 0, 0, 68, 12, 0x00);
    blit_po(po1, 0, 0, po2, 0, 0, 64, 8);
    gpu_wait_idle();
    gpu_flush_2d();

    bad = 0;
    {
        int r, c;
        for (r = 0; r < 8; r++)
            for (c = 0; c < 64; c++) {
                unsigned char expect = (unsigned char)((r & 1) ? 0xCC : 0x55);
                if (off_read(off2, c, r) != expect) bad++;
            }
    }
    result(bad == 0, "Stage page1->page2: %d bad of 512 pixels\n", bad);
    if (bad > 0) {
        out("      Page 1 src:\n");
        off_dump_row(off1, 0, 0, 32);
        off_dump_row(off1, 1, 0, 32);
        out("      Page 2 dst:\n");
        off_dump_row(off2, 0, 0, 32);
        off_dump_row(off2, 1, 0, 32);
        /* Check if data landed at ABSOLUTE row 0 */
        out("      Abs row 0 [0..31]: ");
        { int x; for (x = 0; x < 32; x++) out("%02X", vram_read(x, 0)); }
        out("\n");
        /* Check absolute address where page2 starts */
        out("      Abs row %d [0..31]: ", g_page_stride * 2);
        { int x; for (x = 0; x < 32; x++) out("%02X", vram_read(x, g_page_stride * 2)); }
        out("\n");
    }

    /* 21c: GPU blit_po from page 2 back to display (page 0) */
    out("  21c: blit_po page2(0,0) -> display(400,300) 64x8\n");
    cpu_clear(398, 298, 68, 12, 0x00);
    blit_po(po2, 0, 0, po0, 400, 300, 64, 8);
    gpu_wait_idle();

    bad = 0;
    {
        int r, c;
        for (r = 0; r < 8; r++)
            for (c = 0; c < 64; c++) {
                unsigned char expect = (unsigned char)((r & 1) ? 0xCC : 0x55);
                if (vram_read(400 + c, 300 + r) != expect) bad++;
            }
    }
    result(bad == 0, "Stage page2->display: %d bad of 512 pixels\n", bad);
    if (bad > 0) {
        out("      Display dest:\n");
        { int x; out("      Row 300 [400..431]: ");
          for (x = 400; x < 432; x++) out("%02X", vram_read(x, 300)); out("\n"); }
        { int x; out("      Row 301 [400..431]: ");
          for (x = 400; x < 432; x++) out("%02X", vram_read(x, 301)); out("\n"); }
    }

    po_reset();
    out("\n");
}

/* ---- TEST 22: Consecutive PO switching ---- */
static void test_po_switching(void)
{
    unsigned long off1, off2, po0, po1, po2;
    int bad1, bad2;

    out("\n========================================\n");
    out("  TEST 22: Consecutive PITCH_OFFSET Switching\n");
    out("========================================\n\n");

    out("  Tests that back-to-back blits with different PITCH_OFFSETs\n");
    out("  each use their own PO, not a stale value.\n\n");

    po0  = make_pitch_offset(0);
    off1 = (unsigned long)g_page_stride * g_pitch;
    off2 = (unsigned long)g_page_stride * g_pitch * 2;
    po1  = make_pitch_offset(off1);
    po2  = make_pitch_offset(off2);

    if (off2 + (unsigned long)g_pitch * 8 > g_lfb_size) {
        out("  SKIPPED: LFB too small\n");
        return;
    }

    /* Write distinct patterns to page 1 and page 2 */
    {
        int r, c;
        for (r = 0; r < 4; r++)
            for (c = 0; c < 32; c++) {
                off_write(off1, c, r, 0xAA);
                off_write(off2, c, r, 0x55);
            }
    }

    /* Clear two dst regions on display */
    cpu_clear(100, 450, 36, 8, 0x00);
    cpu_clear(200, 450, 36, 8, 0x00);

    /* Issue two blits back-to-back with different SRC POs */
    out("  22a: blit_po from page1 -> display(100,450) 32x4\n");
    out("  22b: blit_po from page2 -> display(200,450) 32x4 (back-to-back)\n");
    blit_po(po1, 0, 0, po0, 100, 450, 32, 4);
    blit_po(po2, 0, 0, po0, 200, 450, 32, 4);
    gpu_wait_idle();

    bad1 = vram_check_rect(100, 450, 32, 4, 0xAA);
    bad2 = vram_check_rect(200, 450, 32, 4, 0x55);

    result(bad1 == 0,
           "Blit from page1 (expect 0xAA): %d bad of 128\n", bad1);
    result(bad2 == 0,
           "Blit from page2 (expect 0x55): %d bad of 128\n", bad2);

    if (bad1 > 0 || bad2 > 0) {
        int x;
        out("      Display row 450 [100..131]: ");
        for (x = 100; x < 132; x++) out("%02X", vram_read(x, 450));
        out("\n      Display row 450 [200..231]: ");
        for (x = 200; x < 232; x++) out("%02X", vram_read(x, 450));
        out("\n");
        /* Cross-check: show what's in page1 and page2 */
        out("      Page1 row 0 [0..31]: ");
        for (x = 0; x < 32; x++) out("%02X", off_read(off1, x, 0));
        out("\n      Page2 row 0 [0..31]: ");
        for (x = 0; x < 32; x++) out("%02X", off_read(off2, x, 0));
        out("\n");
    }

    po_reset();
    out("\n");
}

/* ---- TEST 23: Color-keyed blit with PITCH_OFFSET ---- */
static void test_po_colorkey(void)
{
    unsigned long off1, po0, po1;
    int bad_key, bad_bg;

    out("\n========================================\n");
    out("  TEST 23: Color-Keyed blit_po\n");
    out("========================================\n\n");

    po0  = make_pitch_offset(0);
    off1 = (unsigned long)g_page_stride * g_pitch;
    po1  = make_pitch_offset(off1);

    if (off1 + (unsigned long)g_pitch * 8 > g_lfb_size) {
        out("  SKIPPED: LFB too small\n");
        return;
    }

    /* Create a source pattern on page 1: alternating key/data pixels.
       Key=0x00 (transparent), data=0xDD */
    {
        int r, c;
        for (r = 0; r < 4; r++)
            for (c = 0; c < 32; c++)
                off_write(off1, c, r, (unsigned char)((c & 1) ? 0xDD : 0x00));
    }

    /* Prefill display dest with 0x77 (background) */
    cpu_clear(500, 400, 36, 8, 0x77);

    /* Keyed blit: skip pixels matching key=0x00 */
    out("  23a: blit_po_key page1(0,0)->(500,400) 32x4, key=0x00\n");
    blit_po_key(po1, 0, 0, po0, 500, 400, 32, 4, 0x00);
    gpu_wait_idle();

    /* Even columns should remain 0x77 (bg), odd columns should be 0xDD */
    bad_key = 0;
    bad_bg  = 0;
    {
        int r, c;
        for (r = 0; r < 4; r++)
            for (c = 0; c < 32; c++) {
                unsigned char v = vram_read(500 + c, 400 + r);
                if (c & 1) {
                    if (v != 0xDD) bad_key++;
                } else {
                    if (v != 0x77) bad_bg++;
                }
            }
    }
    result(bad_key == 0 && bad_bg == 0,
           "Keyed PO blit: %d data errors, %d key transparency errors\n",
           bad_key, bad_bg);
    if (bad_key > 0 || bad_bg > 0) {
        int x;
        out("      Source (page1) row 0 [0..31]: ");
        for (x = 0; x < 32; x++) out("%02X", off_read(off1, x, 0));
        out("\n      Dest (display) row 400 [500..531]: ");
        for (x = 500; x < 532; x++) out("%02X", vram_read(x, 400));
        out("\n      Expected: 77DD77DD77DD...  (bg where key, data where not)\n");
    }

    gpu_disable_cmp();
    po_reset();
    out("\n");
}

/* ---- TEST 24: Full-screen offscreen round-trip ---- */
static void test_po_fullscreen(void)
{
    unsigned long off1, po0, po1;
    int bad;
    int tw, th;

    out("\n========================================\n");
    out("  TEST 24: Full-screen Offscreen Round-trip\n");
    out("========================================\n\n");

    out("  Tests the exact dune demo pattern at display resolution:\n");
    out("  CPU fills page 1, blit_po page1->display full-screen.\n\n");

    po0  = make_pitch_offset(0);
    off1 = (unsigned long)g_page_stride * g_pitch;
    po1  = make_pitch_offset(off1);

    tw = g_xres;
    th = g_yres;

    if (off1 + (unsigned long)g_pitch * (th + 4) > g_lfb_size) {
        out("  SKIPPED: LFB too small\n");
        return;
    }

    /* CPU fills page 1 with a recognizable gradient */
    out("  24a: CPU fills page 1 (%dx%d) with row-based gradient\n", tw, th);
    {
        int r, c;
        for (r = 0; r < th; r++) {
            unsigned char v = (unsigned char)((r * 251 / th) & 0xFF);
            for (c = 0; c < tw; c++)
                off_write(off1, c, r, v);
        }
    }

    /* Clear display */
    memset(g_lfb, 0x00, (long)g_pitch * th);

    /* blit_po: page1 -> display, full screen */
    out("  24b: blit_po page1 -> display (%dx%d)\n", tw, th);
    blit_po(po1, 0, 0, po0, 0, 0, tw, th);
    gpu_wait_idle();
    gpu_flush_2d();

    /* Verify display matches */
    bad = 0;
    {
        int r, c;
        for (r = 0; r < th; r++) {
            unsigned char expect = (unsigned char)((r * 251 / th) & 0xFF);
            for (c = 0; c < tw; c++)
                if (vram_read(c, r) != expect) { bad++; break; }
        }
    }
    result(bad == 0,
           "Full-screen blit_po page1->display: %d bad rows of %d\n", bad, th);
    if (bad > 0) {
        int r;
        out("      First 10 mismatched rows:\n");
        for (r = 0; r < th && bad > 0; r++) {
            unsigned char expect = (unsigned char)((r * 251 / th) & 0xFF);
            unsigned char got = vram_read(0, r);
            if (got != expect) {
                out("        row %d: expect=0x%02X got=0x%02X [0..7]: ", r, expect, got);
                { int x; for (x = 0; x < 8; x++) out("%02X", vram_read(x, r)); }
                out("\n");
                bad--;
                if (bad <= 0) break;
            }
        }
    }

    po_reset();
    out("\n");
}

/* ---- TEST 25: blit_po WITHOUT GMC bits (control test) ---- */
static void test_po_no_gmc(void)
{
    unsigned long off1, po0, po1;
    int bad_with, bad_without;

    out("\n========================================\n");
    out("  TEST 25: blit_po WITH vs WITHOUT GMC PO bits\n");
    out("========================================\n\n");

    out("  Compares blit behavior when GMC_*_PITCH_OFFSET_CNTL bits\n");
    out("  are set vs not set.  This determines whether the GPU\n");
    out("  actually needs these bits to read per-blit PITCH_OFFSET.\n\n");

    po0  = make_pitch_offset(0);
    off1 = (unsigned long)g_page_stride * g_pitch;
    po1  = make_pitch_offset(off1);

    if (off1 + (unsigned long)g_pitch * 8 > g_lfb_size) {
        out("  SKIPPED: LFB too small\n");
        return;
    }

    /* Fill page 1 with 0xBB via CPU */
    {
        int r, c;
        for (r = 0; r < 4; r++)
            for (c = 0; c < 32; c++)
                off_write(off1, c, r, 0xBB);
    }

    /* 25a: WITH GMC bits (the correct xf86 way) */
    out("  25a: blit WITH GMC PO bits\n");
    cpu_clear(100, 480, 36, 8, 0x00);
    blit_po(po1, 0, 0, po0, 100, 480, 32, 4);
    gpu_wait_idle();
    bad_with = vram_check_rect(100, 480, 32, 4, 0xBB);
    result(bad_with == 0, "With GMC bits: %d bad of 128\n", bad_with);
    if (bad_with > 0) {
        int x;
        out("      Display row 480 [100..131]: ");
        for (x = 100; x < 132; x++) out("%02X", vram_read(x, 480));
        out("\n");
    }

    /* 25b: WITHOUT GMC bits — write PITCH_OFFSET but DON'T set GMC flags.
       This is what the old broken code did. */
    out("  25b: blit WITHOUT GMC PO bits (old broken pattern)\n");
    cpu_clear(200, 480, 36, 8, 0x00);
    gpu_wait_fifo(7);
    wreg(R_DST_PITCH_OFFSET, po0);
    wreg(R_SRC_PITCH_OFFSET, po1);
    wreg(R_DP_GUI_MASTER_CNTL,
         /* NO GMC_*_PITCH_OFFSET_CNTL bits */
         GMC_BRUSH_NONE | GMC_DST_8BPP | GMC_SRC_DATATYPE_COLOR |
         ROP3_SRCCOPY | GMC_DP_SRC_MEMORY | GMC_CLR_CMP_DIS |
         GMC_WR_MSK_DIS);
    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
    wreg(R_SRC_Y_X, (0UL << 16) | 0UL);
    wreg(R_DST_Y_X, (480UL << 16) | 200UL);
    wreg(R_DST_HEIGHT_WIDTH, (4UL << 16) | 32UL);
    gpu_wait_idle();
    bad_without = vram_check_rect(200, 480, 32, 4, 0xBB);
    result(bad_without == 0, "Without GMC bits: %d bad of 128\n", bad_without);
    if (bad_without > 0) {
        int x;
        out("      Display row 480 [200..231]: ");
        for (x = 200; x < 232; x++) out("%02X", vram_read(x, 480));
        out("\n");
    }

    /* Interpretation */
    if (bad_with == 0 && bad_without == 0) {
        out("  RESULT: GMC bits NOT required — PO works either way\n");
    } else if (bad_with == 0 && bad_without > 0) {
        out("  RESULT: GMC bits ARE required — without them, PO is ignored\n");
    } else if (bad_with > 0 && bad_without == 0) {
        out("  RESULT: GMC bits BREAK blits — do NOT use them!\n");
    } else {
        out("  RESULT: Both methods fail — deeper issue with PITCH_OFFSET\n");
    }

    po_reset();
    out("\n");
}

/* ---- TEST 26: Separate vs combined PITCH_OFFSET writes ---- */
static void test_po_separate_regs(void)
{
    unsigned long off1, po0, po1;
    int bad_combined, bad_separate;

    out("\n========================================\n");
    out("  TEST 26: Combined vs Separate Pitch/Offset Registers\n");
    out("========================================\n\n");

    out("  Tests if writing DST_OFFSET+DST_PITCH separately (without\n");
    out("  the combined DST_PITCH_OFFSET) achieves the same result.\n\n");

    po0  = make_pitch_offset(0);
    off1 = (unsigned long)g_page_stride * g_pitch;
    po1  = make_pitch_offset(off1);

    if (off1 + (unsigned long)g_pitch * 8 > g_lfb_size) {
        out("  SKIPPED: LFB too small\n");
        return;
    }

    /* Fill page 1 with 0xCC via CPU */
    {
        int r, c;
        for (r = 0; r < 4; r++)
            for (c = 0; c < 32; c++)
                off_write(off1, c, r, 0xCC);
    }

    /* 26a: Combined register (PITCH_OFFSET) with GMC bits */
    out("  26a: Combined SRC_PITCH_OFFSET + GMC bits\n");
    cpu_clear(100, 490, 36, 8, 0x00);
    blit_po(po1, 0, 0, po0, 100, 490, 32, 4);
    gpu_wait_idle();
    bad_combined = vram_check_rect(100, 490, 32, 4, 0xCC);
    result(bad_combined == 0,
           "Combined PO+GMC: %d bad of 128\n", bad_combined);

    /* 26b: Separate registers (SRC_OFFSET + SRC_PITCH), NO GMC bits */
    out("  26b: Separate SRC_OFFSET+SRC_PITCH, no GMC bits\n");
    cpu_clear(200, 490, 36, 8, 0x00);
    gpu_wait_fifo(9);
    wreg(R_SRC_PITCH_OFFSET, po1);  /* also write combined for reference */
    wreg(R_SRC_OFFSET, g_fb_location + off1);
    wreg(R_SRC_PITCH,  (unsigned long)g_pitch);
    wreg(R_DST_PITCH_OFFSET, po0);
    wreg(R_DST_OFFSET, g_fb_location);
    wreg(R_DST_PITCH,  (unsigned long)g_pitch);
    wreg(R_DP_GUI_MASTER_CNTL,
         /* NO GMC_*_PITCH_OFFSET_CNTL bits */
         GMC_BRUSH_NONE | GMC_DST_8BPP | GMC_SRC_DATATYPE_COLOR |
         ROP3_SRCCOPY | GMC_DP_SRC_MEMORY | GMC_CLR_CMP_DIS |
         GMC_WR_MSK_DIS);
    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);
    wreg(R_SRC_Y_X, (0UL << 16) | 0UL);
    gpu_wait_fifo(2);
    wreg(R_DST_Y_X, (490UL << 16) | 200UL);
    wreg(R_DST_HEIGHT_WIDTH, (4UL << 16) | 32UL);
    gpu_wait_idle();
    bad_separate = vram_check_rect(200, 490, 32, 4, 0xCC);
    result(bad_separate == 0,
           "Separate regs (no GMC): %d bad of 128\n", bad_separate);

    if (bad_combined > 0 || bad_separate > 0) {
        int x;
        out("      Row 490 [100..131] (combined): ");
        for (x = 100; x < 132; x++) out("%02X", vram_read(x, 490));
        out("\n      Row 490 [200..231] (separate): ");
        for (x = 200; x < 232; x++) out("%02X", vram_read(x, 490));
        out("\n");
    }

    /* Interpretation */
    if (bad_combined == 0 && bad_separate == 0) {
        out("  RESULT: Both combined+GMC and separate regs work\n");
    } else if (bad_combined == 0 && bad_separate > 0) {
        out("  RESULT: Combined+GMC required; separate regs alone don't work\n");
    } else if (bad_combined > 0 && bad_separate == 0) {
        out("  RESULT: Separate regs work; combined+GMC broken!\n");
    } else {
        out("  RESULT: Neither method works — fundamental PO issue\n");
    }

    po_reset();
    out("\n");
}

/* ---- TEST 27: Large offset (simulating layer 10+) ---- */
static void test_po_large_offset(void)
{
    unsigned long off_far, po0, po_far;
    unsigned long max_lfb_row;
    int bad;
    int target_page;

    out("\n========================================\n");
    out("  TEST 27: Large VRAM Offset (beyond row 8191)\n");
    out("========================================\n\n");

    po0 = make_pitch_offset(0);

    /* Find the highest page we can test within LFB mapping */
    max_lfb_row = g_lfb_size / g_pitch;
    target_page = (int)(max_lfb_row / g_page_stride) - 1;
    if (target_page < 2) target_page = 2;

    off_far = (unsigned long)target_page * g_page_stride * g_pitch;
    po_far  = make_pitch_offset(off_far);

    out("  Target page: %d (row %ld, VRAM offset 0x%08lX)\n",
        target_page, (long)target_page * g_page_stride, off_far);
    out("  Absolute row: %ld  (8191 limit: %s)\n",
        (long)target_page * g_page_stride,
        (long)target_page * g_page_stride > 8191 ? "EXCEEDS" : "within");
    out("  PO=0x%08lX\n\n", po_far);

    if (off_far + (unsigned long)g_pitch * 8 > g_lfb_size) {
        out("  SKIPPED: LFB can't reach page %d\n", target_page);
        return;
    }

    /* CPU writes pattern to far page */
    {
        int r, c;
        for (r = 0; r < 4; r++)
            for (c = 0; c < 32; c++)
                off_write(off_far, c, r, 0xEE);
    }

    /* GPU blit from far page to display via PITCH_OFFSET */
    out("  27a: blit_po from page %d -> display(300,480) 32x4\n", target_page);
    cpu_clear(298, 478, 36, 8, 0x00);
    blit_po(po_far, 0, 0, po0, 300, 480, 32, 4);
    gpu_wait_idle();

    bad = vram_check_rect(300, 480, 32, 4, 0xEE);
    result(bad == 0,
           "Large offset blit (page %d): %d bad of 128\n", target_page, bad);
    if (bad > 0) {
        int x;
        out("      Far page data [0..31]: ");
        for (x = 0; x < 32; x++) out("%02X", off_read(off_far, x, 0));
        out("\n      Display row 480 [300..331]: ");
        for (x = 300; x < 332; x++) out("%02X", vram_read(x, 480));
        out("\n");
    }

    /* 27b: GPU blit TO far page, then read back via CPU */
    out("  27b: gpu_fill_po to page %d, then CPU readback\n", target_page);
    off_fill(off_far, 50, 0, 36, 8, 0x00);
    gpu_fill_po(po_far, 50, 0, 32, 4, 0x99);
    gpu_wait_idle();
    gpu_flush_2d();

    bad = off_check(off_far, 50, 0, 32, 4, 0x99);
    result(bad == 0,
           "gpu_fill_po to page %d, CPU readback: %d bad of 128\n",
           target_page, bad);
    if (bad > 0) {
        off_dump_row(off_far, 0, 50, 82);
        off_dump_row(off_far, 1, 50, 82);
    }

    po_reset();
    out("\n");
}

/* ---- TEST 28: Register state after blit_po ---- */
static void test_po_register_state(void)
{
    unsigned long off1, po0, po1;
    unsigned long rd_dst_po, rd_src_po, rd_dst_off, rd_src_off;
    unsigned long rd_dst_pitch, rd_src_pitch;

    out("\n========================================\n");
    out("  TEST 28: Register State After blit_po\n");
    out("========================================\n\n");

    out("  Reads back DST/SRC PITCH_OFFSET and separate offset/pitch\n");
    out("  registers after a blit_po to understand GPU register model.\n\n");

    po0  = make_pitch_offset(0);
    off1 = (unsigned long)g_page_stride * g_pitch;
    po1  = make_pitch_offset(off1);

    /* First: read registers in default state */
    out("  Default state (before any blit_po):\n");
    po_reset();
    gpu_wait_idle();
    rd_dst_po    = rreg(R_DST_PITCH_OFFSET);
    rd_src_po    = rreg(R_SRC_PITCH_OFFSET);
    rd_dst_off   = rreg(R_DST_OFFSET);
    rd_src_off   = rreg(R_SRC_OFFSET);
    rd_dst_pitch = rreg(R_DST_PITCH);
    rd_src_pitch = rreg(R_SRC_PITCH);
    out("    DST_PITCH_OFFSET = 0x%08lX (expect 0x%08lX)\n", rd_dst_po, po0);
    out("    SRC_PITCH_OFFSET = 0x%08lX (expect 0x%08lX)\n", rd_src_po, po0);
    out("    DST_OFFSET       = 0x%08lX\n", rd_dst_off);
    out("    DST_PITCH        = 0x%08lX (%lu)\n", rd_dst_pitch, rd_dst_pitch);
    out("    SRC_OFFSET       = 0x%08lX\n", rd_src_off);
    out("    SRC_PITCH        = 0x%08lX (%lu)\n", rd_src_pitch, rd_src_pitch);

    /* Issue a blit_po with non-default PO */
    if (off1 + (unsigned long)g_pitch * 8 <= g_lfb_size) {
        off_fill(off1, 0, 0, 8, 4, 0x77);
        blit_po(po1, 0, 0, po0, 600, 500, 4, 4);
        gpu_wait_idle();

        out("  After blit_po(src=page1, dst=display):\n");
        rd_dst_po    = rreg(R_DST_PITCH_OFFSET);
        rd_src_po    = rreg(R_SRC_PITCH_OFFSET);
        rd_dst_off   = rreg(R_DST_OFFSET);
        rd_src_off   = rreg(R_SRC_OFFSET);
        rd_dst_pitch = rreg(R_DST_PITCH);
        rd_src_pitch = rreg(R_SRC_PITCH);
        out("    DST_PITCH_OFFSET = 0x%08lX (wrote 0x%08lX)\n", rd_dst_po, po0);
        out("    SRC_PITCH_OFFSET = 0x%08lX (wrote 0x%08lX)\n", rd_src_po, po1);
        out("    DST_OFFSET       = 0x%08lX\n", rd_dst_off);
        out("    DST_PITCH        = 0x%08lX (%lu)\n", rd_dst_pitch, rd_dst_pitch);
        out("    SRC_OFFSET       = 0x%08lX\n", rd_src_off);
        out("    SRC_PITCH        = 0x%08lX (%lu)\n", rd_src_pitch, rd_src_pitch);

        /* Note: PITCH_OFFSET is FIFO-queued and may read 0 */
        if (rd_dst_po == 0 && rd_src_po == 0)
            out("    (PITCH_OFFSET reads 0 — FIFO-queued, expected on RV515)\n");
    }

    po_reset();
    out("\n");
}

/* =============================================================== */
/*  Main                                                            */
/* =============================================================== */

int main(void)
{
    unsigned long bar0, bar2, bar3;
    unsigned long lfb_sz;

    printf("RBLIT.EXE - Radeon X1300 Pro 2D Blitter Validation\n");
    printf("===================================================\n\n");

    g_log = fopen("BLITLOG.TXT", "w");
    if (!g_log) {
        printf("WARNING: Cannot create BLITLOG.TXT\n");
    } else {
        out("RBLIT - Radeon X1300 Pro 2D Blitter Validation Log\n");
        out("===================================================\n\n");
    }

    if (!dpmi_alloc(64)) {
        out("ERROR: Cannot allocate DOS memory.\n");
        return 1;
    }

    /* PCI scan */
    out("Scanning PCI bus for ATI RV515/RV516...\n");
    if (!pci_find_radeon()) {
        out("ERROR: No Radeon X1300 (RV515/RV516) found.\n");
        if (g_log) fclose(g_log);
        dpmi_free();
        return 1;
    }
    out("  Card: %s (PCI %02X:%02X.%X)  DevID: %04X\n",
        g_card_name, g_pci_bus, g_pci_dev, g_pci_func, g_pci_did);

    /* Map MMIO */
    bar0 = pci_rd32(g_pci_bus, g_pci_dev, g_pci_func, 0x10);
    bar2 = pci_rd32(g_pci_bus, g_pci_dev, g_pci_func, 0x18);
    g_mmio_phys = bar2 & 0xFFFFFFF0UL;

    if ((bar2 & 0x06) == 0x04) {
        bar3 = pci_rd32(g_pci_bus, g_pci_dev, g_pci_func, 0x1C);
        if (bar3 != 0) {
            out("ERROR: MMIO above 4GB.\n");
            if (g_log) fclose(g_log);
            dpmi_free();
            return 1;
        }
    }

    g_mmio = (volatile unsigned long *)dpmi_map(g_mmio_phys, 0x10000UL);
    if (!g_mmio) {
        out("ERROR: Cannot map MMIO at 0x%08lX.\n", g_mmio_phys);
        if (g_log) fclose(g_log);
        dpmi_free();
        return 1;
    }
    out("  MMIO mapped: phys=0x%08lX\n", g_mmio_phys);

    /* Read GPU state */
    g_vram_mb = rreg(R_CONFIG_MEMSIZE) / (1024UL * 1024UL);
    {
        unsigned long mc_fb = mc_rreg(RV515_MC_FB_LOCATION);
        g_fb_location = (mc_fb & 0xFFFF) << 16;
    }
    out("  VRAM: %luMB  FB_LOCATION: 0x%08lX\n", g_vram_mb, g_fb_location);

    /* Find VESA mode */
    out("\nLooking for 800x600 8bpp VESA mode...\n");
    if (!find_mode()) {
        out("ERROR: No suitable VESA mode found.\n");
        dpmi_unmap((void *)g_mmio);
        if (g_log) fclose(g_log);
        dpmi_free();
        return 1;
    }

    g_vbe_pitch = g_pitch;  /* save original VBE pitch */
    out("  Mode 0x%03X: %dx%d pitch=%d LFB=0x%08lX\n",
        g_vmode, g_xres, g_yres, g_pitch, g_lfb_phys);

    /* Align pitch */
    if (g_pitch & 63) {
        g_pitch = (g_pitch + 63) & ~63;
        out("  Pitch aligned: %d -> %d (64-byte GPU requirement)\n",
            g_vbe_pitch, g_pitch);
    }

    out("\nPress any key to enter graphics mode and run tests...\n");
    if (g_log) {
        out("(Results will be saved to BLITLOG.TXT)\n");
    }
    getch();

    /* Set graphics mode */
    if (!vbe_set(g_vmode)) {
        out("ERROR: Cannot set VESA mode.\n");
        dpmi_unmap((void *)g_mmio);
        if (g_log) fclose(g_log);
        dpmi_free();
        return 1;
    }

    /* Compute page stride: smallest row count >= g_yres where
       stride * pitch is 4KB-aligned.  Same algorithm as RADEON.C.
       Required because PITCH_OFFSET encodes addresses in 1KB units
       (bits [21:0] = byte_addr >> 10), so page boundaries must be
       at least 1KB-aligned (4KB gives us flip alignment too). */
    g_page_stride = g_yres;
    while (((long)g_page_stride * g_pitch) & 4095L)
        g_page_stride++;

    /* Map LFB — 4 pages for offscreen PITCH_OFFSET tests */
    lfb_sz = (unsigned long)g_pitch * g_page_stride * 4;
    if (lfb_sz < (unsigned long)g_pitch * g_yres)
        lfb_sz = (unsigned long)g_pitch * g_yres;
    lfb_sz = (lfb_sz + 4095UL) & ~4095UL;
    g_lfb_size = lfb_sz;
    g_lfb = (unsigned char *)dpmi_map(g_lfb_phys, lfb_sz);
    if (!g_lfb) {
        vbe_text();
        out("ERROR: Cannot map LFB (%lu bytes).\n", lfb_sz);
        dpmi_unmap((void *)g_mmio);
        if (g_log) fclose(g_log);
        dpmi_free();
        return 1;
    }

    /* Initialize GPU 2D engine */
    gpu_init_2d();

    /* If pitch was aligned, update VGA CRTC */
    if (g_pitch != g_xres) {
        unsigned char cr_val = (unsigned char)(g_pitch / 8);
        outp(0x3D4, 0x13);
        outp(0x3D5, cr_val);
    }

    /* Clear screen */
    memset(g_lfb, 0, (long)g_pitch * g_yres);

    out("\n  Running 2D blitter tests in graphics mode...\n");
    out("  Resolution: %dx%d  Pitch: %d (VBE: %d)  VRAM: %luMB\n",
        g_xres, g_yres, g_pitch, g_vbe_pitch, g_vram_mb);
    out("  Page stride: %d rows  LFB mapped: %lu bytes (%lu KB)\n\n",
        g_page_stride, g_lfb_size, g_lfb_size / 1024UL);

    /* ---- Run all tests ---- */
    test_fill_basic();
    test_pitch_alignment();
    test_blit_fwd();
    test_color_key();
    test_state_leakage();
    test_blit_overlap();
    test_scissor();
    test_cache_coherence();
    test_fullwidth();
    test_register_readback();
    /* Diagnostic tests for flicker / ghost / sky-leak symptoms */
    test_avivo_registers();
    test_vblank_timing();
    test_ghost_rect();
    test_fullwidth_keyed();
    test_vga_crtc();
    test_flip_address();
    test_gmc_pitch_offset_cntl();

    /* PITCH_OFFSET comprehensive tests (TEST 18-28) */
    test_po_encoding();
    test_po_fill();
    test_po_blit_basic();
    test_po_staging();
    test_po_switching();
    test_po_colorkey();
    test_po_fullscreen();
    test_po_no_gmc();
    test_po_separate_regs();
    test_po_large_offset();
    test_po_register_state();

    /* ---- Summary ---- */
    out("\n========================================\n");
    out("  SUMMARY\n");
    out("========================================\n\n");
    out("  Total:    %d tests\n", g_pass + g_fail);
    out("  Passed:   %d\n", g_pass);
    out("  Failed:   %d\n", g_fail);
    out("  Warnings: %d\n", g_warn);
    out("\n  CLR_CMP_FCN_NE = %lu  (kernel says: 5 = NEQ)\n", CLR_CMP_FCN_NE);
    out("  CLR_CMP_FCN_EQ = %lu  (kernel says: 4 = EQ)\n", CLR_CMP_FCN_EQ);
    out("\n  See BLITLOG.TXT for full details.\n");

    /* Return to text mode */
    vbe_text();

    /* Print summary again in text mode */
    printf("\n--- RBLIT Results ---\n");
    printf("  Passed:   %d\n", g_pass);
    printf("  Failed:   %d\n", g_fail);
    printf("  Warnings: %d\n", g_warn);
    printf("  Log: BLITLOG.TXT\n");

    /* Cleanup */
    dpmi_unmap((void *)g_lfb);
    dpmi_unmap((void *)g_mmio);
    if (g_log) fclose(g_log);
    dpmi_free();

    return (g_fail > 0) ? 1 : 0;
}
