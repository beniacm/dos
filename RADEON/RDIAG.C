/*
 * RDIAG.C  -  ATI Radeon X1300 Pro (RV515/RV516) Hardware Diagnostic
 * For OpenWatcom 2.0, 32-bit DOS (PMODE/W or DOS/4GW)
 *
 * Produces TESTLOG.TXT with comprehensive hardware state dump:
 *   - Full PCI configuration space (256 bytes)
 *   - BAR probing with size detection
 *   - MMIO identification by probing each BAR for known register signatures
 *   - Complete GPU register dump from correct MMIO BAR
 *   - MC (memory controller) indirect register dump
 *   - VESA/VBE mode enumeration
 *   - DPMI capability check
 *   - VRAM read/write test
 *   - GPU 2D engine functional test
 *
 * Run on real RV515/RV516 hardware and send back TESTLOG.TXT for analysis.
 *
 * Build: wcc386 -bt=dos -3r -ox -s -zq RDIAG.C
 *        wlink system pmodew name RDIAG file RDIAG option quiet
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
    unsigned short oem_soft_rev;
    unsigned long  oem_vendor;
    unsigned long  oem_product;
    unsigned long  oem_rev;
    unsigned char  _pad[222];
    unsigned char  oem_data[256];
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
/*  ATI Radeon register offsets (RV515/R300-R500)                    */
/* =============================================================== */

/* Identifiable registers — values that reveal whether we're reading
   real MMIO or random VRAM */
#define R_CONFIG_APER_0_BASE       0x0100  /* should match BAR0 phys addr */
#define R_CONFIG_APER_SIZE         0x0108  /* aperture size */
#define R_CONFIG_MEMSIZE           0x00F8  /* VRAM size in bytes */
#define R_CONFIG_REG_1_BASE        0x010C  /* should match BAR2 phys addr */
#define R_CONFIG_REG_APER_SIZE     0x0110  /* register aperture size */
#define R_HOST_PATH_CNTL           0x0130
#define R_HDP_FB_LOCATION          0x0134

#define R_RBBM_STATUS              0x0E40
#define   RBBM_FIFOCNT_MASK        0x007FUL
#define   RBBM_ACTIVE              (1UL << 31)
#define R_RBBM_SOFT_RESET          0x00F0

#define R_VGA_RENDER_CONTROL       0x0300
#define R_SURFACE_CNTL             0x0B00

#define R_MC_IND_INDEX             0x0070
#define R_MC_IND_DATA              0x0074

#define R_CLOCK_CNTL_INDEX         0x0008
#define R_CLOCK_CNTL_DATA          0x000C

#define R_WAIT_UNTIL               0x1720
#define R_RBBM_GUICNTL             0x172C
#define R_ISYNC_CNTL               0x1724

#define R_DST_OFFSET               0x1404
#define R_DST_PITCH                0x1408
#define R_DST_PITCH_OFFSET         0x142C
#define R_SRC_PITCH_OFFSET         0x1428
#define R_DEFAULT_PITCH_OFFSET     0x16E0
#define R_DEFAULT_SC_BOTTOM_RIGHT  0x16E8
#define R_SC_TOP_LEFT              0x16EC
#define R_SC_BOTTOM_RIGHT          0x16F0

#define R_DP_GUI_MASTER_CNTL      0x146C
#define R_DP_BRUSH_FRGD_CLR       0x147C
#define R_DP_BRUSH_BKGD_CLR       0x1478
#define R_DP_SRC_FRGD_CLR         0x15D8
#define R_DP_SRC_BKGD_CLR         0x15DC
#define R_DP_CNTL                 0x16C0
#define R_DP_DATATYPE             0x16C4
#define R_DP_MIX                  0x16C8
#define R_DP_WRITE_MASK           0x16CC

#define R_DST_Y_X                 0x1438
#define R_DST_HEIGHT_WIDTH        0x143C

#define R_GB_ENABLE               0x4008
#define R_GB_MSPOS0               0x4010
#define R_GB_MSPOS1               0x4014
#define R_GB_TILE_CONFIG          0x4018
#define R_GB_SELECT               0x401C
#define R_GB_AA_CONFIG            0x4020
#define R_GB_PIPE_SELECT          0x402C

#define R_DST_PIPE_CONFIG         0x170C
#define R_SU_REG_DEST             0x42C8
#define R_VAP_INDEX_OFFSET        0x208C

#define R_GA_ENHANCE              0x4274

#define R_RB3D_DSTCACHE_MODE      0x3258
#define R_RB2D_DSTCACHE_MODE      0x3428
#define R_RB2D_DSTCACHE_CTLSTAT   0x342C
#define   RB2D_DC_FLUSH_ALL       0x0FUL
#define   RB2D_DC_BUSY            (1UL << 31)

#define R_RB3D_DSTCACHE_CTLSTAT_RS 0x4E4C
#define R_ZB_ZCACHE_CTLSTAT        0x4F18

/* GMC bits for 2D engine test */
#define GMC_BRUSH_SOLID            (13UL << 4)
#define GMC_DST_8BPP               (2UL << 8)
#define GMC_SRC_DATATYPE_COLOR     (3UL << 12)
#define GMC_CLR_CMP_DIS            (1UL << 28)
#define GMC_WR_MSK_DIS             (1UL << 30)
#define ROP3_PATCOPY               (0xF0UL << 16)
#define DST_X_LEFT_TO_RIGHT        1UL
#define DST_Y_TOP_TO_BOTTOM        2UL

/* RV515 MC indirect registers */
#define RV515_MC_STATUS            0x0000
#define RV515_MC_FB_LOCATION       0x0001
#define RV515_MC_CNTL              0x0005
#define RV515_MC_MISC_LAT_TIMER    0x0009
#define RV515_MC_MISC_UMA_CNTL     0x000C

/* MM_INDEX/MM_DATA indirect MMIO access */
#define R_MM_INDEX                 0x0000
#define R_MM_DATA                  0x0004

/* RBBM engine control */
#define R_RBBM_CNTL               0x0E44

/* Soft reset bits (for engine reset tests) */
#define SOFT_RESET_CP              (1UL << 0)
#define SOFT_RESET_HI              (1UL << 1)
#define SOFT_RESET_SE              (1UL << 2)
#define SOFT_RESET_RE              (1UL << 3)
#define SOFT_RESET_PP              (1UL << 4)
#define SOFT_RESET_E2              (1UL << 5)
#define SOFT_RESET_RB             (1UL << 6)

/* Host path control HDP bits (for engine reset tests) */
#define HDP_SOFT_RESET             (1UL << 26)
#define HDP_APER_CNTL              (1UL << 23)

/* D1/D2 VGA control registers (AVIVO display) */
#define R_D1VGA_CONTROL            0x0330
#define R_D2VGA_CONTROL            0x0338

/* ATI vendor ID */
#define ATI_VID  0x1002

/* =============================================================== */
/*  Device ID table                                                 */
/* =============================================================== */

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
static FILE *g_log = NULL;
static int g_pci_bus, g_pci_dev, g_pci_func;
static unsigned short g_pci_did;
static const char *g_card_name = "Unknown";

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
    if (!p) return;
    memset(&r, 0, sizeof r);
    r.x.eax = 0x0801;
    r.x.ebx = (a >> 16) & 0xFFFF;  r.x.ecx = a & 0xFFFF;
    int386(0x31, &r, &r);
}

/* Get DPMI version info */
static void dpmi_version_info(void)
{
    union REGS r;
    memset(&r, 0, sizeof r);
    r.x.eax = 0x0400;
    int386(0x31, &r, &r);
    out("  DPMI version   : %d.%02d\n",
        (int)(r.x.eax >> 8), (int)(r.x.eax & 0xFF));
    out("  Processor type : %d86\n", (int)(r.x.ecx & 0xFF));
    out("  Flags (DX)     : 0x%04X\n", (unsigned)(r.x.edx & 0xFFFF));
    out("    32-bit progs : %s\n", (r.x.edx & 1) ? "YES" : "no");
    out("    Reflection   : %s\n", (r.x.edx & 2) ? "virt 86" : "real mode");
    out("    Virt memory  : %s\n", (r.x.edx & 4) ? "YES" : "no");
}

/* =============================================================== */
/*  PCI configuration via Mechanism 1                               */
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

static unsigned char pci_rd8(int b, int d, int f, int reg)
{
    unsigned long v = pci_rd32(b, d, f, reg & ~3);
    return (unsigned char)(v >> ((reg & 3) * 8));
}

/* Scan for ATI RV515/RV516 */
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
/*  BAR probing — determine size of each BAR                        */
/* =============================================================== */

static unsigned long bar_probe_size(int b, int d, int f, int bar_reg)
{
    unsigned long orig, size_mask;
    unsigned long pci_cmd;

    /* Disable memory/IO decode during probe */
    pci_cmd = pci_rd32(b, d, f, 0x04);
    pci_wr32(b, d, f, 0x04, pci_cmd & ~0x03UL);

    orig = pci_rd32(b, d, f, bar_reg);
    pci_wr32(b, d, f, bar_reg, 0xFFFFFFFFUL);
    size_mask = pci_rd32(b, d, f, bar_reg);
    pci_wr32(b, d, f, bar_reg, orig);

    /* Restore command register */
    pci_wr32(b, d, f, 0x04, pci_cmd);

    if (size_mask == 0 || size_mask == 0xFFFFFFFF)
        return 0;

    if (orig & 1) {
        /* I/O BAR */
        size_mask &= 0xFFFCUL;
        return (~size_mask + 1) & 0xFFFFUL;
    } else {
        /* Memory BAR */
        size_mask &= 0xFFFFFFF0UL;
        return ~size_mask + 1;
    }
}

/* =============================================================== */
/*  VBE helpers                                                     */
/* =============================================================== */

static unsigned char *dosbuf(void)
{ return (unsigned char *)((unsigned long)g_dseg << 4); }

static int vbe_get_info(VBEInfo *out_info)
{
    RMI rm;
    memset(&rm, 0, sizeof rm);
    memset(dosbuf(), 0, 512);
    memcpy(dosbuf(), "VBE2", 4);
    rm.eax = 0x4F00;  rm.es = g_dseg;  rm.edi = 0;
    if (!dpmi_rmint(0x10, &rm)) return 0;
    if ((rm.eax & 0xFFFF) != 0x004F) return 0;
    memcpy(out_info, dosbuf(), sizeof(VBEInfo));
    return 1;
}

static int vbe_get_mode(unsigned short m, VBEMode *out_mode)
{
    RMI rm;
    memset(&rm, 0, sizeof rm);
    memset(dosbuf(), 0, 256);
    rm.eax = 0x4F01;  rm.ecx = m;  rm.es = g_dseg;  rm.edi = 0;
    if (!dpmi_rmint(0x10, &rm)) return 0;
    if ((rm.eax & 0xFFFF) != 0x004F) return 0;
    memcpy(out_mode, dosbuf(), sizeof(VBEMode));
    return 1;
}

/* =============================================================== */
/*  MMIO register access (parameterized by base pointer)            */
/* =============================================================== */

static unsigned long rreg_at(volatile unsigned long *base, unsigned long off)
{
    return base[off >> 2];
}

static void wreg_at(volatile unsigned long *base, unsigned long off,
                    unsigned long val)
{
    base[off >> 2] = val;
}

/* MC indirect read via given MMIO base */
static unsigned long mc_rreg_at(volatile unsigned long *base, unsigned long reg)
{
    unsigned long r;
    wreg_at(base, R_MC_IND_INDEX, 0x7F0000UL | (reg & 0xFFFF));
    r = rreg_at(base, R_MC_IND_DATA);
    wreg_at(base, R_MC_IND_INDEX, 0);
    return r;
}

/* =============================================================== */
/*  SECTION 1: Full PCI config space dump (256 bytes)               */
/* =============================================================== */

static void dump_pci_config(void)
{
    int i;
    unsigned char cfg[256];

    out("\n========================================\n");
    out("  SECTION 1: PCI Configuration Space\n");
    out("========================================\n\n");

    for (i = 0; i < 256; i++)
        cfg[i] = pci_rd8(g_pci_bus, g_pci_dev, g_pci_func, i);

    out("  Device: %02X:%02X.%X  %s\n",
        g_pci_bus, g_pci_dev, g_pci_func, g_card_name);
    out("  Vendor:Device = %04X:%04X\n", ATI_VID, g_pci_did);
    out("  Revision      = 0x%02X\n", cfg[0x08]);
    out("  Class Code    = %02X%02X%02X\n", cfg[0x0B], cfg[0x0A], cfg[0x09]);
    out("  Header Type   = 0x%02X\n", cfg[0x0E]);
    out("  Subsystem     = %04X:%04X\n",
        *(unsigned short *)(cfg + 0x2C),
        *(unsigned short *)(cfg + 0x2E));
    out("  Command       = 0x%04X\n",
        *(unsigned short *)(cfg + 0x04));
    out("  Status        = 0x%04X\n",
        *(unsigned short *)(cfg + 0x06));
    out("  Cache Line    = %d\n", cfg[0x0C]);
    out("  Lat Timer     = %d\n", cfg[0x0D]);
    out("  Int Line/Pin  = %d / %d\n", cfg[0x3C], cfg[0x3D]);

    /* Capabilities pointer */
    if (*(unsigned short *)(cfg + 0x06) & 0x0010) {
        unsigned char cap_ptr = cfg[0x34] & 0xFC;
        out("  Cap Pointer   = 0x%02X\n", cap_ptr);
        while (cap_ptr && cap_ptr < 0xFF) {
            unsigned char cap_id  = cfg[cap_ptr];
            unsigned char cap_nxt = cfg[cap_ptr + 1] & 0xFC;
            const char *cap_name = "Unknown";
            switch (cap_id) {
            case 0x01: cap_name = "Power Mgmt"; break;
            case 0x02: cap_name = "AGP"; break;
            case 0x05: cap_name = "MSI"; break;
            case 0x10: cap_name = "PCIe"; break;
            case 0x11: cap_name = "MSI-X"; break;
            }
            out("    Cap @0x%02X: ID=0x%02X (%s) next=0x%02X\n",
                cap_ptr, cap_id, cap_name, cap_nxt);
            if (cap_id == 0x10) {
                /* PCIe capability — dump link status */
                unsigned short pcie_flags = *(unsigned short *)(cfg + cap_ptr + 2);
                unsigned short link_cap  = *(unsigned short *)(cfg + cap_ptr + 12);
                unsigned short link_stat = *(unsigned short *)(cfg + cap_ptr + 18);
                out("      PCIe Flags : 0x%04X (Type=%d)\n",
                    pcie_flags, (pcie_flags >> 4) & 0xF);
                out("      Link Cap   : 0x%04X\n", link_cap);
                out("      Link Status: 0x%04X (speed=%d, width=x%d)\n",
                    link_stat, link_stat & 0xF, (link_stat >> 4) & 0x3F);
            }
            cap_ptr = cap_nxt;
        }
    }

    out("\n  Raw PCI config (hex):\n");
    out("       00 01 02 03 04 05 06 07 08 09 0A 0B 0C 0D 0E 0F\n");
    out("       -----------------------------------------------\n");
    for (i = 0; i < 256; i++) {
        if ((i & 0xF) == 0) out("  %02X:  ", i);
        out("%02X ", cfg[i]);
        if ((i & 0xF) == 0xF) out("\n");
    }
}

/* =============================================================== */
/*  SECTION 2: BAR analysis with size probing                       */
/* =============================================================== */

typedef struct {
    unsigned long raw;
    unsigned long phys;
    unsigned long size;
    int           is_io;
    int           is_64bit;
    int           is_pref;
} BarInfo;

static BarInfo g_bars[6];

static void dump_bars(void)
{
    int i;

    out("\n========================================\n");
    out("  SECTION 2: PCI BAR Analysis\n");
    out("========================================\n\n");

    for (i = 0; i < 6; i++) {
        unsigned long raw  = pci_rd32(g_pci_bus, g_pci_dev, g_pci_func,
                                      0x10 + i * 4);
        unsigned long size = bar_probe_size(g_pci_bus, g_pci_dev,
                                            g_pci_func, 0x10 + i * 4);

        g_bars[i].raw     = raw;
        g_bars[i].is_io   = (raw & 1) ? 1 : 0;
        g_bars[i].is_64bit = (!g_bars[i].is_io && ((raw & 0x06) == 0x04));
        g_bars[i].is_pref  = (!g_bars[i].is_io && (raw & 0x08));
        g_bars[i].phys    = g_bars[i].is_io ? (raw & 0xFFFCUL)
                                             : (raw & 0xFFFFFFF0UL);
        g_bars[i].size    = size;

        out("  BAR%d: raw=0x%08lX  phys=0x%08lX  size=", i, raw, g_bars[i].phys);
        if (size == 0) out("unused");
        else if (size >= 1024UL * 1024UL)
            out("%luMB", size / (1024UL * 1024UL));
        else if (size >= 1024)
            out("%luKB", size / 1024);
        else
            out("%lu bytes", size);

        if (g_bars[i].is_io) out("  [I/O]");
        else {
            out("  [Mem%s%s]",
                g_bars[i].is_64bit ? ",64b" : ",32b",
                g_bars[i].is_pref  ? ",pref" : ",non-pref");
        }
        out("\n");

        /* If 64-bit, show upper 32 bits and skip next BAR */
        if (g_bars[i].is_64bit && i < 5) {
            unsigned long upper = pci_rd32(g_pci_bus, g_pci_dev,
                                           g_pci_func, 0x14 + i * 4);
            out("        upper32=0x%08lX%s\n", upper,
                upper ? "  *** ABOVE 4GB! ***" : "  (below 4GB, OK)");
            i++;  /* skip partner BAR */
            g_bars[i].raw = upper;
            g_bars[i].size = 0;
        }
    }

    /* Interpret typical RV515 layout */
    out("\n  Expected RV515 PCIe BAR layout:\n");
    out("    BAR0 = VRAM aperture (128/256MB, prefetchable, 64-bit)\n");
    out("    BAR2 = MMIO registers (64KB, non-prefetchable, 32-bit)\n");
    out("    BAR4 = I/O ports (256 bytes, optional)\n");

    /* Identify which BAR is likely MMIO */
    {
        int mmio_bar = -1, vram_bar = -1, io_bar = -1;
        for (i = 0; i < 6; i++) {
            if (g_bars[i].size == 0) continue;
            if (g_bars[i].is_io) {
                io_bar = i;
            } else if (g_bars[i].size <= 256UL * 1024UL &&
                       !g_bars[i].is_pref) {
                mmio_bar = i;
            } else if (g_bars[i].size >= 16UL * 1024UL * 1024UL) {
                vram_bar = i;
            }
        }
        out("\n  Auto-detect guess:\n");
        out("    VRAM BAR = BAR%d", vram_bar);
        if (vram_bar >= 0)
            out(" (phys=0x%08lX, %luMB)\n",
                g_bars[vram_bar].phys,
                g_bars[vram_bar].size / (1024UL*1024UL));
        else out(" (NOT FOUND)\n");

        out("    MMIO BAR = BAR%d", mmio_bar);
        if (mmio_bar >= 0)
            out(" (phys=0x%08lX, %luKB)\n",
                g_bars[mmio_bar].phys,
                g_bars[mmio_bar].size / 1024);
        else out(" (NOT FOUND)\n");

        out("    I/O  BAR = BAR%d", io_bar);
        if (io_bar >= 0)
            out(" (port=0x%04lX, %lu bytes)\n",
                g_bars[io_bar].phys, g_bars[io_bar].size);
        else out(" (NOT FOUND)\n");
    }
}

/* =============================================================== */
/*  SECTION 3: MMIO probe — try each memory BAR for register reads  */
/* =============================================================== */

typedef struct {
    volatile unsigned long *base;
    unsigned long phys;
    unsigned long size;
    int bar_idx;
    int is_mmio;
    unsigned long config_memsize;
    unsigned long rbbm_status;
    unsigned long config_aper0_base;
    unsigned long config_reg1_base;
} MmioProbe;

static MmioProbe g_probes[6];
static volatile unsigned long *g_mmio = NULL;
static int g_mmio_bar = -1;

static void probe_bars_for_mmio(void)
{
    int i, best = -1;
    unsigned long best_score = 0;
    unsigned long bar0_phys = g_bars[0].phys;  /* expected in CONFIG_APER_0_BASE */

    out("\n========================================\n");
    out("  SECTION 3: MMIO BAR Identification\n");
    out("========================================\n\n");

    for (i = 0; i < 6; i++) {
        unsigned long map_sz;
        volatile unsigned long *p;
        unsigned long v_memsize, v_rbbm, v_aper0, v_reg1;
        unsigned long score = 0;

        g_probes[i].bar_idx = i;
        g_probes[i].base = NULL;
        g_probes[i].is_mmio = 0;

        if (g_bars[i].size == 0 || g_bars[i].is_io) continue;
        if (g_bars[i].phys == 0) continue;

        map_sz = g_bars[i].size;
        if (map_sz > 0x20000) map_sz = 0x20000;  /* map at most 128KB */

        out("  Probing BAR%d (phys=0x%08lX, map %luKB)...\n",
            i, g_bars[i].phys, map_sz / 1024);

        p = (volatile unsigned long *)dpmi_map(g_bars[i].phys, map_sz);
        if (!p) {
            out("    DPMI map FAILED\n");
            continue;
        }

        g_probes[i].base = p;
        g_probes[i].phys = g_bars[i].phys;
        g_probes[i].size = map_sz;

        /* Read diagnostic registers */
        v_memsize = rreg_at(p, R_CONFIG_MEMSIZE);
        v_rbbm    = rreg_at(p, R_RBBM_STATUS);
        v_aper0   = rreg_at(p, R_CONFIG_APER_0_BASE);
        v_reg1    = rreg_at(p, R_CONFIG_REG_1_BASE);

        g_probes[i].config_memsize   = v_memsize;
        g_probes[i].rbbm_status      = v_rbbm;
        g_probes[i].config_aper0_base = v_aper0;
        g_probes[i].config_reg1_base  = v_reg1;

        out("    CONFIG_MEMSIZE     = 0x%08lX", v_memsize);
        if (v_memsize >= 16UL*1024*1024 && v_memsize <= 512UL*1024*1024)
            out("  (%luMB - plausible VRAM)", v_memsize / (1024UL*1024));
        out("\n");

        out("    RBBM_STATUS        = 0x%08lX", v_rbbm);
        {
            unsigned long fifo = v_rbbm & RBBM_FIFOCNT_MASK;
            out("  (FIFO=%lu%s)",
                fifo, (v_rbbm & RBBM_ACTIVE) ? ",BUSY" : ",idle");
            /* On a valid MMIO mapping, idle FIFO should be 64 */
            if (fifo >= 16 && fifo <= 64 && !(v_rbbm & RBBM_ACTIVE))
                out("  <-- looks like real MMIO!");
        }
        out("\n");

        out("    CONFIG_APER_0_BASE = 0x%08lX", v_aper0);
        if (v_aper0 == bar0_phys && v_aper0 != 0)
            out("  (matches BAR0 - MMIO confirmed!)");
        out("\n");

        out("    CONFIG_REG_1_BASE  = 0x%08lX", v_reg1);
        if (v_reg1 == g_bars[i].phys && v_reg1 != 0)
            out("  (matches this BAR - self-reference!)");
        out("\n");

        /* Score this BAR as MMIO candidate */
        /* CONFIG_APER_0_BASE matching BAR0 is very strong evidence */
        if (v_aper0 == bar0_phys && v_aper0 != 0) score += 100;
        /* Self-reference: CONFIG_REG_1_BASE == this BAR's phys */
        if (v_reg1 == g_bars[i].phys && v_reg1 != 0) score += 100;
        /* Plausible VRAM size */
        if (v_memsize >= 16UL*1024*1024 && v_memsize <= 512UL*1024*1024)
            score += 50;
        /* RBBM FIFO in sane range for idle engine */
        if ((v_rbbm & RBBM_FIFOCNT_MASK) >= 16) score += 30;
        /* Non-prefetchable, <=128KB — typical for MMIO */
        if (!g_bars[i].is_pref && g_bars[i].size <= 128UL * 1024) score += 20;

        out("    MMIO score = %lu\n\n", score);

        if (score > best_score) {
            best_score = score;
            best = i;
        }
    }

    if (best >= 0 && best_score >= 50) {
        g_mmio = g_probes[best].base;
        g_mmio_bar = best;
        out("  >>> Best MMIO candidate: BAR%d (score=%lu)\n", best, best_score);
    } else {
        out("  >>> WARNING: No confident MMIO BAR found!\n");
        /* Fall back to non-prefetchable memory BAR */
        for (i = 0; i < 6; i++) {
            if (g_probes[i].base && !g_bars[i].is_pref &&
                g_bars[i].size > 0 && g_bars[i].size <= 256UL*1024) {
                g_mmio = g_probes[i].base;
                g_mmio_bar = i;
                out("  >>> Fallback: using BAR%d (non-pref, %luKB)\n",
                    i, g_bars[i].size / 1024);
                break;
            }
        }
    }
}

/* =============================================================== */
/*  SECTION 4: GPU register dump from identified MMIO               */
/* =============================================================== */

typedef struct { unsigned long off; const char *name; } RegDef;

static const RegDef g_regs[] = {
    /* Config / identification */
    { 0x0000, "MM_INDEX" },
    { 0x0004, "MM_DATA" },
    { 0x0008, "CLOCK_CNTL_INDEX" },
    { 0x000C, "CLOCK_CNTL_DATA" },
    { 0x0070, "MC_IND_INDEX" },
    { 0x0074, "MC_IND_DATA" },
    { 0x00F0, "RBBM_SOFT_RESET" },
    { 0x00F8, "CONFIG_MEMSIZE" },
    { 0x0100, "CONFIG_APER_0_BASE" },
    { 0x0108, "CONFIG_APER_SIZE" },
    { 0x010C, "CONFIG_REG_1_BASE" },
    { 0x0110, "CONFIG_REG_APER_SIZE" },
    { 0x0130, "HOST_PATH_CNTL" },
    { 0x0134, "HDP_FB_LOCATION" },
    { 0x0150, "HDP_SOFT_RESET" },

    /* VGA */
    { 0x0300, "VGA_RENDER_CONTROL" },
    { 0x0330, "D1VGA_CONTROL" },
    { 0x0338, "D2VGA_CONTROL" },

    /* Display */
    { 0x0050, "CRTC_GEN_CNTL" },
    { 0x005C, "CRTC_EXT_CNTL" },
    { 0x0068, "CRTC_OFFSET" },
    { 0x006C, "CRTC_PITCH" },

    /* Surface / MMIO access */
    { 0x0B00, "SURFACE_CNTL" },
    { 0x0B04, "SURFACE0_INFO" },
    { 0x0B0C, "SURFACE0_LOWER" },
    { 0x0B10, "SURFACE0_UPPER" },

    /* RBBM */
    { 0x0E40, "RBBM_STATUS" },
    { 0x0E44, "RBBM_CNTL" },

    /* 2D engine status/control */
    { 0x1404, "DST_OFFSET" },
    { 0x1408, "DST_PITCH" },
    { 0x1428, "SRC_PITCH_OFFSET" },
    { 0x142C, "DST_PITCH_OFFSET" },
    { 0x146C, "DP_GUI_MASTER_CNTL" },
    { 0x1478, "DP_BRUSH_BKGD_CLR" },
    { 0x147C, "DP_BRUSH_FRGD_CLR" },
    { 0x15AC, "SRC_OFFSET" },
    { 0x15B0, "SRC_PITCH" },
    { 0x15D8, "DP_SRC_FRGD_CLR" },
    { 0x15DC, "DP_SRC_BKGD_CLR" },
    { 0x16C0, "DP_CNTL" },
    { 0x16C4, "DP_DATATYPE" },
    { 0x16C8, "DP_MIX" },
    { 0x16CC, "DP_WRITE_MASK" },
    { 0x16E0, "DEFAULT_PITCH_OFFSET" },
    { 0x16E8, "DEFAULT_SC_BOTTOM_RIGHT" },
    { 0x16EC, "SC_TOP_LEFT" },
    { 0x16F0, "SC_BOTTOM_RIGHT" },
    { 0x170C, "DST_PIPE_CONFIG" },
    { 0x1720, "WAIT_UNTIL" },
    { 0x1724, "ISYNC_CNTL" },
    { 0x172C, "RBBM_GUICNTL" },

    /* R300+ 2D cache */
    { 0x3258, "RB3D_DSTCACHE_MODE" },
    { 0x3428, "RB2D_DSTCACHE_MODE" },
    { 0x342C, "RB2D_DSTCACHE_CTLSTAT" },

    /* Graphics block */
    { 0x4008, "GB_ENABLE" },
    { 0x4010, "GB_MSPOS0" },
    { 0x4014, "GB_MSPOS1" },
    { 0x4018, "GB_TILE_CONFIG" },
    { 0x401C, "GB_SELECT" },
    { 0x4020, "GB_AA_CONFIG" },
    { 0x402C, "GB_PIPE_SELECT" },

    /* Geometry arbiter */
    { 0x4274, "GA_ENHANCE" },

    /* Shader unit */
    { 0x42C8, "SU_REG_DEST" },

    /* VAP */
    { 0x208C, "VAP_INDEX_OFFSET" },

    /* RB3D / ZB */
    { 0x4E4C, "RB3D_DSTCACHE_CTLSTAT" },
    { 0x4F18, "ZB_ZCACHE_CTLSTAT" },

    { 0, NULL }
};

static void dump_gpu_registers(void)
{
    int i;

    out("\n========================================\n");
    out("  SECTION 4: GPU Register Dump\n");
    out("========================================\n\n");

    if (!g_mmio) {
        out("  MMIO not available — skipping register dump.\n");
        return;
    }

    out("  Using BAR%d at phys=0x%08lX, linear=0x%08lX\n\n",
        g_mmio_bar, g_bars[g_mmio_bar].phys, (unsigned long)g_mmio);

    for (i = 0; g_regs[i].name; i++) {
        unsigned long val;
        /* Don't read beyond mapped region */
        if (g_regs[i].off >= g_probes[g_mmio_bar].size) {
            out("  0x%04lX %-28s = (beyond mapped range)\n",
                g_regs[i].off, g_regs[i].name);
            continue;
        }
        val = rreg_at(g_mmio, g_regs[i].off);
        out("  0x%04lX %-28s = 0x%08lX", g_regs[i].off, g_regs[i].name, val);

        /* Annotate key values */
        if (g_regs[i].off == R_CONFIG_MEMSIZE && val > 0)
            out("  (%luMB)", val / (1024UL*1024));
        if (g_regs[i].off == R_RBBM_STATUS)
            out("  (FIFO=%lu, %s)",
                val & RBBM_FIFOCNT_MASK,
                (val & RBBM_ACTIVE) ? "BUSY" : "idle");
        if (g_regs[i].off == R_GB_PIPE_SELECT)
            out("  (%d pipes)", (int)((val >> 12) & 3) + 1);

        out("\n");
    }

    /* MC indirect registers */
    out("\n  MC Indirect Registers (via MC_IND_INDEX/DATA):\n");
    {
        static const struct { unsigned long idx; const char *name; } mc_regs[] = {
            { 0x0000, "MC_STATUS" },
            { 0x0001, "MC_FB_LOCATION" },
            { 0x0002, "MC_AGP_LOCATION" },
            { 0x0003, "MC_AGP_BASE" },
            { 0x0004, "MC_AGP_BASE_2" },
            { 0x0005, "MC_CNTL" },
            { 0x0006, "MC_STATUS (alt)" },
            { 0x0009, "MC_MISC_LAT_TIMER" },
            { 0x000C, "MC_MISC_UMA_CNTL" },
            { 0, NULL }
        };
        for (i = 0; mc_regs[i].name; i++) {
            unsigned long val = mc_rreg_at(g_mmio, mc_regs[i].idx);
            out("  MC[0x%04lX] %-24s = 0x%08lX", mc_regs[i].idx,
                mc_regs[i].name, val);
            if (mc_regs[i].idx == 0x0000)
                out("  (%s)", (val & (1UL<<4)) ? "idle" : "BUSY");
            if (mc_regs[i].idx == 0x0001) {
                unsigned long fb_top = ((val >> 16) & 0xFFFFUL) << 16;
                unsigned long fb_bot = (val & 0xFFFFUL) << 16;
                out("  (bot=0x%08lX top=0x%08lX)", fb_bot, fb_top);
            }
            out("\n");
        }
    }

    /* PLL indirect registers (a few key ones) */
    out("\n  PLL Indirect Registers (via CLOCK_CNTL_INDEX/DATA):\n");
    {
        static const struct { unsigned long idx; const char *name; } pll_regs[] = {
            { 0x0003, "PPLL_REF_DIV" },
            { 0x0004, "PPLL_DIV_0" },
            { 0x0007, "PPLL_CNTL" },
            { 0x000A, "SPLL_CNTL" },
            { 0x000B, "SCLK_CNTL" },
            { 0x000D, "DYN_SCLK_PWMEM_PIPE" },
            { 0x0015, "CLK_PIN_CNTL" },
            { 0, NULL }
        };
        for (i = 0; pll_regs[i].name; i++) {
            unsigned long val;
            wreg_at(g_mmio, R_CLOCK_CNTL_INDEX,
                    pll_regs[i].idx & 0x3FUL);
            val = rreg_at(g_mmio, R_CLOCK_CNTL_DATA);
            out("  PLL[0x%02lX] %-24s = 0x%08lX\n",
                pll_regs[i].idx, pll_regs[i].name, val);
        }
    }
}

/* =============================================================== */
/*  SECTION 5: MMIO read-back verification                          */
/* =============================================================== */

static void test_mmio_readback(void)
{
    out("\n========================================\n");
    out("  SECTION 5: MMIO Read-Back Test\n");
    out("========================================\n\n");

    if (!g_mmio) {
        out("  MMIO not available — skipping.\n");
        return;
    }

    /* Test 1: Write a known pattern to DEFAULT_PITCH_OFFSET and read back.
       This is a direct-accessible 2D register (not FIFO-submitted). */
    {
        unsigned long orig, v1, v2;
        orig = rreg_at(g_mmio, R_DEFAULT_PITCH_OFFSET);
        out("  Test 1: DEFAULT_PITCH_OFFSET (direct r/w register)\n");
        out("    Original = 0x%08lX\n", orig);

        wreg_at(g_mmio, R_DEFAULT_PITCH_OFFSET, 0x00A50000UL);
        v1 = rreg_at(g_mmio, R_DEFAULT_PITCH_OFFSET);
        out("    Write 0x00A50000, read = 0x%08lX  %s\n",
            v1, (v1 == 0x00A50000UL) ? "OK" : "MISMATCH!");

        wreg_at(g_mmio, R_DEFAULT_PITCH_OFFSET, 0x005A0000UL);
        v2 = rreg_at(g_mmio, R_DEFAULT_PITCH_OFFSET);
        out("    Write 0x005A0000, read = 0x%08lX  %s\n",
            v2, (v2 == 0x005A0000UL) ? "OK" : "MISMATCH!");

        /* Restore */
        wreg_at(g_mmio, R_DEFAULT_PITCH_OFFSET, orig);

        if (v1 == 0x00A50000UL && v2 == 0x005A0000UL)
            out("    Result: MMIO read-back is WORKING.\n");
        else if (v1 == v2)
            out("    Result: MMIO read-back FAILED (reads constant 0x%08lX).\n"
                "    This BAR is likely VRAM, not MMIO!\n", v1);
        else
            out("    Result: MMIO read-back INCONSISTENT.\n");
    }

    /* Test 2: SURFACE_CNTL — another known-readable register */
    {
        unsigned long orig, v1;
        orig = rreg_at(g_mmio, R_SURFACE_CNTL);
        out("\n  Test 2: SURFACE_CNTL (direct r/w register)\n");
        out("    Original = 0x%08lX\n", orig);

        wreg_at(g_mmio, R_SURFACE_CNTL, 0x00000200UL);
        v1 = rreg_at(g_mmio, R_SURFACE_CNTL);
        out("    Write 0x00000200, read = 0x%08lX  %s\n",
            v1, (v1 == 0x00000200UL) ? "OK" : "MISMATCH!");

        /* Restore */
        wreg_at(g_mmio, R_SURFACE_CNTL, orig);
    }

    /* Test 3: DP_WRITE_MASK — FIFO-submitted register on R300+/R500.
       On these GPUs, writes go through the RBBM command FIFO and
       reads return 0 (no shadow register). This is NORMAL. */
    {
        unsigned long orig, v1;
        orig = rreg_at(g_mmio, R_DP_WRITE_MASK);
        out("\n  Test 3: DP_WRITE_MASK (FIFO-submitted, write-only on R500)\n");
        out("    Original = 0x%08lX\n", orig);

        wreg_at(g_mmio, R_DP_WRITE_MASK, 0xA5A5A5A5UL);
        v1 = rreg_at(g_mmio, R_DP_WRITE_MASK);
        out("    Write 0xA5A5A5A5, read = 0x%08lX", v1);
        if (v1 == 0xA5A5A5A5UL)
            out("  OK (readable on this chip)\n");
        else if (v1 == 0 || v1 == orig)
            out("  (expected — FIFO-submitted, no readback)\n");
        else
            out("  UNEXPECTED\n");

        /* Restore */
        wreg_at(g_mmio, R_DP_WRITE_MASK, orig);
    }

    /* Test: check CONFIG_MEMSIZE against PCI config space value */
    {
        unsigned long mm_memsize = rreg_at(g_mmio, R_CONFIG_MEMSIZE);
        out("\n  CONFIG_MEMSIZE = 0x%08lX", mm_memsize);
        if (mm_memsize >= 16UL*1024*1024 && mm_memsize <= 512UL*1024*1024)
            out(" (%luMB — plausible)\n", mm_memsize / (1024UL*1024));
        else
            out(" (IMPLAUSIBLE for X1300 — expected 64-256MB)\n");
    }

    /* Test: RBBM_STATUS FIFO count */
    {
        unsigned long rbbm = rreg_at(g_mmio, R_RBBM_STATUS);
        unsigned long fifo = rbbm & RBBM_FIFOCNT_MASK;
        out("  RBBM_STATUS = 0x%08lX  FIFO=%lu\n", rbbm, fifo);
        if (fifo == 0)
            out("  *** FIFO=0 is abnormal for idle engine (expect ~64) ***\n"
                "  *** This strongly suggests MMIO mapping is wrong! ***\n");
        else if (fifo >= 16 && fifo <= 64)
            out("  FIFO count looks healthy.\n");
    }

    /* Test: CONFIG_APER_0_BASE should match BAR0 physical address */
    {
        unsigned long aper0 = rreg_at(g_mmio, R_CONFIG_APER_0_BASE);
        out("  CONFIG_APER_0_BASE = 0x%08lX  (BAR0 phys = 0x%08lX)  %s\n",
            aper0, g_bars[0].phys,
            (aper0 == g_bars[0].phys) ? "MATCH" : "MISMATCH");
    }

    /* Test: CONFIG_REG_1_BASE should match MMIO BAR physical address */
    {
        unsigned long reg1 = rreg_at(g_mmio, R_CONFIG_REG_1_BASE);
        unsigned long mmio_phys = g_bars[g_mmio_bar].phys;
        out("  CONFIG_REG_1_BASE  = 0x%08lX  (this BAR = 0x%08lX)  %s\n",
            reg1, mmio_phys,
            (reg1 == mmio_phys) ? "MATCH" : "MISMATCH");
    }
}

/* =============================================================== */
/*  SECTION 6: VRAM access test                                     */
/* =============================================================== */

static void test_vram_access(void)
{
    int vram_bar = -1;
    int i;
    unsigned long map_sz;
    unsigned char *vram;

    out("\n========================================\n");
    out("  SECTION 6: VRAM Direct Access Test\n");
    out("========================================\n\n");

    /* Find the VRAM BAR (largest prefetchable memory BAR) */
    for (i = 0; i < 6; i++) {
        if (g_bars[i].size >= 16UL*1024*1024 && !g_bars[i].is_io) {
            if (vram_bar < 0 || g_bars[i].size > g_bars[vram_bar].size)
                vram_bar = i;
        }
    }
    if (vram_bar < 0) {
        out("  No VRAM BAR found.\n");
        return;
    }

    out("  VRAM BAR%d: phys=0x%08lX, size=%luMB\n",
        vram_bar, g_bars[vram_bar].phys,
        g_bars[vram_bar].size / (1024UL*1024));

    map_sz = 4096;  /* Just map 1 page for testing */
    vram = (unsigned char *)dpmi_map(g_bars[vram_bar].phys, map_sz);
    if (!vram) {
        out("  DPMI map FAILED for VRAM BAR!\n");
        return;
    }
    out("  Mapped at linear 0x%08lX (%lu bytes)\n",
        (unsigned long)vram, map_sz);

    /* Read first 16 bytes */
    out("  First 16 bytes: ");
    for (i = 0; i < 16; i++)
        out("%02X ", vram[i]);
    out("\n");

    /* Write/read test at offset 0 */
    {
        unsigned char orig = vram[0];
        unsigned char test_val = 0xBE;
        unsigned char readback;

        vram[0] = test_val;
        readback = vram[0];
        out("  Write 0x%02X to offset 0, read back 0x%02X — %s\n",
            test_val, readback,
            (readback == test_val) ? "OK" : "FAIL");
        vram[0] = orig;
    }

    /* Write pattern to first 256 bytes and verify */
    {
        unsigned char save[256];
        int pass = 1;

        memcpy(save, (void *)vram, 256);
        for (i = 0; i < 256; i++)
            vram[i] = (unsigned char)i;
        for (i = 0; i < 256; i++) {
            if (vram[i] != (unsigned char)i) {
                out("  Pattern fail at offset %d: wrote 0x%02X, read 0x%02X\n",
                    i, i, vram[i]);
                pass = 0;
                break;
            }
        }
        memcpy((void *)vram, save, 256);

        if (pass)
            out("  256-byte pattern test: PASSED\n");
        else
            out("  256-byte pattern test: FAILED\n");
    }

    dpmi_unmap(vram);
}

/* =============================================================== */
/*  SECTION 7: VBE/VESA information                                 */
/* =============================================================== */

static void dump_vbe_info(void)
{
    VBEInfo vi;
    VBEMode mi;
    unsigned short *ml;
    unsigned long seg, off_v;
    int i, cnt;

    out("\n========================================\n");
    out("  SECTION 7: VBE/VESA Information\n");
    out("========================================\n\n");

    if (!vbe_get_info(&vi)) {
        out("  VBE info call FAILED.\n");
        return;
    }

    out("  VBE Version   : %d.%d\n", vi.ver >> 8, vi.ver & 0xFF);
    out("  Total Memory  : %d x 64KB = %dKB\n",
        vi.tot_mem, vi.tot_mem * 64);
    out("  OEM String ptr: 0x%08lX\n", vi.oem_str);

    if (vi.oem_str) {
        char *oem = (char *)(((vi.oem_str >> 16) << 4) +
                              (vi.oem_str & 0xFFFF));
        out("  OEM String    : %.80s\n", oem);
    }

    if (vi.ver >= 0x0200) {
        out("  OEM Soft Rev  : 0x%04X\n", vi.oem_soft_rev);
    }

    out("  Capabilities  : 0x%08lX\n", vi.caps);

    /* List 8bpp modes with LFB */
    out("\n  8bpp modes with LFB:\n");
    seg = (vi.mode_ptr >> 16) & 0xFFFF;
    off_v = vi.mode_ptr & 0xFFFF;
    ml  = (unsigned short *)((seg << 4) + off_v);

    for (cnt = 0; cnt < 511 && ml[cnt] != 0xFFFF; cnt++)
        ;

    for (i = 0; i < cnt; i++) {
        if (!vbe_get_mode(ml[i], &mi)) continue;
        if (mi.bpp != 8) continue;
        if (!(mi.attr & 0x80)) continue;  /* need LFB */
        out("    Mode 0x%03X: %4dx%4d  pitch=%4d  LFB=0x%08lX  model=%d  attr=0x%04X\n",
            ml[i], mi.xres, mi.yres, mi.pitch, mi.lfb_phys,
            mi.model, mi.attr);
    }

    /* Also list 16bpp and 32bpp modes for completeness */
    out("\n  16/32bpp modes with LFB:\n");
    for (i = 0; i < cnt; i++) {
        if (!vbe_get_mode(ml[i], &mi)) continue;
        if (mi.bpp != 16 && mi.bpp != 32) continue;
        if (!(mi.attr & 0x80)) continue;
        out("    Mode 0x%03X: %4dx%4d  %2dbpp  pitch=%4d  LFB=0x%08lX\n",
            ml[i], mi.xres, mi.yres, mi.bpp, mi.pitch, mi.lfb_phys);
    }
}

/* =============================================================== */
/*  SECTION 8: FB location analysis                                 */
/* =============================================================== */

static void analyze_fb_location(void)
{
    unsigned long hdp_fb, mc_fb, fb_base;

    out("\n========================================\n");
    out("  SECTION 8: Framebuffer Location Analysis\n");
    out("========================================\n\n");

    if (!g_mmio) {
        out("  MMIO not available.\n");
        return;
    }

    hdp_fb = rreg_at(g_mmio, R_HDP_FB_LOCATION);
    mc_fb  = mc_rreg_at(g_mmio, RV515_MC_FB_LOCATION);
    fb_base = (hdp_fb & 0xFFFFUL) << 16;

    out("  HDP_FB_LOCATION = 0x%08lX\n", hdp_fb);
    out("    Low 16 bits (FB base >> 16) = 0x%04lX -> FB base = 0x%08lX\n",
        hdp_fb & 0xFFFF, fb_base);
    out("    High 16 bits (FB top >> 16) = 0x%04lX -> FB top  = 0x%08lX\n",
        (hdp_fb >> 16) & 0xFFFF, ((hdp_fb >> 16) & 0xFFFFUL) << 16);

    out("  MC_FB_LOCATION  = 0x%08lX  (indirect MC reg 0x01)\n", mc_fb);
    out("    MC FB base = 0x%08lX\n", (mc_fb & 0xFFFFUL) << 16);
    out("    MC FB top  = 0x%08lX\n", ((mc_fb >> 16) & 0xFFFFUL) << 16);

    out("\n  For 2D engine DST_PITCH_OFFSET:\n");
    out("    offset field = fb_base >> 10 = 0x%08lX\n",
        (fb_base >> 10) & 0x003FFFFFUL);
    out("    This must be in [0, 0x3FFFFF] range: %s\n",
        ((fb_base >> 10) <= 0x3FFFFF) ? "OK" : "OUT OF RANGE!");

    if (fb_base == 0) {
        out("\n  FB base = 0: GPU sees framebuffer at physical address 0.\n");
        out("  The 2D engine offset field would be 0 — CORRECT for most BIOS setups.\n");
    } else {
        out("\n  FB base = 0x%08lX (non-zero).\n", fb_base);
        out("  The 2D engine needs this added to its destination offset.\n");
        if (fb_base >= 0x40000000UL) {
            out("  *** WARNING: FB base seems very high. If MMIO BAR is\n");
            out("  *** wrong, this is garbage from VRAM, not a real register!\n");
        }
    }
}

/* =============================================================== */
/*  SECTION 9: GPU 2D engine test (simple register-only)            */
/* =============================================================== */

static void test_2d_engine(void)
{
    unsigned long rbbm, fifo;
    unsigned long orig_dpo;
    int timeout;

    out("\n========================================\n");
    out("  SECTION 9: GPU 2D Engine Test\n");
    out("========================================\n\n");

    if (!g_mmio) {
        out("  MMIO not available.\n");
        return;
    }

    /* Check FIFO availability */
    rbbm = rreg_at(g_mmio, R_RBBM_STATUS);
    fifo = rbbm & RBBM_FIFOCNT_MASK;
    out("  Pre-test RBBM_STATUS = 0x%08lX (FIFO=%lu, %s)\n",
        rbbm, fifo, (rbbm & RBBM_ACTIVE) ? "BUSY" : "idle");

    if (fifo < 6) {
        out("  Insufficient FIFO entries (%lu < 6). Engine may be hung.\n", fifo);
        out("  Attempting soft reset...\n");

        /* Try soft reset of 2D engine */
        wreg_at(g_mmio, R_RBBM_SOFT_RESET,
                (1UL << 0) | (1UL << 1) | (1UL << 5));
        (void)rreg_at(g_mmio, R_RBBM_SOFT_RESET);
        wreg_at(g_mmio, R_RBBM_SOFT_RESET, 0);
        (void)rreg_at(g_mmio, R_RBBM_SOFT_RESET);

        rbbm = rreg_at(g_mmio, R_RBBM_STATUS);
        fifo = rbbm & RBBM_FIFOCNT_MASK;
        out("  After reset: RBBM_STATUS = 0x%08lX (FIFO=%lu)\n", rbbm, fifo);

        if (fifo < 6) {
            out("  Engine still has no FIFO space — aborting 2D test.\n");
            return;
        }
    }

    /* Save and test DST_PITCH_OFFSET write-back */
    orig_dpo = rreg_at(g_mmio, R_DST_PITCH_OFFSET);
    out("\n  DST_PITCH_OFFSET readback test:\n");
    out("    Current value = 0x%08lX\n", orig_dpo);

    wreg_at(g_mmio, R_DST_PITCH_OFFSET, 0x00D00000UL);
    {
        unsigned long rb = rreg_at(g_mmio, R_DST_PITCH_OFFSET);
        out("    Write 0x00D00000, read = 0x%08lX  %s\n",
            rb, (rb == 0x00D00000UL) ? "OK" : "MISMATCH");
    }

    wreg_at(g_mmio, R_DST_PITCH_OFFSET, 0x03400000UL);
    {
        unsigned long rb = rreg_at(g_mmio, R_DST_PITCH_OFFSET);
        out("    Write 0x03400000, read = 0x%08lX  %s\n",
            rb, (rb == 0x03400000UL) ? "OK" : "MISMATCH");
    }

    /* Restore */
    wreg_at(g_mmio, R_DST_PITCH_OFFSET, orig_dpo);

    /* Wait for idle */
    out("\n  Wait-for-idle test:\n");
    timeout = 100000;
    while (timeout > 0) {
        rbbm = rreg_at(g_mmio, R_RBBM_STATUS);
        if (!(rbbm & RBBM_ACTIVE)) break;
        timeout--;
    }
    if (timeout <= 0)
        out("    Engine did NOT go idle (RBBM_STATUS=0x%08lX)\n", rbbm);
    else
        out("    Engine is idle (RBBM_STATUS=0x%08lX, FIFO=%lu)\n",
            rbbm, rbbm & RBBM_FIFOCNT_MASK);
}

/* =============================================================== */
/*  SECTION 10: Cross-BAR comparison                                */
/* =============================================================== */

static void cross_bar_compare(void)
{
    int i;

    out("\n========================================\n");
    out("  SECTION 10: Cross-BAR Register Comparison\n");
    out("========================================\n\n");
    out("  Reading key registers from each mapped BAR to identify\n");
    out("  which returns real MMIO values vs. VRAM garbage.\n\n");

    out("  %-6s %-12s %-12s %-12s %-12s %-12s\n",
        "BAR#", "CFG_MEMSIZE", "RBBM_STATUS", "APER_0_BASE",
        "REG_1_BASE", "Verdict");
    out("  %-6s %-12s %-12s %-12s %-12s %-12s\n",
        "----", "-----------", "-----------", "-----------",
        "-----------", "-------");

    for (i = 0; i < 6; i++) {
        unsigned long v_ms, v_rb, v_a0, v_r1;
        const char *verdict;

        if (!g_probes[i].base) continue;

        v_ms = g_probes[i].config_memsize;
        v_rb = g_probes[i].rbbm_status;
        v_a0 = g_probes[i].config_aper0_base;
        v_r1 = g_probes[i].config_reg1_base;

        /* Determine verdict */
        if (v_a0 == g_bars[0].phys && v_a0 != 0 &&
            (v_rb & RBBM_FIFOCNT_MASK) >= 16)
            verdict = "** MMIO **";
        else if (v_a0 == g_bars[0].phys && v_a0 != 0)
            verdict = "Likely MMIO";
        else if ((v_rb & RBBM_FIFOCNT_MASK) == 0 &&
                 g_bars[i].size >= 16UL*1024*1024)
            verdict = "Likely VRAM";
        else
            verdict = "Uncertain";

        out("  BAR%-2d 0x%08lX   0x%08lX   0x%08lX   0x%08lX   %s\n",
            i, v_ms, v_rb, v_a0, v_r1, verdict);
    }
}

/* =============================================================== */
/*  SECTION 11: PCI command register & bus master analysis          */
/* =============================================================== */

static void analyze_pci_command(void)
{
    unsigned long cmd;

    out("\n========================================\n");
    out("  SECTION 11: PCI Command & Bus Master\n");
    out("========================================\n\n");

    cmd = pci_rd32(g_pci_bus, g_pci_dev, g_pci_func, 0x04);
    out("  PCI Command = 0x%04lX\n", cmd & 0xFFFF);
    out("    Bit 0 (I/O Space)     : %s\n", (cmd & 0x01) ? "ENABLED" : "disabled");
    out("    Bit 1 (Memory Space)  : %s\n", (cmd & 0x02) ? "ENABLED" : "disabled");
    out("    Bit 2 (Bus Master)    : %s\n", (cmd & 0x04) ? "ENABLED" : "disabled");
    out("    Bit 3 (Special Cycles): %s\n", (cmd & 0x08) ? "ENABLED" : "disabled");
    out("    Bit 4 (MWI Enable)    : %s\n", (cmd & 0x10) ? "ENABLED" : "disabled");
    out("    Bit 6 (Parity Error)  : %s\n", (cmd & 0x40) ? "ENABLED" : "disabled");
    out("    Bit 8 (SERR# Enable)  : %s\n", (cmd & 0x100) ? "ENABLED" : "disabled");
    out("    Bit 10 (INTx Disable) : %s\n", (cmd & 0x400) ? "YES" : "no");

    out("\n  PCI Status = 0x%04lX\n", (cmd >> 16) & 0xFFFF);
    {
        unsigned long st = (cmd >> 16) & 0xFFFF;
        out("    Bit 3 (Int Status)    : %s\n", (st & 0x08) ? "pending" : "no");
        out("    Bit 4 (Cap List)      : %s\n", (st & 0x10) ? "YES" : "no");
        out("    Bit 5 (66MHz)         : %s\n", (st & 0x20) ? "YES" : "no");
        out("    Bit 8 (Data Parity)   : %s\n", (st & 0x100) ? "detected" : "no");
        out("    Bit 11(Sig Tgt Abort) : %s\n", (st & 0x800) ? "YES" : "no");
        out("    Bit 12(Rcv Tgt Abort) : %s\n", (st & 0x1000) ? "YES" : "no");
        out("    Bit 13(Rcv Mst Abort) : %s\n", (st & 0x2000) ? "YES" : "no");
        out("    Bit 14(Sig Sys Error) : %s\n", (st & 0x4000) ? "YES" : "no");
        out("    Bit 15(Parity Error)  : %s\n", (st & 0x8000) ? "detected" : "no");
    }

    if (!(cmd & 0x02)) {
        out("\n  *** CRITICAL: Memory Space is DISABLED! ***\n");
        out("  MMIO/VRAM BARs cannot be accessed. Enabling...\n");
        pci_wr32(g_pci_bus, g_pci_dev, g_pci_func, 0x04,
                 cmd | 0x06);
        cmd = pci_rd32(g_pci_bus, g_pci_dev, g_pci_func, 0x04);
        out("  After enable: Command = 0x%04lX\n", cmd & 0xFFFF);
    }

    if (!(cmd & 0x04)) {
        out("\n  NOTE: Bus Master is disabled.\n");
        out("  Required for DMA / ring-buffer commands.\n");
        out("  For MMIO-only 2D engine access, this may be OK.\n");
    }
}

/* =============================================================== */
/*  SECTION 12: I/O port access test (legacy VGA, ATI indirect)     */
/* =============================================================== */

static void test_io_ports(void)
{
    out("\n========================================\n");
    out("  SECTION 12: I/O Port Access Test\n");
    out("========================================\n\n");

    /* Test legacy VGA I/O at 0x3D4/0x3D5 */
    {
        unsigned char idx, val;
        outp(0x3D4, 0x00);  /* CRTC index 0 = H Total */
        val = (unsigned char)inp(0x3D5);
        out("  VGA CRTC[0x00] (H Total)   = 0x%02X\n", val);

        outp(0x3D4, 0x01);  /* CRTC H Display End */
        val = (unsigned char)inp(0x3D5);
        out("  VGA CRTC[0x01] (H Disp End)= 0x%02X\n", val);

        outp(0x3D4, 0x12);  /* V Display End */
        val = (unsigned char)inp(0x3D5);
        out("  VGA CRTC[0x12] (V Disp End)= 0x%02X\n", val);
    }

    /* Test ATI specific I/O if BAR4 exists */
    {
        int io_bar = -1;
        int i;
        for (i = 0; i < 6; i++) {
            if (g_bars[i].is_io && g_bars[i].size > 0) {
                io_bar = i;
                break;
            }
        }
        if (io_bar >= 0) {
            unsigned short base = (unsigned short)(g_bars[io_bar].phys & 0xFFFF);
            unsigned long v;
            out("\n  ATI I/O BAR%d at port 0x%04X:\n", io_bar, base);
            /* ATI I/O mapped registers: offset 0 = MM_INDEX, 4 = MM_DATA */
            outpd(base, R_CONFIG_MEMSIZE);
            v = inpd(base + 4);
            out("    I/O indirect CONFIG_MEMSIZE = 0x%08lX", v);
            if (v >= 16UL*1024*1024 && v <= 512UL*1024*1024)
                out("  (%luMB)", v / (1024UL*1024));
            out("\n");

            outpd(base, R_RBBM_STATUS);
            v = inpd(base + 4);
            out("    I/O indirect RBBM_STATUS    = 0x%08lX (FIFO=%lu)\n",
                v, v & RBBM_FIFOCNT_MASK);

            outpd(base, R_CONFIG_APER_0_BASE);
            v = inpd(base + 4);
            out("    I/O indirect APER_0_BASE    = 0x%08lX\n", v);

            outpd(base, R_CONFIG_REG_1_BASE);
            v = inpd(base + 4);
            out("    I/O indirect REG_1_BASE     = 0x%08lX\n", v);

            outpd(base, R_HDP_FB_LOCATION);
            v = inpd(base + 4);
            out("    I/O indirect HDP_FB_LOC     = 0x%08lX\n", v);
        } else {
            out("\n  No I/O BAR found (ATI indirect I/O not available).\n");
        }
    }
}

/* =============================================================== */
/*  SECTION 13: Environment information                             */
/* =============================================================== */

static void dump_environment(void)
{
    out("\n========================================\n");
    out("  SECTION 13: Environment\n");
    out("========================================\n\n");

    dpmi_version_info();

    /* DOS extender info via INT 21h/AX=3306h (get true DOS version) */
    {
        RMI rm;
        memset(&rm, 0, sizeof rm);
        rm.eax = 0x3306;
        dpmi_rmint(0x21, &rm);
        out("  DOS version    : %d.%d\n",
            (int)(rm.ebx & 0xFF), (int)((rm.ebx >> 8) & 0xFF));
    }

    /* Check if running under PMODE/W or DOS/4GW */
    out("  Extender       : %s\n",
        getenv("DOS4G") ? "DOS/4GW (DOS4G env set)" : "(PMODE/W or other)");

    /* Timestamp */
    {
        time_t t = time(NULL);
        out("  Timestamp      : %s", ctime(&t));
    }
}

/* =============================================================== */
/*  SECTION 14: I/O Port Indirect Register Write Test               */
/*  Tests writing GPU registers via I/O BAR MM_INDEX/MM_DATA,       */
/*  bypassing the MMIO BAR memory mapping entirely.                 */
/* =============================================================== */

static void test_io_indirect_write(void)
{
    int io_bar = -1;
    int i;
    unsigned short base;

    out("\n========================================\n");
    out("  SECTION 14: I/O Indirect Write Test\n");
    out("========================================\n\n");

    /* Find I/O BAR */
    for (i = 0; i < 6; i++) {
        if (g_bars[i].is_io && g_bars[i].size > 0) {
            io_bar = i;
            break;
        }
    }
    if (io_bar < 0) {
        out("  No I/O BAR found — cannot test I/O indirect access.\n");
        return;
    }

    base = (unsigned short)(g_bars[io_bar].phys & 0xFFFF);
    out("  I/O BAR%d at port 0x%04X\n", io_bar, base);
    out("  Using MM_INDEX(port+0) / MM_DATA(port+4) for indirect access.\n\n");

    /* Read current DP_WRITE_MASK via I/O indirect */
    {
        unsigned long orig, v1, v2;

        outpd(base, R_DP_WRITE_MASK);
        orig = inpd(base + 4);
        out("  DP_WRITE_MASK (I/O read)    = 0x%08lX\n", orig);

        /* Write via I/O indirect, read back via I/O indirect */
        outpd(base, R_DP_WRITE_MASK);
        outpd(base + 4, 0xA5A5A5A5UL);
        outpd(base, R_DP_WRITE_MASK);
        v1 = inpd(base + 4);
        out("  Write 0xA5A5A5A5 via I/O, read = 0x%08lX  %s\n",
            v1, (v1 == 0xA5A5A5A5UL) ? "OK" : "MISMATCH");

        outpd(base, R_DP_WRITE_MASK);
        outpd(base + 4, 0x5A5A5A5AUL);
        outpd(base, R_DP_WRITE_MASK);
        v2 = inpd(base + 4);
        out("  Write 0x5A5A5A5A via I/O, read = 0x%08lX  %s\n",
            v2, (v2 == 0x5A5A5A5AUL) ? "OK" : "MISMATCH");

        /* Restore */
        outpd(base, R_DP_WRITE_MASK);
        outpd(base + 4, orig);

        if (v1 == 0xA5A5A5A5UL && v2 == 0x5A5A5A5AUL)
            out("  Result: I/O indirect write-back WORKS.\n");
        else
            out("  Result: DP_WRITE_MASK not readable (FIFO-submitted on R500).\n");

        /* Cross-check: write via I/O, read via MMIO BAR */
        if (g_mmio) {
            unsigned long io_val, mmio_val;
            out("\n  Cross-check: write via I/O, read via MMIO BAR:\n");

            outpd(base, R_DP_WRITE_MASK);
            outpd(base + 4, 0xDEADBEEFUL);
            mmio_val = rreg_at(g_mmio, R_DP_WRITE_MASK);
            outpd(base, R_DP_WRITE_MASK);
            io_val = inpd(base + 4);

            out("    Write 0xDEADBEEF via I/O\n");
            out("    Read via MMIO BAR = 0x%08lX  %s\n",
                mmio_val, (mmio_val == 0xDEADBEEFUL) ? "OK" : "MISMATCH");
            out("    Read via I/O      = 0x%08lX  %s\n",
                io_val, (io_val == 0xDEADBEEFUL) ? "OK" : "MISMATCH");

            if (io_val != 0xDEADBEEFUL && mmio_val != 0xDEADBEEFUL) {
                out("    >> Neither path reads back — register is FIFO-submitted\n");
                out("    >> (write-only). This is normal for R300+/R500.\n");
            } else if (io_val == 0xDEADBEEFUL && mmio_val != 0xDEADBEEFUL) {
                out("    >> I/O works but MMIO BAR doesn't — possible caching issue\n");
                out("    >> or DPMI mapping not set as uncacheable.\n");
            }

            /* Restore */
            outpd(base, R_DP_WRITE_MASK);
            outpd(base + 4, orig);
        }
    }

    /* Test DEFAULT_PITCH_OFFSET — a known readable register, via I/O */
    {
        unsigned long orig, v1;

        out("\n  DEFAULT_PITCH_OFFSET (I/O, readable register):\n");
        outpd(base, R_DEFAULT_PITCH_OFFSET);
        orig = inpd(base + 4);
        out("    Current = 0x%08lX\n", orig);

        outpd(base, R_DEFAULT_PITCH_OFFSET);
        outpd(base + 4, 0x00A50000UL);
        outpd(base, R_DEFAULT_PITCH_OFFSET);
        v1 = inpd(base + 4);
        out("    Write 0x00A50000, read = 0x%08lX  %s\n",
            v1, (v1 == 0x00A50000UL) ? "OK" : "MISMATCH");

        /* Restore */
        outpd(base, R_DEFAULT_PITCH_OFFSET);
        outpd(base + 4, orig);

        if (v1 == 0x00A50000UL)
            out("    Result: I/O indirect write-back confirmed WORKING.\n");
    }

    /* Test DST_PITCH_OFFSET via I/O */
    {
        unsigned long orig, v1;

        out("\n  DST_PITCH_OFFSET (I/O indirect):\n");
        outpd(base, R_DST_PITCH_OFFSET);
        orig = inpd(base + 4);
        out("    Current = 0x%08lX\n", orig);

        outpd(base, R_DST_PITCH_OFFSET);
        outpd(base + 4, 0x00D00000UL);
        outpd(base, R_DST_PITCH_OFFSET);
        v1 = inpd(base + 4);
        out("    Write 0x00D00000, read = 0x%08lX  %s\n",
            v1, (v1 == 0x00D00000UL) ? "OK" : "MISMATCH");

        /* Restore */
        outpd(base, R_DST_PITCH_OFFSET);
        outpd(base + 4, orig);
    }

    /* Test RBBM_SOFT_RESET (low-offset, known writable) via I/O */
    {
        unsigned long orig, v1;

        out("\n  RBBM_SOFT_RESET (low offset 0x%04X, I/O):\n", R_RBBM_SOFT_RESET);
        outpd(base, R_RBBM_SOFT_RESET);
        orig = inpd(base + 4);
        out("    Current = 0x%08lX\n", orig);

        outpd(base, R_RBBM_SOFT_RESET);
        outpd(base + 4, 0x00000001UL);
        outpd(base, R_RBBM_SOFT_RESET);
        v1 = inpd(base + 4);
        out("    Write 0x00000001, read = 0x%08lX  %s\n",
            v1, (v1 & 0x01) ? "OK (bit latched)" : "BIT NOT SET");

        /* Clear reset immediately */
        outpd(base, R_RBBM_SOFT_RESET);
        outpd(base + 4, 0);
        outpd(base, R_RBBM_SOFT_RESET);
        v1 = inpd(base + 4);
        out("    Write 0x00000000, read = 0x%08lX\n", v1);
    }
}

/* =============================================================== */
/*  SECTION 15: MM_INDEX/MM_DATA Indirect Write Test                */
/*  Tests MMIO offset 0x0000/0x0004 indirect mechanism which can    */
/*  reach all registers regardless of BAR direct-decode range.      */
/* =============================================================== */

static void test_mmindex_indirect_write(void)
{
    out("\n========================================\n");
    out("  SECTION 15: MM_INDEX/MM_DATA Write Test\n");
    out("========================================\n\n");

    if (!g_mmio) {
        out("  MMIO not available.\n");
        return;
    }

    out("  Using MMIO offsets 0x0000 (MM_INDEX) / 0x0004 (MM_DATA)\n");
    out("  to access registers indirectly through the MMIO BAR.\n\n");

    /* Test: write DP_WRITE_MASK via MM_INDEX/MM_DATA */
    {
        unsigned long orig_direct, orig_indirect, v1, v2;

        /* Read directly and indirectly for comparison */
        orig_direct = rreg_at(g_mmio, R_DP_WRITE_MASK);

        wreg_at(g_mmio, R_MM_INDEX, R_DP_WRITE_MASK);
        orig_indirect = rreg_at(g_mmio, R_MM_DATA);

        out("  DP_WRITE_MASK direct read   = 0x%08lX\n", orig_direct);
        out("  DP_WRITE_MASK indirect read = 0x%08lX  %s\n",
            orig_indirect,
            (orig_direct == orig_indirect) ? "(match)" : "(DIFFER!)");

        /* Write via indirect, read both ways */
        wreg_at(g_mmio, R_MM_INDEX, R_DP_WRITE_MASK);
        wreg_at(g_mmio, R_MM_DATA, 0xA5A5A5A5UL);

        wreg_at(g_mmio, R_MM_INDEX, R_DP_WRITE_MASK);
        v1 = rreg_at(g_mmio, R_MM_DATA);
        v2 = rreg_at(g_mmio, R_DP_WRITE_MASK);

        out("\n  Write 0xA5A5A5A5 via MM_INDEX/MM_DATA:\n");
        out("    Read back (indirect) = 0x%08lX  %s\n",
            v1, (v1 == 0xA5A5A5A5UL) ? "OK" : "MISMATCH");
        out("    Read back (direct)   = 0x%08lX  %s\n",
            v2, (v2 == 0xA5A5A5A5UL) ? "OK" : "MISMATCH");

        if (v1 == 0xA5A5A5A5UL && v2 != 0xA5A5A5A5UL)
            out("    >> Indirect works, direct doesn't — register is beyond\n"
                "    >> direct-decode range, use MM_INDEX/MM_DATA.\n");
        else if (v1 != 0xA5A5A5A5UL && v2 != 0xA5A5A5A5UL)
            out("    >> Neither path works — register may be clock-gated.\n");

        /* Restore */
        wreg_at(g_mmio, R_MM_INDEX, R_DP_WRITE_MASK);
        wreg_at(g_mmio, R_MM_DATA, orig_direct);
    }

    /* Test with DST_PITCH_OFFSET */
    {
        unsigned long v1;

        wreg_at(g_mmio, R_MM_INDEX, R_DST_PITCH_OFFSET);
        wreg_at(g_mmio, R_MM_DATA, 0x00D00000UL);
        wreg_at(g_mmio, R_MM_INDEX, R_DST_PITCH_OFFSET);
        v1 = rreg_at(g_mmio, R_MM_DATA);

        out("\n  DST_PITCH_OFFSET via MM_INDEX:\n");
        out("    Write 0x00D00000, read = 0x%08lX  %s\n",
            v1, (v1 == 0x00D00000UL) ? "OK" : "MISMATCH");

        /* Restore to 0 */
        wreg_at(g_mmio, R_MM_INDEX, R_DST_PITCH_OFFSET);
        wreg_at(g_mmio, R_MM_DATA, 0);
    }

    /* Test with low-offset register (MC_IND_INDEX) that we know works */
    {
        unsigned long orig, v1;

        orig = rreg_at(g_mmio, R_MC_IND_INDEX);
        wreg_at(g_mmio, R_MM_INDEX, R_MC_IND_INDEX);
        wreg_at(g_mmio, R_MM_DATA, 0x007F0001UL);
        wreg_at(g_mmio, R_MM_INDEX, R_MC_IND_INDEX);
        v1 = rreg_at(g_mmio, R_MM_DATA);

        out("\n  MC_IND_INDEX (low offset 0x%04X) via MM_INDEX:\n",
            R_MC_IND_INDEX);
        out("    Write 0x007F0001, read = 0x%08lX  %s\n",
            v1, (v1 == 0x007F0001UL) ? "OK" : "MISMATCH");

        /* Restore */
        wreg_at(g_mmio, R_MC_IND_INDEX, orig);
    }
}

/* =============================================================== */
/*  SECTION 16: Engine Reset + Write Test                           */
/*  Attempts a soft reset of the 2D engine, then tests if           */
/*  registers become writable.                                      */
/* =============================================================== */

static void test_reset_then_write(void)
{
    unsigned long rbbm_before, rbbm_after;
    unsigned long orig_dp_wm;
    unsigned long host_path;

    out("\n========================================\n");
    out("  SECTION 16: Engine Reset + Write Test\n");
    out("========================================\n\n");

    if (!g_mmio) {
        out("  MMIO not available.\n");
        return;
    }

    rbbm_before = rreg_at(g_mmio, R_RBBM_STATUS);
    out("  Pre-reset RBBM_STATUS  = 0x%08lX (FIFO=%lu, %s)\n",
        rbbm_before, rbbm_before & RBBM_FIFOCNT_MASK,
        (rbbm_before & RBBM_ACTIVE) ? "BUSY" : "idle");

    orig_dp_wm = rreg_at(g_mmio, R_DP_WRITE_MASK);
    out("  Pre-reset DP_WRITE_MASK = 0x%08lX\n\n", orig_dp_wm);

    /* Step 1: Soft reset CP + HI + E2 (R300+/R500 safe reset) */
    out("  Step 1: Soft reset (CP|HI|E2)...\n");
    wreg_at(g_mmio, R_RBBM_SOFT_RESET,
            SOFT_RESET_CP | SOFT_RESET_HI | SOFT_RESET_E2);
    (void)rreg_at(g_mmio, R_RBBM_SOFT_RESET);
    wreg_at(g_mmio, R_RBBM_SOFT_RESET, 0);
    (void)rreg_at(g_mmio, R_RBBM_SOFT_RESET);

    rbbm_after = rreg_at(g_mmio, R_RBBM_STATUS);
    out("    Post-reset RBBM_STATUS = 0x%08lX (FIFO=%lu)\n",
        rbbm_after, rbbm_after & RBBM_FIFOCNT_MASK);

    /* Step 2: HDP reset via HOST_PATH_CNTL toggle */
    out("  Step 2: HDP reset via HOST_PATH_CNTL...\n");
    host_path = rreg_at(g_mmio, R_HOST_PATH_CNTL);
    wreg_at(g_mmio, R_HOST_PATH_CNTL, host_path | HDP_SOFT_RESET | HDP_APER_CNTL);
    (void)rreg_at(g_mmio, R_HOST_PATH_CNTL);
    wreg_at(g_mmio, R_HOST_PATH_CNTL, host_path | HDP_APER_CNTL);
    (void)rreg_at(g_mmio, R_HOST_PATH_CNTL);
    out("    Done.\n");

    /* Step 3: Enable 2D cache autoflush */
    out("  Step 3: Enable 2D dst-cache autoflush...\n");
    wreg_at(g_mmio, R_RB3D_DSTCACHE_MODE,
            rreg_at(g_mmio, R_RB3D_DSTCACHE_MODE) | (1UL << 17));
    wreg_at(g_mmio, R_RB2D_DSTCACHE_MODE,
            rreg_at(g_mmio, R_RB2D_DSTCACHE_MODE) | (1UL << 2) | (1UL << 15));
    out("    Done.\n");

    /* Step 4: Test DP_WRITE_MASK again */
    out("\n  Step 4: Test DP_WRITE_MASK write after reset:\n");
    {
        unsigned long v1, v2;

        wreg_at(g_mmio, R_DP_WRITE_MASK, 0xA5A5A5A5UL);
        v1 = rreg_at(g_mmio, R_DP_WRITE_MASK);
        out("    Write 0xA5A5A5A5, read = 0x%08lX  %s\n",
            v1, (v1 == 0xA5A5A5A5UL) ? "OK" : "MISMATCH");

        wreg_at(g_mmio, R_DP_WRITE_MASK, 0x5A5A5A5AUL);
        v2 = rreg_at(g_mmio, R_DP_WRITE_MASK);
        out("    Write 0x5A5A5A5A, read = 0x%08lX  %s\n",
            v2, (v2 == 0x5A5A5A5AUL) ? "OK" : "MISMATCH");

        if (v1 == 0xA5A5A5A5UL && v2 == 0x5A5A5A5AUL)
            out("    Result: Engine registers WRITABLE after reset!\n");
        else if (v1 != orig_dp_wm || v2 != orig_dp_wm)
            out("    Result: Partial change — engine partially responding.\n");
        else
            out("    Result: Still not writable after reset.\n");

        /* Restore */
        wreg_at(g_mmio, R_DP_WRITE_MASK, orig_dp_wm);
    }

    /* Step 5: Test DST_PITCH_OFFSET after reset */
    out("\n  Step 5: Test DST_PITCH_OFFSET write after reset:\n");
    {
        unsigned long v1;

        wreg_at(g_mmio, R_DST_PITCH_OFFSET, 0x00D00000UL);
        v1 = rreg_at(g_mmio, R_DST_PITCH_OFFSET);
        out("    Write 0x00D00000, read = 0x%08lX  %s\n",
            v1, (v1 == 0x00D00000UL) ? "OK" : "MISMATCH");

        /* Restore */
        wreg_at(g_mmio, R_DST_PITCH_OFFSET, 0);
    }
}

/* =============================================================== */
/*  SECTION 17: Register Writability Range Test                     */
/*  Tests writes at various offsets to determine which ranges       */
/*  respond to direct MMIO writes vs which need indirect access.    */
/* =============================================================== */

static void test_write_ranges(void)
{
    typedef struct {
        unsigned long off;
        const char *name;
        unsigned long test_val;
        int restore_zero;  /* 1 = restore to 0, 0 = restore original */
        int is_fifo;       /* 1 = FIFO-submitted (write-only on R500) */
    } WRTest;

    static const WRTest tests[] = {
        { 0x0070, "MC_IND_INDEX",      0x007F0001UL, 1, 0 },
        { 0x0008, "CLOCK_CNTL_INDEX",  0x0000001FUL, 1, 0 },
        { 0x00F0, "RBBM_SOFT_RESET",   0x00000000UL, 1, 0 },
        { 0x0E44, "RBBM_CNTL",         0x0000444FUL, 0, 0 },
        { 0x0B00, "SURFACE_CNTL",      0x00000100UL, 0, 0 },
        { 0x1404, "DST_OFFSET",        0x12345678UL, 1, 0 },
        { 0x1408, "DST_PITCH",         0x00002000UL, 0, 0 },
        { 0x142C, "DST_PITCH_OFFSET",  0x00D00000UL, 1, 1 },
        { 0x146C, "DP_GUI_MASTER_CNTL",0x10000000UL, 1, 1 },
        { 0x16C0, "DP_CNTL",           0x00000003UL, 0, 0 },
        { 0x16CC, "DP_WRITE_MASK",     0xFFFFFFFFUL, 1, 1 },
        { 0x16E0, "DEFAULT_PITCH_OFS", 0x00D00000UL, 1, 0 },
        { 0x1720, "WAIT_UNTIL",        0x00000000UL, 0, 0 },
        { 0x172C, "RBBM_GUICNTL",      0x00000000UL, 0, 0 },
    };
    int i, n = sizeof(tests) / sizeof(tests[0]);

    out("\n========================================\n");
    out("  SECTION 17: Register Write Range Test\n");
    out("========================================\n\n");

    if (!g_mmio) {
        out("  MMIO not available.\n");
        return;
    }

    out("  Testing direct MMIO writes at various offset ranges.\n");
    out("  Registers below 0x4000 are 'direct-decode', above may\n");
    out("  need MM_INDEX/MM_DATA on some R300+/R500 chips.\n\n");

    out("  %-8s %-20s %-12s %-12s %s\n",
        "Offset", "Register", "Write", "ReadBack", "Status");
    out("  %-8s %-20s %-12s %-12s %s\n",
        "------", "--------", "-----", "--------", "------");

    for (i = 0; i < n; i++) {
        unsigned long orig, rb;
        const char *status;

        orig = rreg_at(g_mmio, tests[i].off);

        wreg_at(g_mmio, tests[i].off, tests[i].test_val);
        rb = rreg_at(g_mmio, tests[i].off);

        if (rb == tests[i].test_val)
            status = "OK";
        else if (tests[i].is_fifo && (rb == 0 || rb == orig))
            status = "OK (FIFO w/o)";
        else if (rb == orig)
            status = "NO CHANGE";
        else
            status = "PARTIAL";

        out("  0x%04lX  %-20s 0x%08lX   0x%08lX   %s\n",
            tests[i].off, tests[i].name, tests[i].test_val, rb, status);

        /* Restore */
        if (tests[i].restore_zero)
            wreg_at(g_mmio, tests[i].off, 0);
        else
            wreg_at(g_mmio, tests[i].off, orig);
    }

    /* Summary: count results, accounting for FIFO-submitted regs */
    {
        int ok = 0, fifo_ok = 0, fail = 0, partial = 0;
        for (i = 0; i < n; i++) {
            unsigned long rb;
            wreg_at(g_mmio, tests[i].off, tests[i].test_val);
            rb = rreg_at(g_mmio, tests[i].off);
            if (tests[i].restore_zero)
                wreg_at(g_mmio, tests[i].off, 0);

            if (rb == tests[i].test_val)
                ok++;
            else if (tests[i].is_fifo && (rb == 0))
                fifo_ok++;
            else if (rb == 0)
                fail++;
            else
                partial++;
        }
        out("\n  Summary: %d OK, %d FIFO-submitted (write-only, normal), "
            "%d partial, %d failed\n", ok, fifo_ok, partial, fail);

        if (fifo_ok > 0)
            out("\n  NOTE: FIFO-submitted registers (DST_PITCH_OFFSET,\n"
                "  DP_GUI_MASTER_CNTL, DP_WRITE_MASK) read back as 0 on\n"
                "  R300+/R500. Writes go through RBBM FIFO to the engine.\n"
                "  This is NORMAL — the 2D engine receives the commands.\n");
    }
}

/* =============================================================== */
/*  SECTION 18: Display Controller / VGA State Analysis             */
/*  Dumps AVIVO display controller state that affects 2D engine.    */
/* =============================================================== */

static void analyze_display_state(void)
{
    out("\n========================================\n");
    out("  SECTION 18: Display / VGA State\n");
    out("========================================\n\n");

    if (!g_mmio) {
        out("  MMIO not available.\n");
        return;
    }

    /* VGA render control */
    {
        unsigned long vga_rc = rreg_at(g_mmio, R_VGA_RENDER_CONTROL);
        out("  VGA_RENDER_CONTROL = 0x%08lX\n", vga_rc);
        out("    VGA rendering    : %s\n",
            (vga_rc & 0x0F) ? "ENABLED (some planes on)" : "disabled");
        out("    VGA vsync status : %s\n",
            (vga_rc & (1UL << 24)) ? "controlled" : "free-running");
    }

    /* D1VGA / D2VGA control (AVIVO) */
    {
        unsigned long d1 = rreg_at(g_mmio, R_D1VGA_CONTROL);
        unsigned long d2 = rreg_at(g_mmio, R_D2VGA_CONTROL);
        out("\n  D1VGA_CONTROL = 0x%08lX\n", d1);
        out("    D1 VGA enable    : %s\n",
            (d1 & (1UL << 0)) ? "ENABLED" : "disabled");
        out("    D1 timing select : %s\n",
            (d1 & (1UL << 8)) ? "from D1CRTC" : "VGA timing");
        out("    D1 sync polarity : %s\n",
            (d1 & (1UL << 16)) ? "from D1CRTC" : "VGA default");

        out("  D2VGA_CONTROL = 0x%08lX\n", d2);
        out("    D2 VGA enable    : %s\n",
            (d2 & (1UL << 0)) ? "ENABLED" : "disabled");

        if ((d1 & 1) || (d2 & 1))
            out("\n  NOTE: VGA mode active — 2D engine registers may be\n"
                "  accessible but rendering may conflict with VGA output.\n"
                "  RADEON.C disables VGA rendering before using 2D engine.\n");
    }

    /* AVIVO D1GRPH surface / CRTC state (for page flip diagnosis) */
    {
        unsigned long d1grph_surf = rreg_at(g_mmio, 0x6110);
        unsigned long d1grph_sec  = rreg_at(g_mmio, 0x6118);
        unsigned long d1grph_upd  = rreg_at(g_mmio, 0x6144);
        unsigned long d1grph_flip = rreg_at(g_mmio, 0x6148);
        unsigned long d1grph_pitch= rreg_at(g_mmio, 0x6120);
        unsigned long d1crtc_ctl  = rreg_at(g_mmio, 0x6080);
        unsigned long d1crtc_stat = rreg_at(g_mmio, 0x609C);
        out("\n  AVIVO D1GRPH / D1CRTC state:\n");
        out("    D1GRPH_PRIMARY_SURFACE_ADDR = 0x%08lX\n", d1grph_surf);
        out("    D1GRPH_SECONDARY_SURFACE_ADDR = 0x%08lX\n", d1grph_sec);
        out("    D1GRPH_PITCH        = 0x%08lX\n", d1grph_pitch);
        out("    D1GRPH_UPDATE       = 0x%08lX", d1grph_upd);
        if (d1grph_upd & (1UL << 16)) out(" [LOCKED]");
        if (d1grph_upd & (1UL << 2))  out(" [UPDATE_PENDING]");
        out("\n");
        out("    D1GRPH_FLIP_CONTROL = 0x%08lX\n", d1grph_flip);
        out("    D1CRTC_CONTROL      = 0x%08lX  %s\n",
            d1crtc_ctl, (d1crtc_ctl & 1) ? "(CRTC enabled)" : "(CRTC DISABLED)");
        out("    D1CRTC_STATUS       = 0x%08lX  %s\n",
            d1crtc_stat, (d1crtc_stat & 1) ? "(in VBLANK)" : "(in active)");

        /* HW flip feasibility: does D1GRPH surface match MC_FB_LOCATION? */
        {
            unsigned long mc_fb = rreg_at(g_mmio, 0x2180); /* RV515_MC_FB_LOCATION */
            unsigned long fb_base = (mc_fb & 0xFFFFUL) << 16;
            if ((d1grph_surf & 0xFFF00000UL) == (fb_base & 0xFFF00000UL))
                out("    HW page flip     : FEASIBLE (D1GRPH surf matches VRAM base)\n");
            else
                out("    HW page flip     : UNLIKELY (D1GRPH=0x%08lX vs FB=0x%08lX)\n",
                    d1grph_surf, fb_base);
        }
    }

    /* RBBM control (engine block enables) */
    {
        unsigned long rbbm_cntl = rreg_at(g_mmio, R_RBBM_CNTL);
        out("\n  RBBM_CNTL = 0x%08lX\n", rbbm_cntl);
        out("    This controls engine block scheduling and FIFO.\n");
        if (rbbm_cntl == 0)
            out("    WARNING: value is 0 — engine blocks may not be\n"
                "    properly configured.\n");
    }

    /* HDP / Surface state */
    {
        unsigned long hdp = rreg_at(g_mmio, 0x0134);  /* HDP_FB_LOCATION */
        unsigned long surf = rreg_at(g_mmio, R_SURFACE_CNTL);
        unsigned long host = rreg_at(g_mmio, R_HOST_PATH_CNTL);
        out("\n  HDP_FB_LOCATION  = 0x%08lX  (FB base = 0x%08lX)\n",
            hdp, (hdp & 0xFFFFUL) << 16);
        out("  SURFACE_CNTL     = 0x%08lX\n", surf);
        out("  HOST_PATH_CNTL   = 0x%08lX\n", host);
        out("    HDP_APER_CNTL  : %s\n",
            (host & HDP_APER_CNTL) ? "SET" : "clear");
    }

    /* 2D engine current state summary */
    {
        unsigned long dp_cntl  = rreg_at(g_mmio, R_DP_CNTL);
        unsigned long dp_dtype = rreg_at(g_mmio, R_DP_DATATYPE);
        unsigned long dp_mix   = rreg_at(g_mmio, R_DP_MIX);
        unsigned long dp_wm    = rreg_at(g_mmio, R_DP_WRITE_MASK);
        unsigned long dp_gmc   = rreg_at(g_mmio, R_DP_GUI_MASTER_CNTL);
        unsigned long dpo      = rreg_at(g_mmio, R_DST_PITCH_OFFSET);
        unsigned long def_po   = rreg_at(g_mmio, R_DEFAULT_PITCH_OFFSET);

        out("\n  2D Engine Register State:\n");
        out("    DP_CNTL            = 0x%08lX\n", dp_cntl);
        out("    DP_DATATYPE        = 0x%08lX\n", dp_dtype);
        out("    DP_MIX             = 0x%08lX\n", dp_mix);
        out("    DP_WRITE_MASK      = 0x%08lX\n", dp_wm);
        out("    DP_GUI_MASTER_CNTL = 0x%08lX\n", dp_gmc);
        out("    DST_PITCH_OFFSET   = 0x%08lX\n", dpo);
        out("    DEFAULT_PITCH_OFS  = 0x%08lX\n", def_po);
    }
}

/* =============================================================== */
/*  SECTION 19: Recommendations (updated)                           */
/* =============================================================== */

static void print_recommendations(void)
{
    unsigned long rbbm_fifo;
    int mmio_seems_correct = 0;
    int bar0_is_vram = 0;

    out("\n========================================\n");
    out("  SECTION 19: Analysis & Recommendations\n");
    out("========================================\n\n");

    if (!g_mmio) {
        out("  Could not map any MMIO BAR.\n");
        out("  Check: DPMI extender, physical address range.\n");
        return;
    }

    rbbm_fifo = rreg_at(g_mmio, R_RBBM_STATUS) & RBBM_FIFOCNT_MASK;

    if (g_mmio_bar == 0 && g_bars[0].is_pref &&
        g_bars[0].size >= 16UL*1024*1024) {
        bar0_is_vram = 1;
    }

    if (rreg_at(g_mmio, R_CONFIG_APER_0_BASE) == g_bars[0].phys &&
        g_bars[0].phys != 0 && rbbm_fifo >= 16) {
        mmio_seems_correct = 1;
    }

    if (bar0_is_vram && !mmio_seems_correct) {
        out("  ISSUE: BAR0 appears to be VRAM (large, prefetchable),\n");
        out("    but is being used as MMIO in the current RADEON.C code.\n");
        out("    On RV515 PCIe, BAR0=VRAM, BAR2=MMIO.\n\n");
        out("  FIX: Change RADEON.C to use BAR2 (PCI offset 0x18) for MMIO.\n");
    }

    if (rbbm_fifo == 0) {
        out("  ISSUE: RBBM_STATUS FIFO count = 0.\n");
        out("    A healthy idle engine should show FIFO ~64.\n");
        out("    FIFO=0 typically means we're reading VRAM, not MMIO.\n\n");
    }

    if (rbbm_fifo >= 16 && mmio_seems_correct) {
        out("  MMIO mapping appears CORRECT.\n");
        out("  RBBM_STATUS FIFO=%lu (healthy).\n\n", rbbm_fifo);
    }

    /* Check FIFO-submitted register behavior */
    {
        unsigned long dpo_orig, dpo_rb;
        dpo_orig = rreg_at(g_mmio, R_DP_WRITE_MASK);
        wreg_at(g_mmio, R_DP_WRITE_MASK, 0xFFFFFFFFUL);
        dpo_rb = rreg_at(g_mmio, R_DP_WRITE_MASK);
        wreg_at(g_mmio, R_DP_WRITE_MASK, dpo_orig);

        if (dpo_rb == 0 || dpo_rb == dpo_orig) {
            out("  FIFO-submitted registers (DST_PITCH_OFFSET,\n");
            out("  DP_GUI_MASTER_CNTL, DP_WRITE_MASK) don't support\n");
            out("  readback on this chip — this is NORMAL for R300+/R500.\n");
            out("  Writes reach the engine via RBBM FIFO; verification\n");
            out("  must use a GPU fill + VRAM readback test, not register\n");
            out("  readback.\n");
        }
    }

    out("\n  Summary of BAR usage for RADEON.C:\n");
    {
        int i;
        for (i = 0; i < 6; i++) {
            if (g_bars[i].size == 0) continue;
            if (g_bars[i].is_io)
                out("    BAR%d: I/O ports (0x%04lX, %lu bytes)\n",
                    i, g_bars[i].phys, g_bars[i].size);
            else if (!g_bars[i].is_pref && g_bars[i].size <= 256UL*1024)
                out("    BAR%d: MMIO registers (0x%08lX, %luKB) <-- use for g_mmio\n",
                    i, g_bars[i].phys, g_bars[i].size / 1024);
            else
                out("    BAR%d: VRAM aperture (0x%08lX, %luMB) <-- use for g_lfb\n",
                    i, g_bars[i].phys,
                    g_bars[i].size / (1024UL*1024));
        }
    }
}

/* =============================================================== */
/*  main                                                            */
/* =============================================================== */

int main(void)
{
    printf("RDIAG.EXE - ATI Radeon X1300 Pro Hardware Diagnostic\n");
    printf("====================================================\n\n");

    /* Open log file */
    g_log = fopen("TESTLOG.TXT", "w");
    if (!g_log) {
        printf("WARNING: Cannot create TESTLOG.TXT — console output only.\n");
    } else {
        printf("Logging to TESTLOG.TXT\n\n");
    }

    out("RDIAG - ATI Radeon X1300 Pro Hardware Diagnostic\n");
    out("================================================\n");

    /* Allocate DOS transfer buffer */
    if (!dpmi_alloc(64)) {
        out("ERROR: Cannot allocate DOS memory.\n");
        if (g_log) fclose(g_log);
        return 1;
    }

    /* Find the card */
    out("\nScanning PCI bus for ATI RV515/RV516...\n");
    if (!pci_find_radeon()) {
        out("ERROR: No Radeon X1300 (RV515/RV516) found.\n");
        dpmi_free();
        if (g_log) fclose(g_log);
        return 1;
    }
    out("Found: %s at PCI %02X:%02X.%X (VID:DID = %04X:%04X)\n",
        g_card_name, g_pci_bus, g_pci_dev, g_pci_func,
        ATI_VID, g_pci_did);

    /* Run all diagnostic sections */
    dump_pci_config();
    analyze_pci_command();
    dump_bars();
    probe_bars_for_mmio();
    cross_bar_compare();
    dump_gpu_registers();
    test_mmio_readback();
    analyze_fb_location();
    test_2d_engine();
    test_vram_access();
    dump_vbe_info();
    test_io_ports();
    dump_environment();
    test_io_indirect_write();
    test_mmindex_indirect_write();
    test_reset_then_write();
    test_write_ranges();
    analyze_display_state();
    print_recommendations();

    /* Cleanup all mapped BARs */
    {
        int i;
        for (i = 0; i < 6; i++) {
            if (g_probes[i].base)
                dpmi_unmap((void *)g_probes[i].base);
        }
    }
    dpmi_free();

    out("\n================================================\n");
    out("  Diagnostic complete.\n");
    if (g_log) {
        out("  Results saved to TESTLOG.TXT\n");
        fclose(g_log);
        g_log = NULL;
    }
    out("================================================\n");

    printf("\nPress any key to exit...\n");
    getch();

    return 0;
}
