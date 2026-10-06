#pragma once
// EAGLE graphics engine - built-in copies of the external shader files.
// A file in /storage/emulated/0/TESTLIT/graphics/shaders/ always wins; these
// are only used when the file is missing, so the engine never depends on them.

#include "ShaderManager.h"
#include "ShaderPatcher.h"

namespace gfx {

inline constexpr char kDebugQuadVert[] = R"GLSL(#version 300 es
// debug_quad.vert - screen-space rectangle for debug overlays.
layout(location = 0) in vec2 aPosition;
uniform vec4 uRect; // NDC x0, y0, x1, y1
out vec2 vUV;
void main() {
    vUV = aPosition;
    gl_Position = vec4(mix(uRect.xy, uRect.zw, aPosition), 0.0, 1.0);
}
)GLSL";

inline constexpr char kDebugShadowMapFrag[] = R"GLSL(#version 300 es
// debug_shadowmap.frag - visualises the depth atlas (near = dark, far/clear = white).
precision highp float;
uniform highp sampler2D uDepth;
in vec2 vUV;
out vec4 fragColor;
void main() {
    float d = texture(uDepth, vUV).r;
    float v = d >= 0.9999 ? 1.0 : pow(d, 3.0);
    fragColor = vec4(v, v * 0.97, v * 0.92, 1.0);
}
)GLSL";

inline void RegisterEmbeddedShaders(ShaderManager& sm) {
    sm.RegisterEmbedded("debug_quad.vert", kDebugQuadVert);
    sm.RegisterEmbedded("debug_shadowmap.frag", kDebugShadowMapFrag);
    sm.RegisterEmbedded("world_shadow.glsl", ShaderPatcher::EmbeddedSnippet());
}

} // namespace gfx
