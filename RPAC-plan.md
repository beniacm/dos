# RPAC - Scrolling Pac-Man Demo for Radeon Suite

## Problem Statement

Add a Pac-Man game to the Radeon demo suite where Pac-Man stays centered
on screen and the maze scrolls around him. Supports GPU and CPU compositing
modes like RDUNE. Uses 8bpp palette mode at 800×600. All standard Pac-Man
rules apply: dots, power pellets, 4 ghosts with distinct AI, lives, scoring,
level progression.

### DOSLIB.C/H — Universal DOS demo library (17+ functions)
From overlapping code in PLASMA/VGA.C and RADEON/RADEONHW.C:
- **DPMI**: dos_buf, dpmi_alloc, dpmi_free, dpmi_real_int, dpmi_map, dpmi_unmap
- **VBE**: vbe_get_info, vbe_get_mode_info, vbe_set_mode, vbe_set_text_mode
- **VBE flip**: vbe_set_display_start, vbe_schedule_display_start, vbe_is_flip_complete
- **PMI**: query_pmi, _pmi_call4, pmi_set_display_start, etc. (with inline CS expansion)
- **DAC/Palette**: init_dac, vbe_set_palette
- **Font/Text**: get_bios_font_8x8, draw_char, draw_string, draw_char_2x, draw_string_2x
- **Timing**: rdtsc_read, calibrate_rdtsc, tsc_to_ms
- **CPU**: _has_cpuid, djgpp_enable_sse, expand_cs_to_4gb, MTRR/PAT
- **Blit**: wc_memcpy, blit_to_lfb
- **Misc**: wait_vsync, find_hud_colours
- **PCI**: pci_read32, pci_write32 (generic, not Radeon-specific)

### RADEONHW.C/H — Radeon GPU layer (builds on DOSLIB)
- GPU MMIO: rreg, wreg, mc_rreg
- FIFO: gpu_wait_fifo, gpu_wait_idle
- 2D engine: gpu_init_2d, gpu_fill, gpu_blit, gpu_line, etc.
- Display: detect_flip_mode, flip_page, hw_page_flip, avivo_wait_vblank
- PCI: pci_find_radeon (Radeon-specific device scan)
- Palette: setup_palette, setup_dune_palette (Radeon demo palettes)

### RSETUP.C/H — Shared Radeon demo init/cleanup
Extracted from RADEON.C main()'s ~200 lines of boilerplate:
- radeon_init(): RDTSC calibrate, DPMI alloc, PCI find, MMIO map, VRAM check, 
  VESA mode find, font load, mode set, LFB map, palette, gpu_init_2d, detect_flip
- radeon_cleanup(): vbe_text, dpmi_unmap(lfb), dpmi_unmap(mmio), dpmi_free

### Each demo program — minimal, ~50-300 lines
```c
#include "RSETUP.H"
int main(void) {
    if (radeon_init("Demo Name", pages_needed)) return 1;
    // ... demo code using gpu_fill, gpu_blit, etc. ...
    radeon_cleanup();
    return 0;
}
```


---

# RPAC Detailed Implementation Plan

## Approach

Single-file `RPAC.C` following the RDUNE pattern: `#include "RSETUP.H"`,
`radeon_init()` / `radeon_cleanup()`, double-buffered page flips, GPU/CPU
toggle with SPACE.

### Maze Architecture

- **Tile size**: 16×16 pixels
- **Maze dimensions**: 56×72 tiles = 896×1152 pixels (exceeds 800×600 viewport)
- **Tile types**: WALL, DOT, POWER_PELLET, EMPTY, GHOST_HOUSE, TUNNEL
- **Maze stored as**: `unsigned char maze[MAZE_H][MAZE_W]` — static mutable array
- **Maze design**: Original hand-crafted layout inspired by Pac-Man with
  extra corridors and tunnels to fill the larger dimensions. NOT a copy of
  the Namco maze — original design with similar flavor (narrow corridors,
  ghost house in center, 4 corner power pellets, wrap tunnels)

### Rendering Layers (bottom to top)

1. **Background**: Black (color 0)
2. **Maze walls**: Blue outlined tiles (palette blue shades)
3. **Dots + power pellets**: Small white pixels / large flashing circles
4. **Ghosts**: 4 colored 16×16 sprites
5. **Pac-Man**: Yellow 16×16 sprite with animated mouth (3 frames)
6. **HUD**: Score, lives, level, FPS, CPU%, mode — rendered via `cpu_str()`

### Scrolling

- Pac-Man world position: `pac_wx, pac_wy` (pixel coordinates in maze)
- Camera offset: `cam_x = pac_wx - g_xres/2`, `cam_y = pac_wy - g_yres/2`
- Clamped to maze bounds: `cam_x ∈ [0, MAZE_PX_W - g_xres]`
- All entities drawn at screen position = world position - camera offset
- Smooth sub-tile scrolling (Pac-Man moves pixel-by-pixel, not tile-snapped)

### GPU Compositing Mode

- **VRAM page layout**: 4 pages (2 display + 1 staging + 1 sprites)
- **Maze bitmap in sys RAM** (modified in-place when dots eaten)
- **Per-frame GPU operations**:
  1. Copy visible viewport from sys RAM maze → VRAM staging page
  2. `gpu_blit_po()` — staging → back buffer
  3. `gpu_blit_po_key()` × 4 — ghost sprites with transparency
  4. `gpu_blit_po_key()` × 1 — Pac-Man sprite with transparency
  5. `cpu_str()` — HUD text overlay
- Dots are baked into the maze bitmap — eating clears those pixels
- Power pellets flash by toggling pixels every N frames in maze bitmap
- Total GPU blits per frame: ~6-8 (very lightweight)

### CPU Compositing Mode

- **System RAM buffers**:
  - `g_ram_maze[]` — full maze bitmap (896×1152 ≈ 1 MB)
  - `g_ram_frame[]` — viewport framebuffer (800×600 = 480 KB)
  - `g_ram_sprites[]` — sprite sheet (~200×128 = ~25 KB)
- **Per-frame CPU operations**:
  1. Copy visible viewport from `g_ram_maze` → `g_ram_frame` (memcpy rows)
  2. Composite ghost sprites with color-key (sw_sprite_key)
  3. Composite Pac-Man sprite with color-key
  4. `sw_blit_to_vram()` — final WC REP MOVSD blit to VRAM back page
- Same dot-erasure: clear dots from `g_ram_maze` when eaten

### Pac-Man Movement

- **Input**: Arrow keys (scan codes via `getch()` / `kbhit()`)
  - Extended: 0x00 prefix + 0x48=up, 0x50=down, 0x4B=left, 0x4D=right
- **Movement model**: Tile-aligned turns, pixel-smooth motion
  - Pac-Man moves at constant speed (e.g., 2 pixels/frame)
  - Direction change queued; applied when Pac-Man reaches next tile center
    and the requested direction is not blocked by a wall
  - "Pre-turning": if player presses a direction before reaching the
    intersection, the turn is buffered and applied at the next opportunity
  - Pac-Man stops when hitting a wall in current direction
- **Tunnel wrapping**: When Pac-Man enters a tunnel tile at maze edge,
  warp to opposite side (world coordinate wrap)

### Ghost AI

Each ghost has 3 modes cycling on a timer:
- **Scatter**: Go to assigned corner (7s)
- **Chase**: Individual targeting (20s)
- **Frightened**: Random movement, blue color, can be eaten (7s after power pellet)

#### Individual ghost behaviors (Chase mode)

1. **Blinky (red)**: Target = Pac-Man's current tile
2. **Pinky (pink)**: Target = 4 tiles ahead of Pac-Man's facing direction
3. **Inky (cyan)**: Target = 2× vector from Blinky to (2 tiles ahead of Pac-Man)
4. **Clyde (orange)**: Target = Pac-Man if distance > 8 tiles, else scatter corner

#### Pathfinding

- At each intersection, choose direction minimizing Euclidean distance
  to target tile (excluding reverse direction)
- Ghosts cannot reverse direction except when mode changes
- Ghosts slow down in tunnels (move every other frame)

#### Ghost house

- Ghosts start in the ghost house (center of maze)
- Released one at a time on a timer (0s, 3s, 7s, 12s per level)
- When eaten: eyes return to ghost house, then regenerate

### Palette Layout (256 colors, 8bpp)

```
  0       : Black (transparent key / background)
  1-15    : Blue wall shades (gradient for 3D wall effect)
  16-19   : Dot/pellet whites
  20-23   : Pac-Man yellows (body shades)
  24-27   : Red ghost shades (Blinky)
  28-31   : Pink ghost shades (Pinky)
  32-35   : Cyan ghost shades (Inky)
  36-39   : Orange ghost shades (Clyde)
  40-43   : Frightened ghost blue
  44-47   : Ghost eyes (white + blue pupil)
  48-55   : Fruit colors
  56-63   : Reserved
  248-255 : System/HUD colors (from setup_palette: grays, green, red, cyan, yellow, white)
```

### Sprite Sheet Layout

All sprites 16×16, arranged in a grid:

```
Row 0: Pac-Man — 3 mouth frames × 4 directions = 12 cells
Row 1: Pac-Man death animation (8 frames) + Pac-Man full circle
Row 2: Blinky — 2 frames × 4 directions = 8 cells
Row 3: Pinky — same
Row 4: Inky — same
Row 5: Clyde — same
Row 6: Frightened (2 blue + 2 white flash) + eyes (4 dirs) = 8 cells
Row 7: Dot, Power pellet (2 flash), Cherry, Strawberry, Orange, Apple, Grape
```

Sprite sheet: ~192×128 pixels (12 cols × 8 rows × 16px)

### Game State Structures

```c
typedef struct {
    int wx, wy;           /* world pixel position */
    int dir, next_dir;    /* current + queued direction */
    int speed;
    int anim_frame;
    int alive;
    int death_frame;      /* -1 if alive, 0..N during death animation */
} PacState;

typedef struct {
    int wx, wy;
    int dir;
    int mode;             /* SCATTER, CHASE, FRIGHTENED, EATEN */
    int speed;
    int anim_frame;
    int home_tx, home_ty; /* scatter corner target */
    int in_house;
    int release_timer;
    int fright_timer;
    unsigned char pal_base; /* palette base index for this ghost */
} GhostState;

typedef struct {
    int score, lives, level;
    int dots_total, dots_eaten;
    int power_timer;
    int scatter_timer;
    int scatter_mode;
    int fruit_timer, fruit_visible;
    int ghosts_eaten_combo;  /* 200, 400, 800, 1600 multiplier */
    int game_over;
    int ready_timer;         /* "READY!" countdown */
} GameState;
```

### Gameplay Flow

1. **Init**: `radeon_init("RPAC - Pac-Man", 6)`, generate maze bitmap + sprites
2. **Title**: "RPAC - Scrolling Pac-Man" + instructions, press key to start
3. **Level start**: "READY!" text 2s, Pac-Man + ghosts at start positions
4. **Gameplay**: Standard Pac-Man with scrolling viewport
5. **Death**: Death animation (1s), lose 1 life, restart positions if lives > 0
6. **Level complete**: All dots eaten → flash maze → next level (faster ghosts)
7. **Game over**: "GAME OVER" + score, press key → restart or ESC to quit

### Controls

- **Arrow keys**: Move Pac-Man
- **SPACE**: Toggle GPU/CPU compositing mode
- **P**: Pause
- **ESC**: Quit

### HUD

Bottom 16px of viewport:
```
Score: 12340  Lives: ●●●  Lv:3  [GPU] 60.0fps CPU:5%
```

### Build Integration

Add to both build scripts:
- **build.sh**: Add `RPAC` to the demo loop → `RPAC.EXE`
- **build-djgpp.sh**: Add `"RPAC:RPACGCC"` to SPEC loop → `RPACGCC.EXE`

### Memory Budget

- Maze bitmap: 896 × 1152 = 1,032,192 bytes (~1 MB)
- Frame buffer: 800 × 600 = 480,000 bytes (~470 KB)
- Sprite sheet: 192 × 128 = 24,576 bytes (~24 KB)
- Game state: < 1 KB
- Total: ~1.5 MB sys RAM — fine for DOS DPMI

### VRAM Budget

- 6 pages × ~500 KB = ~3 MB (well within 128 MB VRAM)
- Pages: 2 display + 1 staging + 1 sprite sheet + 2 spare

---

## Todos

1. **pac-palette** — Create `setup_pac_palette()` with Pac-Man color scheme
2. **pac-maze** — Design and encode the 56×72 tile maze as a static array
3. **pac-sprites** — Procedural sprite generation (Pac-Man frames, ghost frames, dots)
4. **pac-render** — Maze bitmap renderer (tiles → pixel buffer) + viewport blit
5. **pac-movement** — Pac-Man tile-aligned movement with pre-turn buffering
6. **pac-ghosts** — 4 ghost AI (scatter/chase/frightened) + pathfinding
7. **pac-game** — Game state: dots, scoring, lives, levels, power pellets, fruit
8. **pac-mainloop** — Main loop: input, update, render, HUD, page flip, GPU/CPU toggle
9. **pac-build** — Add RPAC to both build scripts, verify all targets build clean
10. **pac-commit** — Final build verification + git commit

## Notes

- The maze is in sys RAM (not VRAM) to avoid PITCH_OFFSET width mismatch
- Dots baked into maze bitmap; eating clears pixels directly
- Power pellets flash by toggling pixels every N frames in the bitmap
- Ghost AI simplified but authentic to original Pac-Man behaviors
- Arrow keys: extended scan codes (0x00 + 0x48/50/4B/4D)
- Speed: ~2 pixels/frame at 60 FPS = ~120 px/s (adjustable per level)
- File size: ~1500-2000 lines (maze array ~300-400 lines)
- Maze is left-right symmetric (classic Pac-Man style)
