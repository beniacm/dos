/*
 * RADEONHW.H  -  Shared hardware-layer declarations for ATI Radeon RV515/RV516
 *
 * Consolidates register definitions, packed structs, device table, shared
 * global externs and function prototypes used by RADEON.C, RBLIT.C and
 * RDIAG.C.  Mirrors the VGA.H pattern used in the PLASMA project.
 *
 * Include guard prevents multiple-inclusion problems when transitional
 * unit tests or future files include this header more than once.
 */

#ifndef RADEONHW_H
#define RADEONHW_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------
 * Compiler-specific includes and port I/O compatibility
 * --------------------------------------------------------------- */

#ifdef __DJGPP__
#  include <dpmi.h>
#  include <pc.h>
#  include <go32.h>
#  include <sys/nearptr.h>
   /* Map OpenWatcom port-I/O names to DJGPP equivalents */
#  define inp(p)      inportb(p)
#  define outp(p,v)   outportb((p),(v))
#  define inpd(p)     inportl(p)
#  define outpd(p,v)  outportl((p),(v))
#else
#  include <i86.h>
#endif

/* =============================================================== */
/*  Packed structures (shared by all three programs)               */
/* =============================================================== */

#pragma pack(1)

/* RDIAG's more complete VBEInfo includes OEM string data fields */
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

/* Real-Mode Interrupt call block (DPMI function 0x0300) */
typedef struct {
    unsigned long  edi, esi, ebp, _rz;
    unsigned long  ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs;
    unsigned short ip, cs, sp, ss;
} RMI;

#pragma pack()

/* Device-ID table entry */
typedef struct { unsigned short did; const char *name; } DevEntry;

/* =============================================================== */
/*  ATI Radeon register offsets (superset: R300-R500 + RDIAG extras) */
/*  Reference: Linux kernel radeon_reg.h, rv515d.h, xf86-video-ati */
/* =============================================================== */

/* Indirect MMIO access (MM_INDEX/MM_DATA — not normally needed) */
#define R_MM_INDEX                 0x0000
#define R_MM_DATA                  0x0004

/* PLL register access (via CLOCK_CNTL_INDEX/DATA) */
#define R_CLOCK_CNTL_INDEX         0x0008
#define R_CLOCK_CNTL_DATA          0x000C
#define   PLL_WR_EN                (1UL << 7)

/* MC indirect access (via MC_IND_INDEX/DATA) */
#define R_MC_IND_INDEX             0x0070
#define R_MC_IND_DATA              0x0074

/* Soft reset for the 2D/3D sub-engines */
#define R_RBBM_SOFT_RESET          0x00F0
#define   SOFT_RESET_CP            (1UL << 0)
#define   SOFT_RESET_HI            (1UL << 1)
#define   SOFT_RESET_SE            (1UL << 2)
#define   SOFT_RESET_RE            (1UL << 3)
#define   SOFT_RESET_PP            (1UL << 4)
#define   SOFT_RESET_E2            (1UL << 5)
#define   SOFT_RESET_RB            (1UL << 6)
#define   SOFT_RESET_HDP           (1UL << 7)

/* Frame-buffer / aperture config registers (for MMIO identification) */
#define R_CONFIG_MEMSIZE           0x00F8
#define R_CONFIG_APER_0_BASE       0x0100  /* should match BAR0 phys addr */
#define R_CONFIG_APER_SIZE         0x0108  /* VRAM aperture size */
#define R_CONFIG_REG_1_BASE        0x010C  /* should match MMIO BAR phys */
#define R_CONFIG_REG_APER_SIZE     0x0110  /* MMIO aperture size */

/* Host data-path control (used for HDP reset) */
#define R_HOST_PATH_CNTL           0x0130
#define R_HDP_FB_LOCATION          0x0134
#define   HDP_SOFT_RESET           (1UL << 26)
#define   HDP_APER_CNTL            (1UL << 23)

/* VGA render control — disable VGA to avoid 2D engine conflicts */
#define R_VGA_RENDER_CONTROL       0x0300
#define   VGA_VSTATUS_CNTL_MASK    0x00070000UL

/* D1/D2 VGA control (AVIVO display block) */
#define R_D1VGA_CONTROL            0x0330
#define   D1VGA_MODE_ENABLE        (1UL << 0)
#define R_D2VGA_CONTROL            0x0338

/* Surface byte-swap control */
#define R_SURFACE_CNTL             0x0B00

/* RBBM engine status / control */
#define R_RBBM_STATUS              0x0E40
#define   RBBM_FIFOCNT_MASK        0x007F
#define   RBBM_ACTIVE              (1UL << 31)
#define R_RBBM_CNTL                0x0E44

/* 2D engine: source pipe config */
#define R_DST_PIPE_CONFIG          0x170C
#define   PIPE_AUTO_CONFIG         (1UL << 31)

/* 2D engine: source/destination offsets and pitches (separate registers) */
#define R_DST_OFFSET               0x1404
#define R_DST_PITCH                0x1408
#define R_DST_WIDTH                0x140C
#define R_DST_HEIGHT               0x1410
#define R_SRC_PITCH_OFFSET         0x1428
#define R_DST_PITCH_OFFSET         0x142C
#define R_SRC_Y_X                  0x1434
#define R_DST_Y_X                  0x1438
#define R_DST_HEIGHT_WIDTH         0x143C

/* 2D engine: GUI master control and ROP select */
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

/* 2D engine: brush and source foreground/background colours */
#define R_DP_BRUSH_BKGD_CLR        0x1478
#define R_DP_BRUSH_FRGD_CLR        0x147C

#define R_SRC_OFFSET               0x15AC
#define R_SRC_PITCH                0x15B0

/* Color-compare registers (source color-key transparency).
   FCN_NE=4 draws pixels where src != key (skip-if-equal).
   FCN_EQ=5 draws pixels where src == key (skip-if-neq).
   Hardware-confirmed on RV515 X1300 — xf86 names are confusing. */
#define R_CLR_CMP_CNTL             0x15C0
#define R_CLR_CMP_CLR_SRC          0x15C4
#define R_CLR_CMP_MASK             0x15CC
#define   CLR_CMP_FCN_NE           4UL          /* skip-if-equal  → draw when src != key */
#define   CLR_CMP_FCN_EQ           5UL          /* skip-if-neq    → draw when src == key */
#define   CLR_CMP_SRC_SOURCE       (1UL << 24)  /* compare source pixels */

/* 2D engine: source foreground/background colours for host data */
#define R_DP_SRC_FRGD_CLR          0x15D8
#define R_DP_SRC_BKGD_CLR          0x15DC

/* 2D engine: direction, data type, mix, write mask, pitch/offset */
#define R_DP_CNTL                  0x16C0
#define   DST_X_LEFT_TO_RIGHT      1UL
#define   DST_Y_TOP_TO_BOTTOM      2UL
#define R_DP_DATATYPE              0x16C4
#define R_DP_MIX                   0x16C8
#define R_DP_WRITE_MASK            0x16CC
#define R_DEFAULT_PITCH_OFFSET     0x16E0

/* Scissor / clip rectangles */
#define R_DEFAULT_SC_BOTTOM_RIGHT  0x16E8
#define R_SC_TOP_LEFT              0x16EC
#define R_SC_BOTTOM_RIGHT          0x16F0

/* 2D line drawing registers */
#define R_DST_LINE_START           0x1600
#define R_DST_LINE_END             0x1604
#define R_DST_LINE_PATCOUNT        0x1608

/* NOTE: R_DSTCACHE_CTLSTAT (0x1714) is the R100/R200 cache flush register.
   For R300+/R500, use R_RB2D_DSTCACHE_CTLSTAT (0x342C) instead. */
#define R_DSTCACHE_CTLSTAT         0x1714
#define   R300_DC_FLUSH_2D         (1UL << 0)
#define   R300_DC_FREE_2D          (1UL << 2)
#define   R300_RB2D_DC_FLUSH_ALL   0x05UL

/* WAIT_UNTIL — synchronize CPU with GPU operations */
#define R_WAIT_UNTIL               0x1720
#define   WAIT_2D_IDLE             (1UL << 14)
#define   WAIT_3D_IDLE             (1UL << 15)
#define   WAIT_2D_IDLECLEAN        (1UL << 16)
#define   WAIT_3D_IDLECLEAN        (1UL << 17)
#define   WAIT_DMA_GUI_IDLE        (1UL << 9)

/* 2D/3D synchronization control (rv515d.h) */
#define R_ISYNC_CNTL               0x1724
#define   ISYNC_ANY2D_IDLE3D       (1UL << 0)
#define   ISYNC_ANY3D_IDLE2D       (1UL << 1)
#define   ISYNC_WAIT_IDLEGUI       (1UL << 4)
#define   ISYNC_CPSCRATCH_IDLEGUI  (1UL << 5)

/* RBBM GUI endian control */
#define R_RBBM_GUICNTL             0x172C

/* R300+ 3D destination cache mode/status */
#define R_RB3D_DSTCACHE_MODE       0x3258

/* R300+/R500 2D destination cache mode/status */
#define R_RB2D_DSTCACHE_MODE       0x3428
#define R_RB2D_DSTCACHE_CTLSTAT    0x342C
#define   RB2D_DC_FLUSH            (3UL << 0)
#define   RB2D_DC_FREE             (3UL << 2)
#define   RB2D_DC_FLUSH_ALL        0x0FUL
#define   RB2D_DC_BUSY             (1UL << 31)

/* Graphics block configuration */
#define R_GB_ENABLE                0x4008
#define R_GB_MSPOS0                0x4010
#define R_GB_MSPOS1                0x4014
#define R_GB_TILE_CONFIG           0x4018
#define   GB_TILE_ENABLE           (1UL << 0)
#define   GB_TILE_SIZE_16          (1UL << 4)
#define   GB_PIPE_COUNT_RV350      (0UL << 1)
#define R_GB_SELECT                0x401C
#define R_GB_AA_CONFIG             0x4020
#define R_GB_PIPE_SELECT           0x402C

/* Geometry arbiter — deadlock/fastsync prevention */
#define R_GA_ENHANCE               0x4274
#define   GA_DEADLOCK_CNTL         (1UL << 0)
#define   GA_FASTSYNC_CNTL         (1UL << 1)

/* R500 shader-unit register destination (pipe mask) */
#define R_SU_REG_DEST              0x42C8

/* Vertex array processing */
#define R_VAP_INDEX_OFFSET         0x208C

/* RB3D dst cache (ring-start variant used during initialization) */
#define R_RB3D_DSTCACHE_CTLSTAT_RS 0x4E4C
#define   RB3D_DC_FLUSH_RS         (2UL << 0)
#define   RB3D_DC_FREE_RS          (2UL << 2)

/* ZB z-cache control/status */
#define R_ZB_ZCACHE_CTLSTAT        0x4F18
#define   ZC_FLUSH                 (1UL << 0)
#define   ZC_FREE                  (1UL << 1)

/* RV515 MC indirect register numbers (via MC_IND_INDEX/DATA) */
#define RV515_MC_STATUS            0x0000
#define   MC_STATUS_IDLE           (1UL << 4)
#define RV515_MC_FB_LOCATION       0x0001
#define RV515_MC_CNTL              0x0005
#define RV515_MC_MISC_LAT_TIMER    0x0009
#define RV515_MC_MISC_UMA_CNTL     0x000C

/* R500 PLL register for pipe memory power (indirect PLL register) */
#define R500_DYN_SCLK_PWMEM_PIPE  0x000D

/* AVIVO display controller registers (R500/RV515).
   Used for tear-free hardware page flipping via surface update lock.
   Reference: Linux kernel rs600.c rs600_page_flip(). */
#define R_D1GRPH_PRIMARY_SURFACE_ADDRESS   0x6110
#define R_D1GRPH_SECONDARY_SURFACE_ADDRESS 0x6118
#define R_D1GRPH_PITCH                     0x6120
#define R_D1GRPH_X_START                   0x612C  /* left edge of display window */
#define R_D1GRPH_Y_START                   0x6130  /* top edge */
#define R_D1GRPH_X_END                     0x6134  /* right edge (exclusive) = xres */
#define R_D1GRPH_Y_END                     0x6138  /* bottom edge (exclusive) = yres */
#define R_D1GRPH_UPDATE                    0x6144
#define   D1GRPH_SURFACE_UPDATE_PENDING    (1UL << 2)
#define   D1GRPH_SURFACE_UPDATE_LOCK       (1UL << 16)
#define R_D1GRPH_FLIP_CONTROL              0x6148
#define   D1GRPH_SURFACE_UPDATE_H_RETRACE_EN (1UL << 0)

/* AVIVO CRTC control/status — vblank detection */
#define R_D1CRTC_CONTROL           0x6080
#define   AVIVO_CRTC_EN            (1UL << 0)
#define R_D1CRTC_STATUS            0x609C
#define   D1CRTC_V_BLANK           (1UL << 0)

/* =============================================================== */
/*  ATI vendor ID and device-ID table                               */
/* =============================================================== */

#define ATI_VID  0x1002

/* g_devtab[] is defined in RADEONHW.C */
extern const DevEntry g_devtab[];

/* =============================================================== */
/*  Shared global state (defined in RADEONHW.C)                    */
/* =============================================================== */

extern unsigned short g_dseg;             /* DOS real-mode segment for transfer buffer */
extern unsigned short g_dsel;             /* DPMI selector for the same buffer         */
extern volatile unsigned long *g_mmio;    /* mapped MMIO register window               */
extern unsigned long  g_mmio_phys;        /* physical address of MMIO BAR              */
extern unsigned long  g_vram_mb;          /* GPU VRAM size in MB                       */
extern unsigned char *g_lfb;             /* linear framebuffer pointer                 */
extern unsigned long  g_lfb_phys;        /* physical address of LFB                   */
extern unsigned long  g_fb_location;     /* GPU internal FB base (from HDP_FB_LOCATION)*/
extern int g_xres, g_yres, g_pitch;      /* display geometry                          */
extern int g_page_stride;                /* rows per page (4KB-aligned stride)        */
extern unsigned long g_default_po;       /* cached default PITCH_OFFSET (gpu_init_2d) */
extern unsigned short g_vmode;           /* active VESA mode number                   */
extern unsigned char *g_font;            /* pointer to BIOS 8x8 font data             */
extern int g_fifo_free;                  /* tracked free FIFO entries                 */
extern int g_avivo_flip;                 /* 1=AVIVO D1GRPH flip, 0=VBE+port 0x3DA    */
extern double g_rdtsc_mhz;              /* calibrated CPU MHz (RDTSC)                */
extern int g_num_gb_pipes;              /* number of GPU geometry back-end pipes      */
extern int g_dac_bits;                  /* DAC bits (6 or 8), set by init_dac()       */
extern unsigned long g_vesa_mem_kb;     /* VESA total video memory in KB               */

extern int  g_pci_bus, g_pci_dev, g_pci_func;
extern unsigned short g_pci_did;
extern const char    *g_card_name;

/* =============================================================== */
/*  Function prototypes                                             */
/* =============================================================== */

/* ---- DPMI helpers ---- */
int   dpmi_alloc(unsigned short para);
void  dpmi_free(void);
int   dpmi_rmint(unsigned char n, RMI *p);
void *dpmi_map(unsigned long phys, unsigned long sz);
void  dpmi_unmap(void *p);

/* ---- PCI configuration (Mechanism 1) ---- */
unsigned long  pci_rd32(int b, int d, int f, int reg);
void           pci_wr32(int b, int d, int f, int reg, unsigned long v);
unsigned short pci_rd16(int b, int d, int f, int reg);
int            pci_find_radeon(void);

/* ---- VESA mode enumeration ---- */
int find_mode(void);

/* ---- VBE helpers ---- */
unsigned char *dosbuf(void);
int   vbe_get_info(VBEInfo *out);
int   vbe_get_mode(unsigned short m, VBEMode *out);
int   vbe_set(unsigned short m);
void  vbe_text(void);

/* ---- BIOS 8x8 font ---- */
unsigned char *get_font(void);

/* ---- CPU drawing primitives (writes directly to g_lfb) ---- */
void cpu_char(int x, int y, char ch, unsigned char c, int sc);
void cpu_str(int x, int y, const char *s, unsigned char c, int sc);
void cpu_str_c(int y, const char *s, unsigned char c, int sc);

/* ---- VGA palette via VBE 4F08h/4F09h ---- */
void init_dac(void);
void vbe_set_palette(int start, int count, const unsigned char *p);
void pal_set(unsigned char *p, int idx, int r, int g, int b);
void pal_lerp(unsigned char *p, int i0, int i1,
              int r0, int g0, int b0, int r1, int g1, int b1);
void setup_palette(void);
void setup_dune_palette(void);

/* ---- Radeon MMIO register access ---- */
unsigned long rreg(unsigned long off);
void          wreg(unsigned long off, unsigned long val);
unsigned long mc_rreg(unsigned long reg);
unsigned long pll_rreg(unsigned long reg);
void          pll_wreg(unsigned long reg, unsigned long val);

/* ---- GPU engine control ---- */
void gpu_vga_render_disable(void);
int  gpu_mc_wait_idle(void);
void gpu_wait_fifo(int n);
void gpu_engine_flush(void);
void gpu_wait_idle(void);
void gpu_engine_reset(void);
void gpu_init_2d(void);

/* ---- RDTSC high-resolution timer ---- */
void   rdtsc_read(unsigned long *lo, unsigned long *hi);
double tsc_to_ms(unsigned long lo1, unsigned long hi1,
                 unsigned long lo0, unsigned long hi0);
void   calibrate_rdtsc(void);

/* ---- Page-flip helpers ---- */
void detect_flip_mode(void);
void avivo_wait_vblank(void);
void hw_page_flip(unsigned long surface_addr);
void wait_vsync(void);
void flip_page(int back_page);
void flip_restore_page0(void);

#endif /* RADEONHW_H */
