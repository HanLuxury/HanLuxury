#pragma once
// Car paint / glass look ([Vehicle] in ZyZGfx.ini) in GTA's OWN shaders.
//
// libGTASA 2.10: RQShader::BuildSource (0x264c4c) writes the generated GLSL
// into static buffers (8193 bytes) and returns pointers to them; every
// caller (shader cache 0x253114, EmuShader::Recompile*, emu_ShaderListCompile)
// hashes and strdup()s them right away, emu_ShaderGetCurSource strcpy()s. The hook
// lets GTA build the source, then INSERTS a few lines into env-mapped shaders
// (vehicles, glass): a Fresnel term on the environment reflection, extra
// specular and a sharp sun glint. Original lines are never removed, so other
// patches that look for them still find them. Missing anchors = untouched.
namespace ShaderTweaks {
struct Vehicle {
    bool enabled = true;
    float fresnel = 0.5f;   // extra reflection at grazing angles (0..1.5)
    float reflection = 1.0f;
    float specular = 0.4f;  // added share of GTA's specular
    float glint = 0.3f;     // sharp sun highlight
    float gloss = 96.0f;    // glint exponent (16..256)
};
// Before the game builds its shaders (settings load at start). Shaders built
// later use the new values; GTA builds each variant once per run.
void SetVehicle(const Vehicle& vehicle);
void InstallHooks();
} // namespace ShaderTweaks
