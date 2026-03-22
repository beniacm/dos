/*
 * VGA.H — DOS VBE/VESA graphics library interface
 *
 * Provides DPMI memory management, VBE mode setting, PMI page flip,
 * MTRR/PAT write-combining, GPU register access, text rendering,
 * WC-optimal blitting, and RDTSC timing for 32-bit DOS protected mode.
 *
 * Compiles under DJGPP/GCC and OpenWatcom (C89).
 */

#ifndef VGA_H
#define VGA_H

/* --------------------------------------------------------------------------
 * Platform includes
 * -------------------------------------------------------------------------- */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __DJGPP__
#include <dos.h>
#include <dpmi.h>
#include <go32.h>
#include <sys/nearptr.h>
#include <sys/movedata.h>
#include <sys/segments.h>
#include <pc.h>
#include <conio.h>
#define far
#define __far
#define __interrupt
#define inp(p)      inportb(p)
#define outp(p,v)   outportb(p,v)
#define stricmp     strcasecmp
#else
#include <conio.h>
#include <dos.h>
#include <i86.h>
#endif

/* --------------------------------------------------------------------------
 * Default video mode dimensions
 * -------------------------------------------------------------------------- */

#define VGA_WIDTH   1024
#define VGA_HEIGHT  768
#define VGA_PIXELS  ((unsigned long)VGA_WIDTH * VGA_HEIGHT)

/* --------------------------------------------------------------------------
 * Packed VBE / DPMI structures
 * -------------------------------------------------------------------------- */

#pragma pack(1)

typedef struct {
    char           vbe_signature[4];
    unsigned short vbe_version;
    unsigned long  oem_string_ptr;
    unsigned long  capabilities;
    unsigned long  video_mode_ptr;
    unsigned short total_memory;
    unsigned short oem_software_rev;
    unsigned long  oem_vendor_name_ptr;
    unsigned long  oem_product_name_ptr;
    unsigned long  oem_product_rev_ptr;
    unsigned char  reserved[222];
    unsigned char  oem_data[256];
} VBEInfo;                              /* 512 bytes */

typedef struct {
    unsigned short mode_attributes;
    unsigned char  win_a_attributes;
    unsigned char  win_b_attributes;
    unsigned short win_granularity;
    unsigned short win_size;
    unsigned short win_a_segment;
    unsigned short win_b_segment;
    unsigned long  win_func_ptr;
    unsigned short bytes_per_scan_line;
    unsigned short x_resolution;
    unsigned short y_resolution;
    unsigned char  x_char_size;
    unsigned char  y_char_size;
    unsigned char  number_of_planes;
    unsigned char  bits_per_pixel;
    unsigned char  number_of_banks;
    unsigned char  memory_model;
    unsigned char  bank_size;
    unsigned char  number_of_image_pages;
    unsigned char  reserved1;
    unsigned char  red_mask_size,   red_field_position;
    unsigned char  green_mask_size, green_field_position;
    unsigned char  blue_mask_size,  blue_field_position;
    unsigned char  rsvd_mask_size,  rsvd_field_position;
    unsigned char  direct_color_info;
    unsigned long  phys_base_ptr;
    unsigned long  reserved2;
    unsigned short reserved3;
    unsigned char  padding[206];
} VBEModeInfo;                          /* 256 bytes */

/* DPMI INT 31h AX=0300h register structure */
typedef struct {
    unsigned long  edi, esi, ebp, reserved_zero;
    unsigned long  ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs;
    unsigned short ip, cs, sp, ss;
} RMI;

#pragma pack()

/* --------------------------------------------------------------------------
 * Extern globals
 * -------------------------------------------------------------------------- */

/* DPMI state */
extern unsigned short g_dos_seg;
extern unsigned short g_dos_sel;

/* VBE 3.0 / PMI state */
extern int           g_vbe3;
extern int           g_force_vbe2;
extern int           g_no_pmi;
extern int           g_no_mtrr;
extern int           g_mtrr_info;
extern int           g_direct_vram;
extern unsigned short g_vbe_version;
extern int           g_pmi_ok;
extern int           g_sched_flip;
extern int           g_sched_req;
extern unsigned short g_pmi_rm_seg;
extern unsigned short g_pmi_rm_off;
extern unsigned short g_pmi_size;
extern unsigned short g_pmi_setw_off;
extern unsigned short g_pmi_setds_off;
extern unsigned short g_pmi_setpal_off;

/* Direct GPU register page flip */
extern int            g_hw_flip;
extern unsigned short g_gpu_iobase;
extern unsigned long  g_fb_phys;

/* MTRR state */
extern int           g_mtrr_slot;
extern unsigned long g_mtrr_save_base_lo, g_mtrr_save_base_hi;
extern unsigned long g_mtrr_save_mask_lo, g_mtrr_save_mask_hi;
extern int           g_mtrr_wc;
extern int           g_mtrr_replaced_uc;

/* PAT state */
extern unsigned long g_pat_save_lo, g_pat_save_hi;
extern int           g_pat_modified;

/* DAC */
extern int g_dac_bits;

/* HUD colour indices */
extern unsigned char g_hud_dark;
extern unsigned char g_hud_bright;

/* Benchmark / timing */
extern double g_bench_render_ms;
extern double g_bench_blit_ms;
extern double g_bench_combined_ms;
extern double g_rdtsc_mhz;

/* --------------------------------------------------------------------------
 * Function prototypes
 * -------------------------------------------------------------------------- */

/* DPMI functions */
unsigned char *dos_buf(void);
int  dpmi_alloc_dos(unsigned short paragraphs);
void dpmi_free_dos(void);
int  dpmi_real_int(unsigned char intno, RMI *rmi);
void *dpmi_map_physical(unsigned long phys, unsigned long size);
void dpmi_unmap_physical(void *ptr);

/* VBE functions */
int  vbe_get_info(VBEInfo *out);
int  vbe_get_mode_info(unsigned short mode, VBEModeInfo *out);
int  vbe_set_mode(unsigned short mode);
void vbe_set_text_mode(void);
int  vbe_set_display_start(unsigned short cx, unsigned short dy, int wait);
int  vbe_schedule_display_start(unsigned short cx, unsigned short dy);
int  vbe_is_flip_complete(void);

/* PMI functions */
int  query_pmi(void);
unsigned long _pmi_call4(unsigned long entry, unsigned long _eax,
                         unsigned long _ebx, unsigned long _ecx,
                         unsigned long _edx);
unsigned long _pmi_call4_ebx(unsigned long entry, unsigned long _eax,
                              unsigned long _ebx, unsigned long _ecx,
                              unsigned long _edx);
int  pmi_set_display_start(unsigned short cx, unsigned short dy, int wait);
int  pmi_schedule_display_start(unsigned short cx, unsigned short dy);
int  pmi_is_flip_complete(void);

/* R500 (RV515/X1300) register definitions */
#define R5_D1GRPH_PRIMARY_SURFACE_ADDRESS   0x6110
#define R5_D1GRPH_SECONDARY_SURFACE_ADDRESS 0x6118
#define R5_D1GRPH_UPDATE                    0x6144
#define R5_D1GRPH_SURFACE_UPDATE_PENDING    (1UL << 2)
#define R5_D1GRPH_SURFACE_UPDATE_LOCK       (1UL << 16)

/* GPU functions */
void _gpu_outpd(unsigned short port, unsigned long val);
unsigned long _gpu_inpd(unsigned short port);
unsigned long gpu_reg_read(unsigned long reg);
void gpu_reg_write(unsigned long reg, unsigned long val);
int  detect_gpu_iobase(unsigned long lfb_phys);
void hw_page_flip(unsigned long page_phys_addr);
int  hw_is_flip_done(void);

/* CPU ring-0 helpers */
unsigned short _get_cs(void);
#ifndef __DJGPP__
#pragma aux _get_cs = "mov ax, cs" value [ax]
#endif
int  _has_cpuid(void);
unsigned long _cpuid1_edx(void);
unsigned long _rdmsr_lo(unsigned long msr);
unsigned long _rdmsr_hi(unsigned long msr);
void _wrmsr_3(unsigned long msr, unsigned long lo, unsigned long hi);
void _wbinvd(void);
unsigned long _read_cr3(void);
void _flush_tlb(void);
unsigned long _read_cr0(void);
#ifdef __DJGPP__
void djgpp_enable_sse(void);
unsigned long get_cs_limit(void);
unsigned long desc_base(const unsigned char *d);
int  expand_cs_to_4gb(void);
#endif

/* MTRR / PAT */
unsigned long next_pow2(unsigned long v);
int  setup_mtrr_wc(unsigned long phys_addr, unsigned long vram_bytes);
void restore_mtrr(void);
int  setup_pat_uc_minus(void);
void restore_pat(void);

/* MTRR diagnostics */
const char *mtrr_type_name(unsigned long type);
void dump_mtrr_info(unsigned long lfb_va, unsigned long map_size);

/* DAC / Palette */
void init_dac(void);
void vbe_set_palette(int start, int count, const unsigned char *pal);

/* Font */
unsigned char *get_bios_font_8x8(void);

/* Text rendering */
void draw_char_bg(unsigned char *fb, int pitch, int x, int y, char ch,
                  unsigned char fg, unsigned char bg,
                  const unsigned char *font);
void draw_str_bg(unsigned char *fb, int pitch, int x, int y, const char *s,
                 unsigned char fg, unsigned char bg,
                 const unsigned char *font);
void draw_char_2x(unsigned char *fb, int pitch, int x, int y, char ch,
                  unsigned char fg, unsigned char bg,
                  const unsigned char *font);
void draw_str_2x(unsigned char *fb, int pitch, int x, int y, const char *s,
                 unsigned char fg, unsigned char bg,
                 const unsigned char *font);

/* Vsync */
void wait_vsync(void);

/* HUD utilities */
void find_hud_colours(const unsigned char *pal);
void show_palette_grid(unsigned char *lfb, int lfb_pitch,
                       const unsigned char *font);

/* Blit */
void blit_to_lfb(unsigned char *lfb, int lfb_pitch,
                 const unsigned char *src);
#ifdef __DJGPP__
void wc_memcpy(void *dst, const void *src, unsigned long count);
#else
void wc_rep_movsd(void *dst, const void *src, unsigned long dwords);
#endif

/* Timing */
void rdtsc_read(unsigned long *lo, unsigned long *hi);
double tsc_to_ms(unsigned long lo1, unsigned long hi1,
                 unsigned long lo0, unsigned long hi0);
void calibrate_rdtsc(void);

#endif /* VGA_H */
