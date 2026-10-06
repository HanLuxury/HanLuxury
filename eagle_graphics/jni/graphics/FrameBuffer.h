#pragma once
// EAGLE graphics engine - depth-only render target used for the shadow atlas.
// GL thread only. ES 3.0 is required (depth texture + FBO without colour).

#include <GLES3/gl3.h>

namespace gfx {

class DepthFrameBuffer {
public:
    struct Desc {
        int width = 0;
        int height = 0;
        bool depth24 = false;
        bool hardwareCompare = false; // GL_COMPARE_REF_TO_TEXTURE + LINEAR (sampler2DShadow)
    };

    bool Create(const Desc& desc);
    void Destroy();      // deletes GL objects (current context)
    void Forget();       // context lost: drop handles without deleting
    bool Valid() const { return m_fbo != 0 && m_texture != 0; }

    GLuint Fbo() const { return m_fbo; }
    GLuint Texture() const { return m_texture; }
    int Width() const { return m_desc.width; }
    int Height() const { return m_desc.height; }
    const Desc& Description() const { return m_desc; }

private:
    GLuint m_fbo = 0;
    GLuint m_texture = 0;
    Desc m_desc;
};

// 1x1 depth texture holding 1.0 ("fully lit"). Bound to the shadow unit when
// no shadow map may be sampled (caster pass, HUD, reflections).
class LitDummyTexture {
public:
    bool Create(bool hardwareCompare);
    void Destroy();
    void Forget() { m_texture = 0; }
    GLuint Texture() const { return m_texture; }

private:
    GLuint m_texture = 0;
};

void ConfigureDepthSampling(GLuint texture, bool hardwareCompare);

} // namespace gfx
