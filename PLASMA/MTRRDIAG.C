/*
 * MTRRDIAG.C - MTRR / PAT / Page Table Diagnostic Tool
 *
 * Dumps the full memory-type resolution chain for VBE LFB regions:
 *   1. All variable-range MTRRs
 *   2. Default MTRR type (IA32_MTRR_DEF_TYPE MSR 0x2FF)
 *   3. PAT MSR entries (IA32_PAT MSR 0x277)
 *   4. Page Directory / Page Table entries for the LFB virtual address
 *   5. Effective memory type (MTRR + PAT combination)
 *
 * Requires PMODE/W (ring 0) for MSR and CR access.
 *
 * Build:
 *   wcc386 -bt=dos -5r -ox -s -zq MTRRDIAG.C
 *   wlink system pmodew name MTRRDIAG file MTRRDIAG option quiet
 *
 * Usage:
 *   MTRRDIAG [mode_hex]
 *   e.g. MTRRDIAG 0x105   (defaults to 0x105 = 1024x768x8)
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

static void *dpmi_map_physical(unsigned long phys, unsigned long size)
{
    union REGS r;
    r.w.ax  = 0x0800;
    r.w.bx  = (unsigned short)(phys >> 16);
    r.w.cx  = (unsigned short)(phys & 0xFFFF);
    r.w.si  = (unsigned short)(size >> 16);
    r.w.di  = (unsigned short)(size & 0xFFFF);
    int386(0x31, &r, &r);
    if (r.x.cflag) return NULL;
    return (void *)(((unsigned long)r.w.bx << 16) | r.w.cx);
}

/* --------------------------------------------------------------------------
 * VBE structures (minimal)
 * -------------------------------------------------------------------------- */
#pragma pack(push, 1)
typedef struct {
    char        sig[4];
    unsigned short version;
    unsigned long  oem_str;
    unsigned long  capabilities;
    unsigned long  mode_list;
    unsigned short total_memory;
    unsigned short oem_sw_rev;
    unsigned long  oem_vendor;
    unsigned long  oem_product;
    unsigned long  oem_product_rev;
    char           reserved[222];
    char           oem_data[256];
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
    unsigned long  offscreen_addr;
    unsigned short offscreen_size;
    unsigned short lin_pitch;
    /* VBE 3.0 fields */
    unsigned char  bnk_image_pages;
    unsigned char  lin_image_pages;
    unsigned char  lin_red_mask, lin_red_pos;
    unsigned char  lin_green_mask, lin_green_pos;
    unsigned char  lin_blue_mask, lin_blue_pos;
    unsigned char  lin_rsvd_mask, lin_rsvd_pos;
    unsigned long  max_pixel_clock;
    char           reserved2[190];
} VBEModeInfo;
#pragma pack(pop)

/* --------------------------------------------------------------------------
 * Inline asm helpers (ring 0 only)
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

unsigned long _read_cr0(void);
#pragma aux _read_cr0 = \
    "db 0Fh, 20h, 0C0h"    \
    value [eax]

unsigned long _read_cr3(void);
#pragma aux _read_cr3 = \
    "db 0Fh, 20h, 0D8h"    \
    value [eax]

unsigned long _read_cr4(void);
#pragma aux _read_cr4 = \
    "db 0Fh, 20h, 0E0h"    \
    value [eax]

/* --------------------------------------------------------------------------
 * MSR addresses
 * -------------------------------------------------------------------------- */
#define MSR_MTRRCAP         0xFE
#define MSR_MTRR_PHYSBASE0  0x200
#define MSR_MTRR_PHYSMASK0  0x201
#define MSR_MTRR_DEF_TYPE   0x2FF
#define MSR_PAT             0x277

/* --------------------------------------------------------------------------
 * Type name helpers
 * -------------------------------------------------------------------------- */
static const char *type_name(unsigned long t)
{
    switch (t & 0xFF) {
        case 0: return "UC  (Uncacheable)";
        case 1: return "WC  (Write-Combining)";
        case 4: return "WT  (Write-Through)";
        case 5: return "WP  (Write-Protect)";
        case 6: return "WB  (Write-Back)";
        case 7: return "UC- (Uncacheable-minus)";
        default: return "??  (Unknown)";
    }
}

static const char *type_short(unsigned long t)
{
    switch (t & 7) {
        case 0: return "UC";
        case 1: return "WC";
        case 4: return "WT";
        case 5: return "WP";
        case 6: return "WB";
        default: return "??";
    }
}

/* --------------------------------------------------------------------------
 * Effective memory type from MTRR + PAT combination
 * Intel SDM Vol 3A, Table 11-7
 * -------------------------------------------------------------------------- */
static const char *effective_type(int mtrr_type, int pat_type)
{
    /* UC (strong, type 0) always wins from either side */
    if (pat_type == 0 || mtrr_type == 0) return "UC";

    /* UC- from PAT (index 2 or 6 in default table) allows MTRR to win */
    /* We represent UC- as value 7 for this function */
    if (pat_type == 7) { /* UC- */
        switch (mtrr_type) {
            case 1: return "WC";
            case 4: return "WT";
            case 5: return "WP";
            case 6: return "WB";
            default: return "UC";
        }
    }

    /* WC from MTRR overrides most PAT types */
    if (mtrr_type == 1) return "WC";

    /* WB + WT = WT, etc. */
    if (pat_type == 4 || mtrr_type == 4) return "WT";
    if (pat_type == 5 || mtrr_type == 5) return "WP";
    if (pat_type == 6 && mtrr_type == 6) return "WB";

    return "UC";
}

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

/* --------------------------------------------------------------------------
 * Main
 * -------------------------------------------------------------------------- */
int main(int argc, char *argv[])
{
    VBEInfo vi;
    VBEModeInfo vbmi;
    unsigned short target_mode = 0x105; /* 1024x768x8 default */
    unsigned long lfb_phys = 0;
    unsigned long total_vram;
    void *lfb = NULL;
    unsigned long lfb_va;
    unsigned long cr0, cr3, cr4;
    unsigned long cap_lo, def_lo;
    unsigned long pat_lo, pat_hi;
    int num_var, i;
    int ring;
    unsigned long edx_feat;

    if (argc > 1) {
        target_mode = (unsigned short)strtol(argv[1], NULL, 0);
    }

    printf("MTRRDIAG - MTRR / PAT / Page Table Diagnostic\n");
    printf("==============================================\n\n");

    /* Check ring level */
    ring = _get_cs() & 3;
    printf("Ring level    : %d %s\n", ring,
           ring == 0 ? "(PMODE/W - full access)" :
                       "(ring 3 - MSR/CR access will fail!)");
    if (ring != 0) {
        printf("\nERROR: This tool requires ring 0 (PMODE/W).\n");
        printf("Rebuild with: wlink system pmodew ...\n");
        return 1;
    }

    /* CPUID feature flags */
    if (!_has_cpuid()) {
        printf("CPUID not supported - very old CPU\n");
        return 1;
    }
    edx_feat = _cpuid1_edx();
    printf("CPUID.1.EDX   : 0x%08lX\n", edx_feat);
    printf("  MTRR (bit12): %s\n", (edx_feat & (1UL << 12)) ? "YES" : "NO");
    printf("  PAT  (bit16): %s\n", (edx_feat & (1UL << 16)) ? "YES" : "NO");
    printf("  PSE  (bit 3): %s\n", (edx_feat & (1UL <<  3)) ? "YES" : "NO");
    printf("\n");

    /* Control registers */
    cr0 = _read_cr0();
    cr3 = _read_cr3();
    cr4 = _read_cr4();
    printf("CR0           : 0x%08lX  CD=%d NW=%d PG=%d\n",
           cr0, (int)((cr0 >> 30) & 1), (int)((cr0 >> 29) & 1),
           (int)((cr0 >> 31) & 1));
    printf("CR3           : 0x%08lX  (page directory base)\n", cr3);
    printf("CR4           : 0x%08lX  PSE=%d PAE=%d PGE=%d\n",
           cr4, (int)((cr4 >> 4) & 1), (int)((cr4 >> 5) & 1),
           (int)((cr4 >> 7) & 1));
    printf("\n");

    /* ---- MTRR state ---------------------------------------------------- */
    if (!(edx_feat & (1UL << 12))) {
        printf("MTRR not supported.\n");
    } else {
        cap_lo = _rdmsr_lo(MSR_MTRRCAP);
        num_var = (int)(cap_lo & 0xFF);
        def_lo = _rdmsr_lo(MSR_MTRR_DEF_TYPE);

        printf("IA32_MTRRCAP (0xFE)\n");
        printf("  Variable MTRRs : %d\n", num_var);
        printf("  WC supported   : %s\n",
               (cap_lo & (1UL << 10)) ? "YES" : "NO");
        printf("  Fixed supported: %s\n",
               (cap_lo & (1UL <<  8)) ? "YES" : "NO");
        printf("\n");

        printf("IA32_MTRR_DEF_TYPE (0x2FF) = 0x%04lX_%04lX\n",
               _rdmsr_hi(MSR_MTRR_DEF_TYPE), def_lo);
        printf("  Default type   : %s\n", type_name(def_lo));
        printf("  MTRR enable    : %s (bit 11)\n",
               (def_lo & (1UL << 11)) ? "YES" : "NO");
        printf("  Fixed enable   : %s (bit 10)\n",
               (def_lo & (1UL << 10)) ? "YES" : "NO");
        printf("\n");

        printf("Variable-range MTRRs:\n");
        printf("  Slot  Base         Size       Type  Mask\n");
        printf("  ----  ----------   --------   ----  ----------\n");
        for (i = 0; i < num_var && i < 16; i++) {
            unsigned long blo = _rdmsr_lo(MSR_MTRR_PHYSBASE0 + i * 2);
            unsigned long bhi = _rdmsr_hi(MSR_MTRR_PHYSBASE0 + i * 2);
            unsigned long mlo = _rdmsr_lo(MSR_MTRR_PHYSMASK0 + i * 2);
            unsigned long mhi = _rdmsr_hi(MSR_MTRR_PHYSMASK0 + i * 2);
            int valid = (mlo >> 11) & 1;

            if (valid) {
                unsigned long base = blo & 0xFFFFF000UL;
                unsigned long mask = mlo & 0xFFFFF000UL;
                unsigned long sz = (~mask + 1) & 0xFFFFF000UL;
                printf("  [%2d]  0x%08lX   %4luMB     %s   0x%08lX\n",
                       i, base, sz >> 20, type_short(blo), mask);
            } else {
                printf("  [%2d]  <free>\n", i);
            }
        }
        printf("\n");
    }

    /* ---- PAT state ----------------------------------------------------- */
    if (edx_feat & (1UL << 16)) {
        pat_lo = _rdmsr_lo(MSR_PAT);
        pat_hi = _rdmsr_hi(MSR_PAT);
        printf("IA32_PAT (0x277) = 0x%08lX_%08lX\n", pat_hi, pat_lo);
        printf("  PAT entries (PWT + 2*PCD + 4*PAT_bit = index):\n");
        for (i = 0; i < 4; i++) {
            unsigned long t = (pat_lo >> (i * 8)) & 7;
            printf("    [%d] PCD=%d PWT=%d PAT=0 -> %s\n",
                   i, (i >> 1) & 1, i & 1, type_name(t));
        }
        for (i = 0; i < 4; i++) {
            unsigned long t = (pat_hi >> (i * 8)) & 7;
            printf("    [%d] PCD=%d PWT=%d PAT=1 -> %s\n",
                   i + 4, (i >> 1) & 1, i & 1, type_name(t));
        }
        printf("\n");
        printf("  KEY: Index 2 (PCD=1,PWT=0) is UC-.  UC- allows MTRR WC passthrough.\n");
        printf("       Index 3 (PCD=1,PWT=1) is UC.   UC BLOCKS MTRR WC.\n");
        printf("\n");
    } else {
        printf("PAT not supported by this CPU.\n\n");
    }

    /* ---- VBE / LFB ----------------------------------------------------- */
    printf("VBE / LFB Information:\n");

    if (!dpmi_alloc_dos(128)) {
        printf("  DPMI alloc failed\n");
        return 1;
    }

    if (!vbe_get_info(&vi)) {
        printf("  VBE info query failed\n");
        dpmi_free_dos();
        return 1;
    }

    total_vram = (unsigned long)vi.total_memory * 65536UL;
    printf("  VBE version : %d.%d\n", vi.version >> 8, vi.version & 0xFF);
    printf("  Total VRAM  : %lu KB\n", total_vram >> 10);

    printf("  Target mode : 0x%03X\n", target_mode);
    if (!vbe_get_mode_info(target_mode, &vbmi)) {
        printf("  Mode info query failed for 0x%03X\n", target_mode);
        dpmi_free_dos();
        return 1;
    }

    lfb_phys = vbmi.lfb_phys;
    printf("  Resolution  : %dx%d %dbpp  pitch=%d\n",
           vbmi.width, vbmi.height, vbmi.bpp, vbmi.pitch);
    printf("  LFB phys    : 0x%08lX\n", lfb_phys);
    printf("  Attributes  : 0x%04X  LFB=%s\n",
           vbmi.attributes,
           (vbmi.attributes & 0x80) ? "yes" : "NO");

    /* Map LFB */
    lfb = dpmi_map_physical(lfb_phys, 0x1000); /* just 1 page for diagnosis */
    if (!lfb) {
        printf("  DPMI map failed for 0x%08lX\n", lfb_phys);
        dpmi_free_dos();
        return 1;
    }
    lfb_va = (unsigned long)lfb;
    printf("  LFB virt    : 0x%08lX\n", lfb_va);
    printf("\n");

    /* ---- Page table / PAT analysis for LFB ----------------------------- */
    printf("Page Table / PAT Analysis for LFB VA 0x%08lX:\n", lfb_va);
    if (!(cr0 & (1UL << 31))) {
        printf("  Paging is OFF (CR0.PG=0)\n");
        printf("  MTRR type is effective directly (no PTE/PAT override).\n");
    } else {
        printf("  Paging is ON (CR0.PG=1)\n");
        printf("  PMODE/W typically sets PCD=1,PWT=1 for MMIO mappings.\n");
        printf("  PCD=1,PWT=1,PAT=0 -> PAT index 3\n");
        if (edx_feat & (1UL << 16)) {
            unsigned long entry3 = (pat_lo >> 24) & 7;
            printf("  PAT entry[3] = %s\n", type_name(entry3));
            if (entry3 == 0)
                printf("  ** PAT entry 3 = UC (strong) - blocks MTRR WC! **\n");
            else if (entry3 == 7)
                printf("  PAT entry 3 = UC- - MTRR WC passes through.\n");
            else if (entry3 == 1)
                printf("  PAT entry 3 = WC - write-combining active.\n");
        }
        printf("  (Skipping page table walk: CR3 phys addr not\n");
        printf("   linearly accessible in PMODE/W.)\n");
    }
    printf("\n");

    /* ---- MTRR match for LFB phys --------------------------------------- */
    /* ---- MTRR match for LFB phys --------------------------------------- */
    if (edx_feat & (1UL << 12)) {
        int mtrr_type = -1;
        int match_slot = -1;
        unsigned long cap_lo2, def_lo2;
        int num_var2, i2;

        cap_lo2 = _rdmsr_lo(MSR_MTRRCAP);
        num_var2 = (int)(cap_lo2 & 0xFF);
        def_lo2 = _rdmsr_lo(MSR_MTRR_DEF_TYPE);

        for (i2 = 0; i2 < num_var2 && i2 < 16; i2++) {
            unsigned long blo = _rdmsr_lo(MSR_MTRR_PHYSBASE0 + i2 * 2);
            unsigned long mlo = _rdmsr_lo(MSR_MTRR_PHYSMASK0 + i2 * 2);
            if (!(mlo & 0x800)) continue;
            if ((lfb_phys & (mlo & 0xFFFFF000UL)) ==
                (blo & 0xFFFFF000UL)) {
                mtrr_type = (int)(blo & 0xFF);
                match_slot = i2;
            }
        }

        printf("MTRR resolution for LFB phys 0x%08lX:\n", lfb_phys);
        if (match_slot >= 0) {
            printf("  Matched slot [%d]: type = %s\n",
                   match_slot, type_name(mtrr_type));
        } else {
            mtrr_type = (int)(def_lo2 & 0xFF);
            printf("  No variable MTRR match -> default type: %s\n",
                   type_name(mtrr_type));
        }

        /* Compute effective type based on PAT entry 3 (assumed PCD=1,PWT=1) */
        if (edx_feat & (1UL << 16)) {
            int pat_idx = (cr0 & (1UL << 31)) ? 3 : 0;
            int pat_type_raw;
            const char *eff;

            if (!(cr0 & (1UL << 31))) {
                /* Paging OFF: MTRR type directly effective */
                eff = type_name(mtrr_type);
            } else {
                /* Paging ON: use PAT entry 3 */
                pat_type_raw = (int)((pat_lo >> (pat_idx * 8)) & 7);
                eff = effective_type(mtrr_type, pat_type_raw);
            }

            printf("\n");
            printf("  EFFECTIVE MEMORY TYPE:\n");
            printf("    MTRR type      : %s\n", type_name(mtrr_type));
            if (cr0 & (1UL << 31)) {
                printf("    PAT entry[3]   : %s\n",
                       type_name((pat_lo >> 24) & 7));
            } else {
                printf("    Paging         : OFF (MTRR type effective directly)\n");
            }
            printf("    Combined       : %s\n", eff);
            printf("\n");

            if (mtrr_type == 1 && strcmp(eff, "UC") == 0) {
                printf("  *** PROBLEM DETECTED ***\n");
                printf("  MTRR is WC but PAT entry 3 = UC (strong) blocks it.\n");
                printf("  FIX: Run PLASMA.EXE which changes PAT entry 3 to UC-.\n");
            } else if (mtrr_type == 1 && (strcmp(eff, "WC") == 0 ||
                       strcmp(eff, type_name(mtrr_type)) == 0)) {
                printf("  WC is active - write-combining should be working.\n");
            }
        }
    }

    printf("\n==============================================\n");
    printf("Done.\n");

    dpmi_free_dos();
    return 0;
}
