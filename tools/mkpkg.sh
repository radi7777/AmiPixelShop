#!/bin/sh
# Archiv build/AmiPixelShop.lha und Update-Paket build/AmiPixelShop.upd bauen (nach make).
# Das Update-Paket wird signiert (tools/mkupd.py) und entsteht daher nur dort, wo der
# Signaturschluessel liegt - in GitHub Actions also nur das Archiv.
set -e
pkg=build/pkg/AmiPixelShop
mkdir -p "$pkg/images" "$pkg/ComfyUI"
cp build/AmiPixelShop dist/AmiPixelShop.info LICENSE AmiPixelShop.readme docs/Guide.html "$pkg/"
cp docs/images/*.jpg "$pkg/images/"
cp dist/ComfyUI/* "$pkg/ComfyUI/"
rm -f build/AmiPixelShop.lha build/AmiPixelShop.upd
(cd build/pkg && lha -aq ../AmiPixelShop.lha AmiPixelShop)
if [ -f "$HOME/.amipixelshop/upd_sign.key" ]; then
    python3 tools/mkupd.py build/AmiPixelShop.upd
else
    echo "No signing key (~/.amipixelshop/upd_sign.key): update package skipped"
fi
ls -l build/AmiPixelShop build/AmiPixelShop.lha build/AmiPixelShop.upd 2>/dev/null || true
