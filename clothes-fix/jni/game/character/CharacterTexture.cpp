#include "CharacterTexture.h"
#include "../../vendor/imgui/stb_image.h" // implementation compiled in main.cpp
#include <algorithm>
#include <cstring>
#include <fstream>
namespace Eagle::Character {
namespace {
constexpr int kMaxRaster=4096;
int PowerOfTwo(int v) {int p=1;while(p<v) p<<=1;return p;}
}
bool ReadPng(const std::string& path,int maxSize,std::vector<uint8_t>& rgba,int& width,int& height,std::string& error) {
    rgba.clear();width=height=0;
    std::ifstream file(path,std::ios::binary|std::ios::ate);
    if(!file) {error="file not found: "+path;return false;}
    const auto size=file.tellg();
    if(size<33||size>16*1024*1024) {error="PNG file size: "+path;return false;}
    std::vector<uint8_t> bytes(static_cast<size_t>(size));file.seekg(0);
    if(!file.read(reinterpret_cast<char*>(bytes.data()),bytes.size())) {error="read failed: "+path;return false;}
    if(std::memcmp(bytes.data(),"\x89PNG\r\n\x1a\n",8)) {error="not a PNG: "+path;return false;}
    // Dimensions from the header first: an oversized image is refused before it is decoded.
    int w=0,h=0,channels=0;
    if(!stbi_info_from_memory(bytes.data(),int(bytes.size()),&w,&h,&channels)||w<1||h<1||w>maxSize||h>maxSize) {
        error="PNG dimensions (max "+std::to_string(maxSize)+"): "+path;return false;
    }
    uint8_t* pixels=stbi_load_from_memory(bytes.data(),int(bytes.size()),&w,&h,&channels,4);
    if(!pixels) {const char* why=stbi_failure_reason();error=std::string("PNG decode failed (")+(why ? why:"?")+"): "+path;return false;}
    if(w<1||h<1||w>maxSize||h>maxSize) {stbi_image_free(pixels);error="PNG dimensions: "+path;return false;}
    rgba.assign(pixels,pixels+size_t(w)*size_t(h)*4);stbi_image_free(pixels);
    width=w;height=h;return true;
}
RwTexture* CreateRgbaTexture(const uint8_t* rgba,int width,int height) {
    if(!rgba||width<1||height<1||width>kMaxRaster||height>kMaxRaster) return nullptr;
    if(!RwRasterCreate||!RwRasterLock||!RwRasterUnlock||!RwRasterDestroy) return nullptr;
    // Power-of-two raster: RwRasterUnlock gives every new texture GL_REPEAT (0x23f870) and the texture's
    // clamp bits are not applied, and GLES2 without NPOT support samples a repeated NPOT texture as black.
    RwRaster* raster=RwRasterCreate(PowerOfTwo(width),PowerOfTwo(height),32,int(rwRASTERTYPETEXTURE)|int(rwRASTERFORMAT8888));
    if(!raster) return nullptr;
    bool copied=false;
    if(RwUInt8* pixels=RwRasterLock(raster,0,rwRASTERLOCKWRITE)) {
        // The locked allocation decides what may be written: never RGBA into a 16-bit raster or past a row.
        const int w=raster->width,h=raster->height;
        if(raster->depth==32&&w>=1&&h>=1&&w<=kMaxRaster&&h<=kMaxRaster&&raster->stride>=w*4) {
            for(int y=0;y<h;++y) {
                uint8_t* row=pixels+size_t(y)*size_t(raster->stride);
                if(w==width&&h==height) {std::memcpy(row,rgba+size_t(y)*size_t(width)*4,size_t(width)*4);continue;}
                // Bilinear resample over the whole raster, so UV 0..1 still cover all of the image.
                // Padding would move every UV of the model (384x512, 512x509, 104x104 in TESTLIT).
                const float sy=std::clamp((y+.5f)*height/h-.5f,0.f,float(height-1));
                const int y0=int(sy),y1=std::min(y0+1,height-1);const float ty=sy-y0;
                for(int x=0;x<w;++x) {
                    const float sx=std::clamp((x+.5f)*width/w-.5f,0.f,float(width-1));
                    const int x0=int(sx),x1=std::min(x0+1,width-1);const float tx=sx-x0;
                    const uint8_t* p00=rgba+(size_t(y0)*width+x0)*4;const uint8_t* p01=rgba+(size_t(y0)*width+x1)*4;
                    const uint8_t* p10=rgba+(size_t(y1)*width+x0)*4;const uint8_t* p11=rgba+(size_t(y1)*width+x1)*4;
                    for(int c=0;c<4;++c) {
                        const float top=p00[c]+(p01[c]-p00[c])*tx,bottom=p10[c]+(p11[c]-p10[c])*tx;
                        row[size_t(x)*4+c]=uint8_t(top+(bottom-top)*ty+.5f);
                    }
                }
            }
            copied=true;
        }
        RwRasterUnlock(raster);
    }
    RwTexture* texture=copied ? RwTextureCreate(raster):nullptr;
    if(!texture) RwRasterDestroy(raster);
    return texture;
}
}
