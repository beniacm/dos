/*
 * DUCKHUNT.C  -  QUACK HUNT : Doom-style Duck Hunt
 *
 * Wolfenstein 3D raycasting FPS where you hunt ducks in a labyrinth.
 * VBE 640x480x8bpp, MTRR WC + PAT fix, system-RAM render + LFB blit.
 *
 * Controls:
 *   W / Up        - Move forward
 *   S / Down      - Move backward
 *   A             - Strafe left
 *   D             - Strafe right
 *   Mouse X       - Turn left/right
 *   Mouse Y       - Look up/down
 *   Left click    - Shoot
 *   R             - Reload (resets ammo to 6)
 *   ESC           - Quit
 *
 * Command line:
 *   DUCKHUNT [-vbe2] [-pmi] [-nopmi] [-hwflip] [-sched] [-nomtrr] [-mtrrinfo] [-vsync] [-nodblbuf]
 *
 * Requires: PMODE/W (ring 0), VBE 2.0+, mouse driver (CTMOUSE etc.)
 *
 * Build:
 *   wcc386 -bt=dos -5r -fp5 -ox -s -zq DUCKHUNT.C
 *   wlink system pmodew name DUCKHUNT file DUCKHUNT option quiet
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <conio.h>
#include <i86.h>
#include <dos.h>

/* ========================================================================
 *  CONSTANTS
 * ======================================================================== */

#define WIDTH       640
#define HEIGHT      480
#define HUD_H       48
#define VIEW_H      (HEIGHT - HUD_H)

#define TEX_W       64
#define TEX_H       64
#define MAP_W       24
#define MAP_H       24
#define MAX_DUCKS   20
#define SPRITE_W    32
#define SPRITE_H    32
#define NUM_TEX     4

#define FOV_PLANE   0.66f
#define MOVE_SPEED  3.5f
#define STRAFE_SPEED 3.0f
#define MOUSE_SENS  0.003f
#define PITCH_SENS  1.5f
#define MAX_PITCH   200
#define PLAYER_RAD  0.25f
#define MAX_FOG     18.0f
#define DUCK_SIZE   0.5f

/* Palette: 16 hues x 16 shades = 256 */
#define HUE_GRAY    0
#define HUE_BRICK   1
#define HUE_STONE   2
#define HUE_WOOD    3
#define HUE_MOSS    4
#define HUE_METAL   5
#define HUE_SKY     6
#define HUE_FLOOR   7
#define HUE_DUCK    8
#define HUE_ORANGE  9
#define HUE_BLOOD   10
#define HUE_TAN     11
#define HUE_TEAL    12
#define HUE_PURPLE  13
#define HUE_YELLOW  14
#define HUE_RED     15

#define PAL(h,s)    ((unsigned char)((h)*16+(s)))

/* Scan codes */
#define SC_ESC   0x01
#define SC_W     0x11
#define SC_A     0x1E
#define SC_S     0x1F
#define SC_D     0x20
#define SC_R     0x13
#define SC_SPACE 0x39
#define SC_UP    0x48
#define SC_DOWN  0x50
#define SC_LEFT  0x4B
#define SC_RIGHT 0x4D
#define SC_LSHIFT 0x2A

/* VBE mode for 640x480x8 */
#define TARGET_MODE 0x101

/* MTRR MSR numbers */
#define MSR_MTRRCAP         0xFE
#define MSR_MTRR_PHYSBASE0  0x200
#define MSR_MTRR_PHYSMASK0  0x201
#define MSR_MTRR_DEF_TYPE   0x2FF
#define MSR_PAT             0x277

/* ========================================================================
 *  VBE STRUCTURES
 * ======================================================================== */

#pragma pack(push, 1)
typedef struct {
    char     sig[4];
    unsigned short version;
    unsigned long  oem_str;
    unsigned long  caps;
    unsigned long  mode_list;
    unsigned short total_memory;
    unsigned short oem_rev;
    unsigned long  oem_vendor;
    unsigned long  oem_product;
    unsigned long  oem_product_rev;
    unsigned char  reserved[222];
    unsigned char  oem_data[256];
} VbeInfoBlock;

typedef struct {
    unsigned short mode_attr;
    unsigned char  win_a_attr, win_b_attr;
    unsigned short win_granularity, win_size;
    unsigned short win_a_seg, win_b_seg;
    unsigned long  win_func;
    unsigned short bytes_per_line;
    unsigned short x_res, y_res;
    unsigned char  x_char, y_char, planes, bpp, banks, mem_model;
    unsigned char  bank_size, image_pages, reserved1;
    unsigned char  red_mask, red_pos, green_mask, green_pos;
    unsigned char  blue_mask, blue_pos, rsvd_mask, rsvd_pos;
    unsigned char  direct_color_info;
    unsigned long  phys_base;
    unsigned long  offscreen_offset;
    unsigned short offscreen_size;
    unsigned short lin_bytes_per_line;
    unsigned char  bank_image_pages;
    unsigned char  lin_image_pages;
    unsigned char  lin_red_mask, lin_red_pos;
    unsigned char  lin_green_mask, lin_green_pos;
    unsigned char  lin_blue_mask, lin_blue_pos;
    unsigned char  lin_rsvd_mask, lin_rsvd_pos;
    unsigned long  max_pixel_clock;
    unsigned char  reserved2[189];
} ModeInfoBlock;
#pragma pack(pop)

/* ========================================================================
 *  DPMI HELPERS
 * ======================================================================== */

typedef struct {
    unsigned long edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs, ip, cs, sp, ss;
} RMI;

static unsigned short g_dos_seg = 0, g_dos_sel = 0;

static int dpmi_alloc_dos(unsigned short paras)
{
    union REGS r;
    r.w.ax = 0x0100;
    r.w.bx = paras;
    int386(0x31, &r, &r);
    if (r.x.cflag) return 0;
    g_dos_seg = r.w.ax;
    g_dos_sel = r.w.dx;
    return 1;
}

static void dpmi_free_dos(void)
{
    union REGS r;
    if (!g_dos_sel) return;
    r.w.ax = 0x0101;
    r.w.dx = g_dos_sel;
    int386(0x31, &r, &r);
    g_dos_sel = 0;
}

static int dpmi_real_int(int intno, RMI *rmi)
{
    union REGS r;
    struct SREGS s;
    memset(&s, 0, sizeof(s));
    r.w.ax = 0x0300;
    r.w.bx = (unsigned short)intno;
    r.w.cx = 0;
    s.es = FP_SEG(rmi);
    r.x.edi = FP_OFF(rmi);
    int386x(0x31, &r, &r, &s);
    return !r.x.cflag;
}

static void far *dpmi_map_physical(unsigned long phys, unsigned long size)
{
    union REGS r;
    r.w.ax = 0x0800;
    r.w.bx = (unsigned short)(phys >> 16);
    r.w.cx = (unsigned short)(phys & 0xFFFF);
    r.w.si = (unsigned short)(size >> 16);
    r.w.di = (unsigned short)(size & 0xFFFF);
    int386(0x31, &r, &r);
    if (r.x.cflag) return NULL;
    return (void far *)(((unsigned long)r.w.bx << 16) | r.w.cx);
}

static void dpmi_unmap_physical(void far *addr)
{
    union REGS r;
    unsigned long la = (unsigned long)addr;
    r.w.ax = 0x0801;
    r.w.bx = (unsigned short)(la >> 16);
    r.w.cx = (unsigned short)(la & 0xFFFF);
    int386(0x31, &r, &r);
}

/* ========================================================================
 *  VBE FUNCTIONS
 * ======================================================================== */

static VbeInfoBlock  g_vbi;
static ModeInfoBlock g_mib;
static unsigned long g_lfb_phys = 0;
static int           g_lfb_pitch = 0;

/* VBE 3.0 / PMI state */
static int           g_vbe3 = 0;
static int           g_force_vbe2 = 0;
static int           g_pmi_ok = 0;
static unsigned short g_pmi_rm_seg = 0;
static unsigned short g_pmi_rm_off = 0;
static unsigned short g_pmi_size = 0;
static unsigned short g_pmi_setw_off = 0;
static unsigned short g_pmi_setds_off = 0;
static unsigned short g_pmi_setpal_off = 0;
static unsigned short g_vbe_version = 0;

/* Page flip state */
static int           g_use_doublebuf = 0;
static int           g_back_page = 0;
static unsigned long g_page_size = 0;
static int           g_sched_flip = 0;
static int           g_vsync_on = 0;

/* Hardware flip (direct GPU register) */
static int           g_hw_flip = 0;
static unsigned short g_gpu_iobase = 0;
static unsigned long g_fb_phys = 0;

static int vbe_get_info(void)
{
    RMI rmi;
    unsigned char *buf = (unsigned char *)((unsigned long)g_dos_seg << 4);
    memset(buf, 0, 512);
    memcpy(buf, "VBE2", 4);
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F00;
    rmi.es  = g_dos_seg;
    rmi.edi = 0;
    dpmi_real_int(0x10, &rmi);
    if ((rmi.eax & 0xFFFF) != 0x004F) return 0;
    memcpy(&g_vbi, buf, sizeof(g_vbi));
    return 1;
}

static int vbe_get_mode_info(unsigned short mode)
{
    RMI rmi;
    unsigned char *buf = (unsigned char *)((unsigned long)g_dos_seg << 4);
    memset(buf, 0, 256);
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F01;
    rmi.ecx = mode;
    rmi.es  = g_dos_seg;
    rmi.edi = 0;
    dpmi_real_int(0x10, &rmi);
    if ((rmi.eax & 0xFFFF) != 0x004F) return 0;
    memcpy(&g_mib, buf, sizeof(g_mib));
    return 1;
}

static int vbe_set_mode(unsigned short mode)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F02;
    rmi.ebx = mode | 0x4000;  /* LFB bit */
    dpmi_real_int(0x10, &rmi);
    return (rmi.eax & 0xFFFF) == 0x004F;
}

static void vbe_set_text_mode(void)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x0003;
    dpmi_real_int(0x10, &rmi);
}

/* ========================================================================
 *  VBE PALETTE (DAC via INT 10h / 4F09h)
 * ======================================================================== */

static int g_dac_bits = 6;

static void vbe_set_dac_width(int bits)
{
    RMI rmi;
    /* VBE 4F08h BL=00: Set DAC width (BH=desired bits)
     * VBE 4F08h BL=01: Get DAC width (returns BH=current bits)
     * NOTE: was incorrectly using 4F09h (palette data) which trashes IVT! */
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F08;
    rmi.ebx = 0x0001;  /* BL=01: get current DAC width */
    dpmi_real_int(0x10, &rmi);
    if ((rmi.eax & 0xFFFF) == 0x004F) {
        int current = (int)((rmi.ebx >> 8) & 0xFF);  /* BH = current width */
        if (current >= bits) { g_dac_bits = current; return; }
    }
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F08;
    rmi.ebx = (unsigned short)(bits << 8);  /* BH=bits, BL=00: set */
    dpmi_real_int(0x10, &rmi);
    if ((rmi.eax & 0xFFFF) == 0x004F)
        g_dac_bits = (int)((rmi.ebx >> 8) & 0xFF);  /* BH = actual width */
}

static void set_palette_entry(int idx, int r, int g, int b)
{
    unsigned char *buf = (unsigned char *)((unsigned long)g_dos_seg << 4);
    RMI rmi;
    buf[0] = (unsigned char)r;
    buf[1] = (unsigned char)g;
    buf[2] = (unsigned char)b;
    buf[3] = 0;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F09;
    rmi.ebx = 0x0000;
    rmi.ecx = 1;
    rmi.edx = (unsigned short)idx;
    rmi.es  = g_dos_seg;
    rmi.edi = 0;
    dpmi_real_int(0x10, &rmi);
}

static void set_palette_block(int start, int count, unsigned char *rgb)
{
    unsigned char *buf = (unsigned char *)((unsigned long)g_dos_seg << 4);    RMI rmi;
    int i;
    for (i = 0; i < count; i++) {
        buf[i * 4 + 0] = rgb[i * 3 + 0];
        buf[i * 4 + 1] = rgb[i * 3 + 1];
        buf[i * 4 + 2] = rgb[i * 3 + 2];
        buf[i * 4 + 3] = 0;
    }
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F09;
    rmi.ebx = 0x0000;
    rmi.ecx = (unsigned short)count;
    rmi.edx = (unsigned short)start;
    rmi.es  = g_dos_seg;
    rmi.edi = 0;
    dpmi_real_int(0x10, &rmi);
}

/* ========================================================================
 *  VBE PAGE FLIP (4F07h)
 * ======================================================================== */

static int vbe_set_display_start(unsigned short cx, unsigned short dy, int wait)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F07;
    rmi.ebx = wait ? 0x0080UL : 0x0000UL;
    rmi.ecx = (unsigned long)cx;
    rmi.edx = (unsigned long)dy;
    if (!dpmi_real_int(0x10, &rmi)) return 0;
    return ((rmi.eax & 0xFFFF) == 0x004F);
}

static int vbe_schedule_display_start(unsigned short cx, unsigned short dy)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F07;
    rmi.ebx = 0x0002;
    rmi.ecx = (unsigned long)cx;
    rmi.edx = (unsigned long)dy;
    if (!dpmi_real_int(0x10, &rmi)) return 0;
    return ((rmi.eax & 0xFFFF) == 0x004F);
}

static int vbe_is_flip_complete(void)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F07;
    rmi.ebx = 0x0004;
    if (!dpmi_real_int(0x10, &rmi)) return 1;
    if ((rmi.eax & 0xFFFF) != 0x004F) return 1;
    return ((rmi.ebx & 0xFF00) == 0);
}

/* ========================================================================
 *  VBE 3.0 PROTECTED MODE INTERFACE (PMI)
 * ======================================================================== */

static int query_pmi(void)
{
    RMI rmi;
    unsigned long pmi_flat;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F0A;
    rmi.ebx = 0x0000;
    if (!dpmi_real_int(0x10, &rmi)) return 0;
    if ((rmi.eax & 0xFFFF) != 0x004F) return 0;
    g_pmi_rm_seg = (unsigned short)rmi.es;
    g_pmi_rm_off = (unsigned short)(rmi.edi & 0xFFFF);
    g_pmi_size   = (unsigned short)(rmi.ecx & 0xFFFF);
    if (g_pmi_size == 0) return 0;
    pmi_flat = (unsigned long)g_pmi_rm_seg * 16 + g_pmi_rm_off;
    g_pmi_setw_off   = *(unsigned short *)(pmi_flat + 0);
    g_pmi_setds_off  = *(unsigned short *)(pmi_flat + 2);
    g_pmi_setpal_off = *(unsigned short *)(pmi_flat + 4);
    return 1;
}

unsigned long _pmi_call4(unsigned long entry, unsigned long eax,
                         unsigned long ebx,  unsigned long ecx,
                         unsigned long edx);
#pragma aux _pmi_call4 = \
    "call esi"           \
    parm [esi] [eax] [ebx] [ecx] [edx] \
    value [eax]          \
    modify [ebx ecx edx esi edi]

unsigned long _pmi_call4_ebx(unsigned long entry, unsigned long eax,
                              unsigned long ebx,  unsigned long ecx,
                              unsigned long edx);
#pragma aux _pmi_call4_ebx = \
    "call esi"               \
    parm [esi] [eax] [ebx] [ecx] [edx] \
    value [ebx]              \
    modify [eax ecx edx esi edi]

static int pmi_set_display_start(unsigned short cx, unsigned short dy, int wait)
{
    unsigned long entry = (unsigned long)g_pmi_rm_seg * 16
                        + g_pmi_rm_off + g_pmi_setds_off;
    unsigned long result;
    result = _pmi_call4(entry, 0x4F07,
                        wait ? 0x0080UL : 0x0000UL,
                        (unsigned long)cx,
                        (unsigned long)dy);
    return ((result & 0xFFFF) == 0x004F);
}

static int pmi_schedule_display_start(unsigned short cx, unsigned short dy)
{
    unsigned long entry = (unsigned long)g_pmi_rm_seg * 16
                        + g_pmi_rm_off + g_pmi_setds_off;
    unsigned long result;
    result = _pmi_call4(entry, 0x4F07, 0x0002UL,
                        (unsigned long)cx,
                        (unsigned long)dy);
    return ((result & 0xFFFF) == 0x004F);
}

static int pmi_is_flip_complete(void)
{
    unsigned long entry = (unsigned long)g_pmi_rm_seg * 16
                        + g_pmi_rm_off + g_pmi_setds_off;
    unsigned long ebx_out;
    ebx_out = _pmi_call4_ebx(entry, 0x4F07, 0x0004UL, 0, 0);
    return ((ebx_out & 0xFF00) == 0);
}

/* ========================================================================
 *  GPU REGISTER ACCESS / HARDWARE FLIP (R500)
 * ======================================================================== */

#define R5_D1GRPH_PRIMARY_SURFACE_ADDRESS   0x6110
#define R5_D1GRPH_SECONDARY_SURFACE_ADDRESS 0x6118
#define R5_D1GRPH_UPDATE                    0x6144
#define R5_D1GRPH_SURFACE_UPDATE_PENDING    (1UL << 2)
#define R5_D1GRPH_SURFACE_UPDATE_LOCK       (1UL << 16)

void _gpu_outpd(unsigned short port, unsigned long val);
#pragma aux _gpu_outpd = "out dx, eax" parm [dx] [eax] modify exact []

unsigned long _gpu_inpd(unsigned short port);
#pragma aux _gpu_inpd = "in eax, dx" parm [dx] value [eax] modify exact []

static unsigned long gpu_reg_read(unsigned long reg)
{
    _gpu_outpd(g_gpu_iobase, reg);
    return _gpu_inpd(g_gpu_iobase + 4);
}

static void gpu_reg_write(unsigned long reg, unsigned long val)
{
    _gpu_outpd(g_gpu_iobase, reg);
    _gpu_outpd(g_gpu_iobase + 4, val);
}

static int detect_gpu_iobase(unsigned long lfb_phys)
{
    unsigned int bus, dev;
    unsigned long addr, bar;
    int i, found_lfb;
    unsigned short io_base;
    for (bus = 0; bus < 16; bus++) {
        for (dev = 0; dev < 32; dev++) {
            addr = 0x80000000UL | ((unsigned long)bus << 16)
                 | ((unsigned long)dev << 11);
            _gpu_outpd(0x0CF8, addr);
            if ((_gpu_inpd(0x0CFC) & 0xFFFF) == 0xFFFF) continue;
            found_lfb = 0;
            io_base   = 0;
            for (i = 0; i < 6; i++) {
                _gpu_outpd(0x0CF8, addr | (unsigned long)(0x10 + i * 4));
                bar = _gpu_inpd(0x0CFC);
                if (bar & 1) {
                    if (!io_base)
                        io_base = (unsigned short)(bar & ~0xFFUL);
                } else if ((bar & 0xFFF00000UL) == (lfb_phys & 0xFFF00000UL)
                        && (bar & 0xFFF00000UL) != 0) {
                    found_lfb = 1;
                }
            }
            if (found_lfb && io_base) {
                g_gpu_iobase = io_base;
                return 1;
            }
        }
    }
    return 0;
}

static void hw_page_flip(unsigned long page_phys_addr)
{
    unsigned long lock;
    lock = gpu_reg_read(R5_D1GRPH_UPDATE);
    gpu_reg_write(R5_D1GRPH_UPDATE, lock | R5_D1GRPH_SURFACE_UPDATE_LOCK);
    gpu_reg_write(R5_D1GRPH_PRIMARY_SURFACE_ADDRESS, page_phys_addr);
    gpu_reg_write(R5_D1GRPH_SECONDARY_SURFACE_ADDRESS, page_phys_addr);
    lock = gpu_reg_read(R5_D1GRPH_UPDATE);
    gpu_reg_write(R5_D1GRPH_UPDATE, lock & ~R5_D1GRPH_SURFACE_UPDATE_LOCK);
}

static int hw_is_flip_done(void)
{
    return !(gpu_reg_read(R5_D1GRPH_UPDATE) & R5_D1GRPH_SURFACE_UPDATE_PENDING);
}

/* ========================================================================
 *  VSYNC WAIT
 * ======================================================================== */

#define VGA_STAT1 0x03DA

static void wait_vsync(void)
{
    while (  inp(VGA_STAT1) & 0x08) {}
    while (!(inp(VGA_STAT1) & 0x08)) {}
}

/* ========================================================================
 *  LFB BLIT HELPER
 * ======================================================================== */

static void blit_to_lfb(unsigned char *dst, int lfb_pitch,
                        const unsigned char *src)
{
    if (lfb_pitch == WIDTH) {
        memcpy(dst, src, (unsigned long)WIDTH * HEIGHT);
    } else {
        int y;
        for (y = 0; y < HEIGHT; y++)
            memcpy(dst + y * lfb_pitch, src + y * WIDTH, WIDTH);
    }
}

/* ========================================================================
 *  INLINE ASM FOR MSR / CR / RDTSC
 * ======================================================================== */

unsigned short _get_cs(void);
#pragma aux _get_cs = "mov ax, cs" value [ax]

int _has_cpuid(void);
#pragma aux _has_cpuid = \
    "pushfd"           \
    "pop eax"          \
    "mov ecx, eax"     \
    "xor eax, 200000h" \
    "push eax"         \
    "popfd"            \
    "pushfd"           \
    "pop eax"          \
    "xor eax, ecx"     \
    "shr eax, 21"      \
    "and eax, 1"       \
    value [eax]        \
    modify [ecx]

unsigned long _cpuid1_edx(void);
#pragma aux _cpuid1_edx = \
    "mov eax, 1"       \
    "db 0Fh, 0A2h"     \
    value [edx]         \
    modify [eax ebx ecx]

unsigned long _rdmsr_lo(unsigned long);
#pragma aux _rdmsr_lo = \
    "db 0Fh, 32h"      \
    parm [ecx]          \
    value [eax]         \
    modify [edx]

unsigned long _rdmsr_hi(unsigned long);
#pragma aux _rdmsr_hi = \
    "db 0Fh, 32h"      \
    parm [ecx]          \
    value [edx]         \
    modify [eax]

void _wrmsr_3(unsigned long, unsigned long, unsigned long);
#pragma aux _wrmsr_3 = \
    "db 0Fh, 30h"      \
    parm [ecx] [eax] [edx]

void _wbinvd(void);
#pragma aux _wbinvd = "db 0Fh, 09h"

void _flush_tlb(void);
#pragma aux _flush_tlb = \
    "db 0Fh, 20h, 0D8h"    \
    "db 0Fh, 22h, 0D8h"    \
    modify [eax]

void _rdtsc_pair(unsigned long *, unsigned long *);
#pragma aux _rdtsc_pair = \
    "db 0Fh, 31h"          \
    "mov [esi], eax"        \
    "mov [edi], edx"        \
    parm [esi] [edi]        \
    modify [eax edx]

/* ========================================================================
 *  RDTSC TIMING
 * ======================================================================== */

static double g_rdtsc_mhz = 0.0;

static void calibrate_rdtsc(void)
{
    volatile unsigned long *bios_tick =
        (volatile unsigned long *)0x0046CUL;
    unsigned long t0, t1, lo0, hi0, lo1, hi1;
    unsigned long long c0, c1;
    double ticks;

    t0 = *bios_tick;
    while (*bios_tick == t0) {}
    t0 = *bios_tick;
    _rdtsc_pair(&lo0, &hi0);
    while (*bios_tick < t0 + 10) {}
    _rdtsc_pair(&lo1, &hi1);
    t1 = *bios_tick;

    c0 = ((unsigned long long)hi0 << 32) | lo0;
    c1 = ((unsigned long long)hi1 << 32) | lo1;
    ticks = (double)(t1 - t0);
    g_rdtsc_mhz = (double)(c1 - c0) / (ticks / 18.2065) / 1e6;
}

static double rdtsc_elapsed_ms(unsigned long lo0, unsigned long hi0,
                               unsigned long lo1, unsigned long hi1)
{
    unsigned long long c0 = ((unsigned long long)hi0 << 32) | lo0;
    unsigned long long c1 = ((unsigned long long)hi1 << 32) | lo1;
    return (double)(c1 - c0) / (g_rdtsc_mhz * 1000.0);
}

/* ========================================================================
 *  MTRR / PAT WRITE-COMBINING
 * ======================================================================== */

static int           g_mtrr_slot = -1;
static unsigned long g_mtrr_save_base_lo, g_mtrr_save_base_hi;
static unsigned long g_mtrr_save_mask_lo, g_mtrr_save_mask_hi;
static int           g_mtrr_wc = 0;
static unsigned long g_pat_save_lo = 0, g_pat_save_hi = 0;
static int           g_pat_modified = 0;

static unsigned long next_pow2(unsigned long v)
{
    v--;
    v |= v >> 1;  v |= v >> 2;
    v |= v >> 4;  v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

static int setup_mtrr_wc(unsigned long phys_addr, unsigned long vram_bytes)
{
    unsigned long cap_lo, def_lo;
    unsigned long sz;
    int num_var, i, free_slot;

    if ((_get_cs() & 3) != 0) return 0;
    if (!_has_cpuid()) return 0;
    if (!(_cpuid1_edx() & (1UL << 12))) return 0;

    cap_lo = _rdmsr_lo(MSR_MTRRCAP);
    if (!(cap_lo & (1UL << 10))) return 0;
    num_var = (int)(cap_lo & 0xFF);

    sz = next_pow2(vram_bytes);
    free_slot = -1;

    for (i = 0; i < num_var && i < 8; i++) {
        unsigned long blo = _rdmsr_lo(MSR_MTRR_PHYSBASE0 + i * 2);
        unsigned long mlo = _rdmsr_lo(MSR_MTRR_PHYSMASK0 + i * 2);
        if (!(mlo & 0x800)) { if (free_slot < 0) free_slot = i; continue; }
        if ((blo & 0xFFFFF000UL) == (phys_addr & 0xFFFFF000UL)) {
            if ((blo & 0xFF) == 1) { g_mtrr_wc = 2; return -1; }
        }
    }

    if (free_slot < 0) return 0;

    g_mtrr_slot = free_slot;
    g_mtrr_save_base_lo = _rdmsr_lo(MSR_MTRR_PHYSBASE0 + free_slot * 2);
    g_mtrr_save_base_hi = _rdmsr_hi(MSR_MTRR_PHYSBASE0 + free_slot * 2);
    g_mtrr_save_mask_lo = _rdmsr_lo(MSR_MTRR_PHYSMASK0 + free_slot * 2);
    g_mtrr_save_mask_hi = _rdmsr_hi(MSR_MTRR_PHYSMASK0 + free_slot * 2);

    def_lo = _rdmsr_lo(MSR_MTRR_DEF_TYPE);

    _disable();
    _wbinvd();
    _wrmsr_3(MSR_MTRR_DEF_TYPE, def_lo & ~(1UL << 11), 0);
    _wrmsr_3(MSR_MTRR_PHYSBASE0 + free_slot * 2,
             (phys_addr & 0xFFFFF000UL) | 1, 0);
    _wrmsr_3(MSR_MTRR_PHYSMASK0 + free_slot * 2,
             (~(sz - 1) & 0xFFFFF000UL) | 0x800, 0);
    _wrmsr_3(MSR_MTRR_DEF_TYPE, def_lo, 0);
    _wbinvd();
    _enable();

    g_mtrr_wc = 1;
    return 1;
}

static void restore_mtrr(void)
{
    unsigned long def_lo;
    if (g_mtrr_slot < 0) return;
    if ((_get_cs() & 3) != 0) return;

    def_lo = _rdmsr_lo(MSR_MTRR_DEF_TYPE);
    _disable();
    _wbinvd();
    _wrmsr_3(MSR_MTRR_DEF_TYPE, def_lo & ~(1UL << 11), 0);
    _wrmsr_3(MSR_MTRR_PHYSBASE0 + g_mtrr_slot * 2,
             g_mtrr_save_base_lo, g_mtrr_save_base_hi);
    _wrmsr_3(MSR_MTRR_PHYSMASK0 + g_mtrr_slot * 2,
             g_mtrr_save_mask_lo, g_mtrr_save_mask_hi);
    _wrmsr_3(MSR_MTRR_DEF_TYPE, def_lo, 0);
    _wbinvd();
    _enable();
    g_mtrr_slot = -1;
    g_mtrr_wc = 0;
}

static int setup_pat_uc_minus(void)
{
    unsigned long pat_lo, pat_hi, entry3;
    if ((_get_cs() & 3) != 0) return 0;
    if (!(_cpuid1_edx() & (1UL << 16))) return 0;

    pat_lo = _rdmsr_lo(MSR_PAT);
    pat_hi = _rdmsr_hi(MSR_PAT);
    g_pat_save_lo = pat_lo;
    g_pat_save_hi = pat_hi;

    entry3 = (pat_lo >> 24) & 7;
    if (entry3 == 7 || entry3 == 1) return -1;

    pat_lo = (pat_lo & 0x00FFFFFFUL) | (7UL << 24);
    _disable();
    _wbinvd();
    _wrmsr_3(MSR_PAT, pat_lo, pat_hi);
    _wbinvd();
    _flush_tlb();
    _enable();
    g_pat_modified = 1;
    return 1;
}

static void restore_pat(void)
{
    if (!g_pat_modified) return;
    if ((_get_cs() & 3) != 0) return;
    _disable();
    _wbinvd();
    _wrmsr_3(MSR_PAT, g_pat_save_lo, g_pat_save_hi);
    _wbinvd();
    _flush_tlb();
    _enable();
    g_pat_modified = 0;
}

/* ========================================================================
 *  KEYBOARD INPUT (IRQ 1 handler)
 * ======================================================================== */

static void (__interrupt __far *g_old_kb_isr)();
static volatile unsigned char g_keys[128];

static void __interrupt __far kb_isr(void)
{
    unsigned char sc = inp(0x60);
    unsigned char ack = inp(0x61);
    outp(0x61, ack | 0x80);
    outp(0x61, ack);
    if (sc & 0x80)
        g_keys[sc & 0x7F] = 0;
    else
        g_keys[sc & 0x7F] = 1;
    outp(0x20, 0x20);
}

static void install_keyboard(void)
{
    memset((void *)g_keys, 0, sizeof(g_keys));
    g_old_kb_isr = _dos_getvect(9);
    _dos_setvect(9, kb_isr);
}

static void restore_keyboard(void)
{
    if (g_old_kb_isr)
        _dos_setvect(9, g_old_kb_isr);
}

/* ========================================================================
 *  MOUSE INPUT (INT 33h via DPMI)
 * ======================================================================== */

static int g_mouse_ok = 0;

static int mouse_init(void)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0;
    dpmi_real_int(0x33, &rmi);
    return (rmi.eax & 0xFFFF) == 0xFFFF;
}

static void mouse_get_mickeys(int *dx, int *dy)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x000B;
    dpmi_real_int(0x33, &rmi);
    *dx = (short)(rmi.ecx & 0xFFFF);
    *dy = (short)(rmi.edx & 0xFFFF);
}

static int mouse_get_buttons(void)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 3;
    dpmi_real_int(0x33, &rmi);
    return (int)(rmi.ebx & 0x7);
}

/* ========================================================================
 *  PALETTE SETUP
 * ======================================================================== */

static void build_ramp(unsigned char *out, int r0, int g0, int b0,
                       int r1, int g1, int b1)
{
    int i;
    for (i = 0; i < 16; i++) {
        out[i * 3 + 0] = (unsigned char)(r0 + (r1 - r0) * i / 15);
        out[i * 3 + 1] = (unsigned char)(g0 + (g1 - g0) * i / 15);
        out[i * 3 + 2] = (unsigned char)(b0 + (b1 - b0) * i / 15);
    }
}

static void setup_palette(void)
{
    unsigned char pal[256 * 3];
    int scale = (g_dac_bits == 8) ? 1 : 4;  /* 6-bit DAC: values 0-63 */
    int m = (g_dac_bits == 8) ? 255 : 63;
    int i;

    memset(pal, 0, sizeof(pal));

    /* HUE 0: grayscale */
    build_ramp(&pal[HUE_GRAY * 16 * 3], 0, 0, 0, m, m, m);
    /* HUE 1: brick (dark red-brown to red) */
    build_ramp(&pal[HUE_BRICK * 16 * 3], m/16, 0, 0, m*3/4, m/4, m/8);
    /* HUE 2: stone (dark gray to light gray) */
    build_ramp(&pal[HUE_STONE * 16 * 3], m/16, m/16, m/16, m*3/4, m*3/4, m*11/16);
    /* HUE 3: wood (dark brown to light brown) */
    build_ramp(&pal[HUE_WOOD * 16 * 3], m/16, m/32, 0, m*5/8, m*3/8, m/8);
    /* HUE 4: moss (dark green to green) */
    build_ramp(&pal[HUE_MOSS * 16 * 3], 0, m/16, 0, m/4, m*5/8, m/8);
    /* HUE 5: metal (dark blue-gray to light steel) */
    build_ramp(&pal[HUE_METAL * 16 * 3], m/16, m/16, m/8, m/2, m*9/16, m*3/4);
    /* HUE 6: sky (dark blue to medium blue) */
    build_ramp(&pal[HUE_SKY * 16 * 3], 0, 0, m/8, m/6, m/4, m*5/8);
    /* HUE 7: floor (very dark brown to medium brown) */
    build_ramp(&pal[HUE_FLOOR * 16 * 3], m/20, m/32, m/40, m*3/8, m/4, m/8);
    /* HUE 8: duck yellow */
    build_ramp(&pal[HUE_DUCK * 16 * 3], m/8, m/8, 0, m, m*7/8, m/8);
    /* HUE 9: orange (bill, feet) */
    build_ramp(&pal[HUE_ORANGE * 16 * 3], m/8, m/16, 0, m, m/2, 0);
    /* HUE 10: blood/damage red */
    build_ramp(&pal[HUE_BLOOD * 16 * 3], m/8, 0, 0, m, 0, 0);
    /* HUE 11: tan */
    build_ramp(&pal[HUE_TAN * 16 * 3], m/16, m/16, m/32, m*5/8, m/2, m*3/8);
    /* HUE 12: teal */
    build_ramp(&pal[HUE_TEAL * 16 * 3], 0, m/16, m/16, m/4, m*5/8, m*5/8);
    /* HUE 13: purple */
    build_ramp(&pal[HUE_PURPLE * 16 * 3], m/16, 0, m/16, m/2, m/8, m*5/8);
    /* HUE 14: bright yellow/white */
    build_ramp(&pal[HUE_YELLOW * 16 * 3], m/8, m/8, 0, m, m, m*3/4);
    /* HUE 15: bright red/white for HUD */
    build_ramp(&pal[HUE_RED * 16 * 3], m/8, 0, 0, m, m/4, m/4);

    set_palette_block(0, 256, pal);
}

/* ========================================================================
 *  TEXTURE GENERATION
 * ======================================================================== */

static unsigned char g_tex[NUM_TEX][TEX_H][TEX_W];

static int hash2d(int x, int y)
{
    int h = x * 374761 + y * 668265;
    h = (h ^ (h >> 13)) * 127413;
    return h & 0xFF;
}

static void gen_brick_tex(unsigned char tex[TEX_H][TEX_W])
{
    int x, y;
    for (y = 0; y < TEX_H; y++) {
        for (x = 0; x < TEX_W; x++) {
            int row = y / 8;
            int bx = x + (row & 1) * 8;
            int noise = (hash2d(x, y) & 3) - 1;
            if ((y & 7) == 0 || (bx & 15) == 0)
                tex[y][x] = PAL(HUE_STONE, 5 + (noise & 1));
            else
                tex[y][x] = PAL(HUE_BRICK, 8 + noise);
        }
    }
}

static void gen_stone_tex(unsigned char tex[TEX_H][TEX_W])
{
    int x, y;
    for (y = 0; y < TEX_H; y++) {
        for (x = 0; x < TEX_W; x++) {
            int n = hash2d(x / 4, y / 4) & 15;
            int mortar = 0;
            if ((y % 16) < 1 || (x % 12) < 1) mortar = 1;
            if (mortar)
                tex[y][x] = PAL(HUE_STONE, 3 + (n & 1));
            else
                tex[y][x] = PAL(HUE_STONE, 7 + (n >> 2));
        }
    }
}

static void gen_wood_tex(unsigned char tex[TEX_H][TEX_W])
{
    int x, y;
    for (y = 0; y < TEX_H; y++) {
        for (x = 0; x < TEX_W; x++) {
            float grain = (float)sin(x * 0.3 + sin(y * 0.07) * 3.0) * 0.5f + 0.5f;
            int shade = 6 + (int)(grain * 7.0f);
            int noise = hash2d(x, y) & 1;
            tex[y][x] = PAL(HUE_WOOD, shade + noise);
        }
    }
}

static void gen_metal_tex(unsigned char tex[TEX_H][TEX_W])
{
    int x, y;
    for (y = 0; y < TEX_H; y++) {
        for (x = 0; x < TEX_W; x++) {
            int n = hash2d(x, y) & 3;
            int shade = 9 + n;
            /* Horizontal banding */
            shade += (y & 3) == 0 ? 1 : 0;
            /* Rivets at regular intervals */
            if (((x - 8) * (x - 8) + (y - 8) * (y - 8)) < 9 ||
                ((x - 40) * (x - 40) + (y - 40) * (y - 40)) < 9 ||
                ((x - 8) * (x - 8) + (y - 40) * (y - 40)) < 9 ||
                ((x - 40) * (x - 40) + (y - 8) * (y - 8)) < 9)
                shade = 14;
            if (shade > 15) shade = 15;
            tex[y][x] = PAL(HUE_METAL, shade);
        }
    }
}

static void generate_textures(void)
{
    gen_brick_tex(g_tex[0]);
    gen_stone_tex(g_tex[1]);
    gen_wood_tex(g_tex[2]);
    gen_metal_tex(g_tex[3]);
}

/* ========================================================================
 *  DUCK SPRITES (procedurally generated)
 * ======================================================================== */

static unsigned char g_duck_spr[2][SPRITE_H][SPRITE_W];  /* 2 animation frames */

static void generate_duck_sprites(void)
{
    int frame, x, y;
    for (frame = 0; frame < 2; frame++) {
        memset(g_duck_spr[frame], 0, SPRITE_H * SPRITE_W);
        for (y = 0; y < SPRITE_H; y++) {
            for (x = 0; x < SPRITE_W; x++) {
                float bx = (x - 16.0f) / 9.0f;
                float by = (y - 18.0f) / 6.0f;
                float hx = (x - 16.0f) / 5.0f;
                float hy = (y - 9.0f) / 4.5f;
                /* Body (oval) */
                if (bx * bx + by * by < 1.0f) {
                    int sh = 10 + (int)(2.0f * (1.0f - by));
                    if (sh > 14) sh = 14;
                    g_duck_spr[frame][y][x] = PAL(HUE_DUCK, sh);
                }
                /* Breast (lighter belly) */
                if (bx * bx * 1.5f + (by - 0.3f) * (by - 0.3f) * 3.0f < 0.6f)
                    g_duck_spr[frame][y][x] = PAL(HUE_YELLOW, 13);
                /* Head (circle) */
                if (hx * hx + hy * hy < 1.0f) {
                    g_duck_spr[frame][y][x] = PAL(HUE_MOSS, 10);
                }
                /* Eye */
                if ((x == 18 || x == 19) && (y == 8 || y == 9))
                    g_duck_spr[frame][y][x] = PAL(HUE_GRAY, 15);
                if (x == 19 && y == 9)
                    g_duck_spr[frame][y][x] = PAL(HUE_GRAY, 0);
                /* Bill */
                if (x >= 20 && x <= 25 && y >= 10 && y <= 12)
                    g_duck_spr[frame][y][x] = PAL(HUE_ORANGE, 12);
            }
        }
        /* Wings */
        for (y = 12; y <= 22; y++) {
            int span;
            if (frame == 0)
                span = (22 - y) * 3 / 4;   /* wings up */
            else
                span = (y - 12) * 3 / 4;   /* wings down */
            for (x = 0; x < span; x++) {
                if (3 - x >= 0)
                    g_duck_spr[frame][y][3 - x] = PAL(HUE_WOOD, 9);
                if (28 + x < SPRITE_W)
                    g_duck_spr[frame][y][28 + x] = PAL(HUE_WOOD, 9);
            }
        }
        /* Feet */
        if (1) {
            int fx;
            for (fx = 13; fx <= 15; fx++)
                g_duck_spr[frame][26][fx] = PAL(HUE_ORANGE, 10);
            for (fx = 17; fx <= 19; fx++)
                g_duck_spr[frame][26][fx] = PAL(HUE_ORANGE, 10);
        }
    }
}

/* ========================================================================
 *  FONT (from BIOS ROM)
 * ======================================================================== */

static unsigned char *g_font = NULL;

static void init_font(void)
{
    RMI rmi;
    unsigned long addr;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x1130;
    rmi.ebx = 0x0300;
    dpmi_real_int(0x10, &rmi);
    addr = ((unsigned long)(rmi.es & 0xFFFF) << 4) + (rmi.ebp & 0xFFFF);
    g_font = (unsigned char *)addr;
}

static void draw_char(unsigned char *buf, int pitch,
                      int x0, int y0, char ch, unsigned char color)
{
    unsigned char *glyph;
    int row, bit;
    if (!g_font) return;
    glyph = g_font + (unsigned char)ch * 8;
    for (row = 0; row < 8; row++) {
        unsigned char bits = glyph[row];
        for (bit = 7; bit >= 0; bit--) {
            if (bits & (1 << bit))
                buf[(y0 + row) * pitch + x0 + (7 - bit)] = color;
        }
    }
}

static void draw_str(unsigned char *buf, int pitch,
                     int x, int y, const char *s, unsigned char color)
{
    while (*s) {
        draw_char(buf, pitch, x, y, *s, color);
        x += 8;
        s++;
    }
}

static void draw_str_bg(unsigned char *buf, int pitch,
                        int x, int y, const char *s,
                        unsigned char fg, unsigned char bg)
{
    while (*s) {
        unsigned char *glyph;
        int row, bit;
        if (!g_font) return;
        glyph = g_font + (unsigned char)*s * 8;
        for (row = 0; row < 8; row++) {
            unsigned char bits = glyph[row];
            for (bit = 7; bit >= 0; bit--) {
                buf[(y + row) * pitch + x + (7 - bit)] =
                    (bits & (1 << bit)) ? fg : bg;
            }
        }
        x += 8;
        s++;
    }
}

/* ========================================================================
 *  MAP DATA
 * ======================================================================== */

/*  0 = empty, 1 = brick, 2 = stone, 3 = wood, 4 = metal */
static unsigned char g_map[MAP_H][MAP_W] = {
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,2,2,2,2,0,0,0,3,3,3,0,0,0,4,4,4,4,0,0,0,1},
    {1,0,0,2,0,0,0,0,0,0,0,0,3,0,0,0,4,0,0,0,0,0,0,1},
    {1,0,0,2,0,0,0,0,0,0,0,0,3,0,0,0,4,0,0,0,0,0,0,1},
    {1,0,0,2,0,0,3,3,3,0,0,0,3,0,0,0,0,0,0,4,4,0,0,1},
    {1,0,0,0,0,0,3,0,3,0,0,0,0,0,0,0,0,0,0,4,0,0,0,1},
    {1,0,0,0,0,0,3,0,3,0,0,0,0,0,0,0,0,0,0,4,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,1,1,1,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,1,0,1,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,2,2,0,0,0,0,0,0,1,0,1,0,0,0,3,3,3,0,0,0,1},
    {1,0,0,2,0,0,0,0,0,0,0,0,0,1,0,0,0,3,0,0,0,0,0,1},
    {1,0,0,2,0,0,0,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,4,4,4,4,0,0,0,0,0,0,2,2,2,2,0,0,0,0,1},
    {1,0,0,0,0,4,0,0,0,0,0,0,0,0,0,0,0,0,2,0,0,0,0,1},
    {1,0,0,0,0,4,0,0,0,0,0,3,3,0,0,0,0,0,2,0,0,0,0,1},
    {1,0,0,0,0,4,0,0,0,0,0,3,0,0,0,0,0,0,2,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,3,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,0,1},
    {1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1}
};

/* ========================================================================
 *  DUCK STATE
 * ======================================================================== */

typedef struct {
    float x, y;         /* map position */
    float vx, vy;       /* velocity */
    float z;            /* height in wall units (0=ground, 1=ceiling) */
    float bob_phase;
    float speed;
    int   alive;
    int   anim_frame;
    int   flash_timer;  /* >0 = flash white on hit */
} Duck;

static Duck  g_ducks[MAX_DUCKS];
static int   g_num_ducks = 0;
static int   g_ducks_alive = 0;
static int   g_score = 0;
static int   g_round = 1;
static int   g_ammo = 6;
static int   g_shots_fired = 0;
static float g_shot_cooldown = 0.0f;

static void spawn_duck(float x, float y, float spd)
{
    Duck *d;
    float angle;
    if (g_num_ducks >= MAX_DUCKS) return;
    d = &g_ducks[g_num_ducks];
    d->x = x;
    d->y = y;
    angle = (float)(rand() % 628) / 100.0f;
    d->vx = (float)cos(angle) * spd;
    d->vy = (float)sin(angle) * spd;
    d->z = 0.3f + (float)(rand() % 40) / 100.0f;
    d->bob_phase = (float)(rand() % 628) / 100.0f;
    d->speed = spd;
    d->alive = 1;
    d->anim_frame = 0;
    d->flash_timer = 0;
    g_num_ducks++;
    g_ducks_alive++;
}

static void spawn_round_ducks(int round)
{
    int count = 5 + round * 2;
    int i;
    float spd = 1.0f + round * 0.3f;
    if (count > MAX_DUCKS) count = MAX_DUCKS;
    g_num_ducks = 0;
    g_ducks_alive = 0;
    for (i = 0; i < count; i++) {
        float sx, sy;
        int tries = 0;
        do {
            sx = 2.0f + (float)(rand() % (MAP_W - 4));
            sy = 2.0f + (float)(rand() % (MAP_H - 4));
            tries++;
        } while (g_map[(int)sy][(int)sx] != 0 && tries < 100);
        if (tries < 100)
            spawn_duck(sx, sy, spd + (float)(rand() % 100) / 100.0f);
    }
}

static void update_ducks(float dt)
{
    int i;
    for (i = 0; i < g_num_ducks; i++) {
        Duck *d = &g_ducks[i];
        float nx, ny, spd, len;
        if (!d->alive) {
            if (d->flash_timer > 0) d->flash_timer--;
            continue;
        }

        nx = d->x + d->vx * dt;
        ny = d->y + d->vy * dt;

        /* Wall bounce */
        if (g_map[(int)d->y][(int)nx] != 0) {
            d->vx = -d->vx;
            d->vx += ((rand() % 100) - 50) * 0.005f;
            nx = d->x;
        }
        if (g_map[(int)ny][(int)d->x] != 0) {
            d->vy = -d->vy;
            d->vy += ((rand() % 100) - 50) * 0.005f;
            ny = d->y;
        }

        /* Normalize speed */
        len = (float)sqrt(d->vx * d->vx + d->vy * d->vy);
        if (len > 0.01f) {
            d->vx = d->vx / len * d->speed;
            d->vy = d->vy / len * d->speed;
        }

        d->x = nx;
        d->y = ny;

        /* Bobbing motion */
        d->bob_phase += dt * 3.0f;
        d->z = 0.4f + (float)sin(d->bob_phase) * 0.15f;

        /* Animation frame toggle */
        d->anim_frame = ((int)(d->bob_phase * 2.0f)) & 1;
    }
}

/* ========================================================================
 *  PLAYER STATE
 * ======================================================================== */

static float g_posX = 2.5f, g_posY = 2.5f;
static float g_dirX = 1.0f, g_dirY = 0.0f;
static float g_planeX = 0.0f, g_planeY = FOV_PLANE;
static int   g_pitch = 0;

static int can_move_to(float x, float y)
{
    float r = PLAYER_RAD;
    if (g_map[(int)(y - r)][(int)(x - r)] != 0) return 0;
    if (g_map[(int)(y - r)][(int)(x + r)] != 0) return 0;
    if (g_map[(int)(y + r)][(int)(x - r)] != 0) return 0;
    if (g_map[(int)(y + r)][(int)(x + r)] != 0) return 0;
    return 1;
}

static void update_player(float dt)
{
    float moveF = 0.0f, moveS = 0.0f;
    float cs, sn, nx, ny;
    int mdx, mdy;

    /* Keyboard movement */
    if (g_keys[SC_W] || g_keys[SC_UP])    moveF += MOVE_SPEED * dt;
    if (g_keys[SC_S] || g_keys[SC_DOWN])  moveF -= MOVE_SPEED * dt;
    if (g_keys[SC_A])                      moveS -= STRAFE_SPEED * dt;
    if (g_keys[SC_D])                      moveS += STRAFE_SPEED * dt;

    /* Forward/backward */
    nx = g_posX + g_dirX * moveF;
    ny = g_posY + g_dirY * moveF;
    if (can_move_to(nx, g_posY)) g_posX = nx;
    if (can_move_to(g_posX, ny)) g_posY = ny;

    /* Strafing (perpendicular to direction) */
    nx = g_posX + g_planeX * moveS;
    ny = g_posY + g_planeY * moveS;
    if (can_move_to(nx, g_posY)) g_posX = nx;
    if (can_move_to(g_posX, ny)) g_posY = ny;

    /* Mouse look */
    if (g_mouse_ok) {
        mouse_get_mickeys(&mdx, &mdy);
        if (mdx != 0) {
            float rot = -(float)mdx * MOUSE_SENS;
            float odx = g_dirX, ody = g_dirY;
            float opx = g_planeX, opy = g_planeY;
            cs = (float)cos(rot);
            sn = (float)sin(rot);
            g_dirX   = odx * cs - ody * sn;
            g_dirY   = odx * sn + ody * cs;
            g_planeX = opx * cs - opy * sn;
            g_planeY = opx * sn + opy * cs;
        }
        g_pitch -= (int)((float)mdy * PITCH_SENS);
        if (g_pitch > MAX_PITCH) g_pitch = MAX_PITCH;
        if (g_pitch < -MAX_PITCH) g_pitch = -MAX_PITCH;
    }

    /* Keyboard turning (if no mouse) */
    if (g_keys[SC_LEFT]) {
        float rot = 2.5f * dt;
        float odx = g_dirX, ody = g_dirY;
        float opx = g_planeX, opy = g_planeY;
        cs = (float)cos(rot);
        sn = (float)sin(rot);
        g_dirX   = odx * cs - ody * sn;
        g_dirY   = odx * sn + ody * cs;
        g_planeX = opx * cs - opy * sn;
        g_planeY = opx * sn + opy * cs;
    }
    if (g_keys[SC_RIGHT]) {
        float rot = -2.5f * dt;
        float odx = g_dirX, ody = g_dirY;
        float opx = g_planeX, opy = g_planeY;
        cs = (float)cos(rot);
        sn = (float)sin(rot);
        g_dirX   = odx * cs - ody * sn;
        g_dirY   = odx * sn + ody * cs;
        g_planeX = opx * cs - opy * sn;
        g_planeY = opx * sn + opy * cs;
    }
}

/* ========================================================================
 *  RAYCASTING RENDERER
 * ======================================================================== */

static float g_zbuf[WIDTH];   /* wall distance per column (for sprites) */

static void render_walls(unsigned char *buf)
{
    int x;
    for (x = 0; x < WIDTH; x++) {
        float cameraX = 2.0f * x / (float)WIDTH - 1.0f;
        float rayDirX = g_dirX + g_planeX * cameraX;
        float rayDirY = g_dirY + g_planeY * cameraX;

        int mapX = (int)g_posX;
        int mapY = (int)g_posY;

        float deltaDistX = (rayDirX == 0.0f) ? 1e30f : (float)fabs(1.0f / rayDirX);
        float deltaDistY = (rayDirY == 0.0f) ? 1e30f : (float)fabs(1.0f / rayDirY);
        float sideDistX, sideDistY, perpWallDist;

        int stepX, stepY, side, hit, wallType;
        int lineHeight, drawStart, drawEnd;
        float wallX;
        int texX, y;
        unsigned char *texData;
        int fog, sideFog;

        if (rayDirX < 0) {
            stepX = -1;
            sideDistX = (g_posX - mapX) * deltaDistX;
        } else {
            stepX = 1;
            sideDistX = (mapX + 1.0f - g_posX) * deltaDistX;
        }
        if (rayDirY < 0) {
            stepY = -1;
            sideDistY = (g_posY - mapY) * deltaDistY;
        } else {
            stepY = 1;
            sideDistY = (mapY + 1.0f - g_posY) * deltaDistY;
        }

        /* DDA */
        hit = 0;
        side = 0;
        while (!hit) {
            if (sideDistX < sideDistY) {
                sideDistX += deltaDistX;
                mapX += stepX;
                side = 0;
            } else {
                sideDistY += deltaDistY;
                mapY += stepY;
                side = 1;
            }
            if (mapX < 0 || mapX >= MAP_W || mapY < 0 || mapY >= MAP_H) {
                hit = 1; wallType = 1; break;
            }
            if (g_map[mapY][mapX] > 0) {
                hit = 1;
                wallType = g_map[mapY][mapX];
            }
        }

        if (side == 0)
            perpWallDist = sideDistX - deltaDistX;
        else
            perpWallDist = sideDistY - deltaDistY;

        if (perpWallDist < 0.01f) perpWallDist = 0.01f;
        g_zbuf[x] = perpWallDist;

        lineHeight = (int)((float)VIEW_H / perpWallDist);
        drawStart = -lineHeight / 2 + VIEW_H / 2 + g_pitch;
        drawEnd   =  lineHeight / 2 + VIEW_H / 2 + g_pitch;

        /* Texture X coordinate */
        if (side == 0)
            wallX = g_posY + perpWallDist * rayDirY;
        else
            wallX = g_posX + perpWallDist * rayDirX;
        wallX -= (float)floor(wallX);
        texX = (int)(wallX * TEX_W);
        if (texX < 0) texX = 0;
        if (texX >= TEX_W) texX = TEX_W - 1;

        texData = &g_tex[(wallType - 1) % NUM_TEX][0][0];
        fog = (int)(perpWallDist * 0.8f);
        if (fog > 12) fog = 12;
        sideFog = side ? 2 : 0;

        /* Draw ceiling */
        {
            int ylim = drawStart;
            if (ylim > VIEW_H) ylim = VIEW_H;
            for (y = 0; y < ylim; y++) {
                int dist = abs(VIEW_H / 2 + g_pitch - y);
                int sh = dist * 12 / (VIEW_H / 2);
                if (sh > 12) sh = 12;
                buf[y * WIDTH + x] = PAL(HUE_SKY, sh);
            }
        }

        /* Draw wall strip */
        {
            int ys = drawStart < 0 ? 0 : drawStart;
            int ye = drawEnd >= VIEW_H ? VIEW_H - 1 : drawEnd;
            for (y = ys; y <= ye; y++) {
                int texY = ((y - drawStart) * TEX_H) / lineHeight;
                int hue, shade;
                unsigned char c;
                if (texY < 0) texY = 0;
                if (texY >= TEX_H) texY = TEX_H - 1;
                c = texData[texY * TEX_W + texX];
                hue = c >> 4;
                shade = (c & 15) - fog - sideFog;
                if (shade < 0) shade = 0;
                buf[y * WIDTH + x] = (unsigned char)(hue * 16 + shade);
            }
        }

        /* Draw floor */
        {
            int ys = drawEnd + 1;
            if (ys < 0) ys = 0;
            for (y = ys; y < VIEW_H; y++) {
                int dist = abs(y - VIEW_H / 2 - g_pitch);
                int sh = dist * 10 / (VIEW_H / 2);
                if (sh > 10) sh = 10;
                buf[y * WIDTH + x] = PAL(HUE_FLOOR, sh);
            }
        }
    }
}

/* ========================================================================
 *  SPRITE RENDERING (ducks as billboards)
 * ======================================================================== */

static int g_sprite_order[MAX_DUCKS];
static float g_sprite_dist[MAX_DUCKS];

static void sort_sprites(void)
{
    int i, j;
    for (i = 0; i < g_num_ducks; i++) {
        float dx = g_ducks[i].x - g_posX;
        float dy = g_ducks[i].y - g_posY;
        g_sprite_order[i] = i;
        g_sprite_dist[i] = dx * dx + dy * dy;
    }
    /* Simple insertion sort (few sprites) */
    for (i = 1; i < g_num_ducks; i++) {
        int key_idx = g_sprite_order[i];
        float key_dist = g_sprite_dist[i];
        j = i - 1;
        while (j >= 0 && g_sprite_dist[j] < key_dist) {
            g_sprite_order[j + 1] = g_sprite_order[j];
            g_sprite_dist[j + 1] = g_sprite_dist[j];
            j--;
        }
        g_sprite_order[j + 1] = key_idx;
        g_sprite_dist[j + 1] = key_dist;
    }
}

static void render_sprites(unsigned char *buf)
{
    float invDet;
    int i;

    invDet = 1.0f / (g_planeX * g_dirY - g_dirX * g_planeY);
    sort_sprites();

    for (i = 0; i < g_num_ducks; i++) {
        Duck *d = &g_ducks[g_sprite_order[i]];
        float sprX, sprY, transformX, transformY;
        int scrX, sprHeight, sprWidth;
        int drawStartX, drawEndX, drawStartY, drawEndY;
        int vMoveScr;
        int stripe;

        if (!d->alive && d->flash_timer <= 0) continue;

        sprX = d->x - g_posX;
        sprY = d->y - g_posY;

        transformX = invDet * (g_dirY * sprX - g_dirX * sprY);
        transformY = invDet * (-g_planeY * sprX + g_planeX * sprY);

        if (transformY <= 0.1f) continue;

        scrX = (int)((WIDTH / 2) * (1.0f + transformX / transformY));

        sprHeight = (int)(DUCK_SIZE * VIEW_H / transformY);
        sprWidth  = sprHeight;

        /* Vertical shift: duck at height d->z (0=ground, 0.5=eye level) */
        vMoveScr = (int)((d->z - 0.5f) * VIEW_H / transformY) + g_pitch;

        drawStartY = -sprHeight / 2 + VIEW_H / 2 - vMoveScr;
        drawEndY   =  sprHeight / 2 + VIEW_H / 2 - vMoveScr;
        drawStartX = -sprWidth / 2 + scrX;
        drawEndX   =  sprWidth / 2 + scrX;

        for (stripe = drawStartX; stripe < drawEndX; stripe++) {
            int texX, y;
            if (stripe < 0 || stripe >= WIDTH) continue;
            if (transformY >= g_zbuf[stripe]) continue;

            texX = (stripe - drawStartX) * SPRITE_W / sprWidth;
            if (texX < 0 || texX >= SPRITE_W) continue;

            for (y = drawStartY; y < drawEndY; y++) {
                int texY;
                unsigned char c;
                if (y < 0 || y >= VIEW_H) continue;
                texY = (y - drawStartY) * SPRITE_H / sprHeight;
                if (texY < 0 || texY >= SPRITE_H) continue;

                if (d->flash_timer > 0) {
                    /* Hit flash: draw white */
                    c = g_duck_spr[0][texY][texX];
                    if (c != 0)
                        buf[y * WIDTH + stripe] = PAL(HUE_GRAY, 15);
                } else {
                    c = g_duck_spr[d->anim_frame][texY][texX];
                    if (c != 0) {
                        /* Apply distance fog */
                        int hue = c >> 4;
                        int sh  = (c & 15) - (int)(transformY * 0.6f);
                        if (sh < 1) sh = 1;
                        buf[y * WIDTH + stripe] = (unsigned char)(hue * 16 + sh);
                    }
                }
            }
        }
    }
}

/* ========================================================================
 *  SHOOTING
 * ======================================================================== */

static int g_hit_flash = 0;  /* screen flash timer for hit feedback */

static int try_shoot(void)
{
    int best = -1;
    float best_dist = 1e30f;
    float invDet;
    int i;

    invDet = 1.0f / (g_planeX * g_dirY - g_dirX * g_planeY);

    for (i = 0; i < g_num_ducks; i++) {
        Duck *d = &g_ducks[i];
        float sprX, sprY, tx, ty;
        int scrX, scrY, sprSz, halfSz;

        if (!d->alive) continue;

        sprX = d->x - g_posX;
        sprY = d->y - g_posY;
        tx = invDet * (g_dirY * sprX - g_dirX * sprY);
        ty = invDet * (-g_planeY * sprX + g_planeX * sprY);

        if (ty <= 0.1f) continue;
        if (ty >= g_zbuf[WIDTH / 2]) continue;

        scrX = (int)((WIDTH / 2) * (1.0f + tx / ty));
        sprSz = (int)(DUCK_SIZE * VIEW_H / ty);
        halfSz = sprSz / 2;

        {
            int vMoveScr = (int)((d->z - 0.5f) * VIEW_H / ty) + g_pitch;
            scrY = VIEW_H / 2 - vMoveScr;
        }

        if (abs(scrX - WIDTH / 2) < halfSz &&
            abs(scrY - HEIGHT / 2 + HUD_H / 2) < halfSz) {
            if (ty < best_dist) {
                best_dist = ty;
                best = i;
            }
        }
    }

    if (best >= 0) {
        g_ducks[best].alive = 0;
        g_ducks[best].flash_timer = 8;
        g_ducks_alive--;
        g_score += 100 * g_round;
        g_hit_flash = 4;
        return 1;
    }
    return 0;
}

/* ========================================================================
 *  HUD RENDERING
 * ======================================================================== */

static void draw_crosshair(unsigned char *buf)
{
    int cx = WIDTH / 2;
    int cy = VIEW_H / 2;
    int i;
    unsigned char col = PAL(HUE_RED, 14);

    /* Horizontal line */
    for (i = -8; i <= 8; i++) {
        if (i >= -2 && i <= 2) continue;
        if (cx + i >= 0 && cx + i < WIDTH)
            buf[cy * WIDTH + cx + i] = col;
    }
    /* Vertical line */
    for (i = -8; i <= 8; i++) {
        if (i >= -2 && i <= 2) continue;
        if (cy + i >= 0 && cy + i < VIEW_H)
            buf[(cy + i) * WIDTH + cx] = col;
    }
    /* Center dot */
    buf[cy * WIDTH + cx] = PAL(HUE_GRAY, 15);
}

static void render_hud(unsigned char *buf, float fps)
{
    int y0 = VIEW_H;
    int i;
    char msg[80];

    /* HUD background */
    for (i = 0; i < HUD_H * WIDTH; i++)
        buf[y0 * WIDTH + i] = PAL(HUE_STONE, 3);

    /* Separator line */
    for (i = 0; i < WIDTH; i++)
        buf[y0 * WIDTH + i] = PAL(HUE_STONE, 8);

    /* Score */
    sprintf(msg, "SCORE: %d", g_score);
    draw_str_bg(buf, WIDTH, 16, y0 + 8, msg,
                PAL(HUE_YELLOW, 14), PAL(HUE_STONE, 3));

    /* Ammo */
    sprintf(msg, "AMMO: ");
    draw_str_bg(buf, WIDTH, 16, y0 + 24, msg,
                PAL(HUE_GRAY, 12), PAL(HUE_STONE, 3));
    for (i = 0; i < 6; i++) {
        unsigned char col = (i < g_ammo) ? PAL(HUE_ORANGE, 13) : PAL(HUE_STONE, 5);
        int bx = 64 + i * 12;
        int by = y0 + 24;
        int j, k;
        for (j = 0; j < 8; j++)
            for (k = 0; k < 8; k++)
                buf[(by + j) * WIDTH + bx + k] = col;
    }

    /* Round and ducks remaining */
    sprintf(msg, "ROUND: %d    DUCKS: %d", g_round, g_ducks_alive);
    draw_str_bg(buf, WIDTH, 200, y0 + 8, msg,
                PAL(HUE_GRAY, 14), PAL(HUE_STONE, 3));

    /* FPS */
    sprintf(msg, "FPS: %.0f", fps);
    draw_str_bg(buf, WIDTH, WIDTH - 96, y0 + 8, msg,
                PAL(HUE_MOSS, 12), PAL(HUE_STONE, 3));

    /* WC / MTRR / PMI / HW status */
    sprintf(msg, "WC:%s PAT:%s HWF:%s PMI:%s",
            g_mtrr_wc ? "ON" : "OFF",
            g_pat_modified ? "FIX" : "---",
            g_hw_flip ? "ON" : "---",
            g_pmi_ok ? "ON" : "---");
    draw_str_bg(buf, WIDTH, WIDTH - 300, y0 + 24, msg,
                PAL(HUE_TEAL, 10), PAL(HUE_STONE, 3));

    /* Minimap */
    {
        int mx0 = WIDTH - 130;
        int my0 = y0 + 4;
        int scale = 2;
        int mx, my;
        for (my = 0; my < MAP_H && my * scale + my0 < HEIGHT - 2; my++) {
            for (mx = 0; mx < MAP_W && mx * scale + mx0 < WIDTH - 2; mx++) {
                unsigned char c;
                int px = mx0 + mx * scale;
                int py = my0 + my * scale;
                if (g_map[my][mx] > 0)
                    c = PAL(HUE_STONE, 10);
                else
                    c = PAL(HUE_GRAY, 2);
                buf[py * WIDTH + px] = c;
                buf[py * WIDTH + px + 1] = c;
                if (py + 1 < HEIGHT) {
                    buf[(py + 1) * WIDTH + px] = c;
                    buf[(py + 1) * WIDTH + px + 1] = c;
                }
            }
        }
        /* Player dot */
        {
            int ppx = mx0 + (int)(g_posX * scale);
            int ppy = my0 + (int)(g_posY * scale);
            if (ppx >= 0 && ppx < WIDTH && ppy >= 0 && ppy < HEIGHT)
                buf[ppy * WIDTH + ppx] = PAL(HUE_MOSS, 14);
        }
        /* Duck dots */
        for (i = 0; i < g_num_ducks; i++) {
            if (!g_ducks[i].alive) continue;
            {
                int dpx = mx0 + (int)(g_ducks[i].x * scale);
                int dpy = my0 + (int)(g_ducks[i].y * scale);
                if (dpx >= 0 && dpx < WIDTH && dpy >= 0 && dpy < HEIGHT)
                    buf[dpy * WIDTH + dpx] = PAL(HUE_DUCK, 14);
            }
        }
    }
}

/* ========================================================================
 *  GUN FLASH OVERLAY
 * ======================================================================== */

static int g_muzzle_flash = 0;

static void render_gun_flash(unsigned char *buf)
{
    int cx, cy, x, y, r;
    if (g_muzzle_flash <= 0) return;

    cx = WIDTH / 2;
    cy = VIEW_H - 20;
    r = 15 + g_muzzle_flash * 5;

    for (y = cy - r; y <= cy + r; y++) {
        for (x = cx - r; x <= cx + r; x++) {
            int dx, dy;
            if (x < 0 || x >= WIDTH || y < 0 || y >= VIEW_H) continue;
            dx = x - cx;
            dy = y - cy;
            if (dx * dx + dy * dy < r * r) {
                int sh = 15 - (dx * dx + dy * dy) * 15 / (r * r);
                if (sh > 0)
                    buf[y * WIDTH + x] = PAL(HUE_YELLOW, sh);
            }
        }
    }
}

/* ========================================================================
 *  TITLE SCREEN
 * ======================================================================== */

static void draw_title(void)
{
    printf("==========================================\n");
    printf("       Q U A C K   H U N T\n");
    printf("==========================================\n");
    printf("  Doom-style Duck Hunt in a Labyrinth\n");
    printf("\n");
    printf("  Controls:\n");
    printf("    W/S or Up/Down  - Move fwd/back\n");
    printf("    A/D             - Strafe left/right\n");
    printf("    Mouse           - Look / Aim\n");
    printf("    Left click      - Shoot\n");
    printf("    R               - Reload (6 shells)\n");
    printf("    Left/Right      - Keyboard turning\n");
    printf("    ESC             - Quit\n");
    printf("\n");
    printf("  Hunt all the ducks in the maze!\n");
    printf("  Each round spawns more, faster ducks.\n");
    printf("==========================================\n");
}

/* ========================================================================
 *  MAIN
 * ======================================================================== */

int main(int argc, char *argv[])
{
    unsigned char *lfb = NULL;
    unsigned char *frame_buf = NULL;
    unsigned long lfb_phys, map_size;
    unsigned long total_vram;
    int no_mtrr = 0, show_mtrrinfo = 0;
    int no_pmi = 1;   /* PMI off by default, -pmi enables */
    int no_dblbuf = 0;
    int hw_flip_requested = 0;
    int sched_requested = 0;
    int running = 1;
    int prev_lmb = 0;
    float fps = 0.0f;
    int frame_count = 0;
    unsigned long fps_lo0, fps_hi0, fps_lo1, fps_hi1;
    unsigned long frame_lo0, frame_hi0, frame_lo1, frame_hi1;
    float dt = 0.016f;
    int i;

    /* Parse args */
    for (i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-nomtrr") == 0) no_mtrr = 1;
        else if (strcmp(argv[i], "-mtrrinfo") == 0) show_mtrrinfo = 1;
        else if (strcmp(argv[i], "-vbe2") == 0) g_force_vbe2 = 1;
        else if (strcmp(argv[i], "-pmi") == 0) no_pmi = 0;
        else if (strcmp(argv[i], "-nopmi") == 0) no_pmi = 1;
        else if (strcmp(argv[i], "-hwflip") == 0) hw_flip_requested = 1;
        else if (strcmp(argv[i], "-sched") == 0) sched_requested = 1;
        else if (strcmp(argv[i], "-vsync") == 0) g_vsync_on = 1;
        else if (strcmp(argv[i], "-nodblbuf") == 0) no_dblbuf = 1;
    }

    draw_title();
    printf("[1] DOS mem...\n");

    /* Conventional DOS memory for VBE calls:
     *   512 B VBEInfo + 256 B ModeInfo + 1024 B palette = 1792 B
     *   128 paragraphs = 2048 B — enough with room to spare */
    if (!dpmi_alloc_dos(128)) {
        printf("Failed to allocate DOS memory\n");
        return 1;
    }

    /* RDTSC calibration */
    printf("[2] RDTSC cal...\n");
    calibrate_rdtsc();
    printf("CPU: %.0f MHz\n", g_rdtsc_mhz);

    /* VBE init */
    printf("[3] VBE info...\n");
    if (!vbe_get_info()) {
        dpmi_free_dos();
        printf("VBE BIOS not found\n");
        return 1;
    }
    printf("VBE %d.%d%s\n", g_vbi.version >> 8, g_vbi.version & 0xFF,
           (g_vbi.version >= 0x0300 && !g_force_vbe2) ? " (VBE3)" : "");
    g_vbe_version = g_vbi.version;
    g_vbe3 = (g_vbi.version >= 0x0300);
    if (g_force_vbe2) g_vbe3 = 0;
    if (hw_flip_requested) g_hw_flip = 1;

    printf("[4] Mode info...\n");
    if (!vbe_get_mode_info(TARGET_MODE)) {
        dpmi_free_dos();
        printf("Mode 0x%03X not supported\n", TARGET_MODE);
        return 1;
    }

    if (!(g_mib.mode_attr & 0x80)) {
        dpmi_free_dos();
        printf("Mode 0x%03X has no LFB support\n", TARGET_MODE);
        return 1;
    }

    lfb_phys = g_mib.phys_base;
    g_lfb_pitch = g_mib.bytes_per_line;
    if (g_lfb_pitch == 0) g_lfb_pitch = WIDTH;
    g_page_size = (unsigned long)g_lfb_pitch * HEIGHT;
    total_vram = (unsigned long)g_vbi.total_memory * 65536UL;
    if (total_vram >= g_page_size * 2UL && !no_dblbuf)
        g_use_doublebuf = 1;
    map_size = g_use_doublebuf ? g_page_size * 2UL : g_page_size;

    printf("LFB phys=0x%08lX  pitch=%d  dblbuf=%s\n",
           lfb_phys, g_lfb_pitch, g_use_doublebuf ? "YES" : "NO");

    printf("[5] Map LFB...\n");
    /* Map LFB */
    lfb = (unsigned char *)dpmi_map_physical(lfb_phys, map_size);
    if (!lfb) {
        dpmi_free_dos();
        printf("Failed to map LFB\n");
        return 1;
    }

    /* MTRR WC */
    printf("[6] MTRR...\n");
    if (!no_mtrr) {
        int wc = setup_mtrr_wc(lfb_phys, total_vram);
        if (wc == 1)
            printf("MTRR WC    : enabled (slot %d)\n", g_mtrr_slot);
        else if (wc == -1)
            printf("MTRR WC    : already active\n");
        else
            printf("MTRR WC    : not available\n");
    }

    /* PAT fix */
    printf("[7] PAT...\n");
    if (g_mtrr_wc >= 1) {
        int pat = setup_pat_uc_minus();
        if (pat == 1)
            printf("PAT fix    : entry 3 UC->UC- (WC passthrough)\n");
        else if (pat == -1)
            printf("PAT fix    : not needed\n");
    }

    /* PMI init */
    printf("[8] PMI...\n");
    if (g_vbe3 && !no_pmi) {
        if ((_get_cs() & 3) != 0) {
            printf("PMI        : skipped (ring 3)\n");
        } else {
            g_pmi_ok = query_pmi();
            if (g_pmi_ok) {
                unsigned long entry = (unsigned long)g_pmi_rm_seg * 16
                                    + g_pmi_rm_off + g_pmi_setds_off;
                printf("PMI        : available (entry 0x%08lX)\n", entry);
            } else {
                printf("PMI        : not available\n");
            }
        }
    }

    /* System RAM frame buffer */
    printf("[9] malloc...\n");
    frame_buf = (unsigned char *)malloc(WIDTH * HEIGHT);
    if (!frame_buf) {
        dpmi_unmap_physical(lfb);
        dpmi_free_dos();
        printf("Out of memory for frame buffer\n");
        return 1;
    }
    memset(frame_buf, 0, WIDTH * HEIGHT);

    /* Init font (before mode set, uses INT 10h) */
    printf("[10] font...\n");
    init_font();

    /* Generate assets */
    printf("[11] textures...\n");
    generate_textures();
    generate_duck_sprites();

    /* Init mouse */
    printf("[12] mouse...\n");
    g_mouse_ok = mouse_init();
    printf("Mouse      : %s\n", g_mouse_ok ? "OK" : "not found (keyboard only)");

    /* Spawn initial ducks */
    srand(42);
    spawn_round_ducks(g_round);
    printf("Ducks      : %d spawned for round %d\n", g_ducks_alive, g_round);

    printf("[13] Press key...\n");
    getch();

    /* Set VBE mode */
    printf("[14] Set mode...");
    if (!vbe_set_mode(TARGET_MODE)) {
        free(frame_buf);
        dpmi_unmap_physical(lfb);
        dpmi_free_dos();
        printf("Set mode failed\n");
        return 1;
    }

    /* Test VBE 4F07h page flip */
    if (g_use_doublebuf) {
        if (!vbe_set_display_start(0, 0, 0)) {
            g_use_doublebuf = 0;
        } else if (g_pmi_ok && !pmi_set_display_start(0, 0, 0)) {
            g_pmi_ok = 0;
            vbe_set_display_start(0, 0, 0);
        }
        /* Test scheduled flip (opt-in: ATI ATOMBIOS crashes on BL=02h) */
        if (g_use_doublebuf && g_vbe3 && sched_requested) {
            int sched_ok;
            if (g_pmi_ok)
                sched_ok = pmi_schedule_display_start(0, (unsigned short)HEIGHT);
            else
                sched_ok = vbe_schedule_display_start(0, (unsigned short)HEIGHT);
            if (sched_ok) {
                g_sched_flip = 1;
                if (g_pmi_ok) {
                    while (!pmi_is_flip_complete()) {}
                    pmi_set_display_start(0, 0, 0);
                } else {
                    while (!vbe_is_flip_complete()) {}
                    vbe_set_display_start(0, 0, 0);
                }
            }
        }
    }

    /* Test HW flip */
    if (g_hw_flip && g_use_doublebuf) {
        if ((_get_cs() & 3) != 0) {
            g_hw_flip = 0;
        } else if (!detect_gpu_iobase(lfb_phys)) {
            g_hw_flip = 0;
        } else {
            unsigned long cur = gpu_reg_read(R5_D1GRPH_PRIMARY_SURFACE_ADDRESS);
            if ((cur & 0xFFF00000UL) != (lfb_phys & 0xFFF00000UL)) {
                g_hw_flip = 0;
            } else {
                g_fb_phys = lfb_phys;
            }
        }
    } else {
        g_hw_flip = 0;
    }

    /* Try 8-bit DAC */
    vbe_set_dac_width(8);

    /* Set palette */
    setup_palette();

    /* Set initial display start for double buffering */
    if (g_use_doublebuf) {
        vbe_set_display_start(0, 0, 0);
        g_back_page = 1;
    }

    /* Install keyboard handler */
    install_keyboard();

    /* Drain any pending mouse motion */
    if (g_mouse_ok) {
        int dx, dy;
        mouse_get_mickeys(&dx, &dy);
    }

    /* Main game loop */
    _rdtsc_pair(&fps_lo0, &fps_hi0);
    _rdtsc_pair(&frame_lo0, &frame_hi0);

    while (running) {
        int lmb;

        /* Timing */
        _rdtsc_pair(&frame_lo1, &frame_hi1);
        dt = (float)rdtsc_elapsed_ms(frame_lo0, frame_hi0, frame_lo1, frame_hi1) / 1000.0f;
        if (dt > 0.1f) dt = 0.1f;
        if (dt < 0.001f) dt = 0.001f;
        frame_lo0 = frame_lo1;
        frame_hi0 = frame_hi1;

        /* FPS calculation */
        frame_count++;
        {
            double elapsed = rdtsc_elapsed_ms(fps_lo0, fps_hi0, frame_lo1, frame_hi1);
            if (elapsed > 500.0) {
                fps = (float)(frame_count * 1000.0 / elapsed);
                frame_count = 0;
                fps_lo0 = frame_lo1;
                fps_hi0 = frame_hi1;
            }
        }

        /* Check ESC */
        if (g_keys[SC_ESC]) { running = 0; break; }

        /* Reload */
        if (g_keys[SC_R]) g_ammo = 6;

        /* Update player */
        update_player(dt);

        /* Update ducks */
        update_ducks(dt);

        /* Shooting */
        lmb = g_mouse_ok ? (mouse_get_buttons() & 1) : g_keys[SC_SPACE];
        if (lmb && !prev_lmb && g_shot_cooldown <= 0.0f) {
            if (g_ammo > 0) {
                g_ammo--;
                g_shots_fired++;
                g_muzzle_flash = 4;
                try_shoot();
                g_shot_cooldown = 0.25f;
            }
        }
        prev_lmb = lmb;
        if (g_shot_cooldown > 0.0f) g_shot_cooldown -= dt;
        if (g_muzzle_flash > 0) g_muzzle_flash--;
        if (g_hit_flash > 0) g_hit_flash--;

        /* Next round check */
        if (g_ducks_alive <= 0) {
            g_round++;
            g_ammo = 6;
            spawn_round_ducks(g_round);
        }

        /* ---- RENDER ---- */
        render_walls(frame_buf);
        render_sprites(frame_buf);
        render_gun_flash(frame_buf);
        draw_crosshair(frame_buf);
        render_hud(frame_buf, fps);

        /* Blit and flip */
        if (g_use_doublebuf) {
            if (g_hw_flip) {
                if (g_vsync_on)
                    while (!hw_is_flip_done()) {}
                blit_to_lfb(lfb + (unsigned long)g_back_page * g_page_size,
                            g_lfb_pitch, frame_buf);
                hw_page_flip(g_fb_phys + (unsigned long)g_back_page * g_page_size);
            } else if (g_sched_flip) {
                if (g_pmi_ok)
                    while (!pmi_is_flip_complete()) {}
                else
                    while (!vbe_is_flip_complete()) {}
                blit_to_lfb(lfb + (unsigned long)g_back_page * g_page_size,
                            g_lfb_pitch, frame_buf);
                if (g_vsync_on) {
                    if (g_pmi_ok)
                        pmi_schedule_display_start(0, (unsigned short)(g_back_page * HEIGHT));
                    else
                        vbe_schedule_display_start(0, (unsigned short)(g_back_page * HEIGHT));
                } else {
                    if (g_pmi_ok)
                        pmi_set_display_start(0, (unsigned short)(g_back_page * HEIGHT), 0);
                    else
                        vbe_set_display_start(0, (unsigned short)(g_back_page * HEIGHT), 0);
                }
            } else {
                blit_to_lfb(lfb + (unsigned long)g_back_page * g_page_size,
                            g_lfb_pitch, frame_buf);
                if (g_pmi_ok)
                    pmi_set_display_start(0, (unsigned short)(g_back_page * HEIGHT), 0);
                else
                    vbe_set_display_start(0, (unsigned short)(g_back_page * HEIGHT), 0);
                if (g_vsync_on)
                    wait_vsync();
            }
            g_back_page = 1 - g_back_page;
        } else {
            if (g_vsync_on)
                wait_vsync();
            blit_to_lfb(lfb, g_lfb_pitch, frame_buf);
        }
    }

    /* ---- Cleanup ---- */
    restore_keyboard();
    if (g_use_doublebuf)
        vbe_set_display_start(0, 0, 0);
    vbe_set_text_mode();
    restore_pat();
    restore_mtrr();
    dpmi_unmap_physical(lfb);
    free(frame_buf);
    dpmi_free_dos();

    printf("QUACK HUNT done. Score: %d  Round: %d  Shots: %d\n",
           g_score, g_round, g_shots_fired);
    printf("  WC:%s PAT:%s PMI:%s HWF:%s DBLBUF:%s SCHED:%s VSYNC:%s\n",
           g_mtrr_wc ? "ON" : "OFF",
           g_pat_modified ? "FIX" : "---",
           g_pmi_ok ? "ON" : "---",
           g_hw_flip ? "ON" : "---",
           g_use_doublebuf ? "ON" : "---",
           g_sched_flip ? "ON" : "---",
           g_vsync_on ? "ON" : "---");
    return 0;
}
