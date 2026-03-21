/*
 * PLASMA.C  -  8-bit VESA plasma effect, 1024x768, vsync demonstration
 * OpenWatcom 2.0, 32-bit DOS/4GW
 *
 * Demonstrates:
 *   - Classic 4-component plasma (horizontal, vertical, diagonal, radial waves)
 *   - VBE 2.0+ linear frame buffer in 1024x768 8bpp
 *   - VBE 4F09h palette programming (works on Radeon in VESA modes)
 *   - VBE 2.0 hardware page flip via INT 10h AX=4F07h — triple-buffered:
 *     sysram render buffer + 2 VRAM pages (render→sysram, blit→back, flip)
 *   - VBE 3.0 Protected Mode Interface (PMI) for faster page flip via
 *     DPMI 0301h direct procedure call (bypasses INT 10h dispatch)
 *   - Falls back to double-buffer (sysram+blit, no flip) if VRAM < 2 pages
 *     or 4F07h fails
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
 *   -vbe2       = force VBE 2.0 mode (disable VBE 3.0 features)
 *   -pmi        = enable VBE 3.0 Protected Mode Interface
 *   -nopmi      = disable PMI (default)
 *   -nomtrr     = skip MTRR write-combining setup
 *   -mtrrinfo   = show detailed MTRR/PAT/PTE diagnostic dump
 *   -directvram = render directly to VRAM (slow, for benchmarking)
 *   -hwflip     = direct GPU register page flip (tear-free, bypasses BIOS)
 *
 * Build:
 *   wcc386 -bt=dos -3r -ox -s -zq PLASMA.C
 *   wlink system dos4g name PLASMA file PLASMA option quiet
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <time.h>
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
static int           g_no_pmi      = 1;    /* 0=use PMI, 1=skip (default: skip, use -pmi to enable) */
static int           g_no_mtrr     = 0;    /* non-zero to skip MTRR WC     */
static int           g_mtrr_info   = 0;    /* non-zero to show MTRR dump   */
static int           g_direct_vram = 0;    /* non-zero to render direct to VRAM (slow, for comparison) */
static unsigned short g_vbe_version = 0;    /* raw VBE version (BCD)        */
static int           g_pmi_ok       = 0;    /* non-zero when PMI page-flip  */
static int           g_sched_flip   = 0;    /* VBE 3.0 scheduled flip (BL=02h) */
static int           g_sched_req    = 0;    /* -sched flag (opt-in, ATI crashes) */
static unsigned short g_pmi_rm_seg  = 0;    /* real-mode segment of PMI tbl */
static unsigned short g_pmi_rm_off  = 0;    /* real-mode offset  of PMI tbl */
static unsigned short g_pmi_size    = 0;    /* PMI table size in bytes      */
static unsigned short g_pmi_setw_off  = 0;  /* SetWindow entry offset       */
static unsigned short g_pmi_setds_off = 0;  /* SetDisplayStart entry offset */
static unsigned short g_pmi_setpal_off= 0;  /* SetPrimaryPalette offset     */

/* Direct GPU register page flip (bypasses BIOS completely) */
static int            g_hw_flip      = 0;   /* use direct HW register flip  */
static unsigned short g_gpu_iobase   = 0;   /* GPU I/O base (MM_INDEX port) */
static unsigned long  g_fb_phys      = 0;   /* LFB physical base for flip   */

static unsigned char *dos_buf(void)
{
#ifdef __DJGPP__
    return (unsigned char *)((unsigned long)g_dos_seg * 16 + __djgpp_conventional_base);
#else
    return (unsigned char *)((unsigned long)g_dos_seg << 4);
#endif
}

#ifdef __DJGPP__
static int dpmi_alloc_dos(unsigned short paragraphs)
{
    int sel_or_max;
    int seg = __dpmi_allocate_dos_memory(paragraphs, &sel_or_max);
    if (seg == -1) return 0;
    g_dos_seg = (unsigned short)seg;
    g_dos_sel = (unsigned short)sel_or_max;
    return 1;
}
#else
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
#endif

#ifdef __DJGPP__
static void dpmi_free_dos(void)
{
    if (!g_dos_sel) return;
    __dpmi_free_dos_memory(g_dos_sel);
    g_dos_sel = 0;
    g_dos_seg = 0;
}
#else
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
#endif

#ifdef __DJGPP__
static int dpmi_real_int(unsigned char intno, RMI *rmi)
{
    return (__dpmi_simulate_real_mode_interrupt(intno, (__dpmi_regs *)rmi) == 0);
}
#else
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
#endif

#ifdef __DJGPP__
static void *dpmi_map_physical(unsigned long phys, unsigned long size)
{
    __dpmi_meminfo mi;
    mi.address = phys;
    mi.size = size;
    if (__dpmi_physical_address_mapping(&mi) == -1) return NULL;
    return (void *)(mi.address + __djgpp_conventional_base);
}
#else
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
#endif

#ifdef __DJGPP__
static void dpmi_unmap_physical(void *ptr)
{
    __dpmi_meminfo mi;
    mi.address = (unsigned long)ptr - __djgpp_conventional_base;
    mi.size = 0;
    __dpmi_free_physical_address_mapping(&mi);
}
#else
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
#endif

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
/*
 * VBE 4F07h: Set/Get Display Start
 *
 * BL=00h: set display start immediately (may tear)
 * BL=02h: VBE 3.0 scheduled flip — sets start address to take effect at
 *         next vsync, returns immediately (non-blocking, tear-free)
 * BL=04h: VBE 3.0 get scheduled flip status
 *         returns BH: 0 = flip completed, 1 = flip still pending
 * BL=80h: wait for vsync then set display start (blocking, tear-free)
 *         NOTE: unreliable on ATI ATOMBIOS — slow command table execution
 *         can miss the vsync window, causing tearing.  Prefer BL=00h with
 *         separate wait_vsync() via port 0x3DA.
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

/* VBE 3.0: schedule display start at next vsync (BL=02h, non-blocking). */
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

/* VBE 3.0: check if the last scheduled flip has completed (BL=04h).
 * Returns 1 = flip done, 0 = still pending or error. */
static int vbe_is_flip_complete(void)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F07;
    rmi.ebx = 0x0004;
    if (!dpmi_real_int(0x10, &rmi)) return 1; /* assume done on error */
    if ((rmi.eax & 0xFFFF) != 0x004F) return 1;
    return ((rmi.ebx & 0xFF00) == 0);  /* BH=0 means flip complete */
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
#ifdef __DJGPP__
    pmi_flat += __djgpp_conventional_base;
#endif
    g_pmi_setw_off   = *(unsigned short *)(pmi_flat + 0);
    g_pmi_setds_off  = *(unsigned short *)(pmi_flat + 2);
    g_pmi_setpal_off = *(unsigned short *)(pmi_flat + 4);

    return 1;
}

/*
 * VBE 3.0 PMI: direct 32-bit protected-mode call.
 *
 * The PMI table returned by INT 10h AX=4F0Ah contains 32-bit PM code
 * that does direct port I/O to the video hardware (CRTC registers).
 * It lives in the BIOS ROM area (typically 0xC0000–0xCFFFF), which is
 * accessible via the flat memory model.
 *
 * The correct calling convention is a near CALL in 32-bit PM — NOT
 * DPMI 0300h/0301h (those are for real-mode code).  Registers follow
 * the same convention as INT 10h VBE calls.
 *
 * Requires ring 0 (PMODE/W) for the direct port I/O in the PMI code.
 */
#ifdef __DJGPP__
static unsigned long __attribute__((noinline))
_pmi_call4(unsigned long entry, unsigned long _eax,
           unsigned long _ebx, unsigned long _ecx, unsigned long _edx)
{
    unsigned long result = _eax;
    __asm__ __volatile__ (
        "call *%%esi"
        : "+a"(result), "+S"(entry), "+b"(_ebx), "+c"(_ecx), "+d"(_edx)
        :: "edi", "memory"
    );
    return result;
}

static unsigned long __attribute__((noinline))
_pmi_call4_ebx(unsigned long entry, unsigned long _eax,
               unsigned long _ebx, unsigned long _ecx, unsigned long _edx)
{
    unsigned long result = _ebx;
    __asm__ __volatile__ (
        "call *%%esi"
        : "+b"(result), "+S"(entry), "+a"(_eax), "+c"(_ecx), "+d"(_edx)
        :: "edi", "memory"
    );
    return result;
}
#else
unsigned long _pmi_call4(unsigned long entry, unsigned long eax,
                         unsigned long ebx,  unsigned long ecx,
                         unsigned long edx);
#pragma aux _pmi_call4 = \
    "call esi"           \
    parm [esi] [eax] [ebx] [ecx] [edx] \
    value [eax]          \
    modify [ebx ecx edx esi edi]

/* Same as above but returns EBX (for 4F07h BL=04h status check). */
unsigned long _pmi_call4_ebx(unsigned long entry, unsigned long eax,
                              unsigned long ebx,  unsigned long ecx,
                              unsigned long edx);
#pragma aux _pmi_call4_ebx = \
    "call esi"               \
    parm [esi] [eax] [ebx] [ecx] [edx] \
    value [ebx]              \
    modify [eax ecx edx esi edi]
#endif

static int pmi_set_display_start(unsigned short cx, unsigned short dy,
                                 int wait)
{
    unsigned long entry = (unsigned long)g_pmi_rm_seg * 16
                        + g_pmi_rm_off + g_pmi_setds_off;
#ifdef __DJGPP__
    entry += __djgpp_conventional_base;
#endif
    unsigned long result;
    result = _pmi_call4(entry, 0x4F07,
                        wait ? 0x0080UL : 0x0000UL,
                        (unsigned long)cx,
                        (unsigned long)dy);
    return ((result & 0xFFFF) == 0x004F);
}

/* PMI: schedule display start at next vsync (BL=02h, non-blocking). */
static int pmi_schedule_display_start(unsigned short cx, unsigned short dy)
{
    unsigned long entry = (unsigned long)g_pmi_rm_seg * 16
                        + g_pmi_rm_off + g_pmi_setds_off;
#ifdef __DJGPP__
    entry += __djgpp_conventional_base;
#endif
    unsigned long result;
    result = _pmi_call4(entry, 0x4F07, 0x0002UL,
                        (unsigned long)cx,
                        (unsigned long)dy);
    return ((result & 0xFFFF) == 0x004F);
}

/* PMI: check if the last scheduled flip has completed (BL=04h). */
static int pmi_is_flip_complete(void)
{
    unsigned long entry = (unsigned long)g_pmi_rm_seg * 16
                        + g_pmi_rm_off + g_pmi_setds_off;
#ifdef __DJGPP__
    entry += __djgpp_conventional_base;
#endif
    unsigned long ebx_out;
    ebx_out = _pmi_call4_ebx(entry, 0x4F07, 0x0004UL, 0, 0);
    return ((ebx_out & 0xFF00) == 0);  /* BH=0 means flip complete */
}

/* --------------------------------------------------------------------------
 * Direct GPU register access via PCI I/O-mapped MMIO (MM_INDEX / MM_DATA)
 *
 * ATI/AMD GPUs expose their register file through an I/O BAR:
 *   port+0 = MM_INDEX (write register address)
 *   port+4 = MM_DATA  (read/write register value)
 *
 * This bypasses the BIOS entirely, allowing us to use the hardware's
 * built-in surface update lock for tear-free page flipping.
 *
 * VBEDUMP analysis of the X1300 Pro PMI code revealed three BIOS bugs:
 *   1. PMI SetDisplayStart address overflows for Y >= 64 (24-bit mask)
 *   2. PMI BL=80h vsync wait writes registers AFTER blanking ends
 *   3. No D1GRPH_UPDATE_LOCK used — immediate writes cause tearing
 *
 * This direct implementation fixes all three by:
 *   - Computing correct physical address (base + page * pitch * height)
 *   - Using D1GRPH_UPDATE_LOCK so hardware applies at next vsync
 *   - Never blocking — CPU is free to render the next frame immediately
 *
 * Requires ring 0 (PMODE/W) for I/O port access.
 * -------------------------------------------------------------------------- */

/* R500 (RV515/X1300) register definitions */
#define R5_D1GRPH_PRIMARY_SURFACE_ADDRESS   0x6110
#define R5_D1GRPH_SECONDARY_SURFACE_ADDRESS 0x6118
#define R5_D1GRPH_UPDATE                    0x6144
#define R5_D1GRPH_SURFACE_UPDATE_PENDING    (1UL << 2)
#define R5_D1GRPH_SURFACE_UPDATE_LOCK       (1UL << 16)

/* Dword I/O port access (named to avoid conio.h conflict) */
#ifdef __DJGPP__
static inline void _gpu_outpd(unsigned short port, unsigned long val)
{
    __asm__ __volatile__ ("outl %0, %w1" :: "a"(val), "Nd"(port));
}
static inline unsigned long _gpu_inpd(unsigned short port)
{
    unsigned long val;
    __asm__ __volatile__ ("inl %w1, %0" : "=a"(val) : "Nd"(port));
    return val;
}
#else
void _gpu_outpd(unsigned short port, unsigned long val);
#pragma aux _gpu_outpd = "out dx, eax" parm [dx] [eax] modify exact []

unsigned long _gpu_inpd(unsigned short port);
#pragma aux _gpu_inpd = "in eax, dx" parm [dx] value [eax] modify exact []
#endif

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

/* Detect GPU I/O base by PCI bus scan.
 * Finds the PCI device whose memory BAR matches lfb_phys,
 * then locates its I/O BAR for MM_INDEX/MM_DATA access. */
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
                    /* I/O BAR — strip indicator bits (low byte like PMI) */
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

/* Tear-free page flip using D1GRPH surface update lock.
 * The lock buffers register writes; on unlock the hardware applies
 * the new surface address atomically at the next vsync boundary. */
static void hw_page_flip(unsigned long page_phys_addr)
{
    unsigned long lock;

    /* Lock surface updates */
    lock = gpu_reg_read(R5_D1GRPH_UPDATE);
    gpu_reg_write(R5_D1GRPH_UPDATE, lock | R5_D1GRPH_SURFACE_UPDATE_LOCK);

    /* Write new surface address to both primary and secondary */
    gpu_reg_write(R5_D1GRPH_PRIMARY_SURFACE_ADDRESS, page_phys_addr);
    gpu_reg_write(R5_D1GRPH_SECONDARY_SURFACE_ADDRESS, page_phys_addr);

    /* Unlock — hardware applies at next vsync */
    lock = gpu_reg_read(R5_D1GRPH_UPDATE);
    gpu_reg_write(R5_D1GRPH_UPDATE, lock & ~R5_D1GRPH_SURFACE_UPDATE_LOCK);
}

/* Check if the previous hw_page_flip has been applied by hardware.
 * Returns 1 if flip is done, 0 if still pending. */
static int hw_is_flip_done(void)
{
    return !(gpu_reg_read(R5_D1GRPH_UPDATE) & R5_D1GRPH_SURFACE_UPDATE_PENDING);
}

/* --------------------------------------------------------------------------
 * MTRR Write-Combining for LFB
 *
 * When running at ring 0 (PMODE/W), programs a CPU variable-range MTRR to
 * mark the LFB physical address range as write-combining (WC).  The CPU
 * then combines sequential byte/word/dword writes into full cache-line
 * burst transactions over PCI/PCIe — typically ~10x faster VRAM blits.
 *
 * When running at ring 3, MTRR setup is skipped since
 * RDMSR/WRMSR are privileged (ring-0) instructions.
 * Use CWSDPR0 (ring 0) or PMODE/W for MTRR access.
 * -------------------------------------------------------------------------- */

#define MSR_MTRRCAP         0xFE
#define MSR_MTRR_PHYSBASE0  0x200
#define MSR_MTRR_PHYSMASK0  0x201
#define MTRR_TYPE_WC        1

/* Inline helpers for privileged CPU instructions (ring 0 only).
 * Use raw opcodes to avoid assembler version dependencies. */

#ifdef __DJGPP__

static inline unsigned short _get_cs(void)
{
    unsigned short cs;
    __asm__ __volatile__ ("movw %%cs, %0" : "=r"(cs));
    return cs;
}

static inline int _has_cpuid(void)
{
    unsigned long result, scratch;
    __asm__ __volatile__ (
        "pushfl\n\t"
        "popl %%eax\n\t"
        "movl %%eax, %%ecx\n\t"
        "xorl $0x200000, %%eax\n\t"
        "pushl %%eax\n\t"
        "popfl\n\t"
        "pushfl\n\t"
        "popl %%eax\n\t"
        "xorl %%ecx, %%eax\n\t"
        "shrl $21, %%eax\n\t"
        "andl $1, %%eax"
        : "=a"(result), "=c"(scratch) :: "memory"
    );
    return (int)result;
}

static inline unsigned long _cpuid1_edx(void)
{
    unsigned long eax = 1, ebx_out, ecx_out, edx;
    __asm__ __volatile__ (
        "cpuid"
        : "+a"(eax), "=b"(ebx_out), "=c"(ecx_out), "=d"(edx)
    );
    return edx;
}

static inline unsigned long _rdmsr_lo(unsigned long msr)
{
    unsigned long lo, hi;
    __asm__ __volatile__ ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return lo;
}

static inline unsigned long _rdmsr_hi(unsigned long msr)
{
    unsigned long lo, hi;
    __asm__ __volatile__ ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return hi;
}

static inline void _wrmsr_3(unsigned long msr, unsigned long lo, unsigned long hi)
{
    __asm__ __volatile__ ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

static inline void _wbinvd(void)
{
    __asm__ __volatile__ ("wbinvd" ::: "memory");
}

static inline unsigned long _read_cr3(void)
{
    unsigned long val;
    __asm__ __volatile__ ("movl %%cr3, %0" : "=r"(val));
    return val;
}

static inline void _flush_tlb(void)
{
    unsigned long tmp;
    __asm__ __volatile__ (
        "movl %%cr3, %0\n\t"
        "movl %0, %%cr3"
        : "=r"(tmp) :: "memory"
    );
}

static inline unsigned long _read_cr0(void)
{
    unsigned long val;
    __asm__ __volatile__ ("movl %%cr0, %0" : "=r"(val));
    return val;
}

/* Enable SSE/SSE2/SSE3 for Pentium D: set OSFXSR + OSXMMEXCPT in CR4 */
static void djgpp_enable_sse(void)
{
    unsigned long cr4;
    if ((_get_cs() & 3) != 0) return;
    if (!_has_cpuid()) return;
    if (!(_cpuid1_edx() & (1UL << 25))) return;
    __asm__ __volatile__ ("movl %%cr4, %0" : "=r"(cr4));
    cr4 |= (1UL << 9) | (1UL << 10);
    __asm__ __volatile__ ("movl %0, %%cr4" :: "r"(cr4) : "memory");
}

/* Read the current CS segment limit via the LSL instruction. */
static unsigned long get_cs_limit(void)
{
    unsigned long limit;
    unsigned short cs = _get_cs();
    __asm__ __volatile__ ("lsll %1, %0"
                          : "=r"(limit)
                          : "r"((unsigned long)cs));
    return limit;
}

/*
 * Extract the 32-bit base address from an 8-byte descriptor entry.
 * GDT/LDT descriptor layout: base is split across bytes 2-4 and 7.
 */
static unsigned long desc_base(const unsigned char *d)
{
    return (unsigned long)d[2]
         | ((unsigned long)d[3] << 8)
         | ((unsigned long)d[4] << 16)
         | ((unsigned long)d[7] << 24);
}

/*
 * Expand CS limit to 4 GB so PMI calls can reach BIOS ROM code.
 *
 * Strategy:
 *   1. Try DPMI function 0008h (Set Segment Limit) — the clean way.
 *      Works on PMODE/DJ (GDT selectors, ring 0).  The INT 31h return
 *      via IRETD automatically reloads CS from the modified descriptor.
 *   2. If DPMI fails (e.g. CWSDPR0 rejects 0008h for CS), fall back to
 *      direct descriptor table patching at ring 0 + LRET to reload CS.
 *      Handles both GDT selectors (PMODE/DJ) and LDT selectors (CWSDPR0).
 *
 * Required: ring 0, nearptr already enabled (DS limit = 4 GB).
 */
static int expand_cs_to_4gb(void)
{
    unsigned short cs = _get_cs();
    unsigned long old_limit;

    if ((cs & 3) != 0) return 0;     /* ring 0 only */

    old_limit = get_cs_limit();
    printf("  CS=0x%04X (%s, idx=%u), limit=0x%08lX\n",
           cs, (cs & 4) ? "LDT" : "GDT", cs >> 3, old_limit);

    /* --- Method 1: DPMI function 0008h (Set Segment Limit) --- */
    __dpmi_set_segment_limit(_get_cs(), 0xFFFFFFFFUL);
    if (get_cs_limit() == 0xFFFFFFFFUL) {
        printf("  Expanded via DPMI 0008h\n");
        return 1;
    }

    /* --- Method 2: Direct descriptor table patching at ring 0 --- */
    {
        unsigned char gdt_buf[8];
        unsigned long gdt_base;
        unsigned int idx = cs >> 3;
        int use_ldt = (cs & 4) != 0;
        unsigned char *table_ptr;
        unsigned char *entry;

        __asm__ __volatile__ ("sgdt %0" : "=m"(gdt_buf));
        gdt_base = *(unsigned long *)&gdt_buf[2];

        if (use_ldt) {
            /* CS is in the LDT — find LDT base from its GDT descriptor */
            unsigned short ldt_sel;
            unsigned char *ldt_gdt_entry;
            unsigned long ldt_base;

            __asm__ __volatile__ ("sldt %0" : "=r"(ldt_sel));

            ldt_gdt_entry = (unsigned char *)
                ((long)gdt_base + __djgpp_conventional_base)
                + (ldt_sel >> 3) * 8;
            ldt_base = desc_base(ldt_gdt_entry);

            table_ptr = (unsigned char *)
                ((long)ldt_base + __djgpp_conventional_base);
        } else {
            /* CS is in the GDT */
            table_ptr = (unsigned char *)
                ((long)gdt_base + __djgpp_conventional_base);
        }

        entry = table_ptr + idx * 8;

        /* Set limit to 0xFFFFF with G=1 (page granularity) → 4 GB */
        entry[0] = 0xFF;                        /* limit[0:7]   */
        entry[1] = 0xFF;                        /* limit[8:15]  */
        entry[6] = (entry[6] & 0xF0) | 0x0F;   /* limit[16:19] */
        entry[6] |= 0x80;                       /* G = 1        */

        /* Reload CS descriptor cache via far return */
        __asm__ __volatile__ (
            "pushl %0\n\t"
            "pushl $1f\n\t"
            "lret\n\t"
            "1:\n\t"
            :: "ri"((unsigned long)cs)
            : "memory"
        );
    }

    if (get_cs_limit() == 0xFFFFFFFFUL) {
        printf("  Expanded via direct %s patch\n",
               (cs & 4) ? "LDT" : "GDT");
        return 1;
    }
    return 0;
}

#else /* Watcom */

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

unsigned long _read_cr3(void);
#pragma aux _read_cr3 = \
    "db 0Fh, 20h, 0D8h"    \
    value [eax]

void _flush_tlb(void);
#pragma aux _flush_tlb = \
    "db 0Fh, 20h, 0D8h"    \
    "db 0Fh, 22h, 0D8h"    \
    modify [eax]

/* mov eax, cr0 = 0F 20 C0 (reg=000=CR0, rm=000=EAX) */
unsigned long _read_cr0(void);
#pragma aux _read_cr0 = \
    "db 0Fh, 20h, 0C0h"    \
    value [eax]

/*
 * WC-optimal block copy using REP MOVSD (x86 fast-string path).
 * Takes a DWORD count (not byte count) so the compiler cannot confuse
 * this with memcpy and substitute its own intrinsic expansion.
 * Forces the hardware fast-string engine (Pentium Pro+) which fills
 * WC write-combine buffers optimally (~900+ MB/s vs ~700).
 */
void wc_rep_movsd(void *dst, const void *src, unsigned long dwords);
#pragma aux wc_rep_movsd = \
    "rep movsd"             \
    parm [edi] [esi] [ecx]  \
    modify [edi esi ecx]

#endif /* __DJGPP__ */

#define MSR_MTRR_DEF_TYPE   0x2FF
#define MSR_PAT             0x277

/* MTRR state for cleanup on exit */
static int           g_mtrr_slot = -1;
static unsigned long g_mtrr_save_base_lo, g_mtrr_save_base_hi;
static unsigned long g_mtrr_save_mask_lo, g_mtrr_save_mask_hi;
static int           g_mtrr_wc = 0;  /* 0=off, 1=set by us, 2=already set */
static int           g_mtrr_replaced_uc = 0; /* 1 if we replaced a BIOS UC entry */

/* PAT state for cleanup on exit */
static unsigned long g_pat_save_lo = 0, g_pat_save_hi = 0;
static int           g_pat_modified = 0; /* 1 if we changed PAT entry 3 */

static unsigned long next_pow2(unsigned long v)
{
    v--;
    v |= v >> 1;  v |= v >> 2;
    v |= v >> 4;  v |= v >> 8;
    v |= v >> 16;
    return v + 1;
}

/*
 * Set up MTRR write-combining for [phys_addr, phys_addr + vram_bytes).
 *  Returns:  1 = WC enabled by us
 *           -1 = WC already active (BIOS/chipset)
 *            0 = not possible (ring 3, no MTRR, no free slot, etc.)
 *
 * Intel MTRR overlap rule: if two entries cover the same address, the result
 * type is the LOWEST-priority type (UC=0 beats WC=1 beats everything else).
 * So adding WC alongside an existing UC for the same range does NOTHING.
 * Fix: scan for a conflicting UC MTRR covering our LFB and replace it in-place
 * with WC.  Only fall back to a free slot if no UC conflict is found.
 */
static int setup_mtrr_wc(unsigned long phys_addr, unsigned long vram_bytes)
{
    unsigned long cap_lo, size, mask_val;
    int num_var, i;
    int free_slot = -1, replace_slot = -1;

    /* Ring 0 required for RDMSR/WRMSR */
    if ((_get_cs() & 3) != 0) return 0;

    if (!_has_cpuid()) return 0;

    /* CPUID.1 EDX bit 12 = MTRR support */
    if (!(_cpuid1_edx() & (1UL << 12))) return 0;

    /* MTRRCAP: bits 7:0 = # variable MTRRs, bit 10 = WC type supported */
    cap_lo = _rdmsr_lo(MSR_MTRRCAP);
    num_var = (int)(cap_lo & 0xFF);
    if (!(cap_lo & (1UL << 10))) return 0;
    if (num_var == 0) return 0;

    /* Compute WC range parameters (must do before scanning) */
    size = next_pow2(vram_bytes);
    if (phys_addr & (size - 1)) return 0;   /* must be naturally aligned */
    mask_val = ~(size - 1) & 0xFFFFF000UL;

    /* Single pass: look for existing WC covering us, a UC conflict, or a
     * free slot.  Priority: WC already set > replace UC conflict > free slot */
    for (i = 0; i < num_var; i++) {
        unsigned long mlo = _rdmsr_lo(MSR_MTRR_PHYSMASK0 + i * 2);
        unsigned long blo = _rdmsr_lo(MSR_MTRR_PHYSBASE0 + i * 2);

        if (!(mlo & 0x800)) {
            /* Free slot — record first one found */
            if (free_slot < 0) free_slot = i;
            continue;
        }

        /* Check if this active MTRR's mask covers phys_addr */
        if ((phys_addr & (mlo & 0xFFFFF000UL)) != (blo & 0xFFFFF000UL))
            continue;

        if ((blo & 0xFF) == MTRR_TYPE_WC) {
            /* LFB already WC — nothing to do */
            g_mtrr_wc = 2;
            return -1;
        }

        if ((blo & 0xFF) == 0 /* UC */) {
            /* Conflicting UC entry — must replace to avoid overlap loss */
            replace_slot = i;
        }
    }

    /* Prefer replacing a conflicting UC over adding to a free slot */
    i = (replace_slot >= 0) ? replace_slot : free_slot;
    if (i < 0) return 0;   /* no usable slot */

    /* Save original values for cleanup on exit */
    g_mtrr_save_base_lo = _rdmsr_lo(MSR_MTRR_PHYSBASE0 + i * 2);
    g_mtrr_save_base_hi = _rdmsr_hi(MSR_MTRR_PHYSBASE0 + i * 2);
    g_mtrr_save_mask_lo = _rdmsr_lo(MSR_MTRR_PHYSMASK0 + i * 2);
    g_mtrr_save_mask_hi = _rdmsr_hi(MSR_MTRR_PHYSMASK0 + i * 2);
    g_mtrr_slot = i;
    g_mtrr_replaced_uc = (replace_slot >= 0) ? 1 : 0;

    /* Program MTRR: CLI, flush caches, write, flush, STI */
    _disable();
    _wbinvd();
    _wrmsr_3(MSR_MTRR_PHYSBASE0 + i * 2,
             (phys_addr & 0xFFFFF000UL) | MTRR_TYPE_WC, 0);
    _wrmsr_3(MSR_MTRR_PHYSMASK0 + i * 2,
             mask_val | 0x800, 0);
    _wbinvd();
    _enable();

    g_mtrr_wc = 1;
    return 1;
}

static void restore_mtrr(void)
{
    if (g_mtrr_slot < 0) return;
    if ((_get_cs() & 3) != 0) return;

    _disable();
    _wbinvd();
    _wrmsr_3(MSR_MTRR_PHYSBASE0 + g_mtrr_slot * 2,
             g_mtrr_save_base_lo, g_mtrr_save_base_hi);
    _wrmsr_3(MSR_MTRR_PHYSMASK0 + g_mtrr_slot * 2,
             g_mtrr_save_mask_lo, g_mtrr_save_mask_hi);
    _wbinvd();
    _enable();

    g_mtrr_slot = -1;
    g_mtrr_wc = 0;
}

/* --------------------------------------------------------------------------
 * PAT Entry 3 fix for WC passthrough
 *
 * PMODE/W maps MMIO regions with PCD=1, PWT=1 in the PTE.
 * On PAT-capable CPUs, PCD=1 PWT=1 PAT=0 selects PAT entry 3.
 * The default PAT entry 3 is UC (strong), which BLOCKS MTRR WC.
 *
 * Fix: change PAT entry 3 from UC (0x00) to UC- (0x07) via MSR 0x277.
 * UC- defers to MTRR for the final memory type:
 *   UC- + MTRR WC → effective WC  (what we want for the LFB)
 *   UC- + MTRR UC → effective UC  (safe for other MMIO)
 *
 * This avoids fragile page table walking (which crashes in PMODE/W
 * because CR3 holds a physical address that may not be linearly mapped).
 * -------------------------------------------------------------------------- */

static int setup_pat_uc_minus(void)
{
    unsigned long pat_lo, pat_hi, entry3;

    if ((_get_cs() & 3) != 0) return 0;       /* need ring 0 */
    if (!(_cpuid1_edx() & (1UL << 16))) return 0; /* no PAT */

    pat_lo = _rdmsr_lo(MSR_PAT);
    pat_hi = _rdmsr_hi(MSR_PAT);

    /* Save for restore */
    g_pat_save_lo = pat_lo;
    g_pat_save_hi = pat_hi;

    /* Entry 3 = bits 24-31 of low dword (only bits 2:0 matter) */
    entry3 = (pat_lo >> 24) & 7;
    if (entry3 == 7) return -1;  /* already UC- */
    if (entry3 == 1) return -1;  /* already WC */

    /* Change entry 3 from UC (0) to UC- (7) */
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

/* --------------------------------------------------------------------------
 * MTRR diagnostic dump (printed with -mtrrinfo flag)
 * -------------------------------------------------------------------------- */
static const char *mtrr_type_name(unsigned long type)
{
    switch (type & 0xFF) {
        case 0: return "UC";
        case 1: return "WC";
        case 4: return "WT";
        case 5: return "WP";
        case 6: return "WB";
        case 7: return "UC-";
        default: return "??";
    }
}

static void dump_mtrr_info(unsigned long lfb_va, unsigned long map_size)
{
    unsigned long cap_lo, def_lo, pat_lo, pat_hi;
    unsigned long cr0;
    int num_var, i;

    if ((_get_cs() & 3) != 0) {
        printf("MTRR dump: ring 3, cannot read MSRs\n");
        return;
    }
    if (!_has_cpuid()) { printf("MTRR dump: no CPUID\n"); return; }

    cap_lo = _rdmsr_lo(MSR_MTRRCAP);
    num_var = (int)(cap_lo & 0xFF);
    def_lo = _rdmsr_lo(MSR_MTRR_DEF_TYPE);
    cr0 = _read_cr0();

    printf("==========================================\n");
    printf(" MTRR / PAT Diagnostic\n");
    printf("==========================================\n");
    printf(" MTRRCAP      : %d variable, WC=%s, FIX=%s\n",
           num_var,
           (cap_lo & (1UL << 10)) ? "yes" : "no",
           (cap_lo & (1UL <<  8)) ? "yes" : "no");
    printf(" DEF_TYPE     : %s (0x%02lX), MTRR_E=%d, FIX_E=%d\n",
           mtrr_type_name(def_lo), def_lo & 0xFF,
           (int)((def_lo >> 11) & 1), (int)((def_lo >> 10) & 1));
    printf(" Paging       : %s\n", (cr0 & (1UL << 31)) ? "ON" : "OFF");

    printf(" Variable MTRRs:\n");
    for (i = 0; i < num_var && i < 8; i++) {
        unsigned long blo = _rdmsr_lo(MSR_MTRR_PHYSBASE0 + i * 2);
        unsigned long mlo = _rdmsr_lo(MSR_MTRR_PHYSMASK0 + i * 2);
        int valid = (mlo >> 11) & 1;
        if (valid) {
            unsigned long base = blo & 0xFFFFF000UL;
            unsigned long mask = mlo & 0xFFFFF000UL;
            unsigned long sz = (~mask + 1) & 0xFFFFF000UL;
            printf("   [%d] base=0x%08lX size=%luMB type=%s\n",
                   i, base, sz >> 20, mtrr_type_name(blo));
        } else {
            printf("   [%d] <free>\n", i);
        }
    }

    /* PAT entries (shows current state, after our modification if any) */
    if (_cpuid1_edx() & (1UL << 16)) {
        pat_lo = _rdmsr_lo(MSR_PAT);
        pat_hi = _rdmsr_hi(MSR_PAT);
        printf(" PAT entries  : ");
        for (i = 0; i < 4; i++)
            printf("%d=%s ", i, mtrr_type_name((pat_lo >> (i * 8)) & 7));
        for (i = 0; i < 4; i++)
            printf("%d=%s ", i + 4, mtrr_type_name((pat_hi >> (i * 8)) & 7));
        printf("\n");
        if (g_pat_modified)
            printf(" PAT fix      : entry 3 changed UC->UC- for WC passthrough\n");
    } else {
        printf(" PAT          : not supported\n");
    }

    /* Page table info -- skip walk since CR3 physical addr not linearly mapped */
    if (lfb_va) {
        printf(" LFB VA       : 0x%08lX\n", lfb_va);
        if (!(cr0 & (1UL << 31)))
            printf(" PTE          : paging OFF, MTRR type is effective directly\n");
        else
            printf(" PTE          : paging ON (PCD=1,PWT=1 -> PAT idx 3)\n");
    }

    printf("==========================================\n");
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
#ifdef __DJGPP__
    return (unsigned char *)(((unsigned long)rmi.es << 4) + (rmi.ebp & 0xFFFF) + __djgpp_conventional_base);
#else
    return (unsigned char *)(((unsigned long)rmi.es << 4) + (rmi.ebp & 0xFFFF));
#endif
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

static unsigned char  g_sintab[256];         /* 256 B — always L1-hot       */
static unsigned char  g_xdist[WIDTH];        /* 1 KB — always L1-hot        */
static unsigned char  g_ydist[HEIGHT];       /* 768 B — always L1-hot       */
static unsigned char *g_dist  = NULL;        /* WIDTH*HEIGHT Euclidean dist  */

/* Benchmark results — filled by run_benchmark(), shown in summary */
static double g_bench_render_ms   = 0.0;
static double g_bench_blit_ms     = 0.0;
static double g_bench_combined_ms = 0.0;
static double g_rdtsc_mhz         = 0.0; /* calibrated CPU MHz via RDTSC */

static void init_plasma_tables(void)
{
    int x, y;
    double cx   = WIDTH  / 2.0;
    double cy   = HEIGHT / 2.0;
    double maxd = sqrt(cx*cx + cy*cy);
    int icx = WIDTH  / 2;
    int icy = HEIGHT / 2;

    for (x = 0; x < 256; x++)
        g_sintab[x] = (unsigned char)((sin(x * 6.283185307 / 256.0) + 1.0) * 127.5);

    /* Euclidean distance table (768 KB — primary radial wave) */
    for (y = 0; y < HEIGHT; y++) {
        double dy = y - cy;
        for (x = 0; x < WIDTH; x++) {
            double dx = x - cx;
            double d  = sqrt(dx*dx + dy*dy) / maxd * 255.0;
            g_dist[y * WIDTH + x] = (unsigned char)(d > 255.0 ? 255.0 : d);
        }
    }

    /* 1-D Chebyshev tables (2 KB total — kept for reference/future use) */
    for (x = 0; x < WIDTH;  x++)
        g_xdist[x] = (unsigned char)((abs(x - icx) * 255 + icx - 1) / icx);
    for (y = 0; y < HEIGHT; y++)
        g_ydist[y] = (unsigned char)((abs(y - icy) * 255 + icy - 1) / icy);
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
        int vy    = (int)g_sintab[((unsigned int)(y >> 1) + t1) & 0xFF];
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

#ifdef __DJGPP__
/*
 * WC-optimal memcpy using REP MOVSD (x86 fast-string path).
 *
 * With -O3, GCC inlines memcpy as SSE2 MOVDQA temporal stores which are
 * ~25% slower on WC memory than the CPU's fast-string microcode.  REP MOVSD
 * triggers the hardware fast-string engine (Pentium Pro+) which fills WC
 * buffers optimally.  This keeps blit at ~1000+ MB/s with -O3.
 *
 * count must be a multiple of 4 (always true for WIDTH=1024 scanlines).
 * (Watcom version is declared via #pragma aux in the Watcom section above.)
 */
static inline void wc_memcpy(void *dst, const void *src, unsigned long count)
{
    unsigned long dwords = count >> 2;
    __asm__ __volatile__ (
        "rep movsl"
        : "+D"(dst), "+S"(src), "+c"(dwords)
        :
        : "memory"
    );
}
#endif

/* Copy frame_buf (pitch=WIDTH) to LFB (pitch=lfb_pitch).
 * noinline + O2: prevents GCC -O3 from inlining and over-unrolling
 * the rep movsl loop, which adds startup overhead per rep invocation
 * that hurts WC throughput on Prescott/Pentium D. */
#ifdef __DJGPP__
static void __attribute__((noinline, optimize("O2,no-unroll-loops")))
blit_to_lfb(unsigned char *lfb, int lfb_pitch, const unsigned char *src)
{
    int y;
    if (lfb_pitch == WIDTH) {
        wc_memcpy(lfb, src, PIXELS);
    } else {
        for (y = 0; y < HEIGHT; y++)
            wc_memcpy(lfb + (unsigned long)y * lfb_pitch,
                       src + (unsigned long)y * WIDTH, WIDTH);
    }
}
#else
/*
 * Watcom version: calls wc_rep_movsd (#pragma aux) which emits a single
 * REP MOVSD to guarantee the hardware fast-string path for WC memory.
 * The dword count is precomputed so the compiler cannot confuse this
 * with memcpy and substitute its own intrinsic (which adds a redundant
 * REP MOVSB byte-tail and EDI save/restore).
 */
static void blit_to_lfb(unsigned char *lfb, int lfb_pitch,
                         const unsigned char *src)
{
    if (lfb_pitch == WIDTH) {
        wc_rep_movsd(lfb, src, PIXELS / 4);
    } else {
        int y;
        for (y = 0; y < HEIGHT; y++)
            wc_rep_movsd(lfb  + (unsigned long)y * lfb_pitch,
                          src + (unsigned long)y * WIDTH, WIDTH / 4);
    }
}
#endif

/* --------------------------------------------------------------------------
 * RDTSC helpers (Pentium+)
 * -------------------------------------------------------------------------- */

#ifdef __DJGPP__
static void rdtsc_read(unsigned long *lo, unsigned long *hi)
{
    unsigned long _lo, _hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(_lo), "=d"(_hi));
    *lo = _lo;
    *hi = _hi;
}
#else
static void rdtsc_read(unsigned long *lo, unsigned long *hi)
{
    unsigned long a, d;
    __asm {
        rdtsc
        mov [a], eax
        mov [d], edx
    }
    *lo = a;
    *hi = d;
}
#endif

/* Convert two (hi:lo) TSC snapshots to milliseconds using g_rdtsc_mhz */
static double tsc_to_ms(unsigned long lo1, unsigned long hi1,
                         unsigned long lo0, unsigned long hi0)
{
    double cycles = (double)hi1 * 4294967296.0 + (double)lo1
                  - (double)hi0 * 4294967296.0 - (double)lo0;
    return (g_rdtsc_mhz > 0.0) ? cycles / (g_rdtsc_mhz * 1000.0) : 0.0;
}

/* Calibrate RDTSC against DOS clock() (18.2 Hz reference).
 * Called in text mode before graphics.  Takes ~220ms. */
static void calibrate_rdtsc(void)
{
    /* Use BIOS tick counter at 0x46C (18.2065 Hz) directly — avoids any
     * CLOCKS_PER_SEC ambiguity in the C runtime.                          */
#ifdef __DJGPP__
    volatile unsigned long *bios_ticks = (volatile unsigned long *)(0x0046CUL + __djgpp_conventional_base);
#else
    volatile unsigned long *bios_ticks = (volatile unsigned long *)0x46CUL;
#endif
    unsigned long bt0, bt1;
    unsigned long lo0, hi0, lo1, hi1;
    double elapsed_s, cycles;

    /* Sync to a tick edge */
    bt0 = *bios_ticks;
    while (*bios_ticks == bt0) {}

    /* Measure over 4 BIOS ticks = exactly 4/18.2065 s ≈ 219.7 ms */
    rdtsc_read(&lo0, &hi0);
    bt0 = *bios_ticks;
    while ((bt1 = *bios_ticks) - bt0 < 4) {}
    rdtsc_read(&lo1, &hi1);

    elapsed_s = (double)(bt1 - bt0) / 18.2065;
    cycles    = (double)hi1 * 4294967296.0 + (double)lo1
              - (double)hi0 * 4294967296.0 - (double)lo0;
    g_rdtsc_mhz = (elapsed_s > 0.0) ? cycles / elapsed_s / 1.0e6 : 0.0;
}

/* --------------------------------------------------------------------------
 * Startup performance benchmark
 *
 * Runs before the demo loop to show where the time goes.
 * Called after LFB is mapped and MTRR WC is set up.
 * Measures render, blit, and render+blit in isolation.
 * -------------------------------------------------------------------------- */

#define BENCH_FRAMES 100

#ifdef __DJGPP__
static void __attribute__((noinline, optimize("O2,no-unroll-loops")))
run_benchmark(unsigned char *lfb, int lfb_pitch)
#else
static void run_benchmark(unsigned char *lfb, int lfb_pitch)
#endif
{
    unsigned long lo0, hi0, lo1, hi1;
    unsigned char *tmp;
    int      i;

    tmp = (unsigned char *)malloc(PIXELS);
    if (!tmp) return;
    memset(tmp, 0x80, PIXELS);

    /* Warm up */
    render_plasma(tmp, WIDTH, 0);
    blit_to_lfb(lfb, lfb_pitch, tmp);

    /* Render only */
    rdtsc_read(&lo0, &hi0);
    for (i = 0; i < BENCH_FRAMES; i++)
        render_plasma(tmp, WIDTH, (unsigned int)i * 3);
    rdtsc_read(&lo1, &hi1);
    g_bench_render_ms = tsc_to_ms(lo1, hi1, lo0, hi0) / BENCH_FRAMES;

    /* Blit only */
    rdtsc_read(&lo0, &hi0);
    for (i = 0; i < BENCH_FRAMES; i++)
        blit_to_lfb(lfb, lfb_pitch, tmp);
    rdtsc_read(&lo1, &hi1);
    g_bench_blit_ms = tsc_to_ms(lo1, hi1, lo0, hi0) / BENCH_FRAMES;

    /* Combined */
    rdtsc_read(&lo0, &hi0);
    for (i = 0; i < BENCH_FRAMES; i++) {
        render_plasma(tmp, WIDTH, (unsigned int)i * 3);
        blit_to_lfb(lfb, lfb_pitch, tmp);
    }
    rdtsc_read(&lo1, &hi1);
    g_bench_combined_ms = tsc_to_ms(lo1, hi1, lo0, hi0) / BENCH_FRAMES;

    free(tmp);
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
    unsigned char *frame_buf = NULL; /* system-RAM render buffer          */
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
#ifdef __DJGPP__
    bios_ticks = (volatile unsigned long *)(0x0046CUL + __djgpp_conventional_base);
#else
    bios_ticks = (volatile unsigned long *)0x46CUL;
#endif

    /* Parse command-line flags */
    for (i = 1; i < argc; i++) {
        if (stricmp(argv[i], "-vbe2") == 0 ||
            stricmp(argv[i], "/vbe2") == 0) {
            g_force_vbe2 = 1;
        }
        if (stricmp(argv[i], "-pmi") == 0 ||
            stricmp(argv[i], "/pmi") == 0) {
            g_no_pmi = 0;
        }
        if (stricmp(argv[i], "-nopmi") == 0 ||
            stricmp(argv[i], "/nopmi") == 0) {
            g_no_pmi = 1;
        }
        if (stricmp(argv[i], "-nomtrr") == 0 ||
            stricmp(argv[i], "/nomtrr") == 0) {
            g_no_mtrr = 1;
        }
        if (stricmp(argv[i], "-directvram") == 0 ||
            stricmp(argv[i], "/directvram") == 0) {
            g_direct_vram = 1;
        }
        if (stricmp(argv[i], "-hwflip") == 0 ||
            stricmp(argv[i], "/hwflip") == 0) {
            g_hw_flip = 1;
        }
        if (stricmp(argv[i], "-mtrrinfo") == 0 ||
            stricmp(argv[i], "/mtrrinfo") == 0) {
            g_mtrr_info = 1;
        }
        if (stricmp(argv[i], "-sched") == 0 ||
            stricmp(argv[i], "/sched") == 0) {
            g_sched_req = 1;
        }
    }

    /* Conventional DOS memory:
     *   512 B VBEInfo  + 256 B ModeInfo  + 1024 B palette = 1792 B
     *   128 paragraphs = 2048 B - enough with room to spare              */
#ifdef __DJGPP__
    /* Enable near pointer access for direct physical memory mapping */
    if (!__djgpp_nearptr_enable()) {
        printf("Failed to enable near pointer access\n");
        return 1;
    }
    /* nearptr_enable expands DS/ES/SS to 4 GB but leaves CS at program
     * size.  PMI calls jump into BIOS ROM code outside our binary, so
     * expand CS to cover the full 4 GB address space as well.
     * Try DPMI 0008h first (works on PMODE/DJ), fall back to direct
     * GDT/LDT patching (needed for CWSDPR0). */
    if ((_get_cs() & 3) == 0) {
        if (expand_cs_to_4gb()) {
            printf("[0] CS limit expanded to 4 GB\n");
        } else {
            printf("Warning: could not expand CS limit (PMI may not work)\n");
        }
    }
    printf("[0] DJGPP nearptr enabled, SSE init...\n");
    djgpp_enable_sse();
#endif
    if (!dpmi_alloc_dos(128)) {
        printf("DPMI: cannot allocate conventional memory\n");
        return 1;
    }

    /* Calibrate RDTSC frequency (~220ms, text mode, safe) */
    printf("Calibrating CPU frequency...\n");
    calibrate_rdtsc();
    printf("CPU: %.0f MHz\n", g_rdtsc_mhz);

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

    /* VBE 3.0: query Protected Mode Interface
     * PMI code does direct port I/O — requires ring 0 (PMODE/W). */
    if (g_vbe3 && !g_no_pmi) {
        if ((_get_cs() & 3) != 0) {
            printf("PMI        : skipped (ring 3 — needs ring 0 for direct I/O)\n");
        } else {
            g_pmi_ok = query_pmi();
            if (g_pmi_ok) {
                unsigned long entry = (unsigned long)g_pmi_rm_seg * 16
                                    + g_pmi_rm_off + g_pmi_setds_off;
                printf("PMI        : available, entry at 0x%08lX (linear)\n", entry);
            } else {
                printf("PMI        : not available\n");
            }
        }
    } else if (g_no_pmi) {
        printf("PMI        : disabled (use -pmi to enable)\n");
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

#ifdef __DJGPP__
        src = (unsigned short *)(ml_seg * 16 + ml_off + __djgpp_conventional_base);
#else
        src = (unsigned short *)(ml_seg * 16 + ml_off);
#endif
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

    /* ---- Check VRAM for page-flipping --------------------------------- */
    page_size     = (unsigned long)lfb_pitch * HEIGHT;
    use_doublebuf = 0;
    back_page     = 0;

    {
        unsigned long total_vram = (unsigned long)vbi.total_memory * 65536UL;
        printf("VRAM: %luKB  page: %luKB  ",
               total_vram / 1024UL, page_size / 1024UL);
        if (total_vram >= page_size * 2UL) {
            /* sysram render + 2 VRAM pages = triple-buffer */
            printf("-> triple-buffer candidate (sysram + 2x VRAM)\n");
            use_doublebuf = 1;   /* confirmed after 4F07h test below    */
        } else {
            /* sysram render + 1 VRAM page = double-buffer */
            printf("-> double-buffer (sysram + 1x VRAM, insufficient for page flip)\n");
        }
    }

    /* ---- Allocate system-RAM frame buffer (skipped with -directvram) --- */
    /*
     * Rendering plasma directly into VRAM is extremely slow because LFB
     * writes are uncached (UC) or write-combining (WC).  Individual byte
     * writes at ~100ns each × 786K pixels ≈ 78ms/frame → ~13 FPS.
     * Instead, render into fast cached system RAM, then blit to VRAM in
     * one sequential burst (WC-friendly memcpy).
     * Use -directvram to measure the difference on real hardware.
     */
    if (!g_direct_vram) {
        frame_buf = (unsigned char *)malloc(PIXELS);
        if (!frame_buf) {
            free(g_dist);
            dpmi_free_dos();
            printf("Out of memory (frame buffer)\n");
            return 1;
        }
    }
    printf("Render     : %s\n", g_direct_vram ? "direct-to-VRAM (-directvram)" : "system-RAM + blit");

    /* ---- Allocate distance table --------------------------------------- */
    g_dist = (unsigned char *)malloc(PIXELS);
    if (!g_dist) {
        dpmi_free_dos();
        printf("Out of memory (distance table)\n");
        return 1;
    }

    init_plasma_tables();

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

    /* ---- MTRR Write-Combining for LFB --------------------------------- */
    if (!g_no_mtrr) {
        unsigned long total_vram = (unsigned long)vbi.total_memory * 65536UL;
        int wc = setup_mtrr_wc(lfb_phys, total_vram);
        if (wc == 1) {
            if (g_mtrr_replaced_uc)
                printf("MTRR WC    : enabled (slot %d, replaced BIOS UC, %luMB at 0x%08lX)\n",
                       g_mtrr_slot, next_pow2(total_vram) >> 20, lfb_phys);
            else
                printf("MTRR WC    : enabled (slot %d, new entry, %luMB at 0x%08lX)\n",
                       g_mtrr_slot, next_pow2(total_vram) >> 20, lfb_phys);
        } else if (wc == -1)
            printf("MTRR WC    : already active (BIOS/chipset)\n");
        else if ((_get_cs() & 3) != 0)
            printf("MTRR WC    : skipped (ring 3 — use CWSDPR0 for ring 0)\n");
        else
            printf("MTRR WC    : not available\n");
    } else {
        printf("MTRR WC    : disabled by -nomtrr\n");
    }

    /* ---- PAT fix: entry 3 UC->UC- for WC passthrough ------------------- */
    if (g_mtrr_wc >= 1) {
        int pat_ok = setup_pat_uc_minus();
        if (pat_ok == 1)
            printf("PAT fix    : entry 3 UC->UC- (WC passthrough enabled)\n");
        else if (pat_ok == -1)
            printf("PAT fix    : not needed (entry 3 already UC- or WC)\n");
        else
            printf("PAT fix    : not available\n");
    }

    /* ---- MTRR diagnostic dump (with -mtrrinfo flag) -------------------- */
    if (g_mtrr_info) {
        unsigned long map_size = use_doublebuf ? page_size * 2UL : page_size;
        dump_mtrr_info((unsigned long)lfb, map_size);
    }

    /* ---- Brief mode set to test page-flip features --------------------- */
    if (!vbe_set_mode(target_mode)) {
        dpmi_unmap_physical(lfb);
        if (frame_buf) free(frame_buf);
        free(g_dist);
        dpmi_free_dos();
        printf("Set mode 0x%03X failed\n", target_mode);
        return 1;
    }

    /* Test DAC width (needs VBE mode active) */
    init_dac();

    /* Test VBE 4F07h page-flip support (needs VBE mode active) */
    if (use_doublebuf) {
        if (!vbe_set_display_start(0, 0, 0)) {
            use_doublebuf = 0;
        } else {
            /* Test PMI SetDisplayStart if available */
            if (g_pmi_ok) {
                if (!pmi_set_display_start(0, 0, 0)) {
                    g_pmi_ok = 0;
                    vbe_set_display_start(0, 0, 0);
                }
            }
            /* Test VBE 3.0 scheduled flip (BL=02h).
             * Opt-in via -sched: ATI ATOMBIOS crashes (exception 0Dh)
             * on unsupported BL=02h subfunction. */
            if (g_vbe3 && g_sched_req) {
                int sched_ok;
                if (g_pmi_ok)
                    sched_ok = pmi_schedule_display_start(0,
                                   (unsigned short)HEIGHT);
                else
                    sched_ok = vbe_schedule_display_start(0,
                                   (unsigned short)HEIGHT);
                if (sched_ok) {
                    g_sched_flip = 1;
                    /* Wait for test flip, then reset to page 0 */
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
    }

    /* Test direct GPU register access for -hwflip */
    if (g_hw_flip && use_doublebuf) {
        if ((_get_cs() & 3) != 0) {
            printf("HW flip    : skipped (ring 3 — use CWSDPR0 for ring 0)\n");
            g_hw_flip = 0;
        } else if (!detect_gpu_iobase(lfb_phys)) {
            printf("HW flip    : GPU I/O base not found\n");
            g_hw_flip = 0;
        } else {
            /* Verify by reading D1GRPH_PRIMARY_SURFACE_ADDRESS.
             * After mode set, it should contain our LFB physical base. */
            unsigned long cur = gpu_reg_read(R5_D1GRPH_PRIMARY_SURFACE_ADDRESS);
            if ((cur & 0xFFF00000UL) != (lfb_phys & 0xFFF00000UL)) {
                printf("HW flip    : verification failed "
                       "(D1GRPH=0x%08lX, LFB=0x%08lX)\n", cur, lfb_phys);
                g_hw_flip = 0;
            } else {
                g_fb_phys = lfb_phys;
                printf("HW flip    : active (I/O=0x%04X, "
                       "D1GRPH=0x%08lX)\n",
                       g_gpu_iobase, cur);
            }
        }
    } else if (g_hw_flip) {
        printf("HW flip    : disabled (no page flip support)\n");
        g_hw_flip = 0;
    }

    /* ---- Startup benchmark (while in graphics mode — safe VRAM writes) - */
    run_benchmark(lfb, lfb_pitch);

    /* Return to text mode for feature summary */
    vbe_set_text_mode();

    /* ---- Feature Summary ----------------------------------------------- */
    {
        const char *ring_str  = ((_get_cs() & 3) == 0) ?
                                "0 (ring 0)" : "3 (ring 3 — use CWSDPR0)";
        const char *pmi_str, *mtrr_str, *flip_str;
        const char *sched_str, *buf_str, *render_str;
        const char *hwflip_str;

        if (g_pmi_ok)
            pmi_str = "available";
        else if (g_no_pmi)
            pmi_str = "disabled (use -pmi to enable)";
        else if (g_force_vbe2)
            pmi_str = "disabled (-vbe2)";
        else if ((_get_cs() & 3) != 0)
            pmi_str = "skipped (ring 3)";
        else
            pmi_str = "not available";

        if (g_mtrr_wc == 1)
            mtrr_str = g_mtrr_replaced_uc ? "enabled (replaced BIOS UC)"
                                           : "enabled (new slot)";
        else if (g_mtrr_wc == 2)
            mtrr_str = "already active (BIOS)";
        else if (g_no_mtrr)
            mtrr_str = "disabled (-nomtrr)";
        else if ((_get_cs() & 3) != 0)
            mtrr_str = "skipped (ring 3 — use CWSDPR0 for ring 0)";
        else
            mtrr_str = "not available";

        flip_str = use_doublebuf ? "yes (4F07h)" : "no";

        if (g_sched_flip)
            sched_str = "yes (BL=02h)";
        else if (!g_vbe3)
            sched_str = "n/a (VBE 2.0)";
        else if (!use_doublebuf)
            sched_str = "n/a (no page flip)";
        else
            sched_str = "not supported";

        if (g_hw_flip)
            hwflip_str = "active (GPU register + update lock)";
        else
            hwflip_str = "off (use -hwflip to enable)";

        if (g_direct_vram)
            buf_str = use_doublebuf ? "double (2x VRAM)" :
                                      "single (1x VRAM)";
        else
            buf_str = use_doublebuf ? "triple (sysram + 2x VRAM)" :
                                      "double (sysram + 1x VRAM)";

        render_str = g_direct_vram ? "direct-to-VRAM (-directvram)" :
                                     "system-RAM + blit";

        printf("\n");
        printf("==========================================\n");
        printf(" PLASMA - Feature Summary\n");
        printf("==========================================\n");
        printf(" CPU          : %.0f MHz%s\n", g_rdtsc_mhz,
               (g_rdtsc_mhz < 100.0 || g_rdtsc_mhz > 10000.0)
                   ? " (emulator - timing unreliable)" : "");
        printf(" VBE version  : %d.%d%s\n",
               g_vbe_version >> 8, g_vbe_version & 0xFF,
               g_force_vbe2 ? " (forced VBE 2.0)" : "");
        printf(" Video memory : %u KB\n", (unsigned)vbi.total_memory * 64);
        printf(" Mode         : 0x%03X  %dx%d %dbpp\n",
               target_mode, WIDTH, HEIGHT, 8);
        printf(" LFB          : 0x%08lX  pitch=%d\n",
               lfb_phys, lfb_pitch);
        printf(" DAC          : %d-bit\n", g_dac_bits);
        printf(" Ring         : %s\n", ring_str);
        printf(" PMI          : %s\n", pmi_str);
        printf(" MTRR WC      : %s\n", mtrr_str);
        printf(" PAT fix      : %s\n",
               g_pat_modified ? "entry 3 UC->UC- (WC passthrough)" :
               g_mtrr_wc ? "not needed" : "n/a");
        printf(" Page flip    : %s\n", flip_str);
        printf(" Sched flip   : %s\n", sched_str);
        printf(" HW flip      : %s\n", hwflip_str);
        printf(" Buffering    : %s\n", buf_str);
        printf(" Render       : %s\n", render_str);
        printf("==========================================\n");
        /* Benchmark results */
        if (g_bench_combined_ms > 0.0) {
            double r_fps  = g_bench_render_ms   > 0.0 ? 1000.0 / g_bench_render_ms   : 0.0;
            double b_fps  = g_bench_blit_ms     > 0.0 ? 1000.0 / g_bench_blit_ms     : 0.0;
            double c_fps  = g_bench_combined_ms > 0.0 ? 1000.0 / g_bench_combined_ms : 0.0;
            double r_pct  = g_bench_render_ms   / g_bench_combined_ms * 100.0;
            double b_pct  = g_bench_blit_ms     / g_bench_combined_ms * 100.0;
            unsigned long blit_mbs = g_bench_blit_ms > 0.0
                ? (unsigned long)(PIXELS / 1024.0 / 1024.0 / (g_bench_blit_ms / 1000.0))
                : 0UL;
            const char *bn = (r_pct >= 60.0) ? "RENDER" :
                             (b_pct >= 60.0) ? "BLIT"   : "balanced";
            printf("==========================================\n");
            printf(" Benchmark (%d frames, sysram+blit)\n", BENCH_FRAMES);
            printf("  Render : %8.3f ms  %7.2f FPS\n",
                   g_bench_render_ms, r_fps);
            if (g_bench_blit_ms > 0.0)
                printf("  Blit   : %8.3f ms  %7.2f FPS  %lu MB/s\n",
                       g_bench_blit_ms, b_fps, blit_mbs);
            else
                printf("  Blit   : <0.001 ms  (sub-us, fast emulator RAM)\n");
            printf("  Total  : %8.3f ms  %7.2f FPS  bottleneck: %s\n",
                   g_bench_combined_ms, c_fps, bn);
            printf("  Share  : render %3.0f%%  blit %3.0f%%\n",
                   r_pct > 0.0 ? r_pct : 0.0,
                   b_pct > 0.0 ? b_pct : 0.0);
        }
        printf("==========================================\n");
        printf(" Controls: [V] toggle vsync  [ESC] quit\n");
        printf("==========================================\n");
        printf("\n Press any key to start...");
    }
    getch();

    /* ---- Set video mode (for the demo) --------------------------------- */
    if (!vbe_set_mode(target_mode)) {
        dpmi_unmap_physical(lfb);
        if (frame_buf) free(frame_buf);
        free(g_dist);
        dpmi_free_dos();
        return 1;
    }

    /* Re-initialize DAC and display start for the demo */
    init_dac();
    if (use_doublebuf) {
        vbe_set_display_start(0, 0, 0);
        back_page = 1;
    }

    /* ---- Palette ------------------------------------------------------- */
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

        /* --- Render plasma ------------------------------------------------- */
        /* With -directvram: render straight into VRAM (slow, for comparison).
         * Normal:           render into system RAM, then blit to VRAM.       */
        if (g_direct_vram) {
            unsigned char *dst = lfb + (use_doublebuf ?
                                  (unsigned long)back_page * page_size : 0UL);
            if (vsync_on) wait_vsync();
            render_plasma(dst, lfb_pitch, t);
            /* No HUD in direct-VRAM mode (would need a scratch buffer) */
            if (use_doublebuf) {
                if (g_hw_flip)
                    hw_page_flip(g_fb_phys +
                        (unsigned long)back_page * page_size);
                else if (g_pmi_ok)
                    pmi_set_display_start(0,
                        (unsigned short)(back_page * HEIGHT), 0);
                else
                    vbe_set_display_start(0,
                        (unsigned short)(back_page * HEIGHT), 0);
                back_page = 1 - back_page;
            }
        } else {
            render_plasma(frame_buf, WIDTH, t);

            /* --- HUD overlay (into system RAM) -------------------------- */
            {
                const char *sync_str = vsync_on ?
                    (g_sched_flip ? "SCH" : "ON ") : "OFF";
                const char *buf_str  = g_direct_vram ?
                                       (use_doublebuf ? "DBL" : "SGL") :
                                       (use_doublebuf ? "TRP" : "DBL");
                const char *pmi_str  = g_pmi_ok ? "YES" : "NO ";
                const char *wc_str   = g_mtrr_wc ? "YES" : "NO ";
                const char *hwf_str  = g_hw_flip ? "YES" : "NO ";
                sprintf(msg, "VSYNC:%s BUF:%s PMI:%s HWF:%s WC:%s FPS:%5.1f  [V] [ESC]",
                        sync_str, buf_str, pmi_str, hwf_str, wc_str, fps);
                draw_str_bg(frame_buf, WIDTH, 4, 4, msg, 255, 0, font);
            }

            if (vsync_on && g_hw_flip) {
                draw_str_2x(frame_buf, WIDTH, 4, 16,
                            "HW FLIP - GPU locked, tear-free  ",
                            160, 0, font);
            } else if (!vsync_on && g_hw_flip) {
                draw_str_2x(frame_buf, WIDTH, 4, 16,
                            "HW FLIP - tear-free, no throttle ",
                            160, 0, font);
            } else if (vsync_on && g_sched_flip) {
                draw_str_2x(frame_buf, WIDTH, 4, 16,
                            "VSYNC SCHED - non-blocking flip  ",
                            180, 0, font);
            } else if (vsync_on) {
                draw_str_2x(frame_buf, WIDTH, 4, 16,
                            "VSYNC ON  - tearing suppressed   ",
                            200, 0, font);
            } else {
                draw_str_2x(frame_buf, WIDTH, 4, 16,
                            "VSYNC OFF - watch for tear line! ",
                            240, 0, font);
            }

            /* --- Present frame ------------------------------------------ */
            if (use_doublebuf) {
                if (g_hw_flip) {
                    /* Direct GPU register flip with update lock.
                     * The lock buffers the surface address write; on
                     * unlock the hardware applies it atomically at the
                     * next vsync — guaranteed tear-free, non-blocking.
                     *
                     * For vsync throttling we wait for the pending bit
                     * to clear (i.e. hardware applied previous flip). */
                    if (vsync_on)
                        while (!hw_is_flip_done()) {}

                    blit_to_lfb(lfb + (unsigned long)back_page * page_size,
                                lfb_pitch, frame_buf);

                    hw_page_flip(g_fb_phys +
                        (unsigned long)back_page * page_size);

                } else if (g_sched_flip) {
                    /* Wait for previous scheduled flip to complete before
                     * overwriting the back page that may still be displayed. */
                    if (g_pmi_ok) {
                        while (!pmi_is_flip_complete()) {}
                    } else {
                        while (!vbe_is_flip_complete()) {}
                    }

                    blit_to_lfb(lfb + (unsigned long)back_page * page_size,
                                lfb_pitch, frame_buf);

                    if (vsync_on) {
                        /* VBE 3.0 non-blocking scheduled flip */
                        if (g_pmi_ok)
                            pmi_schedule_display_start(0,
                                (unsigned short)(back_page * HEIGHT));
                        else
                            vbe_schedule_display_start(0,
                                (unsigned short)(back_page * HEIGHT));
                    } else {
                        if (g_pmi_ok)
                            pmi_set_display_start(0,
                                (unsigned short)(back_page * HEIGHT), 0);
                        else
                            vbe_set_display_start(0,
                                (unsigned short)(back_page * HEIGHT), 0);
                    }
                } else {
                    /* Standard BIOS flip: always BL=00h (immediate).
                     *
                     * BL=80h (BIOS-managed vsync wait) is unreliable on
                     * modern GPUs: ATI ATOMBIOS writes registers AFTER
                     * blanking ends, causing tearing at the top of screen.
                     *
                     * For vsync throttling we wait_vsync() AFTER the flip. */
                    blit_to_lfb(lfb + (unsigned long)back_page * page_size,
                                lfb_pitch, frame_buf);

                    if (g_pmi_ok)
                        pmi_set_display_start(0,
                            (unsigned short)(back_page * HEIGHT), 0);
                    else
                        vbe_set_display_start(0,
                            (unsigned short)(back_page * HEIGHT), 0);
                    if (vsync_on)
                        wait_vsync();
                }
                back_page = 1 - back_page;
            } else {
                if (vsync_on)
                    wait_vsync();
                blit_to_lfb(lfb, lfb_pitch, frame_buf);
            }
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
    restore_pat();
    restore_mtrr();
    dpmi_unmap_physical(lfb);
    if (frame_buf) free(frame_buf);
    free(g_dist);
    dpmi_free_dos();
#ifdef __DJGPP__
    __djgpp_nearptr_disable();
#endif

    printf("PLASMA done.  mode=0x%03X  DAC=%d-bit  buf:%s  PMI:%s  HWF:%s  WC:%s  PAT:%s  sched:%s  render:%s\n",
           target_mode, g_dac_bits,
           g_direct_vram ? (use_doublebuf ? "double" : "single") :
                           (use_doublebuf ? "triple" : "double"),
           g_pmi_ok ? "yes" : "no",
           g_hw_flip ? "yes" : "no",
           g_mtrr_wc == 1 ? "mtrr" : g_mtrr_wc == 2 ? "bios" : "no",
           g_pat_modified ? "fix" : "no",
           g_sched_flip ? "yes" : "no",
           g_direct_vram ? "direct" : "sysram");
    return 0;
}
