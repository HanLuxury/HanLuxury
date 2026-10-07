#include "DebugOverlay.h"
#include "GLStateBackup.h"
#include "GraphicsLog.h"
#include "ShaderManager.h"

#include <algorithm>

namespace gfx::DebugOverlay {
namespace {

constexpr char kTag[] = "Debug";

GLuint g_vao = 0, g_vbo = 0, g_sampler = 0;
GLint g_locRect = -1, g_locTex = -1;
GLuint g_program = 0;
unsigned g_generation = 0;

bool EnsureGpu() {
    ShaderManager& sm = ShaderManager::Get();
    if (g_generation != sm.Generation()) {
        g_program = 0; // reloaded or context lost: the cached handle is stale
        g_generation = sm.Generation();
    }
    if (!g_program) {
        static const AttribBinding bindings[] = {{0, "aPosition"}};
        g_program = sm.GetProgram("debug_shadowmap", "Debug/ShadowMap.shader:vert", "Debug/ShadowMap.shader:frag",
                                  bindings, 1);
        if (!g_program) return false;
        g_locRect = glGetUniformLocation(g_program, "uRect");
        g_locTex = glGetUniformLocation(g_program, "uDepth");
    }
    if (!g_vao) {
        const GLfloat quad[] = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 1.0f};
        glGenVertexArrays(1, &g_vao);
        glGenBuffers(1, &g_vbo);
        glBindVertexArray(g_vao);
        glBindBuffer(GL_ARRAY_BUFFER, g_vbo);
        glBufferData(GL_ARRAY_BUFFER, sizeof(quad), quad, GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, nullptr);
    }
    if (!g_sampler) {
        // Sampler object overrides the atlas' compare mode for a plain depth read.
        glGenSamplers(1, &g_sampler);
        glSamplerParameteri(g_sampler, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glSamplerParameteri(g_sampler, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glSamplerParameteri(g_sampler, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(g_sampler, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glSamplerParameteri(g_sampler, GL_TEXTURE_COMPARE_MODE, GL_NONE);
    }
    return true;
}

} // namespace

void DrawDepthTexture(GLuint texture, int width, int height) {
    if (!texture || width <= 0 || height <= 0) return;
    GLStateBackup state;
    state.Save(GLStateBackup::kAll, 1u << 0);
    if (EnsureGpu()) {
        GLint vp[4];
        glGetIntegerv(GL_VIEWPORT, vp);
        const float screenH = static_cast<float>(std::max(vp[3], 1));
        const float screenW = static_cast<float>(std::max(vp[2], 1));
        const float h = 0.42f * screenH;
        const float w = h * static_cast<float>(width) / static_cast<float>(height);
        // NDC rectangle in the lower-left corner with a small margin.
        const float x0 = -1.0f + 2.0f * 12.0f / screenW, y0 = -1.0f + 2.0f * 12.0f / screenH;
        const float x1 = x0 + 2.0f * w / screenW, y1 = y0 + 2.0f * h / screenH;

        glDisable(GL_DEPTH_TEST);
        glDisable(GL_BLEND);
        glDisable(GL_CULL_FACE);
        glDisable(GL_SCISSOR_TEST);
        glDisable(GL_STENCIL_TEST);
        glDisable(GL_RASTERIZER_DISCARD);
        glDisable(GL_POLYGON_OFFSET_FILL);
        glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
        glDepthMask(GL_FALSE);
        glUseProgram(g_program);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, texture);
        glBindSampler(0, g_sampler);
        glUniform1i(g_locTex, 0);
        glUniform4f(g_locRect, x0, y0, x1, y1);
        glBindVertexArray(g_vao);
        glDrawArrays(GL_TRIANGLE_STRIP, 0, 4);
    } else if (GraphicsLog::Once("debug-overlay")) {
        GFX_LOGW(kTag, "shadow map overlay unavailable (shader failed)");
    }
    state.Restore();
}

void ReleaseGpu() {
    if (g_vao) glDeleteVertexArrays(1, &g_vao);
    if (g_vbo) glDeleteBuffers(1, &g_vbo);
    if (g_sampler) glDeleteSamplers(1, &g_sampler);
    g_vao = g_vbo = g_sampler = 0;
    g_program = 0; // owned by ShaderManager
}

void ForgetGpu() {
    g_vao = g_vbo = g_sampler = 0;
    g_program = 0;
}

} // namespace gfx::DebugOverlay
