#include "WorldSunShadow.h"
#include "SunShadowMath.h"
#include "SunShadowShaders.h"
#include "../RenderThread.h"
#include "../../modloader/HookScope.h"
#include <GLES3/gl3.h>
#include <android/log.h>
#include <dlfcn.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <utility>

#define SUN_LOG(...) __android_log_print(ANDROID_LOG_INFO,"WorldSunShadow",__VA_ARGS__)
namespace WorldSunShadow {
namespace {
using DrawElementsFn=void(GL_APIENTRYP)(GLenum,GLsizei,GLenum,const void*);
using DrawArraysFn=void(GL_APIENTRYP)(GLenum,GLint,GLsizei);
using ProgramFn=void(GL_APIENTRYP)(GLuint);
DrawElementsFn originalElements=nullptr;
DrawArraysFn originalArrays=nullptr;
ProgramFn originalLink=nullptr,originalDeleteProgram=nullptr;
std::atomic<bool> hooksReady{false};
std::atomic<bool> programHooks{false};
// Bumped by every glLinkProgram/glDeleteProgram: cached uniform locations are
// dropped only then, not every frame (GTA has 100+ programs).
std::atomic<unsigned> programGeneration{1};
thread_local bool replaying=false;
std::mutex settingsMutex;
Settings settings;
constexpr GLsizei kMinCasterIndices=12; // smaller draws cast no visible shadow
void Toggle(GLenum key,GLboolean enabled) { if(enabled) glEnable(key);else glDisable(key); }
template<class T> T Proc(const char* name) { return reinterpret_cast<T>(eglGetProcAddress(name)); }
bool HasExtension(bool es3,const char* name) {
    if(es3) {
        GLint n=0;glGetIntegerv(GL_NUM_EXTENSIONS,&n);
        for(int i=0;i<n;++i) {
            const char* ext=reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS,GLuint(i)));
            if(ext&&std::strcmp(ext,name)==0) return true;
        }
    } else {
        const char* text=reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
        const size_t size=std::strlen(name);
        for(const char* p=text ? std::strstr(text,name):nullptr;p;p=std::strstr(p+size,name))
            if((p==text||p[-1]==' ')&&(p[size]==0||p[size]==' ')) return true;
    }
    return false;
}
struct Caps {
    using GenVAO=void(GL_APIENTRYP)(GLsizei,GLuint*);
    using DelVAO=void(GL_APIENTRYP)(GLsizei,const GLuint*);
    using BindVAO=void(GL_APIENTRYP)(GLuint);
    using EnableI=void(GL_APIENTRYP)(GLenum,GLuint);
    using EnabledI=GLboolean(GL_APIENTRYP)(GLenum,GLuint);
    using MaskI=void(GL_APIENTRYP)(GLuint,GLboolean,GLboolean,GLboolean,GLboolean);
    using BlendFnI=void(GL_APIENTRYP)(GLuint,GLenum,GLenum,GLenum,GLenum);
    using BlendEqI=void(GL_APIENTRYP)(GLuint,GLenum,GLenum);
    GenVAO gen=nullptr;DelVAO del=nullptr;BindVAO bind=nullptr;
    EnableI enableI=nullptr,disableI=nullptr;EnabledI enabledI=nullptr;MaskI maskI=nullptr;
    BlendFnI blendFnI=nullptr;BlendEqI blendEqI=nullptr;
    bool es3=false,es31=false,split=false,pbo=false,unpack=false,depth24=false,drawBuffers=false;
    bool Init() {
        int major=0,minor=0;
        const char* version=reinterpret_cast<const char*>(glGetString(GL_VERSION));
        if(!version||std::sscanf(version,"OpenGL ES %d.%d",&major,&minor)!=2||major<2) return false;
        es3=major>=3;es31=major>3||(major==3&&minor>=1);
        depth24=es3||HasExtension(false,"GL_OES_depth24");
        if(!es3&&!HasExtension(false,"GL_OES_depth_texture")) {
            SUN_LOG("SUN4 bypass: GLES2 needs GL_OES_depth_texture");return false;
        }
        GLint range[2]{},precision=0;glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER,GL_HIGH_FLOAT,range,&precision);
        if(precision<16) { SUN_LOG("SUN4 bypass: fragment highp insufficient");return false; }
        if(es3) {gen=&glGenVertexArrays;del=&glDeleteVertexArrays;bind=&glBindVertexArray;}
        else if(HasExtension(false,"GL_OES_vertex_array_object")) {
            gen=Proc<GenVAO>("glGenVertexArraysOES");del=Proc<DelVAO>("glDeleteVertexArraysOES");
            bind=Proc<BindVAO>("glBindVertexArrayOES");
        }
        if(!gen||!del||!bind) {SUN_LOG("SUN4 bypass: vertex array API unavailable");return false;}
        split=es3||HasExtension(false,"GL_ANGLE_framebuffer_blit")||HasExtension(false,"GL_NV_framebuffer_blit");
        pbo=es3||HasExtension(false,"GL_NV_pixel_buffer_object")||HasExtension(false,"GL_EXT_pixel_buffer_object");
        unpack=es3||HasExtension(false,"GL_EXT_unpack_subimage");
        drawBuffers=es3||HasExtension(false,"GL_EXT_draw_buffers")||HasExtension(false,"GL_NV_draw_buffers");
        const bool indexedCore=major>3||(major==3&&minor>=2);
        const bool indexedExt=HasExtension(es3,"GL_EXT_draw_buffers_indexed");
        const bool indexedOes=HasExtension(es3,"GL_OES_draw_buffers_indexed");
        if(indexedCore||indexedExt||indexedOes) {
            enableI=Proc<EnableI>(indexedCore ? "glEnablei":indexedExt ? "glEnableiEXT":"glEnableiOES");
            disableI=Proc<EnableI>(indexedCore ? "glDisablei":indexedExt ? "glDisableiEXT":"glDisableiOES");
            enabledI=Proc<EnabledI>(indexedCore ? "glIsEnabledi":indexedExt ? "glIsEnablediEXT":"glIsEnablediOES");
            maskI=Proc<MaskI>(indexedCore ? "glColorMaski":indexedExt ? "glColorMaskiEXT":"glColorMaskiOES");
            blendFnI=Proc<BlendFnI>(indexedCore ? "glBlendFuncSeparatei":indexedExt ? "glBlendFuncSeparateiEXT":"glBlendFuncSeparateiOES");
            blendEqI=Proc<BlendEqI>(indexedCore ? "glBlendEquationSeparatei":indexedExt ? "glBlendEquationSeparateiEXT":"glBlendEquationSeparateiOES");
            if(!enableI||!disableI||!enabledI||!maskI||!blendFnI||!blendEqI) return false;
        }
        return true;
    }
    void ColorMask(GLboolean r,GLboolean g,GLboolean b,GLboolean a) const {
        if(maskI) maskI(0,r,g,b,a);else glColorMask(r,g,b,a);
    }
    void Blend(bool on) const {
        if(enableI) {if(on) enableI(GL_BLEND,0);else disableI(GL_BLEND,0);}
        else Toggle(GL_BLEND,on);
    }
};

// Used once per frame (setup, depth copy, composite).
struct ReplayGuard {
    const Caps& caps;
    GLint draw=0,read=0,viewport[4]{},depthFunction=0;
    GLboolean color[4]{},depthMask=0,blend=0,sampleMask=0;
    GLfloat depthRange[2]{},clearDepth=1,offsetFactor=0,offsetUnits=0;
    static constexpr GLenum switches[]={GL_DEPTH_TEST,GL_STENCIL_TEST,GL_SCISSOR_TEST,
        GL_CULL_FACE,GL_POLYGON_OFFSET_FILL,GL_SAMPLE_ALPHA_TO_COVERAGE,GL_SAMPLE_COVERAGE};
    GLboolean enabled[7]{};
    explicit ReplayGuard(const Caps& c):caps(c) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING,&draw);
        if(caps.split) glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);
        glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_DEPTH_FUNC,&depthFunction);
        glGetBooleanv(GL_DEPTH_WRITEMASK,&depthMask);glGetFloatv(GL_DEPTH_RANGE,depthRange);
        glGetFloatv(GL_DEPTH_CLEAR_VALUE,&clearDepth);
        glGetFloatv(GL_POLYGON_OFFSET_FACTOR,&offsetFactor);glGetFloatv(GL_POLYGON_OFFSET_UNITS,&offsetUnits);
        // Non-indexed queries return slot zero, including on GLES2 EXT paths.
        glGetBooleanv(GL_COLOR_WRITEMASK,color);blend=glIsEnabled(GL_BLEND);
        for(int i=0;i<7;++i) enabled[i]=glIsEnabled(switches[i]);
        if(caps.es31) sampleMask=glIsEnabled(0x8E51); // GL_SAMPLE_MASK, ES 3.1.
    }
    void Prepare() {
        for(auto key:switches) glDisable(key);
        if(caps.es31) glDisable(0x8E51);
        caps.Blend(false);caps.ColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);
        glEnable(GL_DEPTH_TEST);glDepthMask(GL_TRUE);glDepthRangef(0,1);
    }
    ~ReplayGuard() {
        glBindFramebuffer(GL_FRAMEBUFFER,GLuint(draw));
        if(caps.split) glBindFramebuffer(GL_READ_FRAMEBUFFER,GLuint(read));
        glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
        glDepthFunc(GLenum(depthFunction));glDepthMask(depthMask);glDepthRangef(depthRange[0],depthRange[1]);
        glClearDepthf(clearDepth);glPolygonOffset(offsetFactor,offsetUnits);
        caps.ColorMask(color[0],color[1],color[2],color[3]);caps.Blend(blend);
        for(int i=0;i<7;++i) Toggle(switches[i],enabled[i]);
        if(caps.es31) Toggle(0x8E51,sampleMask);
    }
};
struct FullGuard : ReplayGuard {
    GLint program=0,vao=0,vbo=0,renderbuffer=0,activeTexture=0,texture[2]{},sampler[2]{};
    GLint unpackBuffer=0,alignment=4,rowLength=0,skipRows=0,skipPixels=0;
    GLint blendEquationRGB=0,blendEquationAlpha=0,srcRGB=0,dstRGB=0,srcAlpha=0,dstAlpha=0;
    explicit FullGuard(const Caps& c):ReplayGuard(c) {
        glGetIntegerv(GL_CURRENT_PROGRAM,&program);glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&vbo);glGetIntegerv(GL_RENDERBUFFER_BINDING,&renderbuffer);
        glGetIntegerv(GL_ACTIVE_TEXTURE,&activeTexture);
        for(int i=0;i<2;++i) {
            glActiveTexture(GL_TEXTURE0+i);glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture[i]);
            if(caps.es3) {glGetIntegerv(GL_SAMPLER_BINDING,&sampler[i]);glBindSampler(i,0);}
        }
        glActiveTexture(GL_TEXTURE0);
        if(caps.pbo) {glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&unpackBuffer);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);}
        glGetIntegerv(GL_UNPACK_ALIGNMENT,&alignment);glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        if(caps.unpack) {
            glGetIntegerv(GL_UNPACK_ROW_LENGTH,&rowLength);glGetIntegerv(GL_UNPACK_SKIP_ROWS,&skipRows);
            glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&skipPixels);
            glPixelStorei(GL_UNPACK_ROW_LENGTH,0);glPixelStorei(GL_UNPACK_SKIP_ROWS,0);glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
        }
        // The composite is allowed only on a single color target. No MRT blend
        // state is modified: indexed entry points are used when available.
        auto get=[](GLenum key,GLint* out) {glGetIntegerv(key,out);};
        get(GL_BLEND_EQUATION_RGB,&blendEquationRGB);get(GL_BLEND_EQUATION_ALPHA,&blendEquationAlpha);
        get(GL_BLEND_SRC_RGB,&srcRGB);get(GL_BLEND_DST_RGB,&dstRGB);
        get(GL_BLEND_SRC_ALPHA,&srcAlpha);get(GL_BLEND_DST_ALPHA,&dstAlpha);
    }
    void BlendFunction(GLenum src,GLenum dst,GLenum srcA,GLenum dstA) {
        if(caps.maskI) caps.blendFnI(0,src,dst,srcA,dstA);
        else glBlendFuncSeparate(src,dst,srcA,dstA);
    }
    void BlendEquation(GLenum rgb,GLenum alpha) {
        if(caps.maskI) caps.blendEqI(0,rgb,alpha);
        else glBlendEquationSeparate(rgb,alpha);
    }
    ~FullGuard() {
        glUseProgram(GLuint(program));caps.bind(GLuint(vao));glBindBuffer(GL_ARRAY_BUFFER,GLuint(vbo));
        glBindRenderbuffer(GL_RENDERBUFFER,GLuint(renderbuffer));
        for(int i=0;i<2;++i) {
            glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,GLuint(texture[i]));
            if(caps.es3) glBindSampler(GLuint(i),GLuint(sampler[i]));
        }
        glActiveTexture(GLenum(activeTexture));
        if(caps.pbo) glBindBuffer(GL_PIXEL_UNPACK_BUFFER,GLuint(unpackBuffer));
        glPixelStorei(GL_UNPACK_ALIGNMENT,alignment);
        if(caps.unpack) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH,rowLength);glPixelStorei(GL_UNPACK_SKIP_ROWS,skipRows);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS,skipPixels);
        }
        BlendFunction(GLenum(srcRGB),GLenum(dstRGB),GLenum(srcAlpha),GLenum(dstAlpha));
        BlendEquation(GLenum(blendEquationRGB),GLenum(blendEquationAlpha));
    }
};
// One depth texture (+ the color buffer GLES2 needs for completeness).
struct DepthTarget {
    GLuint fbo=0,depth=0,color=0;
    void Destroy() {
        if(fbo) glDeleteFramebuffers(1,&fbo);
        if(depth) glDeleteTextures(1,&depth);
        if(color) glDeleteRenderbuffers(1,&color);
        fbo=depth=color=0;
    }
    // compare=true: sampler2DShadow with hardware 2x2 PCF (GLES3 only).
    bool Create(const Caps& caps,int w,int h,bool compare) {
        glGenTextures(1,&depth);glBindTexture(GL_TEXTURE_2D,depth);
        glTexImage2D(GL_TEXTURE_2D,0,caps.es3 ? GL_DEPTH_COMPONENT24:GL_DEPTH_COMPONENT,w,h,0,
                     GL_DEPTH_COMPONENT,caps.depth24 ? GL_UNSIGNED_INT:GL_UNSIGNED_SHORT,nullptr);
        const GLint filter=compare&&caps.es3 ? GL_LINEAR:GL_NEAREST;
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,filter);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,filter);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        if(caps.es3) {
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_COMPARE_MODE,compare ? GL_COMPARE_REF_TO_TEXTURE:GL_NONE);
            glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_COMPARE_FUNC,GL_LEQUAL);
        }
        glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,depth,0);
        if(caps.es3) {
            // Depth-only target: no color memory/bandwidth on tilers.
            const GLenum none=GL_NONE;glDrawBuffers(1,&none);glReadBuffer(GL_NONE);
        } else {
            glGenRenderbuffers(1,&color);glBindRenderbuffer(GL_RENDERBUFFER,color);
            glRenderbufferStorage(GL_RENDERBUFFER,GL_RGBA4,w,h);
            glFramebufferRenderbuffer(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_RENDERBUFFER,color);
        }
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) {Destroy();return false;}
        return true;
    }
};
// Copy of the world target's real depth buffer (same internal format, as
// glBlitFramebuffer requires for depth).
struct DepthCopy {
    GLuint fbo=0,texture=0;int width=0,height=0;GLenum format=0;
    void Destroy() {
        if(fbo) glDeleteFramebuffers(1,&fbo);
        if(texture) glDeleteTextures(1,&texture);
        fbo=texture=0;width=height=0;format=0;
    }
    bool Ensure(int w,int h,GLenum f) {
        if(fbo&&width==w&&height==h&&format==f) return true;
        Destroy();
        glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
        glTexStorage2D(GL_TEXTURE_2D,1,f,w,h);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_COMPARE_MODE,GL_NONE);
        glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        const bool packed=f==GL_DEPTH24_STENCIL8||f==GL_DEPTH32F_STENCIL8;
        glFramebufferTexture2D(GL_FRAMEBUFFER,packed ? GL_DEPTH_STENCIL_ATTACHMENT:GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,texture,0);
        const GLenum none=GL_NONE;glDrawBuffers(1,&none);glReadBuffer(GL_NONE);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) {Destroy();return false;}
        width=w;height=h;format=f;return true;
    }
};
GLint Attachment(GLenum attachment,GLenum pname) {
    GLint value=0;glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,attachment,pname,&value);return value;
}
// Internal format of the bound draw framebuffer's depth, or 0 when it cannot
// be copied (no depth, multisampled, or a format we do not recreate exactly).
GLenum CopyableDepthFormat(GLint framebuffer) {
    GLint samples=0;glGetIntegerv(GL_SAMPLES,&samples);
    if(samples!=0) return 0;
    const GLenum depthName=framebuffer ? GL_DEPTH_ATTACHMENT:GL_DEPTH;
    const GLenum stencilName=framebuffer ? GL_STENCIL_ATTACHMENT:GL_STENCIL;
    if(Attachment(depthName,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE)==GL_NONE) return 0;
    const GLint bits=Attachment(depthName,GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE);
    const GLint component=Attachment(depthName,GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE);
    GLint stencil=0;
    if(Attachment(stencilName,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE)!=GL_NONE)
        stencil=Attachment(stencilName,GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE);
    if(component==GL_FLOAT&&bits==32) return stencil==8 ? GL_DEPTH32F_STENCIL8:(stencil==0 ? GL_DEPTH_COMPONENT32F:0);
    if(component!=GL_UNSIGNED_NORMALIZED) return 0;
    if(bits==24&&stencil==8) return GL_DEPTH24_STENCIL8;
    if(stencil!=0) return 0;
    return bits==16 ? GL_DEPTH_COMPONENT16:(bits==24 ? GL_DEPTH_COMPONENT24:0);
}
GLuint Compile(GLenum kind,const char* source) {
    GLuint shader=glCreateShader(kind);if(!shader) return 0;
    glShaderSource(shader,1,&source,nullptr);glCompileShader(shader);
    GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    if(!ok) {char message[1024]{};glGetShaderInfoLog(shader,sizeof(message),nullptr,message);
        SUN_LOG("SUN4 compile failed: %s",message);glDeleteShader(shader);return 0;}
    return shader;
}
struct Locations {GLint view=-1,projection=-1,object=-1;};
struct State {
    Caps caps;
    bool checked=false,failed=false,active=false,shadow=false,reported=false,reportedDepth=false;
    std::atomic<bool> retired{false};
    Settings config;
    Frame frame;
    Frame queued;
    bool armed=false;
    unsigned missingFrames=0;
    unsigned unarmedFrames=0; // world ends without a world scope (wrong expected target)
    EGLContext owner=EGL_NO_CONTEXT;
    GLint expectedViewport[4]{},expectedFramebuffer=0;
    EGLSurface surface=EGL_NO_SURFACE;
    GLint output=0,viewport[4]{};
    int width=0,height=0,resolution=0,casters=0,skipped=0,cameraDraws=0;
    GLuint program=0,vao=0,vbo=0;
    bool program3=false;
    GLint inverseLocation=-1,lightLocation=-1,parametersLocation=-1,receiverLocation=-1;
    GLint sunLocation=-1,cameraLocation=-1,texelLocation=-1;
    DepthTarget camera,sun;
    DepthCopy world;
    GLenum worldDepthFormat=0;   // copyable format of the current world target, 0 = replay fallback
    bool replayCamera=false,cameraCleared=false,worldCopied=false;
    float lightView[16]{},lightProjection[16]{},lightVP[16]{},cameraView[16]{},cameraProjection[16]{},inverseCamera[16]{};
    float cameraPosition[3]{};
    float stableSun[3]{},lightAxis[3]{};
    float cameraClear=1;
    bool haveCamera=false,published=false;
    unsigned generation=0;
    std::map<GLuint,Locations> programs;
    void Destroy() {
        camera.Destroy();sun.Destroy();world.Destroy();
        if(program) glDeleteProgram(program);
        if(vao&&caps.del) caps.del(1,&vao);
        if(vbo) glDeleteBuffers(1,&vbo);
        program=vao=vbo=0;active=shadow=published=false;width=height=resolution=0;
        programs.clear();
    }
    bool EnsureProgram() {
        if(program) return true;
        program3=caps.es3;
        GLuint vs=Compile(GL_VERTEX_SHADER,program3 ? Shaders::Vertex3:Shaders::Vertex);
        GLuint fs=Compile(GL_FRAGMENT_SHADER,program3 ? Shaders::Fragment3:Shaders::Fragment);
        if(vs&&fs) {
            program=glCreateProgram();glAttachShader(program,vs);glAttachShader(program,fs);
            glBindAttribLocation(program,0,"aPosition");glLinkProgram(program);
        }
        if(vs) glDeleteShader(vs);
        if(fs) glDeleteShader(fs);
        GLint ok=0;if(program) glGetProgramiv(program,GL_LINK_STATUS,&ok);
        if(!ok) {if(program) glDeleteProgram(program);program=0;return false;}
        inverseLocation=glGetUniformLocation(program,"uInverseCamera");
        lightLocation=glGetUniformLocation(program,"uLightVP");
        parametersLocation=glGetUniformLocation(program,"uParameters");
        receiverLocation=glGetUniformLocation(program,"uReceiver");
        sunLocation=glGetUniformLocation(program,"uLight");
        cameraLocation=glGetUniformLocation(program,"uCamera");
        texelLocation=glGetUniformLocation(program,"uDepthTexel");
        glUseProgram(program);glUniform1i(glGetUniformLocation(program,"uCameraDepth"),0);
        glUniform1i(glGetUniformLocation(program,"uSunDepth"),1);
        const float triangle[]={-1,-1,3,-1,-1,3};
        caps.gen(1,&vao);caps.bind(vao);glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);
        glBufferData(GL_ARRAY_BUFFER,sizeof(triangle),triangle,GL_STATIC_DRAW);
        glEnableVertexAttribArray(0);glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr);
        return true;
    }
    bool EnsureSun() {
        if(sun.fbo&&resolution==config.resolution) return true;
        sun.Destroy();
        if(!sun.Create(caps,config.resolution,config.resolution,caps.es3)) return false;
        resolution=config.resolution;
        SUN_LOG("SUN4 sun map ready: %d, radius=%.0f, %s",resolution,config.radius,caps.es3 ? "hardware PCF":"GLES2 PCF");
        return true;
    }
    bool EnsureCamera(int w,int h) {
        if(camera.fbo&&width==w&&height==h) return true;
        camera.Destroy();
        if(!camera.Create(caps,w,h,false)) return false;
        width=w;height=h;return true;
    }
    Locations Find(GLuint id) {
        const unsigned now=programGeneration.load(std::memory_order_relaxed);
        if(generation!=now) {programs.clear();generation=now;}
        auto old=programs.find(id);if(old!=programs.end()) return old->second;
        Locations result;GLint uniforms=0;glGetProgramiv(id,GL_ACTIVE_UNIFORMS,&uniforms);
        for(int i=0;i<uniforms;++i) {
            char name[128]{};GLint size=0;GLenum type=0;
            glGetActiveUniform(id,GLuint(i),sizeof(name),nullptr,&size,&type,name);
            if(size!=1||type!=GL_FLOAT_MAT4) continue;
            if(std::strcmp(name,"ViewMatrix")==0) result.view=glGetUniformLocation(id,name);
            if(std::strcmp(name,"ProjMatrix")==0) result.projection=glGetUniformLocation(id,name);
            if(std::strcmp(name,"ObjMatrix")==0) result.object=glGetUniformLocation(id,name);
        }
        programs.emplace(id,result);return result;
    }
};
using Key=std::pair<EGLDisplay,EGLContext>;
std::mutex statesMutex;
std::map<Key,std::shared_ptr<State>> states;
// GL work happens on the thread that owns the context (GTA's render thread);
// other threads that draw in this process (HWUI, WebView) see empty slots.
thread_local std::shared_ptr<State> active;
thread_local std::shared_ptr<State> automatic;
thread_local std::shared_ptr<State> lastWorld;
std::atomic<bool> depthWanted{true};
struct ReplayScope {ReplayScope(){replaying=true;}~ReplayScope(){replaying=false;}};
bool UnsafeQueryState(const Caps& caps) {
    if(!caps.es3) return false;
    GLboolean transform=0,paused=0;
    glGetBooleanv(GL_TRANSFORM_FEEDBACK_ACTIVE,&transform);glGetBooleanv(GL_TRANSFORM_FEEDBACK_PAUSED,&paused);
    GLint query=0,conservative=0;
    glGetQueryiv(GL_ANY_SAMPLES_PASSED,GL_CURRENT_QUERY,&query);
    glGetQueryiv(GL_ANY_SAMPLES_PASSED_CONSERVATIVE,GL_CURRENT_QUERY,&conservative);
    return (transform&&!paused)||query||conservative||glIsEnabled(GL_RASTERIZER_DISCARD);
}
// The world scope starts on the first perspective, depth-writing draw into
// the target recorded at the end of the previous world pass.
void TryBegin() {
    auto next=automatic;
    if(eglGetCurrentContext()!=next->owner||eglGetCurrentSurface(EGL_DRAW)!=next->surface) return;
    GLboolean write=0;glGetBooleanv(GL_DEPTH_WRITEMASK,&write);
    GLint viewport[4]{},id=0,framebuffer=0;glGetIntegerv(GL_VIEWPORT,viewport);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&framebuffer);
    if(!write||!glIsEnabled(GL_DEPTH_TEST)) return;
    if(framebuffer!=next->expectedFramebuffer||std::memcmp(viewport,next->expectedViewport,sizeof(viewport))!=0) {
        // Never stay unarmed: after a few frames without a world scope accept a
        // perspective draw covering at least a quarter of the window (excludes
        // reflection/mirror maps, plates and snapshots).
        if(next->unarmedFrames<3) return;
        EGLint w=0,h=0;
        if(!eglQuerySurface(eglGetCurrentDisplay(),next->surface,EGL_WIDTH,&w)||
           !eglQuerySurface(eglGetCurrentDisplay(),next->surface,EGL_HEIGHT,&h)||w<=0||h<=0) return;
        if(int64_t(viewport[2])*viewport[3]*4<int64_t(w)*h) return;
    }
    glGetIntegerv(GL_CURRENT_PROGRAM,&id);if(!id) return;
    const auto location=next->Find(GLuint(id));
    if(location.view<0||location.projection<0) return;
    float view[16],projection[16],inverse[16],forward=0;
    glGetUniformfv(GLuint(id),location.view,view);glGetUniformfv(GLuint(id),location.projection,projection);
    if(!EglPostFX::Math::Projection(projection,inverse,forward)||!EglPostFX::Math::Invert(view,inverse)) return;
    GLint depthFunction=GL_LESS;glGetIntegerv(GL_DEPTH_FUNC,&depthFunction);
    auto frame=next->queued;
    // Use this draw's camera; do not use last frame's position after a
    // vehicle transition, teleport, camera cut, or first-person switch.
    for(int i=0;i<3;++i) frame.focus[i]=inverse[12+i]+inverse[8+i]*forward*22.0f;
    next->armed=false;
    std::copy(view,view+16,next->cameraView);std::copy(projection,projection+16,next->cameraProjection);
    for(int i=0;i<3;++i) next->cameraPosition[i]=inverse[12+i];
    next->cameraClear=(depthFunction==GL_GREATER||depthFunction==GL_GEQUAL) ? 0.0f:1.0f;
    next->haveCamera=true;
    BeginWorld(frame);
}
// One replayed caster. Only state that differs from the game's draw is
// touched; the destructor puts back exactly what the game had set.
struct CasterGuard {
    const Caps& caps;const State& s;
    GLint read=0,depthFunction=GL_LESS;
    GLfloat offsetFactor=0,offsetUnits=0;
    GLboolean color[4]{},blend=0,cull=0,scissor=0,alphaCoverage=0,coverage=0,sampleMask=0;
    CasterGuard(const Caps& c,const State& state,GLint depth):caps(c),s(state),depthFunction(depth) {
        if(caps.split) glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&read);
        glGetFloatv(GL_POLYGON_OFFSET_FACTOR,&offsetFactor);glGetFloatv(GL_POLYGON_OFFSET_UNITS,&offsetUnits);
        glGetBooleanv(GL_COLOR_WRITEMASK,color);blend=glIsEnabled(GL_BLEND);
        cull=glIsEnabled(GL_CULL_FACE);scissor=glIsEnabled(GL_SCISSOR_TEST);
        alphaCoverage=glIsEnabled(GL_SAMPLE_ALPHA_TO_COVERAGE);coverage=glIsEnabled(GL_SAMPLE_COVERAGE);
        if(caps.es31) sampleMask=glIsEnabled(0x8E51);
        if(scissor) glDisable(GL_SCISSOR_TEST);
        if(alphaCoverage) glDisable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        if(coverage) glDisable(GL_SAMPLE_COVERAGE);
        if(sampleMask) glDisable(0x8E51);
        if(blend) caps.Blend(false);
        caps.ColorMask(GL_FALSE,GL_FALSE,GL_FALSE,GL_FALSE);
    }
    ~CasterGuard() {
        glBindFramebuffer(GL_FRAMEBUFFER,GLuint(s.output));
        if(caps.split) glBindFramebuffer(GL_READ_FRAMEBUFFER,GLuint(read));
        glViewport(s.viewport[0],s.viewport[1],s.viewport[2],s.viewport[3]);
        glDepthFunc(GLenum(depthFunction));
        glDisable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(offsetFactor,offsetUnits);
        Toggle(GL_CULL_FACE,cull);
        if(scissor) glEnable(GL_SCISSOR_TEST);
        if(alphaCoverage) glEnable(GL_SAMPLE_ALPHA_TO_COVERAGE);
        if(coverage) glEnable(GL_SAMPLE_COVERAGE);
        if(sampleMask) glEnable(0x8E51);
        caps.ColorMask(color[0],color[1],color[2],color[3]);
        if(blend) caps.Blend(true);
    }
};
// Object origin far outside the sun map: the draw cannot cast into it.
bool OutsideSunMap(const State& s,GLuint program,GLint objectLocation) {
    if(objectLocation<0) return false;
    float object[16];glGetUniformfv(program,objectLocation,object);
    const float* t=object+12;
    if(!std::isfinite(t[0])||!std::isfinite(t[1])||!std::isfinite(t[2])) return false;
    if(t[0]==0.0f&&t[1]==0.0f&&t[2]==0.0f) return false; // world-space batch: keep
    const float* m=s.lightVP;
    const float x=m[0]*t[0]+m[4]*t[1]+m[8]*t[2]+m[12];
    const float y=m[1]*t[0]+m[5]*t[1]+m[9]*t[2]+m[13];
    // Orthographic: w == 1. 2.2 = the map plus 1.2 radii of margin for large objects.
    return std::abs(x)>2.2f||std::abs(y)>2.2f;
}
template<class Draw> void Capture(Draw draw,GLsizei count) {
    if(replaying||!hooksReady.load(std::memory_order_acquire)) return;
    if(!active&&automatic&&automatic->armed) TryBegin();
    State* s=active.get();
    if(!s||!s->active||!s->shadow||s->retired.load(std::memory_order_relaxed)) return;
    if(count<kMinCasterIndices) return;
    if(eglGetCurrentContext()!=s->owner||eglGetCurrentSurface(EGL_DRAW)!=s->surface) return;
    GLint framebuffer=0,viewport[4]{};glGetIntegerv(GL_FRAMEBUFFER_BINDING,&framebuffer);glGetIntegerv(GL_VIEWPORT,viewport);
    if(framebuffer!=s->output||std::memcmp(viewport,s->viewport,sizeof(viewport))!=0) return;
    GLboolean write=0;glGetBooleanv(GL_DEPTH_WRITEMASK,&write);
    if(!write||!glIsEnabled(GL_DEPTH_TEST)) return; // Transparent overlays are not opaque blockers.
    // Decals (polygon offset) and stencil marks lie on other surfaces: skip
    // only this draw. The old code dropped the whole frame here, which made
    // all shadows pop off/on while the camera moved.
    if(glIsEnabled(GL_POLYGON_OFFSET_FILL)||glIsEnabled(GL_STENCIL_TEST)) return;
    GLint id=0;glGetIntegerv(GL_CURRENT_PROGRAM,&id);if(!id) return;
    const auto location=s->Find(GLuint(id));
    if(location.view<0||location.projection<0) return;
    if(s->casters>=s->config.maxDraws) {++s->skipped;return;}
    if(OutsideSunMap(*s,GLuint(id),location.object)) return;
    GLfloat range[2]{};glGetFloatv(GL_DEPTH_RANGE,range);
    if(range[0]!=0.0f||range[1]!=1.0f) return;
    GLint depthFunction=GL_LESS;glGetIntegerv(GL_DEPTH_FUNC,&depthFunction);
    if(depthFunction!=GL_LESS&&depthFunction!=GL_LEQUAL&&depthFunction!=GL_GREATER&&depthFunction!=GL_GEQUAL) return;
    float view[16],projection[16];
    glGetUniformfv(GLuint(id),location.view,view);glGetUniformfv(GLuint(id),location.projection,projection);
    ++s->casters;
    ReplayScope scope;
    {
        CasterGuard guard(s->caps,*s,depthFunction);
        // Camera-depth replay only when the real depth cannot be copied
        // (GLES2, multisampled target). Same camera required for a receiver.
        if(s->replayCamera&&Math::Same(view,s->cameraView)&&Math::Same(projection,s->cameraProjection)) {
            glBindFramebuffer(GL_FRAMEBUFFER,s->camera.fbo);glViewport(0,0,s->width,s->height);
            if(!s->cameraCleared) {
                GLfloat clear=1;glGetFloatv(GL_DEPTH_CLEAR_VALUE,&clear);
                glClearDepthf(s->cameraClear);glClear(GL_DEPTH_BUFFER_BIT);glClearDepthf(clear);
                s->cameraCleared=true;
            }
            draw();++s->cameraDraws;
        }
        glDisable(GL_CULL_FACE); // Two-sided casters: thin walls can block sunlight.
        glBindFramebuffer(GL_FRAMEBUFFER,s->sun.fbo);glViewport(0,0,s->resolution,s->resolution);
        glDepthFunc(GL_LEQUAL);glEnable(GL_POLYGON_OFFSET_FILL);glPolygonOffset(1.25f,2.0f);
        glUniformMatrix4fv(location.view,1,GL_FALSE,s->lightView);
        glUniformMatrix4fv(location.projection,1,GL_FALSE,s->lightProjection);
        draw();
        glUniformMatrix4fv(location.view,1,GL_FALSE,view);
        glUniformMatrix4fv(location.projection,1,GL_FALSE,projection);
    }
}
void GL_APIENTRY HookElements(GLenum mode,GLsizei count,GLenum type,const void* indices) {
    ML_HOOK_SCOPE();
    if(!originalElements) return;
    originalElements(mode,count,type,indices);
    if(count>0&&(mode==GL_TRIANGLES||mode==GL_TRIANGLE_STRIP||mode==GL_TRIANGLE_FAN))
        try {Capture([&]{originalElements(mode,count,type,indices);},count);}
        catch(...) {if(active) active->shadow=false;}
}
void GL_APIENTRY HookArrays(GLenum mode,GLint first,GLsizei count) {
    ML_HOOK_SCOPE();
    if(!originalArrays) return;
    originalArrays(mode,first,count);
    if(count>0&&(mode==GL_TRIANGLES||mode==GL_TRIANGLE_STRIP||mode==GL_TRIANGLE_FAN))
        try {Capture([&]{originalArrays(mode,first,count);},count);}
        catch(...) {if(active) active->shadow=false;}
}
void GL_APIENTRY HookLinkProgram(GLuint program) {
    ML_HOOK_SCOPE();
    programGeneration.fetch_add(1,std::memory_order_relaxed);
    if(originalLink) originalLink(program);
}
void GL_APIENTRY HookDeleteProgram(GLuint program) {
    ML_HOOK_SCOPE();
    programGeneration.fetch_add(1,std::memory_order_relaxed);
    if(originalDeleteProgram) originalDeleteProgram(program);
}
// Render-thread side of SubmitWorldEnd. Each slot is reused only after the
// render thread consumed it; the game thread drops a frame instead of waiting.
struct WorldEndSlot {std::atomic<bool> busy{false};Frame next;};
WorldEndSlot worldEndSlots[8];
std::atomic<unsigned> worldEndCursor{0};
void RunWorldEnd(void* argument) {
    auto* slot=static_cast<WorldEndSlot*>(argument);
    const Frame next=slot->next;
    slot->busy.store(false,std::memory_order_release);
    EndWorld();QueueFrame(next);
}
}

bool InstallDrawHooks(HookBackend backend) {
    static std::once_flag once;
    std::call_once(once,[&] {
        if(!backend) return;
        // Pin the system GL dispatch library; the hooks live as long as the process.
        void* gl=dlopen("libGLESv2.so",RTLD_NOW|RTLD_LOCAL);
        if(!gl) {SUN_LOG("SUN4 draw hooks unavailable: %s",dlerror());return;}
        void* elements=dlsym(gl,"glDrawElements"),*arrays=dlsym(gl,"glDrawArrays");
        if(!elements||!arrays||elements==arrays) {SUN_LOG("SUN4 draw symbols invalid");return;}
        if(!backend(elements,reinterpret_cast<void*>(&HookElements),reinterpret_cast<void**>(&originalElements))||!originalElements) {
            SUN_LOG("SUN4 glDrawElements hook failed");return;
        }
        if(!backend(arrays,reinterpret_cast<void*>(&HookArrays),reinterpret_cast<void**>(&originalArrays))||!originalArrays) {
            SUN_LOG("SUN4 glDrawArrays hook failed; installed hooks stay pass-through");return;
        }
        // Optional: without them, cached uniform locations are dropped each frame.
        void* link=dlsym(gl,"glLinkProgram"),*remove=dlsym(gl,"glDeleteProgram");
        if(link&&remove&&link!=remove&&
           backend(link,reinterpret_cast<void*>(&HookLinkProgram),reinterpret_cast<void**>(&originalLink))&&originalLink&&
           backend(remove,reinterpret_cast<void*>(&HookDeleteProgram),reinterpret_cast<void**>(&originalDeleteProgram))&&originalDeleteProgram)
            programHooks=true;
        else SUN_LOG("SUN4 program hooks unavailable; uniform cache is per frame");
        hooksReady.store(true,std::memory_order_release);
        SUN_LOG("SUN4 native draw replay installed (ShadowHook); no GTA offset changes");
    });
    return hooksReady.load();
}
void SetSettings(const Settings& input) {
    Settings value=input;
    value.resolution=std::clamp(value.resolution,512,4096);value.maxDraws=std::clamp(value.maxDraws,64,8000);
    value.radius=std::isfinite(value.radius) ? std::clamp(value.radius,15.0f,250.0f):70.0f;
    value.darkness=std::isfinite(value.darkness) ? std::clamp(value.darkness,0.0f,0.85f):0.50f;
    value.softness=std::isfinite(value.softness) ? std::clamp(value.softness,0.5f,3.0f):1.0f;
    std::lock_guard<std::mutex> lock(settingsMutex);settings=value;
}
Settings GetSettings() {std::lock_guard<std::mutex> lock(settingsMutex);return settings;}
static void BeginWorldImpl(const Frame& frame) {
    if(active) active->active=false;
    active.reset();
    if(!hooksReady.load()) return;
    Settings config;{std::lock_guard<std::mutex> lock(settingsMutex);config=settings;}
    const auto display=eglGetCurrentDisplay();const auto context=eglGetCurrentContext();
    if(display==EGL_NO_DISPLAY||context==EGL_NO_CONTEXT) return;
    std::shared_ptr<State> s;
    {std::lock_guard<std::mutex> lock(statesMutex);auto& slot=states[{display,context}];
        if(!slot) slot=std::make_shared<State>();s=slot;}
    if(s->retired.load()||s->failed) return;
    if(!s->checked) {s->checked=true;if(!s->caps.Init()) {s->failed=true;return;}}
    // Resource setup changes program/vertex bindings. Do not enter it inside
    // transform feedback or a query, even for the first automatic world draw.
    if(UnsafeQueryState(s->caps)) return;
    if(!programHooks.load(std::memory_order_relaxed)) s->programs.clear();
    s->config=config;s->frame=frame;s->frame.sunlight=std::clamp(frame.sunlight,0.0f,1.0f);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING,&s->output);glGetIntegerv(GL_VIEWPORT,s->viewport);
    if(s->viewport[2]<16||s->viewport[3]<16||glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) return;
    if(s->caps.drawBuffers) {
        GLint max=0;glGetIntegerv(GL_MAX_DRAW_BUFFERS,&max);
        for(int i=1;i<max;++i) {GLint target=0;glGetIntegerv(GL_DRAW_BUFFER0+i,&target);if(target!=GL_NONE) return;}
    }
    GLfloat range[2]{};glGetFloatv(GL_DEPTH_RANGE,range);
    if(range[0]!=0.0f||range[1]!=1.0f) return;
    s->owner=context;s->surface=eglGetCurrentSurface(EGL_DRAW);
    s->casters=s->skipped=s->cameraDraws=0;s->cameraCleared=s->worldCopied=s->published=false;
    s->worldDepthFormat=s->caps.es3&&(config.enabled||depthWanted.load(std::memory_order_relaxed))
        ? CopyableDepthFormat(s->output):0;
    s->active=true;s->shadow=false;active=s;
    if(!config.enabled||!frame.valid||!std::isfinite(frame.sunlight)||frame.sunlight<=0) return;
    // Sun shadow pass of this frame.
    Math::StableDirection(frame.toSun,s->stableSun);
    if(!Math::LightCamera(frame.focus,s->stableSun,config.radius,config.resolution,s->lightAxis,
                          s->lightView,s->lightProjection,s->lightVP)) return;
    s->replayCamera=s->worldDepthFormat==0;
    if(s->replayCamera&&!s->haveCamera) return;
    FullGuard guard(s->caps);ReplayScope scope;
    if(!s->EnsureProgram()||!s->EnsureSun()||(s->replayCamera&&!s->EnsureCamera(s->viewport[2],s->viewport[3]))) {
        s->failed=true;s->Destroy();active.reset();SUN_LOG("SUN4 resource setup failed");return;
    }
    if(s->replayCamera) {
        float vp[16];Math::Multiply(s->cameraProjection,s->cameraView,vp);
        if(!EglPostFX::Math::Invert(vp,s->inverseCamera)) return;
    }
    guard.Prepare();glBindFramebuffer(GL_FRAMEBUFFER,s->sun.fbo);
    glClearDepthf(1.0f);glClear(GL_DEPTH_BUFFER_BIT);
    s->shadow=true;
}
void BeginWorld(const Frame& frame) {
    try {BeginWorldImpl(frame);}
    catch(...) {if(active) active->active=false;active.reset();SUN_LOG("SUN4 setup allocation failed");}
}
void QueueFrame(const Frame& frame) {
    if(!hooksReady.load()) return;
    const auto display=eglGetCurrentDisplay();const auto context=eglGetCurrentContext();
    if(display==EGL_NO_DISPLAY||context==EGL_NO_CONTEXT) return;
    try {
        std::shared_ptr<State> s;
        {std::lock_guard<std::mutex> lock(statesMutex);auto& slot=states[{display,context}];
            if(!slot) slot=std::make_shared<State>();s=slot;}
        s->queued=frame;s->owner=context;s->surface=eglGetCurrentSurface(EGL_DRAW);
        if(s->published) {
            // The next frame draws its world into the same target as this one.
            s->expectedFramebuffer=s->output;
            std::copy(s->viewport,s->viewport+4,s->expectedViewport);
            s->unarmedFrames=0;
        } else {
            glGetIntegerv(GL_VIEWPORT,s->expectedViewport);
            glGetIntegerv(GL_FRAMEBUFFER_BINDING,&s->expectedFramebuffer);
            if(s->unarmedFrames<1000) ++s->unarmedFrames;
        }
        s->armed=false;automatic=s;
    } catch(...) {automatic.reset();SUN_LOG("SUN4 queue allocation failed");}
}
void AfterSwap(EGLBoolean result) {
    if(active) active->active=false;
    active.reset();
    if(lastWorld) lastWorld->published=false;
    lastWorld.reset();
    if(automatic) {
        if(!programHooks.load(std::memory_order_relaxed)) automatic->programs.clear();
        // Armed every frame: the camera/depth capture also runs at night.
        automatic->armed=result==EGL_TRUE&&!automatic->retired.load();
    }
}
static bool CopyWorldDepth(State& s) {
    if(!s.worldDepthFormat) return false;
    const int w=s.viewport[2],h=s.viewport[3];
    if(!s.world.Ensure(w,h,s.worldDepthFormat)) {s.worldDepthFormat=0;return false;}
    glBindFramebuffer(GL_READ_FRAMEBUFFER,GLuint(s.output));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,s.world.fbo);
    glBlitFramebuffer(s.viewport[0],s.viewport[1],s.viewport[0]+w,s.viewport[1]+h,0,0,w,h,GL_DEPTH_BUFFER_BIT,GL_NEAREST);
    return true;
}
void EndWorld() {
    auto s=active;active.reset();
    if(!s) {
        if(automatic&&automatic->armed&&automatic->missingFrames<120&&++automatic->missingFrames==120)
            SUN_LOG("SUN4 waiting: no matching main-world draw (FBO=%d viewport=%dx%d); check native draw hooks and ViewMatrix/ProjMatrix",
                    automatic->expectedFramebuffer,automatic->expectedViewport[2],automatic->expectedViewport[3]);
        return;
    }
    s->active=false;
    if(s->retired.load()||!s->haveCamera) return;
    if(eglGetCurrentContext()!=s->owner||eglGetCurrentSurface(EGL_DRAW)!=s->surface) return;
    // The world target is bound explicitly below (FullGuard restores the game's
    // binding); it only has to still exist.
    if(s->output!=0&&!glIsFramebuffer(GLuint(s->output))) return;
    if(UnsafeQueryState(s->caps)) return;
    FullGuard guard(s->caps);ReplayScope scope;guard.Prepare();
    s->worldCopied=CopyWorldDepth(*s);
    if(s->worldCopied&&!s->reportedDepth) {
        SUN_LOG("SUN4 world depth copied: %dx%d format=0x%04X FBO=%d",s->viewport[2],s->viewport[3],s->worldDepthFormat,s->output);
        s->reportedDepth=true;
    }
    s->published=true;lastWorld=s;
    if(!s->shadow) return;
    const bool realDepth=s->worldCopied&&!s->replayCamera;
    if(!realDepth&&(!s->replayCamera||!s->cameraDraws)) return;
    float vp[16],inverse[16];Math::Multiply(s->cameraProjection,s->cameraView,vp);
    if(!EglPostFX::Math::Invert(vp,inverse)) return;
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_FALSE);s->caps.ColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_FALSE);s->caps.Blend(true);
    guard.BlendEquation(GL_FUNC_ADD,GL_FUNC_ADD);guard.BlendFunction(GL_ZERO,GL_SRC_COLOR,GL_ZERO,GL_ONE);
    glBindFramebuffer(GL_FRAMEBUFFER,GLuint(s->output));
    glViewport(s->viewport[0],s->viewport[1],s->viewport[2],s->viewport[3]);
    glUseProgram(s->program);s->caps.bind(s->vao);
    glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,realDepth ? s->world.texture:s->camera.depth);
    glActiveTexture(GL_TEXTURE1);glBindTexture(GL_TEXTURE_2D,s->sun.depth);
    const float texelMetres=2.0f*s->config.radius/float(s->resolution);
    const float depthPerMetre=1.0f/(6.0f*s->config.radius-0.1f); // LightCamera: near 0.1, far 6r
    glUniformMatrix4fv(s->inverseLocation,1,GL_FALSE,inverse);
    glUniformMatrix4fv(s->lightLocation,1,GL_FALSE,s->lightVP);
    glUniform4f(s->parametersLocation,1.0f/float(s->resolution),s->config.darkness*s->frame.sunlight,s->cameraClear,
                s->program3 ? 0.03f*depthPerMetre:0.0010f);
    glUniform4f(s->receiverLocation,texelMetres,depthPerMetre,s->config.softness,s->config.radius*1.6f);
    glUniform4f(s->sunLocation,s->stableSun[0],s->stableSun[1],s->stableSun[2],0.0f);
    glUniform4f(s->cameraLocation,s->cameraPosition[0],s->cameraPosition[1],s->cameraPosition[2],1.0f);
    if(s->texelLocation>=0) glUniform2f(s->texelLocation,1.0f/float(s->viewport[2]),1.0f/float(s->viewport[3]));
    originalArrays(GL_TRIANGLES,0,3);
    if(!s->reported) {
        SUN_LOG("SUN4 world shadows rendered: %d casters (%d over budget), depth=%s",
                s->casters,s->skipped,realDepth ? "copied":"replayed");
        s->reported=true;
    }
}
bool GetWorldView(WorldView& out) {
    out=WorldView{};
    auto s=lastWorld;
    if(!s||!s->published||!s->haveCamera||s->retired.load()) return false;
    if(eglGetCurrentContext()!=s->owner) return false;
    out.valid=true;
    if(s->worldCopied) {out.depthTexture=s->world.texture;out.width=s->world.width;out.height=s->world.height;}
    std::copy(s->cameraView,s->cameraView+16,out.view);
    std::copy(s->cameraProjection,s->cameraProjection+16,out.projection);
    out.clearDepth=s->cameraClear;
    return true;
}
void RequestWorldDepth(bool wanted) {depthWanted.store(wanted,std::memory_order_relaxed);}
void CleanupCurrent() {
    const Key key{eglGetCurrentDisplay(),eglGetCurrentContext()};
    std::shared_ptr<State> s;
    {std::lock_guard<std::mutex> lock(statesMutex);auto it=states.find(key);
        if(it==states.end()) return;s=it->second;states.erase(it);}
    if(active==s) active.reset();
    if(automatic==s) automatic.reset();
    if(lastWorld==s) lastWorld.reset();
    s->Destroy();
}
void Retire(EGLDisplay display,EGLContext context) {
    std::lock_guard<std::mutex> lock(statesMutex);auto it=states.find({display,context});
    if(it!=states.end()) it->second->retired=true;
}
void SubmitWorldEnd(const Frame& next) {
    if(GraphicsRenderThread::Ready()) {
        WorldEndSlot& slot=worldEndSlots[worldEndCursor.fetch_add(1,std::memory_order_relaxed)%8];
        bool idle=false;
        if(!slot.busy.compare_exchange_strong(idle,true,std::memory_order_acq_rel)) return;
        slot.next=next;
        if(!GraphicsRenderThread::Enqueue(&RunWorldEnd,&slot)) slot.busy.store(false,std::memory_order_release);
        return;
    }
    // Single-threaded queue (or bridge unavailable): this thread owns the
    // context, exactly as the original direct calls assumed.
    EndWorld();QueueFrame(next);
}
}
