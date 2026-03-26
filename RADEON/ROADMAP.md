# Radeon X1300 Pro DOS Demo — Improvement Roadmap

Target hardware: ATI Radeon X1300 Pro (RV515 / RV516), OpenWatcom 2.0, DOS/4GW or PMODE/W.
Reference drivers: `xf86-video-ati` (radeon_accel.c, radeon_textured_videofuncs.c),
Haiku `src/add-ons/accelerants/radeon_hd/`.

---

## 1  Known code gaps (fix before adding features)

### 1.1  RBLIT.C — missing `gpu_mc_wait_idle()` after pipe config

**File:** `RBLIT.C`, inside `gpu_init_2d()`, after the second `gpu_wait_idle()` call.

`RADEON.C` calls both `gpu_wait_idle()` and `gpu_mc_wait_idle()` after the
`R500_DYN_SCLK_PWMEM_PIPE` / `R_GB_TILE_CONFIG` setup phase (lines 804–806).
`RBLIT.C` only calls `gpu_wait_idle()`.  The MC idle wait is in Linux
`rv515_gpu_init()` for a reason: the tile-config write touches the memory
controller, and the 2D engine reset immediately after it can race if the MC
hasn't settled.

**Fix:** add `gpu_mc_wait_idle()` in RBLIT.C after the second `gpu_wait_idle()`
call in its `gpu_init_2d`, matching the RADEON.C sequence exactly.

### 1.2  RBLIT.C — `R_SC_BOTTOM_RIGHT` uses max sentinel, not screen size

`RBLIT.C` writes `(0x1FFFUL << 16) | 0x1FFFUL` to `R_SC_BOTTOM_RIGHT`.
`RADEON.C` writes the actual `g_yres / g_xres`.  For diagnostic tests that
verify clipping behaviour the sentinel suppresses real clip errors.  Consider
adding a separate `test_scissor_clamp()` test that temporarily restores the
correct screen-sized scissor, runs a fill beyond `g_xres`, and verifies the
overshoot pixels are untouched.

---

## 2  Hardware cursor (no CP ring required)

**Complexity:** low — 6 MMIO register writes.
**Reference:** `xf86-video-ati/src/radeon_reg.h` lines 3695–3706.

The AVIVO display controller has an independent 64×64 ARGB cursor plane.
It sits above the framebuffer in the display pipeline and does not interact
with the 2D FIFO engine.

### Registers

```c
#define R_D1CUR_CONTROL          0x6400
#define   D1CUR_ENABLE           (1UL << 0)
#define   D1CUR_MODE_24BPP       (0x2UL << 8)   /* ARGB8888, pre-multiplied */
#define R_D1CUR_SURFACE_ADDRESS  0x6408  /* VRAM byte offset of 64×64 bitmap */
#define R_D1CUR_SIZE             0x6410  /* bits[15:0]=width-1, bits[31:16]=height-1 */
#define R_D1CUR_POSITION         0x6414  /* bits[15:0]=X, bits[31:16]=Y */
#define R_D1CUR_HOT_SPOT         0x6418  /* bits[15:0]=hot_x, bits[31:16]=hot_y */
#define R_D1CUR_UPDATE           0x6424
#define   D1CUR_UPDATE_LOCK      (1UL << 16)
```

### Enable sequence

```c
/* Reserve 64*64*4 = 16 384 bytes in VRAM above the framebuffer pages */
unsigned long cursor_vram_off = <page-aligned offset above display pages>;

/* Write 64x64 ARGB8888 bitmap into g_lfb + cursor_vram_off */

/* Lock, program, unlock (same protocol as D1GRPH page flip) */
wreg(R_D1CUR_UPDATE, D1CUR_UPDATE_LOCK);
wreg(R_D1CUR_SURFACE_ADDRESS, g_fb_location + cursor_vram_off);
wreg(R_D1CUR_SIZE,    (63UL << 16) | 63UL);   /* 64×64 */
wreg(R_D1CUR_HOT_SPOT, 0);
wreg(R_D1CUR_POSITION, ((unsigned long)y << 16) | (unsigned long)x);
wreg(R_D1CUR_CONTROL, D1CUR_ENABLE | D1CUR_MODE_24BPP);
wreg(R_D1CUR_UPDATE, 0);   /* unlock */
```

### Move cursor each frame

```c
wreg(R_D1CUR_POSITION, ((unsigned long)new_y << 16) | (unsigned long)new_x);
```

No lock needed for position-only updates; a single write is atomic.

---

## 3  CP ring buffer setup (prerequisite for steps 4 and 5)

**Complexity:** medium — ~40 lines of new init code, no new DPMI techniques needed.
**Reference:** `xf86-video-ati/src/radeon_reg.h` lines 3132–3141,
Linux `drivers/gpu/drm/radeon/r100.c` `r100_cp_init()`.

RV515 uses the R300-era CP registers at 0x0700 (not R600's 0xC100).

### New registers

```c
#define R_CP_RB_BASE      0x0700  /* physical addr of ring, bits[31:8] → bits[23:0] of reg */
#define R_CP_RB_CNTL      0x0704  /* ring size (log2 dwords), writeback enable */
#define R_CP_RB_RPTR_ADDR 0x070C  /* physical addr where HW writes its read pointer */
#define R_CP_RB_RPTR      0x0710  /* read pointer (dword index) */
#define R_CP_RB_WPTR      0x0714  /* write pointer (dword index) — write here to kick CP */
#define R_CP_CSQ_CNTL     0x0740  /* 0 = ring mode, 0xC0000000 = CSQ mode */
#define R_CP_ME_CNTL      0x07D0  /* microengine control (R300-era, not R600's 0x86D8) */
#define   ME_HALT         (1UL << 28)
```

### Allocation

```c
/* Ring: 4096 dwords = 16 KB.  Must be physically contiguous and 4KB-aligned. */
#define RING_DWORDS  4096
#define RING_BYTES   (RING_DWORDS * 4)   /* 16384 */

/* RPTR writeback: 4 bytes, can share the same allocation */
```

Use DPMI `int 31h / AX=0800h` to map a 16 KB region of physical RAM below
the 4 GB boundary.  In practice, allocate it from VRAM (above your display
pages) so physical contiguity is guaranteed.

### Init sequence

```c
static volatile unsigned long *g_ring;     /* mapped pointer to ring dwords */
static unsigned long  g_ring_phys;         /* physical byte address */
static volatile unsigned long *g_rptr_wb;  /* mapped pointer to RPTR writeback slot */
static unsigned long  g_rptr_wb_phys;
static unsigned int   g_ring_wptr = 0;

static void cp_init(void)
{
    unsigned long rb_cntl;

    /* Stop ME before reconfiguring */
    wreg(R_CP_ME_CNTL, rreg(R_CP_ME_CNTL) | ME_HALT);

    /* Disable CSQ — use ring mode */
    wreg(R_CP_CSQ_CNTL, 0);

    /* RPTR writeback address (physical, 64-byte aligned) */
    wreg(R_CP_RB_RPTR_ADDR, g_rptr_wb_phys);

    /* Ring base (physical byte address, bits [31:8] → reg [23:0]) */
    wreg(R_CP_RB_BASE, g_ring_phys >> 8);

    /* RB_CNTL: log2(RING_DWORDS) in bits[5:0], writeback enable in bit 31 */
    rb_cntl = 12UL              /* log2(4096) = 12 */
            | (1UL << 31);      /* RB_RPTR_WR_ENA */
    wreg(R_CP_RB_CNTL, rb_cntl);

    /* Reset pointers */
    g_ring_wptr = 0;
    wreg(R_CP_RB_RPTR, 0);
    wreg(R_CP_RB_WPTR, 0);

    /* Start ME */
    wreg(R_CP_ME_CNTL, rreg(R_CP_ME_CNTL) & ~ME_HALT);
}
```

### Ring write helpers

```c
/* CP packet formats (R300-era) */
/* Type 0: sequential register write.  reg = byte offset >> 2.  count = dwords of data. */
#define CP_PACKET0(reg, count)  (((unsigned long)((count)-1) << 16) | ((reg) >> 2))
/* Type 3: opcode packet */
#define CP_PACKET3(op, count)   (0xC0000000UL | ((op) << 8) | ((count)-1))

static void ring_write(unsigned long v)
{
    g_ring[g_ring_wptr & (RING_DWORDS - 1)] = v;
    g_ring_wptr++;
}

/* Write n register values starting at reg, then kick the CP */
static void ring_reg_seq(unsigned long reg, const unsigned long *vals, int n)
{
    int i;
    ring_write(CP_PACKET0(reg, n));
    for (i = 0; i < n; i++)
        ring_write(vals[i]);
    /* Align to even dword count (CP requires even-length packets on R300) */
    if ((n + 1) & 1)
        ring_write(CP_PACKET3(0x10 /*NOP*/, 1));  /* WAIT_FOR_IDLE NOP */
    wreg(R_CP_RB_WPTR, g_ring_wptr & (RING_DWORDS - 1));
}

/* Wait until the CP has consumed all submitted packets */
static void cp_wait_idle(void)
{
    unsigned long timeout = 2000000UL;
    while (timeout--) {
        unsigned int rptr = (unsigned int)(*g_rptr_wb & (RING_DWORDS - 1));
        if (rptr == (g_ring_wptr & (RING_DWORDS - 1)))
            break;
    }
}
```

---

## 4  Textured blit with bilinear scaling (via CP ring)

**Complexity:** high — ~120 new lines for state setup, ~30 for the draw call.
**Reference:** `xf86-video-ati/src/radeon_textured_videofuncs.c`
`R500PrepareTexturedVideo()` (line 2368) and `R500DisplayTexturedVideo()` (line 3745).
**Prerequisite:** step 3 (CP ring running).

### What it provides

- Blit any VRAM region to any other VRAM region at a different size
- Bilinear filtering (no pixelation on upscale, no aliasing on downscale)
- Works on 8bpp indexed source with palette lookup via `R300_TX_FORMAT_X8`
  (outputs 8-bit luma; full-colour requires 16bpp or 32bpp framebuffer)

### New registers (texture unit 0)

```c
/* Texture filter / format / pitch / offset — R300/R500 texture unit 0 */
#define R300_TX_FILTER0_0     0x4400
#define R300_TX_FILTER1_0     0x4440
#define R300_TX_FORMAT0_0     0x4480   /* width-1 [10:0], height-1 [21:11] */
#define R300_TX_FORMAT1_0     0x44C0   /* pixel format */
#define R300_TX_FORMAT2_0     0x4500   /* pitch-1 in texels */
#define R300_TX_OFFSET_0      0x4540   /* VRAM byte offset of texture */

#define R300_TX_FORMAT_X8           0x1B  /* 8-bit single channel (palette/luma) */
#define R300_TX_FORMAT_VYUY422      0x0E  /* packed YUV422 with YUV→RGB */
#define R300_TX_MAG_FILTER_LINEAR   (1UL << 11)
#define R300_TX_MIN_FILTER_LINEAR   (1UL << 13)
#define R300_TX_CLAMP_CLAMP_LAST    3UL
#define R300_TX_CLAMP_S(v)          ((v) << 0)
#define R300_TX_CLAMP_T(v)          ((v) << 3)

/* Output / render target */
#define R300_RB3D_COLOROFFSET0      0x4E00  /* VRAM offset of render target */
#define R300_RB3D_COLORPITCH0       0x4E38  /* pitch in pixels | format bits */
#define R300_COLORFORMAT_I8         0x0     /* 8bpp indexed */
#define R300_COLORFORMAT_RGB565     0x4
#define R300_COLORFORMAT_ARGB8888   0x6

/* Scissor */
#define R300_SC_SCISSOR0            0x43E0
#define R300_SC_SCISSOR1            0x43E4
#define R300_SCISSOR_X_SHIFT        0
#define R300_SCISSOR_Y_SHIFT        13

/* Vertex / geometry */
#define R300_VAP_VTX_STATE_CNTL     0x2180
#define R300_VAP_PVS_STATE_FLUSH_REG 0x2284
#define R300_VAP_CNTL_STATUS        0x2140
#define   R300_PVS_BYPASS           (1UL << 8)  /* bypass vertex shader */
#define R300_VAP_PROG_STREAM_CNTL_0 0x2150      /* vertex layout: 2× FLOAT2 */
#define R300_DATA_TYPE_FLOAT_2      0x3
#define R300_DATA_TYPE_0_SHIFT      0
#define R300_DATA_TYPE_1_SHIFT      16
#define R300_DST_VEC_LOC_0_SHIFT    5
#define R300_DST_VEC_LOC_1_SHIFT    21

/* Draw packet */
#define R200_CP_PACKET3_3D_DRAW_IMMD_2  0x3500
#define RADEON_CP_VC_CNTL_PRIM_TYPE_TRI_LIST  0x00000004
#define RADEON_CP_VC_CNTL_PRIM_WALK_RING      0x00000030
#define RADEON_CP_VC_CNTL_NUM_SHIFT           16
```

### State setup (call once per scale operation, or batch multiple)

Directly mirrors `R500PrepareTexturedVideo()` with the CP ring calls
replaced by `ring_write()`:

```
1. RADEON_SWITCH_TO_3D equivalent:
     — flush 2D cache, write WAIT_UNTIL 2D+3D idleclean via ring

2. Texture unit 0:
     TX_FILTER0_0  = clamp_last_s | clamp_last_t | linear_mag | linear_min
     TX_FILTER1_0  = 0
     TX_FORMAT0_0  = (src_w-1) | ((src_h-1) << 11) | TXPITCH_EN
     TX_FORMAT1_0  = format (X8 for 8bpp, VYUY422 for YUV)
     TX_FORMAT2_0  = src_pitch_in_pixels - 1
     TX_OFFSET_0   = g_fb_location + src_vram_offset

3. Render target:
     RB3D_COLOROFFSET0 = g_fb_location + dst_vram_offset
     RB3D_COLORPITCH0  = (dst_pitch_in_pixels) | dst_color_format

4. Vertex stream layout (PVS bypass — no vertex shader):
     VAP_CNTL_STATUS = PVS_BYPASS
     VAP_PROG_STREAM_CNTL_0:
       attr 0 = FLOAT2 at dst_vec_loc 0  (destination XY position)
       attr 1 = FLOAT2 at dst_vec_loc 6  (texture UV)

5. Pixel shader output:
     R300_US_CONFIG / R300_US_PIXSIZE / R300_US_ALU_* for R500 pass-through
     (copy texture sample to output without modification)
```

### Draw call (one scaled blit)

```
Compute for each of the 3 triangle vertices (single-triangle covering the dst rect):
  dst_x, dst_y    — destination pixel coordinates (float)
  src_u, src_v    — normalised texture coordinates (0.0 – 1.0)

Emit:
  SCISSOR0 = (dst_x << 0) | (dst_y << 13)
  SCISSOR1 = ((dst_x+dst_w-1) << 0) | ((dst_y+dst_h-1) << 13)

  CP_PACKET3(3D_DRAW_IMMD_2, 3*vtx_count):
    VC_CNTL = TRI_LIST | WALK_RING | (vtx_count << 16)
    for each vertex: dst_x(float), dst_y(float), src_u(float), src_v(float)

  cp_wait_idle()
```

The single-triangle approach (used on R300+ by xf86-video-ati because quad
rendering causes a diagonal tear) covers the destination rectangle and uses
the scissor to clip to the exact output bounds.

---

## 5  YUV→RGB textured blit (video frame decode display)

**Complexity:** low incremental cost over step 4 — format change only.
**Reference:** `R500PrepareTexturedVideo()`, `FOURCC_UYVY` path.

Change `TX_FORMAT1_0` to `R300_TX_FORMAT_VYUY422` (packed YUV 4:2:2) and set
`R300_TX_FORMAT_YUV_TO_RGB_CLAMP` in the same field.  The texture unit
performs the YCbCr→RGB conversion in hardware during sampling.  The pitch
field in `TX_FORMAT2_0` is in units of 2-byte YUV macro-pixels (pitch/2).

Useful for displaying decoded video frames or pre-rendered YUV sprites without
a CPU colour-space conversion step.

---

## 6  Summary and suggested build order

| Step | File(s) to create | Prerequisite | Estimated new code |
|------|-------------------|--------------|-------------------|
| 1.1  Fix RBLIT mc_wait | `RBLIT.C`          | —            | 1 line             |
| 1.2  RBLIT scissor test | `RBLIT.C`         | —            | ~30 lines          |
| 2    Hardware cursor    | `RADEON.C`        | —            | ~50 lines          |
| 3    CP ring init       | `RCPRING.C` (new) | —            | ~120 lines         |
| 4    Textured scale blit| `RCPRING.C`       | step 3       | ~200 lines         |
| 5    YUV display        | `RCPRING.C`       | step 4       | ~15 lines (delta)  |

Steps 1.x are maintenance.  Step 2 is self-contained and immediately useful
for on-screen overlays.  Steps 3–5 form a single coherent module (`RCPRING.C`)
that should be validated with a diagnostic test program analogous to `RBLIT.C`
before being integrated into the demo.
