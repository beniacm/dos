/*
 * RPAC.C  -  Scrolling Pac-Man demo for the Radeon suite
 *
 * Pac-Man stays near the centre of the view while the maze scrolls around
 * him.  Supports GPU and CPU compositing modes so the Radeon 2D engine can
 * be compared directly with software composition, matching the style of the
 * other standalone demos in this directory.
 */

#include "RSETUP.H"
#include <time.h>

#define TILE_SIZE           16
#define MAZE_W              56
#define MAZE_H              72
#define MAZE_PX_W           (MAZE_W * TILE_SIZE)
#define MAZE_PX_H           (MAZE_H * TILE_SIZE)

#define PAGE_COUNT          6
#define PAGE_STAGE          2
#define PAGE_SPRITES        3

#define HUD_H               16
#define SPR_W               16
#define SPR_H               16
#define SPR_COLS            12
#define SPR_ROWS             8
#define SPR_SHEET_W         (SPR_COLS * SPR_W)
#define SPR_SHEET_H         (SPR_ROWS * SPR_H)

#define PAC_SPEED            2
#define GHOST_SPEED          2
#define GHOST_FRIGHT_SPEED   1
#define GHOST_EYES_SPEED     3

#define SCATTER_FRAMES     420
#define CHASE_FRAMES      1200
#define READY_FRAMES       120
#define DEATH_FRAMES        64
#define LEVEL_CLEAR_FRAMES 120
#define FRUIT_FRAMES       420

#define PELLET_COUNT         4
#define GHOST_COUNT          4

#define PAC_BODY_0          20
#define PAC_BODY_1          21
#define PAC_BODY_2          22
#define PAC_BODY_3          23
#define GHOST_RED_BASE      24
#define GHOST_PINK_BASE     28
#define GHOST_CYAN_BASE     32
#define GHOST_ORANGE_BASE   36
#define GHOST_FRIGHT_BASE   40
#define GHOST_EYE_BASE      44

#define WALL_DARK            2
#define WALL_MID             8
#define WALL_LIGHT          14
#define DOT_COLOUR          18
#define PELLET_COLOUR       19
#define DOOR_COLOUR         47
#define FRUIT_COLOUR_0      48
#define FRUIT_COLOUR_1      49
#define FRUIT_COLOUR_2      50
#define FRUIT_COLOUR_3      51
#define HUD_GREY           249
#define HUD_GREEN          250
#define HUD_RED            251
#define HUD_CYAN           253
#define HUD_YELLOW         254
#define HUD_WHITE          255

#define SCR_TITLE            0
#define SCR_PLAY             1
#define SCR_GAME_OVER        2

#define DIR_UP               0
#define DIR_LEFT             1
#define DIR_DOWN             2
#define DIR_RIGHT            3
#define DIR_NONE            -1

#define GHOST_SCATTER        0
#define GHOST_CHASE          1
#define GHOST_FRIGHT         2
#define GHOST_EATEN          3

#define TILE_WALL            0
#define TILE_DOT             1
#define TILE_EMPTY           2
#define TILE_POWER           3
#define TILE_HOUSE           4
#define TILE_TUNNEL          5
#define TILE_DOOR            6

#define GHOST_BLINKY         0
#define GHOST_PINKY          1
#define GHOST_INKY           2
#define GHOST_CLYDE          3

#define FRUIT_TX            28
#define FRUIT_TY            44
#define HOUSE_LEFT          24
#define HOUSE_TOP           31
#define HOUSE_W              8
#define HOUSE_H              6
#define HOUSE_DOOR_Y        30
#define HOUSE_DOOR_X0       27
#define HOUSE_DOOR_X1       28
#define TUNNEL_ROW          36
#define PAC_START_TX        28
#define PAC_START_TY        52
#define HOUSE_EXIT_TX       28
#define HOUSE_EXIT_TY       29

#define SPRITE_PAC_ROW       0
#define SPRITE_DEATH_ROW     1
#define SPRITE_BLINKY_ROW    2
#define SPRITE_PINKY_ROW     3
#define SPRITE_INKY_ROW      4
#define SPRITE_CLYDE_ROW     5
#define SPRITE_MISC_ROW      6
#define SPRITE_ITEM_ROW      7

#define FRUIT_NONE           0
#define FRUIT_CHERRY         1

typedef struct {
    int wx, wy;
    int dir, next_dir;
    int speed;
    int anim_frame;
    int alive;
    int death_frame;
} PacState;

typedef struct {
    int wx, wy;
    int dir;
    int mode;
    int speed;
    int anim_frame;
    int home_tx, home_ty;
    int in_house;
    int release_timer;
    int fright_timer;
    unsigned char pal_base;
} GhostState;

typedef struct {
    int score, lives, level;
    int dots_total, dots_eaten;
    int power_timer;
    int scatter_timer;
    int scatter_mode;
    int fruit_timer, fruit_visible;
    int ghosts_eaten_combo;
    int game_over;
    int ready_timer;
    int death_timer;
    int level_clear_timer;
    int pause;
    int fruit_type;
    int fruit_award;
} GameState;

typedef struct {
    unsigned char x, y, w, h;
} WallRect;

static unsigned char g_maze[MAZE_H][MAZE_W];
static unsigned char *g_ram_maze;
static unsigned char *g_ram_frame;
static unsigned char *g_ram_sprites;
static int g_play_h;
static unsigned long g_stage_po;
static unsigned long g_sprite_po;
static int g_back_page;
static int g_gpu_mode = 1;
static long g_frame_no;
static int g_screen = SCR_TITLE;

static PacState g_pac;
static GhostState g_ghosts[GHOST_COUNT];
static GameState g_game;

static int g_pellet_xy[PELLET_COUNT][2] = {
    { 3,  6},
    {52,  6},
    { 3, 65},
    {52, 65}
};

static const int g_dir_dx[4] = { 0, -1, 0, 1 };
static const int g_dir_dy[4] = { -1, 0, 1, 0 };
static const int g_opposite_dir[4] = { DIR_DOWN, DIR_RIGHT, DIR_UP, DIR_LEFT };

static const WallRect g_left_walls[] = {
    { 2,  2,  8, 4}, {12,  2,  6, 4}, {20,  2,  6, 4},
    { 4,  8,  4, 6}, {10,  8, 10, 2}, {22,  8,  4, 6},
    { 2, 16,  8, 2}, {12, 14,  4, 8}, {18, 16,  8, 2},
    { 4, 22,  4, 8}, {10, 24, 12, 2}, {24, 22,  2, 8},
    { 2, 34, 10, 2}, {14, 30,  4,10}, {20, 34,  6, 2},
    { 2, 42,  6, 6}, {10, 42, 12, 2}, {24, 42,  2, 8},
    { 4, 52,  6, 2}, {12, 50,  4, 8}, {18, 52,  8, 2},
    { 2, 58,  8, 4}, {12, 60,  6, 2}, {20, 58,  6, 4},
    { 4, 66, 22, 2}
};

static void maze_set_rect(int x, int y, int w, int h, unsigned char t)
{
    int ix, iy;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > MAZE_W) w = MAZE_W - x;
    if (y + h > MAZE_H) h = MAZE_H - y;
    if (w <= 0 || h <= 0) return;
    for (iy = y; iy < y + h; iy++)
        for (ix = x; ix < x + w; ix++)
            g_maze[iy][ix] = t;
}

static void maze_set_mirror_rect(int x, int y, int w, int h, unsigned char t)
{
    maze_set_rect(x, y, w, h, t);
    maze_set_rect(MAZE_W - x - w, y, w, h, t);
}

static int clamp_i(int v, int lo, int hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static void put_maze_px(int x, int y, unsigned char c)
{
    if (x < 0 || y < 0 || x >= MAZE_PX_W || y >= MAZE_PX_H) return;
    g_ram_maze[(long)y * MAZE_PX_W + x] = c;
}

static void fill_buf_rect(unsigned char *buf, int pitch,
                          int x, int y, int w, int h, unsigned char c)
{
    int iy;
    if (w <= 0 || h <= 0) return;
    for (iy = 0; iy < h; iy++)
        memset(buf + (long)(y + iy) * pitch + x, c, (size_t)w);
}

static void draw_box(unsigned char *buf, int pitch,
                     int x, int y, int w, int h,
                     unsigned char border, unsigned char fill)
{
    int iy;
    fill_buf_rect(buf, pitch, x, y, w, h, fill);
    for (iy = 0; iy < h; iy++) {
        buf[(long)(y + iy) * pitch + x] = border;
        buf[(long)(y + iy) * pitch + x + w - 1] = border;
    }
    memset(buf + (long)y * pitch + x, border, (size_t)w);
    memset(buf + (long)(y + h - 1) * pitch + x, border, (size_t)w);
}

static void render_tile_to_maze(int tx, int ty, int flash_on)
{
    int px, py;
    unsigned char tile;
    int x, y;

    if (tx < 0 || ty < 0 || tx >= MAZE_W || ty >= MAZE_H) return;
    tile = g_maze[ty][tx];
    px = tx * TILE_SIZE;
    py = ty * TILE_SIZE;

    fill_buf_rect(g_ram_maze, MAZE_PX_W, px, py, TILE_SIZE, TILE_SIZE, 0);

    if (tile == TILE_WALL) {
        draw_box(g_ram_maze, MAZE_PX_W, px, py, TILE_SIZE, TILE_SIZE,
                 WALL_LIGHT, WALL_DARK);
        draw_box(g_ram_maze, MAZE_PX_W, px + 3, py + 3,
                 TILE_SIZE - 6, TILE_SIZE - 6, WALL_MID, 0);
    } else if (tile == TILE_DOT) {
        for (y = 7; y <= 8; y++)
            for (x = 7; x <= 8; x++)
                put_maze_px(px + x, py + y, DOT_COLOUR);
    } else if (tile == TILE_POWER) {
        if (flash_on) {
            fill_buf_rect(g_ram_maze, MAZE_PX_W, px + 5, py + 5, 6, 6, PELLET_COLOUR);
            draw_box(g_ram_maze, MAZE_PX_W, px + 4, py + 4, 8, 8, DOT_COLOUR, PELLET_COLOUR);
        }
    } else if (tile == TILE_DOOR) {
        fill_buf_rect(g_ram_maze, MAZE_PX_W, px + 2, py + 7, 12, 2, DOOR_COLOUR);
    }
}

static void render_full_maze_bitmap(int flash_on)
{
    int x, y;
    for (y = 0; y < MAZE_H; y++)
        for (x = 0; x < MAZE_W; x++)
            render_tile_to_maze(x, y, flash_on);
}

static void init_maze_layout(void)
{
    int x, y, i;

    for (y = 0; y < MAZE_H; y++) {
        for (x = 0; x < MAZE_W; x++) {
            if (x == 0 || y == 0 || x == MAZE_W - 1 || y == MAZE_H - 1)
                g_maze[y][x] = TILE_WALL;
            else
                g_maze[y][x] = TILE_DOT;
        }
    }

    for (i = 0; i < (int)(sizeof(g_left_walls) / sizeof(g_left_walls[0])); i++) {
        maze_set_mirror_rect(g_left_walls[i].x, g_left_walls[i].y,
                             g_left_walls[i].w, g_left_walls[i].h, TILE_WALL);
    }

    maze_set_rect(24,  6,  8, 2, TILE_WALL);
    maze_set_rect(26, 10,  4, 4, TILE_WALL);
    maze_set_rect(24, 18,  8, 2, TILE_WALL);
    maze_set_rect(24, 24,  8, 2, TILE_WALL);
    maze_set_rect(24, 40,  8, 2, TILE_WALL);
    maze_set_rect(24, 48,  8, 2, TILE_WALL);
    maze_set_rect(24, 56,  8, 2, TILE_WALL);
    maze_set_rect(24, 62,  8, 2, TILE_WALL);

    maze_set_rect(HOUSE_LEFT - 1, HOUSE_TOP - 1, HOUSE_W + 2, 1, TILE_WALL);
    maze_set_rect(HOUSE_LEFT - 1, HOUSE_TOP + HOUSE_H, HOUSE_W + 2, 1, TILE_WALL);
    maze_set_rect(HOUSE_LEFT - 1, HOUSE_TOP, 1, HOUSE_H, TILE_WALL);
    maze_set_rect(HOUSE_LEFT + HOUSE_W, HOUSE_TOP, 1, HOUSE_H, TILE_WALL);
    maze_set_rect(HOUSE_LEFT, HOUSE_TOP, HOUSE_W, HOUSE_H, TILE_HOUSE);
    maze_set_rect(HOUSE_DOOR_X0, HOUSE_DOOR_Y, 2, 1, TILE_DOOR);
    maze_set_rect(HOUSE_LEFT, HOUSE_TOP - 1, 3, 1, TILE_WALL);
    maze_set_rect(HOUSE_LEFT + 5, HOUSE_TOP - 1, 3, 1, TILE_WALL);

    for (x = 1; x < MAZE_W - 1; x++) {
        if (x < 22 || x > 33) {
            g_maze[TUNNEL_ROW][x] = TILE_EMPTY;
            if (x >= 2 && x <= 6) g_maze[TUNNEL_ROW - 1][x] = TILE_WALL;
            if (x >= MAZE_W - 7 && x <= MAZE_W - 3) g_maze[TUNNEL_ROW - 1][x] = TILE_WALL;
        }
    }
    g_maze[TUNNEL_ROW][0] = TILE_TUNNEL;
    g_maze[TUNNEL_ROW][1] = TILE_TUNNEL;
    g_maze[TUNNEL_ROW][MAZE_W - 2] = TILE_TUNNEL;
    g_maze[TUNNEL_ROW][MAZE_W - 1] = TILE_TUNNEL;

    maze_set_rect(1, 1, MAZE_W - 2, 1, TILE_EMPTY);
    maze_set_rect(1, MAZE_H - 2, MAZE_W - 2, 1, TILE_EMPTY);

    for (i = 0; i < PELLET_COUNT; i++)
        g_maze[g_pellet_xy[i][1]][g_pellet_xy[i][0]] = TILE_POWER;

    for (y = HOUSE_TOP - 2; y < HOUSE_TOP + HOUSE_H + 2; y++)
        for (x = HOUSE_LEFT - 2; x < HOUSE_LEFT + HOUSE_W + 2; x++)
            if (x >= 0 && y >= 0 && x < MAZE_W && y < MAZE_H && g_maze[y][x] == TILE_DOT)
                g_maze[y][x] = TILE_EMPTY;

    for (y = PAC_START_TY - 1; y <= PAC_START_TY + 1; y++)
        for (x = PAC_START_TX - 1; x <= PAC_START_TX + 1; x++)
            if (g_maze[y][x] == TILE_DOT)
                g_maze[y][x] = TILE_EMPTY;

    g_maze[FRUIT_TY][FRUIT_TX] = TILE_EMPTY;
    g_maze[HOUSE_EXIT_TY][HOUSE_EXIT_TX] = TILE_EMPTY;
    g_maze[HOUSE_EXIT_TY - 1][HOUSE_EXIT_TX] = TILE_EMPTY;
}

static void recount_dots(void)
{
    int x, y;
    g_game.dots_total = 0;
    g_game.dots_eaten = 0;
    for (y = 0; y < MAZE_H; y++)
        for (x = 0; x < MAZE_W; x++)
            if (g_maze[y][x] == TILE_DOT || g_maze[y][x] == TILE_POWER)
                g_game.dots_total++;
}

static void refresh_pellet_tiles(int force)
{
    static int prev_on = -1;
    int on;
    int i;

    on = ((g_frame_no / 12) & 1L) ? 1 : 0;
    if (!force && on == prev_on) return;
    prev_on = on;

    for (i = 0; i < PELLET_COUNT; i++) {
        if (g_maze[g_pellet_xy[i][1]][g_pellet_xy[i][0]] == TILE_POWER)
            render_tile_to_maze(g_pellet_xy[i][0], g_pellet_xy[i][1], on);
    }
}

static int sw_alloc(void)
{
    g_ram_maze = (unsigned char *)malloc((size_t)MAZE_PX_W * (size_t)MAZE_PX_H);
    g_ram_frame = (unsigned char *)malloc((size_t)g_xres * (size_t)g_yres);
    g_ram_sprites = (unsigned char *)malloc((size_t)SPR_SHEET_W * (size_t)SPR_SHEET_H);
    return g_ram_maze != NULL && g_ram_frame != NULL && g_ram_sprites != NULL;
}

static void sw_free(void)
{
    if (g_ram_maze) { free(g_ram_maze); g_ram_maze = NULL; }
    if (g_ram_frame) { free(g_ram_frame); g_ram_frame = NULL; }
    if (g_ram_sprites) { free(g_ram_sprites); g_ram_sprites = NULL; }
}

static void spr_put(int cell_x, int cell_y, int x, int y, unsigned char c)
{
    int px, py;
    if (x < 0 || y < 0 || x >= SPR_W || y >= SPR_H) return;
    px = cell_x * SPR_W + x;
    py = cell_y * SPR_H + y;
    g_ram_sprites[(long)py * SPR_SHEET_W + px] = c;
}

static void spr_clear_cell(int cell_x, int cell_y)
{
    int y;
    for (y = 0; y < SPR_H; y++)
        memset(g_ram_sprites + (long)(cell_y * SPR_H + y) * SPR_SHEET_W + cell_x * SPR_W,
               0, SPR_W);
}

static void spr_disc(int cell_x, int cell_y, int cx, int cy, int r,
                     unsigned char c0, unsigned char c1)
{
    int x, y, dx, dy, r2;
    r2 = r * r;
    for (y = 0; y < SPR_H; y++) {
        for (x = 0; x < SPR_W; x++) {
            dx = x - cx;
            dy = y - cy;
            if (dx * dx + dy * dy <= r2) {
                if (dx * dx + dy * dy <= (r2 * 2) / 5)
                    spr_put(cell_x, cell_y, x, y, c1);
                else
                    spr_put(cell_x, cell_y, x, y, c0);
            }
        }
    }
}

static void gen_pac_frame(int dir, int frame)
{
    int cell_x, x, y, dx, dy, adx, ady, body, hi;
    cell_x = dir * 3 + frame;
    spr_clear_cell(cell_x, SPRITE_PAC_ROW);
    body = (frame == 0) ? PAC_BODY_1 : PAC_BODY_2;
    hi = PAC_BODY_3;
    for (y = 0; y < SPR_H; y++) {
        for (x = 0; x < SPR_W; x++) {
            dx = x - 7;
            dy = y - 7;
            adx = dx < 0 ? -dx : dx;
            ady = dy < 0 ? -dy : dy;
            if (dx * dx + dy * dy > 49) continue;
            if (frame != 0) {
                if (dir == DIR_RIGHT && dx > 0 && ady * (frame + 1) <= dx * 2) continue;
                if (dir == DIR_LEFT  && dx < 0 && ady * (frame + 1) <= (-dx) * 2) continue;
                if (dir == DIR_UP    && dy < 0 && adx * (frame + 1) <= (-dy) * 2) continue;
                if (dir == DIR_DOWN  && dy > 0 && adx * (frame + 1) <= dy * 2) continue;
            }
            if (dx * dx + dy * dy < 16)
                spr_put(cell_x, SPRITE_PAC_ROW, x, y, hi);
            else
                spr_put(cell_x, SPRITE_PAC_ROW, x, y, body);
        }
    }
    if (dir == DIR_RIGHT) spr_put(cell_x, SPRITE_PAC_ROW, 9, 5, 0);
    if (dir == DIR_LEFT)  spr_put(cell_x, SPRITE_PAC_ROW, 5, 5, 0);
    if (dir == DIR_UP)    spr_put(cell_x, SPRITE_PAC_ROW, 6, 4, 0);
    if (dir == DIR_DOWN)  spr_put(cell_x, SPRITE_PAC_ROW, 6, 8, 0);
}

static void gen_death_frames(void)
{
    int f, x, y, dx, dy, r;
    for (f = 0; f < 8; f++) {
        spr_clear_cell(f, SPRITE_DEATH_ROW);
        r = 7 - f;
        if (r < 1) r = 1;
        for (y = 0; y < SPR_H; y++) {
            for (x = 0; x < SPR_W; x++) {
                dx = x - 7;
                dy = y - 7;
                if (dx * dx + dy * dy <= r * r) {
                    if (x >= 7 && (dy < 0 ? -dy : dy) * 2 <= dx * 2)
                        continue;
                    spr_put(f, SPRITE_DEATH_ROW, x, y,
                            (dx * dx + dy * dy < (r * r) / 2) ? PAC_BODY_3 : PAC_BODY_2);
                }
            }
        }
    }
}

static void gen_ghost_frame(int row, int dir, int frame, unsigned char base)
{
    int cell_x, x, y;
    cell_x = dir * 2 + frame;
    spr_clear_cell(cell_x, row);

    for (y = 1; y < 11; y++) {
        for (x = 2; x < 14; x++) {
            int dx = x - 7;
            int dy = y - 6;
            if (dy <= 0) {
                if (dx * dx + dy * dy <= 30)
                    spr_put(cell_x, row, x, y, base + 1);
            } else {
                spr_put(cell_x, row, x, y, base + ((x + y) & 1 ? 1 : 2));
            }
        }
    }

    for (x = 2; x < 14; x++) {
        if (((x + frame) & 3) != 0)
            spr_put(cell_x, row, x, 12 + ((x + frame) & 1), x < 8 ? base + 1 : base + 2);
    }

    for (x = 4; x <= 6; x++) {
        spr_put(cell_x, row, x, 6, 44);
        spr_put(cell_x, row, x, 7, 44);
        spr_put(cell_x, row, x + 4, 6, 44);
        spr_put(cell_x, row, x + 4, 7, 44);
    }

    if (dir == DIR_LEFT) {
        spr_put(cell_x, row, 4, 6, 47);
        spr_put(cell_x, row, 8, 6, 47);
    } else if (dir == DIR_RIGHT) {
        spr_put(cell_x, row, 6, 6, 47);
        spr_put(cell_x, row, 10, 6, 47);
    } else if (dir == DIR_UP) {
        spr_put(cell_x, row, 5, 5, 47);
        spr_put(cell_x, row, 9, 5, 47);
    } else {
        spr_put(cell_x, row, 5, 7, 47);
        spr_put(cell_x, row, 9, 7, 47);
    }
}

static void gen_fright_misc(void)
{
    int f, x;
    for (f = 0; f < 4; f++) {
        int cell = f;
        spr_clear_cell(cell, SPRITE_MISC_ROW);
        for (x = 2; x < 14; x++) {
            spr_put(cell, SPRITE_MISC_ROW, x, 12 + ((x + f) & 1), 41 + (f & 1));
        }
        spr_disc(cell, SPRITE_MISC_ROW, 7, 6, 6,
                 (unsigned char)(41 + (f & 1)), (unsigned char)(42 + ((f >> 1) & 1)));
        spr_put(cell, SPRITE_MISC_ROW, 5, 6, 44);
        spr_put(cell, SPRITE_MISC_ROW, 6, 6, 44);
        spr_put(cell, SPRITE_MISC_ROW, 9, 6, 44);
        spr_put(cell, SPRITE_MISC_ROW, 10, 6, 44);
        spr_put(cell, SPRITE_MISC_ROW, 5, 7, 44);
        spr_put(cell, SPRITE_MISC_ROW, 10, 7, 44);
        spr_put(cell, SPRITE_MISC_ROW, 4, 10, 255);
        spr_put(cell, SPRITE_MISC_ROW, 6, 11, 255);
        spr_put(cell, SPRITE_MISC_ROW, 8, 10, 255);
        spr_put(cell, SPRITE_MISC_ROW, 10, 11, 255);
    }

    for (f = 0; f < 4; f++) {
        int cell = 4 + f;
        spr_clear_cell(cell, SPRITE_MISC_ROW);
        spr_put(cell, SPRITE_MISC_ROW, 4, 6, 44);
        spr_put(cell, SPRITE_MISC_ROW, 5, 6, 44);
        spr_put(cell, SPRITE_MISC_ROW, 9, 6, 44);
        spr_put(cell, SPRITE_MISC_ROW, 10, 6, 44);
        spr_put(cell, SPRITE_MISC_ROW, 4, 7, 44);
        spr_put(cell, SPRITE_MISC_ROW, 5, 7, 44);
        spr_put(cell, SPRITE_MISC_ROW, 9, 7, 44);
        spr_put(cell, SPRITE_MISC_ROW, 10, 7, 44);
        if (f == DIR_LEFT) {
            spr_put(cell, SPRITE_MISC_ROW, 4, 6, 47);
            spr_put(cell, SPRITE_MISC_ROW, 9, 6, 47);
        } else if (f == DIR_RIGHT) {
            spr_put(cell, SPRITE_MISC_ROW, 6, 6, 47);
            spr_put(cell, SPRITE_MISC_ROW, 11, 6, 47);
        } else if (f == DIR_UP) {
            spr_put(cell, SPRITE_MISC_ROW, 5, 5, 47);
            spr_put(cell, SPRITE_MISC_ROW, 10, 5, 47);
        } else {
            spr_put(cell, SPRITE_MISC_ROW, 5, 7, 47);
            spr_put(cell, SPRITE_MISC_ROW, 10, 7, 47);
        }
    }
}

static void gen_items(void)
{
    int x;
    spr_clear_cell(0, SPRITE_ITEM_ROW);
    spr_clear_cell(1, SPRITE_ITEM_ROW);
    spr_clear_cell(2, SPRITE_ITEM_ROW);

    spr_put(0, SPRITE_ITEM_ROW, 7, 7, DOT_COLOUR);
    fill_buf_rect(g_ram_sprites, SPR_SHEET_W, 1 * SPR_W + 5, SPRITE_ITEM_ROW * SPR_H + 5,
                  6, 6, PELLET_COLOUR);

    for (x = 0; x < 5; x++) {
        spr_put(2, SPRITE_ITEM_ROW, 5 + x, 6, FRUIT_COLOUR_0);
        spr_put(2, SPRITE_ITEM_ROW, 3 + x, 10, FRUIT_COLOUR_1);
        spr_put(2, SPRITE_ITEM_ROW, 8 + x, 10, FRUIT_COLOUR_1);
    }
    spr_put(2, SPRITE_ITEM_ROW, 6, 4, FRUIT_COLOUR_2);
    spr_put(2, SPRITE_ITEM_ROW, 9, 4, FRUIT_COLOUR_2);
    spr_put(2, SPRITE_ITEM_ROW, 7, 3, FRUIT_COLOUR_3);
    spr_put(2, SPRITE_ITEM_ROW, 8, 2, FRUIT_COLOUR_3);
}

static void gen_sprite_sheet(void)
{
    int dir;
    memset(g_ram_sprites, 0, (size_t)SPR_SHEET_W * (size_t)SPR_SHEET_H);
    for (dir = 0; dir < 4; dir++) {
        gen_pac_frame(dir, 0);
        gen_pac_frame(dir, 1);
        gen_pac_frame(dir, 2);
        gen_ghost_frame(SPRITE_BLINKY_ROW, dir, 0, GHOST_RED_BASE);
        gen_ghost_frame(SPRITE_BLINKY_ROW, dir, 1, GHOST_RED_BASE);
        gen_ghost_frame(SPRITE_PINKY_ROW, dir, 0, GHOST_PINK_BASE);
        gen_ghost_frame(SPRITE_PINKY_ROW, dir, 1, GHOST_PINK_BASE);
        gen_ghost_frame(SPRITE_INKY_ROW, dir, 0, GHOST_CYAN_BASE);
        gen_ghost_frame(SPRITE_INKY_ROW, dir, 1, GHOST_CYAN_BASE);
        gen_ghost_frame(SPRITE_CLYDE_ROW, dir, 0, GHOST_ORANGE_BASE);
        gen_ghost_frame(SPRITE_CLYDE_ROW, dir, 1, GHOST_ORANGE_BASE);
    }
    gen_death_frames();
    gen_fright_misc();
    gen_items();
}

static void ram_to_vram_page(const unsigned char *buf, int pitch, int w, int h, int page)
{
    int y;
    int dst_y;
    dst_y = page * g_page_stride;
    for (y = 0; y < h; y++) {
        memcpy(g_lfb + (long)(dst_y + y) * g_pitch,
               buf + (long)y * pitch, (size_t)w);
    }
}

static void upload_sprite_sheet(void)
{
    ram_to_vram_page(g_ram_sprites, SPR_SHEET_W, SPR_SHEET_W, SPR_SHEET_H, PAGE_SPRITES);
    gpu_wait_idle();
    gpu_flush_2d_cache();
}

static int tile_allows_pac(int tx, int ty)
{
    unsigned char t;
    if (tx < 0 || ty < 0 || tx >= MAZE_W || ty >= MAZE_H) return 0;
    t = g_maze[ty][tx];
    return t != TILE_WALL && t != TILE_HOUSE && t != TILE_DOOR;
}

static int tile_allows_ghost(int tx, int ty, GhostState *g, int dir)
{
    unsigned char t;
    if (tx < 0 || ty < 0 || tx >= MAZE_W || ty >= MAZE_H) return 0;
    t = g_maze[ty][tx];
    if (t == TILE_WALL) return 0;
    if (t == TILE_DOOR) {
        if (g->mode == GHOST_EATEN) return 1;
        if (g->in_house && dir == DIR_UP) return 1;
        return 0;
    }
    if (t == TILE_HOUSE) return 1;
    return 1;
}

static int entity_aligned(int wx, int wy)
{
    return ((wx & (TILE_SIZE - 1)) == 0) && ((wy & (TILE_SIZE - 1)) == 0);
}

static int can_move_pac(int wx, int wy, int dir)
{
    int tx, ty;
    tx = wx / TILE_SIZE;
    ty = wy / TILE_SIZE;
    tx += g_dir_dx[dir];
    ty += g_dir_dy[dir];
    return tile_allows_pac(tx, ty);
}

static int can_move_ghost(GhostState *g, int dir)
{
    int tx, ty;
    tx = g->wx / TILE_SIZE + g_dir_dx[dir];
    ty = g->wy / TILE_SIZE + g_dir_dy[dir];
    return tile_allows_ghost(tx, ty, g, dir);
}

static void reverse_active_ghosts(void)
{
    int i;
    for (i = 0; i < GHOST_COUNT; i++) {
        if (g_ghosts[i].mode != GHOST_EATEN && !g_ghosts[i].in_house)
            g_ghosts[i].dir = g_opposite_dir[g_ghosts[i].dir];
    }
}

static void set_frightened_mode(void)
{
    int i;
    g_game.power_timer = clamp_i(420 - g_game.level * 20, 180, 420);
    g_game.ghosts_eaten_combo = 0;
    for (i = 0; i < GHOST_COUNT; i++) {
        if (g_ghosts[i].mode != GHOST_EATEN) {
            g_ghosts[i].mode = GHOST_FRIGHT;
            g_ghosts[i].fright_timer = g_game.power_timer;
            if (!g_ghosts[i].in_house)
                g_ghosts[i].dir = g_opposite_dir[g_ghosts[i].dir];
        }
    }
}

static int current_base_ghost_mode(void)
{
    return g_game.scatter_mode ? GHOST_SCATTER : GHOST_CHASE;
}

static void restore_ghost_modes(void)
{
    int i;
    for (i = 0; i < GHOST_COUNT; i++) {
        if (g_ghosts[i].mode == GHOST_FRIGHT)
            g_ghosts[i].mode = current_base_ghost_mode();
    }
}

static void reset_positions(void)
{
    g_pac.wx = PAC_START_TX * TILE_SIZE;
    g_pac.wy = PAC_START_TY * TILE_SIZE;
    g_pac.dir = DIR_LEFT;
    g_pac.next_dir = DIR_LEFT;
    g_pac.speed = PAC_SPEED;
    g_pac.anim_frame = 0;
    g_pac.alive = 1;
    g_pac.death_frame = -1;

    g_ghosts[GHOST_BLINKY].wx = HOUSE_EXIT_TX * TILE_SIZE;
    g_ghosts[GHOST_BLINKY].wy = (HOUSE_EXIT_TY - 3) * TILE_SIZE;
    g_ghosts[GHOST_BLINKY].dir = DIR_LEFT;
    g_ghosts[GHOST_BLINKY].mode = GHOST_SCATTER;
    g_ghosts[GHOST_BLINKY].speed = GHOST_SPEED;
    g_ghosts[GHOST_BLINKY].home_tx = MAZE_W - 4;
    g_ghosts[GHOST_BLINKY].home_ty = 3;
    g_ghosts[GHOST_BLINKY].in_house = 0;
    g_ghosts[GHOST_BLINKY].release_timer = 0;
    g_ghosts[GHOST_BLINKY].pal_base = GHOST_RED_BASE;

    g_ghosts[GHOST_PINKY].wx = (HOUSE_EXIT_TX - 1) * TILE_SIZE;
    g_ghosts[GHOST_PINKY].wy = (HOUSE_TOP + 2) * TILE_SIZE;
    g_ghosts[GHOST_PINKY].dir = DIR_UP;
    g_ghosts[GHOST_PINKY].mode = GHOST_SCATTER;
    g_ghosts[GHOST_PINKY].speed = GHOST_SPEED;
    g_ghosts[GHOST_PINKY].home_tx = 3;
    g_ghosts[GHOST_PINKY].home_ty = 3;
    g_ghosts[GHOST_PINKY].in_house = 1;
    g_ghosts[GHOST_PINKY].release_timer = clamp_i(180 - g_game.level * 10, 60, 180);
    g_ghosts[GHOST_PINKY].pal_base = GHOST_PINK_BASE;

    g_ghosts[GHOST_INKY].wx = HOUSE_EXIT_TX * TILE_SIZE;
    g_ghosts[GHOST_INKY].wy = (HOUSE_TOP + 2) * TILE_SIZE;
    g_ghosts[GHOST_INKY].dir = DIR_UP;
    g_ghosts[GHOST_INKY].mode = GHOST_SCATTER;
    g_ghosts[GHOST_INKY].speed = GHOST_SPEED;
    g_ghosts[GHOST_INKY].home_tx = MAZE_W - 4;
    g_ghosts[GHOST_INKY].home_ty = MAZE_H - 4;
    g_ghosts[GHOST_INKY].in_house = 1;
    g_ghosts[GHOST_INKY].release_timer = clamp_i(420 - g_game.level * 15, 180, 420);
    g_ghosts[GHOST_INKY].pal_base = GHOST_CYAN_BASE;

    g_ghosts[GHOST_CLYDE].wx = (HOUSE_EXIT_TX + 1) * TILE_SIZE;
    g_ghosts[GHOST_CLYDE].wy = (HOUSE_TOP + 2) * TILE_SIZE;
    g_ghosts[GHOST_CLYDE].dir = DIR_UP;
    g_ghosts[GHOST_CLYDE].mode = GHOST_SCATTER;
    g_ghosts[GHOST_CLYDE].speed = GHOST_SPEED;
    g_ghosts[GHOST_CLYDE].home_tx = 3;
    g_ghosts[GHOST_CLYDE].home_ty = MAZE_H - 4;
    g_ghosts[GHOST_CLYDE].in_house = 1;
    g_ghosts[GHOST_CLYDE].release_timer = clamp_i(720 - g_game.level * 20, 240, 720);
    g_ghosts[GHOST_CLYDE].pal_base = GHOST_ORANGE_BASE;
}

static void init_level(void)
{
    init_maze_layout();
    recount_dots();
    render_full_maze_bitmap(1);
    refresh_pellet_tiles(1);
    g_game.power_timer = 0;
    g_game.scatter_mode = 1;
    g_game.scatter_timer = SCATTER_FRAMES;
    g_game.ghosts_eaten_combo = 0;
    g_game.ready_timer = READY_FRAMES;
    g_game.death_timer = 0;
    g_game.level_clear_timer = 0;
    g_game.pause = 0;
    g_game.fruit_timer = 0;
    g_game.fruit_visible = 0;
    g_game.fruit_type = FRUIT_CHERRY;
    g_game.fruit_award = 100 + (g_game.level - 1) * 100;
    reset_positions();
}

static void start_new_game(void)
{
    memset(&g_game, 0, sizeof(g_game));
    g_game.level = 1;
    g_game.lives = 3;
    g_screen = SCR_PLAY;
    init_level();
}

static void next_level(void)
{
    g_game.level++;
    init_level();
}

static void lose_life(void)
{
    if (g_game.death_timer > 0 || g_game.level_clear_timer > 0) return;
    g_game.death_timer = DEATH_FRAMES;
    g_pac.death_frame = 0;
    g_pac.alive = 0;
}

static void eat_tile_at_pac(void)
{
    int tx, ty;
    unsigned char t;
    tx = g_pac.wx / TILE_SIZE;
    ty = g_pac.wy / TILE_SIZE;
    if (tx < 0 || ty < 0 || tx >= MAZE_W || ty >= MAZE_H) return;
    t = g_maze[ty][tx];
    if (t == TILE_DOT) {
        g_maze[ty][tx] = TILE_EMPTY;
        g_game.score += 10;
        g_game.dots_eaten++;
        render_tile_to_maze(tx, ty, 1);
    } else if (t == TILE_POWER) {
        g_maze[ty][tx] = TILE_EMPTY;
        g_game.score += 50;
        g_game.dots_eaten++;
        render_tile_to_maze(tx, ty, 1);
        set_frightened_mode();
    }

    if (!g_game.fruit_visible &&
        (g_game.dots_eaten == g_game.dots_total / 3 ||
         g_game.dots_eaten == (g_game.dots_total * 2) / 3)) {
        g_game.fruit_visible = 1;
        g_game.fruit_timer = FRUIT_FRAMES;
    }
}

static void update_pac(void)
{
    int speed;
    if (!g_pac.alive) return;
    speed = g_pac.speed;

    if (entity_aligned(g_pac.wx, g_pac.wy)) {
        if (g_pac.next_dir != DIR_NONE && can_move_pac(g_pac.wx, g_pac.wy, g_pac.next_dir))
            g_pac.dir = g_pac.next_dir;
        if (!can_move_pac(g_pac.wx, g_pac.wy, g_pac.dir))
            return;
    }

    g_pac.wx += g_dir_dx[g_pac.dir] * speed;
    g_pac.wy += g_dir_dy[g_pac.dir] * speed;

    if ((g_pac.wy / TILE_SIZE) == TUNNEL_ROW) {
        if (g_pac.wx < -TILE_SIZE)
            g_pac.wx = (MAZE_W - 1) * TILE_SIZE;
        else if (g_pac.wx > (MAZE_W - 1) * TILE_SIZE)
            g_pac.wx = -TILE_SIZE;
    }

    if (entity_aligned(g_pac.wx, g_pac.wy))
        eat_tile_at_pac();

    g_pac.anim_frame = (int)((g_frame_no / 3) % 3);
}

static void get_scatter_target(int idx, int *tx, int *ty)
{
    *tx = g_ghosts[idx].home_tx;
    *ty = g_ghosts[idx].home_ty;
}

static void get_chase_target(int idx, int *tx, int *ty)
{
    int ptx, pty;
    int bx, by;
    int ahead_x, ahead_y;
    int dx, dy;

    ptx = g_pac.wx / TILE_SIZE;
    pty = g_pac.wy / TILE_SIZE;
    ahead_x = ptx + g_dir_dx[g_pac.dir] * 4;
    ahead_y = pty + g_dir_dy[g_pac.dir] * 4;

    if (idx == GHOST_BLINKY) {
        *tx = ptx;
        *ty = pty;
    } else if (idx == GHOST_PINKY) {
        *tx = ahead_x;
        *ty = ahead_y;
    } else if (idx == GHOST_INKY) {
        bx = g_ghosts[GHOST_BLINKY].wx / TILE_SIZE;
        by = g_ghosts[GHOST_BLINKY].wy / TILE_SIZE;
        ahead_x = ptx + g_dir_dx[g_pac.dir] * 2;
        ahead_y = pty + g_dir_dy[g_pac.dir] * 2;
        *tx = ahead_x + (ahead_x - bx);
        *ty = ahead_y + (ahead_y - by);
    } else {
        dx = ptx - g_ghosts[idx].wx / TILE_SIZE;
        dy = pty - g_ghosts[idx].wy / TILE_SIZE;
        if (dx * dx + dy * dy > 64) {
            *tx = ptx;
            *ty = pty;
        } else {
            *tx = g_ghosts[idx].home_tx;
            *ty = g_ghosts[idx].home_ty;
        }
    }

    *tx = clamp_i(*tx, 0, MAZE_W - 1);
    *ty = clamp_i(*ty, 0, MAZE_H - 1);
}

static void choose_ghost_dir(int idx)
{
    GhostState *g;
    int dir;
    int best_dir;
    long best_score;
    int tx, ty;
    int gx, gy;

    g = &g_ghosts[idx];
    gx = g->wx / TILE_SIZE;
    gy = g->wy / TILE_SIZE;

    if (g->mode == GHOST_EATEN) {
        tx = HOUSE_EXIT_TX;
        ty = HOUSE_TOP + 2;
    } else if (g->mode == GHOST_FRIGHT) {
        best_dir = DIR_NONE;
        for (dir = 0; dir < 4; dir++) {
            if (!can_move_ghost(g, dir)) continue;
            if (!g->in_house && dir == g_opposite_dir[g->dir]) continue;
            best_dir = dir;
            if ((rand() & 1) != 0) break;
        }
        if (best_dir != DIR_NONE) g->dir = best_dir;
        return;
    } else if (g->mode == GHOST_SCATTER) {
        get_scatter_target(idx, &tx, &ty);
    } else {
        get_chase_target(idx, &tx, &ty);
    }

    best_dir = DIR_NONE;
    best_score = 0x7FFFFFFFL;
    for (dir = 0; dir < 4; dir++) {
        int nx, ny;
        long score;
        if (!can_move_ghost(g, dir)) continue;
        if (!g->in_house && dir == g_opposite_dir[g->dir]) continue;
        nx = gx + g_dir_dx[dir];
        ny = gy + g_dir_dy[dir];
        score = (long)(nx - tx) * (long)(nx - tx) + (long)(ny - ty) * (long)(ny - ty);
        if (best_dir == DIR_NONE || score < best_score) {
            best_dir = dir;
            best_score = score;
        }
    }

    if (best_dir == DIR_NONE) {
        if (can_move_ghost(g, g_opposite_dir[g->dir]))
            best_dir = g_opposite_dir[g->dir];
        else
            best_dir = g->dir;
    }
    g->dir = best_dir;
}

static void update_ghosts(void)
{
    int i;
    int base_mode;

    if (g_game.scatter_timer > 0) {
        g_game.scatter_timer--;
        if (g_game.scatter_timer == 0) {
            g_game.scatter_mode = !g_game.scatter_mode;
            g_game.scatter_timer = g_game.scatter_mode ? SCATTER_FRAMES : CHASE_FRAMES;
            reverse_active_ghosts();
            if (g_game.power_timer <= 0)
                restore_ghost_modes();
        }
    }

    if (g_game.power_timer > 0) {
        g_game.power_timer--;
        if (g_game.power_timer == 0)
            restore_ghost_modes();
    }

    base_mode = current_base_ghost_mode();

    for (i = 0; i < GHOST_COUNT; i++) {
        GhostState *g;
        int speed;
        g = &g_ghosts[i];

        if (g->mode == GHOST_FRIGHT && g_game.power_timer <= 0)
            g->mode = base_mode;
        if (g->mode != GHOST_FRIGHT && g->mode != GHOST_EATEN)
            g->mode = base_mode;

        if (g->in_house && g->release_timer > 0 && g->mode != GHOST_EATEN) {
            g->release_timer--;
            continue;
        }

        if (entity_aligned(g->wx, g->wy))
            choose_ghost_dir(i);

        speed = GHOST_SPEED + (g_game.level > 5 ? 1 : 0);
        if (g->mode == GHOST_FRIGHT) speed = GHOST_FRIGHT_SPEED;
        if (g->mode == GHOST_EATEN) speed = GHOST_EYES_SPEED;
        if ((g->wy / TILE_SIZE) == TUNNEL_ROW && ((g_frame_no + i) & 1L)) speed = 0;

        g->wx += g_dir_dx[g->dir] * speed;
        g->wy += g_dir_dy[g->dir] * speed;

        if ((g->wy / TILE_SIZE) == TUNNEL_ROW) {
            if (g->wx < -TILE_SIZE)
                g->wx = (MAZE_W - 1) * TILE_SIZE;
            else if (g->wx > (MAZE_W - 1) * TILE_SIZE)
                g->wx = -TILE_SIZE;
        }

        if (g->mode == GHOST_EATEN &&
            (g->wx / TILE_SIZE) == HOUSE_EXIT_TX &&
            (g->wy / TILE_SIZE) >= HOUSE_TOP + 1 &&
            (g->wy / TILE_SIZE) <= HOUSE_TOP + 3) {
            g->mode = base_mode;
            g->in_house = 1;
            g->release_timer = 90;
            g->wx = HOUSE_EXIT_TX * TILE_SIZE;
            g->wy = (HOUSE_TOP + 2) * TILE_SIZE;
            g->dir = DIR_UP;
        }

        if (g->in_house && (g->wy / TILE_SIZE) <= HOUSE_DOOR_Y - 1)
            g->in_house = 0;

        g->anim_frame = (int)((g_frame_no / 8 + i) & 1L);
    }
}

static void handle_collisions(void)
{
    int i;
    for (i = 0; i < GHOST_COUNT; i++) {
        int dx, dy;
        dx = g_pac.wx - g_ghosts[i].wx;
        dy = g_pac.wy - g_ghosts[i].wy;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx < 12 && dy < 12) {
            if (g_ghosts[i].mode == GHOST_FRIGHT) {
                g_game.score += 200 << g_game.ghosts_eaten_combo;
                if (g_game.ghosts_eaten_combo < 3) g_game.ghosts_eaten_combo++;
                g_ghosts[i].mode = GHOST_EATEN;
                g_ghosts[i].in_house = 0;
                g_ghosts[i].dir = g_opposite_dir[g_ghosts[i].dir];
            } else if (g_ghosts[i].mode != GHOST_EATEN) {
                lose_life();
                return;
            }
        }
    }

    if (g_game.fruit_visible) {
        int fx, fy, dx, dy;
        fx = FRUIT_TX * TILE_SIZE;
        fy = FRUIT_TY * TILE_SIZE;
        dx = g_pac.wx - fx;
        dy = g_pac.wy - fy;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        if (dx < 12 && dy < 12) {
            g_game.score += g_game.fruit_award;
            g_game.fruit_visible = 0;
            g_game.fruit_timer = 0;
        }
    }
}

static void update_gameplay(void)
{
    if (g_game.pause) return;

    refresh_pellet_tiles(0);

    if (g_game.ready_timer > 0) {
        g_game.ready_timer--;
        return;
    }

    if (g_game.death_timer > 0) {
        g_game.death_timer--;
        g_pac.death_frame = 7 - (g_game.death_timer * 8 / DEATH_FRAMES);
        if (g_pac.death_frame < 0) g_pac.death_frame = 0;
        if (g_game.death_timer == 0) {
            g_game.lives--;
            if (g_game.lives <= 0) {
                g_game.game_over = 1;
                g_screen = SCR_GAME_OVER;
            } else {
                g_game.ready_timer = READY_FRAMES;
                reset_positions();
            }
        }
        return;
    }

    if (g_game.level_clear_timer > 0) {
        g_game.level_clear_timer--;
        if (g_game.level_clear_timer == 0)
            next_level();
        return;
    }

    if (g_game.fruit_visible && g_game.fruit_timer > 0) {
        g_game.fruit_timer--;
        if (g_game.fruit_timer == 0)
            g_game.fruit_visible = 0;
    }

    update_pac();
    update_ghosts();
    handle_collisions();

    if (g_game.dots_eaten >= g_game.dots_total) {
        g_game.level_clear_timer = LEVEL_CLEAR_FRAMES;
        render_full_maze_bitmap(((g_frame_no / 6) & 1L) ? 1 : 0);
    }
}

static void copy_viewport_to_stage_or_frame(unsigned char *dst, int pitch, int cam_x, int cam_y)
{
    int y;
    const unsigned char *src;
    for (y = 0; y < g_play_h; y++) {
        src = g_ram_maze + (long)(cam_y + y) * MAZE_PX_W + cam_x;
        memcpy(dst + (long)y * pitch, src, (size_t)g_xres);
    }
    if (pitch == g_xres)
        memset(dst + (long)g_play_h * pitch, 0, (size_t)(g_yres - g_play_h) * (size_t)pitch);
}

static void sw_sprite_key(const unsigned char *spr, int spr_pitch,
                          int sx, int sy,
                          unsigned char *frame, int dx, int dy,
                          int w, int h)
{
    int x, y;
    for (y = 0; y < h; y++) {
        int fy = dy + y;
        if (fy < 0 || fy >= g_play_h) continue;
        for (x = 0; x < w; x++) {
            int fx = dx + x;
            unsigned char c;
            if (fx < 0 || fx >= g_xres) continue;
            c = spr[(long)(sy + y) * spr_pitch + sx + x];
            if (c != 0) frame[(long)fy * g_xres + fx] = c;
        }
    }
}

static void sw_blit_to_vram(const unsigned char *frame, int back_y)
{
    int y;
    for (y = 0; y < g_yres; y++)
        wc_memcpy(g_lfb + (long)(back_y + y) * g_pitch,
                  frame + (long)y * g_xres, (unsigned long)g_xres);
}

static void draw_sprite_gpu(int cell_x, int cell_y, int x, int y, int back_y)
{
    gpu_blit_po_key(g_sprite_po, cell_x * SPR_W, cell_y * SPR_H,
                    g_default_po, x, back_y + y, SPR_W, SPR_H, 0);
}

static void draw_sprite_cpu(int cell_x, int cell_y, int x, int y)
{
    sw_sprite_key(g_ram_sprites, SPR_SHEET_W,
                  cell_x * SPR_W, cell_y * SPR_H,
                  g_ram_frame, x, y, SPR_W, SPR_H);
}

static void compute_camera(int *cam_x, int *cam_y)
{
    *cam_x = g_pac.wx - g_xres / 2 + TILE_SIZE / 2;
    *cam_y = g_pac.wy - g_play_h / 2 + TILE_SIZE / 2;
    *cam_x = clamp_i(*cam_x, 0, MAZE_PX_W - g_xres);
    *cam_y = clamp_i(*cam_y, 0, MAZE_PX_H - g_play_h);
}

static int pac_sprite_cell(void)
{
    int dir;
    dir = g_pac.dir;
    if (dir == DIR_NONE) dir = DIR_LEFT;
    return dir * 3 + g_pac.anim_frame;
}

static void ghost_sprite_cell(int idx, int *cell_x, int *cell_y)
{
    GhostState *g;
    g = &g_ghosts[idx];
    if (g->mode == GHOST_EATEN) {
        *cell_x = 4 + g->dir;
        *cell_y = SPRITE_MISC_ROW;
    } else if (g->mode == GHOST_FRIGHT) {
        int flash;
        flash = (g_game.power_timer < 120 && ((g_frame_no / 8) & 1L)) ? 2 : 0;
        *cell_x = flash + g->anim_frame;
        *cell_y = SPRITE_MISC_ROW;
    } else {
        *cell_x = g->dir * 2 + g->anim_frame;
        *cell_y = SPRITE_BLINKY_ROW + idx;
    }
}

static void draw_entities_gpu(int cam_x, int cam_y, int back_y)
{
    int i;
    int x, y;
    int cell_x, cell_y;

    if (g_game.fruit_visible) {
        x = FRUIT_TX * TILE_SIZE - cam_x;
        y = FRUIT_TY * TILE_SIZE - cam_y;
        draw_sprite_gpu(2, SPRITE_ITEM_ROW, x, y, back_y);
    }

    for (i = 0; i < GHOST_COUNT; i++) {
        ghost_sprite_cell(i, &cell_x, &cell_y);
        x = g_ghosts[i].wx - cam_x;
        y = g_ghosts[i].wy - cam_y;
        draw_sprite_gpu(cell_x, cell_y, x, y, back_y);
    }

    if (g_pac.alive) {
        cell_x = pac_sprite_cell();
        draw_sprite_gpu(cell_x, SPRITE_PAC_ROW,
                        g_pac.wx - cam_x, g_pac.wy - cam_y, back_y);
    } else {
        draw_sprite_gpu(g_pac.death_frame, SPRITE_DEATH_ROW,
                        g_pac.wx - cam_x, g_pac.wy - cam_y, back_y);
    }
}

static void draw_entities_cpu(int cam_x, int cam_y)
{
    int i;
    int x, y;
    int cell_x, cell_y;

    if (g_game.fruit_visible) {
        x = FRUIT_TX * TILE_SIZE - cam_x;
        y = FRUIT_TY * TILE_SIZE - cam_y;
        draw_sprite_cpu(2, SPRITE_ITEM_ROW, x, y);
    }

    for (i = 0; i < GHOST_COUNT; i++) {
        ghost_sprite_cell(i, &cell_x, &cell_y);
        x = g_ghosts[i].wx - cam_x;
        y = g_ghosts[i].wy - cam_y;
        draw_sprite_cpu(cell_x, cell_y, x, y);
    }

    if (g_pac.alive) {
        cell_x = pac_sprite_cell();
        draw_sprite_cpu(cell_x, SPRITE_PAC_ROW, g_pac.wx - cam_x, g_pac.wy - cam_y);
    } else {
        draw_sprite_cpu(g_pac.death_frame, SPRITE_DEATH_ROW,
                        g_pac.wx - cam_x, g_pac.wy - cam_y);
    }
}

static void draw_playfield(int back_y)
{
    int cam_x, cam_y;
    compute_camera(&cam_x, &cam_y);

    if (g_gpu_mode) {
        int stage_y;
        stage_y = PAGE_STAGE * g_page_stride;
        copy_viewport_to_stage_or_frame(g_lfb + (long)stage_y * g_pitch, g_pitch, cam_x, cam_y);
        gpu_flush_2d_cache();
        gpu_blit_po(g_stage_po, 0, 0, g_default_po, 0, back_y, g_xres, g_play_h);
        gpu_fill(0, back_y + g_play_h, g_xres, HUD_H, 0);
        draw_entities_gpu(cam_x, cam_y, back_y);
        gpu_wait_idle();
        gpu_color_compare_off();
        gpu_pitch_offset_reset();
    } else {
        copy_viewport_to_stage_or_frame(g_ram_frame, g_xres, cam_x, cam_y);
        draw_entities_cpu(cam_x, cam_y);
        sw_blit_to_vram(g_ram_frame, back_y);
    }
}

static void draw_hud(int back_y, double fps, double cpu_pct)
{
    char buf[160];
    sprintf(buf, "Score:%d  Lives:%d  Lv:%d  [%s] %.1ffps CPU:%d%%",
            g_game.score, g_game.lives, g_game.level,
            g_gpu_mode ? "GPU" : "CPU",
            fps, (int)(cpu_pct + 0.5));
    cpu_str(4, back_y + g_play_h + 4, buf, HUD_WHITE, 1);

    if (g_game.ready_timer > 0)
        cpu_str_c(back_y + g_play_h / 2 - 8, "READY!", HUD_YELLOW, 2);
    if (g_game.pause)
        cpu_str_c(back_y + g_play_h / 2 - 8, "PAUSED", HUD_CYAN, 2);
    if (g_game.level_clear_timer > 0)
        cpu_str_c(back_y + g_play_h / 2 - 8, "LEVEL CLEAR!", HUD_GREEN, 2);
    if (g_game.death_timer > 0)
        cpu_str_c(back_y + g_play_h / 2 - 8, "OUCH!", HUD_RED, 2);
}

static void draw_title_screen(int back_y, double fps)
{
    char buf[80];
    gpu_fill(0, back_y, g_xres, g_yres, 0);
    gpu_wait_idle();
    cpu_str_c(back_y + 80, "RPAC - Scrolling Pac-Man", HUD_YELLOW, 2);
    cpu_str_c(back_y + 130, "Pac-Man stays centred while the maze scrolls.", HUD_WHITE, 1);
    cpu_str_c(back_y + 154, "Arrow keys move, SPACE toggles GPU/CPU, P pauses, ESC quits.", HUD_CYAN, 1);
    cpu_str_c(back_y + 190, "Dots, power pellets, four ghosts, lives, levels, fruit.", HUD_GREY, 1);
    cpu_str_c(back_y + 260, "Press any key to start", HUD_GREEN, 2);
    sprintf(buf, "Render mode: %s   %.1f FPS", g_gpu_mode ? "GPU" : "CPU", fps);
    cpu_str_c(back_y + 320, buf, HUD_CYAN, 1);
}

static void draw_game_over_screen(int back_y)
{
    gpu_fill(0, back_y, g_xres, g_yres, 0);
    gpu_wait_idle();
    cpu_str_c(back_y + 180, "GAME OVER", HUD_RED, 3);
    {
        char buf[80];
        sprintf(buf, "Final score: %d", g_game.score);
        cpu_str_c(back_y + 250, buf, HUD_WHITE, 2);
    }
    cpu_str_c(back_y + 300, "Press any key to restart, ESC to quit", HUD_CYAN, 1);
}

static void poll_game_keys(int *quit)
{
    while (kbhit()) {
        int ch;
        ch = getch();
        if (g_screen == SCR_TITLE) {
            if (ch == 27) { *quit = 1; return; }
            start_new_game();
            return;
        }
        if (g_screen == SCR_GAME_OVER) {
            if (ch == 27) { *quit = 1; return; }
            start_new_game();
            return;
        }

        if (ch == 27) {
            *quit = 1;
            return;
        }
        if (ch == ' ' || ch == 'S' || ch == 's') {
            g_gpu_mode = !g_gpu_mode;
            continue;
        }
        if (ch == 'P' || ch == 'p') {
            g_game.pause = !g_game.pause;
            continue;
        }
        if (ch == 0 || ch == 0xE0) {
            int sc;
            sc = getch();
            if (sc == 0x48) g_pac.next_dir = DIR_UP;
            else if (sc == 0x4B) g_pac.next_dir = DIR_LEFT;
            else if (sc == 0x50) g_pac.next_dir = DIR_DOWN;
            else if (sc == 0x4D) g_pac.next_dir = DIR_RIGHT;
        }
    }
}

int main(void)
{
    int quit;
    int back_y;
    long fps_count;
    clock_t fps_t0, t_render_start, t_render_end, t_frame_end;
    double fps, cpu_pct, cpu_acc;
    long cpu_samples;
    unsigned long need;

    if (radeon_init("RPAC - Scrolling Pac-Man", PAGE_COUNT))
        return 1;

    setup_pac_palette();
    g_play_h = g_yres - HUD_H;
    need = (unsigned long)g_pitch * (unsigned long)g_page_stride * PAGE_COUNT;
    if (g_vram_mb > 0 && need > g_vram_mb * 1024UL * 1024UL) {
        gpu_fill(0, 0, g_xres, g_yres, 0);
        gpu_wait_idle();
        cpu_str_c(g_yres / 2, "Not enough VRAM for RPAC", HUD_RED, 2);
        getch();
        radeon_cleanup();
        return 1;
    }

    if (!sw_alloc()) {
        gpu_fill(0, 0, g_xres, g_yres, 0);
        gpu_wait_idle();
        cpu_str_c(g_yres / 2, "Not enough RAM for RPAC buffers", HUD_RED, 2);
        getch();
        sw_free();
        radeon_cleanup();
        return 1;
    }

    g_stage_po = make_pitch_offset((unsigned long)PAGE_STAGE * (unsigned long)g_page_stride * (unsigned long)g_pitch);
    g_sprite_po = make_pitch_offset((unsigned long)PAGE_SPRITES * (unsigned long)g_page_stride * (unsigned long)g_pitch);

    gpu_scissor_max();
    gen_sprite_sheet();
    upload_sprite_sheet();
    init_maze_layout();
    render_full_maze_bitmap(1);
    recount_dots();
    reset_positions();

    gpu_fill(0, 0, g_xres, g_page_stride * 2, 0);
    gpu_wait_idle();

    quit = 0;
    g_back_page = 1;
    g_frame_no = 0;
    fps_count = 0;
    fps = 0.0;
    cpu_pct = 0.0;
    cpu_acc = 0.0;
    cpu_samples = 0;
    fps_t0 = clock();
    srand(12345);

    while (!quit) {
        poll_game_keys(&quit);
        if (quit) break;

        back_y = g_back_page * g_page_stride;
        t_render_start = clock();

        if (g_screen == SCR_TITLE) {
            draw_title_screen(back_y, fps);
        } else if (g_screen == SCR_GAME_OVER) {
            draw_game_over_screen(back_y);
        } else {
            update_gameplay();
            draw_playfield(back_y);
            draw_hud(back_y, fps, cpu_pct);
        }

        t_render_end = clock();
        flip_page(g_back_page);
        t_frame_end = clock();

        fps_count++;
        {
            clock_t now;
            double elapsed;
            now = clock();
            elapsed = (double)(now - fps_t0) / CLOCKS_PER_SEC;
            if (elapsed >= 1.0) {
                fps = (double)fps_count / elapsed;
                fps_count = 0;
                fps_t0 = now;
                if (cpu_samples > 0)
                    cpu_pct = cpu_acc / cpu_samples;
                cpu_acc = 0.0;
                cpu_samples = 0;
            }
        }

        {
            double render_t, frame_t;
            render_t = (double)(t_render_end - t_render_start);
            frame_t  = (double)(t_frame_end  - t_render_start);
            if (frame_t > 0.0) {
                cpu_acc += render_t * 100.0 / frame_t;
                cpu_samples++;
            }
        }

        g_back_page ^= 1;
        g_frame_no++;
    }

    flip_restore_page0();
    gpu_pitch_offset_reset();
    gpu_scissor_default();
    gpu_color_compare_off();
    sw_free();
    radeon_cleanup();
    return 0;
}
