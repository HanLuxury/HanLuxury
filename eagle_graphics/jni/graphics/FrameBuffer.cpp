#include "FrameBuffer.h"
#include "GLStateBackup.h"
#include "GraphicsLog.h"

namespace gfx {
namespace {
constexpr char kTag[] = "FrameBuffer";

const char* StatusName(GLenum s) {
    switch (s) {
        case GL_FRAMEBUFFER_COMPLETE: return "COMPLETE";
        case GL_FRAMEBUFFER_INCOMPLETE_ATTACHMENT: return "INCOMPLETE_ATTACHMENT";
        case GL_FRAMEBUFFER_INCOMPLETE_MISSING_ATTACHMENT: return "MISSING_ATTACHMENT";
        case GL_FRAMEBUFFER_INCOMPLETE_DIMENSIONS: return "INCOMPLETE_DIMENSIONS";
        case GL_FRAMEBUFFER_UNSUPPORTED: return "UNSUPPORTED";
        case GL_FRAMEBUFFER_INCOMPLETE_MULTISAMPLE: return "INCOMPLETE_MULTISAMPLE";
        default: return "UNKNOWN";
    }
}
} // namespace

void ConfigureDepthSampling(GLuint texture, bool hardwareCompare) {
    glBindTexture(GL_TEXTURE_2D, texture);
    // Depth formats are only filterable with compare mode on (ES 3.0 table 3.13).
    const GLint filter = hardwareCompare ? GL_LINEAR : GL_NEAREST;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_MODE, hardwareCompare ? GL_COMPARE_REF_TO_TEXTURE : GL_NONE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_COMPARE_FUNC, GL_LEQUAL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
}

bool DepthFrameBuffer::Create(const Desc& desc) {
    Destroy();
    m_desc = desc;
    if (desc.width <= 0 || desc.height <= 0) return false;

    GLStateBackup state;
    state.Save(GLStateBackup::kFramebuffer | GLStateBackup::kTextures, 1u << 0);
    glActiveTexture(GL_TEXTURE0);

    for (int i = 0; i < 8 && glGetError() != GL_NO_ERROR; ++i) {}

    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    const GLenum internal = desc.depth24 ? GL_DEPTH_COMPONENT24 : GL_DEPTH_COMPONENT16;
    glTexStorage2D(GL_TEXTURE_2D, 1, internal, desc.width, desc.height);
    ConfigureDepthSampling(m_texture, desc.hardwareCompare);

    glGenFramebuffers(1, &m_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, m_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_DEPTH_ATTACHMENT, GL_TEXTURE_2D, m_texture, 0);
    const GLenum none = GL_NONE;
    glDrawBuffers(1, &none);
    glReadBuffer(GL_NONE);

    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    const GLenum error = glGetError();
    state.Restore();

    if (status != GL_FRAMEBUFFER_COMPLETE || error != GL_NO_ERROR) {
        GFX_LOGE(kTag, "depth FBO %dx%d (%s) failed: status=%s glError=0x%x", desc.width, desc.height,
                 desc.depth24 ? "D24" : "D16", StatusName(status), error);
        Destroy();
        return false;
    }
    GFX_LOGI(kTag, "depth FBO %dx%d %s compare=%s ready (fbo=%u tex=%u, %.1f MiB)", desc.width, desc.height,
             desc.depth24 ? "D24" : "D16", desc.hardwareCompare ? "hardware" : "manual", m_fbo, m_texture,
             static_cast<double>(desc.width) * desc.height * (desc.depth24 ? 4.0 : 2.0) / (1024.0 * 1024.0));
    return true;
}

void DepthFrameBuffer::Destroy() {
    if (m_fbo) glDeleteFramebuffers(1, &m_fbo);
    if (m_texture) glDeleteTextures(1, &m_texture);
    m_fbo = 0;
    m_texture = 0;
}

void DepthFrameBuffer::Forget() {
    m_fbo = 0;
    m_texture = 0;
}

bool LitDummyTexture::Create(bool hardwareCompare) {
    Destroy();
    GLStateBackup state;
    state.Save(GLStateBackup::kTextures | GLStateBackup::kPixelStore, 1u << 0);
    glActiveTexture(GL_TEXTURE0);
    glBindBuffer(GL_PIXEL_UNPACK_BUFFER, 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glGenTextures(1, &m_texture);
    glBindTexture(GL_TEXTURE_2D, m_texture);
    const GLushort one = 0xFFFF; // depth 1.0
    glTexImage2D(GL_TEXTURE_2D, 0, GL_DEPTH_COMPONENT16, 1, 1, 0, GL_DEPTH_COMPONENT, GL_UNSIGNED_SHORT, &one);
    ConfigureDepthSampling(m_texture, hardwareCompare);
    const GLenum error = glGetError();
    state.Restore();
    if (error != GL_NO_ERROR) {
        GFX_LOGE(kTag, "lit dummy depth texture failed: glError=0x%x", error);
        Destroy();
        return false;
    }
    return true;
}

void LitDummyTexture::Destroy() {
    if (m_texture) glDeleteTextures(1, &m_texture);
    m_texture = 0;
}

} // namespace gfx
