// EAGLE world shadow receiver (GLSL ES 1.00 fragment snippet).
// Injected by ShaderPatcher into GTA's own world pixel shaders, right before
// "void main()". The engine prepends these defines:
//   SG_HW    1 = sampler2DShadow (GL_EXT_shadow_samplers), 0 = manual compare
//   SG_TAPS  1, 4 or 9 PCF taps
//   SG_BLEND 1 = blend band between cascades
//   SG_LIT   1 = GTA lit shader (peds/vehicles/objects), SG_vDirect available
//   SG_ALPHA 1 = alpha-tested shader (leaves/fences) -> caster alpha cutoff
// Contract: must define  void SG_Apply(inout lowp vec4 color)  and the global
// SG_SpecVis. Keep every statement shorter than ~400 characters (GTA prints
// failing shaders statement-by-statement into a 520-byte stack buffer).
// Comments are stripped before injection. Changes need a game restart
// (GTA compiles its shaders once).
#if SG_HW
uniform mediump sampler2DShadow SG_ShadowMap;
#else
uniform highp sampler2D SG_ShadowMap;
#endif
uniform highp mat4 SG_VP[4];
uniform highp vec4 SG_Tile[4];
uniform highp vec4 SG_Split;
uniform highp vec4 SG_Cfg;
uniform highp vec4 SG_Bias;
uniform highp vec4 SG_DBias;
uniform highp vec4 SG_Texel;
uniform mediump vec4 SG_Light;
uniform mediump vec4 SG_Tint;
uniform mediump vec4 SG_Boost;
varying highp vec3 SG_vWorld;
varying highp float SG_vDepth;
varying mediump vec3 SG_vNormal;
#if SG_LIT
varying lowp vec3 SG_vDirect;
#endif
highp float SG_SpecVis;

// SG_Cfg   : x = 1/atlas width, y = 1/atlas height, z = fade start, w = fade end (view depth, m)
// SG_Bias  : x = normal offset (texels), y = blend band (fraction), z = PCF spread (texels), w = caster alpha cutoff
// SG_Light : xyz = direction to the sun, w > 0 receive strength, 0 = off, < 0 = caster pass

highp float SG_Cmp(highp vec2 uv, highp float z) {
#if SG_HW
    return shadow2DEXT(SG_ShadowMap, vec3(uv, z));
#else
    return step(z, texture2D(SG_ShadowMap, uv).r);
#endif
}

highp float SG_Pcf(highp vec4 tile, highp vec3 c, highp float bias) {
    highp float z = min(c.z - bias, 1.0);
    highp vec2 o = SG_Cfg.xy * SG_Bias.z;
#if SG_TAPS == 1
    return SG_Cmp(clamp(c.xy, tile.xy, tile.zw), z);
#elif SG_TAPS == 4
    highp vec2 h = o * 0.5;
    highp float s = SG_Cmp(clamp(c.xy + vec2(-h.x, -h.y), tile.xy, tile.zw), z);
    s += SG_Cmp(clamp(c.xy + vec2(h.x, -h.y), tile.xy, tile.zw), z);
    s += SG_Cmp(clamp(c.xy + vec2(-h.x, h.y), tile.xy, tile.zw), z);
    s += SG_Cmp(clamp(c.xy + vec2(h.x, h.y), tile.xy, tile.zw), z);
    return s * 0.25;
#else
    highp float s = 0.0;
    for (int y = -1; y <= 1; ++y) {
        for (int x = -1; x <= 1; ++x) {
            s += SG_Cmp(clamp(c.xy + vec2(float(x), float(y)) * o, tile.xy, tile.zw), z);
        }
    }
    return s * (1.0 / 9.0);
#endif
}

highp float SG_C0(mediump vec3 n) { return SG_Pcf(SG_Tile[0], (SG_VP[0] * vec4(SG_vWorld + n * (SG_Bias.x * SG_Texel.x), 1.0)).xyz, SG_DBias.x); }
highp float SG_C1(mediump vec3 n) { return SG_Pcf(SG_Tile[1], (SG_VP[1] * vec4(SG_vWorld + n * (SG_Bias.x * SG_Texel.y), 1.0)).xyz, SG_DBias.y); }
highp float SG_C2(mediump vec3 n) { return SG_Pcf(SG_Tile[2], (SG_VP[2] * vec4(SG_vWorld + n * (SG_Bias.x * SG_Texel.z), 1.0)).xyz, SG_DBias.z); }
highp float SG_C3(mediump vec3 n) { return SG_Pcf(SG_Tile[3], (SG_VP[3] * vec4(SG_vWorld + n * (SG_Bias.x * SG_Texel.w), 1.0)).xyz, SG_DBias.w); }

highp float SG_Visibility(mediump vec3 n) {
    highp float d = SG_vDepth;
    if (d >= SG_Cfg.w) return 1.0;
    highp float v;
    if (d < SG_Split.x) {
        v = SG_C0(n);
#if SG_BLEND
        highp float b = SG_Split.x * SG_Bias.y;
        if (d > SG_Split.x - b) v = mix(v, SG_C1(n), (d - (SG_Split.x - b)) / b);
#endif
    } else if (d < SG_Split.y) {
        v = SG_C1(n);
#if SG_BLEND
        highp float b = SG_Split.y * SG_Bias.y;
        if (d > SG_Split.y - b) v = mix(v, SG_C2(n), (d - (SG_Split.y - b)) / b);
#endif
    } else if (d < SG_Split.z) {
        v = SG_C2(n);
#if SG_BLEND
        highp float b = SG_Split.z * SG_Bias.y;
        if (d > SG_Split.z - b) v = mix(v, SG_C3(n), (d - (SG_Split.z - b)) / b);
#endif
    } else {
        v = SG_C3(n);
    }
    return mix(v, 1.0, smoothstep(SG_Cfg.z, SG_Cfg.w, d));
}

void SG_Apply(inout lowp vec4 color) {
    SG_SpecVis = 1.0;
    if (SG_Light.w < 0.0) {
#if SG_ALPHA
        if (color.a < SG_Bias.w) discard;
#endif
        return;
    }
    if (SG_Light.w <= 0.0) return;
    mediump vec3 n = SG_vNormal;
    mediump float nl = 1.0;
    mediump float len2 = dot(n, n);
    if (len2 > 0.01) {
        n *= inversesqrt(len2);
        nl = dot(n, SG_Light.xyz);
    } else {
        n = vec3(0.0);
    }
    highp float vis = min(SG_Visibility(n), smoothstep(-0.03, 0.22, nl));
    SG_SpecVis = vis;
#if SG_LIT
    mediump float share = clamp(dot(SG_vDirect, vec3(0.3333)) / max(dot(Out_Color.rgb, vec3(0.3333)), 0.04), 0.0, 1.0);
#else
    mediump float share = SG_Tint.a;
#endif
    mediump float sh = (1.0 - vis) * SG_Light.w * share;
    color.rgb *= mix(vec3(1.0), SG_Tint.rgb, sh);
    color.rgb *= vec3(1.0) + SG_Boost.rgb * (vis * max(nl, 0.0));
    if (SG_Boost.a > 0.5) {
        highp float d = SG_vDepth;
        mediump vec3 cc = d < SG_Split.x ? vec3(1.0, 0.35, 0.35) : (d < SG_Split.y ? vec3(0.35, 1.0, 0.35) : (d < SG_Split.z ? vec3(0.35, 0.45, 1.0) : vec3(1.0, 1.0, 0.35)));
        color.rgb = mix(color.rgb, cc * (0.35 + 0.65 * vis), 0.5);
    }
}
