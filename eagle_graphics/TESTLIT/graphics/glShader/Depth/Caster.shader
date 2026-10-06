// =====================================================================
//  EAGLE Graphics - glShader/Depth/Caster.shader
//  Pass caster (render ke shadow map). Semua cascade memakai shader GTA
//  sendiri (skinning, alpha test, format vertex asli) dengan SG_Light.w < 0;
//  fungsi ini dipanggil oleh SG_Apply pada pass tersebut.
//  Warna tidak ditulis (atlas hanya depth): yang penting hanya discard.
// =====================================================================
<frag>
void SG_Caster(inout lowp vec4 color) {
#if SG_ALPHA
    if (color.a < SG_Bias.w) discard;
#endif
}
</frag>
