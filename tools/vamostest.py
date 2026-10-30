#!/usr/bin/env python3
"""Rauchtest ohne Amiga: AmiPixelShop unter vamos (amitools) auf einem emulierten
68040 - LAYERTEST (Ebenen speichern, laden, zusammenfuegen) und eine JPEG-Runde
mit CONVERT (PNG -> JPEG -> PNG; der JPEG-Encoder war mit ungeflicktem gcc 16 kaputt).

Aufruf: tools/vamostest.py build/AmiPixelShop <arbeitsverzeichnis>
Braucht: pip install amitools==0.8.1 machine68k==0.3.0

Prueft, dass die PNGs gueltig sind (Chunk-CRCs, zlib-Pruefsumme) und das Bild nach
Speichern/Laden und nach dem Zusammenfuegen pixelgleich ist. Die Grafik-Bibliotheken
werden nur vorgetaeuscht. vamos fehlen FPU-Befehle wie FSIN und FLOGN: FILTERTEST geht
nicht, LAYERTEST ab 0.7 (Levels-Ebene) auch nicht - dann wird es uebersprungen.
"""
import math, os, struct, subprocess, sys, zlib

LIBS = "+".join([
    "intuition.library=mode:fake,version:40",
    "graphics.library=mode:fake,version:40",
    "Picasso96API.library=mode:fake,version:2,num_fake_funcs:200",
    "keymap.library=mode:fake,version:40",
    "datatypes.library=mode:fake,version:45",
    "iffparse.library=mode:fake,version:40",
    "asl.library=mode:fake,version:40",
])


def pixels(path):
    d = open(path, "rb").read()
    assert d[:8] == b"\x89PNG\r\n\x1a\n", path + ": no PNG"
    i, idat = 8, b""
    while i < len(d):
        n, t = struct.unpack(">I4s", d[i:i + 8])
        c = d[i + 8:i + 8 + n]
        assert zlib.crc32(t + c) & 0xFFFFFFFF == struct.unpack(">I", d[i + 8 + n:i + 12 + n])[0], \
            "%s: bad CRC in %s" % (path, t)
        if t == b"IHDR":
            w, h, _, ct = struct.unpack(">IIBB", c[:10])
        elif t == b"IDAT":
            idat += c
        i += 12 + n
    raw = zlib.decompress(idat)         # prueft auch die Adler-32-Summe
    bpp = {0: 1, 2: 3, 4: 2, 6: 4}[ct]
    s, out, prev = w * bpp, bytearray(), bytearray(w * bpp)
    for y in range(h):
        f, r = raw[y * (s + 1)], bytearray(raw[y * (s + 1) + 1:(y + 1) * (s + 1)])
        for x in range(s):
            a, b = r[x - bpp] if x >= bpp else 0, prev[x]
            c = prev[x - bpp] if x >= bpp else 0
            if f == 1:
                r[x] = (r[x] + a) & 255
            elif f == 2:
                r[x] = (r[x] + b) & 255
            elif f == 3:
                r[x] = (r[x] + (a + b) // 2) & 255
            elif f == 4:
                p = a + b - c
                pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                r[x] = (r[x] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
        out += r
        prev = r
    return (w, h), bytes(out)


def write_png(path, w, h, rgb):
    def chunk(t, d):
        return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d) & 0xFFFFFFFF)
    rows = b"".join(b"\0" + rgb[y * w * 3:(y + 1) * w * 3] for y in range(h))
    open(path, "wb").write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                           chunk(b"IDAT", zlib.compress(rows, 6)) + chunk(b"IEND", b""))


def vamos(exe, work, *args):
    # je Aufruf ein eigenes Volume-Verzeichnis: ein abgebrochener Lauf laesst seine
    # RAM:-Platte liegen, und dann startet vamos dort nicht mehr
    vols = os.path.join(work, "vols", str(len(os.listdir(os.path.join(work, "vols")))))
    os.makedirs(vols)
    log = subprocess.run(
        ["vamos", "--vols-base-dir", vols, "-C", "68040", "-H", "disable",
         "-m", "262144", "-s", "512", "-O", LIBS, os.path.abspath(exe)] + list(args),
        cwd=work, capture_output=True, text=True, timeout=1200)
    print(log.stdout.strip())
    return log


def main():
    exe, work = sys.argv[1], os.path.abspath(sys.argv[2])
    os.makedirs(os.path.join(work, "out"), exist_ok=True)
    os.makedirs(os.path.join(work, "vols"), exist_ok=True)
    log = vamos(exe, work, "LAYERTEST", "out")
    if "1 layers after flatten" in log.stdout:
        out = os.path.join(work, "out")
        before = pixels(os.path.join(out, "before.png"))
        for name in ("after.png", "flat.png"):
            assert pixels(os.path.join(out, name)) == before, name + " differs from before.png"
        print("OK: %dx%d, PNGs valid, save/load and flatten pixel-identical" % before[0])
    elif "unimplemented opmode" in log.stdout + log.stderr:
        # ab 0.7 nutzt LAYERTEST Levels (pow -> FLOGN/FETOX), das kann vamos nicht
        print("SKIPPED: LAYERTEST needs an FPU instruction vamos does not emulate")
    else:
        print(log.stderr[-3000:])
        sys.exit("LAYERTEST did not finish")

    # JPEG: weiche Verlaeufe mit etwas Struktur, 256x192
    w, h = 256, 192
    rgb = bytes(int(127 + 100 * math.sin(x / 23.0 + c) * math.cos(y / 17.0 - c) + (x * y + 7 * c) % 13)
                for y in range(h) for x in range(w) for c in range(3))
    write_png(os.path.join(work, "jin.png"), w, h, rgb)
    for a, b in (("jin.png", "jout.jpg"), ("jout.jpg", "jback.png")):
        if vamos(exe, work, "CONVERT", a, b).returncode:
            sys.exit("CONVERT %s %s failed" % (a, b))
    size, back = pixels(os.path.join(work, "jback.png"))
    err = sum(abs(p - q) for p, q in zip(rgb, back)) / len(rgb)
    print("JPEG round trip: %d bytes, mean error %.2f" % (os.path.getsize(os.path.join(work, "jout.jpg")), err))
    assert size == (w, h) and err < 6, "JPEG round trip broken"      # stb q90 here: 3.30
    print("OK: JPEG")


main()
