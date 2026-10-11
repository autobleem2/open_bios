#!/usr/bin/env python3
# MIT License - Copyright (c) 2026 AutoBleem authors (see shell/ab_splash.h for the full text)
"""MOD -> HIT for the shell's modplayer: a Python port of PCSX-Redux's tools/modconv (same sample layout, same
header rewrite, the samples in SPU ADPCM after the pattern data). Usage: python ab_modconv.py in.mod out.hit [amp]
"""

import struct
import sys
from math import gcd

FILTERS = [(0, 0), (60, 0), (115, -52), (98, -55), (122, -60)]
SILENT_LOOP_BLOCK = bytes([0, 7] + [0] * 14)
SPU_MEMORY = 512 * 1024 - 0x1010


class Encoder:
    """SPU ADPCM, 28 samples per 16-byte block: the filter/shift pair with the smallest error, decoded as the SPU
    decodes it so the predictor never drifts."""

    def __init__(self):
        self.reset()

    def reset(self):
        self.s1 = 0
        self.s2 = 0

    def _try(self, pcm, f, shift):
        f0, f1 = FILTERS[f]
        s1, s2 = self.s1, self.s2
        nib = []
        err = 0
        for x in pcm:
            pred = (s1 * f0 + s2 * f1 + 32) >> 6
            # nibble n decodes to (n << 12) >> shift
            r = x - pred
            n = int(round(r * (1 << shift) / 4096.0))
            n = max(-8, min(7, n))
            d = ((n << 12) >> shift) + pred
            d = max(-32768, min(32767, d))
            err += (x - d) * (x - d)
            nib.append(n & 15)
            s2, s1 = s1, d
        return err, nib, s1, s2

    def block(self, pcm, force_filter0=False):
        best = None
        for f in ([0] if force_filter0 else range(5)):
            for shift in range(13):
                err, nib, s1, s2 = self._try(pcm, f, shift)
                if best is None or err < best[0]:
                    best = (err, f, shift, nib, s1, s2)
        _, f, shift, nib, s1, s2 = best
        self.s1, self.s2 = s1, s2
        out = bytearray(16)
        out[0] = (shift & 15) | (f << 4)
        for i in range(14):
            out[2 + i] = nib[2 * i] | (nib[2 * i + 1] << 4)
        return out


def layout(length, loop_start, loop_length, allow_unroll):
    l = dict(has_loop=loop_length > 1, loop_start=0, loop_length=0, skip=2, pad=0, unroll=1, body=0, enc=0)
    if l["has_loop"] and loop_start >= length:
        l["has_loop"] = False
    if l["has_loop"] and loop_start + loop_length > length:
        loop_length = length - loop_start
        l["has_loop"] = loop_length > 1
    if length == 0:
        return l
    if not l["has_loop"]:
        l["enc"] = ((length - 1) * 2 + 27) // 28 * 16 + 16
        return l
    l["loop_start"] = loop_start * 2
    l["loop_length"] = loop_length * 2
    l["skip"] = 0 if loop_start == 0 else 2
    l["pad"] = (28 - (l["loop_start"] - l["skip"]) % 28) % 28
    pre = l["pad"] + l["loop_start"] - l["skip"]
    l["unroll"] = 28 // gcd(l["loop_length"], 28)
    l["body"] = l["loop_length"] * l["unroll"]
    if not allow_unroll or (pre + l["body"]) // 28 * 16 >= 65536:
        l["unroll"] = 1
        l["body"] = max(28, (l["loop_length"] + 14) // 28 * 28)
    l["enc"] = (pre + l["body"]) // 28 * 16
    return l


def convert(src, amp=175):
    hdr = bytearray(src[:1084])
    sig = bytes(hdr[1080:1084])
    if sig in (b"M.K.", b"M!K!"):
        channels = 4
    elif sig[0:1].isdigit() and sig[1:4] == b"CHN":
        channels = sig[0] - 48
    else:
        sys.exit(f"ab_modconv: not a MOD ({sig!r})")
    table = hdr[952:1080]
    patterns = max(table) + 1
    pos = 1084
    pattern_data = src[pos : pos + channels * patterns * 256]
    pos += len(pattern_data)
    samples = []
    for i in range(31):
        o = 20 + 30 * i
        length, ft, vol, ls, ll = struct.unpack(">HBBHH", hdr[o + 22 : o + 30])
        samples.append([o, length, ls, ll])
    allow = sum(layout(s[1], s[2], s[3], True)["enc"] for s in samples) < SPU_MEMORY
    enc = Encoder()
    out_samples = bytearray()
    for o, length, ls, ll in samples:
        enc.reset()
        lay = layout(length, ls, ll, allow)
        raw = src[pos : pos + length * 2]
        pos += length * 2
        if length == 0:
            continue
        pcm_all = [((b ^ 0x80) - 128) * amp for b in raw]  # int8
        start = len(out_samples)
        if not lay["has_loop"]:
            pcm = pcm_all[2:]
            n = len(pcm)
            i = 0
            while n - i >= 28:
                blk = enc.block(pcm[i : i + 28])
                i += 28
                blk[1] = 1 if i == n else 0
                out_samples += blk
            if i < n:
                blk = enc.block(pcm[i:] + [0] * (28 - (n - i)))
                blk[1] = 0
                out_samples += blk
            out_samples += SILENT_LOOP_BLOCK
        else:
            pcm = [0] * lay["pad"] + pcm_all[lay["skip"] : lay["loop_start"]]
            loop_block = len(pcm) // 28
            pcm += [pcm_all[lay["loop_start"] + j % lay["loop_length"]] for j in range(lay["body"])]
            blocks = len(pcm) // 28
            for b in range(blocks):
                blk = enc.block(pcm[b * 28 : b * 28 + 28], force_filter0=(b == loop_block))
                attr = 0
                if b >= loop_block:
                    attr |= 2
                if b == loop_block:
                    attr |= 4
                if b == blocks - 1:
                    attr |= 1
                blk[1] = attr
                out_samples += blk
        enc_len = len(out_samples) - start
        if enc_len >= 65536:
            sys.exit("ab_modconv: sample too big")
        struct.pack_into(">H", hdr, o + 22, enc_len)
        struct.pack_into(">HH", hdr, o + 26, lay["loop_start"] // 2 if lay["has_loop"] else 0,
                         lay["loop_length"] // 2 if lay["has_loop"] else 1)
    if len(out_samples) >= SPU_MEMORY:
        sys.exit("ab_modconv: not enough SPU memory")
    hdr[1080:1084] = b"HIT" + bytes([48 + channels])
    return bytes(hdr) + pattern_data + bytes(out_samples), channels, patterns, len(out_samples)


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    amp = int(sys.argv[3]) if len(sys.argv) > 3 else 175
    data, ch, pat, smp = convert(open(sys.argv[1], "rb").read(), amp)
    open(sys.argv[2], "wb").write(data)
    print(f"ab_modconv: {ch} channels, {pat} patterns, {smp} bytes of SPU samples, {len(data)} bytes written")


if __name__ == "__main__":
    main()
