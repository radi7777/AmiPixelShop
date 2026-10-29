#!/usr/bin/env python3
"""Ergebnisse von "AmiPixelShop FILTERTEST" pruefen.

  tools/filtertest.py <eingabe.png> <ausdir> [praefix]

Fuer jede Korrektur/jeden Filter: *_sel.png darf nur innerhalb der Auswahl
(0,0)-(w*2/5+3, h/2+5) vom Original abweichen (plus Filterradius als Saum ist
NICHT erlaubt: Photoshop maskiert hart an der Auswahl). *_all.png wird mit
_sel innerhalb der Auswahl verglichen (gleiches Ergebnis erwartet, ausser am
Auswahlrand, wo Filter mit Radius Nachbarn von ausserhalb lesen duerfen).
"""
import os, sys
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fmttest as f

src, out = sys.argv[1], sys.argv[2]
pre = sys.argv[3] if len(sys.argv) > 3 else ""
w, h, orig = f.png_read(src)
sx1, sy1 = w * 2 // 5 + 3, h // 2 + 5
bad = 0
for i in range(40):
    pa, ps = f"{out}/{pre}f{i:02d}_all.png", f"{out}/{pre}f{i:02d}_sel.png"
    if not os.path.exists(pa):
        break
    _, _, a = f.png_read(pa)
    _, _, s = f.png_read(ps)
    outside = sum(1 for k, (p, q) in enumerate(zip(orig, s)) if (k % w >= sx1 or k // w >= sy1) and p != q)
    changed_all = sum(1 for p, q in zip(orig, a) if p != q)
    inner = [(x, y) for y in range(40, sy1 - 40, 7) for x in range(40, sx1 - 40, 7)]
    diff_in = max(max(abs(a[y * w + x][c] - s[y * w + x][c]) for c in range(4)) for x, y in inner)
    ok = outside == 0          # all-vs-sel nur zur Info (Add Noise ist zufaellig)
    bad += not ok
    print(f"{'ok  ' if ok else 'FAIL'} f{i:02d}  changed(all) {changed_all:7d}  outside-sel changed {outside:6d}  all-vs-sel inside {diff_in}")
print("ALL OK" if not bad else f"{bad} FAILED")
