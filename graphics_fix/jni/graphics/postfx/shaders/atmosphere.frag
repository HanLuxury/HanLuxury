#version 300 es
precision highp float;
precision highp sampler2D;
in highp vec2 vUV;
layout(location=0) out vec4 outColor;
#define FX_TEXTURE texture
#define FX_DEPTH 1
#include "reference_color.glsl"
#include "light_shafts.glsl"
void main() { outColor=vec4(sunEffects(vUV),1); }
