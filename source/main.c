/*
 * blox-ps3: baseplate + noob prototype for the PS3 (PSL1GHT).
 * Runs on the Cell PPE. Software rasterizer renders at 640x360 into a
 * main-memory buffer, then gets upscaled into the RSX framebuffer.
 *
 * Controls: Left stick = move, Right stick = camera, X = jump, L2 = sprint,
 *           START = pause menu.
 * Menus: D-pad or left stick, L1/R1 = tabs, X = select, O = back.
 * Maps: Baseplate, Castle. Avatar creator (hats, colours, faces), 11 preset skins.
 * Settings: control tips, FPS counter, sound, FPS cap. Walk/jump/land sounds.
 *
 * Performance notes (what keeps this fast on the in-order PPE):
 *  - The baseplate is NOT polygons. It's drawn as an analytic ground plane, one
 *    scanline at a time with integer math, so it costs 0 triangles and needs no
 *    z-buffer and no screen clear.
 *  - The z-buffer is only cleared/used inside the character's screen rectangle.
 *  - Back-face culling, hidden-face removal, and face decals instead of boxes.
 *  - Incremental (add-only) triangle rasterization, no per-pixel multiplies.
 *  - Integer 2x/3x upscale with 64-bit stores; renders to 720p even on 1080p TVs.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <malloc.h>
#include <unistd.h>
#include <time.h>
#include <ppu-lv2.h>
#include <sysutil/sysutil.h>
#include <sysutil/video.h>
#include <rsx/rsx.h>
#include <io/pad.h>
#include <audio/audio.h>
#include <sys/event_queue.h>

#define IW      640
#define IH      360
#define FOCAL   300.0f
#define NEAR    0.1f
#define PI_F    3.14159265f
#define SKY_COL 0x6FB4F0u
#define FLOOR_EXT 256            /* baseplate = 512 x 512 studs, centered on 0,0 */
#define COURT_EXT 36             /* castle courtyard half-size (stone floor) */
#ifndef BLOX_DEFAULT_CAP_IDX
#define BLOX_DEFAULT_CAP_IDX 3   /* 0..5 = 15,30,45,60,uncapped,custom */
#endif

enum { MAP_BASE, MAP_CASTLE, NUM_MAPS };

#define GCM_CB_SIZE 0x100000
#define HOST_SIZE   (32 * 1024 * 1024)

typedef struct { float x, y, z; } v3;

/* ---------- display (libgcm / RSX framebuffer) ---------- */

static gcmContextData *ctx;
static u32 *fb[2];
static u32 fb_off[2];
static u32 dw, dh, dpitch;
static int cur = 0, first_flip = 1;
static int scale = 0;            /* integer upscale factor, 0 = generic path */
static int *xmap;
static u32 *linebuf;
static int running = 1;

static void sysutil_cb(u64 status, u64 param, void *usrdata)
{
    (void)param; (void)usrdata;
    if (status == SYSUTIL_EXIT_GAME) running = 0;
}

static void gfx_init(void)
{
    void *host = memalign(1024 * 1024, HOST_SIZE);
    rsxInit(&ctx, GCM_CB_SIZE, HOST_SIZE, host);

    videoState state;
    videoResolution res;
    videoConfiguration vconf;

    videoGetState(0, 0, &state);
    u32 resid = state.displayMode.resolution;
    /* 1080p means 2.25x the framebuffer writes of 720p; let the TV upscale. */
    if (resid == VIDEO_RESOLUTION_1080 &&
        videoGetResolutionAvailability(0, VIDEO_RESOLUTION_720, VIDEO_ASPECT_AUTO, 0))
        resid = VIDEO_RESOLUTION_720;
    videoGetResolution(resid, &res);

    memset(&vconf, 0, sizeof(vconf));
    vconf.resolution = resid;
    vconf.format     = VIDEO_BUFFER_FORMAT_XRGB;
    vconf.pitch      = res.width * sizeof(u32);
    vconf.aspect     = state.displayMode.aspect;
    videoConfigure(0, &vconf, NULL, 0);
    videoGetState(0, 0, &state);

    gcmSetFlipMode(GCM_FLIP_VSYNC);

    dw = res.width;
    dh = res.height;
    dpitch = vconf.pitch;

    for (int i = 0; i < 2; i++) {
        fb[i] = (u32 *)rsxMemalign(64, dh * dpitch);
        rsxAddressToOffset(fb[i], &fb_off[i]);
        gcmSetDisplayBuffer(i, fb_off[i], dpitch, dw, dh);
    }
    gcmResetFlipStatus();

    if (dw % IW == 0 && dh == (u32)IH * (dw / IW)) scale = (int)(dw / IW);
    xmap = (int *)malloc(dw * sizeof(int));
    linebuf = (u32 *)memalign(16, (dw + 8) * sizeof(u32));
    for (u32 x = 0; x < dw; x++) xmap[x] = (int)(x * IW / dw);
}

static int g_cap = 60;           /* fps cap, 0 = uncapped */
static double next_frame = 0.0;

/* <=60 uses vsync (no tearing); uncapped or >60 needs tearing-allowed flips */
static void apply_cap(int cap)
{
    g_cap = cap;
    gcmSetFlipMode((cap > 0 && cap <= 60) ? GCM_FLIP_VSYNC : GCM_FLIP_HSYNC);
    next_frame = 0.0;
}

static void wait_flip(void)
{
    while (gcmGetFlipStatus() != 0) usleep(200);
    gcmResetFlipStatus();
}

/* ---------- software renderer ---------- */

static u32 pix[IW * IH] __attribute__((aligned(16)));
static float zbuf[IW * IH] __attribute__((aligned(16)));

/* scissor rectangle (inclusive); triangles are clipped to it */
static int sc_x0 = 0, sc_y0 = 0, sc_x1 = IW - 1, sc_y1 = IH - 1;

static v3 cpos, cr, cu, cf; /* camera position + right/up/forward basis */

static v3 sub(v3 a, v3 b) { v3 r = { a.x - b.x, a.y - b.y, a.z - b.z }; return r; }
static float dot(v3 a, v3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
static v3 cross(v3 a, v3 b)
{
    v3 r = { a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x };
    return r;
}
static v3 norm(v3 a)
{
    float l = sqrtf(dot(a, a));
    if (l < 1e-6f) return a;
    v3 r = { a.x / l, a.y / l, a.z / l };
    return r;
}

static void set_camera(v3 pos, v3 target)
{
    cpos = pos;
    cf = norm(sub(target, pos));
    v3 up = { 0, 1, 0 };
    cr = norm(cross(cf, up));
    cu = cross(cr, cf);
}

static v3 to_cam(v3 p)
{
    v3 d = sub(p, cpos);
    v3 r = { dot(d, cr), dot(d, cu), dot(d, cf) };
    return r;
}

static u32 shade(const unsigned char *rgb, float s)
{
    u32 r = (u32)(rgb[0] * s), g = (u32)(rgb[1] * s), b = (u32)(rgb[2] * s);
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    return (r << 16) | (g << 8) | b;
}

/*
 * Triangle in camera space (all z >= NEAR), clipped to the scissor rect.
 * Barycentric weights and 1/z are stepped with additions only.
 */
static void tri(v3 a, v3 b, v3 c, u32 col)
{
    float ia = 1.0f / a.z, ib = 1.0f / b.z, ic = 1.0f / c.z;
    float x0 = IW * 0.5f + a.x * ia * FOCAL, y0 = IH * 0.5f - a.y * ia * FOCAL;
    float x1 = IW * 0.5f + b.x * ib * FOCAL, y1 = IH * 0.5f - b.y * ib * FOCAL;
    float x2 = IW * 0.5f + c.x * ic * FOCAL, y2 = IH * 0.5f - c.y * ic * FOCAL;

    float area = (x1 - x0) * (y2 - y0) - (y1 - y0) * (x2 - x0);
    if (fabsf(area) < 1e-6f) return;
    float inv = 1.0f / area;

    float fminx = fmaxf((float)sc_x0, fminf(x0, fminf(x1, x2)));
    float fmaxx = fminf((float)sc_x1, fmaxf(x0, fmaxf(x1, x2)));
    float fminy = fmaxf((float)sc_y0, fminf(y0, fminf(y1, y2)));
    float fmaxy = fminf((float)sc_y1, fmaxf(y0, fmaxf(y1, y2)));
    if (fminx > fmaxx || fminy > fmaxy) return;

    int minx = (int)fminx, maxx = (int)fmaxx;
    int miny = (int)fminy, maxy = (int)fmaxy;

    float dw0dx = (y1 - y2) * inv, dw0dy = (x2 - x1) * inv;
    float dw1dx = (y2 - y0) * inv, dw1dy = (x0 - x2) * inv;
    float dza = ia - ic, dzb = ib - ic;
    float diz = dw0dx * dza + dw1dx * dzb;

    float px = minx + 0.5f, py = miny + 0.5f;
    float w0 = ((x1 - px) * (y2 - py) - (y1 - py) * (x2 - px)) * inv;
    float w1 = ((x2 - px) * (y0 - py) - (y2 - py) * (x0 - px)) * inv;

    for (int y = miny; y <= maxy; y++) {
        float r0 = w0, r1 = w1, iz = ic + r0 * dza + r1 * dzb;
        float *zr = &zbuf[y * IW + minx];
        u32 *pr = &pix[y * IW + minx];
        for (int x = minx; x <= maxx; x++, r0 += dw0dx, r1 += dw1dx, iz += diz, zr++, pr++) {
            if (r0 >= 0.0f && r1 >= 0.0f && (r0 + r1) <= 1.0f && iz > *zr) {
                *zr = iz;
                *pr = col;
            }
        }
        w0 += dw0dy;
        w1 += dw1dy;
    }
}

/* convex polygon in camera space, clipped against the near plane */
static void draw_poly(const v3 *in, int n, u32 col)
{
    v3 out[8];
    int m = 0;
    for (int i = 0; i < n; i++) {
        v3 a = in[i], b = in[(i + 1) % n];
        int ain = a.z >= NEAR, bin = b.z >= NEAR;
        if (ain) out[m++] = a;
        if (ain != bin) {
            float t = (NEAR - a.z) / (b.z - a.z);
            v3 p = { a.x + (b.x - a.x) * t, a.y + (b.y - a.y) * t, NEAR };
            out[m++] = p;
        }
    }
    for (int i = 1; i + 1 < m; i++) tri(out[0], out[i], out[i + 1], col);
}

/* box corner i: bit0 = +x, bit1 = +y, bit2 = +z */
static const int FACES[6][4] = {
    {1, 3, 7, 5}, {0, 4, 6, 2}, {2, 6, 7, 3},
    {0, 1, 5, 4}, {4, 5, 7, 6}, {0, 2, 3, 1}
};
static const float FACE_SHADE[6] = { 0.85f, 0.65f, 1.0f, 0.5f, 0.75f, 0.6f };
/* face ids for the hidden-face masks: +x -x +y -y +z -z */
#define F_PX 1
#define F_NX 2
#define F_PY 4
#define F_NY 8

/*
 * One body part. `off` = part center relative to character origin (feet).
 * `swing` rotates the part about its top edge on the X axis (arms/legs).
 * `skip` = bitmask of faces that are always hidden inside other parts.
 * With `cull` set, those faces and faces pointing away from the camera are
 * dropped (map blocks). Player models pass cull = 0: every face is drawn and
 * the z-buffer decides what is visible, so there are no holes at any angle.
 */
static void draw_part(v3 origin, float yaw, v3 off, v3 half, float swing,
                      const unsigned char *rgb, int skip, int cull)
{
    float cs = cosf(swing), sn = sinf(swing);
    float cy = cosf(yaw), sy = sinf(yaw);
    v3 c[8];

    for (int i = 0; i < 8; i++) {
        float lx = (i & 1) ? half.x : -half.x;
        float ly = (i & 2) ? half.y : -half.y;
        float lz = (i & 4) ? half.z : -half.z;
        float ry = (ly - half.y) * cs - lz * sn;
        float rz = (ly - half.y) * sn + lz * cs;
        float px = off.x + lx, py = off.y + half.y + ry, pz = off.z + rz;
        v3 w = { origin.x + px * cy + pz * sy,
                 origin.y + py,
                 origin.z - px * sy + pz * cy };
        c[i] = to_cam(w);
    }
    v3 cen = { (c[0].x + c[7].x) * 0.5f, (c[0].y + c[7].y) * 0.5f, (c[0].z + c[7].z) * 0.5f };

    for (int f = 0; f < 6; f++) {
        if (cull && (skip & (1 << f))) continue;
        v3 q[4] = { c[FACES[f][0]], c[FACES[f][1]], c[FACES[f][2]], c[FACES[f][3]] };
        if (cull) {
            v3 n = cross(sub(q[1], q[0]), sub(q[2], q[0]));
            v3 fc = { (q[0].x + q[2].x) * 0.5f, (q[0].y + q[2].y) * 0.5f, (q[0].z + q[2].z) * 0.5f };
            if (dot(n, sub(fc, cen)) < 0.0f) { n.x = -n.x; n.y = -n.y; n.z = -n.z; } /* outward */
            if (dot(n, q[0]) >= 0.0f) continue;                                     /* faces away */
        }
        draw_poly(q, 4, shade(rgb, FACE_SHADE[f]));
    }
}

/* flat quad on the character's +z side (used for eyes/mouth), 1 quad not 6 faces */
#define PLAYER_CULL 0     /* 0 = no culling on player models (all faces drawn) */

static void draw_decal(v3 origin, float yaw, v3 c, float hx, float hy, u32 col)
{
    float cy = cosf(yaw), sy = sinf(yaw);
    float lx[4] = { -hx, hx, hx, -hx }, ly[4] = { -hy, -hy, hy, hy };
    v3 q[4];
    for (int i = 0; i < 4; i++) {
        float px = c.x + lx[i], py = c.y + ly[i], pz = c.z;
        v3 w = { origin.x + px * cy + pz * sy, origin.y + py, origin.z - px * sy + pz * cy };
        q[i] = to_cam(w);
    }
    v3 nw = { sy, 0.0f, cy };
    v3 nc = { dot(nw, cr), dot(nw, cu), dot(nw, cf) };
    v3 mid = { (q[0].x + q[2].x) * 0.5f, (q[0].y + q[2].y) * 0.5f, (q[0].z + q[2].z) * 0.5f };
    if (PLAYER_CULL && dot(nc, mid) >= 0.0f) return;
    draw_poly(q, 4, col);
}

enum { FACE_STRAIGHT, FACE_SMILE, FACE_ANGRY, FACE_VISOR, FACE_SLIT };
enum { HAT_NONE, HAT_TOP, HAT_SKATER, HAT_BEANIE, HAT_CROWN, HAT_HEADBAND, HAT_ANTENNA, NUM_HATS };
enum { CHEST_NONE, CHEST_ROBOT, CHEST_SUIT, CHEST_TEE, CHEST_COLLAR };

typedef struct {
    const char *name;
    unsigned char skin[3], torso[3], legs[3];   /* head+arms, torso, legs */
    u32 eye;                                    /* face / visor colour */
    int face;
    int hat;
    unsigned char hat_col[3];
    int chest;                                  /* decal on the torso front */
} Costume;

static const Costume COSTUMES[] = {
    { "NOOB",        {245, 205,  48}, { 13, 105, 172}, { 40, 127,  71}, 0x141414, FACE_STRAIGHT, HAT_NONE,     {  0,   0,   0}, CHEST_NONE   },
    { "RED",         {245, 205,  48}, {196,  40,  28}, { 60,  60,  66}, 0x141414, FACE_SMILE,    HAT_NONE,     {  0,   0,   0}, CHEST_NONE   },
    { "ZOMBIE",      { 96, 150,  84}, { 52,  84,  96}, { 74,  92,  62}, 0x6A1010, FACE_ANGRY,    HAT_NONE,     {  0,   0,   0}, CHEST_NONE   },
    { "NINJA",       { 38,  38,  44}, { 28,  28,  34}, { 28,  28,  34}, 0xF2F2F2, FACE_SLIT,     HAT_HEADBAND, {200,  40,  40}, CHEST_NONE   },
    { "ROBOT",       {170, 178, 188}, { 96, 106, 128}, { 74,  80,  98}, 0x40E8F8, FACE_VISOR,    HAT_ANTENNA,  { 64, 232, 248}, CHEST_ROBOT  },
    { "PINK",        {250, 214, 176}, {240, 120, 170}, {150,  96, 200}, 0x141414, FACE_SMILE,    HAT_NONE,     {  0,   0,   0}, CHEST_NONE   },
    { "GOLD",        {255, 205,  60}, {232, 170,  24}, {196, 136,  12}, 0x3A2A00, FACE_STRAIGHT, HAT_NONE,     {  0,   0,   0}, CHEST_NONE   },
    { "TOP HAT MAN", {250, 214, 176}, { 28,  28,  34}, { 44,  44,  54}, 0x141414, FACE_SMILE,    HAT_TOP,      { 30,  30,  34}, CHEST_SUIT   },
    { "SKATER BOY",  {220, 170, 120}, {235, 235, 240}, { 60,  90, 160}, 0x141414, FACE_SMILE,    HAT_SKATER,   {200,  40,  40}, CHEST_TEE    },
    { "KING",        {250, 214, 176}, {170,  30,  40}, { 30,  30, 110}, 0x141414, FACE_STRAIGHT, HAT_CROWN,    {240, 200,  60}, CHEST_COLLAR },
    { "SNOW",        {250, 225, 215}, { 90, 150, 220}, { 40,  70, 140}, 0x141414, FACE_SMILE,    HAT_BEANIE,   {220,  60,  60}, CHEST_NONE   },
};
#define NUM_PRESETS ((int)(sizeof(COSTUMES) / sizeof(COSTUMES[0])))
#define CUSTOM_SKIN NUM_PRESETS                 /* skin index of the player-made avatar */
#define NUM_SKINS   (NUM_PRESETS + 1)

static const char *HAT_NAMES[NUM_HATS] = { "NONE", "TOP HAT", "SKATER CAP", "BEANIE", "CROWN", "HEADBAND", "ANTENNA" };

static const unsigned char COL_GRASS1[3] = { 75, 151, 75 };
static const unsigned char COL_GRASS2[3] = { 66, 135, 66 };
static const unsigned char COL_COURT1[3] = { 150, 150, 156 };
static const unsigned char COL_COURT2[3] = { 132, 132, 140 };

/*
 * Ground, drawn analytically: for each screen row the ground plane is hit at
 * one distance t, and world x/z then change linearly across the row, so a
 * 16.16 fixed-point DDA gives the checker colour with a few integer ops per
 * pixel. 0 triangles, no z-buffer, writes every pixel (so no screen clear).
 * The castle map swaps in a stone courtyard inside the walls.
 */
static void draw_floor(int map)
{
    const u32 g1 = shade(COL_GRASS1, 1.0f), g2 = shade(COL_GRASS2, 1.0f);
    const u32 c1 = shade(COL_COURT1, 1.0f), c2 = shade(COL_COURT2, 1.0f);
    const u64 sky2 = ((u64)SKY_COL << 32) | SKY_COL;
    const unsigned EXT = (unsigned)FLOOR_EXT << 16, CEXT = (unsigned)COURT_EXT << 16;
    const int castle = (map == MAP_CASTLE);

    if (cpos.y < 0.05f) {                       /* camera under the ground (falling): sky only */
        u64 *r = (u64 *)pix;
        for (int i = 0; i < IW * IH / 2; i++) r[i] = sky2;
        return;
    }

    for (int y = 0; y < IH; y++) {
        u32 *row = &pix[y * IW];
        float sy = (IH * 0.5f - (y + 0.5f)) / FOCAL;
        float diry = cf.y + sy * cu.y;
        float t = (diry < -1e-4f) ? -cpos.y / diry : 1e9f;

        if (t > 900.0f) {                       /* above the horizon: sky */
            u64 *r = (u64 *)row;
            for (int x = 0; x < IW / 2; x++) r[x] = sky2;
            continue;
        }

        float k = t / FOCAL;
        float sx0 = 0.5f - IW * 0.5f;
        float wx = cpos.x + t * (cf.x + sy * cu.x) + sx0 * k * cr.x;
        float wz = cpos.z + t * (cf.z + sy * cu.z) + sx0 * k * cr.z;
        int ix = (int)(wx * 65536.0f), iz = (int)(wz * 65536.0f);
        int dx = (int)(k * cr.x * 65536.0f), dz = (int)(k * cr.z * 65536.0f);

        if (castle) {
            for (int x = 0; x < IW; x++, ix += dx, iz += dz) {
                if ((unsigned)(ix + (int)EXT) >= 2u * EXT || (unsigned)(iz + (int)EXT) >= 2u * EXT)
                    row[x] = SKY_COL;
                else if ((unsigned)(ix + (int)CEXT) < 2u * CEXT && (unsigned)(iz + (int)CEXT) < 2u * CEXT)
                    row[x] = (((ix >> 18) + (iz >> 18)) & 1) ? c1 : c2;
                else
                    row[x] = (((ix >> 18) + (iz >> 18)) & 1) ? g1 : g2;
            }
        } else {
            for (int x = 0; x < IW; x++, ix += dx, iz += dz) {
                if ((unsigned)(ix + (int)EXT) >= 2u * EXT || (unsigned)(iz + (int)EXT) >= 2u * EXT)
                    row[x] = SKY_COL;               /* off the edge of the baseplate: void */
                else
                    row[x] = (((ix >> 18) + (iz >> 18)) & 1) ? g1 : g2;   /* 4-stud tiles */
            }
        }
    }
}

/*
 * Conservative screen rectangle of the character; sets the scissor and clears
 * the z-buffer only inside it. Returns 0 if the character is off screen.
 */
static int setup_noob_region(v3 pos, float yaw)
{
    float cy = cosf(yaw), sy = sinf(yaw);
    float minx = 1e9f, maxx = -1e9f, miny = 1e9f, maxy = -1e9f;
    int behind = 0;

    for (int i = 0; i < 8; i++) {
        float lx = (i & 1) ? 2.4f : -2.4f;
        float ly = (i & 2) ? 5.4f : -0.2f;
        float lz = (i & 4) ? 2.4f : -2.4f;
        v3 w = { pos.x + lx * cy + lz * sy, pos.y + ly, pos.z - lx * sy + lz * cy };
        v3 c = to_cam(w);
        if (c.z < NEAR) { behind = 1; continue; }
        float sx = IW * 0.5f + c.x / c.z * FOCAL, sy2 = IH * 0.5f - c.y / c.z * FOCAL;
        if (sx < minx) minx = sx;
        if (sx > maxx) maxx = sx;
        if (sy2 < miny) miny = sy2;
        if (sy2 > maxy) maxy = sy2;
    }
    if (behind) { minx = 0; miny = 0; maxx = IW - 1; maxy = IH - 1; }

    minx = fmaxf(0.0f, minx - 2); miny = fmaxf(0.0f, miny - 2);
    maxx = fminf(IW - 1.0f, maxx + 2); maxy = fminf(IH - 1.0f, maxy + 2);
    if (minx > maxx || miny > maxy) return 0;

    sc_x0 = (int)minx; sc_x1 = (int)maxx;
    sc_y0 = (int)miny; sc_y1 = (int)maxy;
    for (int y = sc_y0; y <= sc_y1; y++)
        memset(&zbuf[y * IW + sc_x0], 0, (size_t)(sc_x1 - sc_x0 + 1) * sizeof(float));
    return 1;
}

/* all face art is flat quads on the head's front, never boxes */
static void draw_face(v3 pos, float yaw, const Costume *cs)
{
    const u32 e = cs->eye;
    const float z = 0.62f;
    v3 eL = { -0.22f, 4.60f, z }, eR = { 0.22f, 4.60f, z };

    switch (cs->face) {
    case FACE_SMILE: {
        v3 m = { 0.00f, 4.26f, z }, mL = { -0.27f, 4.32f, z }, mR = { 0.27f, 4.32f, z };
        draw_decal(pos, yaw, eL, 0.08f, 0.08f, e);
        draw_decal(pos, yaw, eR, 0.08f, 0.08f, e);
        draw_decal(pos, yaw, m,  0.18f, 0.04f, e);
        draw_decal(pos, yaw, mL, 0.05f, 0.05f, e);
        draw_decal(pos, yaw, mR, 0.05f, 0.05f, e);
        break; }
    case FACE_ANGRY: {
        v3 bL = { -0.22f, 4.73f, z }, bR = { 0.22f, 4.73f, z };
        v3 eL2 = { -0.22f, 4.56f, z }, eR2 = { 0.22f, 4.56f, z };
        v3 m = { 0.00f, 4.24f, z };
        draw_decal(pos, yaw, bL,  0.15f, 0.03f, e);
        draw_decal(pos, yaw, bR,  0.15f, 0.03f, e);
        draw_decal(pos, yaw, eL2, 0.09f, 0.05f, e);
        draw_decal(pos, yaw, eR2, 0.09f, 0.05f, e);
        draw_decal(pos, yaw, m,   0.20f, 0.03f, e);
        break; }
    case FACE_VISOR: {
        v3 v = { 0.00f, 4.58f, z };
        draw_decal(pos, yaw, v, 0.46f, 0.09f, e);
        break; }
    case FACE_SLIT: {
        v3 v = { 0.00f, 4.58f, z };
        draw_decal(pos, yaw, v, 0.40f, 0.035f, e);
        break; }
    default: {
        v3 m = { 0.00f, 4.30f, z };
        draw_decal(pos, yaw, eL, 0.08f, 0.08f, e);
        draw_decal(pos, yaw, eR, 0.08f, 0.08f, e);
        draw_decal(pos, yaw, m,  0.25f, 0.04f, e);
        break; }
    }
}

/* hats are a few boxes on top of the head; accent = darker shade of the hat colour */
static void draw_hat(v3 pos, float yaw, int hat, const unsigned char *hc)
{
    if (hat == HAT_NONE) return;
    unsigned char ac[3] = { (unsigned char)(hc[0] * 6 / 10), (unsigned char)(hc[1] * 6 / 10), (unsigned char)(hc[2] * 6 / 10) };
    unsigned char lt[3] = { (unsigned char)((hc[0] + 255) / 2), (unsigned char)((hc[1] + 255) / 2), (unsigned char)((hc[2] + 255) / 2) };
    static const unsigned char band[3] = { 190, 40, 40 };
#define HP(x, y, z, hx, hy, hz, col) do { v3 o_ = { x, y, z }, h_ = { hx, hy, hz }; \
        draw_part(pos, yaw, o_, h_, 0, col, 0, PLAYER_CULL); } while (0)
    switch (hat) {
    case HAT_TOP:
        HP(0, 5.06f, 0, 0.95f, 0.06f, 0.95f, hc);          /* brim */
        HP(0, 5.66f, 0, 0.55f, 0.54f, 0.55f, hc);          /* tall crown */
        HP(0, 5.26f, 0, 0.58f, 0.10f, 0.58f, band);        /* band */
        break;
    case HAT_SKATER:
        HP(0, 5.22f, 0, 0.64f, 0.22f, 0.64f, hc);          /* dome */
        HP(0, 5.06f, 0.90f, 0.55f, 0.05f, 0.40f, ac);      /* flat brim */
        HP(0, 5.48f, 0, 0.10f, 0.04f, 0.10f, ac);          /* button */
        break;
    case HAT_BEANIE:
        HP(0, 5.18f, 0, 0.64f, 0.20f, 0.64f, hc);
        HP(0, 5.04f, 0, 0.68f, 0.10f, 0.68f, ac);          /* cuff */
        HP(0, 5.52f, 0, 0.16f, 0.14f, 0.16f, lt);          /* pom-pom */
        break;
    case HAT_CROWN:
        HP(0, 5.12f, 0, 0.62f, 0.12f, 0.62f, hc);
        HP(-0.46f, 5.36f, -0.46f, 0.10f, 0.14f, 0.10f, hc);
        HP( 0.46f, 5.36f, -0.46f, 0.10f, 0.14f, 0.10f, hc);
        HP(-0.46f, 5.36f,  0.46f, 0.10f, 0.14f, 0.10f, hc);
        HP( 0.46f, 5.36f,  0.46f, 0.10f, 0.14f, 0.10f, hc);
        break;
    case HAT_HEADBAND:
        HP(0, 4.72f, 0, 0.63f, 0.09f, 0.63f, hc);
        HP(0, 4.72f, -0.68f, 0.10f, 0.10f, 0.10f, hc);     /* knot */
        HP(-0.14f, 4.50f, -0.72f, 0.06f, 0.22f, 0.04f, hc);
        HP( 0.14f, 4.46f, -0.72f, 0.06f, 0.22f, 0.04f, hc);
        break;
    case HAT_ANTENNA:
        HP(0, 5.30f, 0, 0.05f, 0.30f, 0.05f, ac);
        HP(0, 5.70f, 0, 0.13f, 0.13f, 0.13f, hc);
        break;
    default: break;
    }
#undef HP
}

static void draw_chest(v3 pos, float yaw, int chest)
{
    const float z = 0.52f;
    v3 c;
    switch (chest) {
    case CHEST_ROBOT:
        c = (v3){ 0, 3.00f, z };         draw_decal(pos, yaw, c, 0.60f, 0.55f, 0x2A2F3C);
        c = (v3){ -0.30f, 3.30f, z + 0.01f }; draw_decal(pos, yaw, c, 0.10f, 0.08f, 0x40E8F8);
        c = (v3){ 0.00f, 3.30f, z + 0.01f };  draw_decal(pos, yaw, c, 0.10f, 0.08f, 0xF2D030);
        c = (v3){ 0.30f, 3.30f, z + 0.01f };  draw_decal(pos, yaw, c, 0.10f, 0.08f, 0xE04040);
        c = (v3){ 0.00f, 2.75f, z + 0.01f };  draw_decal(pos, yaw, c, 0.40f, 0.06f, 0x40E8F8);
        break;
    case CHEST_SUIT:
        c = (v3){ 0, 3.35f, z };         draw_decal(pos, yaw, c, 0.38f, 0.62f, 0xF0F0F4);   /* shirt */
        c = (v3){ 0, 3.85f, z + 0.01f }; draw_decal(pos, yaw, c, 0.20f, 0.09f, 0xBE2828);   /* bow tie */
        c = (v3){ 0, 3.35f, z + 0.01f }; draw_decal(pos, yaw, c, 0.05f, 0.05f, 0x202020);
        c = (v3){ 0, 2.95f, z + 0.01f }; draw_decal(pos, yaw, c, 0.05f, 0.05f, 0x202020);
        break;
    case CHEST_TEE:
        c = (v3){ 0, 3.15f, z };         draw_decal(pos, yaw, c, 1.00f, 0.20f, 0xC82828);   /* stripe */
        break;
    case CHEST_COLLAR:
        c = (v3){ 0, 3.75f, z };         draw_decal(pos, yaw, c, 1.00f, 0.22f, 0xF4F4F4);   /* ermine collar */
        c = (v3){ -0.60f, 3.75f, z + 0.01f }; draw_decal(pos, yaw, c, 0.06f, 0.06f, 0x202020);
        c = (v3){ 0.00f, 3.75f, z + 0.01f };  draw_decal(pos, yaw, c, 0.06f, 0.06f, 0x202020);
        c = (v3){ 0.60f, 3.75f, z + 0.01f };  draw_decal(pos, yaw, c, 0.06f, 0.06f, 0x202020);
        c = (v3){ 0, 3.20f, z + 0.01f }; draw_decal(pos, yaw, c, 0.07f, 0.07f, 0xF0C830);  /* gold button */
        break;
    default: break;
    }
}

/* limb angles are radians about the shoulder/hip: + = swing back, - = swing forward/up */
static void draw_noob(v3 pos, float yaw, float armL_a, float armR_a,
                      float legL_a, float legR_a, const Costume *cs)
{
    v3 head  = { 0, 4.5f, 0 },     headH = { 0.6f, 0.5f, 0.6f };
    v3 torso = { 0, 3.0f, 0 },     torsoH = { 1.0f, 1.0f, 0.5f };
    v3 armL  = { -1.5f, 3.0f, 0 }, armR = { 1.5f, 3.0f, 0 }, limbH = { 0.5f, 1.0f, 0.5f };
    v3 legL  = { -0.5f, 1.0f, 0 }, legR = { 0.5f, 1.0f, 0 };

    /* player models: culling disabled (PLAYER_CULL = 0) -> every face is drawn */
    draw_part(pos, yaw, legL,  limbH,  legL_a, cs->legs,  F_PY, PLAYER_CULL);
    draw_part(pos, yaw, legR,  limbH,  legR_a, cs->legs,  F_PY, PLAYER_CULL);
    draw_part(pos, yaw, torso, torsoH, 0,      cs->torso, F_NY, PLAYER_CULL);
    draw_part(pos, yaw, armL,  limbH,  armL_a, cs->skin,  0,    PLAYER_CULL);
    draw_part(pos, yaw, armR,  limbH,  armR_a, cs->skin,  0,    PLAYER_CULL);
    draw_part(pos, yaw, head,  headH,  0,      cs->skin,  F_NY, PLAYER_CULL);
    draw_face(pos, yaw, cs);
    draw_chest(pos, yaw, cs->chest);
    draw_hat(pos, yaw, cs->hat, cs->hat_col);
}

/* ---------- castle map ---------- */

typedef struct { float x, y, z, hx, hy, hz; unsigned char c; } Block;   /* centre + half extents */
#define MAX_BLOCKS 256
static Block blocks[MAX_BLOCKS];
static int n_blocks = 0;

enum { BC_STONE, BC_STONE_D, BC_WOOD, BC_RED, NUM_BC };
static const unsigned char BLOCK_COL[NUM_BC][3] = {
    { 178, 178, 184 }, { 132, 132, 142 }, { 120, 82, 44 }, { 196, 44, 44 }
};

static void add_block(float x, float y, float z, float hx, float hy, float hz, int c)
{
    if (n_blocks < MAX_BLOCKS) {
        Block b = { x, y, z, hx, hy, hz, (unsigned char)c };
        blocks[n_blocks++] = b;
    }
}

static void build_castle(void)
{
    n_blocks = 0;

    /* curtain wall: 14 high, 4 thick, 12-wide gate gap on the south (+z) side */
    add_block(0, 7, -38, 40, 7, 2, BC_STONE);
    add_block(38, 7, 0, 2, 7, 36, BC_STONE);
    add_block(-38, 7, 0, 2, 7, 36, BC_STONE);
    add_block(-23, 7, 38, 17, 7, 2, BC_STONE);
    add_block(23, 7, 38, 17, 7, 2, BC_STONE);
    add_block(0, 11, 38, 6, 3, 2, BC_STONE);              /* lintel over the gate */

    /* corner towers with merlons and flags */
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sz = -1; sz <= 1; sz += 2) {
            float tx = sx * 40.0f, tz = sz * 40.0f;
            add_block(tx, 10, tz, 6, 10, 6, BC_STONE_D);
            for (int mx = -1; mx <= 1; mx += 2)
                for (int mz = -1; mz <= 1; mz += 2)
                    add_block(tx + mx * 4.5f, 21, tz + mz * 4.5f, 1.5f, 1, 1.5f, BC_STONE_D);
            add_block(tx, 24, tz, 0.25f, 4, 0.25f, BC_WOOD);
            add_block(tx + 1.6f, 26.5f, tz, 1.5f, 1, 0.12f, BC_RED);
        }

    /* battlements along the outer edge of the wall walk */
    for (int i = -6; i <= 6; i++) add_block(i * 5.0f, 15, -39.2f, 1.4f, 1, 0.8f, BC_STONE);
    for (int i = -6; i <= 6; i++) if (i < -2 || i > 2) add_block(i * 5.0f, 15, 39.2f, 1.4f, 1, 0.8f, BC_STONE);
    for (int i = -6; i <= 6; i++) {
        add_block(39.2f, 15, i * 5.0f, 0.8f, 1, 1.4f, BC_STONE);
        add_block(-39.2f, 15, i * 5.0f, 0.8f, 1, 1.4f, BC_STONE);
    }

    /* gatehouse towers + wooden drawbridge */
    add_block(-9, 9, 41, 3, 9, 3, BC_STONE_D);
    add_block(9, 9, 41, 3, 9, 3, BC_STONE_D);
    add_block(0, 0.35f, 46, 5.5f, 0.35f, 8, BC_WOOD);

    /* keep: red roof, four turrets, flag */
    add_block(0, 12, -12, 12, 12, 10, BC_STONE);
    add_block(0, 24.6f, -12, 12.6f, 0.6f, 10.6f, BC_RED);
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sz = -1; sz <= 1; sz += 2)
            add_block(sx * 12.0f, 15, -12 + sz * 10.0f, 3.5f, 15, 3.5f, BC_STONE_D);
    add_block(0, 31, -12, 0.3f, 6, 0.3f, BC_WOOD);
    add_block(2.3f, 34.5f, -12, 2, 1.5f, 0.15f, BC_RED);

    /* courtyard props: well and stacked crates you can jump onto */
    add_block(18, 1.2f, 18, 2.5f, 1.2f, 2.5f, BC_STONE_D);
    add_block(-20, 1, 15, 1.5f, 1, 1.5f, BC_WOOD);
    add_block(-17.2f, 1, 16.5f, 1, 1, 1, BC_WOOD);
    add_block(-20, 3, 15, 1, 1, 1, BC_WOOD);

    /* stairs up to the wall walk on both side walls (1.4 per step) */
    for (int i = 0; i < 10; i++) {
        float h = 1.4f * (i + 1), z = 22.0f - i * 2.6f;
        add_block(33.5f, h * 0.5f, z, 2.5f, h * 0.5f, 1.3f, BC_STONE_D);
        add_block(-33.5f, h * 0.5f, z, 2.5f, h * 0.5f, 1.3f, BC_STONE_D);
    }
}

/* Culling: distance (tiny details only nearby), behind-camera, off-screen
 * bounding circle. Then draw_part drops back faces and clips to the near plane. */
static void draw_blocks(void)
{
    for (int i = 0; i < n_blocks; i++) {
        const Block *b = &blocks[i];
        v3 d = { b->x - cpos.x, b->y - cpos.y, b->z - cpos.z };
        float d2 = d.x * d.x + d.y * d.y + d.z * d.z;
        float rad2 = b->hx * b->hx + b->hy * b->hy + b->hz * b->hz;
        if (d2 > 150.0f * 150.0f) continue;
        if (rad2 < 9.0f && d2 > 80.0f * 80.0f) continue;
        v3 c = { dot(d, cr), dot(d, cu), dot(d, cf) };
        float rad = sqrtf(rad2);
        if (c.z + rad < NEAR) continue;
        if (c.z > rad) {
            float sc = FOCAL / c.z;
            float sx = IW * 0.5f + c.x * sc, sy = IH * 0.5f - c.y * sc, sr = rad * sc;
            if (sx + sr < 0 || sx - sr > IW || sy + sr < 0 || sy - sr > IH) continue;
        }
        v3 o = { b->x, b->y, b->z }, off = { 0, 0, 0 }, h = { b->hx, b->hy, b->hz };
        draw_part(o, 0.0f, off, h, 0.0f, BLOCK_COL[b->c], 0, 1);
    }
}

#define PLAYER_R 1.1f
#define PLAYER_H 5.4f
#define STEP_H   1.7f

/* height of the highest surface under (x,z) that feet at height y can stand on.
 * Off the edge of the map there is no ground at all (you fall and respawn). */
static float ground_at(int map, float x, float y, float z)
{
    if (fabsf(x) > FLOOR_EXT || fabsf(z) > FLOOR_EXT) return -1e9f;
    float g = 0.0f;
    if (map == MAP_CASTLE)
        for (int i = 0; i < n_blocks; i++) {
            const Block *b = &blocks[i];
            float top = b->y + b->hy;
            if (top > y + STEP_H || top <= g) continue;
            if (fabsf(x - b->x) <= b->hx + 0.35f && fabsf(z - b->z) <= b->hz + 0.35f) g = top;
        }
    return g;
}

/* push the player out of any block too tall to step onto */
static void collide(v3 *p)
{
    for (int i = 0; i < n_blocks; i++) {
        const Block *b = &blocks[i];
        float top = b->y + b->hy, bot = b->y - b->hy;
        if (p->y + STEP_H >= top || p->y + PLAYER_H <= bot) continue;
        float dx = p->x - b->x, dz = p->z - b->z;
        float ox = b->hx + PLAYER_R - fabsf(dx), oz = b->hz + PLAYER_R - fabsf(dz);
        if (ox <= 0.0f || oz <= 0.0f) continue;
        if (ox < oz) p->x += (dx < 0.0f) ? -ox : ox;
        else         p->z += (dz < 0.0f) ? -oz : oz;
    }
}

static void present(void)
{
    if (first_flip) { gcmResetFlipStatus(); first_flip = 0; }
    else wait_flip();

    u32 *dst = fb[cur];
    u32 words = dpitch / 4;
    int prev = -1;
    for (u32 y = 0; y < dh; y++) {
        int sy = scale ? (int)(y / (u32)scale) : (int)(y * IH / dh);
        if (sy != prev) {
            const u32 *src = &pix[sy * IW];
            if (scale == 2) {
                u64 *d = (u64 *)linebuf;
                for (int x = 0; x < IW; x++) { u32 v = src[x]; d[x] = ((u64)v << 32) | v; }
            } else if (scale == 3) {
                for (int x = 0; x < IW; x++) {
                    u32 v = src[x];
                    linebuf[3 * x] = v; linebuf[3 * x + 1] = v; linebuf[3 * x + 2] = v;
                }
            } else {
                for (u32 x = 0; x < dw; x++) linebuf[x] = src[xmap[x]];
            }
            prev = sy;
        }
        memcpy(dst + y * words, linebuf, dw * sizeof(u32));
    }

    gcmSetFlip(ctx, cur);
    rsxFlushBuffer(ctx);
    gcmSetWaitFlip(ctx);
    cur ^= 1;
}

/* ---------- timing ---------- */

/* seconds since an arbitrary start. PS3 timebase = 79.8 MHz. */
static double now_sec(void)
{
#if defined(__PPU__) || defined(__powerpc__) || defined(__powerpc64__)
    u64 tb;
    __asm__ volatile("mftb %0" : "=r"(tb));
    return (double)tb / 79800000.0;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec * 1e-9;
#endif
}

/* ---------- sound: small synth (steps, jump, landing) over libaudio ---------- */

enum { SFX_STEP, SFX_JUMP, SFX_LAND };
typedef struct { int active, type, t; float vol, pan, k, lp, ph; } Voice;
#define MAX_VOICES 8
static Voice voices[MAX_VOICES];
static unsigned rng_state = 2463534242u;

static unsigned rnd_u(void) { rng_state = rng_state * 1664525u + 1013904223u; return rng_state >> 8; }
static float frand(void) { return (float)(rnd_u() & 0xFFFF) / 32768.0f - 1.0f; }
static int rnd(int n) { return (int)(rnd_u() % (unsigned)n); }

/* k = noise brightness for steps (0.1 dull .. 0.6 bright); pan -1..1 */
static void sfx(int type, float vol, float k, float pan)
{
    int slot = 0;
    for (int i = 0; i < MAX_VOICES; i++) if (!voices[i].active) { slot = i; break; }
    Voice v = { 1, type, 0, vol, pan, k, 0.0f, 0.0f };
    voices[slot] = v;
}

static void mix_block(float *out)           /* 256 stereo frames at 48 kHz */
{
    const float SR = 48000.0f, TWO_PI = 6.2831853f;
    for (int i = 0; i < AUDIO_BLOCK_SAMPLES; i++) {
        float L = 0.0f, R = 0.0f;
        for (int n = 0; n < MAX_VOICES; n++) {
            Voice *v = &voices[n];
            if (!v->active) continue;
            float t = (float)v->t / SR, s = 0.0f;
            if (v->type == SFX_STEP) {
                v->lp += (frand() - v->lp) * v->k;
                s = (v->lp * 1.4f * expf(-t * 60.0f) + sinf(TWO_PI * 80.0f * t) * expf(-t * 45.0f) * 0.5f) * 0.55f;
                if (t > 0.14f) v->active = 0;
            } else if (v->type == SFX_JUMP) {
                float f = 300.0f + 520.0f * (t / 0.16f);
                v->ph += TWO_PI * f / SR;
                float env = (1.0f - t / 0.16f) * (t < 0.01f ? t / 0.01f : 1.0f);
                s = (sinf(v->ph) + 0.35f * sinf(v->ph * 2.0f)) * env * 0.30f;
                if (t > 0.16f) v->active = 0;
            } else {                                         /* SFX_LAND */
                float f = 50.0f + 110.0f * expf(-t * 14.0f);
                v->ph += TWO_PI * f / SR;
                v->lp += (frand() - v->lp) * 0.25f;
                s = sinf(v->ph) * expf(-t * 20.0f) * 0.55f + v->lp * expf(-t * 55.0f) * 0.35f;
                if (t > 0.2f) v->active = 0;
            }
            s *= v->vol;
            L += s * (v->pan > 0.0f ? 1.0f - v->pan : 1.0f);
            R += s * (v->pan < 0.0f ? 1.0f + v->pan : 1.0f);
            v->t++;
        }
        if (L > 1.0f) L = 1.0f; else if (L < -1.0f) L = -1.0f;
        if (R > 1.0f) R = 1.0f; else if (R < -1.0f) R = -1.0f;
        out[2 * i] = L * 0.8f;
        out[2 * i + 1] = R * 0.8f;
    }
}

static u32 aud_port;
static sys_event_queue_t aud_q;
static sys_ipc_key_t aud_key;
static float *aud_buf;
static int aud_blocks, aud_next, aud_ok;
#ifdef BLOX_AUDIO_DUMP
static float aud_dump[48000 * 2 * 12];
static int aud_dump_n;
#endif

static void audio_init(void)
{
    audioPortParam p;
    audioPortConfig cfg;
    memset(&p, 0, sizeof(p));
    p.numChannels = AUDIO_PORT_2CH;
    p.numBlocks = AUDIO_BLOCK_16;
    p.attrib = 0;
    p.level = 1.0f;
    if (audioInit() != 0) return;
    if (audioPortOpen(&p, &aud_port) != 0) return;
    if (audioGetPortConfig(aud_port, &cfg) != 0) return;
    aud_buf = (float *)(uintptr_t)cfg.audioDataStart;
    aud_blocks = (int)cfg.numBlocks;
    memset(aud_buf, 0, (size_t)aud_blocks * 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
    if (audioCreateNotifyEventQueue(&aud_q, &aud_key) != 0) return;
    if (audioSetNotifyEventQueue(aud_key) != 0) return;
    sysEventQueueDrain(aud_q);
    if (audioPortStart(aud_port) != 0) return;
    aud_ok = 1;
}

/* called once per frame: every queue event means one block can be refilled */
static void audio_pump(void)
{
    sys_event_t ev;
    int n = 0;
    if (!aud_ok) return;
    while (n < aud_blocks && sysEventQueueReceive(aud_q, &ev, 1) == 0) {
        float *blk = aud_buf + (size_t)aud_next * 2 * AUDIO_BLOCK_SAMPLES;
        mix_block(blk);
#ifdef BLOX_AUDIO_DUMP
        if (aud_dump_n + 2 * AUDIO_BLOCK_SAMPLES <= (int)(sizeof(aud_dump) / sizeof(float))) {
            memcpy(&aud_dump[aud_dump_n], blk, 2 * AUDIO_BLOCK_SAMPLES * sizeof(float));
            aud_dump_n += 2 * AUDIO_BLOCK_SAMPLES;
        }
#endif
        aud_next = (aud_next + 1) % aud_blocks;
        n++;
    }
}

static void audio_shutdown(void)
{
    if (!aud_ok) return;
    audioPortStop(aud_port);
    audioRemoveNotifyEventQueue(aud_key);
    audioPortClose(aud_port);
    sysEventQueueDestroy(aud_q, 0);
    audioQuit();
    aud_ok = 0;
}

/* Sleep until the next frame slot. With vsync (cap < 60) frames still snap to
 * refresh boundaries, but the average rate follows the cap. */
static void limit_frame(void)
{
    if (g_cap <= 0 || g_cap == 60) return;      /* uncapped, or plain vsync */
    double t = now_sec();
    if (next_frame < t - 0.25 || next_frame > t + 1.0) next_frame = t;   /* resync */
    while ((t = now_sec()) < next_frame)
        usleep((next_frame - t) > 0.002 ? 1000 : 100);
    next_frame += 1.0 / g_cap;
}

/* ---------- tiny 5x7 bitmap font (A-Z, 0-9, a few symbols) ---------- */

static const unsigned char *glyph(char c)
{
    static const unsigned char DIG[10][7] = {
        {0x0E,0x11,0x13,0x15,0x19,0x11,0x0E}, {0x04,0x0C,0x04,0x04,0x04,0x04,0x0E},
        {0x0E,0x11,0x01,0x02,0x04,0x08,0x1F}, {0x1F,0x02,0x04,0x02,0x01,0x11,0x0E},
        {0x02,0x06,0x0A,0x12,0x1F,0x02,0x02}, {0x1F,0x10,0x1E,0x01,0x01,0x11,0x0E},
        {0x06,0x08,0x10,0x1E,0x11,0x11,0x0E}, {0x1F,0x01,0x02,0x04,0x08,0x08,0x08},
        {0x0E,0x11,0x11,0x0E,0x11,0x11,0x0E}, {0x0E,0x11,0x11,0x0F,0x01,0x02,0x0C}
    };
    static const unsigned char ALP[26][7] = {
        {0x0E,0x11,0x11,0x1F,0x11,0x11,0x11}, {0x1E,0x11,0x11,0x1E,0x11,0x11,0x1E},
        {0x0E,0x11,0x10,0x10,0x10,0x11,0x0E}, {0x1E,0x11,0x11,0x11,0x11,0x11,0x1E},
        {0x1F,0x10,0x10,0x1E,0x10,0x10,0x1F}, {0x1F,0x10,0x10,0x1E,0x10,0x10,0x10},
        {0x0E,0x11,0x10,0x17,0x11,0x11,0x0F}, {0x11,0x11,0x11,0x1F,0x11,0x11,0x11},
        {0x0E,0x04,0x04,0x04,0x04,0x04,0x0E}, {0x07,0x02,0x02,0x02,0x02,0x12,0x0C},
        {0x11,0x12,0x14,0x18,0x14,0x12,0x11}, {0x10,0x10,0x10,0x10,0x10,0x10,0x1F},
        {0x11,0x1B,0x15,0x15,0x11,0x11,0x11}, {0x11,0x11,0x19,0x15,0x13,0x11,0x11},
        {0x0E,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x1E,0x11,0x11,0x1E,0x10,0x10,0x10},
        {0x0E,0x11,0x11,0x11,0x15,0x12,0x0D}, {0x1E,0x11,0x11,0x1E,0x14,0x12,0x11},
        {0x0F,0x10,0x10,0x0E,0x01,0x01,0x1E}, {0x1F,0x04,0x04,0x04,0x04,0x04,0x04},
        {0x11,0x11,0x11,0x11,0x11,0x11,0x0E}, {0x11,0x11,0x11,0x11,0x11,0x0A,0x04},
        {0x11,0x11,0x11,0x15,0x15,0x15,0x0A}, {0x11,0x11,0x0A,0x04,0x0A,0x11,0x11},
        {0x11,0x11,0x0A,0x04,0x04,0x04,0x04}, {0x1F,0x01,0x02,0x04,0x08,0x10,0x1F}
    };
    static const unsigned char COLON[7] = {0x00,0x04,0x00,0x00,0x00,0x04,0x00};
    static const unsigned char DASH[7]  = {0x00,0x00,0x00,0x1F,0x00,0x00,0x00};
    static const unsigned char GT[7]    = {0x08,0x04,0x02,0x01,0x02,0x04,0x08};
    static const unsigned char SLASH[7] = {0x01,0x01,0x02,0x04,0x08,0x10,0x10};
    static const unsigned char DOT[7]   = {0x00,0x00,0x00,0x00,0x00,0x0C,0x0C};
    static const unsigned char BLANK[7] = {0,0,0,0,0,0,0};

    if (c >= 'a' && c <= 'z') c = (char)(c - 'a' + 'A');
    if (c >= '0' && c <= '9') return DIG[c - '0'];
    if (c >= 'A' && c <= 'Z') return ALP[c - 'A'];
    switch (c) {
    case ':': return COLON;
    case '-': return DASH;
    case '>': return GT;
    case '/': return SLASH;
    case '.': return DOT;
    default:  return BLANK;
    }
}

static void fill_rect(int x, int y, int w, int h, u32 col)
{
    for (int yy = y; yy < y + h; yy++) {
        if (yy < 0 || yy >= IH) continue;
        for (int xx = x; xx < x + w; xx++) {
            if (xx < 0 || xx >= IW) continue;
            pix[yy * IW + xx] = col;
        }
    }
}

#define NO_SHADOW 0xFF000000u

static void draw_text_ex(int x, int y, const char *str, int sc, u32 col, u32 shadow)
{
    for (int pass = (shadow == NO_SHADOW) ? 1 : 0; pass < 2; pass++) {   /* pass 0 = shadow */
        int ox = pass == 0 ? sc : 0, oy = pass == 0 ? sc : 0;
        u32 c = pass == 0 ? shadow : col;
        int cx = x;
        for (const char *p = str; *p; p++, cx += 6 * sc) {
            const unsigned char *g = glyph(*p);
            for (int r = 0; r < 7; r++)
                for (int b = 0; b < 5; b++)
                    if (g[r] & (0x10 >> b))
                        fill_rect(cx + b * sc + ox, y + r * sc + oy, sc, sc, c);
        }
    }
}

static void draw_text(int x, int y, const char *str, int sc, u32 col)
{
    draw_text_ex(x, y, str, sc, col, 0x000000);
}

static int text_w(const char *s, int sc)
{
    int n = (int)strlen(s);
    return n ? n * 6 * sc - sc : 0;
}

static void draw_text_c(int cx, int y, const char *s, int sc, u32 col)
{
    draw_text(cx - text_w(s, sc) / 2, y, s, sc, col);
}

static void draw_text_cx(int cx, int y, const char *s, int sc, u32 col, u32 shadow)
{
    draw_text_ex(cx - text_w(s, sc) / 2, y, s, sc, col, shadow);
}

/* darken a rectangle of the screen: shift 1 = half brightness, 2 = quarter */
static void dim_rect(int x0, int y0, int x1, int y1, int shift)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > IW - 1) x1 = IW - 1;
    if (y1 > IH - 1) y1 = IH - 1;
    const u32 mask = shift == 1 ? 0x7F7F7Fu : 0x3F3F3Fu;
    for (int y = y0; y <= y1; y++) {
        u32 *row = &pix[y * IW];
        for (int x = x0; x <= x1; x++) row[x] = (row[x] >> shift) & mask;
    }
}

/* blend a colour over a rectangle; a = 0..255 */
static void blend_rect(int x0, int y0, int x1, int y1, u32 col, int a)
{
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > IW - 1) x1 = IW - 1;
    if (y1 > IH - 1) y1 = IH - 1;
    unsigned cr_ = (col >> 16) & 255, cg_ = (col >> 8) & 255, cb_ = col & 255;
    for (int y = y0; y <= y1; y++) {
        u32 *row = &pix[y * IW];
        for (int x = x0; x <= x1; x++) {
            u32 p = row[x];
            unsigned r = (((p >> 16) & 255) * (256 - a) + cr_ * a) >> 8;
            unsigned g = (((p >> 8) & 255) * (256 - a) + cg_ * a) >> 8;
            unsigned b = ((p & 255) * (256 - a) + cb_ * a) >> 8;
            row[x] = (r << 16) | (g << 8) | b;
        }
    }
}

/* rounded rectangle (pill when r = h/2) */
static void fill_round(int x, int y, int w, int h, int r, u32 col)
{
    for (int yy = 0; yy < h; yy++) {
        int inset = 0;
        int d = (yy < r) ? r - yy : (yy >= h - r) ? yy - (h - r) + 1 : 0;
        if (d > 0) {
            float dy = d - 0.5f;
            float v = (float)(r * r) - dy * dy;
            inset = r - (int)(v > 0 ? sqrtf(v) : 0.0f);
        }
        fill_rect(x + inset, y + yy, w - 2 * inset, 1, col);
    }
}

#define COL_HILITE 0xFFD030u
#define PANEL_TXT  0x14502Au
#define GREEN      0x2FB54Au
#define GREEN_LT   0xBFE8C6u
#define GREEN_DK   0x1E6B2Eu

/* ---------- avatar parts ---------- */

typedef struct { const char *name; unsigned char c[3]; } PalColor;
#define NUM_PAL 16
static const PalColor PALETTE[NUM_PAL] = {
    { "YELLOW", { 245, 205,  48 } }, { "PEACH",  { 250, 214, 176 } }, { "TAN",    { 220, 170, 120 } },
    { "BROWN",  { 140,  94,  66 } }, { "WHITE",  { 240, 240, 244 } }, { "GRAY",   { 150, 152, 160 } },
    { "BLACK",  {  34,  34,  40 } }, { "RED",    { 200,  50,  50 } }, { "ORANGE", { 235, 130,  40 } },
    { "GOLD",   { 235, 190,  50 } }, { "GREEN",  {  70, 160,  80 } }, { "TEAL",   {  40, 160, 160 } },
    { "BLUE",   {  50, 100, 200 } }, { "NAVY",   {  30,  50, 110 } }, { "PURPLE", { 140,  90, 200 } },
    { "PINK",   { 240, 120, 170 } },
};
static const char *FACE_NAMES[5] = { "STRAIGHT", "SMILE", "ANGRY", "VISOR", "SLIT" };

typedef struct { int hat, hat_col, skin_col, shirt_col, pants_col, face; } Custom;

static void custom_default(Custom *c)
{
    c->hat = HAT_NONE; c->hat_col = 6; c->skin_col = 0; c->shirt_col = 12; c->pants_col = 10; c->face = FACE_STRAIGHT;
}

/* ---------- settings ---------- */

typedef struct {
    int skin, map, tooltips, show_fps, fps_menu, cap_idx, custom_cap, sound;
    Custom custom;
} Settings;

static const int CAP_VALUES[5] = { 15, 30, 45, 60, 0 };       /* idx 4 = uncapped, idx 5 = custom */
static const char *SETTING_LABELS[6] = { "CONTROL TIPS", "FPS COUNTER", "FPS IN MENUS", "SOUND", "FPS CAP", "CUSTOM FPS" };

static void settings_default(Settings *s)
{
    s->skin = 0; s->map = MAP_BASE; s->tooltips = 1; s->show_fps = 1; s->fps_menu = 0; s->sound = 1;
    s->cap_idx = BLOX_DEFAULT_CAP_IDX; s->custom_cap = 60;
    custom_default(&s->custom);
}

static int effective_cap(const Settings *s)
{
    return s->cap_idx < 5 ? CAP_VALUES[s->cap_idx] : s->custom_cap;
}

static int settings_rows(const Settings *s) { return s->cap_idx == 5 ? 6 : 5; }

static void settings_adjust(Settings *s, int row, int dir)
{
    switch (row) {
    case 0: s->tooltips ^= 1; break;
    case 1: s->show_fps ^= 1; break;
    case 2: s->fps_menu ^= 1; break;
    case 3: s->sound ^= 1; break;
    case 4: s->cap_idx = (s->cap_idx + 6 + dir) % 6; apply_cap(effective_cap(s)); break;
    case 5:
        if (s->cap_idx == 5) {
            s->custom_cap += dir * 5;
            if (s->custom_cap < 5) s->custom_cap = 5;
            if (s->custom_cap > 240) s->custom_cap = 240;
            apply_cap(effective_cap(s));
        }
        break;
    default: break;
    }
}

static void settings_value(const Settings *s, int row, char *out, size_t n)
{
    switch (row) {
    case 0: snprintf(out, n, "%s", s->tooltips ? "ON" : "OFF"); break;
    case 1: snprintf(out, n, "%s", s->show_fps ? "ON" : "OFF"); break;
    case 2: snprintf(out, n, "%s", s->fps_menu ? "ON" : "OFF"); break;
    case 3: snprintf(out, n, "%s", s->sound ? "ON" : "OFF"); break;
    case 4:
        if (s->cap_idx < 4) snprintf(out, n, "%d", CAP_VALUES[s->cap_idx]);
        else snprintf(out, n, "%s", s->cap_idx == 4 ? "UNCAPPED" : "CUSTOM");
        break;
    default: snprintf(out, n, "%d", s->custom_cap); break;
    }
}

static void draw_settings_rows(const Settings *s, int x, int y, int w, int rowh, int sel, int active,
                               u32 txt, u32 hil_bg, u32 hil_txt, u32 shadow)
{
    int n = settings_rows(s);
    for (int i = 0; i < n; i++) {
        int ry = y + i * rowh, hot = active && i == sel;
        if (hot) fill_round(x - 10, ry, w + 20, rowh - 4, 8, hil_bg);
        u32 col = hot ? hil_txt : txt;
        int ty = ry + (rowh - 4 - 14) / 2;
        char v[16];
        settings_value(s, i, v, sizeof(v));
        draw_text_ex(x, ty, SETTING_LABELS[i], 2, col, hot ? NO_SHADOW : shadow);
        draw_text_ex(x + w - text_w(v, 2), ty, v, 2, col, hot ? NO_SHADOW : shadow);
    }
}

/* ---------- avatar creator ---------- */

static Costume g_custom_cs;

/* preset skins, or (skin >= NUM_PRESETS) the player-made avatar */
static const Costume *costume_for(int skin, const Custom *c)
{
    if (skin >= 0 && skin < NUM_PRESETS) return &COSTUMES[skin];
    Costume *o = &g_custom_cs;
    o->name = "CUSTOM";
    for (int k = 0; k < 3; k++) {
        o->skin[k] = PALETTE[c->skin_col].c[k];
        o->torso[k] = PALETTE[c->shirt_col].c[k];
        o->legs[k] = PALETTE[c->pants_col].c[k];
        o->hat_col[k] = PALETTE[c->hat_col].c[k];
    }
    o->face = c->face;
    o->eye = c->face == FACE_VISOR ? 0x40E8F8u : c->face == FACE_SLIT ? 0xF2F2F2u :
             c->face == FACE_ANGRY ? 0x6A1010u : 0x141414u;
    o->hat = c->hat;
    o->chest = CHEST_NONE;
    return o;
}

#define CR_ROWS 8
static const char *CR_LABELS[CR_ROWS] = { "HAT", "HAT COLOR", "SKIN", "SHIRT", "PANTS", "FACE", "RANDOMIZE", "RESET" };

/* rows 0-5 cycle a value (dir = +1/-1); 6 = randomize, 7 = reset. Any edit selects the custom avatar. */
static void creator_adjust(Settings *s, int row, int dir)
{
    Custom *c = &s->custom;
    switch (row) {
    case 0: c->hat = (c->hat + NUM_HATS + dir) % NUM_HATS; break;
    case 1: c->hat_col = (c->hat_col + NUM_PAL + dir) % NUM_PAL; break;
    case 2: c->skin_col = (c->skin_col + NUM_PAL + dir) % NUM_PAL; break;
    case 3: c->shirt_col = (c->shirt_col + NUM_PAL + dir) % NUM_PAL; break;
    case 4: c->pants_col = (c->pants_col + NUM_PAL + dir) % NUM_PAL; break;
    case 5: c->face = (c->face + 5 + dir) % 5; break;
    case 6:
        c->hat = rnd(NUM_HATS); c->hat_col = rnd(NUM_PAL); c->skin_col = rnd(NUM_PAL);
        c->shirt_col = rnd(NUM_PAL); c->pants_col = rnd(NUM_PAL); c->face = rnd(5);
        break;
    default: custom_default(c); break;
    }
    s->skin = CUSTOM_SKIN;
}

static void draw_creator_rows(const Settings *s, int x, int y, int w, int rowh, int sel, int active,
                              u32 txt, u32 hil_bg, u32 hil_txt, u32 shadow)
{
    const Custom *c = &s->custom;
    for (int i = 0; i < CR_ROWS; i++) {
        int ry = y + i * rowh, hot = active && i == sel;
        if (hot) fill_round(x - 10, ry, w + 20, rowh - 4, 8, hil_bg);
        u32 col = hot ? hil_txt : txt, sh = hot ? NO_SHADOW : shadow;
        int ty = ry + (rowh - 4 - 14) / 2;
        draw_text_ex(x, ty, CR_LABELS[i], 2, col, sh);
        int pal = (i == 1) ? c->hat_col : (i == 2) ? c->skin_col : (i == 3) ? c->shirt_col : (i == 4) ? c->pants_col : -1;
        const char *val = (i == 0) ? HAT_NAMES[c->hat] : (i == 5) ? FACE_NAMES[c->face] : NULL;
        if (pal >= 0) {
            val = PALETTE[pal].name;
            int tw = text_w(val, 2);
            const unsigned char *p = PALETTE[pal].c;
            fill_rect(x + w - tw - 25, ty - 1, 18, 16, 0x202020);
            fill_rect(x + w - tw - 24, ty, 16, 14, (u32)(p[0] << 16 | p[1] << 8 | p[2]));
        }
        if (val) draw_text_ex(x + w - text_w(val, 2), ty, val, 2, col, sh);
    }
}

static void swatches(int x, int y, int sz, const Costume *c)
{
    fill_rect(x, y, sz, sz, (u32)(c->skin[0] << 16 | c->skin[1] << 8 | c->skin[2]));
    fill_rect(x + sz + 4, y, sz, sz, (u32)(c->torso[0] << 16 | c->torso[1] << 8 | c->torso[2]));
    fill_rect(x + 2 * (sz + 4), y, sz, sz, (u32)(c->legs[0] << 16 | c->legs[1] << 8 | c->legs[2]));
}

static int list_first(int sel, int n, int vis)
{
    int f = sel - vis / 2;
    if (f > n - vis) f = n - vis;
    return f < 0 ? 0 : f;
}

/* ---------- Xbox-dashboard-style main menu ---------- */

static u32 bg[IW * IH] __attribute__((aligned(16)));

/* baked once at startup: pale-green gradient, soft flowing bands, a glow */
static void build_menu_bg(void)
{
    for (int y = 0; y < IH; y++) {
        float ty = (float)y / IH;
        for (int x = 0; x < IW; x++) {
            float b1 = sinf(x * 0.011f + y * 0.017f), b2 = sinf(x * 0.019f - y * 0.009f + 2.0f);
            float m = 1.0f + 0.055f * b1 + 0.045f * b2;
            float gx = x - 520.0f, gy = y - 40.0f;
            float gl = 1.0f - sqrtf(gx * gx + gy * gy) / 300.0f;
            gl = gl > 0.0f ? gl * gl * 0.40f : 0.0f;
            float r = (205.0f - 135.0f * ty) * m, g = (240.0f - 85.0f * ty) * m, b = (210.0f - 120.0f * ty) * m;
            r += (255.0f - r) * gl; g += (255.0f - g) * gl; b += (255.0f - b) * gl;
            if (r > 255) r = 255;
            if (g > 255) g = 255;
            if (b > 255) b = 255;
            bg[y * IW + x] = ((u32)r << 16) | ((u32)g << 8) | (u32)b;
        }
    }
}

#define NTABS 6
static const char *TAB_NAMES[NTABS] = { "PLAY", "MAPS", "SKINS", "AVATAR", "SETTINGS", "QUIT" };
static const char *MAP_NAMES[NUM_MAPS] = { "BASEPLATE", "CASTLE" };
static const char *MAP_DESC[NUM_MAPS] = { "OPEN FIELD", "WALLS TOWERS STAIRS" };

static void draw_menu_noob(const Costume *cs, float spin)
{
    set_camera((v3){ -5.5f, 4.2f, 9.5f }, (v3){ -5.5f, 2.6f, 0.0f });   /* noob lands on the right */
    v3 pos = { 0, 0, 0 };
    if (setup_noob_region(pos, spin))
        draw_noob(pos, spin, 0, 0, 0, 0, cs);
}

static void draw_thumb(int map, int x, int y, int w, int h)
{
    int hz = y + h * 45 / 100;
    fill_rect(x, y, w, hz - y, SKY_COL);
    for (int yy = hz; yy < y + h; yy += 8)
        for (int xx = x; xx < x + w; xx += 8) {
            int cw = (xx + 8 > x + w) ? x + w - xx : 8, ch = (yy + 8 > y + h) ? y + h - yy : 8;
            fill_rect(xx, yy, cw, ch, (((xx - x) / 8 + (yy - hz) / 8) & 1) ? 0x4B974Bu : 0x428742u);
        }
    if (map == MAP_CASTLE) {
        int cx = x + w / 2;
        fill_rect(cx - 20, hz - 52, 40, 52, 0xB2B2B8);               /* keep */
        fill_rect(cx - 22, hz - 56, 44, 5, 0xC4282C);                /* roof */
        fill_rect(cx, hz - 76, 2, 20, 0x78522C);                     /* flag pole */
        fill_rect(cx + 2, hz - 76, 12, 7, 0xC4282C);
        fill_rect(x + 18, hz - 26, w - 36, 26, 0x848492);            /* curtain wall */
        for (int i = 0; i < (w - 36) / 12; i++) fill_rect(x + 20 + i * 12, hz - 32, 7, 6, 0xB2B2B8);
        fill_rect(x + 6, hz - 46, 22, 46, 0x6C6C78);                 /* towers */
        fill_rect(x + w - 28, hz - 46, 22, 46, 0x6C6C78);
        for (int i = 0; i < 3; i++) {
            fill_rect(x + 6 + i * 8, hz - 51, 6, 5, 0x6C6C78);
            fill_rect(x + w - 28 + i * 8, hz - 51, 6, 5, 0x6C6C78);
        }
        fill_rect(cx - 9, hz - 17, 18, 17, 0x262628);                /* gate */
    }
    fill_rect(x - 2, y - 2, w + 4, 2, 0xFFFFFF); fill_rect(x - 2, y + h, w + 4, 2, 0xFFFFFF);
    fill_rect(x - 2, y - 2, 2, h + 4, 0xFFFFFF); fill_rect(x + w, y - 2, 2, h + 4, 0xFFFFFF);
}

static void draw_main_menu(const Settings *s, int tab, int focus, int item, float spin)
{
    memcpy(pix, bg, sizeof(pix));

    /* header: logo + tab pills */
    blend_rect(0, 0, IW - 1, 46, 0xFFFFFF, 80);
    fill_rect(0, 46, IW, 2, 0xFFFFFF);
    draw_text_ex(16, 12, "BLOX", 3, 0x1E8A32, 0xFFFFFF);
    int tx = 96;
    for (int i = 0; i < NTABS; i++) {
        int tw = text_w(TAB_NAMES[i], 2), pw = tw + 24;
        if (i == tab) {
            fill_round(tx, 9, pw, 30, 14, focus == 0 ? GREEN : 0xF4FFF4u);
            draw_text_ex(tx + 12, 17, TAB_NAMES[i], 2, focus == 0 ? 0xFFFFFFu : 0x1E8A32u, NO_SHADOW);
        } else {
            draw_text_ex(tx + 12, 17, TAB_NAMES[i], 2, GREEN_DK, NO_SHADOW);
        }
        tx += pw + 6;
    }

    /* content panel */
    blend_rect(24, 62, 616, 318, 0xFFFFFF, 150);
    fill_rect(24, 62, 593, 2, 0xFFFFFF); fill_rect(24, 317, 593, 2, 0xFFFFFF);
    fill_rect(24, 62, 2, 257, 0xFFFFFF); fill_rect(615, 62, 2, 257, 0xFFFFFF);

    char line[48];
    if (tab == 0) {                                                       /* PLAY */
        draw_menu_noob(costume_for(s->skin, &s->custom), spin);
        draw_text_ex(44, 80, "READY TO PLAY?", 2, PANEL_TXT, NO_SHADOW);
        fill_round(44, 118, 220, 46, 12, focus == 0 ? GREEN : 0x8FD39Bu);
        draw_text_ex(44 + 110 - text_w("START GAME", 2) / 2, 133, "START GAME", 2, 0xFFFFFF, NO_SHADOW);
        snprintf(line, sizeof(line), "MAP: %s", MAP_NAMES[s->map]);
        draw_text_ex(44, 192, line, 2, PANEL_TXT, NO_SHADOW);
        snprintf(line, sizeof(line), "SKIN: %s", costume_for(s->skin, &s->custom)->name);
        draw_text_ex(44, 218, line, 2, PANEL_TXT, NO_SHADOW);
        draw_text_ex(44, 262, "CHANGE THEM IN THE MAPS, SKINS AND AVATAR TABS", 1, PANEL_TXT, NO_SHADOW);
    } else if (tab == 1) {                                                /* MAPS */
        int shown = focus ? item : s->map;
        for (int i = 0; i < NUM_MAPS; i++) {
            int y = 84 + i * 46;
            int hot = focus && i == item;
            if (hot) fill_round(40, y, 270, 38, 10, GREEN);
            else if (i == s->map) fill_round(40, y, 270, 38, 10, GREEN_LT);
            draw_text_ex(56, y + 12, MAP_NAMES[i], 2, hot ? 0xFFFFFFu : PANEL_TXT, NO_SHADOW);
            if (i == s->map) draw_text_ex(300 - text_w("CURRENT", 1), y + 16, "CURRENT", 1, hot ? 0xFFFFFFu : PANEL_TXT, NO_SHADOW);
        }
        draw_thumb(shown, 350, 90, 240, 130);
        draw_text_cx(470, 236, MAP_DESC[shown], 2, PANEL_TXT, NO_SHADOW);
    } else if (tab == 2) {                                                /* SKINS (presets + custom) */
        int cur = focus ? item : s->skin, vis = 7, first = list_first(cur, NUM_SKINS, vis);
        draw_menu_noob(costume_for(cur, &s->custom), spin);
        for (int r = 0; r < vis && first + r < NUM_SKINS; r++) {
            int i = first + r, y = 74 + r * 33, hot = focus && i == item;
            if (hot) fill_round(40, y, 270, 29, 8, GREEN);
            else if (i == s->skin) fill_round(40, y, 270, 29, 8, GREEN_LT);
            const Costume *c = costume_for(i, &s->custom);
            draw_text_ex(54, y + 7, i < NUM_PRESETS ? COSTUMES[i].name : "CUSTOM", 2, hot ? 0xFFFFFFu : PANEL_TXT, NO_SHADOW);
            swatches(214, y + 7, 14, c);
        }
        if (NUM_SKINS > vis) {                                            /* scroll bar */
            int th = 7 * 33 * vis / NUM_SKINS, ty = 74 + (7 * 33 - th) * first / (NUM_SKINS - vis);
            fill_rect(318, 74, 4, 7 * 33, 0xD8EBDC); fill_rect(318, ty, 4, th, GREEN);
        }
    } else if (tab == 3) {                                                /* AVATAR creator */
        draw_menu_noob(costume_for(CUSTOM_SKIN, &s->custom), spin);
        draw_creator_rows(s, 56, 74, 250, 30, item, focus, PANEL_TXT, GREEN, 0xFFFFFF, NO_SHADOW);
        if (!focus) draw_text_ex(56, 74 + CR_ROWS * 30 + 2, "PRESS X TO EDIT", 1, PANEL_TXT, NO_SHADOW);
    } else if (tab == 4) {                                                /* SETTINGS */
        draw_settings_rows(s, 56, 78, 504, 36, item, focus, PANEL_TXT, GREEN, 0xFFFFFF, NO_SHADOW);
    } else {                                                              /* QUIT */
        draw_text_cx(320, 150, "QUIT TO THE XMB?", 3, PANEL_TXT, NO_SHADOW);
        draw_text_cx(320, 200, "PRESS X", 2, PANEL_TXT, NO_SHADOW);
    }

    if (s->tooltips)
        draw_text_ex(14, IH - 16, "L1/R1 OR LEFT/RIGHT: TABS   UP/DOWN: MOVE   X: SELECT   O: BACK", 1, 0xFFFFFF, GREEN_DK);
}

/* ---------- in-game menus ---------- */

static const char *PAUSE_ITEMS[6] = { "RESUME", "RESTART", "SKINS", "AVATAR", "SETTINGS", "MAIN MENU" };

static void draw_panel_box(int x0, int y0, int x1, int y1)
{
    dim_rect(x0, y0, x1, y1, 2);
    fill_rect(x0, y0, x1 - x0 + 1, 2, 0x707070);  fill_rect(x0, y1 - 1, x1 - x0 + 1, 2, 0x707070);
    fill_rect(x0, y0, 2, y1 - y0 + 1, 0x707070);  fill_rect(x1 - 1, y0, 2, y1 - y0 + 1, 0x707070);
}

/* pause menu: left-hand green panel in the same style as the main menu */
static void draw_pause_menu(int sel, int tips)
{
    blend_rect(0, 0, IW - 1, IH - 1, 0x0B3D1A, 100);              /* soft green tint over the game */
    blend_rect(14, 14, 254, IH - 15, 0xFFFFFF, 175);               /* panel */
    fill_rect(14, 14, 241, 2, 0xFFFFFF);  fill_rect(14, IH - 16, 241, 2, 0xFFFFFF);
    fill_rect(14, 14, 2, IH - 28, 0xFFFFFF);  fill_rect(253, 14, 2, IH - 28, 0xFFFFFF);

    draw_text_ex(32, 28, "PAUSED", 3, 0x1E8A32, 0xFFFFFF);
    fill_rect(30, 62, 208, 2, GREEN);

    for (int i = 0; i < 6; i++) {
        int y = 80 + i * 38;
        if (i == sel) {
            fill_round(30, y, 208, 32, 12, GREEN);
            draw_text_ex(46, y + 9, PAUSE_ITEMS[i], 2, 0xFFFFFF, NO_SHADOW);
        } else {
            draw_text_ex(46, y + 9, PAUSE_ITEMS[i], 2, PANEL_TXT, NO_SHADOW);
        }
    }
    if (tips) {
        draw_text_ex(30, IH - 52, "X: SELECT", 1, PANEL_TXT, NO_SHADOW);
        draw_text_ex(30, IH - 38, "O OR START: RESUME", 1, PANEL_TXT, NO_SHADOW);
    }
}

static void draw_settings_menu(const Settings *s, int sel, int tips)
{
    dim_rect(0, 0, IW - 1, IH - 1, 1);
    draw_panel_box(130, 40, 510, 336);
    draw_text_c(320, 54, "SETTINGS", 3, 0xFFFFFF);
    draw_settings_rows(s, 156, 98, 328, 34, sel, 1, 0xFFFFFF, GREEN, 0xFFFFFF, 0x000000);
    if (tips) draw_text_c(320, 316, "UP/DOWN: SELECT   LEFT/RIGHT: CHANGE   O: BACK", 1, 0xC0C0C0);
}

static void draw_skins_menu(const Settings *s, int tips)
{
    int vis = 8, first = list_first(s->skin, NUM_SKINS, vis);
    dim_rect(0, 0, 262, IH - 1, 2);                 /* left panel; character stays visible */
    fill_rect(262, 0, 2, IH, 0x707070);

    draw_text(14, 30, "SKINS", 3, 0xFFFFFF);
    for (int r = 0; r < vis && first + r < NUM_SKINS; r++) {
        int i = first + r, y = 68 + r * 30;
        u32 col = (i == s->skin) ? COL_HILITE : 0xFFFFFF;
        if (i == s->skin) draw_text(10, y, ">", 2, col);
        draw_text(30, y, i < NUM_PRESETS ? COSTUMES[i].name : "CUSTOM", 2, col);
        swatches(196, y, 12, costume_for(i, &s->custom));
    }
    if (tips) {
        draw_text(14, 326, "UP/DOWN: CHANGE", 1, 0xC0C0C0);
        draw_text(14, 340, "X OR O: BACK", 1, 0xC0C0C0);
    }
}

static void draw_creator_menu(const Settings *s, int sel, int tips)
{
    dim_rect(0, 0, 262, IH - 1, 2);
    fill_rect(262, 0, 2, IH, 0x707070);
    draw_text(14, 28, "AVATAR", 3, 0xFFFFFF);
    draw_creator_rows(s, 20, 62, 222, 29, sel, 1, 0xFFFFFF, GREEN, 0xFFFFFF, 0x000000);
    if (tips) {
        draw_text(14, 326, "UP/DOWN: SELECT   LEFT/RIGHT: CHANGE", 1, 0xC0C0C0);
        draw_text(14, 340, "O: BACK", 1, 0xC0C0C0);
    }
}

/* ---------- input ---------- */

static float axis(u16 raw)
{
    float v = ((float)raw - 128.0f) / 128.0f;
    return (fabsf(v) < 0.2f) ? 0.0f : v;
}

/* ---------- saved settings (system storage) ---------- */

/* Written to the game's own folder and to /dev_hdd0/tmp, so choices survive a
 * reinstall. "BLX3" + 14 bytes: skin, map, tips, fps, fps-in-menus, cap index,
 * custom cap, sound, then the custom avatar (hat, hat colour, skin, shirt,
 * pants, face). Older BLX2 / BLX1 files still load. Bad file = defaults. */
#ifndef SAVE_PATH_1
#define SAVE_PATH_1 "/dev_hdd0/game/BLOX00001/USRDIR/blox-ps3.sav"
#endif
#ifndef SAVE_PATH_2
#define SAVE_PATH_2 "/dev_hdd0/tmp/blox-ps3.sav"
#endif

static void load_settings(Settings *s)
{
    settings_default(s);
    const char *paths[2] = { SAVE_PATH_1, SAVE_PATH_2 };
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        unsigned char b[24];
        size_t n = fread(b, 1, sizeof(b), f);
        fclose(f);
        if (n >= 18 && memcmp(b, "BLX3", 4) == 0 && b[4] < NUM_SKINS && b[5] < NUM_MAPS && b[9] <= 5 &&
            b[10] >= 5 && b[10] <= 240 && b[12] < NUM_HATS && b[13] < NUM_PAL && b[14] < NUM_PAL &&
            b[15] < NUM_PAL && b[16] < NUM_PAL && b[17] < 5) {
            s->skin = b[4]; s->map = b[5]; s->tooltips = b[6] != 0; s->show_fps = b[7] != 0;
            s->fps_menu = b[8] != 0; s->cap_idx = b[9]; s->custom_cap = b[10]; s->sound = b[11] != 0;
            s->custom.hat = b[12]; s->custom.hat_col = b[13]; s->custom.skin_col = b[14];
            s->custom.shirt_col = b[15]; s->custom.pants_col = b[16]; s->custom.face = b[17];
            return;
        }
        if (n >= 11 && memcmp(b, "BLX2", 4) == 0 && b[4] < 7 && b[5] < NUM_MAPS && b[9] <= 5 &&
            b[10] >= 5 && b[10] <= 240) {
            s->skin = b[4]; s->map = b[5]; s->tooltips = b[6] != 0; s->show_fps = b[7] != 0;
            s->fps_menu = b[8] != 0; s->cap_idx = b[9]; s->custom_cap = b[10];
            return;
        }
        if (n >= 5 && memcmp(b, "BLX1", 4) == 0 && b[4] < 7) { s->skin = b[4]; return; }
    }
}

static void save_settings(const Settings *s)
{
    const char *paths[2] = { SAVE_PATH_1, SAVE_PATH_2 };
    unsigned char b[18] = { 'B', 'L', 'X', '3', (unsigned char)s->skin, (unsigned char)s->map,
                            (unsigned char)s->tooltips, (unsigned char)s->show_fps,
                            (unsigned char)s->fps_menu, (unsigned char)s->cap_idx,
                            (unsigned char)s->custom_cap, (unsigned char)s->sound,
                            (unsigned char)s->custom.hat, (unsigned char)s->custom.hat_col,
                            (unsigned char)s->custom.skin_col, (unsigned char)s->custom.shirt_col,
                            (unsigned char)s->custom.pants_col, (unsigned char)s->custom.face };
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "wb");
        if (!f) continue;                 /* folder missing / not writable: skip */
        fwrite(b, 1, sizeof(b), f);
        fclose(f);
    }
}

/* ---------- main ---------- */

enum { ST_MAIN, ST_PLAY, ST_PAUSE, ST_SKINS, ST_CREATOR, ST_SETTINGS };

#define JUMP_ARMS_UP (-2.75f)   /* arms swing forward and up over the head */

typedef struct { v3 pos; float vy, fyaw, cyaw, cpitch, phase, air_t; int grounded; } Player;

static void reset_player(Player *p, int map)
{
    memset(p, 0, sizeof(*p));
    p->cpitch = 0.35f;
    p->grounded = 1;
    if (map == MAP_CASTLE) { p->pos.z = 66.0f; p->fyaw = PI_F; }   /* outside the gate, facing it */
}

int main(int argc, const char *argv[])
{
    (void)argc; (void)argv;

    gfx_init();
    ioPadInit(7);
    sysUtilRegisterCallback(0, sysutil_cb, NULL);
    audio_init();

    Settings cfg;
    load_settings(&cfg);
    apply_cap(effective_cap(&cfg));
    build_castle();
    build_menu_bg();

    Player pl;
    reset_player(&pl, cfg.map);
    int state = ST_MAIN, sel = 0, ssel = 0, csel = 0, jump_lock = 0, last_step = 0;
    int mtab = 0, mfocus = 0, mitem = 0;
    float saved_yaw = 0, spin = 0;
    int p_start = 0, p_cross = 0, p_circle = 0, p_up = 0, p_down = 0, p_left = 0, p_right = 0, p_l1 = 0, p_r1 = 0;
    padInfo pinfo;
    padData pdata;
    double last = now_sec(), fps_acc = 0.0;
    int fps_frames = 0, fps = 0;

    while (running) {
        sysUtilCheckCallback();
        limit_frame();
        audio_pump();

        /* real frame time, so speed doesn't depend on framerate */
        double tnow = now_sec();
        float real_dt = (float)(tnow - last);
        last = tnow;
        float dt = real_dt;
        if (dt > 0.1f) dt = 0.1f;
        if (dt < 0.0005f) dt = 0.0005f;
#ifdef BLOX_FIXED_DT
        dt = 1.0f / 60.0f;      /* used by the PC preview tool */
#endif
        fps_acc += real_dt;
        fps_frames++;
        if (fps_acc >= 0.5 || (fps == 0 && fps_frames >= 10)) {
            fps = (int)(fps_frames / fps_acc + 0.5);
            fps_acc = 0.0;
            fps_frames = 0;
        }

        /* ---- input ---- */
        float lx = 0, ly = 0, rx = 0, ry = 0;
        int b_start = 0, b_cross = 0, b_circle = 0, b_up = 0, b_down = 0, b_left = 0, b_right = 0;
        int b_l1 = 0, b_r1 = 0, sprint = 0;
        ioPadGetInfo(&pinfo);
        if (pinfo.status[0]) {
            ioPadGetData(0, &pdata);
            lx = axis(pdata.ANA_L_H);
            ly = -axis(pdata.ANA_L_V);
            rx = axis(pdata.ANA_R_H);
            ry = axis(pdata.ANA_R_V);
            b_cross  = pdata.BTN_CROSS;
            b_circle = pdata.BTN_CIRCLE;
            b_start  = pdata.BTN_START;
            b_up     = pdata.BTN_UP;
            b_down   = pdata.BTN_DOWN;
            b_left   = pdata.BTN_LEFT;
            b_right  = pdata.BTN_RIGHT;
            b_l1     = pdata.BTN_L1;
            b_r1     = pdata.BTN_R1;
            sprint   = pdata.BTN_L2 > 100;
        }
        /* menu navigation also works with the left stick */
        if (state != ST_PLAY) {
            if (ly > 0.6f) b_up = 1;
            if (ly < -0.6f) b_down = 1;
            if (lx > 0.6f) b_right = 1;
            if (lx < -0.6f) b_left = 1;
        }
        int e_start = b_start && !p_start, e_cross = b_cross && !p_cross, e_circle = b_circle && !p_circle;
        int e_up = b_up && !p_up, e_down = b_down && !p_down, e_left = b_left && !p_left;
        int e_right = b_right && !p_right, e_l1 = b_l1 && !p_l1, e_r1 = b_r1 && !p_r1;
        p_start = b_start; p_cross = b_cross; p_circle = b_circle; p_up = b_up; p_down = b_down;
        p_left = b_left; p_right = b_right; p_l1 = b_l1; p_r1 = b_r1;

        /* ---- menu state machine ---- */
        int prev_state = state;
        if (state == ST_MAIN) {
            spin += 1.1f * dt;
            if (e_l1) mtab = (mtab + NTABS - 1) % NTABS;
            if (e_r1) mtab = (mtab + 1) % NTABS;
            if (mfocus == 0) {
                if (e_left)  mtab = (mtab + NTABS - 1) % NTABS;
                if (e_right) mtab = (mtab + 1) % NTABS;
                int has_list = (mtab >= 1 && mtab <= 4);
                int start_item = (mtab == 1) ? cfg.map : (mtab == 2) ? cfg.skin : 0;
                if (e_cross) {
                    if (mtab == 0) { reset_player(&pl, cfg.map); save_settings(&cfg); state = ST_PLAY; }
                    else if (mtab == 5) running = 0;
                    else { mfocus = 1; mitem = start_item; }
                } else if (e_down && has_list) {
                    mfocus = 1; mitem = start_item;
                }
            } else {
                int n = (mtab == 1) ? NUM_MAPS : (mtab == 2) ? NUM_SKINS : (mtab == 3) ? CR_ROWS : settings_rows(&cfg);
                if (e_up) { if (mitem == 0) mfocus = 0; else mitem--; }
                if (e_down && mitem < n - 1) mitem++;
                if (mtab == 1) cfg.map = mitem;
                else if (mtab == 2) cfg.skin = mitem;
                else if (mtab == 3) {
                    if (e_left && mitem < 6) creator_adjust(&cfg, mitem, -1);
                    if (e_right && mitem < 6) creator_adjust(&cfg, mitem, 1);
                    if (e_cross) creator_adjust(&cfg, mitem, 1);
                } else {
                    if (e_left) settings_adjust(&cfg, mitem, -1);
                    if (e_right || e_cross) settings_adjust(&cfg, mitem, 1);
                    n = settings_rows(&cfg);
                    if (mitem >= n) mitem = n - 1;
                }
                if (e_circle || (mtab <= 2 && e_cross)) { mfocus = 0; save_settings(&cfg); }
            }
        } else if (state == ST_PLAY) {
            if (e_start) { state = ST_PAUSE; sel = 0; }
        } else if (state == ST_PAUSE) {
            if (e_up)   sel = (sel + 5) % 6;
            if (e_down) sel = (sel + 1) % 6;
            if (e_circle || e_start) state = ST_PLAY;
            else if (e_cross) {
                if (sel == 0) state = ST_PLAY;
                else if (sel == 1) { reset_player(&pl, cfg.map); state = ST_PLAY; }
                else if (sel == 2) { state = ST_SKINS; saved_yaw = pl.fyaw; }
                else if (sel == 3) { state = ST_CREATOR; csel = 0; saved_yaw = pl.fyaw; }
                else if (sel == 4) { state = ST_SETTINGS; ssel = 0; }
                else { state = ST_MAIN; mtab = 0; mfocus = 0; save_settings(&cfg); }
            }
        } else if (state == ST_SKINS) {            /* the character spins so you see every side */
            if (e_up)   cfg.skin = (cfg.skin + NUM_SKINS - 1) % NUM_SKINS;
            if (e_down) cfg.skin = (cfg.skin + 1) % NUM_SKINS;
            pl.fyaw += 1.4f * dt;
            if (e_circle || e_cross || e_start) {
                state = ST_PAUSE; sel = 2; pl.fyaw = saved_yaw;
                save_settings(&cfg);
            }
        } else if (state == ST_CREATOR) {
            if (e_up)   csel = (csel + CR_ROWS - 1) % CR_ROWS;
            if (e_down) csel = (csel + 1) % CR_ROWS;
            if (e_left && csel < 6) creator_adjust(&cfg, csel, -1);
            if (e_right && csel < 6) creator_adjust(&cfg, csel, 1);
            if (e_cross) creator_adjust(&cfg, csel, 1);
            pl.fyaw += 1.4f * dt;
            if (e_circle || e_start) {
                state = ST_PAUSE; sel = 3; pl.fyaw = saved_yaw;
                save_settings(&cfg);
            }
        } else {                                   /* ST_SETTINGS */
            int n = settings_rows(&cfg);
            if (e_up)   ssel = (ssel + n - 1) % n;
            if (e_down) ssel = (ssel + 1) % n;
            if (e_left) settings_adjust(&cfg, ssel, -1);
            if (e_right || e_cross) settings_adjust(&cfg, ssel, 1);
            n = settings_rows(&cfg);
            if (ssel >= n) ssel = n - 1;
            if (e_circle || e_start) { state = ST_PAUSE; sel = 4; save_settings(&cfg); }
        }

        /* don't jump just because X was used to leave a menu */
        if (prev_state != ST_PLAY) jump_lock = 1;
        if (!b_cross) jump_lock = 0;
        int jump = b_cross && !jump_lock && state == ST_PLAY;

        /* ---- gameplay (frozen outside ST_PLAY) ---- */
        float mag = 0.0f;
        if (state == ST_PLAY) {
            pl.cyaw   -= rx * 2.5f * dt;
            pl.cpitch += ry * 1.5f * dt;
            if (pl.cpitch < 0.1f) pl.cpitch = 0.1f;
            if (pl.cpitch > 1.2f) pl.cpitch = 1.2f;

            /* movement relative to camera, Roblox default WalkSpeed = 16 */
            float fx = -sinf(pl.cyaw), fz = -cosf(pl.cyaw);
            float rxw = cosf(pl.cyaw), rzw = -sinf(pl.cyaw);
            float dx = fx * ly + rxw * lx, dz = fz * ly + rzw * lx;
            mag = sqrtf(dx * dx + dz * dz);
            if (mag > 1.0f) { dx /= mag; dz /= mag; mag = 1.0f; }

            float speed = sprint ? 28.0f : 16.0f;
            pl.pos.x += dx * speed * dt;
            pl.pos.z += dz * speed * dt;
            if (cfg.map == MAP_CASTLE) collide(&pl.pos);

            if (mag > 0.1f) {
                float target = atan2f(dx, dz);
                float diff = target - pl.fyaw;
                while (diff > PI_F) diff -= 2 * PI_F;
                while (diff < -PI_F) diff += 2 * PI_F;
                float k = 12.0f * dt;
                pl.fyaw += diff * (k > 1.0f ? 1.0f : k);
                pl.phase += mag * dt * (sprint ? 16.0f : 10.0f);
            }

            /* footsteps: one per half walk-cycle while on the ground */
            int sidx = (int)floorf(pl.phase / PI_F);
            if (cfg.sound && pl.grounded && mag > 0.1f && sidx != last_step)
                sfx(SFX_STEP, 0.55f + 0.3f * mag + (sprint ? 0.15f : 0.0f), sprint ? 0.42f : 0.26f, (sidx & 1) ? 0.3f : -0.3f);
            last_step = sidx;

            /* jump + gravity (Roblox: JumpPower 50, gravity 196.2), standing on blocks/stairs */
            float g = ground_at(cfg.map, pl.pos.x, pl.pos.y, pl.pos.z);
            int was_grounded = pl.grounded;
            if (jump && pl.grounded) {
                pl.vy = 50.0f;
                if (cfg.sound) sfx(SFX_JUMP, 0.9f, 0.0f, 0.0f);
            }
            pl.vy -= 196.2f * dt;
            float impact = pl.vy;
            pl.pos.y += pl.vy * dt;
            if (pl.pos.y <= g) {
                pl.pos.y = g;
                if (pl.vy < 0.0f) pl.vy = 0.0f;
                pl.grounded = 1;
                if (cfg.sound && !was_grounded && impact < -30.0f)
                    sfx(SFX_LAND, 0.2f + 0.7f * fminf(1.0f, -impact / 120.0f), 0.0f, 0.0f);
            } else pl.grounded = (pl.pos.y - g) < 0.05f;
            if (pl.pos.y < -80.0f) reset_player(&pl, cfg.map);      /* fell off the map */
        }

        /* ---- pose: walk swing, blended into the jump pose (arms up) in the air ---- */
        {
            float air_target = pl.grounded ? 0.0f : 1.0f;
            float kk = 14.0f * dt;
            pl.air_t += (air_target - pl.air_t) * (kk > 1.0f ? 1.0f : kk);
        }
        float ws = sinf(pl.phase) * 0.9f * mag, a = pl.air_t;
        float armL_a = -ws * (1.0f - a) + JUMP_ARMS_UP * a;
        float armR_a =  ws * (1.0f - a) + JUMP_ARMS_UP * a;
        float legL_a =  ws * (1.0f - a) + (-0.40f) * a;
        float legR_a = -ws * (1.0f - a) + ( 0.35f) * a;

        /* ---- render ---- */
        if (state == ST_MAIN) {
            draw_main_menu(&cfg, mtab, mfocus, mitem, spin);
        } else {
            const Costume *cs = costume_for(state == ST_CREATOR ? CUSTOM_SKIN : cfg.skin, &cfg.custom);
            float dist = 14.0f;
            v3 target = { pl.pos.x, pl.pos.y + 3.0f, pl.pos.z };
            v3 cp = { target.x + sinf(pl.cyaw) * cosf(pl.cpitch) * dist,
                      target.y + sinf(pl.cpitch) * dist,
                      target.z + cosf(pl.cyaw) * cosf(pl.cpitch) * dist };
            set_camera(cp, target);

            draw_floor(cfg.map);                         /* overwrites every pixel */
            if (cfg.map == MAP_CASTLE) {
                memset(zbuf, 0, sizeof(zbuf));
                sc_x0 = 0; sc_y0 = 0; sc_x1 = IW - 1; sc_y1 = IH - 1;
                draw_blocks();
                draw_noob(pl.pos, pl.fyaw, armL_a, armR_a, legL_a, legR_a, cs);
            } else if (setup_noob_region(pl.pos, pl.fyaw)) {
                draw_noob(pl.pos, pl.fyaw, armL_a, armR_a, legL_a, legR_a, cs);
            }

            if (state == ST_PAUSE) draw_pause_menu(sel, cfg.tooltips);
            else if (state == ST_SKINS) draw_skins_menu(&cfg, cfg.tooltips);
            else if (state == ST_CREATOR) draw_creator_menu(&cfg, csel, cfg.tooltips);
            else if (state == ST_SETTINGS) draw_settings_menu(&cfg, ssel, cfg.tooltips);
        }

        int show_fps = (state == ST_PLAY) ? cfg.show_fps : cfg.fps_menu;
        if (show_fps) {
            char fpstxt[16];
            snprintf(fpstxt, sizeof(fpstxt), "FPS %d", fps);
            if (state == ST_MAIN) draw_text_ex(IW - 100, IH - 20, fpstxt, 2, 0xFFFFFF, GREEN_DK);
            else draw_text(6, 6, fpstxt, 2, 0xFFFFFF);
        }
        if (state == ST_PLAY && cfg.tooltips)
            draw_text(6, IH - 14, "L STICK: MOVE   R STICK: CAMERA   X: JUMP   L2: SPRINT   START: MENU", 1, 0xE0E0E0);

        present();
    }

#ifdef BLOX_AUDIO_DUMP
    {   /* PC preview only: write what the mixer produced to a 16-bit stereo WAV */
        FILE *w = fopen("audio.wav", "wb");
        if (w) {
            unsigned frames = (unsigned)(aud_dump_n / 2), bytes = frames * 4, sr = 48000, br = 48000 * 4;
            unsigned short ch = 2, bits = 16, ba = 4, fmt = 1;
            unsigned fl = 16, rl = 36 + bytes;
            fwrite("RIFF", 1, 4, w); fwrite(&rl, 4, 1, w); fwrite("WAVEfmt ", 1, 8, w); fwrite(&fl, 4, 1, w);
            fwrite(&fmt, 2, 1, w); fwrite(&ch, 2, 1, w); fwrite(&sr, 4, 1, w); fwrite(&br, 4, 1, w);
            fwrite(&ba, 2, 1, w); fwrite(&bits, 2, 1, w); fwrite("data", 1, 4, w); fwrite(&bytes, 4, 1, w);
            for (int i = 0; i < aud_dump_n; i++) {
                short v = (short)(aud_dump[i] * 32000.0f);
                fwrite(&v, 2, 1, w);
            }
            fclose(w);
        }
    }
#endif
    audio_shutdown();
    save_settings(&cfg);        /* also saved when you quit from the XMB menu */
    return 0;
}
