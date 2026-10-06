#pragma once
namespace WorldSunShadow::Shaders {
// GLES2 fallback: camera depth comes from the draw replay.
inline constexpr char Vertex[]=R"GLSL(#version 100
attribute vec2 aPosition;
varying highp vec2 vUV;
void main() { vUV=aPosition*0.5+0.5;gl_Position=vec4(aPosition,0.0,1.0); }
)GLSL";
inline constexpr char Fragment[]=R"GLSL(#version 100
precision highp float;
varying highp vec2 vUV;
uniform highp sampler2D uCameraDepth;
uniform highp sampler2D uSunDepth;
uniform highp mat4 uInverseCamera;
uniform highp mat4 uLightVP;
uniform highp vec4 uParameters; // texel size, darkness, clear depth, receiver bias
uniform highp vec4 uReceiver;   // texel metres, depth per metre, softness, shadow distance
uniform highp vec4 uCamera;     // camera world position
vec3 positionAt(vec2 uv,float depth) {
    vec4 p=uInverseCamera*vec4(uv*2.0-1.0,depth*2.0-1.0,1.0);
    return p.xyz/p.w;
}
void main() {
    float depth=texture2D(uCameraDepth,vUV).r;
    // Clear camera depth means sky or geometry not replayed: leave it alone.
    if(abs(depth-uParameters.z)<0.00001) { gl_FragColor=vec4(1.0);return; }
    vec3 p=positionAt(vUV,depth);
    float fade=1.0-smoothstep(uReceiver.w*0.8,uReceiver.w,length(p-uCamera.xyz));
    vec4 q=uLightVP*vec4(p,1.0);
    vec3 s=q.xyz/q.w*0.5+0.5;
    if(fade<=0.0||s.z<=0.0||s.z>=1.0||s.x<=0.0||s.x>=1.0||s.y<=0.0||s.y>=1.0) {
        gl_FragColor=vec4(1.0);return;
    }
    float shadow=0.0;
    float k=uParameters.x*uReceiver.z;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x) {
        float blocker=texture2D(uSunDepth,s.xy+vec2(float(x),float(y))*k).r;
        shadow+=step(blocker+uParameters.w,s.z);
    }
    float border=min(min(s.x,s.y),min(1.0-s.x,1.0-s.y));
    shadow*=smoothstep(0.0,0.15,border)*fade*uParameters.y/9.0;
    // Only occluded pixels darken. Slightly cool ambient remains in shadow.
    gl_FragColor=vec4(vec3(1.0)-shadow*vec3(1.0,0.96,0.88),1.0);
}
)GLSL";

// GLES3: depth of the real world target, hardware PCF (sampler2DShadow,
// LINEAR + compare), normal offset and slope-scaled receiver bias.
// No per-pixel noise: the result is identical for identical input, so a
// still camera never shimmers.
inline constexpr char Vertex3[]=R"GLSL(#version 300 es
layout(location=0) in vec2 aPosition;
out highp vec2 vUV;
void main() { vUV=aPosition*0.5+0.5;gl_Position=vec4(aPosition,0.0,1.0); }
)GLSL";
inline constexpr char Fragment3[]=R"GLSL(#version 300 es
precision highp float;
precision highp sampler2D;
precision highp sampler2DShadow;
in highp vec2 vUV;
layout(location=0) out vec4 outColor;
uniform sampler2D uCameraDepth;
uniform sampler2DShadow uSunDepth;
uniform mat4 uInverseCamera;  // NDC -> world
uniform mat4 uLightVP;
uniform vec4 uParameters;     // sun texel (uv), darkness, clear depth, constant bias (depth units)
uniform vec4 uReceiver;       // sun texel in metres, depth units per metre, softness (texels), shadow distance (m)
uniform vec4 uLight;          // world direction to the sun
uniform vec4 uCamera;         // camera world position
uniform vec2 uDepthTexel;     // 1 / size of uCameraDepth

vec3 positionAt(vec2 uv) {
    float depth=texture(uCameraDepth,uv).r;
    vec4 p=uInverseCamera*vec4(uv*2.0-1.0,depth*2.0-1.0,1.0);
    return p.xyz/p.w;
}
vec3 normalAt(vec2 uv,vec3 p) {
    // Pick the flatter side at depth edges so silhouettes keep a usable normal.
    vec3 r=positionAt(uv+vec2(uDepthTexel.x,0.0))-p, l=p-positionAt(uv-vec2(uDepthTexel.x,0.0));
    vec3 t=positionAt(uv+vec2(0.0,uDepthTexel.y))-p, b=p-positionAt(uv-vec2(0.0,uDepthTexel.y));
    vec3 dx=dot(r,r)<dot(l,l) ? r : l;
    vec3 dy=dot(t,t)<dot(b,b) ? t : b;
    vec3 n=cross(dx,dy);
    if(dot(n,n)<1e-14) return vec3(0.0);
    n=normalize(n);
    return dot(n,uCamera.xyz-p)<0.0 ? -n : n;
}
void main() {
    float depth=texture(uCameraDepth,vUV).r;
    if(abs(depth-uParameters.z)<0.000001) { outColor=vec4(1.0);return; }
    vec3 p=positionAt(vUV);
    float distance=length(p-uCamera.xyz);
    float fade=1.0-smoothstep(uReceiver.w*0.8,uReceiver.w,distance);
    if(fade<=0.0) { outColor=vec4(1.0);return; }
    vec3 n=normalAt(vUV,p);
    bool hasNormal=dot(n,n)>0.5;
    float ndl=hasNormal ? dot(n,uLight.xyz) : 0.7;
    float cosine=clamp(abs(ndl),0.08,1.0);
    float tangent=sqrt(1.0-cosine*cosine)/cosine;
    // Normal offset grows at grazing angles, where acne starts.
    vec3 q=p+(hasNormal ? n*uReceiver.x*(0.6+1.2*(1.0-cosine)) : vec3(0.0));
    vec4 h=uLightVP*vec4(q,1.0);
    vec3 s=h.xyz/h.w*0.5+0.5;
    if(any(lessThanEqual(s,vec3(0.0)))||any(greaterThanEqual(s,vec3(1.0)))) { outColor=vec4(1.0);return; }
    float bias=uParameters.w+uReceiver.y*uReceiver.x*0.5*min(tangent,4.0);
    float reference=s.z-bias;
    vec2 k=vec2(uParameters.x*uReceiver.z);
    float lit=0.0;
    for(int y=-1;y<=1;++y) for(int x=-1;x<=1;++x)
        lit+=texture(uSunDepth,vec3(s.xy+vec2(float(x),float(y))*k,reference));
    float shadow=1.0-lit/9.0;
    // Faces turned away from the sun are in their own shadow; this also hides
    // the grazing-angle band where the depth test alone is unreliable.
    if(hasNormal) shadow=max(shadow,(1.0-smoothstep(-0.25,0.05,ndl))*0.85);
    // Wide fade at the map edge: the map follows the camera, so a hard edge
    // would sweep across the ground while turning.
    float border=min(min(s.x,s.y),min(1.0-s.x,1.0-s.y));
    shadow*=smoothstep(0.0,0.15,border)*fade*uParameters.y;
    outColor=vec4(vec3(1.0)-shadow*vec3(1.0,0.96,0.88),1.0);
}
)GLSL";
}
