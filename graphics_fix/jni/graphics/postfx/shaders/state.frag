#version 300 es
precision highp float;
precision highp sampler2D;
in highp vec2 vUV;
layout(location=0) out vec4 outColor;
// One pixel of slowly changing frame state, ping-ponged between two 1x1
// targets: r = auto exposure / 4, g = sqrt(focus distance / 1000 m),
// b = average scene luminance. Read by the composite pass.
uniform sampler2D uScene;
uniform sampler2D uDepth;
uniform sampler2D uSource;   // state of the previous frame
uniform int uHasDepth;
uniform int uDecodeSRGB;
uniform float uClearDepth;
uniform vec2 uDepthRange;
uniform mat4 uInvProjection;
uniform vec4 uAdapt;         // key, min, max, blend (1 = reset)
uniform vec4 uFocus;         // blend (1 = reset), fallback focus metres
#include "reference_color.glsl"
float viewDepthAt(vec2 uv,out bool sky) {
    float raw=texture(uDepth,uv).r;
    sky=abs(raw-uClearDepth)<0.0000002;
    float z=(raw-uDepthRange.x)/(uDepthRange.y-uDepthRange.x)*2.0-1.0;
    vec4 p=uInvProjection*vec4(uv*2.0-1.0,z,1.0);
    return abs(p.z/max(abs(p.w),1e-7));
}
void main() {
    // Log-average luminance of an 8x8 grid, centre weighted (HUD-free world).
    float logSum=0.0,weightSum=0.0;
    for(int y=0;y<8;++y) for(int x=0;x<8;++x) {
        vec2 g=(vec2(float(x),float(y))+0.5)/8.0;
        vec2 uv=mix(vec2(0.08),vec2(0.92),g);
        vec3 c=texture(uScene,uv).rgb;
        if(uDecodeSRGB!=0) c=linearRGB(c);
        vec2 q=uv*2.0-1.0;
        float w=1.0-0.6*clamp(dot(q,q),0.0,1.0);
        logSum+=log(max(luma(c),1e-4))*w;weightSum+=w;
    }
    float average=exp(logSum/max(weightSum,1e-4));
    float exposure=clamp(uAdapt.x/max(average,1e-4),uAdapt.y,uAdapt.z);

    float focus=uFocus.y;
    if(uHasDepth!=0) {
        float sum=0.0,count=0.0;
        for(int i=0;i<5;++i) {
            float a=float(i)*1.2566371;
            vec2 uv=vec2(0.5)+(i==0 ? vec2(0.0) : vec2(cos(a),sin(a))*0.035);
            bool sky;float d=viewDepthAt(uv,sky);
            if(!sky) { sum+=d;count+=1.0; }
        }
        focus=count>0.0 ? sum/count : 400.0;
    }
    vec4 previous=texture(uSource,vec2(0.5));
    float oldExposure=previous.r*4.0;
    float oldFocus=previous.g*previous.g*1000.0;
    float e=mix(oldExposure,exposure,clamp(uAdapt.w,0.0,1.0));
    float f=mix(oldFocus,focus,clamp(uFocus.x,0.0,1.0));
    outColor=vec4(clamp(e*0.25,0.0,1.0),sqrt(clamp(f/1000.0,0.0,1.0)),clamp(average,0.0,1.0),1.0);
}
