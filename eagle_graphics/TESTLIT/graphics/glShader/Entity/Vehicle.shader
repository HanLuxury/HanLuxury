// =====================================================================
//  EAGLE Graphics - glShader/Entity/Vehicle.shader
//  Cara kendaraan menerima bayangan matahari (shader GTA dengan
//  env-map + specular). Specular (kilap matahari) ikut redup di bayangan.
//  Wajib mendefinisikan: void SG_Apply(inout lowp vec4 color)
// =====================================================================
<frag>
#include "realtimeShadow.shader";
#include "Depth/Caster.shader";
#include "Debug/Cascade.shader";

@float Vehicle_ShadowStrength < class = "Vehicle", name = "ShadowStrength", default = 0.9; min = 0.0; max = 1.5; step = 0.05; >
@float Vehicle_SunBoost < class = "Vehicle", name = "SunBoost", default = 1.0; min = 0.0; max = 3.0; step = 0.05; >
@float Vehicle_SpecularInShadow < class = "Vehicle", name = "SpecularInShadow", default = 0.1; min = 0.0; max = 1.0; step = 0.05; >

void SG_Apply(inout lowp vec4 color) {
    SG_SpecVis = 1.0;
    if (SG_Light.w < 0.0) {
        SG_Caster(color);
        return;
    }
    if (SG_Light.w <= 0.0) return;
    mediump float nl;
    highp float vis = SG_SunVisibility(nl);
    SG_SpecVis = mix(Vehicle_SpecularInShadow, 1.0, vis);
    SG_Shade(color, vis, nl, Vehicle_ShadowStrength, Vehicle_SunBoost);
    SG_DebugCascade(color, vis);
}
</frag>
