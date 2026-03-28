/*
 * DOSLIB.H — Shared DOS protected-mode demo library
 *
 * Provides DPMI memory management, VBE mode setting/palette/page-flip,
 * PCI configuration, BIOS font access, text rendering, RDTSC timing,
 * and VGA vsync for 32-bit DOS protected-mode programs.
 *
 * Hides all OpenWatcom / DJGPP differences behind a clean C API so
 * that demo programs need zero #ifdef blocks.
 *
 * Compiles under DJGPP/GCC 12+ and OpenWatcom 1.9+ (C89).
 */

#ifndef DOSLIB_H
#define DOSLIB_H

/* ----------------------------------------------------------------
 * Platform includes — Watcom vs DJGPP abstraction
 * ---------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __DJGPP__
#  include <dpmi.h>
#  include <pc.h>
#  include <go32.h>
#  include <sys/nearptr.h>
#  define inp(p)      inportb(p)
#  define outp(p,v)   outportb((p),(v))
#  define inpd(p)     inportl(p)
#  define outpd(p,v)  outportl((p),(v))
#else
#  include <i86.h>
#  include <conio.h>
#endif

/* ----------------------------------------------------------------
 * Packed VBE / DPMI structures
 * ---------------------------------------------------------------- */

#pragma pack(1)

typedef struct {
    char           sig[4];              /* "VESA" */
    unsigned short ver;                 /* BCD version (e.g. 0x0300) */
    unsigned long  oem_str;             /* far ptr to OEM string */
    unsigned long  caps;
    unsigned long  mode_ptr;            /* far ptr to mode list */
    unsigned short tot_mem;             /* 64 KB units */
    unsigned short oem_soft_rev;
    unsigned long  oem_vendor;
    unsigned long  oem_product;
    unsigned long  oem_rev;
    unsigned char  _pad[222];
    unsigned char  oem_data[256];
} VBEInfo;                              /* 512 bytes */

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
} VBEModeInfo;                          /* 256 bytes */

/* Backward-compatibility alias used by RADEON demos */
typedef VBEModeInfo VBEMode;

/* Real-Mode Interrupt call block (DPMI INT 31h AX=0300h) */
typedef struct {
    unsigned long  edi, esi, ebp, _rz;
    unsigned long  ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs;
    unsigned short ip, cs, sp, ss;
} RMI;

#pragma pack()

/* ----------------------------------------------------------------
 * Global state (defined in DOSLIB.C)
 * ---------------------------------------------------------------- */

/* DPMI transfer buffer */
extern unsigned short g_dseg;
extern unsigned short g_dsel;

/* Active video mode geometry */
extern int            g_xres, g_yres, g_pitch;
extern unsigned short g_vmode;
extern unsigned long  g_lfb_phys;
extern unsigned char *g_lfb;
extern unsigned long  g_vesa_mem_kb;

/* BIOS font + DAC */
extern unsigned char *g_font;
extern int            g_dac_bits;

/* RDTSC calibration */
extern double         g_rdtsc_mhz;

/* ----------------------------------------------------------------
 * DPMI helpers
 * ---------------------------------------------------------------- */

unsigned char *dosbuf(void);
int   dpmi_alloc(unsigned short paragraphs);
void  dpmi_free(void);
int   dpmi_rmint(unsigned char intno, RMI *rmi);
void *dpmi_map(unsigned long phys, unsigned long size);
void  dpmi_unmap(void *ptr);

/* ----------------------------------------------------------------
 * PCI configuration (Mechanism 1, port I/O)
 * ---------------------------------------------------------------- */

unsigned long  pci_rd32(int bus, int dev, int func, int reg);
void           pci_wr32(int bus, int dev, int func, int reg, unsigned long val);
unsigned short pci_rd16(int bus, int dev, int func, int reg);

/* ----------------------------------------------------------------
 * VBE mode enumeration and control
 * ---------------------------------------------------------------- */

int  vbe_get_info(VBEInfo *out);
int  vbe_get_mode_info(unsigned short mode, VBEModeInfo *out);
int  vbe_set_mode(unsigned short mode);
void vbe_text_mode(void);

/* Parameterised mode search.
   Prefers exact (want_xres × want_yres), falls back to largest
   mode that fits within (max_xres × max_yres) at the given bpp.
   On success fills g_vmode, g_xres, g_yres, g_pitch, g_lfb_phys. */
int  find_vbe_mode(int want_xres, int want_yres, int want_bpp,
                   int max_xres, int max_yres);

/* Backward-compatibility wrappers used by RADEON demos */
#define vbe_get_mode  vbe_get_mode_info
#define vbe_set       vbe_set_mode
#define vbe_text      vbe_text_mode

/* ----------------------------------------------------------------
 * VGA palette via VBE 4F08h / 4F09h
 * ---------------------------------------------------------------- */

void init_dac(void);
void vbe_set_palette(int start, int count, const unsigned char *pal);
void pal_set(unsigned char *pal, int idx, int r, int g, int b);
void pal_lerp(unsigned char *pal, int i0, int i1,
              int r0, int g0, int b0, int r1, int g1, int b1);

/* ----------------------------------------------------------------
 * BIOS 8×8 font
 * ---------------------------------------------------------------- */

unsigned char *get_font(void);

/* ----------------------------------------------------------------
 * Text rendering (uses g_lfb, g_pitch, g_xres globals)
 * ---------------------------------------------------------------- */

void cpu_char(int x, int y, char ch, unsigned char color, int scale);
void cpu_str(int x, int y, const char *s, unsigned char color, int scale);
void cpu_str_c(int y, const char *s, unsigned char color, int scale);

/* ----------------------------------------------------------------
 * RDTSC high-resolution timer
 * ---------------------------------------------------------------- */

void   rdtsc_read(unsigned long *lo, unsigned long *hi);
double tsc_to_ms(unsigned long lo1, unsigned long hi1,
                 unsigned long lo0, unsigned long hi0);
void   calibrate_rdtsc(void);

/* ----------------------------------------------------------------
 * VGA vsync (port 0x3DA)
 * ---------------------------------------------------------------- */

void wait_vsync(void);

/* ----------------------------------------------------------------
 * Write-Combine optimal memory copy (REP MOVSD)
 *
 * Uses hardware fast-string path for optimal WC writes (1000+ MB/s).
 * count must be a multiple of 4.
 * ---------------------------------------------------------------- */

void wc_memcpy(void *dst, const void *src, unsigned long count);

#endif /* DOSLIB_H */
