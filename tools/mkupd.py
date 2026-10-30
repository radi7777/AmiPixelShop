#!/usr/bin/env python3
"""Update-Paket AmiPixelShop.upd fuer den eingebauten Updater (src/update.c) bauen.

Aufruf: tools/mkupd.py <ausgabe.upd>   (im Projektverzeichnis, nach make)
Enthaelt Programm, Guide, Bilder, LICENSE und Readme - nicht das Icon, damit
eigene Icon-Positionen und Tooltypes erhalten bleiben.

Ab 0.7.1 wird das Paket mit Ed25519 signiert: hinter "END\n" folgt
"SIG <128 Hex-Zeichen>\n" ueber alle Bytes davor. Aeltere Versionen lesen nur
bis END und ignorieren die Signatur. Der geheime Schluessel liegt nur auf dem
Mac in ~/.amipixelshop/upd_sign.key (nie ins Repo, nie in die Dropbox).
"""
import glob, os, sys, zlib

files = [("AmiPixelShop", "build/AmiPixelShop"), ("Guide.html", "docs/Guide.html"),
         ("LICENSE", "LICENSE"), ("AmiPixelShop.readme", "AmiPixelShop.readme")]
files += [("images/" + os.path.basename(p), p) for p in sorted(glob.glob("docs/images/*.jpg"))]

out = bytearray(b"APSUPD1\n")
for name, src in files:
    data = open(src, "rb").read()
    comp = zlib.compress(data, 9)
    out += b"F %s %d %d %08x\n" % (name.encode(), len(data), len(comp), zlib.crc32(data) & 0xFFFFFFFF)
    out += comp
out += b"END\n"

keyfile = os.path.expanduser("~/.amipixelshop/upd_sign.key")
try:
    import nacl.signing
    key = nacl.signing.SigningKey(bytes.fromhex(open(keyfile).read().strip()))
except Exception as e:
    sys.exit("Signieren nicht moeglich (%s): %s" % (keyfile, e))
out += b"SIG " + key.sign(bytes(out)).signature.hex().encode() + b"\n"
open(sys.argv[1], "wb").write(out)
print("%s: %d files, %d bytes" % (sys.argv[1], len(files), len(out)))
