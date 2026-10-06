#pragma once
// EAGLE graphics engine - GPU capability detection.
// Must be called on the thread that owns the GL context (the GTA RenderQueue
// thread on 2.10 arm64, see RenderQueueBridge.h).

#include <string>

namespace gfx {

enum class GpuFamily { Unknown, Adreno, Mali, PowerVR, Other };

struct GLCaps {
    bool detected = false;
    std::string vendor, renderer, version, glslVersion;
    int major = 0, minor = 0;
    bool es3 = false, es31 = false, es32 = false;
    GpuFamily family = GpuFamily::Unknown;

    int maxTextureSize = 0;
    int maxTextureUnits = 0;          // fragment
    int maxVertexTextureUnits = 0;
    int maxCombinedTextureUnits = 0;
    int maxRenderbufferSize = 0;
    int maxViewportDims[2] = {0, 0};
    int maxFragmentUniformVectors = 0;
    int maxVertexUniformVectors = 0;
    int maxVaryingVectors = 0;
    int maxDrawBuffers = 0;
    bool fragmentHighp = false;

    bool depthTexture = false;        // core in ES3
    bool shadowSamplers = false;      // GL_EXT_shadow_samplers (ESSL 1.00 sampler2DShadow)
    bool colorBufferHalfFloat = false;
    bool colorBufferFloat = false;
    bool anisotropic = false;
    bool timerQuery = false;          // GL_EXT_disjoint_timer_query

    // Detects once per GL context. Returns false when GL ES < 3.0.
    bool Detect();
    void Invalidate() { detected = false; }
    void Log() const;
    bool HasExtension(const char* name) const;

    static GLCaps& Get();

private:
    std::string m_extensions; // space separated, with leading/trailing space
};

const char* GpuFamilyName(GpuFamily f);

} // namespace gfx
