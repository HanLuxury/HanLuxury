// =====================================================================
//  EAGLE Graphics - glShader/Entity/Character.shader
//  Cara ped/pemain menerima bayangan matahari (shader GTA dengan skinning).
//  MinLight menjaga wajah tetap terbaca di bawah bayangan gedung.
//  Wajib mendefinisikan: void SG_Apply(inout lowp vec4 color)
// =====================================================================
<frag>
#include "realtimeShadow.shader";
#include "Depth/Caster.shader";
#include "Debug/Cascade.shader";

@float Character_ShadowStrength < class = "Character", name = "ShadowStrength", default = 0.85; min = 0.0; max = 1.5; step = 0.05; >
@float Character_SunBoost < class = "Character", name = "SunBoost", default = 1.0; min = 0.0; max = 3.0; step = 0.05; >
@float Character_MinLight < class = "Character", name = "MinLight", default = 0.15; min = 0.0; max = 1.0; step = 0.05; >

void SG_Apply(inout lowp vec4 color) {
    SG_SpecVis = 1.0;
    if (SG_Light.w < 0.0) {
        SG_Caster(color);
        return;
    }
    if (SG_Light.w <= 0.0) return;
    mediump float nl;
    highp float vis = max(SG_SunVisibility(nl), Character_MinLight);
    SG_SpecVis = vis;
    SG_Shade(color, vis, nl, Character_ShadowStrength, Character_SunBoost);
    SG_DebugCascade(color, vis);
}
</frag>
