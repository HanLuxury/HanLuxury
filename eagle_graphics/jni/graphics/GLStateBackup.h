#pragma once
// EAGLE graphics engine - save/restore of the GL state touched by the engine.
//
// GTA's RenderQueue caches GL state (current program, bound textures, depth/
// blend switches, selected render target). Every engine callback that runs on
// the RenderQueue thread must leave the GL state exactly as it found it, or
// the RQ cache and the real GL state diverge (HUD/scene corruption).
// Only the groups requested in the mask are queried, to keep glGet* cheap.

#include <GLES3/gl3.h>
#include <cstdint>

namespace gfx {

class GLStateBackup {
public:
    enum Group : uint32_t {
        kFramebuffer = 1u << 0,  // draw/read FBO binding
        kViewport    = 1u << 1,  // viewport + scissor box + scissor enable
        kProgram     = 1u << 2,  // current program
        kVertexInput = 1u << 3,  // VAO + array buffer
        kTextures    = 1u << 4,  // active unit + 2D binding + sampler of the units in 'unitMask'
        kDepth       = 1u << 5,  // depth test/func/mask/range, clear depth
        kBlend       = 1u << 6,  // blend enable/func/equation, colour mask, clear colour
        kRaster      = 1u << 7,  // cull enable/mode, front face, polygon offset, stencil test, dither,
                                 // sample coverage/alpha-to-coverage, rasterizer discard
        kPixelStore  = 1u << 8,  // unpack alignment + unpack buffer
        kAll         = 0x1FFu,
    };

    GLStateBackup() = default;
    GLStateBackup(const GLStateBackup&) = delete;
    GLStateBackup& operator=(const GLStateBackup&) = delete;

    // unitMask: bit i = texture unit i (0..15) is saved when kTextures is requested.
    void Save(uint32_t groups, uint32_t unitMask = 0x1u);
    void Restore();
    bool Saved() const { return m_saved; }

    GLint DrawFramebuffer() const { return m_drawFbo; }
    const GLint* Viewport() const { return m_viewport; }

private:
    bool m_saved = false;
    uint32_t m_groups = 0;
    uint32_t m_units = 0;

    GLint m_drawFbo = 0, m_readFbo = 0;
    GLint m_viewport[4] = {0, 0, 0, 0};
    GLint m_scissorBox[4] = {0, 0, 0, 0};
    GLboolean m_scissor = GL_FALSE;
    GLint m_program = 0;
    GLint m_vao = 0, m_arrayBuffer = 0;
    GLint m_activeTexture = GL_TEXTURE0;
    GLint m_texture2D[16] = {};
    GLint m_sampler[16] = {};
    GLboolean m_depthTest = GL_FALSE, m_depthMask = GL_TRUE;
    GLint m_depthFunc = GL_LESS;
    GLfloat m_depthRange[2] = {0.0f, 1.0f};
    GLfloat m_clearDepth = 1.0f;
    GLboolean m_blend = GL_FALSE;
    GLint m_blendSrcRGB = GL_ONE, m_blendDstRGB = GL_ZERO, m_blendSrcA = GL_ONE, m_blendDstA = GL_ZERO;
    GLint m_blendEqRGB = GL_FUNC_ADD, m_blendEqA = GL_FUNC_ADD;
    GLboolean m_colorMask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLfloat m_clearColor[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    GLboolean m_cull = GL_FALSE;
    GLint m_cullMode = GL_BACK, m_frontFace = GL_CCW;
    GLboolean m_polygonOffset = GL_FALSE;
    GLfloat m_offsetFactor = 0.0f, m_offsetUnits = 0.0f;
    GLboolean m_stencil = GL_FALSE, m_dither = GL_TRUE;
    GLboolean m_sampleCoverage = GL_FALSE, m_alphaToCoverage = GL_FALSE, m_rasterDiscard = GL_FALSE;
    GLint m_unpackAlignment = 4, m_unpackBuffer = 0;
};

} // namespace gfx
