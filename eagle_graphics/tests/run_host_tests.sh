#!/bin/bash
# Host tests for the EAGLE graphics files (Linux/macOS, needs clang++ and
# glslangValidator; GLES3/KHR + android/log.h headers via GFX_SYSINC).
# Run after editing TESTLIT/graphics/glShader/*.shader before copying them to
# the phone:   GFX_SYSINC=/path/to/headers tests/run_host_tests.sh
set -e
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="${OUT:-$(mktemp -d)}"
CXX="${CXX:-clang++}"
SYSINC="${GFX_SYSINC:-/usr/include}"
G="$ROOT/jni/graphics"

"$CXX" -std=c++20 -O1 -Wall -Wextra -Wno-unused-parameter -I"$G" -I"$ROOT/tests" -isystem "$SYSINC" \
  "$ROOT/tests/glshader_test.cpp" "$ROOT/tests/gl_stubs.cpp" \
  "$G/GlShader.cpp" "$G/ShaderUniforms.cpp" "$G/ShaderManager.cpp" "$G/ShaderPatcher.cpp" "$G/GLCaps.cpp" \
  "$G/GraphicsLog.cpp" "$G/GraphicsConfig.cpp" "$G/TimeCycleFX.cpp" -o "$OUT/glshader_test"

"$OUT/glshader_test" "$ROOT" "$OUT"

fail=0
count=0
for f in "$OUT"/*.vert "$OUT"/*.frag; do
  count=$((count + 1))
  if ! glslangValidator "$f" > "$OUT/glslang.log" 2>&1; then
    echo "glslang FAIL: $f"; cat "$OUT/glslang.log"; fail=1
  fi
done
# link check of every patched pair (vertex varyings vs fragment varyings)
for v in "$OUT"/*_hw*_t*_b*.vert; do
  p="${v%.vert}.frag"
  if ! glslangValidator -l "$v" "$p" > "$OUT/glslang.log" 2>&1; then
    echo "glslang LINK FAIL: $v"; cat "$OUT/glslang.log"; fail=1
  fi
done
echo "glslang: $count shaders checked, output in $OUT"
exit $fail
