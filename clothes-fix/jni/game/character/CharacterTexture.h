#pragma once
#include "CharacterRenderWare.h"
#include <string>
#include <vector>
namespace Eagle::Character {
// PNGs of the character root are read here by their full path, never with RtPNGImageRead: its RwStream
// goes through the game's NvFOpen, which puts the storage root in front of every name it is given
// ("/storage/emulated/0/TESTLIT//storage/emulated/0/TESTLIT/character/...": file not found).
// Tightly packed RGBA rows, top row first. False (with `error`) for a missing/corrupt file or a size
// outside 1..maxSize.
bool ReadPng(const std::string& path,int maxSize,std::vector<uint8_t>& rgba,int& width,int& height,std::string& error);
// 32-bit texture from tightly packed RGBA rows; nullptr on failure and no raster is left behind.
RwTexture* CreateRgbaTexture(const uint8_t* rgba,int width,int height);
}
