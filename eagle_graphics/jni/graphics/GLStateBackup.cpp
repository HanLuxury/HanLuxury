#include "GLStateBackup.h"

namespace gfx {
namespace {
inline void Toggle(GLenum cap, GLboolean on) {
    if (on) glEnable(cap);
    else glDisable(cap);
}
} // namespace

void GLStateBackup::Save(uint32_t groups, uint32_t unitMask) {
    m_groups = groups;
    m_units = unitMask & 0xFFFFu;

    if (groups & kFramebuffer) {
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &m_drawFbo);
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &m_readFbo);
    }
    if (groups & kViewport) {
        glGetIntegerv(GL_VIEWPORT, m_viewport);
        glGetIntegerv(GL_SCISSOR_BOX, m_scissorBox);
        m_scissor = glIsEnabled(GL_SCISSOR_TEST);
    }
    if (groups & kProgram) glGetIntegerv(GL_CURRENT_PROGRAM, &m_program);
    if (groups & kVertexInput) {
        glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &m_vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &m_arrayBuffer);
    }
    if (groups & kTextures) {
        glGetIntegerv(GL_ACTIVE_TEXTURE, &m_activeTexture);
        for (int i = 0; i < 16; ++i) {
            if (!(m_units & (1u << i))) continue;
            glActiveTexture(GL_TEXTURE0 + i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D, &m_texture2D[i]);
            glGetIntegerv(GL_SAMPLER_BINDING, &m_sampler[i]);
        }
        glActiveTexture(static_cast<GLenum>(m_activeTexture));
    }
    if (groups & kDepth) {
        m_depthTest = glIsEnabled(GL_DEPTH_TEST);
        glGetIntegerv(GL_DEPTH_FUNC, &m_depthFunc);
        glGetBooleanv(GL_DEPTH_WRITEMASK, &m_depthMask);
        glGetFloatv(GL_DEPTH_RANGE, m_depthRange);
        glGetFloatv(GL_DEPTH_CLEAR_VALUE, &m_clearDepth);
    }
    if (groups & kBlend) {
        m_blend = glIsEnabled(GL_BLEND);
        glGetIntegerv(GL_BLEND_SRC_RGB, &m_blendSrcRGB);
        glGetIntegerv(GL_BLEND_DST_RGB, &m_blendDstRGB);
        glGetIntegerv(GL_BLEND_SRC_ALPHA, &m_blendSrcA);
        glGetIntegerv(GL_BLEND_DST_ALPHA, &m_blendDstA);
        glGetIntegerv(GL_BLEND_EQUATION_RGB, &m_blendEqRGB);
        glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &m_blendEqA);
        glGetBooleanv(GL_COLOR_WRITEMASK, m_colorMask);
        glGetFloatv(GL_COLOR_CLEAR_VALUE, m_clearColor);
    }
    if (groups & kRaster) {
        m_cull = glIsEnabled(GL_CULL_FACE);
        glGetIntegerv(GL_CULL_FACE_MODE, &m_cullMode);
        glGetIntegerv(GL_FRONT_FACE, &m_frontFace);
        m_polygonOffset = glIsEnabled(GL_POLYGON_OFFSET_FILL);
        glGetFloatv(GL_POLYGON_OFFSET_FACTOR, &m_offsetFactor);
        glGetFloatv(GL_POLYGON_OFFSET_UNITS, &m_offsetUnits);
        m_stencil = glIsEnabled(GL_STENCIL_TEST);
        m_dither = glIsEnabled(GL_DITHER);
        m_sampleCoverage = glIsEnabled(GL_SAMPLE_COVERAGE);
        m_alphaToCoverage = glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE);
        m_rasterDiscard = glIsEnabled(GL_RASTERIZER_DISCARD);
    }
    if (groups & kPixelStore) {
        glGetIntegerv(GL_UNPACK_ALIGNMENT, &m_unpackAlignment);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING, &m_unpackBuffer);
    }
    m_saved = true;
}

void GLStateBackup::Restore() {
    if (!m_saved) return;
    const uint32_t groups = m_groups;

    if (groups & kPixelStore) {
        glPixelStorei(GL_UNPACK_ALIGNMENT, m_unpackAlignment);
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER, static_cast<GLuint>(m_unpackBuffer));
    }
    if (groups & kRaster) {
        Toggle(GL_CULL_FACE, m_cull);
        glCullFace(static_cast<GLenum>(m_cullMode));
        glFrontFace(static_cast<GLenum>(m_frontFace));
        Toggle(GL_POLYGON_OFFSET_FILL, m_polygonOffset);
        glPolygonOffset(m_offsetFactor, m_offsetUnits);
        Toggle(GL_STENCIL_TEST, m_stencil);
        Toggle(GL_DITHER, m_dither);
        Toggle(GL_SAMPLE_COVERAGE, m_sampleCoverage);
        Toggle(GL_SAMPLE_ALPHA_TO_COVERAGE, m_alphaToCoverage);
        Toggle(GL_RASTERIZER_DISCARD, m_rasterDiscard);
    }
    if (groups & kBlend) {
        Toggle(GL_BLEND, m_blend);
        glBlendFuncSeparate(static_cast<GLenum>(m_blendSrcRGB), static_cast<GLenum>(m_blendDstRGB),
                            static_cast<GLenum>(m_blendSrcA), static_cast<GLenum>(m_blendDstA));
        glBlendEquationSeparate(static_cast<GLenum>(m_blendEqRGB), static_cast<GLenum>(m_blendEqA));
        glColorMask(m_colorMask[0], m_colorMask[1], m_colorMask[2], m_colorMask[3]);
        glClearColor(m_clearColor[0], m_clearColor[1], m_clearColor[2], m_clearColor[3]);
    }
    if (groups & kDepth) {
        Toggle(GL_DEPTH_TEST, m_depthTest);
        glDepthFunc(static_cast<GLenum>(m_depthFunc));
        glDepthMask(m_depthMask);
        glDepthRangef(m_depthRange[0], m_depthRange[1]);
        glClearDepthf(m_clearDepth);
    }
    if (groups & kTextures) {
        for (int i = 0; i < 16; ++i) {
            if (!(m_units & (1u << i))) continue;
            glActiveTexture(GL_TEXTURE0 + i);
            glBindTexture(GL_TEXTURE_2D, static_cast<GLuint>(m_texture2D[i]));
            glBindSampler(static_cast<GLuint>(i), static_cast<GLuint>(m_sampler[i]));
        }
        glActiveTexture(static_cast<GLenum>(m_activeTexture));
    }
    if (groups & kVertexInput) {
        glBindVertexArray(static_cast<GLuint>(m_vao));
        glBindBuffer(GL_ARRAY_BUFFER, static_cast<GLuint>(m_arrayBuffer));
    }
    if (groups & kProgram) glUseProgram(static_cast<GLuint>(m_program));
    if (groups & kViewport) {
        glViewport(m_viewport[0], m_viewport[1], m_viewport[2], m_viewport[3]);
        glScissor(m_scissorBox[0], m_scissorBox[1], m_scissorBox[2], m_scissorBox[3]);
        Toggle(GL_SCISSOR_TEST, m_scissor);
    }
    if (groups & kFramebuffer) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, static_cast<GLuint>(m_drawFbo));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, static_cast<GLuint>(m_readFbo));
    }
    m_saved = false;
}

} // namespace gfx
