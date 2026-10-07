#version 300 es
precision highp float;
precision highp sampler2D;
in highp vec2 vUV;
layout(location=0) out vec4 outColor;
// Temporal anti-aliasing of the world image (before the HUD). GTA does not
// jitter its projection, so this is the stabilising half of TAA: last
// frame's result is reprojected with the depth and both camera matrices,
// clamped to this frame's 3x3 neighbourhood (cars and people that move on
// their own leave no trails) and blended in. Calms crawling edges and
// shimmering foliage/fences while the camera moves. Values stay in the
// scene's own encoding; alpha is passed through.
uniform sampler2D uScene;      // this frame
uniform sampler2D uSource;     // TAA result of the previous frame
uniform sampler2D uDepth;
uniform int uHasDepth;
uniform float uClearDepth;
uniform vec2 uDepthRange;
uniform mat4 uInvProjection;
uniform mat4 uInvView;         // camera -> GTA world
uniform mat4 uPrevViewProj;    // world -> previous frame clip
uniform vec2 uTexel;           // 1 / scene size
uniform vec4 uTaa;             // history weight (0 = reset), near metres, 0, 0
float taaLuma(vec3 c) { return dot(c,vec3(0.299,0.587,0.114)); }
void main() {
    vec4 source=texture(uScene,vUV);
    vec3 current=source.rgb;
    if(uTaa.x<=0.0) { outColor=source;return; }
    vec3 lo=current,hi=current;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        if(x==0 && y==0) continue;
        vec3 c=texture(uScene,vUV+vec2(float(x),float(y))*uTexel).rgb;
        lo=min(lo,c);hi=max(hi,c);
    }
    float raw=texture(uDepth,vUV).r;
    bool sky=uHasDepth==0 || abs(raw-uClearDepth)<0.0000002;
    float z=sky ? 0.0 : (raw-uDepthRange.x)/(uDepthRange.y-uDepthRange.x)*2.0-1.0;
    vec4 view=uInvProjection*vec4(vUV*2.0-1.0,z,1.0);
    vec3 p=view.xyz/(abs(view.w)>1e-7 ? view.w : 1e-7);
    // Sky: direction only (infinitely far), like the motion blur.
    vec4 world=sky ? vec4(normalize(mat3(uInvView)*p),0.0) : uInvView*vec4(p,1.0);
    vec4 clip=uPrevViewProj*world;
    if(clip.w<=1e-4) { outColor=source;return; }
    vec2 previous=clip.xy/clip.w*0.5+0.5;
    // Own car/character moves WITH the camera: keep its screen position.
    float distance=sky ? 1.0e5 : abs(p.z);
    previous=mix(vUV,previous,smoothstep(uTaa.y,uTaa.y*2.5,distance));
    if(any(lessThan(previous,vec2(0.0))) || any(greaterThan(previous,vec2(1.0)))) { outColor=source;return; }
    vec3 history=clamp(texture(uSource,previous).rgb,lo,hi);
    // Resampled history gets softer with speed: trust it less.
    float speed=length((vUV-previous)/uTexel);
    float weight=uTaa.x*(1.0-0.4*smoothstep(2.0,24.0,speed));
    // Luminance weights: one bright flickering pixel does not dominate.
    float wc=(1.0-weight)/(1.0+taaLuma(current));
    float wh=weight/(1.0+taaLuma(history));
    outColor=vec4((current*wc+history*wh)/max(wc+wh,1e-5),source.a);
}
