/*
 * VESADEMO.C - VESA Linear Frame Buffer 8bpp Demo
 * For OpenWatcom 2.0, 32-bit DOS (DOS/4GW)
 *
 * Demonstrates VESA VBE 2.0+ linear frame buffer access in all
 * available 8-bit (256 color) graphics resolutions.
 *
 * Build:  wcl386 -bt=dos -l=dos4g -ox vesademo.c
 *    or:  wmake
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <conio.h>
#include <i86.h>

/* ===== Packed structures for VBE BIOS interface ===== */

#pragma pack(1)

typedef struct {
    char           vbe_signature[4];    /* "VESA" on return          */
    unsigned short vbe_version;         /* BCD version (0x0200=2.0)  */
    unsigned long  oem_string_ptr;      /* real-mode far ptr         */
    unsigned long  capabilities;
    unsigned long  video_mode_ptr;      /* real-mode far ptr to list */
    unsigned short total_memory;        /* in 64 KB blocks           */
    unsigned short oem_software_rev;
    unsigned long  oem_vendor_name_ptr;
    unsigned long  oem_product_name_ptr;
    unsigned long  oem_product_rev_ptr;
    unsigned char  reserved[222];
    unsigned char  oem_data[256];
} VBEInfo;                              /* 512 bytes total           */

typedef struct {
    unsigned short mode_attributes;     /* +00 */
    unsigned char  win_a_attributes;    /* +02 */
    unsigned char  win_b_attributes;    /* +03 */
    unsigned short win_granularity;     /* +04 */
    unsigned short win_size;            /* +06 */
    unsigned short win_a_segment;       /* +08 */
    unsigned short win_b_segment;       /* +0A */
    unsigned long  win_func_ptr;        /* +0C */
    unsigned short bytes_per_scan_line; /* +10 */
    unsigned short x_resolution;        /* +12 */
    unsigned short y_resolution;        /* +14 */
    unsigned char  x_char_size;         /* +16 */
    unsigned char  y_char_size;         /* +17 */
    unsigned char  number_of_planes;    /* +18 */
    unsigned char  bits_per_pixel;      /* +19 */
    unsigned char  number_of_banks;     /* +1A */
    unsigned char  memory_model;        /* +1B */
    unsigned char  bank_size;           /* +1C */
    unsigned char  number_of_image_pages; /* +1D */
    unsigned char  reserved1;           /* +1E */
    unsigned char  red_mask_size;       /* +1F */
    unsigned char  red_field_position;  /* +20 */
    unsigned char  green_mask_size;     /* +21 */
    unsigned char  green_field_position;/* +22 */
    unsigned char  blue_mask_size;      /* +23 */
    unsigned char  blue_field_position; /* +24 */
    unsigned char  rsvd_mask_size;      /* +25 */
    unsigned char  rsvd_field_position; /* +26 */
    unsigned char  direct_color_info;   /* +27 */
    unsigned long  phys_base_ptr;       /* +28  VBE 2.0+ LFB addr   */
    unsigned long  reserved2;           /* +2C */
    unsigned short reserved3;           /* +30 */
    /* VBE 3.0+ fields (offset 0x32) */
    unsigned short lin_bytes_per_scan;  /* +32 */
    unsigned char  bnk_num_img_pages;   /* +34 */
    unsigned char  lin_num_img_pages;   /* +35 */
    unsigned char  lin_red_mask_size;   /* +36 */
    unsigned char  lin_red_field_pos;   /* +37 */
    unsigned char  lin_green_mask_size; /* +38 */
    unsigned char  lin_green_field_pos; /* +39 */
    unsigned char  lin_blue_mask_size;  /* +3A */
    unsigned char  lin_blue_field_pos;  /* +3B */
    unsigned char  lin_rsvd_mask_size;  /* +3C */
    unsigned char  lin_rsvd_field_pos;  /* +3D */
    unsigned long  max_pixel_clock;     /* +3E  max pixel clock in Hz */
    unsigned char  padding[190];        /* +42  pad to 256 bytes      */
} VBEModeInfo;

/* DPMI real-mode register structure for INT 31h AX=0300h */
typedef struct {
    unsigned long  edi, esi, ebp, reserved_zero;
    unsigned long  ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs;
    unsigned short ip, cs, sp, ss;
} RMI;

#pragma pack()

/* ===== VBE 3.0: CRTC timing block (50 bytes, passed in ES:DI on mode set) ===== */

#pragma pack(1)
typedef struct {
    unsigned short htotal;        /* Horizontal total (pixels)          */
    unsigned short hsyncs;        /* HSync start                        */
    unsigned short hsynce;        /* HSync end                          */
    unsigned short vtotal;        /* Vertical total (lines)             */
    unsigned short vsyncs;        /* VSync start                        */
    unsigned short vsynce;        /* VSync end                          */
    unsigned char  flags;         /* bit2=HSync neg, bit3=VSync neg     */
    unsigned long  pixel_clock;   /* Hz                                 */
    unsigned short refresh_rate;  /* units of 0.01 Hz                   */
    unsigned char  reserved[40];
} CRTC_INFO;                      /* 59 bytes total                     */
#pragma pack()

/* Standard 60 Hz CRTC timings for common resolutions (VESA DMT) */
typedef struct { unsigned short xres, yres; CRTC_INFO crtc; } CRTCEntry;
static const CRTCEntry g_crtc_table[] = {
    /* 640x480@59.94Hz */
    { 640,  480,  { 800, 656, 752, 525, 490, 492, 0x0C, 25175000UL, 5994, {0} } },
    /* 800x600@60.32Hz */
    { 800,  600,  { 1056, 840, 968, 628, 601, 605, 0x00, 40000000UL, 6032, {0} } },
    /* 1024x768@60.00Hz */
    { 1024, 768,  { 1344, 1048, 1184, 806, 771, 777, 0x0C, 65000000UL, 6000, {0} } },
    /* terminator */
    { 0, 0, { 0,0,0,0,0,0,0,0,0,{0} } }
};

/* ===== Mode table ===== */

#define MAX_MODES 64

typedef struct {
    unsigned short number;
    unsigned short xres;
    unsigned short yres;
    unsigned short pitch;       /* bytes per scan line */
    unsigned long  lfb_phys;
    unsigned long  lfb_size;
    unsigned long  max_pixel_clock; /* VBE 3.0: max pixel clock Hz, 0 if N/A */
} ModeEntry;

static ModeEntry g_modes[MAX_MODES];
static int       g_num_modes = 0;

/* ===== VBE 3.0 state ===== */
static int           g_vbe3         = 0;    /* non-zero when VBE >= 3.0     */
static int           g_no_crtc      = 0;    /* non-zero to skip CRTC override */
static unsigned short g_vbe_version = 0;    /* raw VBE version (BCD)        */
static int           g_pmi_ok       = 0;    /* non-zero when PMI available  */
static unsigned short g_pmi_rm_seg  = 0;    /* real-mode segment of PMI tbl */
static unsigned short g_pmi_rm_off  = 0;    /* real-mode offset  of PMI tbl */
static unsigned short g_pmi_size    = 0;    /* PMI table size in bytes      */
static unsigned short g_pmi_setw_off  = 0;  /* SetWindow entry offset       */
static unsigned short g_pmi_setds_off = 0;  /* SetDisplayStart entry offset */
static unsigned short g_pmi_setpal_off= 0;  /* SetPrimaryPalette offset     */

/* DOS conventional-memory buffer (allocated via DPMI) */
static unsigned short g_dos_seg = 0;   /* real-mode segment  */
static unsigned short g_dos_sel = 0;   /* protected selector */

/* ===== DPMI helpers ===== */

/* Allocate conventional (< 1 MB) memory via DPMI */
static int dpmi_alloc_dos(unsigned short paragraphs)
{
    union REGS r;
    memset(&r, 0, sizeof(r));
    r.x.eax = 0x0100;
    r.x.ebx = paragraphs;
    int386(0x31, &r, &r);
    if (r.x.cflag) return 0;
    g_dos_seg = (unsigned short)r.x.eax;
    g_dos_sel = (unsigned short)r.x.edx;
    return 1;
}

static void dpmi_free_dos(void)
{
    union REGS r;
    if (!g_dos_sel) return;
    memset(&r, 0, sizeof(r));
    r.x.eax = 0x0101;
    r.x.edx = g_dos_sel;
    int386(0x31, &r, &r);
    g_dos_sel = 0;
    g_dos_seg = 0;
}

/* Simulate a real-mode interrupt via DPMI function 0300h */
static int dpmi_real_int(unsigned char intno, RMI *rmi)
{
    union REGS r;
    struct SREGS sr;

    segread(&sr);
    memset(&r, 0, sizeof(r));
    r.x.eax = 0x0300;
    r.x.ebx = intno;          /* BL = int#, BH = 0 */
    r.x.ecx = 0;
    r.x.edi = (unsigned int)rmi;
    int386x(0x31, &r, &r, &sr);
    return !(r.x.cflag);
}

/* Map physical memory to linear address space */
static void *dpmi_map_physical(unsigned long phys, unsigned long size)
{
    union REGS r;
    unsigned long linear;

    memset(&r, 0, sizeof(r));
    r.x.eax = 0x0800;
    r.x.ebx = (phys >> 16) & 0xFFFF;
    r.x.ecx = phys & 0xFFFF;
    r.x.esi = (size >> 16) & 0xFFFF;
    r.x.edi = size & 0xFFFF;
    int386(0x31, &r, &r);
    if (r.x.cflag) return NULL;
    linear = ((r.x.ebx & 0xFFFF) << 16) | (r.x.ecx & 0xFFFF);
    return (void *)linear;
}

static void dpmi_unmap_physical(void *ptr)
{
    union REGS r;
    unsigned long addr = (unsigned long)ptr;

    memset(&r, 0, sizeof(r));
    r.x.eax = 0x0801;
    r.x.ebx = (addr >> 16) & 0xFFFF;
    r.x.ecx = addr & 0xFFFF;
    int386(0x31, &r, &r);
}

/* ===== VBE functions ===== */

/* Pointer to start of our DOS transfer buffer in flat address space */
static unsigned char *dos_buf(void)
{
    return (unsigned char *)((unsigned long)g_dos_seg << 4);
}

static int vbe_get_info(VBEInfo *out)
{
    RMI rmi;
    VBEInfo *buf = (VBEInfo *)dos_buf();

    memset(&rmi, 0, sizeof(rmi));
    memset(buf, 0, 512);
    memcpy(buf->vbe_signature, "VBE2", 4);   /* request VBE 2.0+ info */

    rmi.eax = 0x4F00;
    rmi.es  = g_dos_seg;
    rmi.edi = 0;

    if (!dpmi_real_int(0x10, &rmi)) return 0;
    if ((rmi.eax & 0xFFFF) != 0x004F) return 0;

    memcpy(out, buf, sizeof(VBEInfo));
    return 1;
}

static int vbe_get_mode_info(unsigned short mode, VBEModeInfo *out)
{
    RMI rmi;

    memset(&rmi, 0, sizeof(rmi));
    memset(dos_buf(), 0, 256);

    rmi.eax = 0x4F01;
    rmi.ecx = mode;
    rmi.es  = g_dos_seg;
    rmi.edi = 0;

    if (!dpmi_real_int(0x10, &rmi)) return 0;
    if ((rmi.eax & 0xFFFF) != 0x004F) return 0;

    memcpy(out, dos_buf(), sizeof(VBEModeInfo));
    return 1;
}

/* Set VESA mode with LFB (bit 14) and don't clear memory (bit 15 off) */
static int vbe_set_mode(unsigned short mode)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F02;
    rmi.ebx = (unsigned long)mode | 0x4000;   /* bit 14 = use LFB */

    if (!dpmi_real_int(0x10, &rmi)) return 0;
    return ((rmi.eax & 0xFFFF) == 0x004F);
}

/*
 * VBE 3.0: set mode with CRTC timing block.
 * BX bit 14 = LFB, bit 11 = use CRTC timing in ES:DI.
 * crtc must be in DOS conventional memory (write to dos_buf first).
 */
static int vbe_set_mode_crtc(unsigned short mode, const CRTC_INFO *crtc)
{
    RMI rmi;
    memcpy(dos_buf(), crtc, sizeof(CRTC_INFO));
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F02;
    rmi.ebx = (unsigned long)mode | 0x4000 | 0x0800; /* LFB + CRTC */
    rmi.es  = g_dos_seg;
    rmi.edi = 0;
    if (!dpmi_real_int(0x10, &rmi)) return 0;
    return ((rmi.eax & 0xFFFF) == 0x004F);
}

/* Look up CRTC timings for the given resolution; returns NULL if not found. */
static const CRTC_INFO *find_crtc(unsigned short xres, unsigned short yres)
{
    const CRTCEntry *e;
    for (e = g_crtc_table; e->xres; e++)
        if (e->xres == xres && e->yres == yres)
            return &e->crtc;
    return NULL;
}

/*
 * VBE 3.0: query Protected Mode Interface via INT 10h AX=4F0Ah.
 * Fills g_pmi_* globals.  Returns 1 on success, 0 if unavailable.
 */
static int query_pmi(void)
{
    RMI rmi;
    unsigned long pmi_flat;

    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F0A;
    rmi.ebx = 0x0000;   /* BL=00: get PM interface info */
    if (!dpmi_real_int(0x10, &rmi)) return 0;
    if ((rmi.eax & 0xFFFF) != 0x004F) return 0;

    g_pmi_rm_seg = (unsigned short)rmi.es;
    g_pmi_rm_off = (unsigned short)(rmi.edi & 0xFFFF);
    g_pmi_size   = (unsigned short)(rmi.ecx & 0xFFFF);

    if (g_pmi_size == 0) return 0;

    /* Read entry offsets from the PMI table header (first 6 bytes) */
    pmi_flat = (unsigned long)g_pmi_rm_seg * 16 + g_pmi_rm_off;
    g_pmi_setw_off   = *(unsigned short *)(pmi_flat + 0);
    g_pmi_setds_off  = *(unsigned short *)(pmi_flat + 2);
    g_pmi_setpal_off = *(unsigned short *)(pmi_flat + 4);

    return 1;
}

static void vbe_set_text_mode(void)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x0003;
    dpmi_real_int(0x10, &rmi);
}

/* ===== BIOS 8x8 Font ===== */

static unsigned char *get_bios_font_8x8(void)
{
    RMI rmi;
    unsigned long seg, off;

    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x1130;
    rmi.ebx = 0x0300;    /* BH=03: ROM 8x8 font, chars 0-127 */

    dpmi_real_int(0x10, &rmi);

    seg = rmi.es;
    off = rmi.ebp & 0xFFFF;
    return (unsigned char *)((seg << 4) + off);
}

/* ===== Drawing primitives ===== */

static void draw_pixel(unsigned char *fb, int pitch,
                       int x, int y, unsigned char c)
{
    fb[y * pitch + x] = c;
}

static void draw_hline(unsigned char *fb, int pitch,
                       int x1, int x2, int y, unsigned char c)
{
    if (x1 > x2) return;
    memset(fb + y * pitch + x1, c, x2 - x1 + 1);
}

static void draw_vline(unsigned char *fb, int pitch,
                       int x, int y1, int y2, unsigned char c)
{
    int y;
    for (y = y1; y <= y2; y++)
        fb[y * pitch + x] = c;
}

static void draw_rect(unsigned char *fb, int pitch,
                      int x1, int y1, int x2, int y2, unsigned char c)
{
    draw_hline(fb, pitch, x1, x2, y1, c);
    draw_hline(fb, pitch, x1, x2, y2, c);
    draw_vline(fb, pitch, x1, y1, y2, c);
    draw_vline(fb, pitch, x2, y1, y2, c);
}

static void fill_rect(unsigned char *fb, int pitch,
                      int x1, int y1, int x2, int y2, unsigned char c)
{
    int y;
    for (y = y1; y <= y2; y++)
        memset(fb + y * pitch + x1, c, x2 - x1 + 1);
}

/* Draw 8x8 character at 1x scale */
static void draw_char(unsigned char *fb, int pitch,
                      int x, int y, char ch, unsigned char color,
                      unsigned char *font)
{
    int row, col;
    unsigned char *glyph = font + (unsigned char)ch * 8;

    for (row = 0; row < 8; row++) {
        unsigned char bits = glyph[row];
        for (col = 0; col < 8; col++) {
            if (bits & (0x80 >> col))
                fb[(y + row) * pitch + x + col] = color;
        }
    }
}

static void draw_string(unsigned char *fb, int pitch,
                        int x, int y, const char *s, unsigned char color,
                        unsigned char *font)
{
    for (; *s; s++, x += 8)
        draw_char(fb, pitch, x, y, *s, color, font);
}

/* Draw character at Nx scale */
static void draw_char_big(unsigned char *fb, int pitch,
                          int x, int y, char ch, unsigned char color,
                          unsigned char *font, int scale)
{
    int row, col, sy, sx;
    unsigned char *glyph = font + (unsigned char)ch * 8;

    for (row = 0; row < 8; row++) {
        unsigned char bits = glyph[row];
        for (col = 0; col < 8; col++) {
            if (bits & (0x80 >> col)) {
                for (sy = 0; sy < scale; sy++)
                    for (sx = 0; sx < scale; sx++)
                        fb[(y + row * scale + sy) * pitch
                           + x + col * scale + sx] = color;
            }
        }
    }
}

static void draw_string_big(unsigned char *fb, int pitch,
                            int x, int y, const char *s,
                            unsigned char color, unsigned char *font,
                            int scale)
{
    for (; *s; s++, x += 8 * scale)
        draw_char_big(fb, pitch, x, y, *s, color, font, scale);
}

/* ===== VGA Palette via VBE 4F09h ===== */

static int g_dac_bits = 6;

/*
 * Try to set DAC to 8-bit via VBE 4F08h, then query actual width.
 */
static void init_dac(void)
{
    RMI rmi;
    int got;

    /* Attempt 8-bit DAC */
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F08;  rmi.ebx = 0x0800;
    dpmi_real_int(0x10, &rmi);

    /* Query actual width */
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F08;  rmi.ebx = 0x0100;
    if (dpmi_real_int(0x10, &rmi) && (rmi.eax & 0xFFFF) == 0x004F)
        got = (int)((rmi.ebx >> 8) & 0xFF);
    else
        got = 6;
    if (got < 6) got = 6;
    if (got > 8) got = 8;
    g_dac_bits = got;
}

/*
 * Set palette entries via VBE function 4F09h.
 * `pal` points to `count` entries, each 4 bytes: R, G, B, x  (8-bit values).
 * Values are scaled to the current DAC width before sending.
 * The BIOS receives them in its native format (B, G, R, pad).
 */
static void vbe_set_palette(int start, int count,
                            const unsigned char *pal)
{
    RMI rmi;
    unsigned char *buf = dos_buf();
    int i, shift;

    shift = 8 - g_dac_bits;

    /* VBE 4F09h expects 4 bytes per entry: Blue, Green, Red, padding */
    for (i = 0; i < count; i++) {
        buf[i * 4 + 0] = pal[i * 4 + 2] >> shift;   /* B */
        buf[i * 4 + 1] = pal[i * 4 + 1] >> shift;   /* G */
        buf[i * 4 + 2] = pal[i * 4 + 0] >> shift;   /* R */
        buf[i * 4 + 3] = 0;
    }

    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F09;
    rmi.ebx = 0x0000;          /* BL=00: set during vretrace */
    rmi.ecx = (unsigned long)count;
    rmi.edx = (unsigned long)start;
    rmi.es  = g_dos_seg;
    rmi.edi = 0;
    dpmi_real_int(0x10, &rmi);
}

/*
 * Palette layout (256 entries):
 *   0       : black
 *   1 -  32 : blue    gradient
 *  33 -  64 : green   gradient
 *  65 -  96 : red     gradient
 *  97 - 128 : cyan    gradient
 * 129 - 160 : yellow  gradient
 * 161 - 192 : magenta gradient
 * 193 - 224 : white   gradient
 * 248-255   : utility colours
 */
static void setup_palette(void)
{
    unsigned char pal[256 * 4];
    int i;

    init_dac();

    memset(pal, 0, sizeof(pal));

    /* entry 0 = black (already zeroed) */

    for (i = 0; i < 32; i++) {
        unsigned char v = (unsigned char)(i * 255 / 31);
        /* R, G, B, pad */
        pal[( 1+i)*4+0] = 0; pal[( 1+i)*4+1] = 0; pal[( 1+i)*4+2] = v;   /* blue */
        pal[(33+i)*4+0] = 0; pal[(33+i)*4+1] = v; pal[(33+i)*4+2] = 0;    /* green */
        pal[(65+i)*4+0] = v; pal[(65+i)*4+1] = 0; pal[(65+i)*4+2] = 0;    /* red */
        pal[(97+i)*4+0] = 0; pal[(97+i)*4+1] = v; pal[(97+i)*4+2] = v;    /* cyan */
        pal[(129+i)*4+0]= v; pal[(129+i)*4+1]= v; pal[(129+i)*4+2]= 0;    /* yellow */
        pal[(161+i)*4+0]= v; pal[(161+i)*4+1]= 0; pal[(161+i)*4+2]= v;    /* magenta */
        pal[(193+i)*4+0]= v; pal[(193+i)*4+1]= v; pal[(193+i)*4+2]= v;    /* gray */
    }

    /* Utility colours: R, G, B, 0 */
    pal[248*4+0]= 64; pal[248*4+1]= 64; pal[248*4+2]= 64;   /* dark gray */
    pal[249*4+0]=112; pal[249*4+1]=112; pal[249*4+2]=112;     /* med gray */
    pal[250*4+0]=  0; pal[250*4+1]=255; pal[250*4+2]=  0;     /* green */
    pal[251*4+0]=255; pal[251*4+1]=  0; pal[251*4+2]=  0;     /* red */
    pal[252*4+0]= 80; pal[252*4+1]= 80; pal[252*4+2]= 80;    /* border */
    pal[253*4+0]=  0; pal[253*4+1]=255; pal[253*4+2]=255;     /* cyan */
    pal[254*4+0]=255; pal[254*4+1]=255; pal[254*4+2]=  0;     /* yellow */
    pal[255*4+0]=255; pal[255*4+1]=255; pal[255*4+2]=255;     /* white */

    vbe_set_palette(0, 256, pal);
}

/* ===== Demo screen for one mode ===== */

static void draw_demo(unsigned char *fb, int xres, int yres, int pitch,
                      unsigned short mode_num, int idx, int total,
                      unsigned char *font, int crtc_used)
{
    int scale, title_h, info_h;
    int top_y, bot_y, avail_h;
    int num_bars, bar_h, bar_gap;
    int i, x, y, bx1, bx2;
    int pal_h, pal_y, blk_w, pal_x;
    int cx, cy, arm;
    char buf[80];

    /* --- clear --- */
    for (y = 0; y < yres; y++)
        memset(fb + y * pitch, 0, xres);

    /* --- border (double line) --- */
    draw_rect(fb, pitch, 0, 0, xres - 1, yres - 1, 255);
    draw_rect(fb, pitch, 1, 1, xres - 2, yres - 2, 252);

    /* --- corner markers --- */
    arm = (xres < 400) ? 12 : 24;
    draw_hline(fb, pitch, 3, 3 + arm, 3, 253);
    draw_vline(fb, pitch, 3, 3, 3 + arm, 253);
    draw_hline(fb, pitch, xres - 4 - arm, xres - 4, 3, 253);
    draw_vline(fb, pitch, xres - 4, 3, 3 + arm, 253);
    draw_hline(fb, pitch, 3, 3 + arm, yres - 4, 253);
    draw_vline(fb, pitch, 3, yres - 4 - arm, yres - 4, 253);
    draw_hline(fb, pitch, xres - 4 - arm, xres - 4, yres - 4, 253);
    draw_vline(fb, pitch, xres - 4, yres - 4 - arm, yres - 4, 253);

    /* --- centre crosshair --- */
    cx = xres / 2;
    cy = yres / 2;
    draw_hline(fb, pitch, cx - arm, cx + arm, cy, 252);
    draw_vline(fb, pitch, cx, cy - arm, cy + arm, 252);
    draw_pixel(fb, pitch, cx, cy, 255);

    /* --- text scaling --- */
    if      (xres >= 1024) scale = 4;
    else if (xres >= 640)  scale = 3;
    else if (xres >= 400)  scale = 2;
    else                   scale = 1;

    title_h = 8 * scale;
    info_h  = 8 * scale;

    /* --- title --- */
    sprintf(buf, "VESA LFB 8bpp - Mode 0x%03X", (unsigned)mode_num);
    bx1 = (xres - (int)strlen(buf) * 8 * scale) / 2;
    if (bx1 < 4) bx1 = 4;
    draw_string_big(fb, pitch, bx1, 6, buf, 255, font, scale);

    /* --- layout zones --- */
    top_y = 6 + title_h + 4;              /* below title       */
    bot_y = yres - 6 - 8 - 4 - info_h;   /* above footer text */

    /* Palette strip at the bottom of the middle zone */
    pal_h = (yres >= 400) ? 14 : 8;
    pal_y = bot_y - pal_h - 4;

    avail_h = pal_y - top_y - 4;

    /* --- gradient bars --- */
    num_bars = 7;
    bar_gap  = 2;
    bar_h = (avail_h - bar_gap * (num_bars - 1)) / num_bars;
    if (bar_h < 2) bar_h = 2;

    bx1 = 6;
    bx2 = xres - 7;

    for (i = 0; i < num_bars; i++) {
        int y0 = top_y + i * (bar_h + bar_gap);
        int y1 = y0 + bar_h - 1;
        int base = 1 + i * 32;

        if (y1 >= pal_y) y1 = pal_y - 1;
        if (y0 >= pal_y) break;

        for (y = y0; y <= y1; y++) {
            for (x = bx1; x <= bx2; x++) {
                int ci = (x - bx1) * 31 / (bx2 - bx1);
                fb[y * pitch + x] = (unsigned char)(base + ci);
            }
        }
    }

    /* --- full 256-colour palette strip --- */
    blk_w = (xres - 12) / 256;
    if (blk_w < 1) blk_w = 1;
    pal_x = (xres - 256 * blk_w) / 2;
    for (i = 0; i < 256; i++) {
        fill_rect(fb, pitch,
                  pal_x + i * blk_w, pal_y,
                  pal_x + i * blk_w + blk_w - 1, pal_y + pal_h - 1,
                  (unsigned char)i);
    }

    /* --- resolution info (scaled) --- */
    sprintf(buf, "%ux%u 8bpp  [%d/%d]",
            (unsigned)xres, (unsigned)yres, idx, total);
    bx1 = (xres - (int)strlen(buf) * 8 * scale) / 2;
    if (bx1 < 4) bx1 = 4;
    draw_string_big(fb, pitch, bx1, bot_y, buf, 254, font, scale);

    /* --- footer: VBE version + PMI + CRTC status (+ keyhelp if room) --- */
    {
        int vmaj = g_vbe_version >> 8, vmin = g_vbe_version & 0xFF;
        const char *pmi_s  = g_pmi_ok ? "YES" : "NO ";
        const char *crtc_s = crtc_used ? "60Hz" : "N/A";
        /* choose footer length based on available pixels (8px per char) */
        if ((int)xres >= 56 * 8)
            sprintf(buf, "VBE %d.%d  PMI:%s  CRTC:%s    [any key=next  ESC=quit]",
                    vmaj, vmin, pmi_s, crtc_s);
        else if ((int)xres >= 38 * 8)
            sprintf(buf, "VBE %d.%d PMI:%s CRTC:%s  [SPC/ESC]",
                    vmaj, vmin, pmi_s, crtc_s);
        else
            sprintf(buf, "VBE %d.%d PMI:%s CRTC:%s",
                    vmaj, vmin, pmi_s, crtc_s);
        bx1 = ((int)xres - (int)strlen(buf) * 8) / 2;
        if (bx1 < 4) bx1 = 4;
        draw_string(fb, pitch, bx1, yres - 6 - 8, buf, 253, font);
    }
}

/* ===== Simple sort by resolution (ascending) ===== */

static void sort_modes(void)
{
    int i, j;
    ModeEntry tmp;
    unsigned long a, b;

    for (i = 0; i < g_num_modes - 1; i++) {
        for (j = i + 1; j < g_num_modes; j++) {
            a = (unsigned long)g_modes[i].xres * g_modes[i].yres;
            b = (unsigned long)g_modes[j].xres * g_modes[j].yres;
            if (b < a) {
                tmp = g_modes[i];
                g_modes[i] = g_modes[j];
                g_modes[j] = tmp;
            }
        }
    }
}

/* ===== Main ===== */

int main(int argc, char *argv[])
{
    VBEInfo      vbe;
    VBEModeInfo  mi;
    unsigned short mode_list[512];
    unsigned short *src;
    unsigned long  seg, off;
    int i, count, ch;
    unsigned char *font;

    /* Parse command-line flags */
    for (i = 1; i < argc; i++) {
        if (stricmp(argv[i], "-nocrtc") == 0 ||
            stricmp(argv[i], "/nocrtc") == 0) {
            g_no_crtc = 1;
        }
    }

    printf("VESADEMO - VESA LFB 8bpp Resolution Demo\n");
    printf("=========================================\n\n");

    /* Allocate 512-byte DOS transfer buffer (32 paragraphs) */
    if (!dpmi_alloc_dos(64)) {
        printf("ERROR: Cannot allocate DOS memory.\n");
        return 1;
    }

    /* Get VBE controller info */
    if (!vbe_get_info(&vbe)) {
        printf("ERROR: VBE BIOS not found.\n");
        dpmi_free_dos();
        return 1;
    }

    if (memcmp(vbe.vbe_signature, "VESA", 4) != 0) {
        printf("ERROR: Invalid VBE signature.\n");
        dpmi_free_dos();
        return 1;
    }

    g_vbe_version = vbe.vbe_version;
    g_vbe3        = (vbe.vbe_version >= 0x0300);

    printf("VBE version : %d.%d%s\n",
           vbe.vbe_version >> 8, vbe.vbe_version & 0xFF,
           g_vbe3 ? " (VBE 3.0 features enabled)" : "");
    if (g_no_crtc)
        printf("CRTC override: disabled by -nocrtc flag\n");
    printf("Video memory: %u KB\n", (unsigned)vbe.total_memory * 64);

    if (vbe.vbe_version < 0x0200) {
        printf("ERROR: VBE 2.0 or later required for LFB support.\n");
        dpmi_free_dos();
        return 1;
    }

    /* VBE 3.0: query Protected Mode Interface */
    if (g_vbe3) {
        g_pmi_ok = query_pmi();
        if (g_pmi_ok) {
            printf("PMI        : available at %04X:%04X  size=%u\n",
                   g_pmi_rm_seg, g_pmi_rm_off, g_pmi_size);
            printf("  SetWindow=%04X  SetDisplayStart=%04X  SetPalette=%04X\n",
                   g_pmi_setw_off, g_pmi_setds_off, g_pmi_setpal_off);
        } else {
            printf("PMI        : not available\n");
        }
    }

    /* Copy mode list from real-mode pointer to local array.
       Must be done BEFORE any further VBE calls reuse the DOS buffer. */
    seg = (vbe.video_mode_ptr >> 16) & 0xFFFF;
    off = vbe.video_mode_ptr & 0xFFFF;
    src = (unsigned short *)((seg << 4) + off);

    for (count = 0; count < 511 && src[count] != 0xFFFF; count++)
        mode_list[count] = src[count];
    mode_list[count] = 0xFFFF;

    /* Scan for 8bpp LFB-capable modes */
    printf("\nScanning %d video modes for 8bpp + LFB...\n", count);

    for (i = 0; i < count && g_num_modes < MAX_MODES; i++) {
        if (!vbe_get_mode_info(mode_list[i], &mi)) continue;

        /* Filter: 8bpp, graphics, supported, LFB present, packed pixel */
        if (mi.bits_per_pixel != 8)             continue;
        if (!(mi.mode_attributes & 0x01))       continue; /* supported   */
        if (!(mi.mode_attributes & 0x10))       continue; /* graphics    */
        if (!(mi.mode_attributes & 0x80))       continue; /* LFB avail.  */
        if (mi.memory_model != 4)               continue; /* packed px   */
        if (mi.phys_base_ptr == 0)              continue;
        if (mi.x_resolution > 1024 || mi.y_resolution > 768) continue;

        g_modes[g_num_modes].number   = mode_list[i];
        g_modes[g_num_modes].xres     = mi.x_resolution;
        g_modes[g_num_modes].yres     = mi.y_resolution;
        g_modes[g_num_modes].pitch    = mi.bytes_per_scan_line;
        g_modes[g_num_modes].lfb_phys = mi.phys_base_ptr;
        g_modes[g_num_modes].lfb_size =
            ((unsigned long)mi.bytes_per_scan_line * mi.y_resolution
             + 4095UL) & ~4095UL;   /* round up to page */
        g_modes[g_num_modes].max_pixel_clock =
            g_vbe3 ? mi.max_pixel_clock : 0;

        {
            const CRTC_INFO *crtc = find_crtc(mi.x_resolution, mi.y_resolution);
            printf("  0x%03X : %4ux%-4u  pitch=%-5u  LFB=0x%08lX",
                   mode_list[i],
                   (unsigned)mi.x_resolution, (unsigned)mi.y_resolution,
                   (unsigned)mi.bytes_per_scan_line,
                   mi.phys_base_ptr);
            if (g_vbe3) {
                printf("  MaxClk=%lu", mi.max_pixel_clock);
                if (crtc) {
                    if (g_no_crtc)
                        printf("  CRTC:disabled");
                    else if (mi.max_pixel_clock &&
                             crtc->pixel_clock > mi.max_pixel_clock)
                        printf("  CRTC:clk-exceeds");
                    else
                        printf("  CRTC:avail");
                } else {
                    printf("  CRTC:no-timing");
                }
            }
            printf("\n");
        }

        g_num_modes++;
    }

    if (g_num_modes == 0) {
        printf("\nNo 8bpp LFB modes found.\n");
        dpmi_free_dos();
        return 1;
    }

    sort_modes();

    printf("\nFound %d mode(s), sorted low-to-high.\n", g_num_modes);
    printf("Press any key to begin demo...\n");
    getch();

    /* Get the 8x8 ROM font while still in text mode */
    font = get_bios_font_8x8();

    /* --- Demo loop --- */
    for (i = 0; i < g_num_modes; i++) {
        unsigned char *lfb;
        int set_ok;
        int crtc_used = 0;

        /*
         * VBE 3.0: attempt CRTC timing override if available and allowed.
         * If the CRTC mode set fails, fall back to standard mode set.
         * Some BIOSes (notably ATI/AMD) report VBE 3.0 but do not
         * properly support the CRTC override (bit 11 of BX).
         */
        if (g_vbe3 && !g_no_crtc) {
            const CRTC_INFO *crtc = find_crtc(g_modes[i].xres, g_modes[i].yres);
            if (crtc) {
                int clk_ok = 1;
                /* Validate against MaxPixelClock if reported */
                if (g_modes[i].max_pixel_clock &&
                    crtc->pixel_clock > g_modes[i].max_pixel_clock)
                    clk_ok = 0;

                if (clk_ok) {
                    set_ok = vbe_set_mode_crtc(g_modes[i].number, crtc);
                    if (set_ok)
                        crtc_used = 1;
                }
            }
        }

        /* Standard mode set (no CRTC): used as primary or fallback */
        if (!crtc_used)
            set_ok = vbe_set_mode(g_modes[i].number);

        if (!set_ok) continue;

        lfb = (unsigned char *)dpmi_map_physical(
                  g_modes[i].lfb_phys, g_modes[i].lfb_size);
        if (!lfb) {
            vbe_set_text_mode();
            printf("Failed to map LFB for mode 0x%03X, skipping.\n",
                   g_modes[i].number);
            getch();
            continue;
        }

        setup_palette();

        draw_demo(lfb, g_modes[i].xres, g_modes[i].yres,
                  g_modes[i].pitch, g_modes[i].number,
                  i + 1, g_num_modes, font, crtc_used);

        ch = getch();

        dpmi_unmap_physical(lfb);

        if (ch == 27) break;       /* ESC = quit early */
    }

    vbe_set_text_mode();
    dpmi_free_dos();

    printf("VESADEMO complete.  Showed %d of %d mode(s).\n",
           (i < g_num_modes) ? i + 1 : g_num_modes,
           g_num_modes);

    return 0;
}
