#!/bin/sh
# Host tests for the Vice City modules that do not depend on the game.
# Usage: sh run.sh                                  (g++ with ASan + UBSan)
#        CXX=clang++ SAN= sh run.sh                 (another compiler, no sanitizers)
#        VC_REPO=<checkout of samp-vice-city> sh run.sh
#                                                   (also runs the tests over the real models and textures)
#        ML_TEST=<text> sh run.sh                   (only the cases whose name contains the text)
D="$(cd "$(dirname "$0")" && pwd)"
M="$D/../../modloader"
OUT="${TMPDIR:-/tmp}/vctests.$$"
CXX="${CXX:-g++}"
SAN="${SAN--fsanitize=address,undefined -fno-sanitize-recover=undefined}"
SRC=""
for m in VcArchive VcCollision VcConfig VcMap VcMapData.gen VcStreamer VcTextures; do
    SRC="$SRC $D/../$m.cpp"
done
# The modloader modules the map is built on, and its test harness.
for m in ImgFile ImgOverlay ModIndex TxdConvert; do
    SRC="$SRC $M/$m.cpp"
done
# shellcheck disable=SC2086
$CXX -std=c++20 -O1 -g -Wall -Wextra $SAN -fno-omit-frame-pointer -pthread \
    "$D"/test_*.cpp "$M/tests/test_main.cpp" $SRC -o "$OUT" || exit 2
"$OUT"
rc=$?
rm -f "$OUT"
exit $rc
