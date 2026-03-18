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
/* =============================================================== */

#define R_CONFIG_MEMSIZE           0x00F8

#define R_RBBM_STATUS              0x0E40
#define   RBBM_FIFOCNT_MASK        0x007F
#define   RBBM_ACTIVE              (1UL << 31)

#define R_DSTCACHE_CTLSTAT         0x1714
#define   DC_FLUSH_ALL             0x000F

#define R_WAIT_UNTIL               0x1720

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
#define R_DP_CNTL                  0x16C0
#define   DST_X_LEFT_TO_RIGHT      1UL
#define   DST_Y_TOP_TO_BOTTOM      2UL
#define R_DP_DATATYPE              0x16C4
#define R_DP_MIX                   0x16C8
#define R_DP_WRITE_MASK            0x16CC

#define R_DEFAULT_SC_BOTTOM_RIGHT  0x16E8
#define R_SC_TOP_LEFT              0x16EC
#define R_SC_BOTTOM_RIGHT          0x16F0

#define R_DST_LINE_START           0x1600
#define R_DST_LINE_END             0x1604
#define R_DST_LINE_PATCOUNT        0x1608

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

/* Wait for at least `n` free FIFO entries */
static void gpu_wait_fifo(int n)
{
    unsigned long timeout = 2000000UL;
    while (timeout--) {
        if ((int)(rreg(R_RBBM_STATUS) & RBBM_FIFOCNT_MASK) >= n)
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
            return;
    }
}

/* =============================================================== */
/*  Radeon 2D engine initialisation                                 */
/* =============================================================== */

static void gpu_init_2d(void)
{
    unsigned long pitch64;

    gpu_wait_idle();

    /* Flush destination cache */
    wreg(R_DSTCACHE_CTLSTAT, DC_FLUSH_ALL);
    gpu_wait_idle();

    /* Encode pitch/offset for the combined register.
       Pitch field is in 64-byte units, offset in 1024-byte units. */
    pitch64 = ((unsigned long)g_pitch + 63) / 64;

    gpu_wait_fifo(10);
    wreg(R_DST_PITCH_OFFSET, (pitch64 << 22) | 0);
    wreg(R_SRC_PITCH_OFFSET, (pitch64 << 22) | 0);

    wreg(R_DEFAULT_SC_BOTTOM_RIGHT, (0x1FFF << 16) | 0x1FFF);
    wreg(R_SC_TOP_LEFT, 0);
    wreg(R_SC_BOTTOM_RIGHT,
         ((unsigned long)g_yres << 16) | (unsigned long)g_xres);

    wreg(R_DP_WRITE_MASK, 0xFFFFFFFF);
    wreg(R_DP_CNTL, DST_X_LEFT_TO_RIGHT | DST_Y_TOP_TO_BOTTOM);

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
/*  Main                                                            */
/* =============================================================== */

int main(void)
{
    unsigned long bar0, pci_cmd;
    unsigned long lfb_sz;
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

    /* --- Map MMIO registers (BAR0) --- */
    bar0 = pci_rd32(g_pci_bus, g_pci_dev, g_pci_func, 0x10);
    g_mmio_phys = bar0 & 0xFFFFFFF0UL;
    printf("  MMIO phys : 0x%08lX\n", g_mmio_phys);

    g_mmio = (volatile unsigned long *)dpmi_map(g_mmio_phys, 0x10000);
    if (!g_mmio) {
        printf("ERROR: Cannot map MMIO registers.\n");
        dpmi_free();
        return 1;
    }

    /* Ensure PCI memory access is enabled */
    pci_cmd = pci_rd32(g_pci_bus, g_pci_dev, g_pci_func, 0x04);
    if (!(pci_cmd & 0x02)) {
        pci_wr32(g_pci_bus, g_pci_dev, g_pci_func, 0x04, pci_cmd | 0x02);
        printf("  (Enabled PCI memory access)\n");
    }

    /* --- Read GPU info --- */
    g_vram_mb = rreg(R_CONFIG_MEMSIZE) / (1024UL * 1024UL);
    printf("  Video RAM : %lu MB\n", g_vram_mb);
    printf("  Engine    : %s\n",
           (rreg(R_RBBM_STATUS) & RBBM_ACTIVE) ? "BUSY" : "idle");

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

    lfb_sz = ((unsigned long)g_pitch * g_yres + 4095UL) & ~4095UL;
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

    /* --- Restore text mode, cleanup --- */
    vbe_text();

    printf("Radeon demo complete.\n");
    printf("Card: %s  |  VRAM: %lu MB  |  Mode: %dx%d\n",
           g_card_name, g_vram_mb, g_xres, g_yres);

    dpmi_unmap(g_lfb);
    dpmi_unmap((void *)g_mmio);
    dpmi_free();

    return 0;
}
