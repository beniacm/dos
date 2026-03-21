/*
 * WCINIT.C - Pre-set MTRR Write-Combining for VBE LFB
 *
 * Tiny ring-0 utility (Watcom + PMODE/W) that programs a CPU variable-range
 * MTRR to mark the VBE linear frame buffer as Write-Combining (WC), then
 * exits.  MTRR settings persist in the CPU until reset or overwritten, so
 * a subsequent DJGPP program (running at ring 3 under CWSDPMI) inherits
 * the WC mapping and gets fast (~700 MB/s) blit speeds instead of the
 * default UC (~80 MB/s).
 *
 * Also patches PAT entry 3 from UC to UC- so that WC passes through
 * regardless of which PTE caching attributes the DPMI host chooses.
 *
 * Build (Watcom):
 *   wcc386 -bt=dos -5r -ox -s -zq WCINIT.C
 *   wlink system pmodew name WCINIT file WCINIT option quiet
 *
 * Usage:
 *   WCINIT              (auto-detect LFB for 1024x768x8)
 *   WCINIT 0x118        (specify VBE mode number)
 *   WCINIT -restore     (undo: restore MTRR/PAT to BIOS defaults)
 *
 * Then run the DJGPP-built plasma:
 *   PLASMGCC.EXE
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <i86.h>

/* --------------------------------------------------------------------------
 * DPMI helpers
 * -------------------------------------------------------------------------- */
typedef struct {
    unsigned long edi, esi, ebp, reserved, ebx, edx, ecx, eax;
    unsigned short flags, es, ds, fs, gs, ip, cs, sp, ss;
} RMI;

static unsigned short g_dos_seg = 0;
static unsigned short g_dos_sel = 0;

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
    g_dos_seg = 0;
    g_dos_sel = 0;
}

static int dpmi_real_int(int intno, RMI *rmi)
{
    union REGS r;
    struct SREGS sr;
    memset(&r, 0, sizeof(r));
    memset(&sr, 0, sizeof(sr));
    r.w.ax = 0x0300;
    r.h.bl = (unsigned char)intno;
    r.h.bh = 0;
    r.w.cx = 0;
    sr.es  = FP_SEG(rmi);
    r.x.edi = FP_OFF(rmi);
    int386x(0x31, &r, &r, &sr);
    return !(r.x.cflag);
}

/* --------------------------------------------------------------------------
 * VBE structures (minimal)
 * -------------------------------------------------------------------------- */
#pragma pack(push, 1)
typedef struct {
    char           sig[4];
    unsigned short version;
    unsigned long  oem_str;
    unsigned long  capabilities;
    unsigned long  mode_list;
    unsigned short total_memory;
    char           rest[238 + 256];
} VBEInfo;

typedef struct {
    unsigned short attributes;
    unsigned char  win_a_attr, win_b_attr;
    unsigned short win_granularity, win_size;
    unsigned short win_a_seg, win_b_seg;
    unsigned long  win_func;
    unsigned short pitch;
    unsigned short width, height;
    unsigned char  char_width, char_height;
    unsigned char  planes, bpp, banks, memory_model;
    unsigned char  bank_size, image_pages, reserved1;
    unsigned char  red_mask, red_pos;
    unsigned char  green_mask, green_pos;
    unsigned char  blue_mask, blue_pos;
    unsigned char  rsvd_mask, rsvd_pos;
    unsigned char  direct_color_info;
    unsigned long  lfb_phys;
    char           rest2[212];
} VBEModeInfo;
#pragma pack(pop)

/* --------------------------------------------------------------------------
 * Inline asm: ring-0 CPU instructions
 * -------------------------------------------------------------------------- */
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

/* --------------------------------------------------------------------------
 * MSR addresses & MTRR types
 * -------------------------------------------------------------------------- */
#define MSR_MTRRCAP         0xFE
#define MSR_MTRR_PHYSBASE0  0x200
#define MSR_MTRR_PHYSMASK0  0x201
#define MSR_MTRR_DEF_TYPE   0x2FF
#define MSR_PAT             0x277

#define MTRR_TYPE_UC  0
#define MTRR_TYPE_WC  1
#define MTRR_TYPE_WB  6

/* --------------------------------------------------------------------------
 * VBE queries
 * -------------------------------------------------------------------------- */
static int vbe_get_info(VBEInfo *info)
{
    RMI rmi;
    unsigned long buf = (unsigned long)g_dos_seg << 4;
    memset((void *)buf, 0, 512);
    memcpy((void *)buf, "VBE2", 4);
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F00;
    rmi.es  = g_dos_seg;
    rmi.edi = 0;
    if (!dpmi_real_int(0x10, &rmi)) return 0;
    if ((rmi.eax & 0xFFFF) != 0x004F) return 0;
    memcpy(info, (void *)buf, sizeof(VBEInfo));
    return 1;
}

static int vbe_get_mode_info(unsigned short mode, VBEModeInfo *mi)
{
    RMI rmi;
    unsigned long buf = (unsigned long)g_dos_seg << 4;
    memset((void *)(buf + 256), 0, 256);
    memset(&rmi, 0, sizeof(rmi));
    rmi.eax = 0x4F01;
    rmi.ecx = mode;
    rmi.es  = g_dos_seg;
    rmi.edi = 256;
    if (!dpmi_real_int(0x10, &rmi)) return 0;
    if ((rmi.eax & 0xFFFF) != 0x004F) return 0;
    memcpy(mi, (void *)(buf + 256), sizeof(VBEModeInfo));
    return 1;
}

/* Find the VBE mode number for a given resolution + bpp */
static unsigned short find_mode(int want_w, int want_h, int want_bpp)
{
    VBEInfo vi;
    unsigned short mode_list[512];
    unsigned short *src;
    unsigned long ml_seg, ml_off;
    int i, count;

    if (!vbe_get_info(&vi)) return 0;

    ml_seg = (vi.mode_list >> 16) & 0xFFFF;
    ml_off =  vi.mode_list        & 0xFFFF;
    src    = (unsigned short *)((ml_seg << 4) + ml_off);

    for (count = 0; count < 512 && src[count] != 0xFFFF; count++)
        mode_list[count] = src[count];

    for (i = 0; i < count; i++) {
        VBEModeInfo mi;
        if (!vbe_get_mode_info(mode_list[i], &mi)) continue;
        if (mi.width == want_w && mi.height == want_h &&
            mi.bpp == want_bpp && (mi.attributes & 0x80))
            return mode_list[i];
    }
    return 0;
}

/* --------------------------------------------------------------------------
 * Power-of-two rounding (for MTRR mask)
 * -------------------------------------------------------------------------- */
static unsigned long next_pow2(unsigned long v)
{
    v--;
    v |= v >> 1;  v |= v >> 2;  v |= v >> 4;
    v |= v >> 8;  v |= v >> 16;
    return v + 1;
}

/* --------------------------------------------------------------------------
 * MTRR helpers
 * -------------------------------------------------------------------------- */
static const char *type_name(int t)
{
    switch (t) {
        case 0: return "UC";
        case 1: return "WC";
        case 4: return "WT";
        case 5: return "WP";
        case 6: return "WB";
        case 7: return "UC-";
        default: return "??";
    }
}

/*
 * Set up MTRR Write-Combining for [phys, phys + vram_bytes).
 * Returns slot number on success, -1 if already WC, -2 on failure.
 *
 * Handles the Intel overlap rule: UC (type 0) beats WC (type 1).
 * If a BIOS UC entry covers our LFB, we replace it in-place with WC.
 */
static int setup_mtrr_wc(unsigned long phys, unsigned long vram_bytes,
                          int *replaced_uc)
{
    unsigned long cap_lo, size, mask_val;
    int num_var, i;
    int free_slot = -1, replace_slot = -1;

    *replaced_uc = 0;

    if (!_has_cpuid()) return -2;
    if (!(_cpuid1_edx() & (1UL << 12))) return -2;

    cap_lo = _rdmsr_lo(MSR_MTRRCAP);
    num_var = (int)(cap_lo & 0xFF);
    if (!(cap_lo & (1UL << 10))) return -2;   /* WC not supported */
    if (num_var == 0) return -2;

    size = next_pow2(vram_bytes);
    if (phys & (size - 1)) return -2;          /* not naturally aligned */
    mask_val = ~(size - 1) & 0xFFFFF000UL;

    for (i = 0; i < num_var && i < 16; i++) {
        unsigned long mlo = _rdmsr_lo(MSR_MTRR_PHYSMASK0 + i * 2);
        unsigned long blo = _rdmsr_lo(MSR_MTRR_PHYSBASE0 + i * 2);

        if (!(mlo & 0x800)) {
            if (free_slot < 0) free_slot = i;
            continue;
        }

        if ((phys & (mlo & 0xFFFFF000UL)) != (blo & 0xFFFFF000UL))
            continue;

        if ((blo & 0xFF) == MTRR_TYPE_WC)
            return -1;   /* already WC */

        if ((blo & 0xFF) == MTRR_TYPE_UC)
            replace_slot = i;
    }

    i = (replace_slot >= 0) ? replace_slot : free_slot;
    if (i < 0) return -2;

    *replaced_uc = (replace_slot >= 0);

    _disable();
    _wbinvd();
    _wrmsr_3(MSR_MTRR_PHYSBASE0 + i * 2,
             (phys & 0xFFFFF000UL) | MTRR_TYPE_WC, 0);
    _wrmsr_3(MSR_MTRR_PHYSMASK0 + i * 2,
             mask_val | 0x800, 0);
    _wbinvd();
    _enable();

    return i;
}

/*
 * Fix PAT entry 3: change from UC (type 0) to UC- (type 7).
 *
 * Default PAT entry 3 (PCD=1, PWT=1, PAT=0) is UC (strong uncacheable).
 * UC overrides MTRR WC, blocking write-combining even when the MTRR is set.
 * UC- (type 7) defers to the MTRR, allowing WC to take effect.
 *
 * This matters when a DPMI host maps physical memory with PCD=1, PWT=1
 * in the page table entry (common for MMIO regions).
 *
 * Returns: 1 = patched, 0 = already OK, -1 = PAT not supported
 */
static int fix_pat_entry3(void)
{
    unsigned long pat_lo, pat_hi;
    unsigned long entry3;

    if (!(_cpuid1_edx() & (1UL << 16))) return -1;

    pat_lo = _rdmsr_lo(MSR_PAT);
    pat_hi = _rdmsr_hi(MSR_PAT);
    entry3 = (pat_lo >> 24) & 7;

    if (entry3 != 0) return 0;   /* not UC, leave it alone */

    /* Replace UC (0) with UC- (7) in entry 3 (bits 31:24 of lo dword) */
    pat_lo = (pat_lo & 0x00FFFFFFUL) | (7UL << 24);

    _disable();
    _wbinvd();
    _wrmsr_3(MSR_PAT, pat_lo, pat_hi);
    _wbinvd();
    _enable();

    return 1;
}

/*
 * Restore PAT entry 3 to its Intel default: UC (type 0).
 */
static void restore_pat_entry3(void)
{
    unsigned long pat_lo, pat_hi;

    if (!(_cpuid1_edx() & (1UL << 16))) return;

    pat_lo = _rdmsr_lo(MSR_PAT);
    pat_hi = _rdmsr_hi(MSR_PAT);

    /* Set entry 3 back to UC (0) */
    pat_lo = (pat_lo & 0x00FFFFFFUL) | (0UL << 24);

    _disable();
    _wbinvd();
    _wrmsr_3(MSR_PAT, pat_lo, pat_hi);
    _wbinvd();
    _enable();
}

/*
 * Scan all variable MTRRs and clear any WC entries.
 * Returns the number of WC entries cleared.
 */
static int clear_wc_mtrrs(void)
{
    unsigned long cap_lo;
    int num_var, i, cleared = 0;

    if (!_has_cpuid()) return 0;
    if (!(_cpuid1_edx() & (1UL << 12))) return 0;

    cap_lo = _rdmsr_lo(MSR_MTRRCAP);
    num_var = (int)(cap_lo & 0xFF);

    for (i = 0; i < num_var && i < 16; i++) {
        unsigned long blo = _rdmsr_lo(MSR_MTRR_PHYSBASE0 + i * 2);
        unsigned long mlo = _rdmsr_lo(MSR_MTRR_PHYSMASK0 + i * 2);

        if (!(mlo & 0x800)) continue;           /* slot not active */
        if ((blo & 0xFF) != MTRR_TYPE_WC) continue;  /* not WC */

        /* Disable this slot */
        _disable();
        _wbinvd();
        _wrmsr_3(MSR_MTRR_PHYSBASE0 + i * 2, 0, 0);
        _wrmsr_3(MSR_MTRR_PHYSMASK0 + i * 2, 0, 0);
        _wbinvd();
        _enable();

        printf("  Cleared WC slot [%d]: base 0x%08lX\n",
               i, blo & 0xFFFFF000UL);
        cleared++;
    }
    return cleared;
}

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    VBEInfo vi;
    VBEModeInfo vbmi;
    unsigned short target_mode = 0;
    unsigned long lfb_phys, total_vram, wc_size;
    int ring, slot, replaced, pat_ok;
    int do_restore = 0;
    int i;

    printf("WCINIT - MTRR Write-Combining Setup for VBE LFB\n");
    printf("================================================\n");

    /* Parse args */
    for (i = 1; i < argc; i++) {
        if (stricmp(argv[i], "-restore") == 0 ||
            stricmp(argv[i], "/restore") == 0) {
            do_restore = 1;
        } else {
            target_mode = (unsigned short)strtol(argv[i], NULL, 0);
        }
    }

    /* Ring check */
    ring = _get_cs() & 3;
    if (ring != 0) {
        printf("ERROR: Ring %d - need ring 0 (PMODE/W).\n", ring);
        printf("Build: wlink system pmodew name WCINIT ...\n");
        return 1;
    }
    printf("Ring          : 0 (PMODE/W)\n");

    /* Restore mode: undo WC MTRRs and PAT fix, then exit */
    if (do_restore) {
        int n;
        printf("Mode          : RESTORE\n\n");
        n = clear_wc_mtrrs();
        printf("Cleared %d WC MTRR slot(s).\n", n);
        restore_pat_entry3();
        printf("PAT entry 3 restored to UC (Intel default).\n");
        printf("\nDone - MTRR/PAT restored to BIOS defaults.\n");
        return 0;
    }

    /* Alloc DOS memory for VBE calls */
    if (!dpmi_alloc_dos(128)) {
        printf("DPMI: cannot allocate conventional memory\n");
        return 1;
    }

    /* Get VBE info */
    if (!vbe_get_info(&vi)) {
        printf("VBE info query failed - no VESA BIOS?\n");
        dpmi_free_dos();
        return 1;
    }
    total_vram = (unsigned long)vi.total_memory * 65536UL;
    printf("VBE           : %d.%d, %lu KB VRAM\n",
           vi.version >> 8, vi.version & 0xFF, total_vram >> 10);

    /* Find mode */
    if (target_mode == 0) {
        target_mode = find_mode(1024, 768, 8);
        if (target_mode == 0) {
            printf("Cannot find 1024x768x8 mode\n");
            dpmi_free_dos();
            return 1;
        }
    }

    if (!vbe_get_mode_info(target_mode, &vbmi)) {
        printf("Mode 0x%03X not available\n", target_mode);
        dpmi_free_dos();
        return 1;
    }

    lfb_phys = vbmi.lfb_phys;
    wc_size  = next_pow2(total_vram);
    printf("Mode          : 0x%03X (%dx%d %dbpp)\n",
           target_mode, vbmi.width, vbmi.height, vbmi.bpp);
    printf("LFB phys      : 0x%08lX\n", lfb_phys);
    printf("VRAM          : %lu MB, WC range: %lu MB\n",
           total_vram >> 20, wc_size >> 20);

    dpmi_free_dos();

    /* Set MTRR WC */
    slot = setup_mtrr_wc(lfb_phys, total_vram, &replaced);
    if (slot >= 0) {
        if (replaced)
            printf("MTRR WC       : SET slot [%d] (replaced BIOS UC)\n", slot);
        else
            printf("MTRR WC       : SET slot [%d] (new entry)\n", slot);
    } else if (slot == -1) {
        printf("MTRR WC       : already active (nothing to do)\n");
    } else {
        printf("MTRR WC       : FAILED (no CPUID/MTRR/free slot)\n");
        return 1;
    }

    /* Fix PAT entry 3 */
    pat_ok = fix_pat_entry3();
    if (pat_ok == 1)
        printf("PAT fix       : entry 3 UC -> UC- (WC passthrough)\n");
    else if (pat_ok == 0)
        printf("PAT fix       : not needed (entry 3 already OK)\n");
    else
        printf("PAT fix       : PAT not supported\n");

    printf("\nDone - WC is active. Run your DJGPP program now.\n");
    printf("MTRRs persist until reboot or WCINIT -restore.\n");
    return 0;
}
