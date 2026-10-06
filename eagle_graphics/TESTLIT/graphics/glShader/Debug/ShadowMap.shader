// =====================================================================
//  EAGLE Graphics - glShader/Debug/ShadowMap.shader
//  [debug] showShadowMap = 1: gambar shadow atlas di kiri bawah layar
//  (dekat = gelap, kosong = putih). Program milik engine (GLSL ES 3.00),
//  bukan shader GTA. Bisa dimuat ulang tanpa restart (nativeReloadShaders).
// =====================================================================
<vert>
#version 300 es
layout(location = 0) in vec2 aPosition;
uniform vec4 uRect; // NDC x0, y0, x1, y1
out vec2 vUV;
void main() {
    vUV = aPosition;
    gl_Position = vec4(mix(uRect.xy, uRect.zw, aPosition), 0.0, 1.0);
}
</vert>

<frag>
#version 300 es
precision highp float;
uniform highp sampler2D uDepth;
in vec2 vUV;
out vec4 fragColor;
void main() {
    float d = texture(uDepth, vUV).r;
    float v = d >= 0.9999 ? 1.0 : pow(d, 3.0);
    fragColor = vec4(v, v * 0.97, v * 0.92, 1.0);
}
</frag>
