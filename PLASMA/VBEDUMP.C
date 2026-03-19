/*
 * VBEDUMP.C — VESA BIOS dumper for offline analysis
 * OpenWatcom 2.0, 32-bit DOS, PMODE/W (ring 0)
 *
 * Dumps everything needed to validate VBE 3.0 support:
 *   - VBE controller info with full capability decode
 *   - All video modes with detailed attribute bits
 *   - VBE function support probe (4F00h–4F15h)
 *   - VBE 4F07h sub-function test (in-mode, with actual page flip)
 *   - VBE 3.0 PMI table, entry points, I/O port list, code hex dump
 *   - PCI ROM header decode (vendor/device ID)
 *   - Full video BIOS ROM binary
 *
 * Output files:
 *   VBEDUMP.TXT  — human-readable report
 *   VBEINFO.BIN  — raw 512-byte VBE controller info block
 *   VBEMODES.BIN — mode records (2-byte mode# + 256-byte info each)
 *   VBEPMI.BIN   — VBE 3.0 PMI table and code (if available)
 *   VBEROM.BIN   — video BIOS ROM (from C0000h)
 *
 * Build:
 *   wcc386 -bt=dos -3r -ox -s -zq VBEDUMP.C
 *   wlink system pmodew name VBEDUMP file VBEDUMP option quiet
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <conio.h>
#include <dos.h>
#include <i86.h>

/* --------------------------------------------------------------------------
 * Constants
 * -------------------------------------------------------------------------- */

#define TEST_WIDTH   1024
#define TEST_HEIGHT  768

/* --------------------------------------------------------------------------
 * Packed structures (VBE spec layout)
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
    /* VBE 1.0+ */
    unsigned short mode_attributes;         /* 0x00 */
    unsigned char  win_a_attributes;        /* 0x02 */
    unsigned char  win_b_attributes;        /* 0x03 */
    unsigned short win_granularity;         /* 0x04 */
    unsigned short win_size;                /* 0x06 */
    unsigned short win_a_segment;           /* 0x08 */
    unsigned short win_b_segment;           /* 0x0A */
    unsigned long  win_func_ptr;            /* 0x0C */
    unsigned short bytes_per_scan_line;     /* 0x10 */
    /* VBE 1.2+ */
    unsigned short x_resolution;            /* 0x12 */
    unsigned short y_resolution;            /* 0x14 */
    unsigned char  x_char_size;             /* 0x16 */
    unsigned char  y_char_size;             /* 0x17 */
    unsigned char  number_of_planes;        /* 0x18 */
    unsigned char  bits_per_pixel;          /* 0x19 */
    unsigned char  number_of_banks;         /* 0x1A */
    unsigned char  memory_model;            /* 0x1B */
    unsigned char  bank_size;               /* 0x1C */
    unsigned char  number_of_image_pages;   /* 0x1D */
    unsigned char  reserved1;               /* 0x1E */
    /* Direct Color fields */
    unsigned char  red_mask_size;           /* 0x1F */
    unsigned char  red_field_position;      /* 0x20 */
    unsigned char  green_mask_size;         /* 0x21 */
    unsigned char  green_field_position;    /* 0x22 */
    unsigned char  blue_mask_size;          /* 0x23 */
    unsigned char  blue_field_position;     /* 0x24 */
    unsigned char  rsvd_mask_size;          /* 0x25 */
    unsigned char  rsvd_field_position;     /* 0x26 */
    unsigned char  direct_color_info;       /* 0x27 */
    /* VBE 2.0+ */
    unsigned long  phys_base_ptr;           /* 0x28 */
    unsigned long  off_screen_mem_off;      /* 0x2C */
    unsigned short off_screen_mem_size;     /* 0x30 */
    /* VBE 3.0+ */
    unsigned short lin_bytes_per_scan_line; /* 0x32 */
    unsigned char  bnk_number_of_pages;     /* 0x34 */
    unsigned char  lin_number_of_pages;     /* 0x35 */
    unsigned char  lin_red_mask_size;       /* 0x36 */
    unsigned char  lin_red_field_pos;       /* 0x37 */
    unsigned char  lin_green_mask_size;     /* 0x38 */
    unsigned char  lin_green_field_pos;     /* 0x39 */
    unsigned char  lin_blue_mask_size;      /* 0x3A */
    unsigned char  lin_blue_field_pos;      /* 0x3B */
    unsigned char  lin_rsvd_mask_size;      /* 0x3C */
    unsigned char  lin_rsvd_field_pos;      /* 0x3D */
    unsigned long  max_pixel_clock;         /* 0x3E */
    unsigned char  reserved4[190];          /* 0x42 */
} VBEModeInfo;                              /* 256 bytes */

/* DPMI real-mode interrupt register structure */
typedef struct {
    unsigned long  edi, esi, ebp, reserved_zero;
    unsigned long  ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs;
    unsigned short ip, cs, sp, ss;
} RMI;

#pragma pack()

/* --------------------------------------------------------------------------
 * DPMI helpers
 * -------------------------------------------------------------------------- */

static unsigned short g_dos_seg = 0;
static unsigned short g_dos_sel = 0;

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

/* --------------------------------------------------------------------------
 * Logging — dual output to stdout and VBEDUMP.TXT
 * -------------------------------------------------------------------------- */

static FILE *g_log = NULL;

static void tprintf(const char *fmt, ...)
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

/* --------------------------------------------------------------------------
 * Utility functions
 * -------------------------------------------------------------------------- */

static const char *rm_string(unsigned long far_ptr)
{
    static char buf[256];
    unsigned long seg, off;
    const char *src;
    int i;
    if (far_ptr == 0) return "(null)";
    seg = (far_ptr >> 16) & 0xFFFF;
    off = far_ptr & 0xFFFF;
    src = (const char *)(seg * 16 + off);
    for (i = 0; i < 255 && src[i]; i++)
        buf[i] = src[i];
    buf[i] = 0;
    return buf;
}

static int save_bin(const char *fn, const void *data, unsigned long size)
{
    FILE *f = fopen(fn, "wb");
    if (!f) return 0;
    fwrite(data, 1, (size_t)size, f);
    fclose(f);
    return 1;
}

static void hex_dump(const unsigned char *data, unsigned long base,
                     unsigned long size, unsigned long max_bytes)
{
    unsigned long i, j;
    if (max_bytes && size > max_bytes) size = max_bytes;
    for (i = 0; i < size; i += 16) {
        tprintf("  %08lX:", base + i);
        for (j = 0; j < 16 && i + j < size; j++)
            tprintf(" %02X", data[i + j]);
        for (; j < 16; j++)
            tprintf("   ");
        tprintf("  ");
        for (j = 0; j < 16 && i + j < size; j++) {
            unsigned char c = data[i + j];
            tprintf("%c", (c >= 0x20 && c < 0x7F) ? c : '.');
        }
        tprintf("\n");
    }
}

static const char *mem_model_str(unsigned char m)
{
    switch (m) {
        case 0: return "Text";
        case 1: return "CGA";
        case 2: return "Hercules";
        case 3: return "Planar";
        case 4: return "PackedPx";
        case 5: return "Non-ch4";
        case 6: return "DirectClr";
        case 7: return "YUV";
        default: return "OEM";
    }
}

static const char *vbe_status_str(int ax)
{
    if (ax == 0x004F) return "OK";
    if (ax == 0x014F) return "FAIL";
    if (ax == 0x024F) return "NOT_SUP_HW";
    if (ax == 0x034F) return "INVALID";
    if (ax == -1)     return "DPMI_ERR";
    return "UNKNOWN";
}

/* --------------------------------------------------------------------------
 * VBE call wrappers
 * -------------------------------------------------------------------------- */

static int vbe_get_info(VBEInfo *out)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    memset(dos_buf(), 0, 512);
    memcpy(dos_buf(), "VBE2", 4);
    rmi.eax = 0x4F00;
    rmi.es  = g_dos_seg;
    rmi.edi = 0;
    if (!dpmi_real_int(0x10, &rmi)) return 0;
    if ((rmi.eax & 0xFFFF) != 0x004F) return 0;
    memcpy(out, dos_buf(), sizeof(VBEInfo));
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
    rmi.ebx = (unsigned long)mode | 0x4000;
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

/* Raw 4F07h call — returns AX in low word, BX in high word of result */
static unsigned long vbe_raw_4f07(unsigned short bl,
                                  unsigned short cx,
                                  unsigned short dx)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F07;
    rmi.ebx = (unsigned long)bl;
    rmi.ecx = (unsigned long)cx;
    rmi.edx = (unsigned long)dx;
    if (!dpmi_real_int(0x10, &rmi))
        return 0xFFFFUL;   /* DPMI error */
    return (rmi.eax & 0xFFFF) | ((rmi.ebx & 0xFFFF) << 16);
}

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */

int main(void)
{
    VBEInfo     vbi;
    VBEModeInfo vbmi;
    unsigned short mode_list[512];
    int            mode_count, i;
    unsigned short test_mode;
    unsigned long  test_pages;
    FILE          *mf;
    unsigned char *rom;
    unsigned long  rom_size;

    printf("VBEDUMP - VESA BIOS Dumper v1.0\n\n");

    g_log = fopen("VBEDUMP.TXT", "w");
    if (!g_log)
        printf("Warning: cannot create VBEDUMP.TXT\n");

    tprintf("VBEDUMP - VESA BIOS Dump Report\n");
    tprintf("===============================\n\n");

    if (!dpmi_alloc_dos(128)) {
        tprintf("ERROR: cannot allocate conventional memory\n");
        if (g_log) fclose(g_log);
        return 1;
    }

    /* ==================================================================
     * 1. VBE Controller Info (4F00h)
     * ================================================================== */
    tprintf("=== 1. VBE Controller Info (4F00h) ===\n\n");

    if (!vbe_get_info(&vbi)) {
        tprintf("ERROR: VBE info call failed\n");
        dpmi_free_dos();
        if (g_log) fclose(g_log);
        return 1;
    }
    save_bin("VBEINFO.BIN", &vbi, 512);
    tprintf("Saved: VBEINFO.BIN (512 bytes)\n\n");

    tprintf("Signature      : %.4s\n", vbi.vbe_signature);
    tprintf("Version        : %d.%d (0x%04X)\n",
            vbi.vbe_version >> 8, vbi.vbe_version & 0xFF,
            vbi.vbe_version);
    tprintf("OEM String     : %s\n", rm_string(vbi.oem_string_ptr));
    tprintf("OEM Vendor     : %s\n", rm_string(vbi.oem_vendor_name_ptr));
    tprintf("OEM Product    : %s\n", rm_string(vbi.oem_product_name_ptr));
    tprintf("OEM Revision   : %s\n", rm_string(vbi.oem_product_rev_ptr));
    tprintf("OEM SW Rev     : %d.%d\n",
            vbi.oem_software_rev >> 8, vbi.oem_software_rev & 0xFF);
    tprintf("Total Memory   : %u x 64KB = %u KB\n",
            (unsigned)vbi.total_memory, (unsigned)vbi.total_memory * 64);
    tprintf("Capabilities   : 0x%08lX\n", vbi.capabilities);
    tprintf("  [0] DAC width   : %s\n",
            (vbi.capabilities & 1) ? "switchable 6/8-bit" : "fixed 6-bit");
    tprintf("  [1] Controller  : %s\n",
            (vbi.capabilities & 2) ? "not VGA compatible" : "VGA compatible");
    tprintf("  [2] RAMDAC blank: %s\n",
            (vbi.capabilities & 4) ? "use VBE blank fn" : "normal blank");
    tprintf("  [3] HW stereo   : %s\n",
            (vbi.capabilities & 8) ? "yes (VESA EVC)" : "no");
    tprintf("  [4] Stereo EVC  : %s\n",
            (vbi.capabilities & 16) ? "via external connector" : "via mini-DIN");

    tprintf("\nRaw VBE Info (first 64 bytes):\n");
    hex_dump((unsigned char *)&vbi, 0, 64, 64);
    tprintf("\n");

    /* ==================================================================
     * 2. Mode Enumeration
     * ================================================================== */
    tprintf("=== 2. Mode Enumeration ===\n\n");
    {
        unsigned long ml_seg = (vbi.video_mode_ptr >> 16) & 0xFFFF;
        unsigned long ml_off = vbi.video_mode_ptr & 0xFFFF;
        unsigned short *src = (unsigned short *)(ml_seg * 16 + ml_off);
        for (mode_count = 0; mode_count < 511 && src[mode_count] != 0xFFFF;
             mode_count++)
            mode_list[mode_count] = src[mode_count];
        mode_list[mode_count] = 0xFFFF;
    }
    tprintf("Total modes: %d\n\n", mode_count);

    /* Summary table */
    tprintf("Mode   Res        BPP Model     LFB PhysAddr   Pg Pitch");
    if (vbi.vbe_version >= 0x0300)
        tprintf(" LPitch MaxClk");
    tprintf("\n");
    tprintf("------ ---------- --- --------- --- ---------- -- -----");
    if (vbi.vbe_version >= 0x0300)
        tprintf(" ------ ----------");
    tprintf("\n");

    test_mode = 0xFFFF;
    test_pages = 0;
    mf = fopen("VBEMODES.BIN", "wb");

    for (i = 0; i < mode_count; i++) {
        if (!vbe_get_mode_info(mode_list[i], &vbmi)) {
            tprintf("0x%03X  (query failed)\n", mode_list[i]);
            continue;
        }
        if (mf) {
            fwrite(&mode_list[i], 2, 1, mf);
            fwrite(&vbmi, 256, 1, mf);
        }

        tprintf("0x%03X %4dx%-4d %3d %-9s %s  0x%08lX %2d %5d",
                mode_list[i],
                vbmi.x_resolution, vbmi.y_resolution,
                vbmi.bits_per_pixel,
                mem_model_str(vbmi.memory_model),
                (vbmi.mode_attributes & 0x80) ? "yes" : " no",
                vbmi.phys_base_ptr,
                vbmi.number_of_image_pages + 1,
                vbmi.bytes_per_scan_line);
        if (vbi.vbe_version >= 0x0300)
            tprintf(" %6d %10lu",
                    vbmi.lin_bytes_per_scan_line,
                    vbmi.max_pixel_clock);
        tprintf("\n");

        /* Remember 1024x768 8bpp LFB mode for in-mode test */
        if (test_mode == 0xFFFF &&
            vbmi.x_resolution == TEST_WIDTH &&
            vbmi.y_resolution == TEST_HEIGHT &&
            vbmi.bits_per_pixel == 8 &&
            (vbmi.mode_attributes & 0x80) &&
            vbmi.phys_base_ptr) {
            test_mode = mode_list[i];
            test_pages = vbmi.number_of_image_pages + 1;
        }
    }
    if (mf) fclose(mf);
    tprintf("\nSaved: VBEMODES.BIN (%d modes)\n\n", mode_count);

    /* Detailed per-mode info */
    tprintf("=== Detailed Mode Info ===\n\n");
    for (i = 0; i < mode_count; i++) {
        unsigned short a;
        if (!vbe_get_mode_info(mode_list[i], &vbmi)) continue;
        a = vbmi.mode_attributes;

        tprintf("--- Mode 0x%03X: %dx%d %dbpp %s ---\n",
                mode_list[i], vbmi.x_resolution, vbmi.y_resolution,
                vbmi.bits_per_pixel, mem_model_str(vbmi.memory_model));
        tprintf("  Attributes: 0x%04X\n", a);
        tprintf("    [0]  Supported by hardware : %s\n", (a&0x0001)?"yes":"no");
        tprintf("    [1]  Optional info avail   : %s\n", (a&0x0002)?"yes":"no");
        tprintf("    [2]  BIOS TTY output       : %s\n", (a&0x0004)?"yes":"no");
        tprintf("    [3]  Color mode            : %s\n", (a&0x0008)?"yes":"no");
        tprintf("    [4]  Graphics mode         : %s\n", (a&0x0010)?"yes":"no");
        tprintf("    [5]  Not VGA compatible    : %s\n", (a&0x0020)?"yes":"no");
        tprintf("    [6]  No VGA window memory  : %s\n", (a&0x0040)?"yes":"no");
        tprintf("    [7]  Linear framebuffer    : %s\n", (a&0x0080)?"yes":"no");
        tprintf("    [8]  Double-scan available : %s\n", (a&0x0100)?"yes":"no");
        tprintf("    [9]  Interlace available   : %s\n", (a&0x0200)?"yes":"no");
        tprintf("    [10] HW triple-buffer      : %s\n", (a&0x0400)?"yes":"no");
        tprintf("    [11] HW stereoscopic       : %s\n", (a&0x0800)?"yes":"no");
        tprintf("    [12] Dual display start    : %s\n", (a&0x1000)?"yes":"no");
        tprintf("  Window A  : attr=0x%02X seg=0x%04X\n",
                vbmi.win_a_attributes, vbmi.win_a_segment);
        tprintf("  Window B  : attr=0x%02X seg=0x%04X\n",
                vbmi.win_b_attributes, vbmi.win_b_segment);
        tprintf("  WinGran   : %d KB   WinSize: %d KB\n",
                vbmi.win_granularity, vbmi.win_size);
        tprintf("  WinFunc   : %04lX:%04lX\n",
                (vbmi.win_func_ptr >> 16) & 0xFFFF,
                vbmi.win_func_ptr & 0xFFFF);
        tprintf("  Pitch     : %d bytes\n", vbmi.bytes_per_scan_line);
        tprintf("  Planes    : %d   Banks: %d (size=%d KB)\n",
                vbmi.number_of_planes, vbmi.number_of_banks,
                vbmi.bank_size);
        tprintf("  Pages     : %d total\n",
                vbmi.number_of_image_pages + 1);
        tprintf("  LFB phys  : 0x%08lX\n", vbmi.phys_base_ptr);
        if (vbmi.bits_per_pixel > 8) {
            tprintf("  Red       : %d bits @ %d\n",
                    vbmi.red_mask_size, vbmi.red_field_position);
            tprintf("  Green     : %d bits @ %d\n",
                    vbmi.green_mask_size, vbmi.green_field_position);
            tprintf("  Blue      : %d bits @ %d\n",
                    vbmi.blue_mask_size, vbmi.blue_field_position);
            tprintf("  Reserved  : %d bits @ %d\n",
                    vbmi.rsvd_mask_size, vbmi.rsvd_field_position);
        }
        if (vbi.vbe_version >= 0x0200) {
            tprintf("  OffScrOff : 0x%08lX  OffScrSz: %d KB\n",
                    vbmi.off_screen_mem_off, vbmi.off_screen_mem_size);
        }
        if (vbi.vbe_version >= 0x0300) {
            tprintf("  LinPitch  : %d bytes\n",
                    vbmi.lin_bytes_per_scan_line);
            tprintf("  LinPages  : %d (bank=%d)\n",
                    vbmi.lin_number_of_pages,
                    vbmi.bnk_number_of_pages);
            tprintf("  MaxPixClk : %lu Hz\n", vbmi.max_pixel_clock);
            if (vbmi.bits_per_pixel > 8) {
                tprintf("  LinRed    : %d bits @ %d\n",
                        vbmi.lin_red_mask_size, vbmi.lin_red_field_pos);
                tprintf("  LinGreen  : %d bits @ %d\n",
                        vbmi.lin_green_mask_size, vbmi.lin_green_field_pos);
                tprintf("  LinBlue   : %d bits @ %d\n",
                        vbmi.lin_blue_mask_size, vbmi.lin_blue_field_pos);
                tprintf("  LinRsvd   : %d bits @ %d\n",
                        vbmi.lin_rsvd_mask_size, vbmi.lin_rsvd_field_pos);
            }
        }
        /* Raw hex dump of first 66 bytes (defined fields) */
        tprintf("  Raw (offset 0x00-0x41):\n");
        hex_dump((unsigned char *)&vbmi, 0, 66, 128);
        tprintf("\n");
    }

    /* ==================================================================
     * 3. VBE Function Support Probe
     * ================================================================== */
    tprintf("=== 3. VBE Function Support Probe ===\n\n");
    {
        static struct { unsigned short f; unsigned short bx;
                        const char *n; } probes[] = {
            { 0x4F00, 0x0000, "4F00h Return VBE Controller Info    " },
            { 0x4F01, 0x0000, "4F01h Return VBE Mode Info          " },
            { 0x4F02, 0x0000, "4F02h Set VBE Mode                  " },
            { 0x4F03, 0x0000, "4F03h Return Current VBE Mode       " },
            { 0x4F04, 0x0000, "4F04h Save/Restore State            " },
            { 0x4F05, 0x0100, "4F05h Display Window Control (get)  " },
            { 0x4F06, 0x0100, "4F06h Logical Scan Line Length (get)" },
            { 0x4F07, 0x0001, "4F07h Display Start (get, BL=01h)   " },
            { 0x4F08, 0x0100, "4F08h DAC Palette Format (get)      " },
            { 0x4F09, 0x0100, "4F09h Palette Data (get)            " },
            { 0x4F0A, 0x0000, "4F0Ah Protected Mode Interface      " },
            { 0x4F0B, 0x0000, "4F0Bh Get/Set Pixel Clock           " },
            { 0x4F10, 0x0000, "4F10h Power Management (DPMS)       " },
            { 0x4F11, 0x0000, "4F11h Flat Panel Interface          " },
            { 0x4F15, 0x0000, "4F15h Display Data Channel (DDC)    " },
        };
        int n = sizeof(probes) / sizeof(probes[0]);
        RMI rmi;
        for (i = 0; i < n; i++) {
            int ax;
            memset(&rmi, 0, sizeof(rmi));
            rmi.eax = probes[i].f;
            rmi.ebx = probes[i].bx;
            rmi.es  = g_dos_seg;
            rmi.edi = 0;
            if (!dpmi_real_int(0x10, &rmi))
                ax = -1;
            else
                ax = (int)(rmi.eax & 0xFFFF);
            tprintf("  %s  AX=%04Xh %s\n",
                    probes[i].n, (unsigned)(ax & 0xFFFF),
                    vbe_status_str(ax));
        }
    }
    tprintf("\n");

    /* ==================================================================
     * 4. PMI — Protected Mode Interface (4F0Ah)
     * ================================================================== */
    tprintf("=== 4. VBE Protected Mode Interface (4F0Ah) ===\n\n");
    {
        RMI rmi;
        unsigned short pmi_seg, pmi_off, pmi_size;
        unsigned short setw, setds, setpal;
        unsigned long  pmi_lin;
        unsigned char *pmi_data;

        memset(&rmi, 0, sizeof(rmi));
        rmi.eax = 0x4F0A;
        rmi.ebx = 0x0000;
        if (!dpmi_real_int(0x10, &rmi) ||
            (rmi.eax & 0xFFFF) != 0x004F) {
            tprintf("PMI not available (AX=%04Xh)\n\n",
                    (unsigned)(rmi.eax & 0xFFFF));
        } else {
            pmi_seg  = (unsigned short)rmi.es;
            pmi_off  = (unsigned short)(rmi.edi & 0xFFFF);
            pmi_size = (unsigned short)(rmi.ecx & 0xFFFF);
            pmi_lin  = (unsigned long)pmi_seg * 16 + pmi_off;
            pmi_data = (unsigned char *)pmi_lin;

            setw   = *(unsigned short *)(pmi_lin + 0);
            setds  = *(unsigned short *)(pmi_lin + 2);
            setpal = *(unsigned short *)(pmi_lin + 4);

            tprintf("Location     : %04X:%04X (linear 0x%08lX)\n",
                    pmi_seg, pmi_off, pmi_lin);
            tprintf("Size         : %u bytes\n", pmi_size);
            tprintf("Entry Offsets:\n");
            tprintf("  SetWindow       : +0x%04X (lin 0x%08lX)\n",
                    setw, pmi_lin + setw);
            tprintf("  SetDisplayStart : +0x%04X (lin 0x%08lX)\n",
                    setds, pmi_lin + setds);
            tprintf("  SetPalette      : +0x%04X (lin 0x%08lX)\n",
                    setpal, pmi_lin + setpal);

            /* I/O port privilege list at offset +6 */
            tprintf("\nI/O Port List (offset +6, terminated FFFFh):\n  ");
            {
                unsigned short *ports =
                    (unsigned short *)(pmi_lin + 6);
                int j, col = 0;
                for (j = 0; j < 256 && ports[j] != 0xFFFF; j++) {
                    tprintf("0x%04X ", ports[j]);
                    if (++col >= 8) { tprintf("\n  "); col = 0; }
                }
                tprintf("[FFFF] (%d ports)\n", j);

                /* Check for MMIO list after port list (VBE 3.0) */
                if (vbi.vbe_version >= 0x0300) {
                    unsigned long *mmio =
                        (unsigned long *)&ports[j + 1];
                    if ((unsigned char *)mmio < pmi_data + pmi_size) {
                        tprintf("\nMMIO List (after port list):\n  ");
                        for (j = 0; j < 32; j++) {
                            if (mmio[j] == 0xFFFFFFFFUL) {
                                tprintf("[FFFFFFFF] end\n");
                                break;
                            }
                            tprintf("0x%08lX ", mmio[j]);
                        }
                        if (j == 32)
                            tprintf("(truncated at 32)\n");
                    }
                }
            }

            /* Code analysis: scan for RET/RETF/IRET in each function */
            tprintf("\nPMI Code Analysis:\n");
            {
                struct { const char *name; unsigned short off;
                         unsigned short end; } fns[3];
                int f;
                fns[0].name = "SetWindow";
                fns[0].off  = setw;
                fns[1].name = "SetDisplayStart";
                fns[1].off  = setds;
                fns[2].name = "SetPalette";
                fns[2].off  = setpal;
                /* Determine end of each function (start of next, or
                 * pmi_size for the last) */
                for (f = 0; f < 3; f++) {
                    unsigned short min_next = pmi_size;
                    int g;
                    for (g = 0; g < 3; g++) {
                        if (fns[g].off > fns[f].off &&
                            fns[g].off < min_next)
                            min_next = fns[g].off;
                    }
                    fns[f].end = min_next;
                }
                for (f = 0; f < 3; f++) {
                    unsigned short len = fns[f].end - fns[f].off;
                    unsigned short j;
                    tprintf("  %s (+0x%04X, ~%u bytes):\n",
                            fns[f].name, fns[f].off, len);
                    /* Scan for return instructions */
                    for (j = 0; j < len; j++) {
                        unsigned char b = pmi_data[fns[f].off + j];
                        if (b == 0xC3)
                            tprintf("    +0x%04X: C3 = RET (near)\n",
                                    fns[f].off + j);
                        else if (b == 0xCB)
                            tprintf("    +0x%04X: CB = RETF (far)\n",
                                    fns[f].off + j);
                        else if (b == 0xCF)
                            tprintf("    +0x%04X: CF = IRET\n",
                                    fns[f].off + j);
                    }
                    /* Hex dump (up to 256 bytes) */
                    tprintf("    Code:\n");
                    hex_dump(pmi_data + fns[f].off,
                             pmi_lin + fns[f].off,
                             len, 256);
                }
            }

            /* Full PMI table dump header */
            tprintf("\nFull PMI Table Header (first 64 bytes):\n");
            hex_dump(pmi_data, pmi_lin, pmi_size < 64 ? pmi_size : 64, 64);

            save_bin("VBEPMI.BIN", pmi_data, pmi_size);
            tprintf("\nSaved: VBEPMI.BIN (%u bytes)\n", pmi_size);
            tprintf("Disassemble: ndisasm -b 32 -o 0x%04X VBEPMI.BIN\n",
                    setds);
        }
    }
    tprintf("\n");

    /* ==================================================================
     * 5. In-Mode 4F07h Sub-function Test
     * ================================================================== */
    tprintf("=== 5. In-Mode VBE 4F07h Sub-function Test ===\n\n");

    if (test_mode == 0xFFFF) {
        tprintf("No suitable 1024x768 8bpp LFB mode found, skipping.\n\n");
    } else {
        tprintf("Using mode 0x%03X (%dx%d 8bpp, %lu pages)\n\n",
                test_mode, TEST_WIDTH, TEST_HEIGHT, test_pages);

        if (!vbe_set_mode(test_mode)) {
            tprintf("Failed to set mode 0x%03X!\n\n", test_mode);
        } else {
            struct {
                unsigned short bl;
                unsigned short cx, dx;
                const char *desc;
            } tests[] = {
                { 0x00, 0, 0,
                  "BL=00h Set Start immed (0,0)          " },
                { 0x01, 0, 0,
                  "BL=01h Get Display Start               " },
                { 0x80, 0, 0,
                  "BL=80h Set Start vsync-wait (0,0)      " },
                { 0x00, 0, 0,
                  "BL=00h Set Start immed (0,0) reset      " },
                { 0x02, 0, 0,
                  "BL=02h Schedule flip (0,0) same-page    " },
                { 0x02, 0, TEST_HEIGHT,
                  "BL=02h Schedule flip (0,768) page 1     " },
                { 0x04, 0, 0,
                  "BL=04h Get sched flip status            " },
                { 0x82, 0, 0,
                  "BL=82h Schedule+wait (0,0)              " },
                { 0x82, 0, TEST_HEIGHT,
                  "BL=82h Schedule+wait (0,768)            " },
                { 0x00, 0, 0,
                  "BL=00h Reset to (0,0)                   " },
            };
            int n = sizeof(tests) / sizeof(tests[0]);
            int t;

            for (t = 0; t < n; t++) {
                unsigned long r = vbe_raw_4f07(tests[t].bl,
                                               tests[t].cx,
                                               tests[t].dx);
                unsigned short ax = (unsigned short)(r & 0xFFFF);
                unsigned short bx = (unsigned short)(r >> 16);
                tprintf("  %s  AX=%04Xh BX=%04Xh %s\n",
                        tests[t].desc, ax, bx,
                        vbe_status_str(ax));
            }

            vbe_set_text_mode();
        }
    }
    tprintf("\n");

    /* ==================================================================
     * 6. Video BIOS ROM Dump
     * ================================================================== */
    tprintf("=== 6. Video BIOS ROM ===\n\n");

    rom = (unsigned char *)0xC0000UL;
    if (rom[0] == 0x55 && rom[1] == 0xAA) {
        rom_size = (unsigned long)rom[2] * 512UL;
        tprintf("ROM Signature : 55 AA (valid)\n");
        tprintf("ROM Size      : %lu bytes (%lu KB)\n",
                rom_size, rom_size / 1024UL);
    } else {
        tprintf("No valid ROM at C0000h (%02X %02X), dumping 64KB\n",
                rom[0], rom[1]);
        rom_size = 65536UL;
    }

    /* PCI ROM header */
    if (rom[0] == 0x55 && rom[1] == 0xAA) {
        unsigned short pcir_off = *(unsigned short *)(rom + 0x18);
        if (pcir_off < rom_size - 24 &&
            rom[pcir_off] == 'P' && rom[pcir_off+1] == 'C' &&
            rom[pcir_off+2] == 'I' && rom[pcir_off+3] == 'R') {
            unsigned short vid = *(unsigned short *)(rom + pcir_off + 4);
            unsigned short did = *(unsigned short *)(rom + pcir_off + 6);
            unsigned long  cls = (unsigned long)rom[pcir_off + 0x0D] |
                                 ((unsigned long)rom[pcir_off + 0x0E] << 8) |
                                 ((unsigned long)rom[pcir_off + 0x0F] << 16);
            tprintf("PCI Data      : at +0x%04X\n", pcir_off);
            tprintf("  Vendor ID   : 0x%04X", vid);
            if (vid == 0x1002) tprintf(" (ATI/AMD)");
            else if (vid == 0x10DE) tprintf(" (NVIDIA)");
            else if (vid == 0x8086) tprintf(" (Intel)");
            tprintf("\n");
            tprintf("  Device ID   : 0x%04X\n", did);
            tprintf("  Class Code  : 0x%06lX\n", cls);
        } else {
            tprintf("PCI Data      : not found (offset 0x%04X)\n",
                    pcir_off);
        }
    }

    /* Search for notable strings */
    tprintf("\nROM String Search:\n");
    {
        static const char *needles[] = {
            "VESA", "PMID", "ATOM", "ATOMBIOS", "VBE3", "VBE/AF",
            "PMI", "BIOS", NULL
        };
        int ni;
        for (ni = 0; needles[ni]; ni++) {
            unsigned long j;
            int nlen = strlen(needles[ni]);
            int found = 0;
            for (j = 0; j + nlen <= rom_size; j++) {
                if (memcmp(rom + j, needles[ni], nlen) == 0) {
                    if (!found)
                        tprintf("  '%s':", needles[ni]);
                    tprintf(" +0x%04lX", j);
                    found++;
                    if (found > 8) { tprintf(" ..."); break; }
                }
            }
            if (found) tprintf("\n");
        }
    }

    tprintf("\nROM Header (first 64 bytes):\n");
    hex_dump(rom, 0xC0000UL, 64, 64);

    save_bin("VBEROM.BIN", rom, rom_size);
    tprintf("\nSaved: VBEROM.BIN (%lu bytes)\n", rom_size);
    tprintf("\n");

    /* ==================================================================
     * 7. VBE/AF Probe
     * ================================================================== */
    tprintf("=== 7. VBE/AF (Accelerator Functions) ===\n\n");
    tprintf("VBE/AF provides HW-accelerated BitBlt, line draw, etc.\n");
    tprintf("Requires VBEAF.DRV driver file.\n\n");
    {
        FILE *af = fopen("VBEAF.DRV", "rb");
        if (!af) af = fopen("C:\\VBEAF.DRV", "rb");
        if (af) {
            long sz;
            fseek(af, 0, SEEK_END);
            sz = ftell(af);
            fclose(af);
            tprintf("VBEAF.DRV found (%ld bytes)\n", sz);
        } else {
            tprintf("VBEAF.DRV not found\n");
        }
    }
    tprintf("\n");

    /* ==================================================================
     * 8. Summary
     * ================================================================== */
    tprintf("=== Files Created ===\n\n");
    tprintf("  VBEDUMP.TXT  - this report\n");
    tprintf("  VBEINFO.BIN  - raw VBE controller info (512 bytes)\n");
    tprintf("  VBEMODES.BIN - mode records (2+256 bytes each)\n");
    tprintf("  VBEPMI.BIN   - PMI table and code\n");
    tprintf("  VBEROM.BIN   - video BIOS ROM\n\n");
    tprintf("Disassembly tips:\n");
    tprintf("  PMI (32-bit PM code):\n");
    tprintf("    ndisasm -b 32 VBEPMI.BIN > VBEPMI.ASM\n");
    tprintf("  ROM (16-bit real-mode BIOS):\n");
    tprintf("    ndisasm -b 16 -o 0xC0000 VBEROM.BIN > VBEROM.ASM\n");
    tprintf("  Or load into IDA Pro / Ghidra for structured analysis.\n");

    /* Cleanup */
    dpmi_free_dos();
    if (g_log) { fclose(g_log); g_log = NULL; }

    printf("\nDone. Full report in VBEDUMP.TXT\n");
    return 0;
}
