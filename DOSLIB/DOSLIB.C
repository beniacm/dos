/*
 * DOSLIB.C — Shared DOS protected-mode demo library
 *
 * Implementation of DPMI, VBE, PCI, palette, font, text, timing and
 * vsync functions.  All OpenWatcom / DJGPP differences are confined
 * to this file so demo programs compile with zero #ifdef blocks.
 *
 * Compiles under DJGPP/GCC 12+ and OpenWatcom 1.9+ (C89).
 */

#include "DOSLIB.H"
#include <conio.h>
#include <time.h>

/* =============================================================== */
/*  Global state definitions                                        */
/* =============================================================== */

unsigned short g_dseg = 0, g_dsel = 0;
int            g_xres = 0, g_yres = 0, g_pitch = 0;
unsigned short g_vmode = 0;
unsigned long  g_lfb_phys = 0;
unsigned char *g_lfb = NULL;
unsigned long  g_vesa_mem_kb = 0;
unsigned char *g_font = NULL;
int            g_dac_bits = 6;
double         g_rdtsc_mhz = 0.0;

/* =============================================================== */
/*  DPMI helpers                                                    */
/* =============================================================== */

unsigned char *dosbuf(void)
{
#ifdef __DJGPP__
    return (unsigned char *)(((unsigned long)g_dseg << 4)
                             + __djgpp_conventional_base);
#else
    return (unsigned char *)((unsigned long)g_dseg << 4);
#endif
}

int dpmi_alloc(unsigned short para)
{
#ifdef __DJGPP__
    int seg;
    seg = __dpmi_allocate_dos_memory(para, (int *)&g_dsel);
    if (seg == -1) return 0;
    g_dseg = (unsigned short)seg;
    return 1;
#else
    union REGS r;
    memset(&r, 0, sizeof r);
    r.x.eax = 0x0100;  r.x.ebx = para;
    int386(0x31, &r, &r);
    if (r.x.cflag) return 0;
    g_dseg = (unsigned short)r.x.eax;
    g_dsel = (unsigned short)r.x.edx;
    return 1;
#endif
}

void dpmi_free(void)
{
#ifdef __DJGPP__
    if (!g_dsel) return;
    __dpmi_free_dos_memory(g_dsel);
    g_dsel = g_dseg = 0;
#else
    union REGS r;
    if (!g_dsel) return;
    memset(&r, 0, sizeof r);
    r.x.eax = 0x0101;  r.x.edx = g_dsel;
    int386(0x31, &r, &r);
    g_dsel = g_dseg = 0;
#endif
}

int dpmi_rmint(unsigned char n, RMI *p)
{
#ifdef __DJGPP__
    __dpmi_regs r;
    memset(&r, 0, sizeof r);
    r.x.di = (unsigned short)(p->edi & 0xFFFF);
    r.x.si = (unsigned short)(p->esi & 0xFFFF);
    r.x.bx = (unsigned short)(p->ebx & 0xFFFF);
    r.x.dx = (unsigned short)(p->edx & 0xFFFF);
    r.x.cx = (unsigned short)(p->ecx & 0xFFFF);
    r.x.ax = (unsigned short)(p->eax & 0xFFFF);
    r.x.es  = p->es;
    r.x.ds  = p->ds;
    r.x.flags = p->flags;
    if (__dpmi_simulate_real_mode_interrupt(n, &r) != 0) return 0;
    p->edi = r.d.edi;  p->esi = r.d.esi;  p->ebp = r.d.ebp;
    p->ebx = r.d.ebx;  p->edx = r.d.edx;  p->ecx = r.d.ecx;
    p->eax = r.d.eax;
    p->flags = r.x.flags;
    p->es = r.x.es;    p->ds = r.x.ds;
    return !(r.x.flags & 1);
#else
    union REGS r;  struct SREGS sr;
    segread(&sr);
    memset(&r, 0, sizeof r);
    r.x.eax = 0x0300;  r.x.ebx = n;  r.x.ecx = 0;
    r.x.edi = (unsigned int)p;
    int386x(0x31, &r, &r, &sr);
    return !(r.x.cflag);
#endif
}

void *dpmi_map(unsigned long phys, unsigned long sz)
{
#ifdef __DJGPP__
    __dpmi_meminfo info;
    info.address = phys;
    info.size = sz;
    if (__dpmi_physical_address_mapping(&info) == -1) return NULL;
    return (void *)((unsigned long)info.address + __djgpp_conventional_base);
#else
    union REGS r;
    memset(&r, 0, sizeof r);
    r.x.eax = 0x0800;
    r.x.ebx = (phys >> 16) & 0xFFFF;  r.x.ecx = phys & 0xFFFF;
    r.x.esi = (sz   >> 16) & 0xFFFF;  r.x.edi = sz   & 0xFFFF;
    int386(0x31, &r, &r);
    if (r.x.cflag) return NULL;
    return (void *)(((r.x.ebx & 0xFFFF) << 16) | (r.x.ecx & 0xFFFF));
#endif
}

void dpmi_unmap(void *p)
{
#ifdef __DJGPP__
    __dpmi_meminfo info;
    if (!p) return;
    info.address = (unsigned long)p - __djgpp_conventional_base;
    info.size = 0;
    __dpmi_free_physical_address_mapping(&info);
#else
    union REGS r;
    unsigned long a = (unsigned long)p;
    if (!p) return;
    memset(&r, 0, sizeof r);
    r.x.eax = 0x0801;
    r.x.ebx = (a >> 16) & 0xFFFF;  r.x.ecx = a & 0xFFFF;
    int386(0x31, &r, &r);
#endif
}

/* =============================================================== */
/*  PCI configuration (Mechanism 1)                                 */
/* =============================================================== */

unsigned long pci_rd32(int b, int d, int f, int reg)
{
    outpd(0xCF8, 0x80000000UL | ((unsigned long)b << 16) |
          ((unsigned long)d << 11) | ((unsigned long)f << 8) | (reg & 0xFC));
    return inpd(0xCFC);
}

void pci_wr32(int b, int d, int f, int reg, unsigned long v)
{
    outpd(0xCF8, 0x80000000UL | ((unsigned long)b << 16) |
          ((unsigned long)d << 11) | ((unsigned long)f << 8) | (reg & 0xFC));
    outpd(0xCFC, v);
}

unsigned short pci_rd16(int b, int d, int f, int reg)
{
    unsigned long v = pci_rd32(b, d, f, reg & ~3);
    return (unsigned short)(v >> ((reg & 2) * 8));
}

/* =============================================================== */
/*  VBE helpers                                                     */
/* =============================================================== */

int vbe_get_info(VBEInfo *out)
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

int vbe_get_mode_info(unsigned short m, VBEModeInfo *out)
{
    RMI rm;
    memset(&rm, 0, sizeof rm);
    memset(dosbuf(), 0, 256);
    rm.eax = 0x4F01;  rm.ecx = m;  rm.es = g_dseg;  rm.edi = 0;
    if (!dpmi_rmint(0x10, &rm)) return 0;
    if ((rm.eax & 0xFFFF) != 0x004F) return 0;
    memcpy(out, dosbuf(), sizeof(VBEModeInfo));
    return 1;
}

int vbe_set_mode(unsigned short m)
{
    RMI rm;
    memset(&rm, 0, sizeof rm);
    rm.eax = 0x4F02;  rm.ebx = (unsigned long)m | 0x4000;
    if (!dpmi_rmint(0x10, &rm)) return 0;
    return ((rm.eax & 0xFFFF) == 0x004F);
}

void vbe_text_mode(void)
{
    RMI rm;
    memset(&rm, 0, sizeof rm);
    rm.eax = 0x0003;
    dpmi_rmint(0x10, &rm);
}

/* =============================================================== */
/*  Parameterised VBE mode search                                   */
/*                                                                  */
/*  Prefers exact (want_xres × want_yres); falls back to the       */
/*  largest mode that fits within (max_xres × max_yres) at the     */
/*  requested bpp.  On success sets g_vmode, g_xres, g_yres,       */
/*  g_pitch, g_lfb_phys, g_vesa_mem_kb.                            */
/* =============================================================== */

int find_vbe_mode(int want_xres, int want_yres, int want_bpp,
                  int max_xres, int max_yres)
{
    VBEInfo vi;
    VBEModeInfo mi;
    unsigned short *ml, modelist[512];
    unsigned long seg, off;
    int i, cnt;
    unsigned long best_pixels = 0;

    if (!vbe_get_info(&vi)) return 0;
    if (vi.ver < 0x0200) return 0;

    g_vesa_mem_kb = (unsigned long)vi.tot_mem * 64;

    seg = (vi.mode_ptr >> 16) & 0xFFFF;
    off = vi.mode_ptr & 0xFFFF;
#ifdef __DJGPP__
    ml = (unsigned short *)((seg << 4) + off + __djgpp_conventional_base);
#else
    ml = (unsigned short *)((seg << 4) + off);
#endif
    for (cnt = 0; cnt < 511 && ml[cnt] != 0xFFFF; cnt++)
        modelist[cnt] = ml[cnt];
    modelist[cnt] = 0xFFFF;

    g_vmode = 0;
    for (i = 0; i < cnt; i++) {
        unsigned long px;
        if (!vbe_get_mode_info(modelist[i], &mi)) continue;
        if (mi.bpp != (unsigned char)want_bpp)  continue;
        if (!(mi.attr & 0x01))  continue;   /* supported */
        if (!(mi.attr & 0x10))  continue;   /* graphics  */
        if (!(mi.attr & 0x80))  continue;   /* LFB       */
        if (mi.model != 4 && mi.model != 6) continue; /* packed or direct */
        if (mi.lfb_phys == 0)   continue;
        if (mi.xres > (unsigned short)max_xres ||
            mi.yres > (unsigned short)max_yres)
            continue;

        px = (unsigned long)mi.xres * mi.yres;

        /* Hard-prefer exact match */
        if (mi.xres == (unsigned short)want_xres &&
            mi.yres == (unsigned short)want_yres) {
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
/*  VGA palette (VBE 4F08h / 4F09h)                                */
/* =============================================================== */

void init_dac(void)
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

void vbe_set_palette(int start, int count, const unsigned char *p)
{
    RMI rm;
    unsigned char *buf = dosbuf();
    int i, shift;

    shift = 8 - g_dac_bits;
    for (i = 0; i < count; i++) {
        buf[i * 4 + 0] = p[i * 4 + 2] >> shift;   /* B */
        buf[i * 4 + 1] = p[i * 4 + 1] >> shift;   /* G */
        buf[i * 4 + 2] = p[i * 4 + 0] >> shift;   /* R */
        buf[i * 4 + 3] = 0;
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

void pal_set(unsigned char *p, int idx, int r, int g, int b)
{
    p[idx * 4 + 0] = (unsigned char)r;
    p[idx * 4 + 1] = (unsigned char)g;
    p[idx * 4 + 2] = (unsigned char)b;
    p[idx * 4 + 3] = 0;
}

void pal_lerp(unsigned char *p, int i0, int i1,
              int r0, int g0, int b0, int r1, int g1, int b1)
{
    int i, n;
    n = i1 - i0;
    if (n <= 0) return;
    for (i = 0; i <= n; i++) {
        pal_set(p, i0 + i,
                r0 + (r1 - r0) * i / n,
                g0 + (g1 - g0) * i / n,
                b0 + (b1 - b0) * i / n);
    }
}

/* =============================================================== */
/*  BIOS 8×8 font                                                   */
/* =============================================================== */

unsigned char *get_font(void)
{
    RMI rm;
    memset(&rm, 0, sizeof rm);
    rm.eax = 0x1130;  rm.ebx = 0x0300;
    dpmi_rmint(0x10, &rm);
#ifdef __DJGPP__
    return (unsigned char *)(((unsigned long)rm.es << 4)
                             + (rm.ebp & 0xFFFF)
                             + __djgpp_conventional_base);
#else
    return (unsigned char *)(((unsigned long)rm.es << 4)
                             + (rm.ebp & 0xFFFF));
#endif
}

/* =============================================================== */
/*  Text rendering (uses g_lfb, g_pitch, g_xres globals)           */
/* =============================================================== */

void cpu_char(int x, int y, char ch, unsigned char c, int sc)
{
    unsigned char *g = g_font + (unsigned char)ch * 8;
    int r, cl, sy, sx;
    for (r = 0; r < 8; r++) {
        unsigned char b = g[r];
        for (cl = 0; cl < 8; cl++) {
            if (b & (0x80 >> cl)) {
                for (sy = 0; sy < sc; sy++)
                    for (sx = 0; sx < sc; sx++)
                        g_lfb[(y + r * sc + sy) * g_pitch
                              + x + cl * sc + sx] = c;
            }
        }
    }
}

void cpu_str(int x, int y, const char *s, unsigned char c, int sc)
{
    for (; *s; s++, x += 8 * sc)
        cpu_char(x, y, *s, c, sc);
}

void cpu_str_c(int y, const char *s, unsigned char c, int sc)
{
    int x = (g_xres - (int)strlen(s) * 8 * sc) / 2;
    if (x < 0) x = 0;
    cpu_str(x, y, s, c, sc);
}

/* =============================================================== */
/*  RDTSC high-resolution timer                                     */
/* =============================================================== */

void rdtsc_read(unsigned long *lo, unsigned long *hi)
{
#ifdef __DJGPP__
    unsigned long a, d;
    __asm__ volatile("rdtsc" : "=a"(a), "=d"(d));
    *lo = a;
    *hi = d;
#else
    unsigned long a, d;
    __asm {
        rdtsc
        mov [a], eax
        mov [d], edx
    }
    *lo = a;
    *hi = d;
#endif
}

double tsc_to_ms(unsigned long lo1, unsigned long hi1,
                 unsigned long lo0, unsigned long hi0)
{
    double cycles = (double)hi1 * 4294967296.0 + (double)lo1
                  - (double)hi0 * 4294967296.0 - (double)lo0;
    return (g_rdtsc_mhz > 0.0) ? cycles / (g_rdtsc_mhz * 1000.0) : 0.0;
}

void calibrate_rdtsc(void)
{
    unsigned long bt0, bt1;
    unsigned long lo0, hi0, lo1, hi1;
    double elapsed_s, cycles;

#ifdef __DJGPP__
    volatile unsigned long *bios_ticks;
    __djgpp_nearptr_enable();
    bios_ticks = (volatile unsigned long *)(0x46CUL
                                            + __djgpp_conventional_base);
#else
    volatile unsigned long *bios_ticks = (volatile unsigned long *)0x46CUL;
#endif

    bt0 = *bios_ticks;
    while (*bios_ticks == bt0) {}

    rdtsc_read(&lo0, &hi0);
    bt0 = *bios_ticks;
    while ((bt1 = *bios_ticks) - bt0 < 4) {}
    rdtsc_read(&lo1, &hi1);

    elapsed_s = (double)(bt1 - bt0) / 18.2065;
    cycles    = (double)hi1 * 4294967296.0 + (double)lo1
              - (double)hi0 * 4294967296.0 - (double)lo0;
    g_rdtsc_mhz = (elapsed_s > 0.0) ? cycles / elapsed_s / 1.0e6 : 0.0;
}

/* =============================================================== */
/*  VGA vsync (port 0x3DA)                                          */
/* =============================================================== */

void wait_vsync(void)
{
    while (inp(0x3DA) & 0x08) {}
    while (!(inp(0x3DA) & 0x08)) {}
}
