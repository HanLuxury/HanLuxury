#include "CharacterTexture.h"
#include "../../vendor/imgui/stb_image.h" // implementation compiled in main.cpp
#include <cstring>
#include <fstream>
namespace Eagle::Character {
namespace {
constexpr int kMaxRaster=4096;
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
    RwRaster* raster=RwRasterCreate(width,height,32,int(rwRASTERTYPETEXTURE)|int(rwRASTERFORMAT8888));
    if(!raster) return nullptr;
    bool copied=false;
    if(RwUInt8* pixels=RwRasterLock(raster,0,rwRASTERLOCKWRITE)) {
        // The locked allocation decides what may be written: never RGBA into a 16-bit raster or past a row.
        const int w=raster->width,h=raster->height;
        if(raster->depth==32&&w>=1&&h>=1&&w<=kMaxRaster&&h<=kMaxRaster&&raster->stride>=w*4) {
            for(int y=0;y<h;++y) {
                uint8_t* row=pixels+size_t(y)*size_t(raster->stride);
                if(w==width&&h==height) {std::memcpy(row,rgba+size_t(y)*size_t(width)*4,size_t(width)*4);continue;}
                // A driver that rounds the size up: stretch the image over the whole raster, so UV 0..1
                // still cover all of it. Padding would move every UV of the model (384x512, 512x509, 104x104).
                const uint8_t* source=rgba+size_t(y*height/h)*size_t(width)*4;
                for(int x=0;x<w;++x) std::memcpy(row+size_t(x)*4,source+size_t(x*width/w)*4,4);
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
