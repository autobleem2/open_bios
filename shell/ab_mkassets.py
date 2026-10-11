#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 AutoBleem authors (see shell/ab_splash.h for the full text)
"""Writes shell/ab_assets.c - the AutoBleem boot screen - from the designer's export in shell/ab_assets/:

  layers.json    per layer: file, mode (add = a 4-bit grey mask drawn B+F and coloured by the vertex tint;
                 opaque = an 8-bit palette picture, colour 0 transparent), size, pivot_in_layer, the letter column
                 ranges of a text layer; _bg (the gouraud fan), _cube (OpenBIOS's cube, small)
  keyframes.csv  every frame (60 fps) of every state: where each layer's pivot lands, scale, scale_x, angle,
                 bright, tint, and the procedural parts (_bg, _rays, _tileN, _cube, the letter wave)

The frames become linear segments (a key where the straight line from the previous key would be off by more than
a small tolerance), so the ROM holds keys, not frames. Run: python shell/ab_mkassets.py  (needs Pillow).
"""

import csv
import json
import re
import sys
from collections import defaultdict
from pathlib import Path

from PIL import Image

HERE = Path(__file__).resolve().parent
SRC = HERE / "ab_assets"
OUT = HERE / "ab_assets.c"

# the designer's state names -> the shell's (enum AbState order)
STATES = [("intro", "intro"), ("idle", "idle"), ("boot", "success"), ("error", "error")]
# drawing order, back to front (the designer's simulator order)
ORDER = ["bar", "auto", "bleem", "rim", "inner", "two", "underline", "ring", "spark", "nodisc", "powered"]
KIND_LAYER, KIND_LETTER, KIND_BG_CENTRE, KIND_BG_EDGE, KIND_RAYS, KIND_CUBE = range(6)

# VRAM: the screen is 640x480 at (0, 0). Texture pages right of it: 4 bpp pages are 64 halfwords wide, 8 bpp 128.
# CLUTs in the rows under the screen, clear of the displayed lines.
PAGES_8 = [(640, 0)]
PAGES_4 = [(768, 0), (832, 0), (896, 0), (960, 0), (640, 256), (704, 256), (768, 256), (832, 256), (896, 256),
           (960, 256)]
CLUT_Y0 = 488

# tolerances for turning frames into segments
TOL = dict(x=0.35, y=0.35, scale=0.003, scale_x=0.003, angle=0.25, bright=0.006, r=1.0, g=1.0, b=1.0)


def fail(msg):
    sys.exit(f"ab_mkassets: {msg}")


class Packer:
    def __init__(self, pages):
        self.pages = list(pages)
        self.page = 0
        self.cx = self.cy = self.row_h = 0

    def place(self, w, h):
        while self.page < len(self.pages):
            if self.cx + w > 256:
                self.cx, self.cy, self.row_h = 0, self.cy + self.row_h, 0
            if self.cy + h <= 256:
                u, v = self.cx, self.cy
                self.cx += w
                self.row_h = max(self.row_h, h)
                return self.pages[self.page], u, v
            self.page += 1
            self.cx = self.cy = self.row_h = 0
        fail("the layers do not fit in VRAM")


class Cluts:
    def __init__(self):
        self.x = 0
        self.y = CLUT_Y0

    def place(self, entries):
        if self.x + entries > 640:
            self.x, self.y = 0, self.y + 1
        if self.y >= 512:
            fail("too many CLUTs")
        x, y = self.x, self.y
        self.x += entries
        return x, y


def load_layer(meta):
    im = Image.open(SRC / meta["file"])
    w, h = im.size
    if meta["mode"] == "add":
        g = im.convert("L").load()
        idx = [[min(15, round(g[x, y] / 17)) for x in range(w)] for y in range(h)]
        clut = [0] + [0x8000 | (v * 31 // 15) * 0x421 for v in range(1, 16)]
        return w, h, idx, clut, 4
    p = im.convert("RGBA").load()
    q = im.convert("RGB").quantize(colors=255, dither=Image.Dither.NONE) if im.mode != "P" else im
    pal = q.getpalette()
    qp = q.load()
    idx = [[(qp[x, y] + 1) if p[x, y][3] >= 128 and not (im.mode == "P" and qp[x, y] == 0) else 0
            for x in range(w)] for y in range(h)]
    clut = [0] * 256
    for i in range(min(255, len(pal) // 3)):
        r, gg, b = pal[3 * i: 3 * i + 3]
        clut[i + 1] = 0x8000 | (r >> 3) | ((gg >> 3) << 5) | ((b >> 3) << 10)
    return w, h, idx, clut, 8


def simplify(frames, fields):
    """frames: list of dicts (one per frame, None = not shown). Returns the key frame indices."""
    n = len(frames)
    keys = [0]
    i = 0
    while i < n - 1:
        j = i + 1
        best = j
        while j < n:
            ok = True
            a, b = frames[i], frames[j]
            for k in range(i + 1, j):
                m = frames[k]
                t = (k - i) / (j - i)
                for f in fields:
                    if abs(a[f] + (b[f] - a[f]) * t - m[f]) > TOL[f]:
                        ok = False
                        break
                if not ok:
                    break
            if not ok:
                break
            best = j
            j += 1
        keys.append(best)
        i = best
    return keys


def num(s, default):
    return float(s) if s not in ("", None) else default


def main():
    meta = json.loads((SRC / "layers.json").read_text(encoding="utf-8"))
    names = [n for n in ORDER if n in meta] + ["tile"]
    lindex = {n: i for i, n in enumerate(names)}
    pack4, pack8, cluts = Packer(PAGES_4), Packer(PAGES_8), Cluts()
    uploads, tiles, layers, letters = [], [], [], []
    for n in names:
        m = meta[n]
        w, h, idx, clut, bpp = load_layer(m)
        cx, cy = cluts.place(len(clut))
        uploads.append((cx, cy, len(clut), 1, clut))
        clut_attr = (cx >> 4) | (cy << 6)
        first = len(tiles)
        for ty in range(0, h, 256):
            for tx in range(0, w, 256):
                tw, th = min(256, w - tx), min(256, h - ty)
                per_hw = 4 if bpp == 4 else 2
                pw = (tw + 2 * per_hw - 1) // (2 * per_hw) * (2 * per_hw)  # whole words per row
                (sx, sy), u, v = (pack4 if bpp == 4 else pack8).place(pw, th)
                data = []
                for y in range(th):
                    row = idx[ty + y][tx: tx + tw] + [0] * (pw - tw)
                    if bpp == 4:
                        data += [row[i] | row[i + 1] << 4 | row[i + 2] << 8 | row[i + 3] << 12 for i in range(0, pw, 4)]
                    else:
                        data += [row[i] | row[i + 1] << 8 for i in range(0, pw, 2)]
                uploads.append((sx + u // per_hw, sy + v, pw // per_hw, th, data))
                tpage = (sx // 64) | ((sy // 256) << 4) | ((0 if bpp == 4 else 1) << 7)
                tiles.append((u, v, tw, th, tx, ty, tpage, clut_attr))
        if "letters_x_in_layer" in m and len(tiles) - first != 1:
            fail(f"{n}: a layer with letters must fit in one 256x256 tile")
        first_letter = len(letters)
        for a, b in m.get("letters_x_in_layer", []):
            letters.append((a, b))
        px, py = m["pivot_in_layer"]
        layers.append((first, len(tiles) - first, 2 if m["mode"] == "add" else 0, round(px * 4), round(py * 4),
                       first_letter, len(letters) - first_letter))

    # the frames, per state and track
    rows = list(csv.DictReader(open(SRC / "keyframes.csv", encoding="utf-8")))
    per = defaultdict(lambda: defaultdict(dict))  # state -> track name -> frame -> values
    nframes = defaultdict(int)
    seen = defaultdict(int)
    for r in rows:
        st, fr, name = r["state"], int(r["frame"]), r["layer"]
        nframes[st] = max(nframes[st], fr + 1)
        base = dict(x=num(r["x"], 0), y=num(r["y"], 0), scale=num(r["scale"], 1), scale_x=num(r["scale_x"], 1),
                    angle=num(r["angle_deg"], 0), bright=num(r["bright"], 0), r=num(r["tint_r"], 0),
                    g=num(r["tint_g"], 0), b=num(r["tint_b"], 0))
        if name == "_bg":
            per[st]["_bg"][fr] = base
            e = re.match(r"edge=(\d+)/(\d+)/(\d+)", r["extra"])
            per[st]["_bgedge"][fr] = dict(base, r=float(e[1]), g=float(e[2]), b=float(e[3]))
            continue
        if name.startswith("_tile"):
            per[st][name][fr] = base
            continue
        # a layer with more than one row in a frame (the rings) = one track per instance
        k = (st, fr, name)
        inst = seen[k]
        seen[k] += 1
        tname = name if inst == 0 else f"{name}#{inst}"
        per[st][tname][fr] = base
        m = re.match(r"letters_dy_bright=(.*)", r["extra"] or "")
        if m:
            for li, pair in enumerate(m[1].split(";")):
                dy, db = pair.split("/")
                per[st][f"{name}@{li}"][fr] = dict(base, x=0.0, y=float(dy), bright=float(db), angle=0.0)

    out = ["// Written by shell/ab_mkassets.py from shell/ab_assets/ - do not edit.", '#include "shell/ab_splash.h"', ""]
    for i, (x, y, w, h, data) in enumerate(uploads):
        out.append(f"static const uint16_t s_up{i}[{len(data)}] = {{")
        for j in range(0, len(data), 16):
            out.append("    " + ", ".join(f"0x{d:04x}" for d in data[j: j + 16]) + ",")
        out.append("};")
    out.append("const struct AbUpload g_abUploads[] = {")
    out += [f"    {{{x}, {y}, {w}, {h}, s_up{i}}}," for i, (x, y, w, h, _) in enumerate(uploads)]
    out.append("};")
    out.append(f"const unsigned g_abUploadCount = {len(uploads)};")
    out.append("const struct AbTile g_abTiles[] = {")
    out += ["    {%d, %d, %d, %d, %d, %d, 0x%04x, 0x%04x}," % t for t in tiles]
    out.append("};")
    out.append("const struct AbLetter g_abLetters[] = {")
    out += [f"    {{{a}, {b}}}," for a, b in letters] or ["    {0, 0},"]
    out.append("};")
    out.append("const struct AbLayer g_abLayers[] = {")
    out += ["    {%d, %d, %d, %d, %d, %d, %d}," % l for l in layers]
    out.append("};")

    bg = meta["_bg"]
    cube = meta["_cube"]
    out.append(f"const struct AbScene g_abScene = {{{bg['center'][0]}, {bg['center'][1]}, {bg['radii'][0]}, "
               f"{bg['radii'][1]}, {bg['segments']}, 10, {round(cube['half_size'] * 4)}, "
               f"{round(cube['tilt_x_rad'] * 2048 / 6.283185307)}}};")

    total_keys = 0
    timelines = []
    for st, shell_name in STATES:
        n = nframes[st]
        tracks = per[st]
        emitted = []

        def emit(kind, layer, letter, tname, fields):
            nonlocal total_keys
            fr = tracks[tname]
            frames = []
            for i in range(n):
                v = fr.get(i)
                frames.append(v if v is not None else dict(x=0, y=0, scale=1, scale_x=1, angle=0, bright=0, r=0, g=0, b=0))
            # a full turn more or less between frames is a wrap of the angle: keep it continuous
            for i in range(1, n):
                while frames[i]["angle"] - frames[i - 1]["angle"] > 180:
                    frames[i] = dict(frames[i], angle=frames[i]["angle"] - 360)
                while frames[i]["angle"] - frames[i - 1]["angle"] < -180:
                    frames[i] = dict(frames[i], angle=frames[i]["angle"] + 360)
            keys = simplify(frames, fields)
            total_keys += len(keys)
            cname = f"s_{shell_name}_{len(emitted)}"
            body = []
            for k in keys:
                f = frames[k]
                body.append("{%d, %d, %d, %d, %d, %d, %d, %d, %d, %d}" % (
                    k, round(f["x"] * 4), round(f["y"] * 4), min(65535, round(f["scale"] * 4096)),
                    min(65535, round(f["scale_x"] * 4096)), round(f["angle"] * 2048 / 360),
                    max(-32768, min(32767, round(f["bright"] * 256))), max(0, min(255, round(f["r"]))),
                    max(0, min(255, round(f["g"]))), max(0, min(255, round(f["b"])))))
            out.append(f"static const struct AbKey {cname}[] = {{ " + ", ".join(body) + " };")
            emitted.append((kind, layer, letter, len(keys), cname))

        allf = ["x", "y", "scale", "scale_x", "angle", "bright", "r", "g", "b"]
        emit(KIND_BG_CENTRE, 0, 0, "_bg", ["r", "g", "b"])
        emit(KIND_BG_EDGE, 0, 0, "_bgedge", ["r", "g", "b"])
        emit(KIND_RAYS, 0, 0, "_rays", ["x", "y", "angle", "bright", "r", "g", "b"])
        for t in sorted((t for t in tracks if t.startswith("_tile")), key=lambda s: int(s[5:])):
            emit(KIND_LAYER, lindex["tile"], 0, t, allf)
        for name in ORDER:
            if name not in lindex:
                continue
            for t in sorted(t for t in tracks if t == name or t.startswith(name + "#")):
                emit(KIND_LAYER, lindex[name], 0, t, allf)
            nlet = layers[lindex[name]][6]
            if nlet and any(t.startswith(name + "@") for t in tracks):
                for li in range(nlet):
                    emit(KIND_LETTER, lindex[name], li, f"{name}@{li}", ["y", "bright"])
        emit(KIND_CUBE, 0, 0, "_cube", ["x", "y", "angle", "bright", "r", "g", "b"])
        out.append(f"static const struct AbTrack s_{shell_name}_tracks[] = {{")
        out += [f"    {{{k}, {l}, {li}, 0, {c}, {nm}}}," for k, l, li, c, nm in emitted]
        out.append("};")
        loop = 1 if shell_name in ("idle", "error") else 0
        timelines.append((n, loop, len(emitted), shell_name))
    out.append("const struct AbTimeline g_abTimelines[AB_STATE_COUNT] = {")
    out += [f"    {{{n}, {loop}, 0, {c}, s_{s}_tracks}}," for n, loop, c, s in timelines]
    out.append("};")
    OUT.write_text("\n".join(out) + "\n", encoding="utf-8", newline="\n")
    texels = sum(t[2] * t[3] for t in tiles)
    words = sum(len(u[4]) for u in uploads)
    print(f"ab_mkassets: {len(layers)} layers, {len(tiles)} tiles, {texels} texels ({words * 2} bytes of VRAM data), "
          f"{total_keys} keys ({total_keys * 20} bytes) for {sum(t[0] for t in timelines)} frames")


if __name__ == "__main__":
    main()
