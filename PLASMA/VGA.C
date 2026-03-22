/*
 * VGA.C — DOS VBE/VESA graphics library
 *
 * Provides DPMI memory management, VBE mode setting, PMI page flip,
 * MTRR/PAT write-combining, GPU register access, text rendering,
 * WC-optimal blitting, and RDTSC timing for 32-bit DOS protected mode.
 *
 * Compiles under DJGPP/GCC and OpenWatcom (C89).
 */

#include "VGA.H"

/* --------------------------------------------------------------------------
 * DPMI state
 * -------------------------------------------------------------------------- */

unsigned short g_dos_seg = 0;
unsigned short g_dos_sel = 0;

/* VBE 3.0 / PMI state */
int           g_vbe3         = 0;    /* non-zero when VBE >= 3.0     */
int           g_force_vbe2   = 0;    /* non-zero to disable VBE 3.0  */
int           g_no_pmi      = 1;    /* 0=use PMI, 1=skip (default: skip, use -pmi to enable) */
int           g_no_mtrr     = 0;    /* non-zero to skip MTRR WC     */
int           g_mtrr_info   = 0;    /* non-zero to show MTRR dump   */
int           g_direct_vram = 0;    /* non-zero to render direct to VRAM (slow, for comparison) */
unsigned short g_vbe_version = 0;    /* raw VBE version (BCD)        */
int           g_pmi_ok       = 0;    /* non-zero when PMI page-flip  */
int           g_sched_flip   = 0;    /* VBE 3.0 scheduled flip (BL=02h) */
int           g_sched_req    = 0;    /* -sched flag (opt-in, ATI crashes) */
unsigned short g_pmi_rm_seg  = 0;    /* real-mode segment of PMI tbl */
unsigned short g_pmi_rm_off  = 0;    /* real-mode offset  of PMI tbl */
unsigned short g_pmi_size    = 0;    /* PMI table size in bytes      */
unsigned short g_pmi_setw_off  = 0;  /* SetWindow entry offset       */
unsigned short g_pmi_setds_off = 0;  /* SetDisplayStart entry offset */
unsigned short g_pmi_setpal_off= 0;  /* SetPrimaryPalette offset     */

/* Direct GPU register page flip (bypasses BIOS completely) */
int            g_hw_flip      = 0;   /* use direct HW register flip  */
unsigned short g_gpu_iobase   = 0;   /* GPU I/O base (MM_INDEX port) */
unsigned long  g_fb_phys      = 0;   /* LFB physical base for flip   */

/* --------------------------------------------------------------------------
 * DPMI functions
 * -------------------------------------------------------------------------- */

unsigned char *dos_buf(void)
{
#ifdef __DJGPP__
    return (unsigned char *)((unsigned long)g_dos_seg * 16 + __djgpp_conventional_base);
#else
    return (unsigned char *)((unsigned long)g_dos_seg << 4);
#endif
}

#ifdef __DJGPP__
int dpmi_alloc_dos(unsigned short paragraphs)
{
    int sel_or_max;
    int seg = __dpmi_allocate_dos_memory(paragraphs, &sel_or_max);
    if (seg == -1) return 0;
    g_dos_seg = (unsigned short)seg;
    g_dos_sel = (unsigned short)sel_or_max;
    return 1;
}
#else
int dpmi_alloc_dos(unsigned short paragraphs)
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
void dpmi_free_dos(void)
{
    if (!g_dos_sel) return;
    __dpmi_free_dos_memory(g_dos_sel);
    g_dos_sel = 0;
    g_dos_seg = 0;
}
#else
void dpmi_free_dos(void)
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
int dpmi_real_int(unsigned char intno, RMI *rmi)
{
    return (__dpmi_simulate_real_mode_interrupt(intno, (__dpmi_regs *)rmi) == 0);
}
#else
int dpmi_real_int(unsigned char intno, RMI *rmi)
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
void *dpmi_map_physical(unsigned long phys, unsigned long size)
{
    __dpmi_meminfo mi;
    mi.address = phys;
    mi.size = size;
    if (__dpmi_physical_address_mapping(&mi) == -1) return NULL;
    return (void *)(mi.address + __djgpp_conventional_base);
}
#else
void *dpmi_map_physical(unsigned long phys, unsigned long size)
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
void dpmi_unmap_physical(void *ptr)
{
    __dpmi_meminfo mi;
    mi.address = (unsigned long)ptr - __djgpp_conventional_base;
    mi.size = 0;
    __dpmi_free_physical_address_mapping(&mi);
}
#else
void dpmi_unmap_physical(void *ptr)
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

int vbe_get_info(VBEInfo *out)
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

int vbe_get_mode_info(unsigned short mode, VBEModeInfo *out)
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

int vbe_set_mode(unsigned short mode)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F02;
    rmi.ebx = (unsigned long)mode | 0x4000;   /* bit 14 = LFB */
    if (!dpmi_real_int(0x10, &rmi)) return 0;
    return ((rmi.eax & 0xFFFF) == 0x004F);
}

void vbe_set_text_mode(void)
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
 * wait = 1 -> BL=80h: block until next vsync, then flip (no tearing)
 *        0 -> BL=00h: flip immediately (may tear)
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
 * BL=02h: VBE 3.0 scheduled flip -- sets start address to take effect at
 *         next vsync, returns immediately (non-blocking, tear-free)
 * BL=04h: VBE 3.0 get scheduled flip status
 *         returns BH: 0 = flip completed, 1 = flip still pending
 * BL=80h: wait for vsync then set display start (blocking, tear-free)
 *         NOTE: unreliable on ATI ATOMBIOS -- slow command table execution
 *         can miss the vsync window, causing tearing.  Prefer BL=00h with
 *         separate wait_vsync() via port 0x3DA.
 */
int vbe_set_display_start(unsigned short cx, unsigned short dy,
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
int vbe_schedule_display_start(unsigned short cx, unsigned short dy)
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
int vbe_is_flip_complete(void)
{
    RMI rmi;
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F07;
    rmi.ebx = 0x0004;
    if (!dpmi_real_int(0x10, &rmi)) return 1; /* assume done on error */
    if ((rmi.eax & 0xFFFF) != 0x004F) return 1;
    return ((rmi.ebx & 0xFF00) == 0);  /* BH=0 means flip complete */
}

/* --------------------------------------------------------------------------
 * VBE 3.0 Protected Mode Interface (PMI)
 * -------------------------------------------------------------------------- */

/*
 * VBE 3.0: query Protected Mode Interface via INT 10h AX=4F0Ah.
 * Fills g_pmi_* globals.  Returns 1 on success, 0 if unavailable.
 */
int query_pmi(void)
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
 * It lives in the BIOS ROM area (typically 0xC0000-0xCFFFF), which is
 * accessible via the flat memory model.
 *
 * The correct calling convention is a near CALL in 32-bit PM -- NOT
 * DPMI 0300h/0301h (those are for real-mode code).  Registers follow
 * the same convention as INT 10h VBE calls.
 *
 * Requires ring 0 (PMODE/W) for the direct port I/O in the PMI code.
 */
#ifdef __DJGPP__
unsigned long __attribute__((noinline))
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

unsigned long __attribute__((noinline))
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

int pmi_set_display_start(unsigned short cx, unsigned short dy,
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
int pmi_schedule_display_start(unsigned short cx, unsigned short dy)
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
int pmi_is_flip_complete(void)
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
 *   3. No D1GRPH_UPDATE_LOCK used -- immediate writes cause tearing
 *
 * This direct implementation fixes all three by:
 *   - Computing correct physical address (base + page * pitch * height)
 *   - Using D1GRPH_UPDATE_LOCK so hardware applies at next vsync
 *   - Never blocking -- CPU is free to render the next frame immediately
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
void _gpu_outpd(unsigned short port, unsigned long val)
{
    __asm__ __volatile__ ("outl %0, %w1" :: "a"(val), "Nd"(port));
}
unsigned long _gpu_inpd(unsigned short port)
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

unsigned long gpu_reg_read(unsigned long reg)
{
    _gpu_outpd(g_gpu_iobase, reg);
    return _gpu_inpd(g_gpu_iobase + 4);
}

void gpu_reg_write(unsigned long reg, unsigned long val)
{
    _gpu_outpd(g_gpu_iobase, reg);
    _gpu_outpd(g_gpu_iobase + 4, val);
}

/* Detect GPU I/O base by PCI bus scan.
 * Finds the PCI device whose memory BAR matches lfb_phys,
 * then locates its I/O BAR for MM_INDEX/MM_DATA access. */
int detect_gpu_iobase(unsigned long lfb_phys)
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
                    /* I/O BAR -- strip indicator bits (low byte like PMI) */
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
void hw_page_flip(unsigned long page_phys_addr)
{
    unsigned long lock;

    /* Lock surface updates */
    lock = gpu_reg_read(R5_D1GRPH_UPDATE);
    gpu_reg_write(R5_D1GRPH_UPDATE, lock | R5_D1GRPH_SURFACE_UPDATE_LOCK);

    /* Write new surface address to both primary and secondary */
    gpu_reg_write(R5_D1GRPH_PRIMARY_SURFACE_ADDRESS, page_phys_addr);
    gpu_reg_write(R5_D1GRPH_SECONDARY_SURFACE_ADDRESS, page_phys_addr);

    /* Unlock -- hardware applies at next vsync */
    lock = gpu_reg_read(R5_D1GRPH_UPDATE);
    gpu_reg_write(R5_D1GRPH_UPDATE, lock & ~R5_D1GRPH_SURFACE_UPDATE_LOCK);
}

/* Check if the previous hw_page_flip has been applied by hardware.
 * Returns 1 if flip is done, 0 if still pending. */
int hw_is_flip_done(void)
{
    return !(gpu_reg_read(R5_D1GRPH_UPDATE) & R5_D1GRPH_SURFACE_UPDATE_PENDING);
}

/* --------------------------------------------------------------------------
 * MTRR Write-Combining for LFB
 *
 * When running at ring 0 (PMODE/W), programs a CPU variable-range MTRR to
 * mark the LFB physical address range as write-combining (WC).  The CPU
 * then combines sequential byte/word/dword writes into full cache-line
 * burst transactions over PCI/PCIe -- typically ~10x faster VRAM blits.
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

unsigned short _get_cs(void)
{
    unsigned short cs;
    __asm__ __volatile__ ("movw %%cs, %0" : "=r"(cs));
    return cs;
}

int _has_cpuid(void)
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

unsigned long _cpuid1_edx(void)
{
    unsigned long eax = 1, ebx_out, ecx_out, edx;
    __asm__ __volatile__ (
        "cpuid"
        : "+a"(eax), "=b"(ebx_out), "=c"(ecx_out), "=d"(edx)
    );
    return edx;
}

unsigned long _rdmsr_lo(unsigned long msr)
{
    unsigned long lo, hi;
    __asm__ __volatile__ ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return lo;
}

unsigned long _rdmsr_hi(unsigned long msr)
{
    unsigned long lo, hi;
    __asm__ __volatile__ ("rdmsr" : "=a"(lo), "=d"(hi) : "c"(msr));
    return hi;
}

void _wrmsr_3(unsigned long msr, unsigned long lo, unsigned long hi)
{
    __asm__ __volatile__ ("wrmsr" :: "c"(msr), "a"(lo), "d"(hi));
}

void _wbinvd(void)
{
    __asm__ __volatile__ ("wbinvd" ::: "memory");
}

unsigned long _read_cr3(void)
{
    unsigned long val;
    __asm__ __volatile__ ("movl %%cr3, %0" : "=r"(val));
    return val;
}

void _flush_tlb(void)
{
    unsigned long tmp;
    __asm__ __volatile__ (
        "movl %%cr3, %0\n\t"
        "movl %0, %%cr3"
        : "=r"(tmp) :: "memory"
    );
}

unsigned long _read_cr0(void)
{
    unsigned long val;
    __asm__ __volatile__ ("movl %%cr0, %0" : "=r"(val));
    return val;
}

/* Enable SSE/SSE2/SSE3 for Pentium D: set OSFXSR + OSXMMEXCPT in CR4 */
void djgpp_enable_sse(void)
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
unsigned long get_cs_limit(void)
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
unsigned long desc_base(const unsigned char *d)
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
 *   1. Try DPMI function 0008h (Set Segment Limit) -- the clean way.
 *      Works on PMODE/DJ (GDT selectors, ring 0).  The INT 31h return
 *      via IRETD automatically reloads CS from the modified descriptor.
 *   2. If DPMI fails (e.g. CWSDPR0 rejects 0008h for CS), fall back to
 *      direct descriptor table patching at ring 0 + LRET to reload CS.
 *      Handles both GDT selectors (PMODE/DJ) and LDT selectors (CWSDPR0).
 *
 * Required: ring 0, nearptr already enabled (DS limit = 4 GB).
 */
int expand_cs_to_4gb(void)
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
            /* CS is in the LDT -- find LDT base from its GDT descriptor */
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

        /* Set limit to 0xFFFFF with G=1 (page granularity) -> 4 GB */
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

/* _get_cs is declared + pragma'd in VGA.H for cross-TU use */

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

/* --------------------------------------------------------------------------
 * MTRR / PAT
 * -------------------------------------------------------------------------- */

#define MSR_MTRR_DEF_TYPE   0x2FF
#define MSR_PAT             0x277

/* MTRR state for cleanup on exit */
int           g_mtrr_slot = -1;
unsigned long g_mtrr_save_base_lo, g_mtrr_save_base_hi;
unsigned long g_mtrr_save_mask_lo, g_mtrr_save_mask_hi;
int           g_mtrr_wc = 0;  /* 0=off, 1=set by us, 2=already set */
int           g_mtrr_replaced_uc = 0; /* 1 if we replaced a BIOS UC entry */

/* PAT state for cleanup on exit */
unsigned long g_pat_save_lo = 0, g_pat_save_hi = 0;
int           g_pat_modified = 0; /* 1 if we changed PAT entry 3 */

unsigned long next_pow2(unsigned long v)
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
int setup_mtrr_wc(unsigned long phys_addr, unsigned long vram_bytes)
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
            /* Free slot -- record first one found */
            if (free_slot < 0) free_slot = i;
            continue;
        }

        /* Check if this active MTRR's mask covers phys_addr */
        if ((phys_addr & (mlo & 0xFFFFF000UL)) != (blo & 0xFFFFF000UL))
            continue;

        if ((blo & 0xFF) == MTRR_TYPE_WC) {
            /* LFB already WC -- nothing to do */
            g_mtrr_wc = 2;
            return -1;
        }

        if ((blo & 0xFF) == 0 /* UC */) {
            /* Conflicting UC entry -- must replace to avoid overlap loss */
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

void restore_mtrr(void)
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
 *   UC- + MTRR WC -> effective WC  (what we want for the LFB)
 *   UC- + MTRR UC -> effective UC  (safe for other MMIO)
 *
 * This avoids fragile page table walking (which crashes in PMODE/W
 * because CR3 holds a physical address that may not be linearly mapped).
 * -------------------------------------------------------------------------- */

int setup_pat_uc_minus(void)
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

void restore_pat(void)
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
const char *mtrr_type_name(unsigned long type)
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

void dump_mtrr_info(unsigned long lfb_va, unsigned long map_size)
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

int g_dac_bits = 6;

void init_dac(void)
{
    RMI rmi;
    int got;

    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F08;  rmi.ebx = 0x0800;   /* try 8-bit */
    dpmi_real_int(0x10, &rmi);

    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F08;  rmi.ebx = 0x0001;   /* BL=01h: query actual width */
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
void vbe_set_palette(int start, int count, const unsigned char *pal)
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

unsigned char *get_bios_font_8x8(void)
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
 * Text rendering into the frame buffer (pitch-stride, 8bpp)
 * -------------------------------------------------------------------------- */

void draw_char_bg(unsigned char *fb, int pitch,
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

void draw_str_bg(unsigned char *fb, int pitch,
                        int x, int y, const char *s,
                        unsigned char fg, unsigned char bg,
                        const unsigned char *font)
{
    for (; *s; s++, x += 8)
        draw_char_bg(fb, pitch, x, y, *s, fg, bg, font);
}

/* 2x scaled character */
void draw_char_2x(unsigned char *fb, int pitch,
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

void draw_str_2x(unsigned char *fb, int pitch,
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

void wait_vsync(void)
{
    while (  inp(VGA_STAT1) & 0x08) {}   /* wait for current vsync to end   */
    while (!(inp(VGA_STAT1) & 0x08)) {}  /* wait for next vsync to begin    */
}

/* --------------------------------------------------------------------------
 * HUD utilities
 * -------------------------------------------------------------------------- */

/* HUD colour indices -- found from natural palette (no overrides needed) */
unsigned char g_hud_dark;    /* darkest palette entry  */
unsigned char g_hud_bright;  /* brightest palette entry */

/* Scan palette for darkest and brightest entries by perceived luminance */
void find_hud_colours(const unsigned char *pal)
{
    int i, best_lo = 999999, best_hi = -1;
    for (i = 0; i < 256; i++) {
        /* Fast approximate luminance: 2R + 5G + B (avoids floats) */
        int lum = 2*pal[i*4+0] + 5*pal[i*4+1] + pal[i*4+2];
        if (lum < best_lo) { best_lo = lum; g_hud_dark   = (unsigned char)i; }
        if (lum > best_hi) { best_hi = lum; g_hud_bright = (unsigned char)i; }
    }
}

/* --------------------------------------------------------------------------
 * Palette debug grid -- 16x16 swatches covering all 256 entries.
 * Drawn into an 8-bpp LFB so the VBE palette hardware does the mapping.
 * Each swatch is SW*SH pixels; hex index label at top-left corner.
 * -------------------------------------------------------------------------- */
void show_palette_grid(unsigned char *lfb, int lfb_pitch,
                              const unsigned char *font)
{
    /* swatch sizes -- chosen so the grid fits comfortably in 1024x768 */
    const int SW = 56;                /* swatch width  (56*16 = 896) */
    const int SH = 40;               /* swatch height (40*16 = 640) */
    const int OX = (VGA_WIDTH  - SW*16) / 2;   /* centre horizontally */
    const int OY = (VGA_HEIGHT - SH*16) / 2;   /* centre vertically   */
    int ix, iy;
    char hex[4];

    /* Clear screen to darkest palette entry */
    {
        int y;
        for (y = 0; y < VGA_HEIGHT; y++)
            memset(lfb + (unsigned long)y * lfb_pitch, g_hud_dark, VGA_WIDTH);
    }

    for (iy = 0; iy < 16; iy++) {
        for (ix = 0; ix < 16; ix++) {
            unsigned char idx = (unsigned char)(iy * 16 + ix);
            int sx = OX + ix * SW;
            int sy = OY + iy * SH;
            int y;

            /* Fill swatch rectangle */
            for (y = 0; y < SH; y++)
                memset(lfb + (unsigned long)(sy + y) * lfb_pitch + sx, idx, SW);

            /* Hex label -- use contrasting text colour */
            {
                unsigned char fg = (idx < 64 || (idx >= 128 && idx < 192))
                                 ? g_hud_bright : g_hud_dark;
                hex[0] = "0123456789ABCDEF"[idx >> 4];
                hex[1] = "0123456789ABCDEF"[idx & 0xF];
                hex[2] = '\0';
                draw_str_bg(lfb, lfb_pitch, sx + 2, sy + 2,
                            hex, fg, idx, font);
            }
        }
    }

    /* Title above grid */
    draw_str_bg(lfb, lfb_pitch, OX, OY - 12,
                "Palette 00-FF   [press any key]",
                g_hud_bright, g_hud_dark, font);
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
 * count must be a multiple of 4 (always true for VGA_WIDTH=1024 scanlines).
 * (Watcom version is declared via #pragma aux in the Watcom section above.)
 */
void wc_memcpy(void *dst, const void *src, unsigned long count)
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

/* Copy frame_buf (pitch=VGA_WIDTH) to LFB (pitch=lfb_pitch).
 * noinline + O2: prevents GCC -O3 from inlining and over-unrolling
 * the rep movsl loop, which adds startup overhead per rep invocation
 * that hurts WC throughput on Prescott/Pentium D. */
#ifdef __DJGPP__
void __attribute__((noinline, optimize("O2,no-unroll-loops")))
blit_to_lfb(unsigned char *lfb, int lfb_pitch, const unsigned char *src)
{
    int y;
    if (lfb_pitch == VGA_WIDTH) {
        wc_memcpy(lfb, src, VGA_PIXELS);
    } else {
        for (y = 0; y < VGA_HEIGHT; y++)
            wc_memcpy(lfb + (unsigned long)y * lfb_pitch,
                       src + (unsigned long)y * VGA_WIDTH, VGA_WIDTH);
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
void blit_to_lfb(unsigned char *lfb, int lfb_pitch,
                         const unsigned char *src)
{
    if (lfb_pitch == VGA_WIDTH) {
        wc_rep_movsd(lfb, src, VGA_PIXELS / 4);
    } else {
        int y;
        for (y = 0; y < VGA_HEIGHT; y++)
            wc_rep_movsd(lfb  + (unsigned long)y * lfb_pitch,
                          src + (unsigned long)y * VGA_WIDTH, VGA_WIDTH / 4);
    }
}
#endif

/* --------------------------------------------------------------------------
 * RDTSC helpers (Pentium+)
 * -------------------------------------------------------------------------- */

/* Benchmark results -- filled by run_benchmark(), shown in summary */
double g_bench_render_ms   = 0.0;
double g_bench_blit_ms     = 0.0;
double g_bench_combined_ms = 0.0;
double g_rdtsc_mhz         = 0.0; /* calibrated CPU MHz via RDTSC */

#ifdef __DJGPP__
void rdtsc_read(unsigned long *lo, unsigned long *hi)
{
    unsigned long _lo, _hi;
    __asm__ __volatile__ ("rdtsc" : "=a"(_lo), "=d"(_hi));
    *lo = _lo;
    *hi = _hi;
}
#else
void rdtsc_read(unsigned long *lo, unsigned long *hi)
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
double tsc_to_ms(unsigned long lo1, unsigned long hi1,
                         unsigned long lo0, unsigned long hi0)
{
    double cycles = (double)hi1 * 4294967296.0 + (double)lo1
                  - (double)hi0 * 4294967296.0 - (double)lo0;
    return (g_rdtsc_mhz > 0.0) ? cycles / (g_rdtsc_mhz * 1000.0) : 0.0;
}

/* Calibrate RDTSC against DOS clock() (18.2 Hz reference).
 * Called in text mode before graphics.  Takes ~220ms. */
void calibrate_rdtsc(void)
{
    /* Use BIOS tick counter at 0x46C (18.2065 Hz) directly -- avoids any
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

    /* Measure over 4 BIOS ticks = exactly 4/18.2065 s ~ 219.7 ms */
    rdtsc_read(&lo0, &hi0);
    bt0 = *bios_ticks;
    while ((bt1 = *bios_ticks) - bt0 < 4) {}
    rdtsc_read(&lo1, &hi1);

    elapsed_s = (double)(bt1 - bt0) / 18.2065;
    cycles    = (double)hi1 * 4294967296.0 + (double)lo1
              - (double)hi0 * 4294967296.0 - (double)lo0;
    g_rdtsc_mhz = (elapsed_s > 0.0) ? cycles / elapsed_s / 1.0e6 : 0.0;
}
