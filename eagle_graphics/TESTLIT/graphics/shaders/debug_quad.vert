#version 300 es
// debug_quad.vert - screen-space rectangle for debug overlays.
layout(location = 0) in vec2 aPosition;
uniform vec4 uRect; // NDC x0, y0, x1, y1
out vec2 vUV;
void main() {
    vUV = aPosition;
    gl_Position = vec4(mix(uRect.xy, uRect.zw, aPosition), 0.0, 1.0);
}
