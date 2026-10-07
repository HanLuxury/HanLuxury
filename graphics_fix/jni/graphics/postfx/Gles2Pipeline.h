#pragma once
// Included privately after Program/Link. No Android hooks or game-memory access.
bool Extension2(const char* name) {
    const char* list=reinterpret_cast<const char*>(glGetString(GL_EXTENSIONS));
    if(!list) return false;
    const size_t size=std::strlen(name);
    const char* p=list;
    while((p=std::strstr(p,name))) {
        if((p==list || p[-1]==' ') && (p[size]==' ' || p[size]=='\0')) return true;
        p+=size;
    } 
    return false;
}
struct CompatCaps {
    using GenVAO=void(GL_APIENTRYP)(GLsizei,GLuint*);
    using DeleteVAO=void(GL_APIENTRYP)(GLsizei,const GLuint*);
    using BindVAO=void(GL_APIENTRYP)(GLuint);
    using GetQuery=void(GL_APIENTRYP)(GLenum,GLenum,GLint*);
    using Blit=void(GL_APIENTRYP)(GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLint,GLbitfield,GLenum);
    using Resolve=void(GL_APIENTRYP)();
    GenVAO gen=nullptr;DeleteVAO del=nullptr;BindVAO bind=nullptr;GetQuery query=nullptr;
    Blit blit=nullptr;Resolve resolve=nullptr;
    bool checked=false,valid=false,unpack=false,pbo=false,srgb=false,separateFbo=false;
    bool Init() {
        if(checked) return valid;checked=true;
        if(Extension2("GL_OES_vertex_array_object")) {
            gen=Proc<GenVAO>("glGenVertexArraysOES");del=Proc<DeleteVAO>("glDeleteVertexArraysOES");
            bind=Proc<BindVAO>("glBindVertexArrayOES");
        }
        if(!gen || !del || !bind) { FX_LOG("REF3 GLES2 skipped: OES vertex array API unavailable");return false; }
        GLint range[2]{},precision=0;glGetShaderPrecisionFormat(GL_FRAGMENT_SHADER,GL_HIGH_FLOAT,range,&precision);
        if(!precision) { FX_LOG("REF3 GLES2 skipped: fragment highp unavailable");return false; }
        unpack=Extension2("GL_EXT_unpack_subimage");
        pbo=Extension2("GL_NV_pixel_buffer_object") || Extension2("GL_EXT_pixel_buffer_object");
        srgb=Extension2("GL_EXT_sRGB_write_control");
        separateFbo=Extension2("GL_ANGLE_framebuffer_blit") || Extension2("GL_NV_framebuffer_blit")
            || Extension2("GL_APPLE_framebuffer_multisample");
        if(Extension2("GL_ANGLE_framebuffer_blit")) blit=Proc<Blit>("glBlitFramebufferANGLE");
        else if(Extension2("GL_NV_framebuffer_blit")) blit=Proc<Blit>("glBlitFramebufferNV");
        if(Extension2("GL_APPLE_framebuffer_multisample")) resolve=Proc<Resolve>("glResolveMultisampleFramebufferAPPLE");
        if(Extension2("GL_EXT_occlusion_query_boolean")) query=Proc<GetQuery>("glGetQueryivEXT");
        valid=true;return true;
    }
    bool QueryActive() const {
        if(!query) return false;
        GLint value=0;query(GL_ANY_SAMPLES_PASSED,GL_CURRENT_QUERY,&value);if(value) return true;
        query(GL_ANY_SAMPLES_PASSED_CONSERVATIVE,GL_CURRENT_QUERY,&value);return value!=0;
    }
};
struct CompatGuard {
    const CompatCaps& caps;
    GLint fbo=0,readFbo=0,program=0,vao=0,array=0,active=0,viewport[4]{},textures[4]{};
    GLint alignment=4,unpack[3]{},pbo=0;
    GLboolean mask[4]{},srgb=0;
    static constexpr GLenum switches[]={GL_BLEND,GL_DEPTH_TEST,GL_STENCIL_TEST,GL_CULL_FACE,
        GL_SCISSOR_TEST,GL_SAMPLE_ALPHA_TO_COVERAGE,GL_SAMPLE_COVERAGE,GL_POLYGON_OFFSET_FILL,GL_DITHER};
    GLboolean enabled[9]{};
    explicit CompatGuard(const CompatCaps& c):caps(c) {
        glGetIntegerv(GL_FRAMEBUFFER_BINDING,&fbo);
        if(caps.separateFbo) glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING,&readFbo);
        glGetIntegerv(GL_CURRENT_PROGRAM,&program);glGetIntegerv(GL_VERTEX_ARRAY_BINDING,&vao);
        glGetIntegerv(GL_ARRAY_BUFFER_BINDING,&array);glGetIntegerv(GL_ACTIVE_TEXTURE,&active);
        glGetIntegerv(GL_VIEWPORT,viewport);glGetIntegerv(GL_UNPACK_ALIGNMENT,&alignment);
        for(unsigned i=0;i<4;++i) {
            glActiveTexture(GL_TEXTURE0+i);glGetIntegerv(GL_TEXTURE_BINDING_2D,&textures[i]);
        }
        glActiveTexture(GL_TEXTURE0);
        if(caps.unpack) {
            glGetIntegerv(GL_UNPACK_ROW_LENGTH,&unpack[0]);glGetIntegerv(GL_UNPACK_SKIP_ROWS,&unpack[1]);
            glGetIntegerv(GL_UNPACK_SKIP_PIXELS,&unpack[2]);
            glPixelStorei(GL_UNPACK_ROW_LENGTH,0);glPixelStorei(GL_UNPACK_SKIP_ROWS,0);glPixelStorei(GL_UNPACK_SKIP_PIXELS,0);
        }
        if(caps.pbo) { glGetIntegerv(GL_PIXEL_UNPACK_BUFFER_BINDING,&pbo);glBindBuffer(GL_PIXEL_UNPACK_BUFFER,0); }
        glPixelStorei(GL_UNPACK_ALIGNMENT,1);
        glGetBooleanv(GL_COLOR_WRITEMASK,mask);glColorMask(1,1,1,1);
        for(unsigned i=0;i<9;++i) { enabled[i]=glIsEnabled(switches[i]);glDisable(switches[i]); }
        if(caps.srgb) { srgb=glIsEnabled(kSRGBWrite);glEnable(kSRGBWrite); }
    }
    ~CompatGuard() {
        glUseProgram(static_cast<GLuint>(program));caps.bind(static_cast<GLuint>(vao));
        glBindBuffer(GL_ARRAY_BUFFER,static_cast<GLuint>(array));
        glBindFramebuffer(GL_FRAMEBUFFER,static_cast<GLuint>(fbo));
        if(caps.separateFbo) glBindFramebuffer(GL_READ_FRAMEBUFFER,static_cast<GLuint>(readFbo));
        glViewport(viewport[0],viewport[1],viewport[2],viewport[3]);
        for(unsigned i=0;i<4;++i) { glActiveTexture(GL_TEXTURE0+i);glBindTexture(GL_TEXTURE_2D,textures[i]); }
        glActiveTexture(active);glPixelStorei(GL_UNPACK_ALIGNMENT,alignment);
        if(caps.unpack) {
            glPixelStorei(GL_UNPACK_ROW_LENGTH,unpack[0]);glPixelStorei(GL_UNPACK_SKIP_ROWS,unpack[1]);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS,unpack[2]);
        }
        if(caps.pbo) glBindBuffer(GL_PIXEL_UNPACK_BUFFER,pbo);
        glColorMask(mask[0],mask[1],mask[2],mask[3]);
        for(unsigned i=0;i<9;++i) Toggle(switches[i],enabled[i]);
        if(caps.srgb) Toggle(kSRGBWrite,srgb);
    }
};
struct CompatTarget {
    GLuint texture=0,fbo=0;int width=0,height=0;
    void Destroy() {
        if(fbo) glDeleteFramebuffers(1,&fbo);if(texture) glDeleteTextures(1,&texture);
        texture=fbo=0;width=height=0;
    }
    bool Create(int w,int h,GLenum format=GL_RGBA,GLenum type=GL_UNSIGNED_BYTE) {
        width=w;height=h;glGenTextures(1,&texture);glBindTexture(GL_TEXTURE_2D,texture);
        glTexImage2D(GL_TEXTURE_2D,0,format,w,h,0,format,type,nullptr);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MIN_FILTER,GL_LINEAR);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_MAG_FILTER,GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_S,GL_CLAMP_TO_EDGE);glTexParameteri(GL_TEXTURE_2D,GL_TEXTURE_WRAP_T,GL_CLAMP_TO_EDGE);
        glGenFramebuffers(1,&fbo);glBindFramebuffer(GL_FRAMEBUFFER,fbo);
        glFramebufferTexture2D(GL_FRAMEBUFFER,GL_COLOR_ATTACHMENT0,GL_TEXTURE_2D,texture,0);
        return glCheckFramebufferStatus(GL_FRAMEBUFFER)==GL_FRAMEBUFFER_COMPLETE;
    }
};
struct CompatPipeline {
    CompatCaps caps;
    GLuint vao=0,vbo=0;int width=0,height=0;GLenum color=0,captureType=0;
    bool reportedMSAA=false;
    Program bloomProgram,compositeProgram,atmosphereProgram;
    CompatTarget scene,bloom[2],ping[2],atmosphere;
    void DestroyTargets() {
        scene.Destroy();for(auto& t:bloom) t.Destroy();for(auto& t:ping) t.Destroy();atmosphere.Destroy();
        width=height=0;
    }
    void Destroy() {
        DestroyTargets();bloomProgram.Destroy();compositeProgram.Destroy();atmosphereProgram.Destroy();
        if(vao && caps.del) caps.del(1,&vao);if(vbo) glDeleteBuffers(1,&vbo);vao=vbo=0;
    }
    bool Init() {
        if(vao) return true;
        GLuint vs=Compile(GL_VERTEX_SHADER,Shaders::kCompatVertex);if(!vs) return false;
        bool ok=Link(bloomProgram,vs,Shaders::kCompatBloom) && Link(compositeProgram,vs,Shaders::kCompatComposite)
            && Link(atmosphereProgram,vs,Shaders::kCompatAtmosphere);
        glDeleteShader(vs);if(!ok) return false;
        caps.gen(1,&vao);caps.bind(vao);glGenBuffers(1,&vbo);glBindBuffer(GL_ARRAY_BUFFER,vbo);
        const GLfloat triangle[]={-1,-1,3,-1,-1,3};
        glBufferData(GL_ARRAY_BUFFER,sizeof(triangle),triangle,GL_STATIC_DRAW);
        glVertexAttribPointer(0,2,GL_FLOAT,GL_FALSE,0,nullptr);glEnableVertexAttribArray(0);
        return vao && vbo;
    }
    bool Targets(int w,int h,GLenum format,GLenum type) {
        if(width==w && height==h && color==format && captureType==type && scene.fbo) return true;
        DestroyTargets();color=format;captureType=type;width=w;height=h;
        if(!scene.Create(w,h,format,type)) return false;
        for(int i=0;i<2;++i) {
            int bw=std::max(1,w>>(i+1)),bh=std::max(1,h>>(i+1));
            if(!bloom[i].Create(bw,bh) || !ping[i].Create(bw,bh)) return false;
        }
        if(!atmosphere.Create(std::max(1,w/4),std::max(1,h/4))) return false;
        FX_LOG("REF3 GLES2 FBO %dx%d: color/bloom/shafts; depth effects unavailable",w,h);return true;
    }
    static void Texture(unsigned unit,GLuint id) { glActiveTexture(GL_TEXTURE0+unit);glBindTexture(GL_TEXTURE_2D,id); }
    static void Begin(const CompatTarget& target,const Program& p) {
        glBindFramebuffer(GL_FRAMEBUFFER,target.fbo);glViewport(0,0,target.width,target.height);glUseProgram(p.id);
    }
    static void Draw() { glDrawArrays(GL_TRIANGLES,0,3); }
    bool Render(int w,int h,GLenum format,bool srgb,const LookProfile& look,bool& failed) {
        if(!caps.Init() || caps.QueryActive()) return false;
        CompatGuard guard(caps);
        glBindFramebuffer(GL_FRAMEBUFFER,0);
        if(glCheckFramebufferStatus(GL_FRAMEBUFFER)!=GL_FRAMEBUFFER_COMPLETE) return false;
        GLint samples=0,r=0,g=0,b=0,a=0;
        glGetIntegerv(GL_SAMPLES,&samples);glGetIntegerv(GL_RED_BITS,&r);glGetIntegerv(GL_GREEN_BITS,&g);
        glGetIntegerv(GL_BLUE_BITS,&b);glGetIntegerv(GL_ALPHA_BITS,&a);
        const bool rgb565=r==5 && g==6 && b==5 && a==0;
        const bool rgb8=r==8 && g==8 && b==8 && (a==0 || a==8);
        if(samples>1 && ((!caps.blit && !caps.resolve) || srgb || (!rgb565 && !rgb8))) {
            if(!reportedMSAA) FX_LOG("REF3 GLES2 bypass: MSAA %d needs compatible resolve extension; use GLES3 or disable MSAA",samples);
            reportedMSAA=true;return false;
        }
        GLenum type=rgb565 ? GL_UNSIGNED_SHORT_5_6_5 : GL_UNSIGNED_BYTE;
        if(!Init() || !Targets(w,h,format,type)) { Destroy();failed=true;return false; }
        caps.bind(vao);
        if(samples>1) {
            glBindFramebuffer(GL_READ_FRAMEBUFFER,0);glBindFramebuffer(GL_DRAW_FRAMEBUFFER,scene.fbo);
            if(caps.blit) caps.blit(0,0,w,h,0,0,w,h,GL_COLOR_BUFFER_BIT,GL_NEAREST);
            else caps.resolve();
        } else {
            glBindFramebuffer(GL_FRAMEBUFFER,0);Texture(0,scene.texture);
            glCopyTexSubImage2D(GL_TEXTURE_2D,0,0,0,0,0,w,h);
        }
        const auto& cfg=look.config;
        GLuint input=scene.texture;
        for(int i=0;i<2;++i) {
            const Program& p=bloomProgram;Begin(ping[i],p);Texture(0,input);
            glUniform1i(p[Source],0);glUniform1i(p[Extract],i==0);
            glUniform1i(p[Decode],i==0 && cfg.sourceIsSRGB);
            glUniform2f(p[ThresholdKnee],cfg.bloomThreshold,cfg.bloomKnee);
            glUniform2f(p[Direction],1.0f/ping[i].width,0);Draw();
            Begin(bloom[i],p);Texture(0,ping[i].texture);glUniform1i(p[Extract],0);glUniform1i(p[Decode],0);
            glUniform2f(p[Direction],0,1.0f/bloom[i].height);Draw();input=bloom[i].texture;
        }
        const Program& ap=atmosphereProgram;Begin(atmosphere,ap);Texture(0,scene.texture);
        glUniform1i(ap[Scene],0);glUniform1i(ap[Decode],cfg.sourceIsSRGB);
        glUniform2f(ap[Texel],1.0f/w,1.0f/h);glUniform4fv(ap[Sun],1,look.sun);
        glUniform2f(ap[Rays],look.config.rayLength,look.config.rayDecay);
        glUniform1f(ap[Aspect],float(w)/h);Draw();
        const Program& p=compositeProgram;
        glBindFramebuffer(GL_FRAMEBUFFER,0);glViewport(0,0,w,h);glUseProgram(p.id);
        Texture(0,scene.texture);Texture(1,bloom[0].texture);Texture(2,bloom[1].texture);Texture(3,atmosphere.texture);
        glUniform1i(p[Scene],0);glUniform1i(p[Bloom0],1);glUniform1i(p[Bloom1],2);glUniform1i(p[Atmosphere],3);
        glUniform1i(p[Decode],cfg.sourceIsSRGB);glUniform1i(p[Encode],!srgb);
        glUniform4f(p[Grade],cfg.exposure,cfg.saturation,cfg.contrast,cfg.toneMix);
        glUniform4fv(p[Detail],1,look.detail);glUniform3fv(p[ShadowTint],1,look.shadow);glUniform3fv(p[HighlightTint],1,look.highlight);
        glUniform2f(p[Texel],1.0f/w,1.0f/h);glUniform2f(p[BloomDirt],cfg.bloomStrength,cfg.dirtStrength);Draw();
        return true;
    }
};
