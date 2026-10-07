#pragma once
// EAGLE graphics engine - debug views drawn before the HUD (GL thread).
// [debug] showShadowMap=1 draws the shadow atlas in the lower-left corner.
// The GL state is saved and restored completely around the draw.

#include <GLES3/gl3.h>

namespace gfx::DebugOverlay {

void DrawDepthTexture(GLuint texture, int width, int height);
void ReleaseGpu();  // delete GL objects (current context)
void ForgetGpu();   // context lost

} // namespace gfx::DebugOverlay
