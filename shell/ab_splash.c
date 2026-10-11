/*

MIT License

Copyright (c) 2026 AutoBleem authors

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.

*/

#include "shell/ab_splash.h"

#include "common/hardware/hwregs.h"
#include "shell/dcos.h"
#include "shell/gpu.h"

// Time runs in 1/300 s: a 60 Hz field adds 5, a 50 Hz one 6, and a timeline frame (60 fps) is 5 of them.
static unsigned s_step = 5;
static enum AbState s_state = AB_STATE_INTRO;
static unsigned s_ticks = 0;
static unsigned s_frame = 0;

// One track's values at the current frame.
struct Value {
    int32_t x, y;  // 1/4 px
    int32_t scale, scaleX;
    int32_t angle;
    int32_t bright;
    int32_t r, g, b;
};

static void uploadRect(const struct AbUpload *u) {
    sendGPUData(0x01000000);  // flush the texture cache
    sendGPUData(0xa0000000);
    sendGPUData(u->x | (u->y << 16));
    sendGPUData(u->w | (u->h << 16));
    const uint16_t *p = u->data;
    unsigned words = ((unsigned)u->w * u->h + 1) / 2;
    for (unsigned i = 0; i < words; i++, p += 2) {
        while ((GPU_STATUS & 0x10000000) == 0);
        GPU_DATA = p[0] | (p[1] << 16);
    }
}

void abSplashInit(unsigned fps) {
    s_step = fps == 50 ? 6 : 5;
    for (unsigned i = 0; i < g_abUploadCount; i++) uploadRect(&g_abUploads[i]);
    abSplashSetState(AB_STATE_INTRO);
}

void abSplashSetState(enum AbState state) {
    s_state = state;
    s_ticks = 0;
    s_frame = 0;
}

enum AbState abSplashState() { return s_state; }

int abSplashTick() {
    const struct AbTimeline *tl = &g_abTimelines[s_state];
    s_ticks += s_step;
    unsigned frame = s_ticks / 5;
    if (frame >= tl->frames) {
        if (!tl->loop) {
            s_frame = tl->frames - 1;
            return 1;
        }
        s_ticks -= tl->frames * 5;
        s_frame = s_ticks / 5;
        return 1;
    }
    s_frame = frame;
    return 0;
}

static int32_t mix(int32_t a, int32_t b, int32_t p) { return a + (((b - a) * p) >> 8); }

static void valueAt(const struct AbTrack *tr, struct Value *v) {
    const struct AbKey *k = tr->keys;
    unsigned n = tr->keyCount;
    unsigned i = 0;
    while (i + 1 < n && k[i + 1].frame <= s_frame) i++;
    const struct AbKey *a = &k[i];
    const struct AbKey *b = &k[i + 1 < n ? i + 1 : i];
    int32_t p = 0;
    if (b->frame > a->frame) p = (int32_t)((s_frame - a->frame) * 256 / (b->frame - a->frame));
    if (p > 256) p = 256;
    v->x = mix(a->x, b->x, p);
    v->y = mix(a->y, b->y, p);
    v->scale = mix(a->scale, b->scale, p);
    v->scaleX = mix(a->scaleX, b->scaleX, p);
    v->angle = mix(a->angle, b->angle, p);
    v->bright = mix(a->bright, b->bright, p);
    v->r = mix(a->r, b->r, p);
    v->g = mix(a->g, b->g, p);
    v->b = mix(a->b, b->b, p);
}

static const struct AbTrack *findTrack(enum AbKind kind) {
    const struct AbTimeline *tl = &g_abTimelines[s_state];
    for (unsigned i = 0; i < tl->trackCount; i++) {
        if (tl->tracks[i].kind == kind) return &tl->tracks[i];
    }
    return 0;
}

union Color abSplashBackground() {
    union Color c;
    c.packed = 0;
    const struct AbTrack *t = findTrack(AB_KIND_BG_EDGE);
    if (!t) return c;
    struct Value v;
    valueAt(t, &v);
    c.r = v.r;
    c.g = v.g;
    c.b = v.b;
    return c;
}

static uint32_t vertex(int32_t x, int32_t y) { return ((uint32_t)x & 0xffff) | (((uint32_t)y & 0xffff) << 16); }
static uint32_t rgb(int32_t r, int32_t g, int32_t b) {
    if (r > 255) r = 255;
    if (g > 255) g = 255;
    if (b > 255) b = 255;
    if (r < 0) r = 0;
    if (g < 0) g = 0;
    if (b < 0) b = 0;
    return r | (g << 8) | (b << 16);
}

// The draw mode: dithering on, the semi-transparency mode for untextured polygons (1 = B+F).
static void drawMode(unsigned semi) { sendGPUData(0xe1000000 | (1 << 9) | (semi << 5)); }

// A part of a tile - the columns [c0, c1) of the layer - placed by a value: the layer's pivot at (x, y), scaled
// (scaleX * scale across, scale down) and turned about it; dy (1/4 px) moves it down before the turn.
static void drawPart(const struct AbLayer *l, const struct AbTile *t, int c0, int c1, const struct Value *v,
                     int32_t dy, uint32_t color) {
    if (c0 < t->x) c0 = t->x;
    if (c1 > t->x + t->w) c1 = t->x + t->w;
    if (c0 >= c1) return;
    int32_t c = dCos(v->angle & (DC_2PI - 1));  // 1 << 24
    int32_t s = dSin(v->angle & (DC_2PI - 1));
    int32_t sx = (int32_t)(((int64_t)v->scale * v->scaleX) >> 12);  // 4096 = 1.0
    int32_t sy = v->scale;
    int32_t xs[2] = {c0 * 4 - l->pivotX, c1 * 4 - l->pivotX};  // 1/4 texel from the pivot
    int32_t ys[2] = {t->y * 4 - l->pivotY + dy, (t->y + t->h) * 4 - l->pivotY + dy};
    uint32_t vx[4];
    for (int i = 0; i < 4; i++) {
        int64_t lx = (int64_t)xs[i & 1] * sx;  // 1/4 px << 12
        int64_t ly = (int64_t)ys[i >> 1] * sy;
        int32_t px = v->x + (int32_t)((lx * c - ly * s) >> 36);
        int32_t py = v->y + (int32_t)((lx * s + ly * c) >> 36);
        vx[i] = vertex((px + 2) >> 2, (py + 2) >> 2);
    }
    unsigned u0 = t->u + (c0 - t->x), u1 = t->u + (c1 - t->x);
    unsigned v0 = t->v, v1 = t->v + t->h;
    if (u1 > 255) u1 = 255;
    if (v1 > 255) v1 = 255;
    unsigned add = l->blend != 0;
    uint32_t tpage = t->tpage | (add ? (1 << 5) : 0);
    sendGPUData((add ? 0x2e000000 : 0x2c000000) | color);
    sendGPUData(vx[0]);
    sendGPUData((t->clut << 16) | (v0 << 8) | u0);
    sendGPUData(vx[1]);
    sendGPUData((tpage << 16) | (v0 << 8) | u1);
    sendGPUData(vx[2]);
    sendGPUData((v1 << 8) | u0);
    sendGPUData(vx[3]);
    sendGPUData((v1 << 8) | u1);
}

// The GPU multiplies a texel by colour / 128: the designer's "tint * bright" (tint 0..255, bright 256 = 1.0).
static uint32_t layerColor(const struct AbLayer *l, const struct Value *v, int32_t bright) {
    if (!l->blend) {  // an opaque picture: only darkened
        int32_t c = bright >= 256 ? 128 : (bright * 128) >> 8;
        return rgb(c, c, c);
    }
    return rgb((v->r * bright) >> 9, (v->g * bright) >> 9, (v->b * bright) >> 9);
}

static void drawLayer(const struct AbTrack *tracks, unsigned count, unsigned index, const struct Value *v) {
    const struct AbTrack *tr = &tracks[index];
    const struct AbLayer *l = &g_abLayers[tr->layer];
    if (v->bright <= 0 || v->scale <= 0) return;
    // the letters' hop: the letter tracks follow the layer's own
    const struct AbTrack *letters = 0;
    if (l->letterCount && index + 1 < count && tracks[index + 1].kind == AB_KIND_LETTER) letters = &tracks[index + 1];
    for (unsigned i = 0; i < l->tileCount; i++) {
        const struct AbTile *t = &g_abTiles[l->firstTile + i];
        if (!letters) {
            drawPart(l, t, t->x, t->x + t->w, v, 0, layerColor(l, v, v->bright));
            continue;
        }
        for (unsigned j = 0; j < l->letterCount; j++) {
            const struct AbLetter *le = &g_abLetters[l->firstLetter + j];
            struct Value w;
            valueAt(&letters[j], &w);
            int32_t bright = v->bright + w.bright;
            if (bright <= 0) continue;
            // a hop is in screen pixels: undo the scale so it stays the size the designer gave
            int32_t dy = (w.y << 12) / v->scale;  // a hop is a few px: no 64-bit division (no libgcc here)
            drawPart(l, t, le->x0, le->x1, v, dy, layerColor(l, v, bright));
        }
    }
}

static void drawBackground(const struct AbTrack *centreTrack, const struct AbTrack *edgeTrack) {
    struct Value c, e;
    valueAt(centreTrack, &c);
    valueAt(edgeTrack, &e);
    if (!(c.r | c.g | c.b | e.r | e.g | e.b)) return;
    const struct AbScene *sc = &g_abScene;
    uint32_t cc = rgb(c.r, c.g, c.b), ec = rgb(e.r, e.g, e.b);
    drawMode(0);
    unsigned n = sc->bgSegments;
    for (unsigned i = 0; i < n; i++) {
        unsigned a0 = DC_2PI * i / n, a1 = DC_2PI * (i + 1) / n;
        int32_t x0 = sc->bgX + (int32_t)(((int64_t)sc->bgRX * dCos(a0)) >> 24);
        int32_t y0 = sc->bgY + (int32_t)(((int64_t)sc->bgRY * dSin(a0)) >> 24);
        int32_t x1 = sc->bgX + (int32_t)(((int64_t)sc->bgRX * dCos(a1)) >> 24);
        int32_t y1 = sc->bgY + (int32_t)(((int64_t)sc->bgRY * dSin(a1)) >> 24);
        sendGPUData(0x30000000 | cc);  // gouraud triangle, opaque
        sendGPUData(vertex(sc->bgX, sc->bgY));
        sendGPUData(ec);
        sendGPUData(vertex(x0, y0));
        sendGPUData(ec);
        sendGPUData(vertex(x1, y1));
    }
}

// Soft rays turning behind the logo: each a pair of additive gouraud triangles, lit on its middle line from the
// centre out, black at its sides and its tip (the designer's profile: 0.075 of the tint at the centre).
static void drawRays(const struct Value *v) {
    if (v->bright <= 0) return;
    const struct AbScene *sc = &g_abScene;
    int32_t k = (v->bright * 77) >> 10;  // 0.075 * bright, 256 = 1.0
    uint32_t lit = rgb((v->r * k) >> 8, (v->g * k) >> 8, (v->b * k) >> 8);
    if (!lit) return;
    int32_t cx = v->x >> 2, cy = v->y >> 2;
    int32_t rx = sc->bgRX * 5 / 4, ry = sc->bgRY * 5 / 4;
    unsigned per = DC_2PI / sc->rays;
    unsigned half = per * 22 / 100;
    drawMode(1);
    for (unsigned i = 0; i < sc->rays; i++) {
        unsigned a = (unsigned)(v->angle + i * per) & (DC_2PI - 1);
        unsigned as[3] = {a - half, a, a + half};
        int32_t px[3], py[3];
        for (int j = 0; j < 3; j++) {
            unsigned aj = as[j] & (DC_2PI - 1);
            px[j] = cx + (int32_t)(((int64_t)rx * dCos(aj)) >> 24);
            py[j] = cy + (int32_t)(((int64_t)ry * dSin(aj)) >> 24);
        }
        for (int side = 0; side < 2; side++) {
            sendGPUData(0x32000000 | lit);  // gouraud triangle, semi-transparent
            sendGPUData(vertex(cx, cy));
            sendGPUData(0);
            sendGPUData(vertex(px[1], py[1]));
            sendGPUData(0);
            sendGPUData(vertex(px[side * 2], py[side * 2]));
        }
    }
}

// OpenBIOS's own cube, small and turning, beside "powered by OpenBIOS": flat quads, the faces that look at us,
// back to front, lit by how much they do.
static void drawCube(const struct Value *v) {
    if (v->bright <= 0) return;
    const struct AbScene *sc = &g_abScene;
    int32_t ca = dCos(v->angle & (DC_2PI - 1)), sa = dSin(v->angle & (DC_2PI - 1));
    int32_t cb = dCos(sc->cubeTilt), sb = dSin(sc->cubeTilt);
    int32_t vx[8], vy[8];
    for (int i = 0; i < 8; i++) {
        int32_t x = (i & 4) ? 1 : -1, y = (i & 2) ? 1 : -1, z = (i & 1) ? 1 : -1;
        // turn about y, then tilt about x (1 << 24 fixed point)
        int64_t x1 = (int64_t)x * ca + (int64_t)z * sa;
        int64_t z1 = -(int64_t)x * sa + (int64_t)z * ca;
        int64_t y1 = (int64_t)y * cb * (1 << 24) / (1 << 24) - ((z1 * sb) >> 24);
        vx[i] = (v->x >> 2) + (int32_t)((x1 * sc->cubeHalf) >> 26);
        vy[i] = (v->y >> 2) + (int32_t)((y1 * sc->cubeHalf) >> 26);
    }
    static const uint8_t faces[6][4] = {{0, 1, 3, 2}, {4, 5, 7, 6}, {0, 1, 5, 4},
                                        {2, 3, 7, 6}, {0, 2, 6, 4}, {1, 3, 7, 5}};
    static const int8_t normals[6][3] = {{-1, 0, 0}, {1, 0, 0}, {0, -1, 0}, {0, 1, 0}, {0, 0, -1}, {0, 0, 1}};
    int32_t nz[6];
    unsigned order[6], count = 0;
    for (unsigned f = 0; f < 6; f++) {
        int32_t x = normals[f][0], y = normals[f][1], z = normals[f][2];
        int64_t z1 = -(int64_t)x * sa + (int64_t)z * ca;
        int64_t z2 = (int64_t)y * sb + ((z1 * cb) >> 24);  // 1 << 24
        nz[f] = (int32_t)z2;
        if (nz[f] > 0) order[count++] = f;
    }
    for (unsigned i = 0; i + 1 < count; i++) {
        for (unsigned j = 0; j + 1 < count - i; j++) {
            if (nz[order[j]] > nz[order[j + 1]]) {
                unsigned t = order[j];
                order[j] = order[j + 1];
                order[j + 1] = t;
            }
        }
    }
    drawMode(0);
    for (unsigned i = 0; i < count; i++) {
        unsigned f = order[i];
        int32_t lum = 90 + ((166 * (nz[f] >> 16)) >> 8);  // (0.35 + 0.65 nz), 256 = 1.0
        int32_t k = (lum * v->bright) >> 8;
        sendGPUData(0x28000000 | rgb((v->r * k) >> 8, (v->g * k) >> 8, (v->b * k) >> 8));
        sendGPUData(vertex(vx[faces[f][0]], vy[faces[f][0]]));
        sendGPUData(vertex(vx[faces[f][1]], vy[faces[f][1]]));
        sendGPUData(vertex(vx[faces[f][3]], vy[faces[f][3]]));
        sendGPUData(vertex(vx[faces[f][2]], vy[faces[f][2]]));
    }
}

void abSplashRender() {
    const struct AbTimeline *tl = &g_abTimelines[s_state];
    const struct AbTrack *centre = 0;
    for (unsigned i = 0; i < tl->trackCount; i++) {
        const struct AbTrack *tr = &tl->tracks[i];
        struct Value v;
        switch (tr->kind) {
            case AB_KIND_BG_CENTRE:
                centre = tr;
                break;
            case AB_KIND_BG_EDGE:
                if (centre) drawBackground(centre, tr);
                break;
            case AB_KIND_RAYS:
                valueAt(tr, &v);
                drawRays(&v);
                break;
            case AB_KIND_LAYER:
                valueAt(tr, &v);
                drawLayer(tl->tracks, tl->trackCount, i, &v);
                break;
            case AB_KIND_CUBE:
                valueAt(tr, &v);
                drawCube(&v);
                break;
            default:
                break;
        }
    }
}
