uniform sampler2D uScene;
uniform vec2 uTexel;
uniform vec4 uSun; // sun UV, shaft strength, flare strength.
uniform float uAspect;
uniform int uDecodeSRGB;
#ifdef FX_DEPTH
uniform sampler2D uDepth;
uniform int uHasDepth;
uniform float uClearDepth;
// 1 on sky pixels. Buildings, trees, vehicles, peds and SA-MP objects write
// depth, so they block the rays instead of glowing themselves.
float skyAt(vec2 uv) {
    if(uHasDepth==0) return 1.0;
    return abs(FX_TEXTURE(uDepth,uv).r-uClearDepth)<0.0000002 ? 1.0 : 0.0;
}
float brightStart() { return uHasDepth!=0 ? 0.45 : 0.65; }
#else
float skyAt(vec2 uv) { return 1.0; }
float brightStart() { return 0.65; }
#endif
vec3 highlightAt(vec2 uv) {
    vec3 c=FX_TEXTURE(uScene,uv).rgb;
    if(uDecodeSRGB!=0) c=linearRGB(c);
    float bright=max(c.r,max(c.g,c.b));
    return c*smoothstep(brightStart(),0.98,bright)*skyAt(uv);
}
vec3 sunEffects(vec2 uv) {
    if(uSun.z+uSun.w<0.0001) return vec3(0);
    // Visibility = share of open, bright sky in a small disc around the sun.
    // Fractional, so the rays fade while the sun slides behind an edge.
    vec2 centre=clamp(uSun.xy,vec2(0.002),vec2(0.998));
    vec3 light=vec3(0);float open=0.0;
    for(int i=0;i<12;++i) {
        float a=float(i)*0.5235988;
        vec2 p=clamp(centre+vec2(cos(a),sin(a))*vec2(1.0/uAspect,1.0)*0.018*(0.5+0.5*mod(float(i),2.0)),vec2(0.001),vec2(0.999));
        light+=highlightAt(p);open+=skyAt(p);
    }
    light/=12.0;open/=12.0;
    float visibility=smoothstep(0.20,0.85,max(light.r,max(light.g,light.b)))*open;
    vec2 delta=(uSun.xy-uv)*(0.88/24.0);
    vec2 p=uv;vec3 rays=vec3(0);float decay=1.0;
    for(int i=0;i<24;++i) {
        p+=delta;rays+=highlightAt(clamp(p,vec2(0.001),vec2(0.999)))*decay;decay*=0.965;
    }
    vec2 sunDelta=(uv-uSun.xy)*vec2(uAspect,1);
    float halo=exp(-dot(sunDelta,sunDelta)/0.025);
    vec2 axis=vec2(0.5)-uSun.xy;
    vec2 ghostDelta=(uv-(vec2(0.5)+axis*0.55))*vec2(uAspect,1);
    float ghost=exp(-dot(ghostDelta,ghostDelta)/0.0018);
    vec3 glow=visibility*(rays*(uSun.z/24.0)+light*uSun.w*halo
        +vec3(0.40,0.28,0.12)*ghost*uSun.w*0.22);
    // Soft limit: added light approaches 1 but never washes the frame white.
    return glow/(1.0+luma(glow));
}
