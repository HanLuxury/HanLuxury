#pragma once
// EAGLE graphics engine - external resource locations (SA_DOX-style layout).
//
//   /storage/emulated/0/TESTLIT/graphics/
//     Config.ini            main settings          (GraphicsConfig)
//     Advanced.ini          expert shadow/sun/debug (GraphicsConfig, overrides Config.ini)
//     shaderUniform.ini     live shader values     (ShaderUniforms, "Grafis" tab)
//     logOutput.log         engine log
//     glShader/             .shader files          (GlShader / ShaderManager)
//     data/eagle_timecyc.dat time-of-day look       (TimeCycleFX)
//     textures/             reserved for PHASE 6 (post-process, LUT)
//
// Every file is optional: a missing file only falls back to the built-in
// default (or disables the feature that needs it).

namespace gfx::paths {

inline constexpr char kRoot[]          = "/storage/emulated/0/TESTLIT/graphics/";
inline constexpr char kConfig[]        = "/storage/emulated/0/TESTLIT/graphics/Config.ini";
inline constexpr char kAdvanced[]      = "/storage/emulated/0/TESTLIT/graphics/Advanced.ini";
inline constexpr char kLegacyConfig[]  = "/storage/emulated/0/TESTLIT/graphics/graphics.ini"; // first release
inline constexpr char kShaderUniform[] = "/storage/emulated/0/TESTLIT/graphics/shaderUniform.ini";
inline constexpr char kLog[]           = "/storage/emulated/0/TESTLIT/graphics/logOutput.log";
inline constexpr char kGlShader[]      = "/storage/emulated/0/TESTLIT/graphics/glShader/";
inline constexpr char kTimecyc[]       = "/storage/emulated/0/TESTLIT/graphics/data/eagle_timecyc.dat";
inline constexpr char kTextures[]      = "/storage/emulated/0/TESTLIT/graphics/textures/";

// glShader/ files used by the engine (paths relative to kGlShader).
inline constexpr char kShaderBuilding[]  = "Entity/Building.shader";
inline constexpr char kShaderVehicle[]   = "Entity/Vehicle.shader";
inline constexpr char kShaderCharacter[] = "Entity/Character.shader";
inline constexpr char kShaderDebugMap[]  = "Debug/ShadowMap.shader";

} // namespace gfx::paths
