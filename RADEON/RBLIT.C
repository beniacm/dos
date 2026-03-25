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
static unsigned long  g_fb_location = 0;
static int g_xres, g_yres, g_pitch;
static int g_vbe_pitch;    /* original VBE pitch before alignment */
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

    gpu_wait_fifo(14);
    wreg(R_DEFAULT_PITCH_OFFSET, pitch_offset);
    wreg(R_DST_PITCH_OFFSET, pitch_offset);
    wreg(R_SRC_PITCH_OFFSET, pitch_offset);

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

    /* Read back PITCH_OFFSET register */
    po_readback = rreg(R_DST_PITCH_OFFSET);
    po_pitch64 = (po_readback >> 22) & 0x3FF;
    out("  DST_PITCH_OFFSET = 0x%08lX  (pitch64=%lu → %lu bytes)\n",
        po_readback, po_pitch64, po_pitch64 * 64);
    result(po_pitch64 * 64 == (unsigned long)g_pitch,
           "PITCH_OFFSET pitch matches g_pitch (%lu == %d)\n",
           po_pitch64 * 64, g_pitch);

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

    /* Map LFB — need enough for test area (up to ~y=560) */
    lfb_sz = (unsigned long)g_pitch * 600;
    if (lfb_sz < (unsigned long)g_pitch * g_yres)
        lfb_sz = (unsigned long)g_pitch * g_yres;
    lfb_sz = (lfb_sz + 4095UL) & ~4095UL;
    g_lfb = (unsigned char *)dpmi_map(g_lfb_phys, lfb_sz);
    if (!g_lfb) {
        vbe_text();
        out("ERROR: Cannot map LFB.\n");
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
    out("  Resolution: %dx%d  Pitch: %d (VBE: %d)  VRAM: %luMB\n\n",
        g_xres, g_yres, g_pitch, g_vbe_pitch, g_vram_mb);

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
