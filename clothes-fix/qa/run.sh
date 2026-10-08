#!/usr/bin/env bash
# Host checks for the EAGLE clothes fix (no phone, no NDK).
# Usage: bash qa/run.sh <fixed package root: has jni/ gamemodes/ cefui/ PED_IMPORT/ sql/> <TESTLIT dir>
# Needs g++ and python3. Optional: Pillow-free; node + playwright for the CEF flow test.
set -euo pipefail
PKG=$(cd "$1" && pwd); TL=$(cd "$2" && pwd); QA=$(cd "$(dirname "$0")" && pwd)
OUT=$(mktemp -d); trap 'rm -rf "$OUT"' EXIT
CXX="g++ -std=c++20 -O1 -DVER_x32=false -I$QA/shim -I$PKG/jni -I$PKG/jni/game/RW -I$PKG/jni/game -I$PKG/jni/vendor -I$PKG/jni/vendor/imgui"
echo "== PNG loader (ASan/UBSan)"
$CXX -fsanitize=address,undefined -o "$OUT/tex" "$QA/tex_test.cpp" "$PKG/jni/game/character/CharacterTexture.cpp" "$QA/stbimpl.cpp"
ASAN_OPTIONS=detect_leaks=0 "$OUT/tex" "$TL/character/textures" "$TL/character.json" | tail -2
echo "== catalog + appearance packet"
$CXX -fsanitize=address,undefined -o "$OUT/cat" "$QA/cat_test.cpp" "$PKG/jni/game/character/CharacterCatalog.cpp" "$PKG/jni/game/character/CharacterTypes.cpp" "$PKG/jni/game/clothes/ClothesNetwork.cpp"
ASAN_OPTIONS=detect_leaks=0 "$OUT/cat" "$TL"
echo "== retarget onto carriers 158/298"
$CXX -o "$OUT/retarget" "$QA/retarget_test.cpp" "$PKG/jni/game/character/CharacterRetarget.cpp"
mkdir -p "$OUT/skel"
for f in $(find "$TL" -name '*.dff'); do python3 -I "$QA/skel_extract.py" "$f" > "$OUT/skel/$(basename "$f" .dff).txt"; done
for c in cwmofr cat; do python3 -I "$QA/skel_extract.py" "$PKG/PED_IMPORT/DFF/$c.dff" > "$OUT/$c.txt"; "$OUT/retarget" "$OUT/$c.txt" "$OUT"/skel/*.txt | tail -1; done
echo "== client / server / SQL catalogs"
python3 -I "$QA/xcheck.py" "$PKG" "$TL"
if command -v node >/dev/null && node -e "require('playwright')" 2>/dev/null; then
  echo "== CEF creator / shop / wardrobe in Chromium"
  $CXX -o "$OUT/uijson" "$QA/uijson.cpp" "$PKG/jni/game/character/CharacterCatalog.cpp" "$PKG/jni/game/character/CharacterTypes.cpp"
  "$OUT/uijson" "$TL" > "$OUT/native_out.txt"
  node "$QA/cef/flow_test.cjs" "$PKG/cefui" "$OUT/native_out.txt" "$PKG/gamemodes/SERVER/player/character/character_catalog.inc"
else
  echo "== CEF flow test skipped (needs node + playwright)"
fi
