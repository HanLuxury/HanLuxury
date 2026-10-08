#include "ClothesLoader.h"
#include "../character/CharacterTexture.h"
#include "../character/CharacterLog.h"
#include <cstring>
#include <cstdio>
#include <cmath>
#include <algorithm>
namespace Eagle::Character {
namespace {
uint32_t U32(const uint8_t* p) {return uint32_t(p[0])|(uint32_t(p[1])<<8)|(uint32_t(p[2])<<16)|(uint32_t(p[3])<<24);}
uint16_t U16(const uint8_t* p) {return uint16_t(p[0])|uint16_t(p[1]<<8);}
struct Chunk {uint32_t type;const uint8_t* data;size_t size;};
bool Children(const uint8_t* data,size_t size,std::vector<Chunk>& result) {
    size_t at=0;while(at<size) {
        if(size-at<12||result.size()>128) return false;
        size_t n=U32(data+at+4);if(n>size-at-12) return false;
        result.push_back({U32(data+at),data+at+12,n});at+=12+n;
    }return at==size;
}
const Chunk* Find(const std::vector<Chunk>& list,uint32_t type) {for(const auto& c:list) if(c.type==type) return &c;return nullptr;}
bool Geometry(const Chunk& geometry) {
    std::vector<Chunk> children;if(!Children(geometry.data,geometry.size,children)) return false;
    const auto* st=Find(children,1);const auto* ext=Find(children,3);if(!st||st->size<16||!ext) return false;
    const auto* ml=Find(children,8);std::vector<Chunk> materials,material,texture;
    if(!ml||!Children(ml->data,ml->size,materials)) return false;
    const auto* mc=Find(materials,1);const auto* mat=Find(materials,7);
    if(!mc||mc->size!=8||U32(mc->data)!=1||U32(mc->data+4)!=0xffffffffu||!mat||!Children(mat->data,mat->size,material)) return false;
    const auto* ms=Find(material,1);const auto* tx=Find(material,6);
    if(!ms||ms->size!=28||!tx||!Children(tx->data,tx->size,texture)) return false;
    for(const auto& t:texture) if(t.type==2&&(t.size<1||t.size>32)) return false;
    uint32_t flags=U32(st->data),tri=U32(st->data+4),verts=U32(st->data+8),morph=U32(st->data+12);
    if(flags&0x01000000u||!verts||verts>40000||!tri||tri>60000||morph!=1) return false;
    unsigned uv=(flags>>16)&255u;uv=uv ? uv:((flags&128u) ? 2:((flags&4u) ? 1:0));if(uv>2) return false;
    size_t offset=16+(flags&8u ? size_t(verts)*4:0)+size_t(verts)*8*uv;
    if(offset+size_t(tri)*8+24>st->size) return false;
    for(uint32_t t=0;t<tri;++t) {
        for(unsigned n:{0u,2u,6u}) if(U16(st->data+offset+t*8+n)>=verts) return false;
        if(U16(st->data+offset+t*8+4)!=0) return false;
    }
    offset+=size_t(tri)*8;uint32_t pos=U32(st->data+offset+16),norm=U32(st->data+offset+20);offset+=24;
    if(pos!=1||norm!=1||offset+size_t(verts)*24!=st->size) return false;
    for(size_t i=offset;i<st->size;i+=4) {float f;std::memcpy(&f,st->data+i,4);if(!std::isfinite(f)||std::abs(f)>1000) return false;}
    std::vector<Chunk> extensions;if(!Children(ext->data,ext->size,extensions)) return false;
    const auto* bm=Find(extensions,0x50e);
    if(!bm||bm->size!=20+size_t(tri)*12||U32(bm->data)!=0||U32(bm->data+4)!=1||U32(bm->data+8)!=tri*3||U32(bm->data+12)!=tri*3||U32(bm->data+16)!=0) return false;
    for(size_t i=20;i<bm->size;i+=4) if(U32(bm->data+i)>=verts) return false;
    const auto* skin=Find(extensions,0x116);if(!skin||skin->size<4) return false;
    const uint8_t bones=skin->data[0],used=skin->data[1];
    if(!bones||bones>64||!used||used>bones||skin->data[2]>4) return false;
    size_t indices=4+used,weights=indices+size_t(verts)*4,matrices=weights+size_t(verts)*16;
    if(matrices+size_t(bones)*64+12!=skin->size) return false;
    for(size_t i=0;i<used;++i) if(skin->data[4+i]>=bones) return false;
    for(uint32_t v=0;v<verts;++v) {
        float total=0;
        for(unsigned k=0;k<4;++k) {
            if(skin->data[indices+v*4+k]>=bones) return false;
            float w;std::memcpy(&w,skin->data+weights+(v*4+k)*4,4);
            if(!std::isfinite(w)||w<0||w>1) return false;total+=w;
        }if(std::abs(total-1)>0.0001f) return false;
    }
    for(size_t i=matrices;i<matrices+size_t(bones)*64;i+=4) {float f;std::memcpy(&f,skin->data+i,4);if(!std::isfinite(f)) return false;}
    return true;
}
}
bool ClothesLoader::ValidateDff(const std::vector<uint8_t>& bytes,std::string& error) {
    auto fail=[&](const char* why) {error=why;return false;};
    if(bytes.size()<24||bytes.size()>4*1024*1024) return fail("DFF size budget");
    std::vector<Chunk> root,clump,frames,geometries;
    if(!Children(bytes.data(),bytes.size(),root)||root.size()!=1||root[0].type!=16||!Children(root[0].data,root[0].size,clump)) return fail("invalid clump chunks");
    const auto* fl=Find(clump,14);const auto* gl=Find(clump,26);const auto* header=Find(clump,1);
    if(!fl||!gl||!header||header->size!=12||!Children(fl->data,fl->size,frames)||!Children(gl->data,gl->size,geometries)) return fail("missing clump structure");
    const auto* f=Find(frames,1);if(!f||f->size<4) return fail("missing frame structure");
    uint32_t n=U32(f->data);if(!n||n>128||f->size!=4+size_t(n)*56) return fail("frame bounds");
    for(uint32_t i=0;i<n;++i) {
        int32_t parent=int32_t(U32(f->data+4+i*56+48));if(parent>=int32_t(i)||parent< -1) return fail("cyclic frame tree");
        for(unsigned j=0;j<12;++j) {float value;std::memcpy(&value,f->data+4+i*56+j*4,4);if(!std::isfinite(value)) return fail("invalid frame matrix");}
    }
    // Validate HAnim before handing this file to the native stream reader.
    unsigned extensions=0,hierarchies=0;
    for(const auto& frame:frames) if(frame.type==3) {
        ++extensions;std::vector<Chunk> plugins;if(!Children(frame.data,frame.size,plugins)) return fail("frame extensions");
        for(const auto& plugin:plugins) {
            if(plugin.type==0x253f2fe) {if(plugin.size>32) return fail("frame name length");}
            else if(plugin.type==0x11e) {
                if(plugin.size<12) return fail("HAnim header");
                const auto count=U32(plugin.data+8);
                if(!count) {if(plugin.size!=12) return fail("HAnim leaf");continue;}
                if(count>64||plugin.size!=20+size_t(count)*12||++hierarchies>1) return fail("HAnim nodes");
                bool used[64]{};std::vector<uint32_t> ids;
                for(uint32_t j=0;j<count;++j) {
                    auto id=U32(plugin.data+20+j*12),index=U32(plugin.data+24+j*12);
                    if(index>=count||used[index]||std::find(ids.begin(),ids.end(),id)!=ids.end()) return fail("HAnim ID/index");
                    used[index]=true;ids.push_back(id);
                }
            } else return fail("unsupported frame plugin");
        }
    }
    if(extensions!=n||hierarchies!=1) return fail("frame hierarchy count");
    unsigned geometryCount=0,atomicCount=0;
    for(const auto& geometry:geometries) if(geometry.type==15) {if(!Geometry(geometry)) return fail("unsupported geometry/skin data");++geometryCount;}
    for(const auto& atomic:clump) if(atomic.type==20) {
        std::vector<Chunk> c;if(!Children(atomic.data,atomic.size,c)) return fail("atomic chunks");
        const auto* a=Find(c,1);if(!a||a->size!=16||U32(a->data)>=n||U32(a->data+4)>=geometryCount) return fail("atomic reference");++atomicCount;
    }
    return geometryCount&&geometryCount<=32&&atomicCount==geometryCount&&U32(header->data)==atomicCount ? true:fail("atomic count");
}
ClothesLoader::~ClothesLoader() {if(dictionary_) RenderWareApi::Get().destroyDictionary(dictionary_);}
TexturePtr ClothesLoader::Texture(const std::string& name) {
    auto old=textures_.find(name);if(old!=textures_.end()) if(auto t=old->second.lock()) return t;
    const TextureDefinition* definition=nullptr;for(const auto& d:catalog_.Textures()) if(d.name==name) {definition=&d;break;}
    if(!definition) return {};
    // Read by full path (see CharacterTexture.h): RtPNGImageRead opened "<storage>/<root>/..." here.
    std::vector<uint8_t> rgba;int width=0,height=0;std::string error;
    if(!ReadPng(root_+"/"+definition->file,512,rgba,width,height,error)) {
        LogLine(ANDROID_LOG_ERROR,"texture %s: %s",name.c_str(),error.c_str());return {};
    }
    TexturePtr texture(CreateRgbaTexture(rgba.data(),width,height),TextureDeleter{});
    if(!texture) {LogLine(ANDROID_LOG_ERROR,"texture %s: no 32-bit raster for %dx%d",name.c_str(),width,height);return {};}
    std::snprintf(texture->name,sizeof(texture->name),"%s",name.c_str());
    texture->filterAddressing=rwFILTERLINEAR|(rwTEXTUREADDRESSCLAMP<<8)|(rwTEXTUREADDRESSCLAMP<<12);
    auto& rw=RenderWareApi::Get();if(!dictionary_) dictionary_=rw.createDictionary();if(!dictionary_) return {};
    rw.addTexture(dictionary_,texture.get());textures_[name]=texture;return texture;
}
std::shared_ptr<ClothesAsset> ClothesLoader::Load(const AssetDefinition& d,const std::vector<uint8_t>& bytes,std::string& error) {
    if(!ValidateDff(bytes,error)||!RenderWareApi::Get().Resolve()) return {};
    auto resource=std::make_shared<ClothesAsset>();
    for(const auto& name:d.textures) {auto texture=Texture(name);if(!texture) {error="texture failed: "+name;return {};}resource->textures.push_back(std::move(texture));}
    auto& rw=RenderWareApi::Get();
    struct DictionaryGuard {RenderWareApi& rw;RwTexDictionary* old;~DictionaryGuard(){rw.setDictionary(old);}} dictionaryGuard{rw,rw.getDictionary()};
    rw.setDictionary(dictionary_);
    RwMemory memory{const_cast<uint8_t*>(bytes.data()),RwUInt32(bytes.size())};
    auto* stream=RwStreamOpen(rwSTREAMMEMORY,rwSTREAMREAD,&memory);
    if(!stream) {error="memory stream failed";return {};}
    if(rw.findChunk(stream,16,nullptr,nullptr)) resource->clump.reset(rw.readClump(stream));
    RwStreamClose(stream,nullptr);
    if(!resource->clump) {error="native clump reader rejected DFF";return {};}
    std::vector<RpAtomic*> atomics;if(!CollectAtomics(resource->clump.get(),atomics)) {error="atomic budget";return {};}
    for(auto* atomic:atomics) {
        auto* frame=RpAtomicGetFrame(atomic);auto* geometry=RpAtomicGetGeometry(atomic);
        auto* tag=frame ? rw.frameName(frame):nullptr;unsigned region=0;char textureName[32]{};
        if(!tag||std::sscanf(tag,"EAGLE_r%u_%31s",&region,textureName)!=2||region>1023||!geometry||geometry->matList.numMaterials!=1) {error="asset must use Eagle frame/material tags";return {};}
        auto texture=Texture(textureName);if(!texture) {error="tagged material texture missing";return {};}
        rw.setTexture(geometry->matList.materials[0],texture.get());
    }
    resource->memory=bytes.size()*4;return resource;
}
}
