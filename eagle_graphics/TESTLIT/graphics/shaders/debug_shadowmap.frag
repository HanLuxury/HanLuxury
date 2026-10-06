#version 300 es
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
