#include "GLCaps.h"
#include "GraphicsLog.h"

#include <GLES3/gl3.h>
#include <cstdio>
#include <cstring>

namespace gfx {
namespace {
constexpr char kTag[] = "GLCaps";

std::string Str(GLenum name) {
    const GLubyte* s = glGetString(name);
    return s ? std::string(reinterpret_cast<const char*>(s)) : std::string("(null)");
}

int Int(GLenum name) {
    GLint v = 0;
    glGetIntegerv(name, &v);
    return v;
}

bool Contains(const std::string& haystack, const char* needle) {
    return haystack.find(needle) != std::string::npos;
}
} // namespace

GLCaps& GLCaps::Get() {
    static GLCaps caps;
    return caps;
}

const char* GpuFamilyName(GpuFamily f) {
    switch (f) {
        case GpuFamily::Adreno: return "Adreno";
        case GpuFamily::Mali: return "Mali";
        case GpuFamily::PowerVR: return "PowerVR";
        case GpuFamily::Other: return "Other";
        default: return "Unknown";
    }
}

bool GLCaps::HasExtension(const char* name) const {
    if (!name || !*name) return false;
    std::string token = " ";
    token += name;
    token += ' ';
    return m_extensions.find(token) != std::string::npos;
}

bool GLCaps::Detect() {
    if (detected) return es3;
    detected = true;

    vendor = Str(GL_VENDOR);
    renderer = Str(GL_RENDERER);
    version = Str(GL_VERSION);
    glslVersion = Str(GL_SHADING_LANGUAGE_VERSION);
    major = minor = 0;
    if (std::sscanf(version.c_str(), "OpenGL ES %d.%d", &major, &minor) != 2) major = minor = 0;
    es3 = major >= 3;
    es31 = major > 3 || (major == 3 && minor >= 1);
    es32 = major > 3 || (major == 3 && minor >= 2);

    if (Contains(renderer, "Adreno")) family = GpuFamily::Adreno;
    else if (Contains(renderer, "Mali")) family = GpuFamily::Mali;
    else if (Contains(renderer, "PowerVR") || Contains(renderer, "IMG")) family = GpuFamily::PowerVR;
    else family = GpuFamily::Other;

    m_extensions = " ";
    if (es3) {
        const int count = Int(GL_NUM_EXTENSIONS);
        for (int i = 0; i < count; ++i) {
            const GLubyte* e = glGetStringi(GL_EXTENSIONS, static_cast<GLuint>(i));
            if (e) {
                m_extensions += reinterpret_cast<const char*>(e);
                m_extensions += ' ';
            }
        }
    } else {
        const GLubyte* e = glGetString(GL_EXTENSIONS);
        if (e) {
            m_extensions += reinterpret_cast<const char*>(e);
            m_extensions += ' ';
        }
    }

    maxTextureSize = Int(GL_MAX_TEXTURE_SIZE);
    maxTextureUnits = Int(GL_MAX_TEXTURE_IMAGE_UNITS);
    maxVertexTextureUnits = Int(GL_MAX_VERTEX_TEXTURE_IMAGE_UNITS);
    maxCombinedTextureUnits = Int(GL_MAX_COMBINED_TEXTURE_IMAGE_UNITS);
    maxRenderbufferSize = Int(GL_MAX_RENDERBUFFER_SIZE);
    glGetIntegerv(GL_MAX_VIEWPORT_DIMS, maxViewportDims);
    maxFragmentUniformVectors = Int(GL_MAX_FRAGMENT_UNIFORM_VECTORS);
    maxVertexUniformVectors = Int(GL_MAX_VERTEX_UNIFORM_VECTORS);
    maxVaryingVectors = Int(GL_MAX_VARYING_VECTORS);
    maxDrawBuffers = es3 ? Int(GL_MAX_DRAW_BUFFERS) : 1;

    GLint range[2] = {0, 0}, precision = 0;
    glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER, GL_HIGH_FLOAT, range, &precision);
    fragmentHighp = precision >= 16;

    depthTexture = es3 || HasExtension("GL_OES_depth_texture");
    shadowSamplers = HasExtension("GL_EXT_shadow_samplers");
    colorBufferHalfFloat = HasExtension("GL_EXT_color_buffer_half_float") || HasExtension("GL_EXT_color_buffer_float");
    colorBufferFloat = HasExtension("GL_EXT_color_buffer_float");
    anisotropic = HasExtension("GL_EXT_texture_filter_anisotropic");
    timerQuery = HasExtension("GL_EXT_disjoint_timer_query");

    // Clear any error raised by queries that are not supported on this driver.
    for (int i = 0; i < 16 && glGetError() != GL_NO_ERROR; ++i) {}
    Log();
    return es3;
}

void GLCaps::Log() const {
    GFX_LOGI(kTag, "OpenGL ES version: %s (parsed %d.%d)", version.c_str(), major, minor);
    GFX_LOGI(kTag, "GPU: %s / %s (%s)", vendor.c_str(), renderer.c_str(), GpuFamilyName(family));
    GFX_LOGI(kTag, "GLSL version: %s", glslVersion.c_str());
    GFX_LOGI(kTag, "MaxTextureSize: %d  MaxRenderbuffer: %d  MaxViewport: %dx%d", maxTextureSize,
             maxRenderbufferSize, maxViewportDims[0], maxViewportDims[1]);
    GFX_LOGI(kTag, "MaxTextureUnits: frag=%d vert=%d combined=%d  MaxDrawBuffers: %d", maxTextureUnits,
             maxVertexTextureUnits, maxCombinedTextureUnits, maxDrawBuffers);
    GFX_LOGI(kTag, "Uniform vectors: vert=%d frag=%d  varyings=%d  fragment highp=%d", maxVertexUniformVectors,
             maxFragmentUniformVectors, maxVaryingVectors, fragmentHighp);
    GFX_LOGI(kTag, "DepthTexture support: %d  EXT_shadow_samplers: %d  half-float RT: %d  float RT: %d  aniso: %d  timer query: %d",
             depthTexture, shadowSamplers, colorBufferHalfFloat, colorBufferFloat, anisotropic, timerQuery);
    GFX_LOGI(kTag, "Framebuffer support: %s", es3 ? "ES3 depth-only FBO" : "unsupported (ES2 context)");
}

} // namespace gfx
