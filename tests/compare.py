#!/usr/bin/env python3
"""compare.py RASTER PBMDIR [REPEAT] - check that every page the simulated printer
received is pixel-identical to the (1-bit) CUPS raster input."""
import glob
import os
import struct
import sys


def raster_pages(path):
    d = open(path, "rb").read()
    e = "<" if d[:4] in (b"3SaR", b"tSaR") else ">"
    if d[:4] in (b"2SaR", b"RaS2"):
        raise SystemExit("compare.py only handles uncompressed raster")
    pos = 4
    while pos < len(d):
        h = d[pos:pos + 1796]
        u = lambda o: struct.unpack(e + "I", h[o:o + 4])[0]
        w, hgt, bpl, ps0 = u(372), u(376), u(392), u(352)
        pos += 1796
        yield w, hgt, bpl, ps0, d[pos:pos + bpl * hgt]
        pos += bpl * hgt


def read_pbm(path):
    d = open(path, "rb").read()
    parts = d.split(b"\n", 2)
    w, h = map(int, parts[1].split())
    return w, h, parts[2]


def main():
    ras, pbmdir = sys.argv[1], sys.argv[2]
    pbms = sorted(glob.glob(os.path.join(pbmdir, "page*.pbm")))
    repeat = int(sys.argv[3]) if len(sys.argv) > 3 else 1
    pages = list(raster_pages(ras)) * repeat
    if len(pages) != len(pbms):
        print(f"FAIL: {len(pages)} raster pages, {len(pbms)} printed pages")
        return 1
    ok = True
    for i, ((w, h, bpl, ps0, data), pbm) in enumerate(zip(pages, pbms), 1):
        pw, ph, img = read_pbm(pbm)
        ls = pw // 8
        src = (w + 7) // 8
        dst_off = (ls - src) // 2 if src < ls else 0
        src_off = (src - ls) // 2 if src > ls else 0
        copy = min(src, ls)
        bad = 0
        black = 0
        for y in range(h):
            row = bytearray(data[y * bpl:y * bpl + src])
            if w & 7:
                row[-1] &= (0xFF << (8 - (w & 7))) & 0xFF
            exp = bytearray(ls)
            exp[dst_off:dst_off + copy] = row[src_off:src_off + copy]
            got = img[y * ls:(y + 1) * ls]
            if bytes(exp) != got:
                bad += 1
            black += sum(bin(b).count("1") for b in got)
        status = "OK" if bad == 0 and ph == h else "FAIL"
        ok &= status == "OK"
        print(f"page {i}: {status} ({pw}x{ph}, {bad} differing lines, {black} black pixels)")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
