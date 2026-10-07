#pragma once
#include "../postfx/ProjectionMath.h"
#include <algorithm>
#include <cmath>

namespace WorldSunShadow::Math {
inline float Dot(const float* a,const float* b) { return a[0]*b[0]+a[1]*b[1]+a[2]*b[2]; }
inline bool Normalize(float* v) {
    float length=std::sqrt(Dot(v,v));
    if(!std::isfinite(length)||length<0.0001f) return false;
    for(int i=0;i<3;++i) v[i]/=length;
    return true;
}
inline void Cross(const float* a,const float* b,float* out) {
    out[0]=a[1]*b[2]-a[2]*b[1];out[1]=a[2]*b[0]-a[0]*b[2];out[2]=a[0]*b[1]-a[1]*b[0];
}
inline void Multiply(const float* a,const float* b,float* out) {
    float result[16]{};
    for(int c=0;c<4;++c) for(int r=0;r<4;++r) for(int k=0;k<4;++k)
        result[c*4+r]+=a[k*4+r]*b[c*4+k];
    std::copy(result,result+16,out);
}
inline bool Same(const float* a,const float* b) {
    for(int i=0;i<16;++i) if(!std::isfinite(a[i])||!std::isfinite(b[i])||
        std::abs(a[i]-b[i])>0.0001f*std::max(1.0f,std::max(std::abs(a[i]),std::abs(b[i])))) return false;
    return true;
}
// basisX is in/out: the previous light X axis, re-orthogonalised against the
// new direction. The map therefore never flips around the light axis (the old
// up-vector switch at |z|>0.98 rotated the whole map in one frame).
inline bool LightCamera(const float* center,const float* direction,float radius,int resolution,
                        float* basisX,float* view,float* projection,float* viewProjection) {
    if(!std::isfinite(radius)||radius<1||resolution<1) return false;
    float z[3]={direction[0],direction[1],direction[2]};
    if(!Normalize(z)) return false;
    for(int i=0;i<3;++i) if(!std::isfinite(center[i])) return false;
    float x[3]={basisX[0],basisX[1],basisX[2]};
    const float along=Dot(x,z);
    for(int i=0;i<3;++i) x[i]-=z[i]*along;
    if(!Normalize(x)) {
        float up[3]={0,0,1};
        if(std::abs(z[2])>0.98f) { up[1]=1;up[2]=0; }
        Cross(up,z,x);if(!Normalize(x)) return false;
    }
    float y[3];Cross(z,x,y);
    for(int i=0;i<3;++i) basisX[i]=x[i];
    // Snap in the light's XY plane; reduce shimmering as the camera moves.
    const float texel=2*radius/float(resolution);
    const float cx=std::round(Dot(center,x)/texel)*texel;
    const float cy=std::round(Dot(center,y)/texel)*texel;
    // Snap the depth origin as well: stored depths stay bit-identical while
    // the camera moves, so acne/bias does not crawl from frame to frame.
    const float depthStep=texel*4;
    // Eye 3 radii toward the sun, depth range 6 radii: tall buildings still
    // cast their long shadows at dawn/dusk (24-bit depth: ~25 um steps).
    const float cz=std::round((Dot(center,z)+3*radius)/depthStep)*depthStep;
    std::fill(view,view+16,0);std::fill(projection,projection+16,0);
    for(int i=0;i<3;++i) { view[i*4]=x[i];view[i*4+1]=y[i];view[i*4+2]=z[i]; }
    view[12]=-cx;view[13]=-cy;view[14]=-cz;view[15]=1;
    const float nearPlane=0.1f,farPlane=6*radius;
    projection[0]=projection[5]=1/radius;
    projection[10]=-2/(farPlane-nearPlane);
    projection[14]=-(farPlane+nearPlane)/(farPlane-nearPlane);projection[15]=1;
    Multiply(projection,view,viewProjection);return true;
}
// Keeps the previous sun direction until it moved more than ~0.3 degrees.
// GTA advances the sun every game minute; re-rasterising the map for every
// tiny step makes every shadow edge crawl.
inline void StableDirection(const float* input,float* stable) {
    float d[3]={input[0],input[1],input[2]};
    if(!Normalize(d)) return;
    float s[3]={stable[0],stable[1],stable[2]};
    if(!Normalize(s) || Dot(d,s)<0.999986f) for(int i=0;i<3;++i) stable[i]=d[i];
}
}
