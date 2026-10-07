#pragma once
// Representative GTA SA 2.10 world shaders, assembled from the string fragments
// of RQShader::BuildSource (libGTASA) - used by the host tests only.
#include <string>
#include <vector>

struct GtaCase {
    const char* name;
    std::string vs, ps;
    bool expectPatch;
};

static const char* kBones4 =
"\tivec4 BlendIndexArray = ivec4(BoneIndices);\tmat4 BoneToLocal;"
"\tBoneToLocal[0] = Bones[BlendIndexArray.x*3] * BoneWeight.x;\tBoneToLocal[1] = Bones[BlendIndexArray.x*3+1] * BoneWeight.x;\tBoneToLocal[2] = Bones[BlendIndexArray.x*3+2] * BoneWeight.x;\tBoneToLocal[3] = vec4(0.0,0.0,0.0,1.0);"
"\tBoneToLocal[0] += Bones[BlendIndexArray.y*3] * BoneWeight.y;\tBoneToLocal[1] += Bones[BlendIndexArray.y*3+1] * BoneWeight.y;\tBoneToLocal[2] += Bones[BlendIndexArray.y*3+2] * BoneWeight.y;"
"\tBoneToLocal[0] += Bones[BlendIndexArray.z*3] * BoneWeight.z;\tBoneToLocal[1] += Bones[BlendIndexArray.z*3+1] * BoneWeight.z;\tBoneToLocal[2] += Bones[BlendIndexArray.z*3+2] * BoneWeight.z;"
"\tBoneToLocal[0] += Bones[BlendIndexArray.w*3] * BoneWeight.w;\tBoneToLocal[1] += Bones[BlendIndexArray.w*3+1] * BoneWeight.w;\tBoneToLocal[2] += Bones[BlendIndexArray.w*3+2] * BoneWeight.w;";


inline std::vector<GtaCase> GtaCases() {
    std::string head = "#version 100\nprecision highp float;\nuniform mat4 ProjMatrix;\nuniform mat4 ViewMatrix;\nuniform mat4 ObjMatrix;\n";
    std::string lightU = "uniform lowp vec3 AmbientLightColor;\nuniform lowp vec4 MaterialEmissive;\nuniform lowp vec4 MaterialAmbient;\nuniform lowp vec4 MaterialDiffuse;\nuniform lowp vec3 DirLightDiffuseColor;\nuniform vec3 DirLightDirection;\n";
    std::string fog = "Out_FogAmt = clamp((length(WorldPos.xyz - CameraPosition.xyz) - FogDistances.x) * FogDistances.z, 0.0, 0.90);";
    std::string xf = "\tvec4 WorldPos = ObjMatrix * vec4(Position,1.0);\tvec4 ViewPos = ViewMatrix * WorldPos;\tgl_Position = ProjMatrix * ViewPos;";
    return {
      {"building",
       head + "attribute vec3 Position;\nattribute vec3 Normal;\nattribute vec2 TexCoord0;\nattribute vec4 GlobalColor;\nvarying mediump vec2 Out_Tex0;\nuniform vec3 CameraPosition;\nvarying mediump float Out_FogAmt;uniform vec3 FogDistances;attribute vec4 Color2;uniform lowp float ColorInterp;varying lowp vec4 Out_Color;void main() {" + xf + fog + "Out_Tex0 = TexCoord0;lowp vec4 InterpColor = mix(GlobalColor, Color2, ColorInterp);Out_Color = InterpColor;}",
       "precision mediump float;uniform sampler2D Diffuse;varying mediump vec2 Out_Tex0;varying mediump float Out_FogAmt;uniform lowp vec3 FogColor;varying lowp vec4 Out_Color;void main(){lowp vec4 fcolor;lowp vec4 diffuseColor = texture2D(Diffuse, Out_Tex0, -0.5);fcolor = diffuseColor;fcolor *= Out_Color;fcolor.xyz = mix(fcolor.xyz, FogColor, Out_FogAmt);gl_FragColor = fcolor;/*ATBEGIN*/if (gl_FragColor.a < 0.2) { discard; }/*ATEND*/}", true},
      {"ped_skinned_lit",
       head + lightU + "uniform vec3 DirBackLightDirection;attribute vec3 Position;\nattribute vec3 Normal;\nattribute vec2 TexCoord0;\nattribute vec4 GlobalColor;\nattribute vec4 BoneWeight;\nattribute vec4 BoneIndices;\nuniform highp vec4 Bones[96];\nvarying mediump vec2 Out_Tex0;\nuniform vec3 CameraPosition;\nvarying mediump float Out_FogAmt;uniform vec3 FogDistances;varying lowp vec4 Out_Color;void main() {" + kBones4 +
       "\tvec4 WorldPos = ObjMatrix * (vec4(Position,1.0) * BoneToLocal);\tvec4 ViewPos = ViewMatrix * WorldPos;\tgl_Position = ProjMatrix * ViewPos;vec3 WorldNormal = mat3(ObjMatrix) * (Normal * mat3(BoneToLocal));" + fog +
       "Out_Tex0 = TexCoord0;vec3 Out_LightingColor;Out_LightingColor = AmbientLightColor * MaterialAmbient.xyz + MaterialEmissive.xyz;Out_LightingColor += (max(dot(DirLightDirection, WorldNormal), 0.0) + max(dot(DirBackLightDirection, WorldNormal), 0.0)) * DirLightDiffuseColor;Out_Color = vec4(Out_LightingColor * MaterialDiffuse.xyz, MaterialAmbient.w * GlobalColor.w);Out_Color = clamp(Out_Color, 0.0, 1.0);}",
       "precision mediump float;uniform sampler2D Diffuse;varying mediump vec2 Out_Tex0;varying mediump float Out_FogAmt;uniform lowp vec3 FogColor;varying lowp vec4 Out_Color;void main(){lowp vec4 fcolor;lowp vec4 diffuseColor = texture2D(Diffuse, Out_Tex0, -0.5);fcolor = diffuseColor;fcolor *= Out_Color;fcolor.xyz = mix(fcolor.xyz, FogColor, Out_FogAmt);gl_FragColor = fcolor;}", true},
      {"vehicle_env_spec",
       head + lightU + "attribute vec3 Position;\nattribute vec3 Normal;\nattribute vec2 TexCoord0;\nattribute vec4 GlobalColor;\nvarying mediump vec2 Out_Tex0;\nuniform lowp float EnvMapCoefficient;\nvarying mediump vec2 Out_Tex1;\nuniform vec3 CameraPosition;\nvarying mediump float Out_FogAmt;uniform vec3 FogDistances;varying lowp vec4 Out_Color;varying lowp vec3 Out_Spec;void main() {" + xf +
       "vec3 WorldNormal = (ObjMatrix * vec4(Normal,0.0)).xyz;" + fog + "Out_Tex0 = TexCoord0;vec3 reflVector = normalize(WorldPos.xyz - CameraPosition.xyz);reflVector = reflVector - 2.0 * dot(reflVector, WorldNormal) * WorldNormal;Out_Tex1 = vec2(length(reflVector.xy), (reflVector.z * 0.5) + 0.25);vec3 Out_LightingColor;Out_LightingColor = AmbientLightColor * MaterialAmbient.xyz + MaterialEmissive.xyz;Out_LightingColor += max(dot(DirLightDirection, WorldNormal), 0.0) * DirLightDiffuseColor;Out_Color = vec4(Out_LightingColor * MaterialDiffuse.xyz, MaterialAmbient.w * GlobalColor.w);Out_Color = clamp(Out_Color, 0.0, 1.0);float specAmt = max(pow(dot(reflVector, DirLightDirection), 9.0), 0.0) * EnvMapCoefficient * 2.0;Out_Spec = specAmt * DirLightDiffuseColor;}",
       "precision mediump float;uniform sampler2D Diffuse;varying mediump vec2 Out_Tex0;uniform sampler2D EnvMap;uniform lowp float EnvMapCoefficient;varying mediump vec2 Out_Tex1;varying mediump float Out_FogAmt;uniform lowp vec3 FogColor;varying lowp vec4 Out_Color;varying lowp vec3 Out_Spec;void main(){lowp vec4 fcolor;lowp vec4 diffuseColor = texture2D(Diffuse, Out_Tex0, -0.5);fcolor = diffuseColor;fcolor *= Out_Color;fcolor.xyz = mix(fcolor.xyz, texture2D(EnvMap, Out_Tex1).xyz, EnvMapCoefficient);fcolor.xyz += Out_Spec;fcolor.xyz = mix(fcolor.xyz, FogColor, Out_FogAmt);gl_FragColor = fcolor;}", true},
      {"tree_alpha_camnormal",
       head + lightU + "attribute vec3 Position;\nattribute vec3 Normal;\nattribute vec2 TexCoord0;\nattribute vec4 GlobalColor;\nvarying mediump vec2 Out_Tex0;\nuniform vec3 CameraPosition;\nvarying mediump float Out_FogAmt;uniform vec3 FogDistances;varying lowp vec4 Out_Color;void main() {" + xf +
       "vec3 WorldNormal = normalize(vec3(WorldPos.xy - CameraPosition.xy, 0.0001)) * 0.85;" + fog + "Out_Tex0 = TexCoord0;vec3 Out_LightingColor;Out_LightingColor = AmbientLightColor * MaterialAmbient.xyz + GlobalColor.xyz;Out_LightingColor += max(dot(DirLightDirection, WorldNormal), 0.0) * DirLightDiffuseColor;Out_Color = vec4((Out_LightingColor.xyz + GlobalColor.xyz * 1.5) * MaterialDiffuse.xyz, (MaterialAmbient.w) * GlobalColor.w);Out_Color = clamp(Out_Color, 0.0, 1.0);}",
       "precision mediump float;uniform sampler2D Diffuse;varying mediump vec2 Out_Tex0;varying mediump float Out_FogAmt;uniform lowp vec3 FogColor;varying lowp vec4 Out_Color;void main(){lowp vec4 fcolor;lowp vec4 diffuseColor = texture2D(Diffuse, Out_Tex0, -0.5);fcolor = diffuseColor;fcolor *= Out_Color;fcolor.xyz = mix(fcolor.xyz, FogColor, Out_FogAmt);gl_FragColor = fcolor;/*ATBEGIN*/if (gl_FragColor.a < 0.5) { discard; }gl_FragColor.a = Out_Color.a;/*ATEND*/}", true},
      {"hud_2d",
       head + "attribute vec3 Position;\nattribute vec3 Normal;\nattribute vec2 TexCoord0;\nattribute vec4 GlobalColor;\nvarying mediump vec2 Out_Tex0;\nvarying lowp vec4 Out_Color;void main() {" + xf + "Out_Tex0 = TexCoord0;Out_Color = GlobalColor;}",
       "precision mediump float;uniform sampler2D Diffuse;varying mediump vec2 Out_Tex0;varying lowp vec4 Out_Color;void main(){lowp vec4 fcolor;lowp vec4 diffuseColor = texture2D(Diffuse, Out_Tex0, -0.5);fcolor = diffuseColor;fcolor *= Out_Color;gl_FragColor = fcolor;}", false},
      {"sphere_reflection",
       head + "attribute vec3 Position;\nattribute vec3 Normal;\nuniform vec3 CameraPosition;\nvarying mediump float Out_FogAmt;uniform vec3 FogDistances;attribute vec4 GlobalColor;varying lowp vec4 Out_Color;void main() {\tvec4 WorldPos = ObjMatrix * vec4(Position,1.0);    vec3 ReflVector = WorldPos.xyz - CameraPosition.xyz;\tvec3 ReflPos = normalize(ReflVector);    ReflPos.xy = normalize(ReflPos.xy) * (ReflPos.z * 0.5 + 0.5);\tgl_Position = vec4(ReflPos.xy, length(ReflVector) * 0.002, 1.0);" + fog + "Out_Color = GlobalColor;}",
       "precision mediump float;varying mediump float Out_FogAmt;uniform lowp vec3 FogColor;varying lowp vec4 Out_Color;void main(){lowp vec4 fcolor;fcolor = Out_Color;fcolor.xyz = mix(fcolor.xyz, FogColor, Out_FogAmt);gl_FragColor = fcolor;}", false},
    };
}
