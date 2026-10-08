#include "ClothesLoader.h"
#include "../character/CharacterTexture.h"
#include "../character/CharacterLog.h"
#include <cstring>
#include <cstdio>
#include <cstdlib>
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
bool Geometry(const Chunk& geometry,const uint8_t*& bind,size_t& bindSize) {
    std::vector<Chunk> children;if(!Children(geometry.data,geometry.size,children)) return false;
    const auto* st=Find(children,1);const auto* ext=Find(children,3);if(!st||st->size<16||!ext) return false;
    const auto* ml=Find(children,8);std::vector<Chunk> materials,material,texture;
    if(!ml||!Children(ml->data,ml->size,materials)) return false;
    const auto* mc=Find(materials,1);const auto* mat=Find(materials,7);
    if(!mc||mc->size!=8||U32(mc->data)!=1||U32(mc->data+4)!=0xffffffffu||!mat||!Children(mat->data,mat->size,material)) return false;
    const auto* ms=Find(material,1);const auto* tx=Find(material,6);
    if(!ms||ms->size!=28||!tx||!Children(tx->data,tx->size,texture)) return false;
    // RwTextureStreamRead (0x26c670) copies a whole name chunk into a 128-byte stack buffer.
    for(const auto& t:texture) {
        if(t.type==0x13) return false;
        if(t.type==2&&(t.size<1||t.size>32||!std::memchr(t.data,0,t.size))) return false;
    }
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
    // maxWeights 0 makes SkinGeometryRead (0x25da5c) parse the old layout, not the one checked here.
    if(!bones||bones>64||!used||used>bones||!skin->data[2]||skin->data[2]>4) return false;
    size_t indices=4+used,weights=indices+size_t(verts)*4,matrices=weights+size_t(verts)*16;
    if(matrices+size_t(bones)*64+12!=skin->size) return false;
    // Split data: with meshes or RLE runs the native reader goes past these 12 bytes (0x25b95c).
    const size_t split=matrices+size_t(bones)*64;
    if(U32(skin->data+split+4)||U32(skin->data+split+8)) return false;
    bind=skin->data+matrices;bindSize=size_t(bones)*64;
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
                if(plugin.size<12||U32(plugin.data)!=0x100) return fail("HAnim header");
                const auto count=U32(plugin.data+8);
                if(!count) {if(plugin.size!=12) return fail("HAnim leaf");continue;}
                if(count>64||plugin.size!=20+size_t(count)*12||++hierarchies>1) return fail("HAnim nodes");
                // HAnimCopy (0x254e0c) skips a sub-hierarchy; HAnimRead sizes the interpolator from keyFrameSize.
                if((U32(plugin.data+12)&3)!=0||U32(plugin.data+16)!=36) return fail("HAnim flags/keyframe size");
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
    unsigned geometryCount=0,atomicCount=0;const uint8_t* bind=nullptr;size_t bindSize=0;
    for(const auto& geometry:geometries) if(geometry.type==15) {
        const uint8_t* own=nullptr;size_t ownSize=0;
        if(!Geometry(geometry,own,ownSize)) return fail("unsupported geometry/skin data");
        // One private hierarchy skins every part, and the skin pipeline caches its matrices per hierarchy
        // (openglSkinAllInOneAtomicInstanceCB 0x25c5c8): all parts need the same bind pose and frame.
        if(bind&&(ownSize!=bindSize||std::memcmp(own,bind,bindSize))) return fail("parts with different bind matrices");
        bind=own;bindSize=ownSize;++geometryCount;
    }
    int64_t atomicFrame=-1;
    for(const auto& atomic:clump) if(atomic.type==20) {
        std::vector<Chunk> c;if(!Children(atomic.data,atomic.size,c)) return fail("atomic chunks");
        const auto* a=Find(c,1);if(!a||a->size!=16||U32(a->data)>=n||U32(a->data+4)>=geometryCount) return fail("atomic reference");++atomicCount;
        const uint32_t fi=U32(a->data);
        if(atomicFrame>=0&&fi!=uint32_t(atomicFrame)&&std::memcmp(f->data+4+fi*56,f->data+4+atomicFrame*56,52)) return fail("parts on different frames");
        atomicFrame=fi;
    }
    return geometryCount&&geometryCount<=32&&atomicCount==geometryCount&&U32(header->data)==atomicCount ? true:fail("atomic count");
}
bool ClothesLoader::ShortenFrameNames(std::vector<uint8_t>& bytes,std::vector<std::string>& names) {
    // The game keeps a frame name in a 24-byte slot: NodeNameStreamRead (0x574128) writes the whole chunk
    // plus a NUL with no bound, and NodeNameCopy (0x5740f0) copies 23 bytes on clone. A longer name is
    // recorded here and the game gets "#<frame>" instead. The rest of the chunk becomes a chunk with an
    // unregistered id, which _rwPluginRegistryReadDataChunks (0x280f84) skips, so no size changes.
    constexpr size_t kGameName=22;constexpr uint32_t kSkipped=0x7ea61e00u;
    std::vector<Chunk> root,clump,frames;
    if(!Children(bytes.data(),bytes.size(),root)||root.size()!=1||!Children(root[0].data,root[0].size,clump)) return false;
    const auto* fl=Find(clump,14);if(!fl||!Children(fl->data,fl->size,frames)) return false;
    names.clear();uint32_t index=0;
    for(const auto& frame:frames) if(frame.type==3) {
        std::vector<Chunk> plugins;if(!Children(frame.data,frame.size,plugins)) return false;
        names.emplace_back();
        for(const auto& plugin:plugins) if(plugin.type==0x253f2fe) {
            names.back().assign(reinterpret_cast<const char*>(plugin.data),strnlen(reinterpret_cast<const char*>(plugin.data),plugin.size));
            if(plugin.size<=kGameName) continue;
            char alias[8];const size_t length=size_t(std::snprintf(alias,sizeof(alias),"#%u",index));
            if(plugin.size<length+12) return false;
            uint8_t* at=bytes.data()+(plugin.data-bytes.data());
            const uint32_t version=U32(at-4),rest=uint32_t(plugin.size-length-12);
            auto put=[](uint8_t* p,uint32_t v) {for(int i=0;i<4;++i) p[i]=uint8_t(v>>(8*i));};
            put(at-8,uint32_t(length));std::memcpy(at,alias,length);
            put(at+length,kSkipped);put(at+length+4,rest);put(at+length+8,version);
        }
        ++index;
    }
    return true;
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
    std::vector<uint8_t> stored(bytes);std::vector<std::string> names;
    if(!ShortenFrameNames(stored,names)) {error="frame names";return {};}
    auto resource=std::make_shared<ClothesAsset>();
    for(const auto& name:d.textures) {auto texture=Texture(name);if(!texture) {error="texture failed: "+name;return {};}resource->textures.push_back(std::move(texture));}
    auto& rw=RenderWareApi::Get();
    struct DictionaryGuard {RenderWareApi& rw;RwTexDictionary* old;~DictionaryGuard(){rw.setDictionary(old);}} dictionaryGuard{rw,rw.getDictionary()};
    rw.setDictionary(dictionary_);
    RwMemory memory{stored.data(),RwUInt32(stored.size())};
    auto* stream=RwStreamOpen(rwSTREAMMEMORY,rwSTREAMREAD,&memory);
    if(!stream) {error="memory stream failed";return {};}
    if(rw.findChunk(stream,16,nullptr,nullptr)) resource->clump.reset(rw.readClump(stream));
    RwStreamClose(stream,nullptr);
    if(!resource->clump) {error="native clump reader rejected DFF";return {};}
    std::vector<RpAtomic*> atomics;if(!CollectAtomics(resource->clump.get(),atomics)) {error="atomic budget";return {};}
    for(auto* atomic:atomics) {
        auto* frame=RpAtomicGetFrame(atomic);auto* geometry=RpAtomicGetGeometry(atomic);
        const char* name=frame ? rw.frameName(frame):nullptr;std::string tag=name ? name:"";
        if(tag.size()>1&&tag[0]=='#') {const unsigned long k=std::strtoul(tag.c_str()+1,nullptr,10);tag=k<names.size() ? names[k]:"";}
        unsigned region=0;char textureName[32]{};
        if(std::sscanf(tag.c_str(),"EAGLE_r%u_%31s",&region,textureName)!=2||region>1023||!geometry||geometry->matList.numMaterials!=1) {error="asset must use Eagle frame/material tags";return {};}
        auto texture=Texture(textureName);if(!texture) {error="tagged material texture missing";return {};}
        rw.setTexture(geometry->matList.materials[0],texture.get());
        // Clones share this geometry (RpAtomicClone 0x2ba898 adds a reference), so parts are found by it.
        if(!resource->parts.emplace(geometry,AssetPart{uint16_t(region),textureName}).second) {error="atomics share a geometry";return {};}
    }
    resource->memory=bytes.size()*4;return resource;
}
}
