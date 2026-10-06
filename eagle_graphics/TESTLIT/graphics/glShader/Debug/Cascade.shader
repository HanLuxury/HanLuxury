// =====================================================================
//  EAGLE Graphics - glShader/Debug/Cascade.shader
//  [debug] showCascade = 1 (Advanced.ini atau tab Grafis): warnai penerima
//  per cascade - merah / hijau / biru / kuning.
// =====================================================================
<frag>
void SG_DebugCascade(inout lowp vec4 color, highp float vis) {
    if (SG_Boost.a > 0.5) {
        highp float d = SG_vDepth;
        mediump vec3 cc = d < SG_Split.x ? vec3(1.0, 0.35, 0.35) : (d < SG_Split.y ? vec3(0.35, 1.0, 0.35) : (d < SG_Split.z ? vec3(0.35, 0.45, 1.0) : vec3(1.0, 1.0, 0.35)));
        color.rgb = mix(color.rgb, cc * (0.35 + 0.65 * vis), 0.5);
    }
}
</frag>
