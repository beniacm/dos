/*
 * PLASMA.C  -  8-bit VESA plasma effect, 1024x768, vsync demonstration
 * OpenWatcom 2.0, 32-bit DOS/4GW
 *
 * Demonstrates:
 *   - Classic 4-component plasma (horizontal, vertical, diagonal, radial waves)
 *   - VBE 2.0+ linear frame buffer in 1024x768 8bpp
 *   - VBE 4F09h palette programming (works on Radeon in VESA modes)
 *   - VBE 2.0 hardware double buffering via INT 10h AX=4F07h page flip
 *     (renders into system RAM, blits to VRAM back page, then flips)
 *   - VBE 3.0 Protected Mode Interface (PMI) for faster page flip via
 *     DPMI 0301h direct procedure call (bypasses INT 10h dispatch)
 *   - Falls back to single-buffer + blit if VRAM < 2 pages or 4F07h fails
 *   - Vertical retrace sync to eliminate frame tearing
 *
 * Performance note:
 *   Rendering is always done into cached system RAM, then blitted to VRAM.
 *   Direct VRAM writes are extremely slow (~100ns/byte uncached), which
 *   limited frame rate to ~13 FPS on real hardware.  The system RAM + blit
 *   approach uses fast cached writes and WC-friendly sequential memcpy.
 *
 * Controls:
 *   V     = toggle vsync on/off  (watch for tear line when OFF)
 *   ESC   = quit
 *
 * Options:
 *   -vbe2 = force VBE 2.0 mode (disable VBE 3.0 PMI)
 *
 * Build:
 *   wcc386 -bt=dos -3r -ox -s -zq PLASMA.C
 *   wlink system dos4g name PLASMA file PLASMA option quiet
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <conio.h>
#include <i86.h>

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

#define WIDTH   1024
#define HEIGHT  768
#define PIXELS  ((unsigned long)WIDTH * HEIGHT)

/* --------------------------------------------------------------------------
 * Packed VBE / DPMI structures  (identical layout to VESADEMO.C)
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
 * DPMI state
 * -------------------------------------------------------------------------- */

static unsigned short g_dos_seg = 0;
static unsigned short g_dos_sel = 0;

/* VBE 3.0 / PMI state */
static int           g_vbe3         = 0;    /* non-zero when VBE >= 3.0     */
static int           g_force_vbe2   = 0;    /* non-zero to disable VBE 3.0  */
static unsigned short g_vbe_version = 0;    /* raw VBE version (BCD)        */
static int           g_pmi_ok       = 0;    /* non-zero when PMI page-flip  */
static unsigned short g_pmi_rm_seg  = 0;    /* real-mode segment of PMI tbl */
static unsigned short g_pmi_rm_off  = 0;    /* real-mode offset  of PMI tbl */
static unsigned short g_pmi_size    = 0;    /* PMI table size in bytes      */
static unsigned short g_pmi_setw_off  = 0;  /* SetWindow entry offset       */
static unsigned short g_pmi_setds_off = 0;  /* SetDisplayStart entry offset */
static unsigned short g_pmi_setpal_off= 0;  /* SetPrimaryPalette offset     */

static unsigned char *dos_buf(void)
{
    return (unsigned char *)((unsigned long)g_dos_seg << 4);
}

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

static int dpmi_real_int(unsigned char intno, RMI *rmi)
{
    union REGS  r;
    struct SREGS sr;
    segread(&sr);
    memset(&r, 0, sizeof(r));
    r.x.eax = 0x0300;
    r.x.ebx = intno;
    r.x.ecx = 0;
    r.x.edi = (unsigned int)rmi;
    int386x(0x31, &r, &r, &sr);
    return !(r.x.cflag);
}

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
    linear = ((unsigned long)(r.x.ebx & 0xFFFF) << 16) | (r.x.ecx & 0xFFFF);
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

/* --------------------------------------------------------------------------
 * VBE functions
 * -------------------------------------------------------------------------- */

static int vbe_get_info(VBEInfo *out)
{
    RMI rmi;
    VBEInfo *buf = (VBEInfo *)dos_buf();
    memset(&rmi, 0, sizeof(rmi));
    memset(buf, 0, 512);
    memcpy(buf->vbe_signature, "VBE2", 4);
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

static int vbe_set_mode(unsigned short mode)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F02;
    rmi.ebx = (unsigned long)mode | 0x4000;   /* bit 14 = LFB */
    if (!dpmi_real_int(0x10, &rmi)) return 0;
    return ((rmi.eax & 0xFFFF) == 0x004F);
}

static void vbe_set_text_mode(void)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x0003;
    dpmi_real_int(0x10, &rmi);
}

/*
 * VBE 4F07h — Set Display Start (page flip).
 * cx  = first displayed pixel in scan line (0 = left)
 * dy  = first displayed scan line (= page_index * HEIGHT for page flip)
 * wait = 1 → BL=80h: block until next vsync, then flip (no tearing)
 *        0 → BL=00h: flip immediately (may tear)
 * Returns 1 on success.
 *
 * Works in 32-bit protected mode: the INT 10h call is issued via DPMI
 * INT 31h AX=0300h (simulate real-mode interrupt), exactly the same
 * mechanism used for all other VBE calls in this program.
 */
static int vbe_set_display_start(unsigned short cx, unsigned short dy,
                                 int wait)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F07;
    rmi.ebx = wait ? 0x0080UL : 0x0000UL;  /* BH=0, BL=80h=vsync or 0 */
    rmi.ecx = (unsigned long)cx;
    rmi.edx = (unsigned long)dy;
    if (!dpmi_real_int(0x10, &rmi)) return 0;
    return ((rmi.eax & 0xFFFF) == 0x004F);
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

/* Call a real-mode far procedure via DPMI 0301h.
 * Avoids INT 10h dispatch overhead vs dpmi_real_int (0300h). */
static int dpmi_call_rm_proc(RMI *rmi)
{
    union REGS  r;
    struct SREGS sr;
    segread(&sr);
    memset(&r, 0, sizeof(r));
    r.x.eax = 0x0301;
    r.x.ebx = 0;          /* BH = flags = 0 */
    r.x.ecx = 0;          /* no stack words to copy */
    r.x.edi = (unsigned int)rmi;
    int386x(0x31, &r, &r, &sr);
    return !(r.x.cflag);
}

/*
 * VBE 3.0 PMI page flip: call SetDisplayStart entry directly via
 * DPMI 0301h (call real-mode far procedure).  Bypasses the INT 10h
 * dispatch and VBE function-number lookup — faster per-frame.
 */
static int pmi_set_display_start(unsigned short cx, unsigned short dy,
                                 int wait)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F07;
    rmi.ebx = wait ? 0x0080UL : 0x0000UL;
    rmi.ecx = (unsigned long)cx;
    rmi.edx = (unsigned long)dy;
    rmi.cs  = g_pmi_rm_seg;
    rmi.ip  = g_pmi_rm_off + g_pmi_setds_off;
    if (!dpmi_call_rm_proc(&rmi)) return 0;
    return ((rmi.eax & 0xFFFF) == 0x004F);
}

/* --------------------------------------------------------------------------
 * DAC width + VBE 4F09h palette
 * -------------------------------------------------------------------------- */

static int g_dac_bits = 6;

static void init_dac(void)
{
    RMI rmi;
    int got;

    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F08;  rmi.ebx = 0x0800;   /* try 8-bit */
    dpmi_real_int(0x10, &rmi);

    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F08;  rmi.ebx = 0x0100;   /* query actual */
    if (dpmi_real_int(0x10, &rmi) && (rmi.eax & 0xFFFF) == 0x004F)
        got = (int)((rmi.ebx >> 8) & 0xFF);
    else
        got = 6;
    if (got < 6) got = 6;
    if (got > 8) got = 8;
    g_dac_bits = got;
}

/*
 * pal: count*4 bytes in R,G,B,pad order (8-bit values).
 * Converts to VBE format B,G,R,pad at g_dac_bits resolution.
 * Buffer must hold count*4 bytes; max 256 entries = 1024 bytes.
 */
static void vbe_set_palette(int start, int count, const unsigned char *pal)
{
    RMI rmi;
    unsigned char *buf = dos_buf();
    int i, shift;

    shift = 8 - g_dac_bits;
    for (i = 0; i < count; i++) {
        buf[i*4+0] = pal[i*4+2] >> shift;   /* B */
        buf[i*4+1] = pal[i*4+1] >> shift;   /* G */
        buf[i*4+2] = pal[i*4+0] >> shift;   /* R */
        buf[i*4+3] = 0;
    }

    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F09;
    rmi.ebx = 0x0000;               /* BL=00: set (BL=80 = during vretrace) */
    rmi.ecx = (unsigned long)count;
    rmi.edx = (unsigned long)start;
    rmi.es  = g_dos_seg;
    rmi.edi = 0;
    dpmi_real_int(0x10, &rmi);
}

/* --------------------------------------------------------------------------
 * BIOS 8x8 font
 * -------------------------------------------------------------------------- */

static unsigned char *get_bios_font_8x8(void)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x1130;
    rmi.ebx = 0x0300;   /* ROM 8x8 font, chars 0-127 */
    dpmi_real_int(0x10, &rmi);
    return (unsigned char *)(((unsigned long)rmi.es << 4) + (rmi.ebp & 0xFFFF));
}

/* --------------------------------------------------------------------------
 * Text rendering into the frame buffer (WIDTH-stride, 8bpp)
 * -------------------------------------------------------------------------- */

static void draw_char_bg(unsigned char *fb, int pitch,
                         int x, int y, char ch,
                         unsigned char fg, unsigned char bg,
                         const unsigned char *font)
{
    const unsigned char *glyph = font + (unsigned char)ch * 8;
    int row, col;
    for (row = 0; row < 8; row++) {
        unsigned char bits = glyph[row];
        for (col = 0; col < 8; col++)
            fb[(y+row)*pitch + x+col] = (bits & (0x80 >> col)) ? fg : bg;
    }
}

static void draw_str_bg(unsigned char *fb, int pitch,
                        int x, int y, const char *s,
                        unsigned char fg, unsigned char bg,
                        const unsigned char *font)
{
    for (; *s; s++, x += 8)
        draw_char_bg(fb, pitch, x, y, *s, fg, bg, font);
}

/* 2x scaled character */
static void draw_char_2x(unsigned char *fb, int pitch,
                         int x, int y, char ch,
                         unsigned char fg, unsigned char bg,
                         const unsigned char *font)
{
    const unsigned char *glyph = font + (unsigned char)ch * 8;
    int row, col, sr, sc;
    for (row = 0; row < 8; row++) {
        unsigned char bits = glyph[row];
        for (col = 0; col < 8; col++) {
            unsigned char c = (bits & (0x80 >> col)) ? fg : bg;
            for (sr = 0; sr < 2; sr++)
                for (sc = 0; sc < 2; sc++)
                    fb[(y+row*2+sr)*pitch + x+col*2+sc] = c;
        }
    }
}

static void draw_str_2x(unsigned char *fb, int pitch,
                        int x, int y, const char *s,
                        unsigned char fg, unsigned char bg,
                        const unsigned char *font)
{
    for (; *s; s++, x += 16)
        draw_char_2x(fb, pitch, x, y, *s, fg, bg, font);
}

/* --------------------------------------------------------------------------
 * Vertical retrace sync via VGA input status register
 * -------------------------------------------------------------------------- */

#define VGA_STAT1  0x3DA

static void wait_vsync(void)
{
    while (  inp(VGA_STAT1) & 0x08) {}   /* wait for current vsync to end   */
    while (!(inp(VGA_STAT1) & 0x08)) {}  /* wait for next vsync to begin    */
}

/* --------------------------------------------------------------------------
 * Plasma engine
 *
 * Uses four overlapping sine waves:
 *   wave 1: horizontal (x)
 *   wave 2: vertical   (y)
 *   wave 3: diagonal   (x+y)
 *   wave 4: radial     (distance from centre)
 *
 * Each wave is sampled from a precomputed 256-entry sine table.
 * The four 8-bit values are averaged to a single palette index.
 * -------------------------------------------------------------------------- */

static unsigned char  g_sintab[256];         /* 0..255                      */
static unsigned char *g_dist  = NULL;         /* WIDTH*HEIGHT bytes          */

static void init_plasma_tables(void)
{
    int x, y;
    double cx = WIDTH  / 2.0;
    double cy = HEIGHT / 2.0;
    double maxd = sqrt(cx*cx + cy*cy);

    for (x = 0; x < 256; x++)
        g_sintab[x] = (unsigned char)((sin(x * 6.283185307 / 256.0) + 1.0) * 127.5);

    printf("  Building distance table...\r");
    fflush(stdout);

    for (y = 0; y < HEIGHT; y++) {
        double dy = y - cy;
        for (x = 0; x < WIDTH; x++) {
            double dx  = x - cx;
            double d   = sqrt(dx*dx + dy*dy) / maxd * 255.0;
            g_dist[y * WIDTH + x] = (unsigned char)(d > 255.0 ? 255.0 : d);
        }
    }
}

/*
 * render_plasma: fill `buf` (WIDTH x HEIGHT, pitch = WIDTH) for frame `t`.
 * Each component uses a slightly different speed multiplier so the waves
 * drift independently and produce interesting interference patterns.
 */
static void render_plasma(unsigned char *buf, int pitch, unsigned int t)
{
    int x, y;
    unsigned int t1 = (t)         & 0xFF;   /* base speed                  */
    unsigned int t2 = (t + t/2)   & 0xFF;   /* 1.5x                        */
    unsigned int t3 = (t*2)       & 0xFF;   /* 2x – diagonal               */
    unsigned int t4 = (t*3)       & 0xFF;   /* 3x – radial (fast ripple)   */
    const unsigned char *dist_row = g_dist;

    for (y = 0; y < HEIGHT; y++) {
        unsigned char *dst = buf + (unsigned long)y * pitch;
        int vy = (int)g_sintab[((unsigned int)(y >> 1) + t1) & 0xFF];
        int dbase = (y >> 1) & 0xFF;

        for (x = 0; x < WIDTH; x++) {
            int v;
            v  = (int)g_sintab[((unsigned int)(x >> 1) + t2) & 0xFF];
            v += vy;
            v += (int)g_sintab[((unsigned int)((x >> 1) + dbase) + t3) & 0xFF];
            v += (int)g_sintab[((unsigned int)dist_row[x] + t4) & 0xFF];
            dst[x] = (unsigned char)(v >> 2);
        }
        dist_row += WIDTH;
    }
}

/* --------------------------------------------------------------------------
 * Plasma palette: 256-entry smooth triple-phase RGB cycle
 * -------------------------------------------------------------------------- */

static void build_plasma_palette(unsigned char *pal)
{
    int i;
    double t;

    for (i = 0; i < 256; i++) {
        t = i * 6.283185307 / 256.0;
        pal[i*4+0] = (unsigned char)((sin(t)                + 1.0) * 127.5); /* R */
        pal[i*4+1] = (unsigned char)((sin(t + 2.094395102)  + 1.0) * 127.5); /* G */
        pal[i*4+2] = (unsigned char)((sin(t + 4.188790205)  + 1.0) * 127.5); /* B */
        pal[i*4+3] = 0;
    }
    /* Reserve index 255 as pure white for HUD text */
    pal[255*4+0] = 255; pal[255*4+1] = 255; pal[255*4+2] = 255; pal[255*4+3] = 0;
    /* Index 0 = black for HUD background */
    pal[0*4+0] = 0; pal[0*4+1] = 0; pal[0*4+2] = 0; pal[0*4+3] = 0;
}

/* --------------------------------------------------------------------------
 * LFB blit helpers
 * -------------------------------------------------------------------------- */

/* Copy frame_buf (pitch=WIDTH) to LFB (pitch=lfb_pitch). */
static void blit_to_lfb(unsigned char *lfb, int lfb_pitch,
                         const unsigned char *src)
{
    int y;
    if (lfb_pitch == WIDTH) {
        memcpy(lfb, src, PIXELS);
    } else {
        for (y = 0; y < HEIGHT; y++)
            memcpy(lfb + (unsigned long)y * lfb_pitch,
                   src + (unsigned long)y * WIDTH, WIDTH);
    }
}

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */

int main(int argc, char *argv[])
{
    VBEInfo    vbi;
    VBEModeInfo vbmi;
    unsigned short target_mode;
    int            lfb_pitch;
    unsigned long  lfb_phys;
    unsigned char *lfb;
    unsigned char *frame_buf;       /* system-RAM render buffer          */
    unsigned char  pal[256*4];
    unsigned char *font;
    char           msg[96];
    unsigned int   t;
    int            vsync_on, running;
    int            use_doublebuf;   /* 1 = VBE 4F07h page flip active   */
    int            back_page;       /* 0 or 1                           */
    unsigned long  page_size;       /* bytes per VRAM page              */
    unsigned long  frame_count, last_ticks, now, elapsed;
    float          fps;
    volatile unsigned long *bios_ticks;
    int            i;

    /* BIOS timer-tick counter at flat address 0x46C  (~18.2 ticks/sec) */
    bios_ticks = (volatile unsigned long *)0x46CUL;

    /* Parse command-line flags */
    for (i = 1; i < argc; i++) {
        if (stricmp(argv[i], "-vbe2") == 0 ||
            stricmp(argv[i], "/vbe2") == 0) {
            g_force_vbe2 = 1;
        }
    }

    /* Conventional DOS memory:
     *   512 B VBEInfo  + 256 B ModeInfo  + 1024 B palette = 1792 B
     *   128 paragraphs = 2048 B - enough with room to spare              */
    if (!dpmi_alloc_dos(128)) {
        printf("DPMI: cannot allocate conventional memory\n");
        return 1;
    }

    /* ---- Locate 1024x768 8bpp LFB mode -------------------------------- */
    printf("Searching for 1024x768 8bpp LFB mode...\n");

    if (!vbe_get_info(&vbi)) {
        dpmi_free_dos();
        printf("VBE info call failed\n");
        return 1;
    }
    if (vbi.vbe_version < 0x0200) {
        dpmi_free_dos();
        printf("VBE 2.0+ required (detected version %04X)\n", vbi.vbe_version);
        return 1;
    }

    g_vbe_version = vbi.vbe_version;
    g_vbe3        = (vbi.vbe_version >= 0x0300);
    if (g_force_vbe2)
        g_vbe3 = 0;

    printf("VBE version : %d.%d%s\n",
           vbi.vbe_version >> 8, vbi.vbe_version & 0xFF,
           g_vbe3 ? " (VBE 3.0)" :
           g_force_vbe2 ? " (VBE 3.0 disabled by -vbe2)" : "");
    printf("Video memory: %u KB\n", (unsigned)vbi.total_memory * 64);

    /* VBE 3.0: query Protected Mode Interface */
    if (g_vbe3) {
        g_pmi_ok = query_pmi();
        if (g_pmi_ok)
            printf("PMI        : available at %04X:%04X  SetDisplayStart=%04X\n",
                   g_pmi_rm_seg, g_pmi_rm_off, g_pmi_setds_off);
        else
            printf("PMI        : not available\n");
    }

    {
        /* Copy mode list to a local array BEFORE any vbe_get_mode_info calls.
         * The mode list pointer often points into our DOS transfer buffer; each
         * vbe_get_mode_info call zeros the first 256 bytes of that buffer,
         * corrupting the list on cards (e.g. Radeon X1300) that store it there. */
        unsigned short  mode_list[512];
        unsigned short *src;
        int             i, count;
        unsigned long   ml_seg = (vbi.video_mode_ptr >> 16) & 0xFFFF;
        unsigned long   ml_off =  vbi.video_mode_ptr        & 0xFFFF;

        src = (unsigned short *)(ml_seg * 16 + ml_off);
        for (count = 0; count < 511 && src[count] != 0xFFFF; count++)
            mode_list[count] = src[count];
        mode_list[count] = 0xFFFF;

        target_mode = 0xFFFF;
        lfb_phys    = 0;
        lfb_pitch   = WIDTH;

        for (i = 0; i < count && target_mode == 0xFFFF; i++) {
            if (!vbe_get_mode_info(mode_list[i], &vbmi)) continue;
            if (vbmi.x_resolution       != WIDTH)  continue;
            if (vbmi.y_resolution       != HEIGHT) continue;
            if (vbmi.bits_per_pixel     != 8)      continue;
            if (!(vbmi.mode_attributes & 0x01))    continue;  /* supported  */
            if (!(vbmi.mode_attributes & 0x10))    continue;  /* graphics   */
            if (!(vbmi.mode_attributes & 0x80))    continue;  /* LFB        */
            if (!vbmi.phys_base_ptr)               continue;
            target_mode = mode_list[i];
            lfb_phys    = vbmi.phys_base_ptr;
            lfb_pitch   = vbmi.bytes_per_scan_line;
        }
    }

    if (target_mode == 0xFFFF) {
        dpmi_free_dos();
        printf("1024x768 8bpp LFB mode not found\n");
        return 1;
    }
    printf("Found mode 0x%03X  LFB at 0x%08lX  pitch=%d\n",
           target_mode, lfb_phys, lfb_pitch);

    /* ---- Check VRAM for double buffering ------------------------------ */
    page_size     = (unsigned long)lfb_pitch * HEIGHT;
    use_doublebuf = 0;
    back_page     = 0;

    {
        unsigned long total_vram = (unsigned long)vbi.total_memory * 65536UL;
        printf("VRAM: %luKB  page: %luKB  ",
               total_vram / 1024UL, page_size / 1024UL);
        if (total_vram >= page_size * 2UL) {
            printf("-> double-buffer candidate\n");
            use_doublebuf = 1;   /* confirmed after 4F07h test below    */
        } else {
            printf("-> single-buffer (insufficient VRAM)\n");
        }
    }

    /* ---- Allocate distance table (always needed) ---------------------- */
    g_dist = (unsigned char *)malloc(PIXELS);
    if (!g_dist) {
        dpmi_free_dos();
        printf("Out of memory (distance table)\n");
        return 1;
    }

    /* ---- Allocate system-RAM frame buffer (always needed) ------------- */
    /*
     * Rendering plasma directly into VRAM is extremely slow because LFB
     * writes are uncached (UC) or write-combining (WC).  Individual byte
     * writes at ~100ns each × 786K pixels ≈ 78ms/frame → ~13 FPS.
     * Instead, always render into fast cached system RAM, then blit to
     * VRAM in one sequential burst (WC-friendly memcpy).
     */
    frame_buf = (unsigned char *)malloc(PIXELS);
    if (!frame_buf) {
        free(g_dist);
        dpmi_free_dos();
        printf("Out of memory (frame buffer)\n");
        return 1;
    }

    printf("Initialising plasma tables...\n");
    init_plasma_tables();
    printf("Done.                          \n");

    /* ---- Map LFB (1 page for single-buf, 2 pages for double-buf) ------ */
    {
        unsigned long map_size = use_doublebuf ? page_size * 2UL : page_size;
        lfb = (unsigned char *)dpmi_map_physical(lfb_phys, map_size);
    }
    if (!lfb) {
        free(frame_buf);
        free(g_dist);
        dpmi_free_dos();
        printf("Cannot map LFB at 0x%08lX\n", lfb_phys);
        return 1;
    }

    /* ---- Set video mode ------------------------------------------------ */
    if (!vbe_set_mode(target_mode)) {
        dpmi_unmap_physical(lfb);
        free(frame_buf);
        free(g_dist);
        dpmi_free_dos();
        printf("Set mode 0x%03X failed\n", target_mode);
        return 1;
    }

    /* ---- Test VBE 4F07h support (must be after set mode) -------------- */
    if (use_doublebuf) {
        if (!vbe_set_display_start(0, 0, 0)) {
            printf("4F07h not supported - falling back to single-buffer\n");
            use_doublebuf = 0;
        } else {
            back_page = 1;   /* start rendering into page 1 */
            /* Test PMI SetDisplayStart if available */
            if (g_pmi_ok) {
                if (!pmi_set_display_start(0, 0, 0)) {
                    printf("PMI SetDisplayStart failed - using standard 4F07h\n");
                    g_pmi_ok = 0;
                    /* Reset display start via standard call */
                    vbe_set_display_start(0, 0, 0);
                }
            }
        }
    }

    /* ---- Palette ------------------------------------------------------- */
    init_dac();
    build_plasma_palette(pal);
    vbe_set_palette(0, 256, pal);

    /* ---- BIOS font ----------------------------------------------------- */
    font = get_bios_font_8x8();

    /* ---- Main loop ----------------------------------------------------- */
    t           = 0;
    vsync_on    = 1;
    running     = 1;
    frame_count = 0;
    last_ticks  = *bios_ticks;
    fps         = 0.0f;

    while (running) {

        /* --- Keyboard --------------------------------------------------- */
        while (kbhit()) {
            int k = getch();
            if (k == 27) {
                running = 0;
            } else if (k == 'v' || k == 'V') {
                vsync_on = !vsync_on;
            }
        }

        /* --- Render plasma into system RAM (fast cached writes) --------- */
        render_plasma(frame_buf, WIDTH, t);

        /* --- HUD overlay (into system RAM) ------------------------------ */
        {
            const char *sync_str = vsync_on ? "ON " : "OFF";
            const char *buf_str  = use_doublebuf ? "DBL" : "SGL";
            const char *pmi_str  = g_pmi_ok ? "YES" : "NO ";
            sprintf(msg, "VSYNC:%s BUF:%s PMI:%s FPS:%5.1f  [V]=toggle [ESC]=quit",
                    sync_str, buf_str, pmi_str, fps);
            draw_str_bg(frame_buf, WIDTH, 4, 4, msg, 255, 0, font);
        }

        if (vsync_on) {
            draw_str_2x(frame_buf, WIDTH, 4, 16,
                        "VSYNC ON  - tearing suppressed   ",
                        200, 0, font);
        } else {
            draw_str_2x(frame_buf, WIDTH, 4, 16,
                        "VSYNC OFF - watch for tear line! ",
                        240, 0, font);
        }

        /* --- Present frame ---------------------------------------------- */
        if (use_doublebuf) {
            /* Blit from system RAM to VRAM back page (sequential, WC-fast) */
            blit_to_lfb(lfb + (unsigned long)back_page * page_size,
                        lfb_pitch, frame_buf);
            /* Page flip: use PMI (faster) or standard VBE 4F07h */
            if (g_pmi_ok)
                pmi_set_display_start(0, (unsigned short)(back_page * HEIGHT),
                                      vsync_on);
            else
                vbe_set_display_start(0, (unsigned short)(back_page * HEIGHT),
                                      vsync_on);
            back_page = 1 - back_page;          /* toggle for next frame   */
        } else {
            /* Single-buffer: optional software vsync + blit              */
            if (vsync_on)
                wait_vsync();
            blit_to_lfb(lfb, lfb_pitch, frame_buf);
        }

        /* --- Advance animation ------------------------------------------ */
        t += 2;
        frame_count++;

        /* --- FPS counter (updated ~once per second) --------------------- */
        now     = *bios_ticks;
        elapsed = now - last_ticks;
        if (elapsed >= 18UL) {
            fps         = (float)frame_count * 18.2f / (float)elapsed;
            frame_count = 0;
            last_ticks  = now;
        }
    }

    /* ---- Cleanup ------------------------------------------------------- */
    vbe_set_text_mode();
    dpmi_unmap_physical(lfb);
    free(frame_buf);
    free(g_dist);
    dpmi_free_dos();

    printf("PLASMA done.  mode=0x%03X  DAC=%d-bit  buf:%s  PMI:%s\n",
           target_mode, g_dac_bits,
           use_doublebuf ? "double" : "single",
           g_pmi_ok ? "yes" : "no");
    return 0;
}
