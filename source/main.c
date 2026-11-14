/*
 * blox-ps3: baseplate + noob prototype for the PS3 (PSL1GHT).
 * Runs on the Cell PPE. Software rasterizer renders at 640x360 into a
 * main-memory buffer, then gets upscaled into the RSX framebuffer.
 *
 * Controls: Left stick = move, Right stick = camera, X = jump, L2 = sprint,
 *           START = pause menu (Resume / Restart / Skins).
 * Menu: D-pad or left stick up/down, X = select, O = back.
 * Top-left corner shows an FPS counter.
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

#define IW      640
#define IH      360
#define FOCAL   300.0f
#define NEAR    0.1f
#define PI_F    3.14159265f
#define SKY_COL 0x6FB4F0u
#define FLOOR_EXT 256            /* baseplate = 512 x 512 studs, centered on 0,0 */

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
 * Faces pointing away from the camera are culled.
 */
static void draw_part(v3 origin, float yaw, v3 off, v3 half, float swing,
                      const unsigned char *rgb, int skip)
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
        if (skip & (1 << f)) continue;
        v3 q[4] = { c[FACES[f][0]], c[FACES[f][1]], c[FACES[f][2]], c[FACES[f][3]] };
        v3 n = cross(sub(q[1], q[0]), sub(q[2], q[0]));
        v3 fc = { (q[0].x + q[2].x) * 0.5f, (q[0].y + q[2].y) * 0.5f, (q[0].z + q[2].z) * 0.5f };
        if (dot(n, sub(fc, cen)) < 0.0f) { n.x = -n.x; n.y = -n.y; n.z = -n.z; } /* outward */
        if (dot(n, q[0]) >= 0.0f) continue;                                     /* faces away */
        draw_poly(q, 4, shade(rgb, FACE_SHADE[f]));
    }
}

/* flat quad on the character's +z side (used for eyes/mouth), 1 quad not 6 faces */
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
    if (dot(nc, mid) >= 0.0f) return;
    draw_poly(q, 4, col);
}

enum { FACE_STRAIGHT, FACE_SMILE, FACE_ANGRY, FACE_VISOR, FACE_SLIT };

typedef struct {
    const char *name;
    unsigned char skin[3], torso[3], legs[3];   /* head+arms, torso, legs */
    u32 eye;                                    /* face / visor colour */
    int face;
} Costume;

static const Costume COSTUMES[] = {
    { "NOOB",   {245, 205,  48}, { 13, 105, 172}, { 40, 127,  71}, 0x141414, FACE_STRAIGHT },
    { "RED",    {245, 205,  48}, {196,  40,  28}, { 60,  60,  66}, 0x141414, FACE_SMILE    },
    { "ZOMBIE", { 96, 150,  84}, { 52,  84,  96}, { 74,  92,  62}, 0x6A1010, FACE_ANGRY    },
    { "NINJA",  { 38,  38,  44}, { 28,  28,  34}, { 28,  28,  34}, 0xF2F2F2, FACE_SLIT     },
    { "ROBOT",  {170, 178, 188}, { 96, 106, 128}, { 74,  80,  98}, 0x40E8F8, FACE_VISOR    },
    { "PINK",   {250, 214, 176}, {240, 120, 170}, {150,  96, 200}, 0x141414, FACE_SMILE    },
    { "GOLD",   {255, 205,  60}, {232, 170,  24}, {196, 136,  12}, 0x3A2A00, FACE_STRAIGHT },
};
#define NUM_COSTUMES ((int)(sizeof(COSTUMES) / sizeof(COSTUMES[0])))

static const unsigned char COL_GRASS1[3] = { 75, 151, 75 };
static const unsigned char COL_GRASS2[3] = { 66, 135, 66 };

/*
 * Baseplate, drawn analytically: for each screen row the ground plane is hit at
 * one distance t, and world x/z then change linearly across the row, so a
 * 16.16 fixed-point DDA gives the checker colour with a few integer ops per
 * pixel. 0 triangles, no z-buffer, writes every pixel (so no screen clear).
 */
static void draw_floor(void)
{
    const u32 g1 = shade(COL_GRASS1, 1.0f), g2 = shade(COL_GRASS2, 1.0f);
    const u64 sky2 = ((u64)SKY_COL << 32) | SKY_COL;
    const unsigned EXT = (unsigned)FLOOR_EXT << 16;

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

        for (int x = 0; x < IW; x++, ix += dx, iz += dz) {
            if ((unsigned)(ix + (int)EXT) >= 2u * EXT || (unsigned)(iz + (int)EXT) >= 2u * EXT)
                row[x] = SKY_COL;               /* off the edge of the baseplate: void */
            else
                row[x] = (((ix >> 18) + (iz >> 18)) & 1) ? g1 : g2;   /* 4-stud tiles */
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

/* limb angles are radians about the shoulder/hip: + = swing back, - = swing forward/up */
static void draw_noob(v3 pos, float yaw, float armL_a, float armR_a,
                      float legL_a, float legR_a, const Costume *cs)
{
    v3 head  = { 0, 4.5f, 0 },     headH = { 0.6f, 0.5f, 0.6f };
    v3 torso = { 0, 3.0f, 0 },     torsoH = { 1.0f, 1.0f, 0.5f };
    v3 armL  = { -1.5f, 3.0f, 0 }, armR = { 1.5f, 3.0f, 0 }, limbH = { 0.5f, 1.0f, 0.5f };
    v3 legL  = { -0.5f, 1.0f, 0 }, legR = { 0.5f, 1.0f, 0 };

    /* faces that sit flush against another part are never drawn */
    draw_part(pos, yaw, legL,  limbH,  legL_a, cs->legs,  F_PY);
    draw_part(pos, yaw, legR,  limbH,  legR_a, cs->legs,  F_PY);
    draw_part(pos, yaw, torso, torsoH, 0,      cs->torso, F_NY);
    draw_part(pos, yaw, armL,  limbH,  armL_a, cs->skin,  0);
    draw_part(pos, yaw, armR,  limbH,  armR_a, cs->skin,  0);
    draw_part(pos, yaw, head,  headH,  0,      cs->skin,  F_NY);
    draw_face(pos, yaw, cs);
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

static void draw_text(int x, int y, const char *str, int sc, u32 col)
{
    for (int pass = 0; pass < 2; pass++) {          /* pass 0 = shadow */
        int ox = pass == 0 ? sc : 0, oy = pass == 0 ? sc : 0;
        u32 c = pass == 0 ? 0x000000 : col;
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

static int text_w(const char *s, int sc)
{
    int n = (int)strlen(s);
    return n ? n * 6 * sc - sc : 0;
}

static void draw_text_c(int cx, int y, const char *s, int sc, u32 col)
{
    draw_text(cx - text_w(s, sc) / 2, y, s, sc, col);
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

#define COL_HILITE 0xFFD030u

static const char *PAUSE_ITEMS[3] = { "RESUME", "RESTART", "SKINS" };

static void draw_pause_menu(int sel)
{
    dim_rect(0, 0, IW - 1, IH - 1, 1);
    dim_rect(215, 78, 425, 286, 2);
    fill_rect(215, 78, 211, 2, 0x707070);  fill_rect(215, 284, 211, 2, 0x707070);
    fill_rect(215, 78, 2, 208, 0x707070);  fill_rect(424, 78, 2, 208, 0x707070);

    draw_text_c(320, 96, "PAUSED", 3, 0xFFFFFF);
    for (int i = 0; i < 3; i++) {
        int y = 150 + i * 34;
        u32 col = (i == sel) ? COL_HILITE : 0xFFFFFF;
        if (i == sel) draw_text(320 - text_w(PAUSE_ITEMS[i], 2) / 2 - 20, y, ">", 2, col);
        draw_text_c(320, y, PAUSE_ITEMS[i], 2, col);
    }
    draw_text_c(320, 262, "X SELECT   O RESUME", 1, 0xC0C0C0);
}

static void draw_skins_menu(int cur_skin)
{
    dim_rect(0, 0, 215, IH - 1, 2);                 /* left panel; character stays visible */
    fill_rect(215, 0, 2, IH, 0x707070);

    draw_text(14, 30, "SKINS", 3, 0xFFFFFF);
    for (int i = 0; i < NUM_COSTUMES; i++) {
        int y = 68 + i * 32;
        u32 col = (i == cur_skin) ? COL_HILITE : 0xFFFFFF;
        if (i == cur_skin) draw_text(10, y, ">", 2, col);
        draw_text(30, y, COSTUMES[i].name, 2, col);
        const Costume *c = &COSTUMES[i];
        fill_rect(132, y, 14, 14, (u32)(c->skin[0] << 16 | c->skin[1] << 8 | c->skin[2]));
        fill_rect(150, y, 14, 14, (u32)(c->torso[0] << 16 | c->torso[1] << 8 | c->torso[2]));
        fill_rect(168, y, 14, 14, (u32)(c->legs[0] << 16 | c->legs[1] << 8 | c->legs[2]));
    }
    draw_text(14, 326, "UP/DOWN CHANGE", 1, 0xC0C0C0);
    draw_text(14, 340, "X OR O BACK", 1, 0xC0C0C0);
}

/* ---------- input ---------- */

static float axis(u16 raw)
{
    float v = ((float)raw - 128.0f) / 128.0f;
    return (fabsf(v) < 0.2f) ? 0.0f : v;
}

/* ---------- saved skin (system storage) ---------- */

/* Written to the game's own folder and to /dev_hdd0/tmp, so the choice survives
 * a reinstall. Format: "BLX1" + 1 byte skin index. Missing/corrupt file = skin 0. */
#ifndef SAVE_PATH_1
#define SAVE_PATH_1 "/dev_hdd0/game/BLOX00001/USRDIR/blox-ps3.sav"
#endif
#ifndef SAVE_PATH_2
#define SAVE_PATH_2 "/dev_hdd0/tmp/blox-ps3.sav"
#endif

static int load_skin(void)
{
    const char *paths[2] = { SAVE_PATH_1, SAVE_PATH_2 };
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "rb");
        if (!f) continue;
        unsigned char buf[8];
        size_t n = fread(buf, 1, sizeof(buf), f);
        fclose(f);
        if (n >= 5 && memcmp(buf, "BLX1", 4) == 0 && buf[4] < NUM_COSTUMES)
            return buf[4];
    }
    return 0;
}

static void save_skin(int skin)
{
    const char *paths[2] = { SAVE_PATH_1, SAVE_PATH_2 };
    unsigned char buf[5] = { 'B', 'L', 'X', '1', (unsigned char)skin };
    for (int i = 0; i < 2; i++) {
        FILE *f = fopen(paths[i], "wb");
        if (!f) continue;                 /* folder missing / not writable: skip */
        fwrite(buf, 1, sizeof(buf), f);
        fclose(f);
    }
}

/* ---------- main ---------- */

enum { ST_PLAY, ST_PAUSE, ST_SKINS };

#define JUMP_ARMS_UP (-2.75f)   /* arms swing forward and up over the head */

int main(int argc, const char *argv[])
{
    (void)argc; (void)argv;

    gfx_init();
    ioPadInit(7);
    sysUtilRegisterCallback(0, sysutil_cb, NULL);

    v3 pos = { 0, 0, 0 };
    float vy = 0, fyaw = 0, cyaw = 0, cpitch = 0.35f, phase = 0, air_t = 0, saved_yaw = 0;
    int state = ST_PLAY, sel = 0, skin = load_skin(), jump_lock = 0;
    int p_start = 0, p_cross = 0, p_circle = 0, p_up = 0, p_down = 0;
    padInfo pinfo;
    padData pdata;
    double last = now_sec(), fps_acc = 0.0;
    int fps_frames = 0, fps = 0;

    while (running) {
        sysUtilCheckCallback();

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
        int b_start = 0, b_cross = 0, b_circle = 0, b_up = 0, b_down = 0, sprint = 0;
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
            sprint   = pdata.BTN_L2 > 100;
        }
        /* menu navigation also works with the left stick */
        if (state != ST_PLAY) {
            if (ly > 0.6f) b_up = 1;
            if (ly < -0.6f) b_down = 1;
        }
        int e_start = b_start && !p_start, e_cross = b_cross && !p_cross;
        int e_circle = b_circle && !p_circle, e_up = b_up && !p_up, e_down = b_down && !p_down;
        p_start = b_start; p_cross = b_cross; p_circle = b_circle; p_up = b_up; p_down = b_down;

        /* ---- menu state machine ---- */
        int prev_state = state;
        if (state == ST_PLAY) {
            if (e_start) { state = ST_PAUSE; sel = 0; }
        } else if (state == ST_PAUSE) {
            if (e_up)   sel = (sel + 2) % 3;
            if (e_down) sel = (sel + 1) % 3;
            if (e_circle || e_start) state = ST_PLAY;
            else if (e_cross) {
                if (sel == 0) state = ST_PLAY;
                else if (sel == 1) {                       /* restart */
                    pos.x = pos.y = pos.z = 0;
                    vy = 0; fyaw = 0; cyaw = 0; cpitch = 0.35f; phase = 0; air_t = 0;
                    state = ST_PLAY;
                } else { state = ST_SKINS; saved_yaw = fyaw; }
            }
        } else { /* ST_SKINS: the character spins behind the list so you see every side */
            if (e_up)   skin = (skin + NUM_COSTUMES - 1) % NUM_COSTUMES;
            if (e_down) skin = (skin + 1) % NUM_COSTUMES;
            fyaw += 1.4f * dt;
            if (e_circle || e_cross || e_start) {
                state = ST_PAUSE; sel = 2; fyaw = saved_yaw;
                save_skin(skin);                       /* remember the choice */
            }
        }

        /* don't jump just because X was used to leave a menu */
        if (prev_state != ST_PLAY) jump_lock = 1;
        if (!b_cross) jump_lock = 0;
        int jump = b_cross && !jump_lock && state == ST_PLAY;

        /* ---- gameplay (frozen while paused) ---- */
        float mag = 0.0f;
        if (state == ST_PLAY) {
            cyaw   -= rx * 2.5f * dt;
            cpitch += ry * 1.5f * dt;
            if (cpitch < 0.1f) cpitch = 0.1f;
            if (cpitch > 1.2f) cpitch = 1.2f;

            /* movement relative to camera, Roblox default WalkSpeed = 16 */
            float fx = -sinf(cyaw), fz = -cosf(cyaw);
            float rxw = cosf(cyaw), rzw = -sinf(cyaw);
            float dx = fx * ly + rxw * lx, dz = fz * ly + rzw * lx;
            mag = sqrtf(dx * dx + dz * dz);
            if (mag > 1.0f) { dx /= mag; dz /= mag; mag = 1.0f; }

            float speed = sprint ? 28.0f : 16.0f;
            pos.x += dx * speed * dt;
            pos.z += dz * speed * dt;

            if (mag > 0.1f) {
                float target = atan2f(dx, dz);
                float diff = target - fyaw;
                while (diff > PI_F) diff -= 2 * PI_F;
                while (diff < -PI_F) diff += 2 * PI_F;
                float k = 12.0f * dt;
                fyaw += diff * (k > 1.0f ? 1.0f : k);
                phase += mag * dt * (sprint ? 16.0f : 10.0f);
            }

            /* jump + gravity (Roblox: JumpPower 50, gravity 196.2) */
            if (jump && pos.y <= 0.0f) vy = 50.0f;
            vy -= 196.2f * dt;
            pos.y += vy * dt;
            if (pos.y < 0.0f) { pos.y = 0.0f; vy = 0.0f; }
        }

        /* ---- pose: walk swing, blended into the jump pose (arms up) in the air ---- */
        {
            float air_target = (pos.y > 0.05f) ? 1.0f : 0.0f;
            float kk = 14.0f * dt;
            air_t += (air_target - air_t) * (kk > 1.0f ? 1.0f : kk);
        }
        float ws = sinf(phase) * 0.9f * mag;
        float armL_a = -ws * (1.0f - air_t) + JUMP_ARMS_UP * air_t;
        float armR_a =  ws * (1.0f - air_t) + JUMP_ARMS_UP * air_t;
        float legL_a =  ws * (1.0f - air_t) + (-0.40f) * air_t;
        float legR_a = -ws * (1.0f - air_t) + ( 0.35f) * air_t;

        /* ---- render ---- */
        float dist = 14.0f;
        v3 target = { pos.x, pos.y + 3.0f, pos.z };
        v3 cp = { target.x + sinf(cyaw) * cosf(cpitch) * dist,
                  target.y + sinf(cpitch) * dist,
                  target.z + cosf(cyaw) * cosf(cpitch) * dist };
        set_camera(cp, target);

        draw_floor();                            /* overwrites every pixel */
        if (setup_noob_region(pos, fyaw))
            draw_noob(pos, fyaw, armL_a, armR_a, legL_a, legR_a, &COSTUMES[skin]);

        if (state == ST_PAUSE) draw_pause_menu(sel);
        else if (state == ST_SKINS) draw_skins_menu(skin);

        char fpstxt[16];
        snprintf(fpstxt, sizeof(fpstxt), "FPS %d", fps);
        draw_text(6, 6, fpstxt, 2, 0xFFFFFF);
        if (state == ST_PLAY) draw_text(6, IH - 14, "START: MENU", 1, 0xE0E0E0);

        present();
    }

    save_skin(skin);            /* also saved when you quit from the XMB menu */
    return 0;
}
