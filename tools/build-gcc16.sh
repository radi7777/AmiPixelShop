#!/bin/sh
# Cross-Toolchain mit gcc 16 bauen: bebbo's amiga-gcc, Zweig amiga16.2, dazu
# NDK 3.2, Roadshow-Header (netinclude), vasm, lha, P96-, CyberGraphX- und AmiSSL-SDK.
#
#   sh tools/build-gcc16.sh [PREFIX]      (Vorgabe /opt/amiga16, dauert 30-60 Minuten)
#
# Debian/Ubuntu braucht: make git wget gcc g++ libgmp-dev libmpfr-dev libmpc-dev
# flex bison gettext texinfo libncurses-dev autoconf automake rsync - aber kein lhasa:
# ist schon ein lha da, baut amiga-gcc keines, und lhasa kann keine Archive anlegen
# Danach: make PATH="$PREFIX/bin:$PATH"
set -e
PREFIX=${1:-/opt/amiga16}
WORK=${WORK:-$PWD/build/amiga-gcc}
REPO=https://franke.ms/git/bebbo
AMISSL_URL=https://aminet.net/util/libs/AmiSSL-v5-SDK.lha

# Feste, getestete Staende: bebbo's Zweige aendern sich laufend, und nicht jeder neue
# Stand baut (Anfang November z. B. scheiterte libatomic). Zum Aktualisieren hier die
# Commits und das Datum (spaetestens der Tag des aeltesten Commits) aendern.
SINCE=2026-09-01
pin() {     # pin <verzeichnis> <repo> <zweig> <commit>
    [ -d "$1/.git" ] || git clone -q --shallow-since=$SINCE -b "$3" "$REPO/$2.git" "$1"
    git -C "$1" -c advice.detachedHead=false checkout -q "$4"
}
pin "$WORK" amiga-gcc master d9d8f48674f94294806b30bce4d90e34a5cf666c
cd "$WORK"
cp default-repos .repos
sed -i -E "s#^gcc[[:space:]].*#gcc $REPO/gcc amiga16.2#" .repos
mkdir -p "$PREFIX" projects
pin projects/gcc gcc amiga16.2 8916416e8a63c72cfcbbec88099a8c04bd61165f
pin projects/binutils binutils-gdb amiga-2.46 9c09d7d33c8ecd54cc6b0f49d8c05feb4160ce55
pin projects/libnix libnix master 493152510ab9636af42f86dfd51483c1685ac40d
pin projects/amiga-netinclude amiga-netinclude master 48b0f9ab4cc6d10b2f5c035d556a2169bdbe6875

# gcc flicken, bevor make es baut: in fix_trunc*si2 (float -> int
# auf dem 68040, ueber FPCR) fehlt den Hilfsregistern das early-clobber "&". gcc 16
# (mit und ohne -mlra; gcc 6.5 zufaellig nicht) legt dann das FPCR-Sicherungsregister
# auf ein Register, das die Zieladresse noch braucht - so schreibt z. B. der JPEG-
# Encoder von stb jeden Koeffizienten an die falsche Stelle (unbrauchbare JPEGs, auch
# die an die KI gesendeten Bilder). Gemeldet: https://codeberg.org/bebbo/gcc/issues/19,
# bei bebbo seit Commit 5e493c379 behoben - mit einem neueren gcc-Stand aendert sed nichts.
sed -i -E '/define_insn "fix_trunc[a-z]+[sqh]i2"/,/TUNE_68040/ s/(match_scratch:SI [23]) "=d"/\1 "=\&d"/' \
    projects/gcc/gcc/config/m68k/m68k.md
[ "$(grep -c 'match_scratch:SI [23] "=&d"' projects/gcc/gcc/config/m68k/m68k.md)" -eq 6 ] ||
    { echo "fix_trunc patch did not apply" >&2; exit 1; }

# Wie "make min", aber ohne libnix4.library (laesst sich mit gcc 16 nicht linken, unnoetig).
# WERROR=: libnix baut sonst mit -Werror, und gcc 16 warnt dort (maybe-uninitialized)
make -j"$(nproc)" binutils gcc gprof libnix libgcc ndk netinclude vasm lha sfdc fd2sfd PREFIX="$PREFIX" \
     NDK_URL=https://aminet.net/dev/misc/NDK3.2.lha WERROR=
make sdk=p96 PREFIX="$PREFIX"
make sdk=cgx PREFIX="$PREFIX"            # ab 0.7: CyberGraphX-Ausweichpfad (MorphOS)

export PATH="$PREFIX/bin:$PATH"
# AmiSSL: nur die Header (net.c oeffnet die Bibliotheken selbst)
mkdir -p amissl
wget -q -O amissl/sdk.lha "$AMISSL_URL"
(cd amissl && lha -xqf sdk.lha)
cp -R amissl/AmiSSL/Developer/include/. "$PREFIX/m68k-amigaos/include/"

"$PREFIX/bin/m68k-amigaos-gcc" --version | head -n 1
