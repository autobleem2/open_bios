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

#pragma once

#include <stdint.h>

#include "common/hardware/gpu.h"

// The AutoBleem boot screen: a gouraud background, light rays, and textured layers (4-bit grey masks drawn
// additively and coloured by the vertex tint, or 8-bit opaque pictures) moved by one timeline per state of the
// shell, plus OpenBIOS's own cube, small. The data (ab_assets.c) is written by ab_mkassets.py from the designer's
// export (ab_assets/layers.json + keyframes.csv); timelines are in 60 fps frames.

enum AbState {
    AB_STATE_INTRO = 0,    // from black, once
    AB_STATE_IDLE = 1,     // waiting for a disc, loops (a ping every loop)
    AB_STATE_SUCCESS = 2,  // the disc is good: the outro, ends in black, then the game boots
    AB_STATE_ERROR = 3,    // no disc / not a PlayStation disc, loops
    AB_STATE_COUNT = 4,
};

enum AbKind {
    AB_KIND_LAYER = 0,
    AB_KIND_LETTER = 1,  // a text layer's letter: y = its hop, bright = added brightness
    AB_KIND_BG_CENTRE = 2,
    AB_KIND_BG_EDGE = 3,
    AB_KIND_RAYS = 4,
    AB_KIND_CUBE = 5,
};

// One rectangle of a layer in VRAM (at most 256x256, inside one texture page).
struct AbTile {
    uint8_t u, v;
    uint16_t w, h;
    uint16_t x, y;   // its top-left corner in the layer
    uint16_t tpage;  // page x/y and depth; the blend bits are or'ed in when drawn
    uint16_t clut;
};

struct AbUpload {
    uint16_t x, y, w, h;  // VRAM rectangle in halfwords
    const uint16_t *data;
};

struct AbLetter {
    uint16_t x0, x1;  // the letter's columns in its layer
};

struct AbLayer {
    uint16_t firstTile;
    uint16_t tileCount;
    uint8_t blend;           // 0 opaque, 2 additive (the GPU's B+F)
    int16_t pivotX, pivotY;  // in the layer, 1/4 texel
    uint16_t firstLetter;
    uint16_t letterCount;
};

struct AbKey {
    uint16_t frame;
    int16_t x, y;       // where the layer's pivot lands, 1/4 px
    uint16_t scale;     // 4096 = 1.0
    uint16_t scaleX;    // extra horizontal scale, 4096 = 1.0
    int16_t angle;      // 2048 = a full turn, clockwise
    int16_t bright;     // 256 = the tint as given
    uint8_t r, g, b;    // the tint
};

struct AbTrack {
    uint8_t kind;    // enum AbKind
    uint8_t layer;
    uint8_t letter;
    uint8_t pad;
    uint16_t keyCount;
    const struct AbKey *keys;
};

struct AbTimeline {
    uint16_t frames;
    uint8_t loop;
    uint8_t pad;
    uint16_t trackCount;
    const struct AbTrack *tracks;
};

struct AbScene {
    int16_t bgX, bgY, bgRX, bgRY;  // the background's ellipse
    uint16_t bgSegments;
    uint16_t rays;
    uint16_t cubeHalf;  // 1/4 px
    uint16_t cubeTilt;  // 2048 = a full turn
};

extern const struct AbUpload g_abUploads[];
extern const unsigned g_abUploadCount;
extern const struct AbTile g_abTiles[];
extern const struct AbLetter g_abLetters[];
extern const struct AbLayer g_abLayers[];
extern const struct AbScene g_abScene;
extern const struct AbTimeline g_abTimelines[AB_STATE_COUNT];

void abSplashInit(unsigned fps);
void abSplashSetState(enum AbState state);
enum AbState abSplashState();
// Advances one frame. Returns 1 once a timeline that does not loop has played to its end, or when a looping one
// starts over (the idle ping).
int abSplashTick();
// The colour the frame is cleared with (the background's edge colour).
union Color abSplashBackground();
void abSplashRender();
