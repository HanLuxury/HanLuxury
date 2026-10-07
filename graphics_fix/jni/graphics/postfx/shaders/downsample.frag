#version 300 es
precision highp float;
in highp vec2 vUV;
layout(location=0) out vec4 outColor;
uniform sampler2D uSource;
uniform vec2 uTexel;
uniform int uDecodeSRGB;
uniform int uExtract;
uniform vec2 uThresholdKnee;
vec3 linearRGB(vec3 c) {
    return mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)),
               step(vec3(0.04045), c));
}
vec3 tap(vec2 p) {
    vec3 c = texture(uSource, p).rgb;
    c=uDecodeSRGB != 0 ? linearRGB(c) : c;
    // Extract before filtering so thin lamp highlights are not averaged away.
    if(uExtract!=0) {
        float b=max(c.r,max(c.g,c.b));
        float knee=max(uThresholdKnee.y,0.0001);
        float soft=clamp(b-uThresholdKnee.x+knee,0.0,2.0*knee);
        c*=max(b-uThresholdKnee.x,soft*soft/(4.0*knee))/max(b,0.0001);
    }
    return c;
}
// Karis weight for the first (extract) level: one very bright pixel cannot
// dominate a texel, so small glints/coronas do not pulse as they move.
float karis(vec3 c) { return uExtract!=0 ? 1.0/(1.0+max(c.r,max(c.g,c.b))) : 1.0; }
vec3 sum; float weight;
void add(vec2 p,float k) { vec3 c=tap(p);float w=karis(c)*k;sum+=c*w;weight+=w; }
void main() {
    // 13-tap tent downsample; stabil saat highlight kecil bergerak.
    sum=vec3(0.0);weight=0.0;
    add(vUV,0.125);
    add(vUV+uTexel*vec2(-2,-2),0.03125);add(vUV+uTexel*vec2(2,-2),0.03125);
    add(vUV+uTexel*vec2(-2, 2),0.03125);add(vUV+uTexel*vec2(2, 2),0.03125);
    add(vUV+uTexel*vec2(-2,0),0.0625);add(vUV+uTexel*vec2(2,0),0.0625);
    add(vUV+uTexel*vec2(0,-2),0.0625);add(vUV+uTexel*vec2(0,2),0.0625);
    add(vUV+uTexel*vec2(-1,-1),0.125);add(vUV+uTexel*vec2(1,-1),0.125);
    add(vUV+uTexel*vec2(-1, 1),0.125);add(vUV+uTexel*vec2(1, 1),0.125);
    outColor = vec4(sum/max(weight,1e-5), 1.0);
}
