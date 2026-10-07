#include "EglPostFX.h"
#include "ProjectionMath.h"
#include "LookProfile.h"
#include "ShaderSources.h"
#include "../sun/WorldSunShadow.h"
#include "../RenderThread.h"
#include "../GraphicsSettings.h"
#include "../TextureFilter.h"
#include "../WorldMsaa.h"
#include "../../modloader/HookScope.h"
#include "../../vendor/imgui/stb_image.h" // implementation compiled in main.cpp
#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <android/log.h> 
#include <dlfcn.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <exception>
#include <map>
#include <memory>
#include <mutex>
#include <new>
#include <utility>

#define FX_LOG(...) __android_log_print(ANDROID_LOG_INFO,"EglPostFX",__VA_ARGS__)
namespace EglPostFX {
namespace {
constexpr unsigned kUnits=12;
constexpr GLenum kSRGBWrite=0x8DB9; // GL_FRAMEBUFFER_SRGB_EXT.
using SwapFn=EGLBoolean(EGLAPIENTRY*)(EGLDisplay,EGLSurface);
using DamageFn=EGLBoolean(EGLAPIENTRY*)(EGLDisplay,EGLSurface,const EGLint*,EGLint);
using MakeCurrentFn=EGLBoolean(EGLAPIENTRY*)(EGLDisplay,EGLSurface,EGLSurface,EGLContext);
using DestroyContextFn=EGLBoolean(EGLAPIENTRY*)(EGLDisplay,EGLContext);
using DestroySurfaceFn=EGLBoolean(EGLAPIENTRY*)(EGLDisplay,EGLSurface);
using ReleaseFn=EGLBoolean(EGLAPIENTRY*)();
using GetProcFn=__eglMustCastToProperFunctionPointerType(EGLAPIENTRY*)(const char*);
SwapFn realSwap=nullptr;
DamageFn realDamageKHR=nullptr,realDamageEXT=nullptr;
MakeCurrentFn realMakeCurrent=nullptr;
DestroyContextFn realDestroyContext=nullptr;
DestroySurfaceFn realDestroySurface=nullptr;
ReleaseFn realReleaseThread=nullptr;
GetProcFn realGetProc=nullptr;
// Inline hooks through the client's ShadowHook (initialised SHARED in
// JNI_OnLoad). No AML runtime is needed or loaded.
using BackendHookFn=void*(*)(void*,void*,void**);
BackendHookFn backendHook=nullptr;
std::atomic<bool> installed{false};
thread_local bool insideSwap=false;
std::mutex settingsMutex;
Settings settings;
// Bumped by SetSettings: a changed option retries setups that failed before.
std::atomic<unsigned> settingsGeneration{1};
// Written by SetFrameEnvironment (render thread via SubmitBeforeHud, or any
// thread), read by Render() on the render thread. Not thread_local: the old
// thread_local copy was written on the game thread and never seen by GL.
std::mutex environmentMutex;
FrameEnvironment frameEnvironment;

template<class T> T Proc(const char* name) {
    return reinterpret_cast<T>(realGetProc ? realGetProc(name) : eglGetProcAddress(name));
}
bool Extension(const char* name) {
    GLint count=0; glGetIntegerv(GL_NUM_EXTENSIONS,&count);
    for (GLint i=0;i<count;++i) {
        const char* ext=reinterpret_cast<const char*>(glGetStringi(GL_EXTENSIONS,static_cast<GLuint>(i)));
        if (ext && std::strcmp(ext,name)==0) return true;
    }
    return false;
}
struct Capabilities {
    using EnableI=void(GL_APIENTRYP)(GLenum,GLuint);
    using IsEnabledI=GLboolean(GL_APIENTRYP)(GLenum,GLuint);
    using ColorMaskI=void(GL_APIENTRYP)(GLuint,GLboolean,GLboolean,GLboolean,GLboolean);
    EnableI enableI=nullptr,disableI=nullptr;
    IsEnabledI isEnabledI=nullptr;
    ColorMaskI colorMaskI=nullptr;
    bool es2=false,es3=false,es31=false,halfFloat=false,srgbWrite=false;
    void Init() {
        const char* v=reinterpret_cast<const char*>(glGetString(GL_VERSION));
        int major=0,minor=0;
        if (!v || std::sscanf(v,"OpenGL ES %d.%d",&major,&minor)!=2 || major<2) return;
        es2=true;if(major<3) return;
        es3=true;
        es31=major>3 || (major==3 && minor>=1);
        halfFloat=Extension("GL_EXT_color_buffer_half_float") || Extension("GL_EXT_color_buffer_float");
        srgbWrite=Extension("GL_EXT_sRGB_write_control");
        const bool coreIndexed=major>3 || (major==3 && minor>=2);
        if (coreIndexed || Extension("GL_EXT_draw_buffers_indexed")) {
            enableI=Proc<EnableI>(coreIndexed ? "glEnablei" : "glEnableiEXT");
            disableI=Proc<EnableI>(coreIndexed ? "glDisablei" : "glDisableiEXT");
            isEnabledI=Proc<IsEnabledI>(coreIndexed ? "glIsEnabledi" : "glIsEnablediEXT");
            colorMaskI=Proc<ColorMaskI>(coreIndexed ? "glColorMaski" : "glColorMaskiEXT");
            if (!enableI || !disableI || !isEnabledI || !colorMaskI)
                enableI=nullptr,disableI=nullptr,isEnabledI=nullptr,colorMaskI=nullptr;
        }
    }
};
void Toggle(GLenum state,GLboolean enabled) { if(enabled) glEnable(state); else glDisable(state); }

// Simpan hanya state yang disentuh modul, termasuk sampler (sering terlupakan).
// Tidak mengubah vertex attributes/array buffers, blend equations, depth func,
// depth mask, stencil mask, atau isi default depth buffer.
struct StateGuard {
    const Capabilities& caps;
    GLint readFbo=0,drawFbo=0,program=0,vao=0,active=0,unpack=0,alignment=4;
    GLint unpackRowLength=0,unpackSkipRows=0,unpackSkipPixels=0;
    GLint viewport[4]{},texture[kUnits]{},sampler[kUnits]{};
    GLboolean mask[4]{},blend=0,srgb=0,sampleMask=0;
    static constexpr GLenum switches[]={GL_DEPTH_TEST,GL_STENCIL_TEST,GL_CULL_FACE,
        GL_SCISSOR_TEST,GL_RASTERIZER_DISCARD,GL_SAMPLE_ALPHA_TO_COVERAGE,
        GL_SAMPLE_COVERAGE,GL_POLYGON_OFFSET_FILL,GL_DITHER};
    GLboolean enabled[sizeof(switches)/sizeof(switches[0])]{};
    explicit StateGuard(const Capabilities& c):caps(c) {
        glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&readFbo);
        glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&drawFbo);
        glGetIntegerv(GL_CURRENT_PROGRAM,&program); glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);
        glGetIntegerv(GL_ACTIVE_TEXTURE,&active); glGetIntegerv(GL_VIEWPORT,viewport);
        glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&unpack);
        glGetIntegerv(GL_UNPACK_ALIGNMENT,&alignment);
        glGetIntegerv(GL_UNPACK_ROW_LENGTH,&unpackRowLength);
        glGetIntegerv(GL_UNPACK_SKIP_ROWS,&unpackSkipRows);
        glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&unpackSkipPixels);
        for (unsigned i=0;i<kUnits;++i) {
            glActiveTexture(GL_TEXTURE0+i);
            glGetIntegerv(GL_TEXTURE_BINDING_2D,&texture[i]);
            glGetIntegerv(GL_SAMPLER_BINDING,&sampler[i]);
            glBindSampler(i,0);
        }
        if (caps.colorMaskI) {
            GLint m[4]{};glGetIntegeri_v(GL_COLOR_WRITEMASK,0,m);
            for(int i=0;i<4;++i) mask[i]=m[i]!=0;
            blend=caps.isEnabledI(GL_BLEND,0);caps.disableI(GL_BLEND,0);
            caps.colorMaskI(0,GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        } else {
            glGetBooleanv(GL_COLOR_WRITEMASK,mask);blend=glIsEnabled(GL_BLEND);
            glDisable(GL_BLEND);glColorMask(GL_TRUE,GL_TRUE,GL_TRUE,GL_TRUE);
        }
        for (unsigned i=0;i<sizeof(switches)/sizeof(switches[0]);++i) {
            enabled[i]=glIsEnabled(switches[i]);glDisable(switches[i]);
        }
        if(caps.srgbWrite) { srgb=glIsEnabled(kSRGBWrite);glEnable(kSRGBWrite); }
        // GL_SAMPLE_MASK (ES 3.1) dapat menutup sampel saat composite ke MSAA.
        if(caps.es31) { sampleMask=glIsEnabled(0x8E51);glDisable(0x8E51); }
        glActiveTexture(GL_TEXTURE0);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0);
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        // Dirt uploads use a tightly packed CPU array, regardless of game state.
        glPixelStorei(GL_UNPACK_ROW_LENGTH,0);
        glPixelStorei(GL_UNPACK_SKIP_ROWS,0);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
    }
    ~StateGuard() {
        glUseProgram(static_cast<GLuint>(program));glBindVertexArray(static_cast<GLuint>(vao));
        glBindFramebuffer(GL_READ_FRAMEBUFFER,static_cast<GLuint>(readFbo));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,static_cast<GLuint>(drawFbo));
        glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
        for(unsigned i=0;i<kUnits;++i) {
            glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,static_cast<GLuint>(texture[i]));
            glBindSampler(i,static_cast<GLuint>(sampler[i]));
        }
        glActiveTexture(static_cast<GLenum>(active));
        glBindBuffer(GL_PIXEL_UNPACK_BUFFER,static_cast<GLuint>(unpack));
        glPixelStorei(GL_UNPACK_ALIGNMENT,alignment);
        glPixelStorei(GL_UNPACK_ROW_LENGTH,unpackRowLength);
        glPixelStorei(GL_UNPACK_SKIP_ROWS,unpackSkipRows);
        glPixelStorei(GL_UNPACK_SKIP_PIXELS,unpackSkipPixels);
        if(caps.colorMaskI) {
            caps.colorMaskI(0,mask[0],mask[1],mask[2],mask[3]);
            if(blend) caps.enableI(GL_BLEND,0); else caps.disableI(GL_BLEND,0);
        } else { glColorMask(mask[0],mask[1],mask[2],mask[3]);Toggle(GL_BLEND,blend); }
        for(unsigned i=0;i<sizeof(switches)/sizeof(switches[0]);++i) Toggle(switches[i],enabled[i]);
        if(caps.srgbWrite) Toggle(kSRGBWrite,srgb);
        if(caps.es31) Toggle(0x8E51,sampleMask);
    }
};

enum Uniform { Source,Texel,Decode,Extract,ThresholdKnee,Direction,Scene,Depth,
    Projection,InvProjection,DepthRange,ClearDepth,ForwardSign,SSR,AO,SSRParams,
    AOParams,Bloom0,Bloom1,Bloom2,Effects,Dirt,HasDepth,Encode,Grade,BloomDirt,
    EffectSize,LightCount,LightPosition,LightColor,Time,Aspect,Detail,ShadowTint,
    HighlightTint,Fog,Sun,Atmosphere,InvView,SunDirection,SkyZenith,SkyHorizon,SunColor,
    Wet,DepthTexel,FXAA,State,DofTex,LUT,Haze,HazeBase,AutoExposure,Dof,Motion,
    PrevViewProj,Lut,Finish,Rays,Adapt,Focus,UniformCount };
constexpr const char* uniformNames[]={"uSource","uTexel","uDecodeSRGB","uExtract",
    "uThresholdKnee","uDirection","uScene","uDepth","uProjection","uInvProjection",
    "uDepthRange","uClearDepth","uForwardSign","uSSR","uAO","uSSRParams","uAOParams",
    "uBloom0","uBloom1","uBloom2","uEffects","uDirt","uHasDepth","uEncodeSRGB",
    "uGrade","uBloomDirt","uEffectSize","uLightCount","uLightPosition[0]",
    "uLightColor[0]","uTime","uAspect","uDetail","uShadowTint","uHighlightTint",
    "uFog","uSun","uAtmosphere","uInvView","uSunDirection","uSkyZenith","uSkyHorizon",
    "uSunColor","uWet","uDepthTexel","uFXAA","uState","uDofTex","uLUT","uHaze",
    "uHazeBase","uAutoExposure","uDof","uMotion","uPrevViewProj","uLut","uFinish","uRays",
    "uAdapt","uFocus"};
static_assert(sizeof(uniformNames)/sizeof(uniformNames[0])==UniformCount);
struct Program {
    GLuint id=0;GLint location[UniformCount]{};
    void Destroy() { if(id) glDeleteProgram(id);id=0; }
    GLint operator[](Uniform u) const { return location[u]; }
};
GLuint Compile(GLenum type,const char* text) {
    GLuint shader=glCreateShader(type);
    if(!shader) return 0;
    glShaderSource(shader,1,&text,nullptr);glCompileShader(shader);
    GLint ok=0;glGetShaderiv(shader,GL_COMPILE_STATUS,&ok);
    if(!ok) {
        char log[2048]{};glGetShaderInfoLog(shader,sizeof(log),nullptr,log);
        FX_LOG("Shader compile failed: %s",log);glDeleteShader(shader);return 0;
    }
    return shader;
}
bool Link(Program& p,GLuint vertex,const char* fragment) {
    GLuint fs=Compile(GL_FRAGMENT_SHADER,fragment);
    if(!fs) return false;
    p.id=glCreateProgram();
    if(!p.id) { glDeleteShader(fs);return false; }
    glAttachShader(p.id,vertex);glAttachShader(p.id,fs);
    glBindAttribLocation(p.id,0,"aPosition");glLinkProgram(p.id);
    GLint ok=0;glGetProgramiv(p.id,GL_LINK_STATUS,&ok);
    glDetachShader(p.id,vertex);glDetachShader(p.id,fs);glDeleteShader(fs);
    if(!ok) {
        char log[2048]{};glGetProgramInfoLog(p.id,sizeof(log),nullptr,log);
        FX_LOG("Shader link failed: %s",log);p.Destroy();return false;
    }
    for(int i=0;i<UniformCount;++i) p.location[i]=glGetUniformLocation(p.id,uniformNames[i]);
    return true;
}
#include "Gles2Pipeline.h"

struct Target {
    GLuint texture=0,fbo=0;int width=0,height=0;
    void Destroy() {
        if(fbo) glDeleteFramebuffers(1,&fbo);
        if(texture) glDeleteTextures(1,&texture);
        fbo=texture=0;width=height=0;
    }
    bool Create(int w,int h,GLenum format) {
        width=w;height=h;
        glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
        glTexStorage2D(GL_TEXTURE_2D,1,format,w,h);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
        const GLenum attachment=GL_COLOR_ATTACHMENT0;glDrawBuffers(1,&attachment);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) { Destroy();return false; }
        return true;
    }
};
// Environment eased on the render thread, so weather/sun changes blend in
// over time instead of switching in one frame.
struct SmoothEnvironment {
    bool initialised=false;
    std::chrono::steady_clock::time_point last{};
    FrameEnvironment environment;
    WeatherTraits traits;
    float sunVisibility=0.0f;
};
struct ContextState {
    Capabilities caps;
    bool checked=false,failed=false,ready=false,processedBeforeHud=false,reportedDepth=false,reportedFallback=false;
    FrameEnvironment environment;
    SmoothEnvironment smooth;
    float invView[16]{};
    bool haveView=false;
    // EGL checks that do not change for a surface: done once, not per frame.
    EGLSurface checkedSurface=EGL_NO_SURFACE;
    EGLConfig surfaceConfig=nullptr;
    bool surfaceUsable=false;
    CompatPipeline compat;
    std::atomic<bool> retiring{false};
    // GL_OUT_OF_MEMORY / incomplete FBO / failed swap are transient (texture-
    // heavy maps streaming in, surface resize): wait, then try again. Only
    // shaders that fail to build three times stop post-processing (failed).
    unsigned retryFrames=0,setupFailures=0,settingsSeen=0;
    EGLSurface gameSurface=EGL_NO_SURFACE; // Hanya diakses thread pemilik context.
    GLuint vao=0,dirt=0,depthTexture=0;
    int width=0,height=0;GLenum colorFormat=0,depthFormat=0;
    Target scene,bloom[3],ping[3],effects,atmosphere;
    Target state[2],dof;          // 1x1 frame state (exposure, focus), half-res DOF source
    Program downsample,blur,depthEffects,composite,atmosphereProgram,stateProgram;
    int stateIndex=0;bool stateValid=false,lowMemoryTargets=false;
    std::chrono::steady_clock::time_point stateTime{};
    GLuint lut=0;int lutSize=0;char lutLoaded[192]{};bool lutTried=false;
    // Motion blur: previous frame camera (world -> clip) and eye/forward for cut detection.
    float prevViewProj[16]{},prevEye[3]{},prevForward[3]{};bool havePrev=false,motionActive=false;
    float motionPrev[16]{};
    float projection[16]{},inverse[16]{},depthRange[2]{0,1},clearDepth=1,forwardSign=-1;
    void DestroyTargets() {
        // Scene FBO owns an attachment reference: delete it before the depth texture.
        scene.Destroy();for(auto& x:bloom) x.Destroy();for(auto& x:ping) x.Destroy();effects.Destroy();
        atmosphere.Destroy();state[0].Destroy();state[1].Destroy();dof.Destroy();
        stateValid=false;havePrev=false;
        if(depthTexture) glDeleteTextures(1,&depthTexture);
        depthTexture=0;depthFormat=0;width=height=0;
    }
    void Destroy() {
        DestroyTargets();downsample.Destroy();blur.Destroy();depthEffects.Destroy();composite.Destroy();
        atmosphereProgram.Destroy();stateProgram.Destroy();compat.Destroy();
        if(lut) glDeleteTextures(1,&lut);
        lut=0;lutSize=0;lutLoaded[0]=0;lutTried=false;
        if(vao) glDeleteVertexArrays(1,&vao);if(dirt) glDeleteTextures(1,&dirt);
        vao=dirt=0;ready=false;processedBeforeHud=false;
    }
    bool InitPrograms() {
        GLuint vs=Compile(GL_VERTEX_SHADER,Shaders::kVertex);
        if(!vs) return false;
        const bool ok=Link(downsample,vs,Shaders::kDownsample) && Link(blur,vs,Shaders::kBlur)
            && Link(depthEffects,vs,Shaders::kDepthEffects) && Link(composite,vs,Shaders::kComposite)
            && Link(atmosphereProgram,vs,Shaders::kAtmosphere) && Link(stateProgram,vs,Shaders::kState);
        glDeleteShader(vs);
        if(!ok) return false;
        glGenVertexArrays(1,&vao);
        // Dirt procedural ringan, sekali per context. Tidak perlu asset/runtime IO.
        std::array<unsigned char,128*128> pixels{};
        for(int y=0;y<128;++y) for(int x=0;x<128;++x) {
            float value=0.0f;unsigned seed=0x193b7u;
            for(int i=0;i<36;++i) {
                seed=1664525u*seed+1013904223u;float cx=float(seed&65535u)/65535.0f;
                seed=1664525u*seed+1013904223u;float cy=float(seed&65535u)/65535.0f;
                seed=1664525u*seed+1013904223u;float radius=0.007f+0.027f*float(seed&65535u)/65535.0f;
                float dx=(float(x)+0.5f)/128.0f-cx,dy=(float(y)+0.5f)/128.0f-cy;
                float q=1.0f-(dx*dx+dy*dy)/(radius*radius);
                value+=std::max(q,0.0f)*0.7f;
            }
            pixels[static_cast<size_t>(y*128+x)]=static_cast<unsigned char>(std::min(value,1.0f)*255.0f);
        }
        glGenTextures(1,&dirt);glBindTexture(GL_TEXTURE_2D,dirt);
        glTexImage2D(GL_TEXTURE_2D,0,GL_R8,128,128,0,GL_RED,GL_UNSIGNED_BYTE,pixels.data());
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        ready=true;return vao!=0 && dirt!=0;
    }
    bool Targets(int w,int h,GLenum color,bool lowMemory) {
        if(width==w && height==h && colorFormat==color && lowMemoryTargets==lowMemory && scene.fbo) return true;
        DestroyTargets();width=w;height=h;colorFormat=color;lowMemoryTargets=lowMemory;
        // Low-RAM phones: 8-bit bloom chain (half the memory and bandwidth).
        const GLenum working=caps.halfFloat && !lowMemory ? GL_RGBA16F : GL_RGBA8;
        if(!scene.Create(w,h,color)) return false;
        for(int i=0;i<3;++i) {
            int bw=std::max(1,w>>(i+1)),bh=std::max(1,h>>(i+1));
            if(!bloom[i].Create(bw,bh,working) || !ping[i].Create(bw,bh,working)) return false;
        }
        if(!effects.Create(std::max(1,w/2),std::max(1,h/2),working)) return false;
        if(!atmosphere.Create(std::max(1,w/4),std::max(1,h/4),working)) return false;
        // State is stored normalised (0..1), so 8-bit works too.
        if(!state[0].Create(1,1,working) || !state[1].Create(1,1,working)) return false;
        if(!dof.Create(std::max(1,w>>1),std::max(1,h>>1),working)) return false;
        FX_LOG("FBO %dx%d, bloom=%s",w,h,caps.halfFloat ? "RGBA16F" : "RGBA8");
        return true;
    }
    bool DepthStorage(GLenum format) {
        if(depthTexture && depthFormat==format) return true;
        if(depthTexture) glDeleteTextures(1,&depthTexture);
        depthTexture=0;depthFormat=0;
        glBindFramebuffer(GL_FRAMEBUFFER,scene.fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_DEPTH_ATTACHMENT,GL_TEXTURE_2D,0,0);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_STENCIL_ATTACHMENT,GL_TEXTURE_2D,0,0);
        glGenTextures(1,&depthTexture);glBindTexture(GL_TEXTURE_2D,depthTexture);
        glTexStorage2D(GL_TEXTURE_2D,1,format,width,height);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_COMPARE_MODE,GL_NONE);
        bool packed=format==GL_DEPTH24_STENCIL8 || format==GL_DEPTH32F_STENCIL8;
        GLenum attachment=packed ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
        glFramebufferTexture2D(GL_FRAMEBUFFER,attachment,GL_TEXTURE_2D,depthTexture,0);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) {
            glFramebufferTexture2D(GL_FRAMEBUFFER,attachment,GL_TEXTURE_2D,0,0);
            glDeleteTextures(1,&depthTexture);depthTexture=0;return false;
        }
        depthFormat=format;return true;
    }
};
using Key=std::pair<EGLDisplay,EGLContext>;
std::mutex statesMutex;
std::map<Key,std::shared_ptr<ContextState>> states;
std::shared_ptr<ContextState> CurrentState(bool create) {
    EGLDisplay d=eglGetCurrentDisplay();EGLContext c=eglGetCurrentContext();
    if(d==EGL_NO_DISPLAY || c==EGL_NO_CONTEXT) return {};
    std::lock_guard<std::mutex> lock(statesMutex);
    auto it=states.find({d,c});if(it!=states.end()) return it->second;
    if(!create) return {};
    auto state=std::make_shared<ContextState>();states.emplace(Key{d,c},state);return state;
}
void CleanupCurrent() {
    WorldSunShadow::CleanupCurrent();
    EGLDisplay d=eglGetCurrentDisplay();EGLContext c=eglGetCurrentContext();
    std::shared_ptr<ContextState> s;
    {
        std::lock_guard<std::mutex> lock(statesMutex);
        auto it=states.find({d,c});if(it==states.end()) return;
        s=it->second;states.erase(it);
    }
    // Selalu sebelum context dilepas/diganti. Jangan delete nama GL milik context lain.
    s->Destroy();
}
void Retire(EGLDisplay d,EGLContext c) {
    WorldSunShadow::Retire(d,c);
    std::lock_guard<std::mutex> lock(statesMutex);
    auto it=states.find({d,c});if(it==states.end()) return;
    // GL names die with the context (no GL calls in destructors). Erase so a
    // reused EGLContext handle starts fresh instead of staying "retiring".
    it->second->retiring=true;states.erase(it);
}
bool ReadProjection(GLuint program,float* projection) {
    if(!program) return false;
    const char* name="ProjMatrix";GLuint index=GL_INVALID_INDEX;
    glGetUniformIndices(program,1,&name,&index);if(index==GL_INVALID_INDEX) return false;
    GLint type=0,size=0;
    glGetActiveUniformsiv(program,1,&index,GL_UNIFORM_TYPE,&type);
    glGetActiveUniformsiv(program,1,&index,GL_UNIFORM_SIZE,&size);
    if(type!=GL_FLOAT_MAT4 || size!=1) return false;
    GLint location=glGetUniformLocation(program,name);if(location<0) return false;
    glGetUniformfv(program,location,projection);return true;
}
GLint Attachment(GLenum attachment,GLenum pname) {
    GLint value=0;glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER,attachment,pname,&value);return value;
}
GLenum DefaultColor(bool& srgb) {
    srgb=Attachment(GL_BACK,GL_FRAMEBUFFER_ATTACHMENT_COLOR_ENCODING)==GL_SRGB;
    GLint r=Attachment(GL_BACK,GL_FRAMEBUFFER_ATTACHMENT_RED_SIZE);
    GLint g=Attachment(GL_BACK,GL_FRAMEBUFFER_ATTACHMENT_GREEN_SIZE);
    GLint b=Attachment(GL_BACK,GL_FRAMEBUFFER_ATTACHMENT_BLUE_SIZE);
    GLint a=Attachment(GL_BACK,GL_FRAMEBUFFER_ATTACHMENT_ALPHA_SIZE);
    if(r==8 && g==8 && b==8 && a==8) return srgb ? GL_SRGB8_ALPHA8 : GL_RGBA8;
    if(r==8 && g==8 && b==8 && a==0) return srgb ? GL_SRGB8 : GL_RGB8;
    if(r==5 && g==6 && b==5 && a==0 && !srgb) return GL_RGB565;
    return 0; // Tidak menebak format resolve untuk RGB10/HDR/vendor formats.
}
GLenum DefaultDepth() {
    GLint type=Attachment(GL_DEPTH,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE);
    if(type==GL_NONE) return 0;
    GLint bits=Attachment(GL_DEPTH,GL_FRAMEBUFFER_ATTACHMENT_DEPTH_SIZE);
    GLint component=Attachment(GL_DEPTH,GL_FRAMEBUFFER_ATTACHMENT_COMPONENT_TYPE);
    GLint stencil=0;
    if(Attachment(GL_STENCIL,GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE)!=GL_NONE)
        stencil=Attachment(GL_STENCIL,GL_FRAMEBUFFER_ATTACHMENT_STENCIL_SIZE);
    if(component==GL_FLOAT && bits==32) return stencil==8 ? GL_DEPTH32F_STENCIL8 : (stencil==0 ? GL_DEPTH_COMPONENT32F : 0);
    if(component!=GL_UNSIGNED_NORMALIZED) return 0;
    if(bits==24 && stencil==8) return GL_DEPTH24_STENCIL8;
    if(stencil!=0) return 0;
    return bits==16 ? GL_DEPTH_COMPONENT16 : (bits==24 ? GL_DEPTH_COMPONENT24 : 0);
}
bool UnsafeDrawState() {
    GLboolean feedback=GL_FALSE;glGetBooleanv(GL_TRANSFORM_FEEDBACK_ACTIVE,&feedback);
    if(feedback) return true;
    for(GLenum target:{GL_ANY_SAMPLES_PASSED,GL_ANY_SAMPLES_PASSED_CONSERVATIVE,GL_TRANSFORM_FEEDBACK_PRIMITIVES_WRITTEN}) {
        GLint query=0;glGetQueryiv(target,GL_CURRENT_QUERY,&query);if(query) return true;
    }
    return false;
}
void Texture(unsigned slot,GLuint texture) {
    glActiveTexture(GL_TEXTURE0+slot);glBindTexture(GL_TEXTURE_2D,texture);
}
void Begin(const Target& t,const Program& p) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,t.fbo);glViewport(0,0,t.width,t.height);glUseProgram(p.id);
}
void Draw() { glDrawArrays(GL_TRIANGLES,0,3); }
void BloomPasses(ContextState& s,const Settings& cfg,bool decode) {
    GLuint input=s.scene.texture;int iw=s.width,ih=s.height;
    for(int i=0;i<3;++i) {
        Begin(s.bloom[i],s.downsample);Texture(0,input);
        glUniform1i(s.downsample[Source],0);glUniform2f(s.downsample[Texel],1.0f/iw,1.0f/ih);
        glUniform1i(s.downsample[Decode],i==0 && decode);
        glUniform1i(s.downsample[Extract],i==0);
        glUniform2f(s.downsample[ThresholdKnee],cfg.bloomThreshold,cfg.bloomKnee);Draw();
        Begin(s.ping[i],s.blur);Texture(0,s.bloom[i].texture);
        glUniform1i(s.blur[Source],0);glUniform2f(s.blur[Direction],1.0f/s.bloom[i].width,0);Draw();
        Begin(s.bloom[i],s.blur);Texture(0,s.ping[i].texture);
        glUniform2f(s.blur[Direction],0,1.0f/s.bloom[i].height);Draw();
        input=s.bloom[i].texture;iw=s.bloom[i].width;ih=s.bloom[i].height;
    }
}
// depth: texture holding this frame's world depth (copied at world end, or
// the default-framebuffer copy of the fallback path); dw/dh: its size.
void DepthPass(ContextState& s,const Settings& cfg,bool decode,GLuint depth,int dw,int dh) {
    const Program& p=s.depthEffects;Begin(s.effects,p);
    Texture(0,s.scene.texture);Texture(1,depth);
    glUniform1i(p[Scene],0);glUniform1i(p[Depth],1);glUniform1i(p[Decode],decode);
    glUniformMatrix4fv(p[Projection],1,GL_FALSE,s.projection);
    glUniformMatrix4fv(p[InvProjection],1,GL_FALSE,s.inverse);
    glUniform2fv(p[DepthRange],1,s.depthRange);glUniform1f(p[ClearDepth],s.clearDepth);
    glUniform1f(p[ForwardSign],s.forwardSign);glUniform2f(p[Texel],1.0f/float(dw),1.0f/float(dh));
    glUniform1i(p[SSR],cfg.ssr);glUniform1i(p[AO],cfg.ssao);
    glUniform4f(p[SSRParams],cfg.ssrStrength,cfg.ssrDistance,cfg.ssrThickness,0);
    glUniform2f(p[AOParams],cfg.aoStrength,cfg.aoRadius);Draw();
}
void AtmospherePass(ContextState& s,const LookProfile& look,bool decode,GLuint depth) {
    const Program& p=s.atmosphereProgram;Begin(s.atmosphere,p);Texture(0,s.scene.texture);
    Texture(1,depth ? depth : s.scene.texture);
    glUniform1i(p[Scene],0);glUniform1i(p[Depth],1);glUniform1i(p[HasDepth],depth!=0);
    glUniform1f(p[ClearDepth],s.clearDepth);
    glUniform1i(p[Decode],decode);glUniform4fv(p[Sun],1,look.sun);
    glUniform2f(p[Rays],look.config.rayLength,look.config.rayDecay);
    glUniform2f(p[Texel],1.0f/s.width,1.0f/s.height);glUniform1f(p[Aspect],float(s.width)/s.height);Draw();
}
// Colour LUT strip (size*size x size PNG, e.g. 256x16 / 1024x32 / 4096x64) from
// TESTLIT/graphics/<Folder>/<File>. Loaded once per path on the render thread.
void EnsureLut(ContextState& s,const Settings& cfg) {
    if(s.lutTried && std::strncmp(cfg.lutPath,s.lutLoaded,sizeof(s.lutLoaded))==0) return;
    s.lutTried=true;std::strncpy(s.lutLoaded,cfg.lutPath,sizeof(s.lutLoaded)-1);
    if(s.lut) glDeleteTextures(1,&s.lut);
    s.lut=0;s.lutSize=0;
    if(!cfg.lutPath[0]) return;
    int w=0,h=0,n=0;
    unsigned char* pixels=stbi_load(cfg.lutPath,&w,&h,&n,3);
    if(!pixels) { FX_LOG("LUT not loaded: %s",cfg.lutPath);return; }
    if(h<2 || h>64 || w!=h*h) {
        FX_LOG("LUT %s is %dx%d; expected size*size x size (256x16, 1024x32, 4096x64)",cfg.lutPath,w,h);
        stbi_image_free(pixels);return;
    }
    glGenTextures(1,&s.lut);glActiveTexture(GL_TEXTURE0);glBindTexture(GL_TEXTURE_2D,s.lut);
    glTexImage2D(GL_TEXTURE_2D,0,GL_RGB8,w,h,0,GL_RGB,GL_UNSIGNED_BYTE,pixels);
    stbi_image_free(pixels);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
    s.lutSize=h;
    FX_LOG("LUT loaded: %s (%d)",cfg.lutPath,h);
}
// 1x1 state: eye adaptation and the auto-focus distance, eased over time.
void StatePass(ContextState& s,const Settings& cfg,bool decode,GLuint depth) {
    const auto now=std::chrono::steady_clock::now();
    const float dt=s.stateValid ? std::clamp(std::chrono::duration<float>(now-s.stateTime).count(),0.0f,0.5f) : 0.0f;
    s.stateTime=now;
    const int write=s.stateIndex^1;
    const Program& p=s.stateProgram;Begin(s.state[write],p);
    Texture(0,s.scene.texture);Texture(1,depth ? depth : s.scene.texture);Texture(2,s.state[s.stateIndex].texture);
    glUniform1i(p[Scene],0);glUniform1i(p[Depth],1);glUniform1i(p[Source],2);
    glUniform1i(p[HasDepth],depth!=0);glUniform1i(p[Decode],decode);
    glUniform1f(p[ClearDepth],s.clearDepth);glUniform2fv(p[DepthRange],1,s.depthRange);
    glUniformMatrix4fv(p[InvProjection],1,GL_FALSE,s.inverse);
    const float exposureK=s.stateValid ? 1.0f-std::exp(-dt*cfg.aeSpeed) : 1.0f;
    const float focusK=s.stateValid ? 1.0f-std::exp(-dt*3.0f) : 1.0f;
    glUniform4f(p[Adapt],cfg.aeKey,cfg.aeMin,cfg.aeMax,exposureK);
    glUniform4f(p[Focus],focusK,30.0f,0.0f,0.0f);Draw();
    s.stateIndex=write;s.stateValid=true;
}
// Half-resolution blurred copy of the scene for depth of field.
void DofPass(ContextState& s,bool decode) {
    Begin(s.dof,s.downsample);Texture(0,s.scene.texture);
    glUniform1i(s.downsample[Source],0);glUniform2f(s.downsample[Texel],1.0f/s.width,1.0f/s.height);
    glUniform1i(s.downsample[Decode],decode);glUniform1i(s.downsample[Extract],0);
    glUniform2f(s.downsample[ThresholdKnee],1.0f,0.2f);Draw();
    Begin(s.ping[0],s.blur);Texture(0,s.dof.texture);
    glUniform1i(s.blur[Source],0);glUniform2f(s.blur[Direction],1.6f/s.dof.width,0);Draw();
    Begin(s.dof,s.blur);Texture(0,s.ping[0].texture);
    glUniform2f(s.blur[Direction],0,1.6f/s.dof.height);Draw();
}
void CompositePass(ContextState& s,const LookProfile& look,GLuint depthTexture,int dw,int dh,bool decode,bool encode) {
    const bool depth=depthTexture!=0;
    const auto& cfg=look.config;
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
    GLint oldDraw=GL_BACK;glGetIntegerv(GL_DRAW_BUFFER0,&oldDraw);
    const GLenum back=GL_BACK;glDrawBuffers(1,&back);
    glViewport(0,0,s.width,s.height);const Program& p=s.composite;glUseProgram(p.id);
    Texture(0,s.scene.texture);Texture(1,s.bloom[0].texture);Texture(2,s.bloom[1].texture);
    Texture(3,s.bloom[2].texture);Texture(4,s.effects.texture);
    Texture(5,depth ? depthTexture : s.scene.texture);Texture(6,s.dirt);
    Texture(7,s.atmosphere.texture);glUniform1i(p[Atmosphere],7);
    const bool dofActive=depth && cfg.dofStrength>0.0f && s.stateValid;
    const bool lutActive=s.lut!=0 && cfg.lutStrength>0.0f;
    Texture(8,s.state[s.stateIndex].texture);Texture(9,dofActive ? s.dof.texture : s.scene.texture);
    Texture(10,lutActive ? s.lut : s.scene.texture);
    glUniform1i(p[State],8);glUniform1i(p[DofTex],9);glUniform1i(p[LUT],10);
    glUniform1i(p[Scene],0);glUniform1i(p[Bloom0],1);glUniform1i(p[Bloom1],2);
    glUniform1i(p[Bloom2],3);glUniform1i(p[Effects],4);glUniform1i(p[Depth],5);glUniform1i(p[Dirt],6);
    glUniform1i(p[HasDepth],depth);glUniform1i(p[Decode],decode);glUniform1i(p[Encode],encode);
    glUniform4f(p[Grade],cfg.exposure,cfg.saturation,cfg.contrast,cfg.toneMix);
    glUniform4fv(p[Detail],1,look.detail);glUniform4fv(p[Fog],1,look.fog);
    glUniform3fv(p[ShadowTint],1,look.shadow);glUniform3fv(p[HighlightTint],1,look.highlight);
    glUniform2f(p[Texel],1.0f/s.width,1.0f/s.height);glUniform1f(p[ClearDepth],s.clearDepth);
    glUniform2f(p[BloomDirt],cfg.bloomStrength,cfg.dirtStrength);
    glUniform2f(p[EffectSize],float(s.effects.width),float(s.effects.height));
    glUniform2fv(p[DepthRange],1,s.depthRange);glUniformMatrix4fv(p[InvProjection],1,GL_FALSE,s.inverse);
    // Sky and wet roads need world space; without a camera they stay off.
    const bool world=depth&&s.haveView;
    static const float identity[16]={1,0,0,0,0,1,0,0,0,0,1,0,0,0,0,1};
    const float zero[4]{};
    glUniformMatrix4fv(p[InvView],1,GL_FALSE,world ? s.invView : identity);
    glUniform4fv(p[SunDirection],1,look.sunDirection);
    glUniform4fv(p[SkyZenith],1,world ? look.skyZenith : zero);
    glUniform4fv(p[SkyHorizon],1,world ? look.skyHorizon : zero);
    glUniform4fv(p[SunColor],1,world ? look.sunColor : zero);
    glUniform4fv(p[Wet],1,world ? look.wet : zero);
    glUniform2f(p[DepthTexel],1.0f/float(std::max(dw,1)),1.0f/float(std::max(dh,1)));
    glUniform1i(p[FXAA],cfg.fxaa ? 1 : 0);
    glUniform4fv(p[Haze],1,world ? look.haze : zero);
    glUniform4fv(p[HazeBase],1,world ? look.hazeBase : zero);
    glUniform4f(p[AutoExposure],s.stateValid ? cfg.autoExposure : 0.0f,0,0,0);
    glUniform4f(p[Dof],dofActive ? cfg.dofStrength : 0.0f,0,0,0);
    glUniform4f(p[Motion],cfg.motionBlur,0.035f,6.0f,world && s.motionActive && cfg.motionBlur>0.0f ? 1.0f : 0.0f);
    glUniformMatrix4fv(p[PrevViewProj],1,GL_FALSE,s.motionPrev);
    glUniform4f(p[Lut],cfg.lutStrength,float(std::max(s.lutSize,2)),0,lutActive ? 1.0f : 0.0f);
    glUniform4f(p[Finish],cfg.grain,cfg.dither,cfg.gradeStrength,0);
    float positions[16]{},colors[16]{};
    for(int i=0;i<cfg.lightCount;++i) {
        const auto& l=cfg.lights[static_cast<size_t>(i)];
        positions[4*i]=l.uv[0];positions[4*i+1]=l.uv[1];positions[4*i+2]=l.radius;positions[4*i+3]=l.intensity;
        std::copy(l.color,l.color+3,colors+4*i);colors[4*i+3]=l.pulse;
    }
    glUniform1i(p[LightCount],cfg.lightCount);
    glUniform4fv(p[LightPosition],4,positions);glUniform4fv(p[LightColor],4,colors);
    static const auto epoch=std::chrono::steady_clock::now();
    float time=std::chrono::duration<float>(std::chrono::steady_clock::now()-epoch).count();
    glUniform1f(p[Time],std::fmod(time,6000.0f));glUniform1f(p[Aspect],float(s.width)/float(s.height));Draw();
    const GLenum restore=static_cast<GLenum>(oldDraw);glDrawBuffers(1,&restore);
}


// Effects dropped first when the adaptive level rises (render thread).
Settings AdaptiveSettings(Settings c) {
    const int level=GraphicsSettings::AdaptiveLevel();
    if(level>=1) { c.ssao=false;c.ssr=false;c.motionBlur=0;c.dofStrength=0; }
    if(level>=2) { c.sunShafts*=0.5f;c.lensFlare*=0.5f;c.clarity=0; }
    if(level>=3) { c.autoExposure=0;c.lutStrength=0; }
    return c;
}
FrameEnvironment CurrentEnvironment() {
    std::lock_guard<std::mutex> lock(environmentMutex);return frameEnvironment;
}
float Ease(float dt,float seconds) { return dt<=0.0f ? 0.0f : 1.0f-std::exp(-dt/seconds); }
void Approach(float& value,float target,float k) { value+=(target-value)*k; }
// Sun position from this frame's world camera and GTA's sun vector. Exact
// even when CCoronas::SunScreenX/Y is stale (sun behind the camera).
// Returns the on-screen weight: 1 inside, fading to 0 a quarter screen outside.
float SunOnScreen(const WorldSunShadow::WorldView& world,const float* toSun,float* uv) {
    const float* v=world.view;const float* p=world.projection;
    float eye[4];
    for(int r=0;r<4;++r) eye[r]=v[r]*toSun[0]+v[4+r]*toSun[1]+v[8+r]*toSun[2]; // w = 0: direction
    float clip[4];
    for(int r=0;r<4;++r) clip[r]=p[r]*eye[0]+p[4+r]*eye[1]+p[8+r]*eye[2]+p[12+r]*eye[3];
    if(!std::isfinite(clip[3]) || clip[3]<=1e-4f) return 0.0f;
    uv[0]=clip[0]/clip[3]*0.5f+0.5f;uv[1]=clip[1]/clip[3]*0.5f+0.5f;
    if(!std::isfinite(uv[0]) || !std::isfinite(uv[1])) return 0.0f;
    const float edge=std::min(std::min(uv[0],uv[1]),std::min(1.0f-uv[0],1.0f-uv[1]));
    uv[0]=std::clamp(uv[0],-1.0f,2.0f);uv[1]=std::clamp(uv[1],-1.0f,2.0f);
    return Smooth(-0.25f,0.02f,edge);
}
void UpdateEnvironment(ContextState& s,const FrameEnvironment& input,const WorldSunShadow::WorldView* world) {
    auto& m=s.smooth;
    FrameEnvironment e=CleanEnvironment(input);
    float onScreen=e.sunVisible ? 1.0f : 0.0f;
    if(world && world->valid) {
        float uv[2]{};
        const float weight=SunOnScreen(*world,e.toSun,uv);
        if(weight>0.0f) { e.sunUV[0]=uv[0];e.sunUV[1]=uv[1]; }
        else if(m.initialised) { e.sunUV[0]=m.environment.sunUV[0];e.sunUV[1]=m.environment.sunUV[1]; }
        onScreen*=weight;
    }
    // traits.clear already encodes cloudy/rainy weather; CloudCoverage is 1.0
    // for most weathers (CWeather::Update), so it only trims a little now
    // (it was counted twice: sun rays/glow vanished in almost every weather).
    const bool followWeather=GetSettings().weatherLook;
    const WeatherTraits traits=followWeather ? EnvironmentTraits(e) : WeatherTraits{};
    const float clear=followWeather && !(e.oldWeather<0&&e.newWeather<0) ? traits.clear : 0.8f;
    const float target=onScreen*Unit(0.3f+0.7f*clear)
        *(1.0f-(followWeather ? 0.30f : 0.0f)*e.cloud)*Smooth(-0.02f,0.08f,e.toSun[2]);
    const auto now=std::chrono::steady_clock::now();
    if(!m.initialised) {
        m.initialised=true;m.last=now;m.environment=e;m.traits=traits;m.sunVisibility=target;
        s.environment=e;return;
    }
    const float dt=std::clamp(std::chrono::duration<float>(now-m.last).count(),0.0f,0.25f);
    m.last=now;
    auto& v=m.environment;
    Approach(v.rain,e.rain,Ease(dt,1.0f));Approach(v.wetness,e.wetness,Ease(dt,1.5f));
    Approach(v.cloud,e.cloud,Ease(dt,2.0f));Approach(v.fog,e.fog,Ease(dt,2.0f));
    Approach(v.underwater,e.underwater,Ease(dt,0.12f));Approach(v.tunnel,e.tunnel,Ease(dt,0.3f));
    Approach(v.sunGlare,e.sunGlare,Ease(dt,0.6f));
    v.valid=e.valid;v.hour=e.hour;v.sunVisible=e.sunVisible;
    v.sunUV[0]=e.sunUV[0];v.sunUV[1]=e.sunUV[1];
    std::copy(e.toSun,e.toSun+3,v.toSun);
    v.oldWeather=e.oldWeather;v.newWeather=e.newWeather;v.weatherBlend=e.weatherBlend;
    const float k=Ease(dt,1.5f);
    auto& t=m.traits;
    Approach(t.clear,traits.clear,k);Approach(t.overcast,traits.overcast,k);Approach(t.rain,traits.rain,k);
    Approach(t.fog,traits.fog,k);Approach(t.smog,traits.smog,k);Approach(t.sand,traits.sand,k);
    Approach(t.warm,traits.warm,k);Approach(t.cool,traits.cool,k);
    Approach(m.sunVisibility,target,Ease(dt,0.35f));
    s.environment=v;
}

bool Render(EGLDisplay display,EGLSurface surface,bool beforeHud,const float* suppliedProjection) {
    if(!installed.load() || eglGetCurrentContext()==EGL_NO_CONTEXT ||
       eglGetCurrentDisplay()!=display || eglGetCurrentSurface(EGL_DRAW)!=surface) return false;
    // Registrasi eksplisit dari batas render game; jangan post-process surface
    // HWUI/launcher/WebView lain yang kebetulan memakai EGL dalam proses sama.
    auto state=CurrentState(beforeHud);
    if(!state || state->retiring.load()) return false;
    ContextState& s=*state;
    if(beforeHud) s.gameSurface=surface;
    if(s.gameSurface!=surface) return false;
    if(!beforeHud && s.processedBeforeHud) return true;
    if(beforeHud && s.processedBeforeHud) return false;
    // Camera + depth of the world pass that just ended (render thread, same frame).
    WorldSunShadow::WorldView world;
    const bool haveWorld=beforeHud && WorldSunShadow::GetWorldView(world);
    if(beforeHud) UpdateEnvironment(s,CurrentEnvironment(),haveWorld ? &world : nullptr);
    const LookProfile look=BuildLook(AdaptiveSettings(GetSettings()),s.environment,s.smooth.traits,s.smooth.sunVisibility);
    const Settings& cfg=look.config;if(!cfg.enabled) return false;
    const unsigned generation=settingsGeneration.load(std::memory_order_relaxed);
    if(s.settingsSeen!=generation) { s.settingsSeen=generation;s.retryFrames=0;s.setupFailures=0;s.failed=false; }
    if(s.retryFrames) { --s.retryFrames;return false; }
    // Hanya window surface, bukan EGL pbuffer milik CEF/video/background task.
    if(s.checkedSurface!=surface) {
        s.checkedSurface=surface;s.surfaceUsable=false;s.surfaceConfig=nullptr;
        EGLint configId=0,count=0,renderBuffer=0;EGLConfig found=nullptr;
        if(eglQuerySurface(display,surface,EGL_CONFIG_ID,&configId)) {
            const EGLint attributes[]={EGL_CONFIG_ID,configId,EGL_NONE};
            if(eglChooseConfig(display,attributes,&found,1,&count) && count==1 &&
               eglQuerySurface(display,surface,EGL_RENDER_BUFFER,&renderBuffer) && renderBuffer==EGL_BACK_BUFFER) {
                s.surfaceConfig=found;s.surfaceUsable=true;
            }
        }
    }
    if(!s.surfaceUsable) return false;
    const EGLConfig config=s.surfaceConfig;
    EGLint w=0,h=0;
    if(!eglQuerySurface(display,surface,EGL_WIDTH,&w) || !eglQuerySurface(display,surface,EGL_HEIGHT,&h) || w<2 || h<2) return false;
    if(!s.checked) {
        s.caps.Init();s.checked=true;
        FX_LOG("REF3 context: %s; renderer=%s; backend=%s",glGetString(GL_VERSION),glGetString(GL_RENDERER),
               s.caps.es3 ? "GLES3 + optional depth" : "GLES2 color compatibility");
    }
    if(!s.caps.es2 || s.failed || (s.caps.es3 && UnsafeDrawState())) return false;
    GLint draw=0,limit=0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING,&draw);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE,&limit);
    if(w>limit || h>limit) return false;
    // Pre-HUD pass only on the default framebuffer, i.e. after
    // emu_FlushAltRenderTarget copied the (possibly scaled) world to the screen.
    // The viewport is set by the passes and restored by StateGuard.
    if(beforeHud && draw!=0) {
        if(!s.reportedFallback) {
            FX_LOG("REF3: pre-HUD target is FBO %d, not the window; post-processing skipped this frame",draw);
            s.reportedFallback=true;
        }
        return false;
    }
    if(!s.caps.es3) {
        EGLint alpha=0;eglGetConfigAttrib(display,config,EGL_ALPHA_SIZE,&alpha);
        // EGL_KHR_gl_colorspace enum, queried only when the extension exists.
        const char* extensions=eglQueryString(display,EGL_EXTENSIONS);
        EGLint colorSpace=0;
        if(extensions && std::strstr(extensions,"EGL_KHR_gl_colorspace"))
            eglQuerySurface(display,surface,0x309D,&colorSpace);
        bool compatFailed=false;
        bool rendered=s.compat.Render(w,h,alpha ? GL_RGBA : GL_RGB,colorSpace==0x3089,look,compatFailed);
        if(compatFailed) {
            if(++s.setupFailures>=3) s.failed=true;else s.retryFrames=180;
            FX_LOG("PostFX GLES2 setup failed %ux%s",s.setupFailures,s.failed ? ", disabled" : ", retry in 180 frames");
        }
        if(rendered && beforeHud) s.processedBeforeHud=true;
        return rendered;
    }
    StateGuard guard(s.caps);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,0);
    if(glCheckFramebufferStatus(GL_DRAW_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) return false;
    glBindFramebuffer(GL_READ_FRAMEBUFFER,0);
    GLint defaultRead=0,defaultDraw=0;
    glGetIntegerv(GL_READ_BUFFER,&defaultRead);glGetIntegerv(GL_DRAW_BUFFER0,&defaultDraw);
    // ES ReadBuffer/DrawBuffers tidak menerima GL_FRONT. Tolak surface yang
    // melaporkan buffer selain BACK/NONE sebelum menulis satu piksel pun.
    if((defaultRead!=GL_BACK && defaultRead!=GL_NONE) ||
       (defaultDraw!=GL_BACK && defaultDraw!=GL_NONE)) return false;
    bool srgb=false;GLenum color=DefaultColor(srgb);if(!color) return false;
    GLint samples=0;glGetIntegerv(GL_SAMPLES,&samples);
    const bool wantDepth=cfg.ssr || cfg.ssao || cfg.fogStrength>0 || cfg.skyStrength>0
        || cfg.wetStrength>0 || cfg.sunShafts>0 || cfg.motionBlur>0 || cfg.dofStrength>0;
    GLuint depthTexture=0;int depthWidth=w,depthHeight=h;
    s.haveView=false;
    if(beforeHud && wantDepth && haveWorld) {
        // Real world depth, copied by WorldSunShadow before the alt render
        // target was flushed: correct with any Render Scale.
        s.haveView=Math::Invert(world.view,s.invView);
        if(world.depthTexture) {
            std::copy(world.projection,world.projection+16,s.projection);
            s.depthRange[0]=0.0f;s.depthRange[1]=1.0f;s.clearDepth=world.clearDepth;
            if(Math::Projection(s.projection,s.inverse,s.forwardSign)) {
                depthTexture=world.depthTexture;depthWidth=world.width;depthHeight=world.height;
            }
        }
    }
    // Old path, only without a world capture: the window's own depth buffer.
    const GLenum depthFormat=beforeHud && wantDepth && !haveWorld && samples==0 ? DefaultDepth() : 0;
    bool windowDepth=false;
    if(depthFormat) {
        if(suppliedProjection) std::copy(suppliedProjection,suppliedProjection+16,s.projection);
        bool matrix=suppliedProjection || ReadProjection(static_cast<GLuint>(guard.program),s.projection);
        glGetFloatv(GL_DEPTH_RANGE,s.depthRange);glGetFloatv(GL_DEPTH_CLEAR_VALUE,&s.clearDepth);
        windowDepth=matrix && Math::Projection(s.projection,s.inverse,s.forwardSign)
            && std::abs(s.depthRange[1]-s.depthRange[0])>1e-6f;
    }
    if(!s.ready && !s.InitPrograms()) {
        s.Destroy();
        if(++s.setupFailures>=3) s.failed=true;else s.retryFrames=120;
        FX_LOG("PostFX shaders failed %ux%s",s.setupFailures,s.failed ? ", disabled" : ", retry in 120 frames");
        return false;
    }
    if(!s.Targets(w,h,color,cfg.lowMemory)) {
        s.DestroyTargets();s.retryFrames=180;
        FX_LOG("PostFX targets %dx%d unavailable (GPU memory), retry in 180 frames",w,h);
        return false;
    }
    if(windowDepth) windowDepth=s.DepthStorage(depthFormat);
    if(windowDepth) depthTexture=s.depthTexture;
    glBindVertexArray(s.vao);
    // Copy/resolve warna GPU -> GPU. Tidak ada readback/glFinish.
    glBindFramebuffer(GL_READ_FRAMEBUFFER,0);
    GLint oldRead=GL_BACK;glGetIntegerv(GL_READ_BUFFER,&oldRead);glReadBuffer(GL_BACK);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER,s.scene.fbo);
    glBlitFramebuffer(0,0,w,h,0,0,w,h,GL_COLOR_BUFFER_BIT,GL_NEAREST);
    if(windowDepth) glBlitFramebuffer(0,0,w,h,0,0,w,h,GL_DEPTH_BUFFER_BIT,GL_NEAREST);
    glReadBuffer(static_cast<GLenum>(oldRead));
    const bool decode=cfg.sourceIsSRGB && !srgb;
    // Motion blur: previous camera of this context. A cut (teleport, respawn,
    // camera switch: > 8 m or > 25 degrees in one frame) blurs nothing.
    s.motionActive=false;
    if(s.haveView && haveWorld) {
        float vp[16];
        for(int c=0;c<4;++c) for(int r=0;r<4;++r) {
            float v=0;for(int k=0;k<4;++k) v+=world.projection[k*4+r]*world.view[c*4+k];
            vp[c*4+r]=v;
        }
        const float* eye=s.invView+12;const float* forward=s.invView+8;
        if(s.havePrev) {
            const float dx=eye[0]-s.prevEye[0],dy=eye[1]-s.prevEye[1],dz=eye[2]-s.prevEye[2];
            const float turn=forward[0]*s.prevForward[0]+forward[1]*s.prevForward[1]+forward[2]*s.prevForward[2];
            s.motionActive=dx*dx+dy*dy+dz*dz<64.0f && turn>0.906f;
            std::copy(s.prevViewProj,s.prevViewProj+16,s.motionPrev);
        }
        std::copy(vp,vp+16,s.prevViewProj);std::copy(eye,eye+3,s.prevEye);std::copy(forward,forward+3,s.prevForward);
        s.havePrev=true;
    } else s.havePrev=false;
    EnsureLut(s,cfg);
    BloomPasses(s,cfg,decode);
    if(cfg.autoExposure>0 || cfg.dofStrength>0) StatePass(s,cfg,decode,depthTexture);
    else s.stateValid=false;
    if(depthTexture && cfg.dofStrength>0 && s.stateValid) DofPass(s,decode);
    if(depthTexture && (cfg.ssr || cfg.ssao)) DepthPass(s,cfg,decode,depthTexture,depthWidth,depthHeight);
    else {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER,s.effects.fbo);
        const GLfloat neutral[]={0,0,0,1};glClearBufferfv(GL_COLOR,0,neutral);
    }
    AtmospherePass(s,look,decode,depthTexture);
    CompositePass(s,look,depthTexture,depthWidth,depthHeight,decode,!srgb);
    if(beforeHud) s.processedBeforeHud=true;
    if(!s.reportedDepth && beforeHud) {
        FX_LOG("Scene boundary: depth=%s (%dx%d); camera=%s; MSAA=%d",
               depthTexture ? (windowDepth ? "window" : "world copy") : "bypass",depthWidth,depthHeight,
               s.haveView ? "world" : "none",samples);
        s.reportedDepth=true;
    }
    return true;
}
bool TryRender(EGLDisplay d,EGLSurface s,bool early,const float* p=nullptr) noexcept {
    try { return Render(d,s,early,p); }
    catch(const std::exception& e) { FX_LOG("PostFX skipped: %s",e.what());return false; }
    catch(...) { FX_LOG("PostFX skipped: unknown error");return false; }
}
// Adaptive quality: frame rate of GTA's own swaps in 2 s windows. Too slow
// twice in a row -> one level lighter; fast for 10 s -> one level back.
void MeasureFrame() {
    if(!GraphicsRenderThread::OnRenderThread()) return;
    static std::chrono::steady_clock::time_point start{};
    static int frames=0,slow=0,fast=0;
    const auto now=std::chrono::steady_clock::now();
    if(start==std::chrono::steady_clock::time_point{}) { start=now;frames=0;return; }
    ++frames;
    const float elapsed=std::chrono::duration<float>(now-start).count();
    if(elapsed<2.0f) return;
    const float fps=float(frames)/elapsed;
    start=now;frames=0;
    if(elapsed>6.0f || !GraphicsSettings::AdaptiveEnabled()) { slow=fast=0;return; } // paused / off
    const int target=GraphicsSettings::TargetFps();
    const int level=GraphicsSettings::AdaptiveLevel();
    if(fps<float(target)*0.80f) {
        fast=0;
        if(++slow>=2 && level<3) {
            slow=0;GraphicsSettings::SetAdaptiveLevel(level+1);
            FX_LOG("adaptive quality: %.0f FPS (target %d) -> level %d",fps,target,level+1);
        }
    } else if(fps>float(target)*0.95f) {
        slow=0;
        if(++fast>=5 && level>0) {
            fast=0;GraphicsSettings::SetAdaptiveLevel(level-1);
            FX_LOG("adaptive quality: %.0f FPS (target %d) -> level %d",fps,target,level-1);
        }
    } else slow=fast=0;
}
void EndFrame(EGLBoolean result) {
    MeasureFrame();
    WorldSunShadow::AfterSwap(result);
    auto s=CurrentState(false);if(!s) return;
    s->processedBeforeHud=false;
    // Tidak memanggil eglGetError (error tetap dapat dibaca oleh game).
    // Gagal swap (BAD_ALLOC/BAD_SURFACE saat memori penuh atau resize) bersifat
    // sementara: cek ulang surface dan coba lagi, jangan mati permanen.
    if(result==EGL_FALSE) {
        s->checkedSurface=EGL_NO_SURFACE;s->surfaceUsable=false;
        s->retryFrames=std::max(s->retryFrames,30u);
    }
}
// With the render-thread bridge, post-processing runs before the HUD every
// frame. The swap fallback (which would also grade HUD/chat/CEF/dialogs and,
// alternating with the pre-HUD path, make them flicker) is only used when
// the bridge could not be installed.
bool SwapFallbackAllowed() { return !GraphicsRenderThread::Ready(); }
struct SwapScope { SwapScope(){insideSwap=true;} ~SwapScope(){insideSwap=false;} };
EGLBoolean EGLAPIENTRY HookSwap(EGLDisplay d,EGLSurface surface) {
    ML_HOOK_SCOPE();
    if(!realSwap) return EGL_FALSE;
    if(insideSwap || !installed.load()) return realSwap(d,surface);
    SwapScope scope;
    if(SwapFallbackAllowed()) TryRender(d,surface,false);
    EGLBoolean result=realSwap(d,surface);EndFrame(result);return result;
}
EGLBoolean SwapDamage(DamageFn fn,EGLDisplay d,EGLSurface surface,const EGLint* rects,EGLint n) {
    if(!fn) return EGL_FALSE;
    if(insideSwap || !installed.load()) return fn(d,surface,rects,n);
    SwapScope scope;bool changed=SwapFallbackAllowed() && TryRender(d,surface,false);
    EGLint w=0,h=0;
    if(changed && eglQuerySurface(d,surface,EGL_WIDTH,&w) && eglQuerySurface(d,surface,EGL_HEIGHT,&h)) {
        const EGLint full[]={0,0,w,h};EGLBoolean result=fn(d,surface,full,1);EndFrame(result);return result;
    }
    EGLBoolean result=fn(d,surface,rects,n);EndFrame(result);return result;
}
EGLBoolean EGLAPIENTRY HookDamageKHR(EGLDisplay d,EGLSurface s,const EGLint* r,EGLint n) {
    ML_HOOK_SCOPE();return SwapDamage(realDamageKHR,d,s,r,n);
}
EGLBoolean EGLAPIENTRY HookDamageEXT(EGLDisplay d,EGLSurface s,const EGLint* r,EGLint n) {
    ML_HOOK_SCOPE();return SwapDamage(realDamageEXT,d,s,r,n);
}
EGLBoolean EGLAPIENTRY HookMakeCurrent(EGLDisplay d,EGLSurface draw,EGLSurface read,EGLContext c) {
    ML_HOOK_SCOPE();
    if(!realMakeCurrent) return EGL_FALSE;
    if(!installed.load()) return realMakeCurrent(d,draw,read,c);
    if(eglGetCurrentContext()!=c || eglGetCurrentDisplay()!=d ||
       eglGetCurrentSurface(EGL_DRAW)!=draw || eglGetCurrentSurface(EGL_READ)!=read) CleanupCurrent();
    return realMakeCurrent(d,draw,read,c);
}
EGLBoolean EGLAPIENTRY HookDestroyContext(EGLDisplay d,EGLContext c) {
    ML_HOOK_SCOPE();
    if(!realDestroyContext) return EGL_FALSE;
    if(!installed.load()) return realDestroyContext(d,c);
    if(d==eglGetCurrentDisplay() && c==eglGetCurrentContext()) CleanupCurrent();
    else Retire(d,c); // Context current di thread lain: cleanup saat thread pemilik melepasnya.
    return realDestroyContext(d,c);
}
EGLBoolean EGLAPIENTRY HookDestroySurface(EGLDisplay d,EGLSurface s) {
    ML_HOOK_SCOPE();
    if(!realDestroySurface) return EGL_FALSE;
    if(!installed.load()) return realDestroySurface(d,s);
    if(d==eglGetCurrentDisplay() && (s==eglGetCurrentSurface(EGL_DRAW) || s==eglGetCurrentSurface(EGL_READ))) CleanupCurrent();
    return realDestroySurface(d,s);
}
EGLBoolean EGLAPIENTRY HookReleaseThread() {
    ML_HOOK_SCOPE();
    if(!realReleaseThread) return EGL_FALSE;
    if(installed.load()) CleanupCurrent();
    return realReleaseThread();
}
void* ShadowHookBackend(void* symbol,void* replacement,void** original) {
    void* stub=shadowhook_hook_func_addr(symbol,replacement,original);
    if(!stub) {
        const int error=shadowhook_get_errno();
        FX_LOG("ShadowHook failed (%d): %s",error,shadowhook_to_errmsg(error));
    }
    return stub;
}
bool OpenHookBackend() {
    // ShadowHook is initialised (SHARED) by JNI_OnLoad before InstallHooks.
    // The libEGL/libGLESv2 entry points are patched in place, so callers that
    // resolved them through eglGetProcAddress or a PLT reach the hooks too:
    // no eglGetProcAddress hook and no AML/GlossHook runtime are needed.
    backendHook=&ShadowHookBackend;
    FX_LOG("PostFX: client ShadowHook backend (mode %s), no AML",
           shadowhook_get_mode()==SHADOWHOOK_MODE_SHARED ? "shared" : "unique");
    return true;
}
void* ResolveEgl(void* handle,const char* name) {
    void* symbol=dlsym(handle,name);
    if(!symbol && realGetProc) symbol=reinterpret_cast<void*>(realGetProc(name));
    return symbol;
}
template<class Function> bool Hook(void* symbol,const char* name,Function replacement,Function& original) {
    if(!symbol || !backendHook) return false;
    void* hook=backendHook(symbol,reinterpret_cast<void*>(replacement),reinterpret_cast<void**>(&original));
    if(!hook || !original) {
        FX_LOG("PostFX disabled: hook failed for %s",name);return false;
    }
    return true;
}
float Clamp(float v,float lo,float hi,float fallback) { return std::isfinite(v) ? std::clamp(v,lo,hi) : fallback; }
// Render-thread side of SubmitBeforeHud; slots are reused only after the
// render thread copied them out (the game thread never waits).
struct HudSlot { std::atomic<bool> busy{false};FrameEnvironment environment; };
HudSlot hudSlots[8];
std::atomic<unsigned> hudCursor{0};
void RunBeforeHud(void* argument) {
    auto* slot=static_cast<HudSlot*>(argument);
    const FrameEnvironment environment=slot->environment;
    slot->busy.store(false,std::memory_order_release);
    SetFrameEnvironment(environment);
    RenderBeforeHud();
}
} // namespace

bool InstallHooks() {
    static std::once_flag once;
    std::call_once(once,[] {
        if(!OpenHookBackend()) return;
        // Handle dan trampoline sengaja hidup sepanjang umur libmultiplayer.
        // Tidak boleh unload library yang fungsi hook-nya masih dipanggil renderer.
        void* egl=dlopen("libEGL.so",RTLD_NOW|RTLD_LOCAL);
        if(!egl) { FX_LOG("libEGL.so unavailable: %s",dlerror());return; }
        realGetProc=reinterpret_cast<GetProcFn>(dlsym(egl,"eglGetProcAddress"));
        if(!realGetProc) { FX_LOG("PostFX disabled: EGL resolver missing");return; }
        // Resolve and validate the complete set before changing any EGL entry.
        const char* names[]={"eglMakeCurrent","eglDestroyContext","eglDestroySurface",
            "eglReleaseThread","eglSwapBuffers","eglGetProcAddress",
            "eglSwapBuffersWithDamageKHR","eglSwapBuffersWithDamageEXT"};
        void* symbols[8]{};
        for(unsigned i=0;i<8;++i) {
            symbols[i]=ResolveEgl(egl,names[i]);
            if(!symbols[i] && i<6) {
                FX_LOG("PostFX disabled: EGL symbol missing: %s",names[i]);return;
            }
            for(unsigned j=0;j<i;++j) {
                if(symbols[i] && symbols[i]==symbols[j] && !(i==7 && j==6)) {
                    FX_LOG("PostFX disabled: incompatible EGL alias %s / %s",names[i],names[j]);return;
                }
            }
        }
        // Lifecycle wajib berhasil lebih dulu: tidak meninggalkan texture di share group.
        bool ok=Hook(symbols[0],names[0],&HookMakeCurrent,realMakeCurrent)
            && Hook(symbols[1],names[1],&HookDestroyContext,realDestroyContext)
            && Hook(symbols[2],names[2],&HookDestroySurface,realDestroySurface)
            && Hook(symbols[3],names[3],&HookReleaseThread,realReleaseThread)
            && Hook(symbols[4],names[4],&HookSwap,realSwap);
        if(!ok) { FX_LOG("PostFX disabled: incomplete EGL lifecycle hooks");return; }
        // EGL KHR/EXT dapat menunjuk alamat sama; jangan hook dua kali alamat itu.
        if(symbols[6] && !Hook(symbols[6],names[6],&HookDamageKHR,realDamageKHR)) return;
        if(symbols[7]==symbols[6] && realDamageKHR) realDamageEXT=realDamageKHR;
        else if(symbols[7] && !Hook(symbols[7],names[7],&HookDamageEXT,realDamageEXT)) return;
        // Publish once. On failure earlier hooks stay inert/pass-through; never
        // free a trampoline that another thread may already be executing.
        installed=true;
        WorldSunShadow::InstallDrawHooks(backendHook);
        TextureFilter::InstallHooks(backendHook);
        WorldMsaa::InstallHooks(backendHook);
        WorldSunShadow::RequestWorldDepth(GetSettings().enabled);
        FX_LOG("EGL postFX SUN4 installed via ShadowHook; no AML, no RenderWare offset changes");
    });
    return installed.load();
}
Settings GetSettings() { std::lock_guard<std::mutex> lock(settingsMutex);return settings; }
void SetSettings(const Settings& input) {
    Settings c=input;
    c.exposure=Clamp(c.exposure,0.1f,4.0f,1.02f);c.saturation=Clamp(c.saturation,0,2,1.04f);
    c.contrast=Clamp(c.contrast,0.5f,1.5f,1.02f);c.toneMix=Clamp(c.toneMix,0,1,0.35f);
    c.bloomStrength=Clamp(c.bloomStrength,0,2,0.16f);c.bloomThreshold=Clamp(c.bloomThreshold,0,4,0.70f);
    c.bloomKnee=Clamp(c.bloomKnee,0.001f,1,0.2f);c.dirtStrength=Clamp(c.dirtStrength,0,2,0.12f);
    c.ssrStrength=Clamp(c.ssrStrength,0,1,0.12f);c.ssrDistance=Clamp(c.ssrDistance,1,100,25);
    c.ssrThickness=Clamp(c.ssrThickness,0.01f,2,0.30f);c.aoStrength=Clamp(c.aoStrength,0,2,0.45f);
    c.aoRadius=Clamp(c.aoRadius,0.05f,5,1);c.lightCount=std::clamp(c.lightCount,0,4);
    if(static_cast<unsigned>(c.preset)>static_cast<unsigned>(LookPreset::Rain)) c.preset=LookPreset::Automatic;
    c.clarity=Clamp(c.clarity,0,0.6f,0.20f);c.vibrance=Clamp(c.vibrance,0,0.4f,0.12f);
    c.shadowLift=Clamp(c.shadowLift,0,0.03f,0.008f);c.vignette=Clamp(c.vignette,0,0.25f,0.04f);
    c.sunShafts=Clamp(c.sunShafts,0,0.5f,0.24f);c.lensFlare=Clamp(c.lensFlare,0,0.2f,0.045f);
    c.fogStrength=Clamp(c.fogStrength,0,1,0.65f);
    c.skyStrength=Clamp(c.skyStrength,0,1,0.35f);c.wetStrength=Clamp(c.wetStrength,0,1.5f,1.0f);
    c.motionBlur=Clamp(c.motionBlur,0,1,0);c.dofStrength=Clamp(c.dofStrength,0,1,0);
    c.autoExposure=Clamp(c.autoExposure,0,1,0.25f);c.aeKey=Clamp(c.aeKey,0.01f,1,0.11f);
    c.aeMin=Clamp(c.aeMin,0.1f,4,0.5f);c.aeMax=Clamp(c.aeMax,c.aeMin,4,2.5f);c.aeSpeed=Clamp(c.aeSpeed,0.05f,10,1.5f);
    c.hazeDensity=Clamp(c.hazeDensity,0,0.05f,0.001f);c.hazeUniform=Clamp(c.hazeUniform,0,0.05f,0.0005f);
    c.hazeStart=Clamp(c.hazeStart,0,2000,40);c.hazeMax=Clamp(c.hazeMax,0,1,0.85f);
    c.hazeFalloff=Clamp(c.hazeFalloff,0,1,0.045f);c.hazeBaseHeight=Clamp(c.hazeBaseHeight,-200,500,0);
    c.hazeSun=Clamp(c.hazeSun,0,4,1);c.hazeFoggy=Clamp(c.hazeFoggy,1,10,3);
    c.rayLength=Clamp(c.rayLength,0.2f,1,0.88f);c.rayDecay=Clamp(c.rayDecay,0.8f,0.999f,0.965f);
    c.grain=Clamp(c.grain,0,1,0);c.dither=Clamp(c.dither,0,4,1);c.gradeStrength=Clamp(c.gradeStrength,0,1,1);
    c.lutStrength=Clamp(c.lutStrength,0,1,1);c.lutPath[sizeof(c.lutPath)-1]=0;
    c.wetReflection=Clamp(c.wetReflection,0,3,1);c.wetPuddles=Clamp(c.wetPuddles,0,2,1);
    c.wetRipples=Clamp(c.wetRipples,0,2,1);c.wetForce=Clamp(c.wetForce,0,1,0);
    c.nightThreshold=Clamp(c.nightThreshold,0.1f,1,0.52f);c.nightGlow=Clamp(c.nightGlow,0,4,1);
    for(auto& l:c.lights) {
        for(float& uv:l.uv) uv=Clamp(uv,-2,3,0.5f);
        for(float& rgb:l.color) rgb=Clamp(rgb,0,8,1);
        l.radius=Clamp(l.radius,0.01f,2,0.35f);l.intensity=Clamp(l.intensity,0,2,0.04f);l.pulse=Clamp(l.pulse,0,1,0);
    }
    {std::lock_guard<std::mutex> lock(settingsMutex);settings=c;}
    settingsGeneration.fetch_add(1,std::memory_order_relaxed);
    WorldSunShadow::RequestWorldDepth(c.enabled);
}
void SetFrameEnvironment(const FrameEnvironment& environment) {
    const FrameEnvironment clean=CleanEnvironment(environment);
    std::lock_guard<std::mutex> lock(environmentMutex);frameEnvironment=clean;
}
void RenderBeforeHud(const float* projection) {
    if(insideSwap || !installed.load()) return;
    TryRender(eglGetCurrentDisplay(),eglGetCurrentSurface(EGL_DRAW),true,projection);
}
void BindCurrentGameSurface() {
    if(!installed.load() || eglGetCurrentContext()==EGL_NO_CONTEXT ||
       eglGetCurrentSurface(EGL_DRAW)==EGL_NO_SURFACE) return;
    try {
        auto s=CurrentState(true);
        if(s) s->gameSurface=eglGetCurrentSurface(EGL_DRAW);
    } catch(...) { FX_LOG("Unable to register game EGL surface"); }
}
void SubmitBeforeHud(const FrameEnvironment& environment) {
    if(GraphicsRenderThread::Ready()) {
        HudSlot& slot=hudSlots[hudCursor.fetch_add(1,std::memory_order_relaxed)%8];
        bool idle=false;
        if(!slot.busy.compare_exchange_strong(idle,true,std::memory_order_acq_rel)) return;
        slot.environment=environment;
        if(!GraphicsRenderThread::Enqueue(&RunBeforeHud,&slot)) slot.busy.store(false,std::memory_order_release);
        return;
    }
    // Single-threaded queue or no bridge: same thread as GL (original behaviour).
    SetFrameEnvironment(environment);
    RenderBeforeHud();
}
} // namespace EglPostFX
