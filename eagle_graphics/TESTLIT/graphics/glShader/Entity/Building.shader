// =====================================================================
//  EAGLE Graphics - glShader/Entity/Building.shader
//  Cara gedung, jalan, pohon dan objek dunia menerima bayangan matahari.
//  Dipakai untuk semua shader dunia GTA yang bukan kendaraan dan bukan ped.
//  Wajib mendefinisikan: void SG_Apply(inout lowp vec4 color)
// =====================================================================
<frag>
#include "realtimeShadow.shader";
#include "Depth/Caster.shader";
#include "Debug/Cascade.shader";

@float Building_ShadowStrength < class = "Building", name = "ShadowStrength", default = 1.0; min = 0.0; max = 1.5; step = 0.05; >
@float Building_SunBoost < class = "Building", name = "SunBoost", default = 1.0; min = 0.0; max = 3.0; step = 0.05; >

void SG_Apply(inout lowp vec4 color) {
    SG_SpecVis = 1.0;
    if (SG_Light.w < 0.0) {
        SG_Caster(color);
        return;
    }
    if (SG_Light.w <= 0.0) return;
    mediump float nl;
    highp float vis = SG_SunVisibility(nl);
    SG_SpecVis = vis;
    SG_Shade(color, vis, nl, Building_ShadowStrength, Building_SunBoost);
    SG_DebugCascade(color, vis);
}
</frag>
