#pragma once
// EAGLE graphics engine - external resource locations.
// Every optional file is looked up here; a missing file only disables the
// feature that needs it (or falls back to the embedded default).

namespace gfx::paths {

inline constexpr char kRoot[]     = "/storage/emulated/0/TESTLIT/graphics/";
inline constexpr char kConfig[]   = "/storage/emulated/0/TESTLIT/graphics/graphics.ini";
inline constexpr char kLog[]      = "/storage/emulated/0/TESTLIT/graphics/graphics.log";
inline constexpr char kShaders[]  = "/storage/emulated/0/TESTLIT/graphics/shaders/";
inline constexpr char kTextures[] = "/storage/emulated/0/TESTLIT/graphics/textures/";
inline constexpr char kLut[]      = "/storage/emulated/0/TESTLIT/graphics/lut/";

} // namespace gfx::paths
