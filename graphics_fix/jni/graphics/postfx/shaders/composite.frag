#version 300 es
precision highp float;
precision highp sampler2D;
in highp vec2 vUV;
layout(location=0) out vec4 outColor;
uniform sampler2D uScene;
uniform sampler2D uBloom0;
uniform sampler2D uBloom1;
uniform sampler2D uBloom2;
uniform sampler2D uEffects;
uniform sampler2D uDepth;
uniform sampler2D uDirt;
uniform sampler2D uAtmosphere;
uniform int uHasDepth;
uniform int uDecodeSRGB;
uniform int uEncodeSRGB;
uniform vec4 uGrade; // exposure, saturation, contrast, toneMix.
uniform vec2 uBloomDirt;
uniform vec2 uEffectSize;
uniform vec2 uDepthRange;
uniform mat4 uInvProjection;
uniform int uLightCount;
uniform vec4 uLightPosition[4]; // uv, radius, intensity.
uniform vec4 uLightColor[4]; // RGB, pulse amplitude.
uniform float uTime;
uniform float uAspect;
uniform vec4 uDetail,uFog;
uniform vec3 uShadowTint,uHighlightTint;
uniform vec2 uTexel;
uniform float uClearDepth;
// World-space data; all zero (effects off) when the camera is unknown.
uniform mat4 uInvView;      // camera -> GTA world (Z up)
uniform vec4 uSunDirection; // world direction to the sun, w = visibility
uniform vec4 uSkyZenith;    // rgb, w = sky tint strength
uniform vec4 uSkyHorizon;   // rgb, w = horizon haze
uniform vec4 uSunColor;     // rgb, w = sun glow strength
uniform vec4 uWet;          // wetness, rain, puddle coverage, reflection boost
uniform vec2 uDepthTexel;   // 1 / depth texture size
uniform int uFXAA;          // 1 = FXAA on the world image (HUD is drawn later)
uniform sampler2D uState;   // 1x1: auto exposure / 4, sqrt(focus / 1000 m), average luminance
uniform sampler2D uDofTex;  // half-resolution blurred scene (depth of field)
uniform sampler2D uLUT;     // colour lookup strip (size*size x size), display space
uniform vec4 uHaze;         // density per metre, start metres, max opacity, height falloff
uniform vec4 uHazeBase;     // base height, sun scattering, horizon colour share, uniform density
uniform vec4 uAutoExposure; // strength, 0, 0, 0
uniform vec4 uDof;          // strength, 0, 0, 0
uniform vec4 uMotion;       // strength, max length (uv), near fade metres, enabled
uniform mat4 uPrevViewProj; // world -> previous frame clip
uniform vec4 uLut;          // strength, size, 0, enabled
uniform vec4 uFinish;       // film grain, dither, grading strength, 0
#include "reference_color.glsl"
float viewDepth(vec2 uv) {
    float z=(texture(uDepth,uv).r-uDepthRange.x)/(uDepthRange.y-uDepthRange.x)*2.0-1.0;
    vec4 p=uInvProjection*vec4(uv*2.0-1.0,z,1);
    return abs(p.z/max(abs(p.w),1e-7));
}
vec3 viewPosition(vec2 uv) {
    float z=(texture(uDepth,uv).r-uDepthRange.x)/(uDepthRange.y-uDepthRange.x)*2.0-1.0;
    vec4 p=uInvProjection*vec4(uv*2.0-1.0,z,1.0);
    return p.xyz/(abs(p.w)>1e-7 ? p.w : (p.w<0.0 ? -1e-7 : 1e-7));
}
vec3 worldDirection(vec2 uv) {
    vec4 p=uInvProjection*vec4(uv*2.0-1.0,0.0,1.0);
    return normalize(mat3(uInvView)*(p.xyz/(abs(p.w)>1e-7 ? p.w : 1e-7)));
}
vec3 viewNormal(vec2 uv,vec3 p) {
    vec3 r=viewPosition(uv+vec2(uDepthTexel.x,0.0))-p, l=p-viewPosition(uv-vec2(uDepthTexel.x,0.0));
    vec3 t=viewPosition(uv+vec2(0.0,uDepthTexel.y))-p, b=p-viewPosition(uv-vec2(0.0,uDepthTexel.y));
    vec3 n=cross(dot(r,r)<dot(l,l) ? r : l,dot(t,t)<dot(b,b) ? t : b);
    if(dot(n,n)<1e-14) return vec3(0.0);
    n=normalize(n);
    return dot(n,-p)<0.0 ? -n : n;
}
float hash12(vec2 p) {
    vec3 q=fract(vec3(p.xyx)*0.1031);
    q+=dot(q,q.yzx+33.33);
    return fract((q.x+q.y)*q.z);
}
float valueNoise(vec2 p) {
    vec2 i=floor(p),f=fract(p);
    vec2 u=f*f*(3.0-2.0*f);
    return mix(mix(hash12(i),hash12(i+vec2(1,0)),u.x),mix(hash12(i+vec2(0,1)),hash12(i+vec2(1,1)),u.x),u.y);
}
// Rain drop rings on standing water, anchored to world XY (stable when the camera moves).
float ripple(vec2 p,float t) {
    vec2 g=p*1.7;vec2 cell=floor(g);vec2 f=fract(g)-0.5;
    float h=hash12(cell);
    vec2 o=vec2(hash12(cell+3.1),hash12(cell+7.7))-0.5;
    float phase=fract(t*0.9+h);
    float d=length(f-o*0.6)-phase*0.5;
    return exp(-d*d*576.0)*(1.0-phase);
}
vec4 effects() {
    if(uHasDepth==0) return vec4(0,0,0,1);
    // Depth-aware 4-tap upsample untuk mengurangi halo half-resolution.
    float center=viewDepth(vUV);
    vec2 base=floor(vUV*uEffectSize-0.5);
    vec2 fraction=fract(vUV*uEffectSize-0.5);
    vec4 sum=vec4(0);float total=0.0;
    for(int y=0;y<2;++y) for(int x=0;x<2;++x) {
        vec2 uv=(base+vec2(x,y)+0.5)/uEffectSize;
        vec2 w=mix(1.0-fraction,fraction,vec2(x,y));
        float weight=w.x*w.y*exp(-abs(viewDepth(uv)-center)/max(0.05,center*0.01));
        sum+=texture(uEffects,uv)*weight;total+=weight;
    }
    return total>1e-5 ? sum/total : vec4(0,0,0,1);
}
vec3 scene(vec2 uv) {
    vec3 c=texture(uScene,uv).rgb;return uDecodeSRGB!=0 ? linearRGB(c) : c;
}
// FXAA (Lottes, console-style): 5 taps, smooths stair-step edges and thin
// shimmering geometry without blurring flat areas.
vec3 fxaaScene(vec2 uv) {
    vec3 m=scene(uv);
    vec3 nw=scene(uv+vec2(-1.0,-1.0)*uTexel), ne=scene(uv+vec2(1.0,-1.0)*uTexel);
    vec3 sw=scene(uv+vec2(-1.0,1.0)*uTexel), se=scene(uv+vec2(1.0,1.0)*uTexel);
    float lm=luma(m),lnw=luma(nw),lne=luma(ne),lsw=luma(sw),lse=luma(se);
    float lmin=min(lm,min(min(lnw,lne),min(lsw,lse)));
    float lmax=max(lm,max(max(lnw,lne),max(lsw,lse)));
    if(lmax-lmin<max(0.0312,lmax*0.125)) return m;
    vec2 dir=vec2(-((lnw+lne)-(lsw+lse)),(lnw+lsw)-(lne+lse));
    float reduce=max((lnw+lne+lsw+lse)*0.03125,1.0/128.0);
    dir=clamp(dir/(min(abs(dir.x),abs(dir.y))+reduce),vec2(-8.0),vec2(8.0))*uTexel;
    vec3 a=0.5*(scene(uv+dir*(1.0/3.0-0.5))+scene(uv+dir*(2.0/3.0-0.5)));
    vec3 b=a*0.5+0.25*(scene(uv-dir*0.5)+scene(uv+dir*0.5));
    float lb=luma(b);
    return (lb<lmin||lb>lmax) ? a : b;
}
// Wet asphalt and puddles on flat ground. Returns the reflection multiplier.
float wetSurface(inout vec3 c) {
    vec3 p=viewPosition(vUV);
    float distance=length(p);
    vec3 n=mat3(uInvView)*viewNormal(vUV,p);
    float flatness=smoothstep(0.80,0.95,n.z)*(1.0-smoothstep(60.0,140.0,distance));
    if(flatness<=0.0) return 1.0;
    vec3 world=(uInvView*vec4(p,1.0)).xyz;
    float noise=valueNoise(world.xy*0.18)*0.65+valueNoise(world.xy*0.55+17.3)*0.35;
    float puddle=smoothstep(1.0-uWet.z,1.0-uWet.z+0.12,noise)*uWet.x*flatness;
    float damp=uWet.x*flatness;
    c*=mix(1.0,0.72,damp);   // wet asphalt is darker
    c=mix(c,c*0.55,puddle);  // standing water hides the albedo
    vec3 toEye=normalize(mat3(uInvView)*(-p));
    float fresnel=0.02+0.98*pow(1.0-clamp(toEye.z,0.0,1.0),5.0);
    vec3 skyColour=mix(uSkyHorizon.rgb,uSkyZenith.rgb,0.3);
    float rings=uWet.y>0.01 ? ripple(world.xy,uTime)+ripple(world.xy+vec2(0.37,0.71),uTime*1.13) : 0.0;
    c+=skyColour*(fresnel*puddle*0.6+rings*puddle*uWet.y*0.25);
    return 1.0+uWet.w*puddle+0.5*damp;
}
// Realtime sky: GTA's own sky/clouds keep their brightness structure, the
// hue follows the time/weather model, plus forward scattering around the sun.
vec3 skyGrade(vec3 c) {
    vec3 d=worldDirection(vUV);
    float h=d.z;
    vec3 model=mix(uSkyHorizon.rgb,uSkyZenith.rgb,smoothstep(-0.02,0.45,h));
    float l=luma(c);
    c=mix(c,model*(l/max(luma(model),1e-4)),uSkyZenith.w);
    c=mix(c,uSkyHorizon.rgb*(l/max(luma(uSkyHorizon.rgb),1e-4)),uSkyHorizon.w*(1.0-smoothstep(0.0,0.35,h)));
    float mu=max(dot(d,uSunDirection.xyz),0.0);
    c+=uSunColor.rgb*(pow(mu,48.0)*0.55+pow(mu,6.0)*0.18)*uSunColor.w;
    return c;
}
// Camera motion blur by reprojection into the previous frame. Pixels close to
// the camera (own car/character, which move WITH the camera) stay sharp.
vec3 motionBlur(vec3 c,bool geometry,float distance) {
    vec4 world=geometry ? uInvView*vec4(viewPosition(vUV),1.0) : vec4(worldDirection(vUV),0.0);
    vec4 clip=uPrevViewProj*world;
    if(clip.w<=1e-4) return c;
    vec2 velocity=(vUV-(clip.xy/clip.w*0.5+0.5))*uMotion.x;
    float len=length(velocity);
    if(len>uMotion.y) velocity*=uMotion.y/len;
    if(geometry) velocity*=smoothstep(uMotion.z,uMotion.z*2.5,distance);
    if(length(velocity/uTexel)<1.0) return c;
    vec3 sum=c;
    for(int i=1;i<8;++i) sum+=scene(vUV-velocity*(float(i)/7.0));
    return sum*0.125;
}
// Aerial haze: thicker near the ground, lit by the sun when looking into it.
vec3 haze(vec3 c,float distance) {
    vec3 p=viewPosition(vUV);
    vec3 world=(uInvView*vec4(p,1.0)).xyz;
    vec3 eye=uInvView[3].xyz;
    float height=0.5*(world.z+eye.z)-uHazeBase.x;
    float density=uHaze.x*exp(-max(height,0.0)*uHaze.w)+uHazeBase.w;
    float amount=min(1.0-exp(-max(distance-uHaze.y,0.0)*density),uHaze.z);
    float mu=max(dot(normalize(world-eye),uSunDirection.xyz),0.0);
    vec3 colour=mix(uFog.rgb,uSkyHorizon.rgb,uHazeBase.z)
        +uSunColor.rgb*(pow(mu,6.0)*0.45+pow(mu,28.0)*0.85)*uHazeBase.y*uSunDirection.w;
    return mix(c,colour,amount);
}
vec3 applyLut(vec3 g) {
    float n=uLut.y;
    g=clamp(g,0.0,1.0);
    float b=g.b*(n-1.0);float b0=floor(b);float b1=min(b0+1.0,n-1.0);
    float x=g.r*(n-1.0)+0.5;float y=(g.g*(n-1.0)+0.5)/n;
    vec3 l0=texture(uLUT,vec2((b0*n+x)/(n*n),y)).rgb;
    vec3 l1=texture(uLUT,vec2((b1*n+x)/(n*n),y)).rgb;
    return mix(g,mix(l0,l1,b-b0),uLut.x);
}
void main() {
    vec4 src=texture(uScene,vUV);
    vec3 c=uFXAA!=0 ? fxaaScene(vUV) : (uDecodeSRGB!=0 ? linearRGB(src.rgb) : src.rgb);
    vec3 local=(scene(vUV+vec2(uTexel.x,0))+scene(vUV-vec2(uTexel.x,0))
        +scene(vUV+vec2(0,uTexel.y))+scene(vUV-vec2(0,uTexel.y)))*0.25;
    c=max(c+clamp((luma(c)-luma(local))*uDetail.x,-0.035,0.035),vec3(0));
    vec4 fx=effects();
    bool geometry=uHasDepth!=0 && abs(texture(uDepth,vUV).r-uClearDepth)>0.0000002;
    float depthMetres=geometry ? viewDepth(vUV) : 1.0e5;
    vec4 state=texture(uState,vec2(0.5));
    if(uMotion.w>0.5) c=motionBlur(c,geometry,depthMetres);
    if(uDof.x>0.0 && uHasDepth!=0) {
        float focus=state.g*state.g*1000.0;
        float start=focus*1.3+4.0;
        float coc=uDof.x*smoothstep(start,start+max(focus*2.0,25.0),depthMetres);
        c=mix(c,texture(uDofTex,vUV).rgb,coc);
    }
    float reflection=geometry && uWet.x>0.01 ? wetSurface(c) : 1.0;
    c=c*fx.a+fx.rgb*reflection;
    if(geometry) {
        float distance=max(depthMetres-8.0,0.0);
        c=mix(c,uFog.rgb,clamp(1.0-exp(-distance*uFog.a),0.0,0.78));
        if(uHaze.x>0.0) c=haze(c,depthMetres);
    } else if(uHasDepth!=0 && uSkyZenith.w+uSkyHorizon.w+uSunColor.w>0.0) {
        c=skyGrade(c);
    }
    vec3 bloom=texture(uBloom0,vUV).rgb*0.50+texture(uBloom1,vUV).rgb*0.30+texture(uBloom2,vUV).rgb*0.20;
    c+=bloom*uBloomDirt.x*(1.0+texture(uDirt,vUV).r*uBloomDirt.y);
    c+=texture(uAtmosphere,vUV).rgb;
    for(int i=0;i<4;++i) {
        if(i>=uLightCount) break;
        vec2 d=(vUV-uLightPosition[i].xy)*vec2(uAspect,1.0);
        float r=max(uLightPosition[i].z,0.001);
        float glow=exp(-dot(d,d)/(r*r));
        float pulse=1.0+uLightColor[i].a*sin(uTime*0.6+float(i)*1.7);
        c+=uLightColor[i].rgb*(uLightPosition[i].w*glow*pulse);
    }
    c*=mix(1.0,state.r*4.0,uAutoExposure.x);
    vec3 graded=referenceGrade(c,uGrade,uDetail,uShadowTint,uHighlightTint,vUV);
    c=mix(min(c,vec3(1.0)),graded,uFinish.z);
    // Display space for the LUT, grain and dither (sRGB surfaces encode later).
    vec3 display=gammaRGB(c);
    if(uLut.w>0.5) display=applyLut(display);
    float noise=hash12(gl_FragCoord.xy+fract(uTime*7.31)*vec2(127.1,311.7));
    display+=(noise-0.5)*(uFinish.y/255.0+uFinish.x*0.08*(1.0-luma(display)));
    display=clamp(display,0.0,1.0);
    outColor=vec4(uEncodeSRGB!=0 ? display : linearRGB(display),src.a);
}
