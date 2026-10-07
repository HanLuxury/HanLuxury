#pragma once
// Anisotropic filtering for GTA's world textures (GRAFIS "Filter anisotropic").
//
// libGTASA 2.10: RQ_Command_rqTextureMipMode (0x265860) binds the texture on
// unit 5 and sets GL_TEXTURE_MIN_FILTER to GL_LINEAR_MIPMAP_LINEAR (0x2703) or
// GL_LINEAR_MIPMAP_NEAREST (0x2701) with glTexParameteri. The hook adds
// GL_TEXTURE_MAX_ANISOTROPY_EXT to that same bound texture right after it, only
// when GL_EXT_texture_filter_anisotropic exists. Nothing else is changed.
namespace TextureFilter {
using HookBackend = void* (*)(void* symbol, void* replacement, void** original);

// Called with the client's ShadowHook backend (EglPostFX::InstallHooks).
bool InstallHooks(HookBackend backend);
// Render thread, current GTA context: reads the extension and its maximum.
void CheckDevice();
// Game thread. level: 0 off, 1 = 2x, 2 = 4x, 3 = 8x, 4 = 16x. Textures that
// already exist are updated on the render thread through the RenderQueue.
void SetLevel(int level);
} // namespace TextureFilter
