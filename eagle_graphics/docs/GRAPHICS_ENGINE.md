# EAGLE Graphics Engine — GTA SA Android 2.10 arm64

Engine grafis native (C++/JNI) di dalam `libmultiplayer.so`. **Tanpa AML**: hook memakai
ShadowHook (framework yang sudah dipakai `CHook`), resource eksternal dari
`/storage/emulated/0/TESTLIT/graphics/` dengan susunan berkas bergaya SA_DOX
(`Config.ini`, `Advanced.ini`, `shaderUniform.ini`, `glShader/`, `data/`, `logOutput.log`, lihat §5).

Fase yang dikerjakan di paket ini: **PHASE 1–5** (fondasi, matahari, CSM, caster
gedung/objek/ped/kendaraan/vegetasi, receiver di shader world). Post‑process (PHASE 6+)
belum: lihat bagian akhir.

---

## 1. Status

| Status | Bagian |
|---|---|
| [IMPLEMENTED] | Log (`logOutput.log`), parser `Config.ini` + `Advanced.ini` (format SDX: `bEnabled = 1`, `[[Grup]`), preset LOW/MEDIUM/HIGH/ULTRA, deteksi GPU, GL state backup |
| [IMPLEMENTED] | `glShader/*.shader` (format `<vert>`/`<frag>`, `#include "x";`, parameter `@float/@int/@bool`), salinan bawaan di library bila file hilang/rusak |
| [IMPLEMENTED] | `shaderUniform.ini`: nilai shader yang berubah LANGSUNG tanpa restart (uniform `SG_User[8]`), disimpan kembali dari tab EFEK di pause menu |
| [IMPLEMENTED] | Receiver per entitas: `Entity/Building.shader`, `Vehicle.shader`, `Character.shader` (klasifikasi: skinning → Character, lit+specular → Vehicle, sisanya → Building) |
| [IMPLEMENTED] | `data/eagle_timecyc.dat`: 4 tampilan waktu + jam transisi bisa diedit |
| [IMPLEMENTED] | Pause menu GTA (`ModernPauseMenu`) bergaya battle‑royale: rail tab kiri, chip pilihan, slider kuning, hint bar. Tab GRAFIS = GTA + EAGLE, tab EFEK = shaderUniform.ini/debug/berkas. Tanpa resource baru. Pilihan pemain disimpan & diterapkan lagi saat start |
| [IMPLEMENTED] | Bridge RenderQueue (command slot 47 + observer `rqSelectShader`), divalidasi terhadap symbol sebelum patch |
| [IMPLEMENTED] | SunManager: arah dari `CTimeCycle` GTA, clamp elevasi, dead‑zone anti‑shimmer, fade horizon/cuaca/interior |
| [IMPLEMENTED] | TimeCycleFX: SUNRISE/DAY/SUNSET/NIGHT + hujan/awan/kabut, cross‑fade halus |
| [IMPLEMENTED] | CSM 1–4 cascade (practical split, sphere fit, texel snapping), atlas depth satu FBO |
| [IMPLEMENTED] | Caster: daftar visible GTA + scan sektor dunia di sekitar kamera; gedung, dummy, objek SA‑MP, kendaraan (+sopir), ped (animasi asli), senjata, daun ber‑alpha |
| [IMPLEMENTED] | Receiver: injeksi ke shader world native GTA (`ES2Shader::Build`), PCF 1/4/9 tap, hardware compare bila ada `GL_EXT_shadow_samplers`, normal offset, blend cascade, fade jarak |
| [IMPLEMENTED] | Debug: `showCascade`, `showShadowMap`, `showSunDirection`, `freezeSun`, `freezeShadowCamera`, `perfCounters` |
| [IMPLEMENTED] | Blob shadow GTA hanya disembunyikan bila bayangan matahari ≥ 0.25 (hujan/mendung: blob tetap, mobil/ped tidak "melayang") |
| [IMPLEMENTED] | Adaptive quality (hysteresis 4 s turun / 12 s naik), JNI kontrol, hot‑reload config & shader engine |
| [IMPLEMENTED] | Failsafe: shader patch gagal → shader asli GTA; FBO gagal → pass dibuang (rasterizer discard); context berganti → resource dibuat ulang |
| [NEEDS BINDING] | Tidak ada. Semua symbol yang dipakai diekspor libGTASA 2.10 arm64 (dicek dengan `llvm-nm -D`) |
| [NEEDS DEVICE TEST] | Belum dijalankan di perangkat (lingkungan ini tidak punya NDK/perangkat). Lihat §8 |
| [PERFORMANCE RISK] | Caster pass memakai shader GTA penuh (fragment shader tetap jalan walau depth‑only). Jumlah draw call naik ~1.5–2.5× di siang hari. Kontrol: `maxCasters`, `cascades`, `distance`, `offscreenCasters`, adaptive |
| [OPTIONAL] | Phase 6+ (tonemap, bloom, fog, wet road, SSAO, godray, LUT) |

---

## 2. Fakta libGTASA 2.10 arm64 yang menentukan desain (diverifikasi dari binary)

`libGTASA.so` yang dikirim **tidak di‑strip dan punya DWARF**, jadi layout struct dan
alur fungsi diambil dari binary, bukan ditebak.

1. **Render thread terpisah.** `RenderQueue::RenderQueue()` men‑set `multiThread = useMutex = 1`
   (`strh 0x101` di +0x3E8), `RenderQueue::Initialize()` meluncurkan `GraphicsThread` yang
   memegang EGL context. Semua GL GTA berjalan di thread itu; game thread hanya menulis command.
   → GL call langsung dari hook game thread tidak punya context. Ini juga alasan jalur lama
   `EglPostFX::RenderBeforeHud()` / `WorldSunShadow::QueueFrame()` di `Render2dStuff` tidak
   pernah benar‑benar bekerja di arm64.
   **Solusi:** command `rqDebugMarker` (id 47) tidak pernah di‑enqueue game (tidak ada store 47 ke
   `curQueueingCommand` di `.text`). Slot itu dipakai sebagai "jalankan fungsi ini di GL thread",
   sehingga GL engine berjalan **berurutan** dengan draw GTA. Protokol enqueue meniru persis kode
   inline GTA (`RQRenderTarget::Clear` 0x269840).
2. **Urutan frame (`Idle`, 0x4D8E60):** `ConstructRenderList → CRenderer::PreRender →
   CWorld::ProcessPedsAfterPreRender → CRealTimeShadowManager::Update → CMirrors::BeforeMainRender →
   DoRWStuffStartOfFrame (clear + begin kamera utama) → RenderScene → RenderWeaponPedsForPC →
   RenderEffects → Render2dStuff → RwCameraEndUpdate → ShowRaster`.
   → Shadow pass dipasang di `CRealTimeShadowManager::Update` (ped sudah dianimasi, kamera utama
   belum di‑clear → tidak ada load/store framebuffer ekstra di GPU tiler).
3. **Shader world dibuat runtime** oleh `RQShader::BuildSource` dan dikompilasi di GL thread oleh
   `ES2Shader::Build(ps, vs)` (dipanggil hanya dari `RQ_Command_rqBuildShader`). Client ini sengaja
   memakai generator native (lihat komentar di `game/hooks.cpp`), jadi engine **membungkus
   `ES2Shader::Build`** dan menyisipkan kode receiver ke teks shader native.
4. **`ES2Shader::CheckCompile` (0x2616A0)** mencetak shader yang gagal per statement ke buffer stack
   0x208 byte → statement > ~510 karakter akan overflow. Semua sumber hasil patch dicek (maks 440).
5. **Kamera RW → matrix GL** (`_rwOpenGLCameraBeginUpdate` 0x23ED98): view
   `x=-right·d, y=up·d, z=-at·d`; parallel projection memakai `projMat` statis (0x8520D0).
   `Math/Matrix4.cpp` mereproduksi rumus ini persis, sehingga matrix receiver = matrix yang
   dipakai GPU saat merender caster.
6. **Arah matahari GTA** (`CTimeCycle::CalcColoursForPoint` 0x5030B0):
   `a = menit_hari·2π/1440`, `toSun = normalize(sin a + 0.7, −0.7, 0.2 − cos a)`; dihitung ulang
   tiap frame (index ring `& 0xF`). Puncak ±50° (siang), terbenam ±18:40.
7. **`CPed::Render` menambah ped ke `CVisibilityPlugins::ms_weaponPedsForPC`** (CLinkList), list
   dikosongkan `Idle` hanya setelah `RenderWeaponPedsForPC` pass utama. Shadow pass menghapus lagi
   entri yang ia tambahkan (urutan unlink/relink sama dengan `Idle` 0x4D9080).
8. Unit tekstur GTA: 0–2 (material) dan 5 (upload). Shadow atlas memakai **unit 7**.

---

## 3. Analisis arsitektur (A–F)

### A. Hook yang dibutuhkan

| Symbol (diekspor libGTASA) | Addr build ini* | Fungsi | File |
|---|---|---|---|
| `_ZN22CRealTimeShadowManager6UpdateEv` | 0x6DCC30 | titik shadow pass per frame | `GraphicsHooks.cpp` |
| `_ZN9ES2Shader5BuildEPKcS1_` | 0x261C60 | injeksi receiver + registrasi program (GL thread) | `GraphicsHooks.cpp` |
| `_ZN8CShadows21StoreShadowForVehicleEP8CVehicle12VEH_SHD_TYPE` | 0x6DDE2C | blob mobil disembunyikan saat bayangan matahari aktif | `GraphicsHooks.cpp` |
| `_ZN8CShadows23StoreShadowForPedObjectEP7CEntityffffff` | 0x6DE5C4 | blob ped/objek | `GraphicsHooks.cpp` |
| `_ZN8CShadows19StoreRealTimeShadowEP9CPhysicalffffff` | 0x6DE7E0 | realtime shadow GTA | `GraphicsHooks.cpp` |
| `_ZN8CShadows18StoreShadowForPoleEP7CEntityfffffj` | 0x6DEC0C | bayangan tiang palsu | `GraphicsHooks.cpp` |
| `RenderQueue::commands[17]` (rqSelectShader) | tabel heap | upload uniform receiver setelah program aktif | `RenderQueueBridge.cpp` |
| `RenderQueue::commands[47]` (rqDebugMarker, tak terpakai) | tabel heap | callback GL engine | `RenderQueueBridge.cpp` |
| `Render2dStuff` | sudah di‑redirect client | batas 3D→HUD: receiver mati, overlay debug | `app/app_game.cpp` |
| `CRealTimeShadowManager::DoShadowThisFrame` | sudah di‑redirect client | realtime shadow GTA tidak dirender | `RealTimeShadowManager.cpp` |

\*Alamat hanya referensi; kode memakai nama symbol (dlsym/ShadowHook), **tidak ada offset baru**.
Tidak ada hook `eglSwapBuffers` (tidak dibutuhkan; post‑process nanti di batas Render2dStuff).

### B. Bagian RenderWare yang dipakai
`RwCamera` paralel milik engine (`RwCameraCreate`, `RwFrameCreate`, `rwPARALLEL`,
`RwCameraSetViewWindow/NearClipPlane/FarClipPlane`), frame `modelling` + `RwMatrixUpdate` +
`RwFrameUpdateObjects`, `RwCameraBeginUpdate/EndUpdate`, `RwRenderStateSet`. Raster kamera cahaya =
raster kamera utama (tidak membuat render target RW baru). Semua wrapper RW memakai binding yang
sudah ada di `game/RW/*`.

### C. Data GTA
`Scene.m_pRwCamera` (LTM, viewWindow, near/far), `CClock` (jam/menit/detik), `CWeather`
(Rain, CloudCoverage, Foggyness, WetRoads, UnderWaterness, InTunnelness), `CTimeCycle::GetVectorToSun()`,
`CGame::currArea`, `CRenderer::ms_aVisibleEntityPtrs/ms_nNoOfVisibleEntities` (versi relokasi client),
sektor `CWorld` (`GetSector`, `GetRepeatSector`), `CModelInfo`/`CColModel` (bounding sphere).

### D. Shadow caster
GTA sendiri yang merender caster: kamera RW paralel per cascade → GTA meng‑upload
`ViewMatrix/ProjMatrix` cahaya → `CRenderer::RenderOneNonRoad(entity)` untuk tiap caster. Jadi
skinning ped, animasi, roda/pintu kendaraan, format vertex terkompresi dan alpha‑test daun
semuanya ikut **tanpa** shader depth terpisah. Callback RQ mengalihkan draw ke atlas FBO
(depth‑only, tanpa color attachment), set viewport/scissor tile, slope bias; lalu memulihkan state
persis sebelum `RwCameraEndUpdate`. Semua cascade dalam satu render pass. Caster dipilih per
cascade dengan bounding sphere vs light box; caster kecil dilewati di cascade jauh; ped hanya di
cascade 0–1.

### E. Shadow receiver
Kode receiver (`glShader/Entity/*.shader` + `realtimeShadow.shader`) disuntik ke semua shader 3D world (yang punya `Out_FogAmt` dan
`ViewPos`; HUD/2D/sphere‑map tidak disentuh):
`warna *= mix(1, tint, (1 − visibility) · strength · share)` + boost matahari kecil di area terang.
`share` = porsi cahaya matahari: untuk shader GTA ber‑lighting dihitung dari suku directional
GTA sendiri (tidak double lighting); untuk gedung prelit = `prelitDirectShare`. Bayangan tidak
pernah hitam (tint ambient langit). Mode uniform per fase: *caster* (tanpa sampling, alpha cutoff),
*receive*, *off* (HUD/refleksi). Atlas tidak pernah ter‑bind saat menjadi depth attachment.

### F. Post‑process
Akan dijalankan di awal `Render2dStuff` (sebelum `emu_FlushAltRenderTarget`, sebelum HUD/SA‑MP UI/
CEF/chat/dialog/keyboard/textdraw) melalui callback RQ yang sama. Tidak perlu hook `eglSwapBuffers`.

---

## 4. Urutan render (sesudah patch)

```
Game thread                                   RenderQueue (GL) thread
-----------                                   -----------------------
CGame::Process / CTimeCycle (sun vector)
CRenderer::ConstructRenderList / PreRender
CRealTimeShadowManager::Update  ──hook──►
  SunManager + TimeCycleFX
  CascadeShadow (split, matrix, snap)
  ShadowCasterCollector
  enqueue BeginShadowPass ──────────────────► atlas/dummy dibuat/dicek; receiver = caster mode
  per cascade: RwCameraBeginUpdate(light)
    enqueue BeginCascade ───────────────────► bind atlas FBO, clear, tile viewport/scissor, slope bias
    RenderOneNonRoad(casters) ───(draw RQ)──► depth ke atlas (shader GTA)
    RenderWeaponPedsForPC + trim list
  enqueue EndCascade ───────────────────────► pulihkan FBO/viewport/raster
  RwCameraEndUpdate(light)
  enqueue EndShadowPass ────────────────────► receiver = receive (matrix cascade), atlas di unit 7
DoRWStuffStartOfFrame → RenderScene ─────────► world menerima bayangan
RenderEffects
Render2dStuff ──► OnEndWorld ───────────────► receiver = off, dummy di unit 7, overlay debug
  HUD / SA-MP / CEF / text                    (tidak terkena bayangan)
ShowRaster → eglSwapBuffers
```

---

## 5. File

### Baru — `jni/graphics/`
| File | Isi |
|---|---|
| `GraphicsEngine.h/.cpp` | singleton, alur frame, request thread‑safe, adaptive quality, perf log |
| `GraphicsConfig.h/.cpp` | `IniFile` (format SDX) + `GraphicsConfig` (Config.ini → Advanced.ini) + preset kualitas |
| `GraphicsLog.h/.cpp` | logcat `EagleGFX` + `logOutput.log` |
| `GraphicsPaths.h` | semua path TESTLIT (susunan gaya SA_DOX) |
| `GraphicsHooks.h/.cpp` | hook ShadowHook |
| `GraphicsJNI.cpp` | JNI `com.holy.game.core.GraphicsNative` (19 fungsi) |
| `GameRenderBridge.h/.cpp` | satu‑satunya akses ke kode/data GTA (dlsym + binding client) |
| `RenderQueueBridge.h/.cpp` | command GL di thread RenderQueue |
| `GLCaps.h/.cpp` | deteksi GPU |
| `GLStateBackup.h/.cpp` | backup/restore state GL |
| `GlShader.h/.cpp` | pembaca `.shader`: section `<vert>/<frag>`, `#include`, parameter `@` → `SG_User` + `SG_InitUser()` |
| `ShaderUniforms.h/.cpp` | `shaderUniform.ini`: baca/simpan, slot, nilai live (thread‑safe) |
| `ShaderManager.h/.cpp` | baca file (TESTLIT → bawaan), `CompileShader/CreateProgram`, cache, reload |
| `ShaderPatcher.h/.cpp` | injeksi receiver per entitas ke shader GTA + registry uniform |
| `EmbeddedGlShader.h` | **dibuat otomatis** oleh `tools/embed_glshader.py` dari `TESTLIT/graphics/glShader/` |
| `FrameBuffer.h/.cpp` | atlas depth FBO + tekstur dummy |
| `SunManager.h/.cpp` | `SunLight` |
| `TimeCycleFX.h/.cpp` | profil waktu & cuaca (+ `data/eagle_timecyc.dat`) |
| `CascadeShadow.h/.cpp` | `CalculateCascadeSplits`, `UpdateLightMatrices` |
| `ShadowCasters.h/.cpp` | koleksi caster |
| `ShadowManager.h/.cpp` | `RenderShadowCasters`, `UploadCascadeUniforms`, Begin/EndShadowPass (GL) |
| `DebugOverlay.h/.cpp` | tampilan atlas (`glShader/Debug/ShadowMap.shader`) |
| `Math/Vector3.h`, `Math/Matrix4.h/.cpp` | matematika (konvensi RW) |

### Diubah — `patches/existing_files.patch` (salinan lengkap juga di `jni/`)
* `main.cpp`: `EglPostFX::InstallHooks()` (AML) → `gfx::GraphicsEngine::Get().EarlyInit()`.
  `AmlBootstrap` tetap dipanggil (untuk mod AML Anda), grafis tidak bergantung padanya.
* `app/app_game.cpp`: `Render2dStuff` memanggil `GraphicsEngine::OnEndWorld()` menggantikan
  `WorldSunShadow`/`EglPostFX`; `GetSunShadowFrame()` dihapus.
* `game/Shadow/RealTimeShadowManager.cpp`: `DoShadowThisFrame_hook` dilewati saat bayangan matahari aktif.
* `CMakeLists.txt`: daftar eksplisit sumber engine (+`REMOVE_DUPLICATES`). `Android.mk`: komentar
  (auto‑scan sudah mengambil `graphics/`).

### Java — `android/` (diff: `patches/java_files.patch`, susunan sama dengan `new_java.zip`)
Project Java ini **tidak boleh menambah resource** (`res/values/public.xml` mengunci ID; resource baru
pernah membuat DuelsHud crash, lihat komentar di `ModernUi.kt`). Karena itu semua tampilan dibuat
dari kode dan `res/` tidak diubah sama sekali.
* Baru `java/com/holy/game/core/GraphicsNative.java` — deklarasi JNI + simpan/terapkan pilihan pemain
  (SharedPreferences lewat `Storage`, kunci `eagle_gfx_*`).
* Baru `java/com/holy/game/gui/modern/MenuStyle.kt` — warna, panel sudut terpotong
  (`CutCornerDrawable`), slider kuning (`SliderView`), font yang sudah ada (bebas_bold, akrobat_bold,
  gilroy, din_pro).
* `java/com/holy/game/gui/modern/ModernPauseMenu.kt` — ditulis ulang, API & alur native sama
  (`ModernMenu` tidak berubah):
  * bar atas: server, nama, jam, ID/skor/ping, uang, tombol ✕; rail kiri: PETA, GAME, GRAFIS, EFEK,
    AUDIO, KONTROL + tombol kuning LANJUTKAN; panel kanan: baris dengan chip pilihan / slider / aksi;
    hint bar menjelaskan baris yang dipilih.
  * GRAFIS: semua pengaturan GTA sebelumnya + bagian **BAYANGAN MATAHARI • EAGLE** (Grafis EAGLE,
    Bayangan matahari, Kualitas Rendah/Sedang/Tinggi/Ultra, Jarak 40–300 m).
  * EFEK: nilai `shaderUniform.ini` dengan label Indonesia (geser = langsung terlihat, lepas = disimpan
    ke file), DEBUG (warna cascade, shadow map), BERKAS (muat ulang Config.ini, reset efek, status).
  * Bila `libmultiplayer.so` belum berisi EAGLE, bagian EAGLE menampilkan "EAGLE tidak tersedia"
    (tidak crash).
* `Samp.kt`: `GraphicsNative.applySavedSettings()` tepat setelah `initSAMP(...)`, sebelum GTA membuat
  shader, sehingga "bayangan aktif" dari pemain sudah ikut saat shader dunia dikompilasi.
* `DialogClientSettings.java` **tidak** diubah (tab Grafis versi XML dari paket sebelumnya dihapus karena
  menambah layout/ID baru).

`graphics/postfx/*` dan `graphics/sun/*` (jalur lama berbasis AML) **tidak lagi dipanggil**;
masih ikut terkompilasi tapi inert. Boleh dihapus setelah engine baru teruji.

### Eksternal — salin `TESTLIT/graphics/` ke `/storage/emulated/0/TESTLIT/graphics/` (gaya SA_DOX)
```
TESTLIT/graphics/
├── Config.ini              pengaturan utama   ([Graphics] bEnabled/iQuality, [Shadow] ..., [Performance])
├── Advanced.ini            pengaturan ahli    (bias, split, caster, [Sun], [Debug]) - menimpa Config.ini
├── shaderUniform.ini       nilai shader LIVE  (class / tipe / nama / nilai / min / max / step)
├── logOutput.log           dibuat engine
├── glShader/
│   ├── realtimeShadow.shader      cascade + PCF + SG_Shade (dipakai semua entitas)
│   ├── Entity/Building.shader     SG_Apply untuk gedung/jalan/pohon/objek
│   ├── Entity/Vehicle.shader      SG_Apply kendaraan (specular redup di bayangan)
│   ├── Entity/Character.shader    SG_Apply ped (MinLight agar wajah terbaca)
│   ├── Depth/Caster.shader        pass caster (alpha cutoff daun/pagar)
│   ├── Debug/Cascade.shader       warna cascade (showCascade)
│   └── Debug/ShadowMap.shader     <vert>+<frag> overlay atlas (showShadowMap)
├── data/eagle_timecyc.dat  tampilan SUNRISE/DAY/SUNSET/NIGHT + RAMPS jam transisi
└── textures/               disiapkan untuk PHASE 6 (belum dibaca)
```
Format `.shader`:
* `<vert> … </vert>` / `<frag> … </frag>`; file tanpa tag = satu stage.
* `#include "realtimeShadow.shader";` — dicari di folder file itu dulu, lalu dari `glShader/`;
  tiap file hanya disertakan sekali.
* `@float Nama < class = "Kelas", name = "Key", default = 1.0; min = 0; max = 2; step = 0.05; >`
  (juga `@int`, `@bool`) — nilai diambil dari `shaderUniform.ini` dan bisa diubah saat game jalan.
  Hanya untuk shader receiver (Entity/*), dipakai di dalam fungsi.
* Mengubah isi `.shader` = restart game (GTA mengompilasi shader dunia sekali). File yang gagal
  dibaca/melanggar kontrak (`SG_Apply`, `SG_SpecVis`, statement < 440 karakter) diganti salinan
  bawaan dan dicatat di log. Jalankan `tests/run_host_tests.sh` di PC sebelum menyalin ke HP.

Beda dengan SA_DOX (disengaja, sesuai batasan proyek): tidak ada `data/script/*.x` (itu sistem
skrip ala CLEO), tidak mengganti shader GTA secara utuh (receiver disuntik ke shader GTA asli,
caster memakai shader GTA sendiri), tidak ada `.assets/` (menu ada di pause menu Java, tab GRAFIS/EFEK). Tidak ada
kode/tekstur SA_DOX yang disalin; hanya susunan & format berkasnya yang diikuti.
`graphics.ini` versi pertama masih dibaca bila `Config.ini` dan `Advanced.ini` tidak ada.

---

## 6. Build
1. Salin `jni/graphics/*` (baru) ke `jni/graphics/` project; terapkan
   `patches/existing_files.patch` (`git apply` atau `patch -p1` dari folder yang berisi `jni/`),
   atau timpa 5 file dari `jni/`.
2. `ndk-build` / CMake seperti biasa. Library tambahan: tidak ada (GLESv3, EGL, shadowhook sudah ada).
3. Java: salin isi `android/` ke project Java (atau `patches/java_files.patch`, path sama dengan
   `new_java.zip`). Tanpa Java pun engine jalan dengan `Config.ini`.
4. Setelah mengedit `TESTLIT/graphics/glShader/*.shader`: `python3 tools/embed_glshader.py`
   (memperbarui salinan bawaan), lalu `tests/run_host_tests.sh` (butuh clang++ + glslangValidator).

---

## 7. Konfigurasi singkat
| Kualitas | Cascade | Resolusi | Jarak | PCF | Atlas (D16) |
|---|---|---|---|---|---|
| LOW (0) | 2 | 1024 | 80 m | 1 tap (bilinear HW) | 2048×1024, 4 MiB |
| MEDIUM (1) | 3 | 1536 | 120 m | 4 tap | 3072², 18 MiB |
| HIGH (2) | 3 | 2048 | 160 m | 9 tap + blend | 4096², 32 MiB |
| ULTRA (3) | 4 | 2048 | 220 m | 9 tap + blend, D24 | 4096², 64 MiB |

Anti acne/peter‑panning: `depthBias` (meter) + `normalBias` (texel, searah normal) +
`slopeBias/slopeUnits` (polygon offset saat caster). Anti shimmer: snapping texel + radius cascade
dikuantisasi 5% + dead‑zone arah matahari (`[Sun] fUpdateThreshold` di Advanced.ini).

---

## 8. Uji target pertama (siang, player di jalan)
1. Set jam game 12:00 dan cuaca cerah (mis. lewat `SetWorldTime`/`SetWeather` di server).
2. `logOutput.log` yang diharapkan (urutan):
   ```
   [Graphics] EAGLE GraphicsEngine starting ...
   [Config] loaded Config.ini + Advanced.ini (N keys)
   [Uniform] loaded .../shaderUniform.ini (10 values)
   [TimeCycleFX] loaded .../data/eagle_timecyc.dat (5 rows, ramps 5.0/7.0/16.0/19.0 h)
   [Graphics] Loaded Shader - glShader/Entity/Building.shader [Entity/Building.shader (file), realtimeShadow.shader (file), ...] 4 params
   [Graphics] Loaded Shader - glShader/Entity/Vehicle.shader [...] 5 params
   [Graphics] Loaded Shader - glShader/Entity/Character.shader [...] 5 params
   [Bridge] GTA symbol bridge ready
   [Hooks] hooked _ZN9ES2Shader5BuildEPKcS1_
   [Hooks] hooked _ZN22CRealTimeShadowManager6UpdateEv
   [GLCaps] OpenGL ES version / GPU / GLSL / MaxTextureSize / DepthTexture support ...
   [ShaderPatch] receiver path: hardware|manual compare
   [ShaderPatch] receiver #1 (Building #1): program ... lit=0 alpha=1 taps=9
   [ShaderPatch] receiver #.. (Vehicle #1) / (Character #1): ...
   [RenderQueue] bridge installed: queue=... multiThread=1
   [Shadow] light camera created
   [FrameBuffer] depth FBO 4096x4096 D16 ...
   [Shadow] Shadow resolution: 2048 per cascade ... Shadow cascades: 3
   [Graphics] first shadow frame: DAY hour=12.00 sun elev=~50 ...
   ```
3. Debug cepat: `showShadowMap=1` (atlas kiri bawah: siluet gelap gedung/pohon/ped/mobil),
   `showCascade=1` (merah/hijau/biru), `perfCounters=1` (ms CPU + jumlah caster) — di `Advanced.ini`
   atau langsung dari pause menu (tab EFEK → DEBUG).
4. Jika ada error compile shader: `[ShaderPatch] world.frag (patched): ...` di log, game tetap
   jalan dengan shader asli (tanpa bayangan pada material itu).
5. Jika atlas terisi (overlay benar) tetapi dunia tidak menerima bayangan dan tidak ada baris
   `[ShaderPatch] receiver #...`: shader world di perangkat itu tidak memakai distance fog
   (`Out_FogAmt`), yang dipakai sebagai penanda shader 3D. Kirim `logOutput.log` + `[Debug] bLog = 1`
   agar kriteria di `ShaderPatcher::PatchSources` disesuaikan.
6. Jika layar hitam/aneh setelah shadow pass: matikan `[Shadow] bEnabled = 0`, kirim `logOutput.log`
   (urutan callback RQ tercatat), jangan ubah kode RenderQueue tanpa log.

Diverifikasi di lingkungan pengembangan (bukan perangkat): `tests/run_host_tests.sh` membangun
ketiga receiver dari `glShader/` (sama persis dengan salinan bawaan), menambal 6 contoh shader
dunia GTA dalam 12 kombinasi (hardware/manual compare × 1/4/9 tap × blend), lalu 110 shader lolos
`glslangValidator` GLSL ES 1.00/3.00 dan 48 pasangan VS/PS lolos link; uji parser `Config.ini`/
`Advanced.ini` (format SDX), `shaderUniform.ini` (baca → set → simpan → baca), `eagle_timecyc.dat`;
klasifikasi entitas (gedung/ped/kendaraan/pohon). Uji unit matematika CSM sebelumnya (split
17.6/48.9/160 m, snapping texel). Semua file engine lolos syntax‑check aarch64‑android C++20 dengan
`-Wall -Wextra`. Kotlin: `ModernPauseMenu.kt` + `MenuStyle.kt` + file modern menu yang ada lolos kotlinc 1.9
(mode K1 dan K2) terhadap framework Android 11 (Robolectric android‑all). Java: `javac` lolos; 19 deklarasi `native` cocok dengan 19 fungsi
JNI C++ (dicek dengan `javac -h`).

---

## 9. Risiko & batasan yang diketahui
* [PERFORMANCE RISK] Caster pass = draw call GTA tambahan. Mulai dari HIGH; jika < 30 FPS pakai
  MEDIUM, `offscreenCasters=0`, `maxCasters=600`. Adaptive quality otomatis menurunkan jarak.
* Bayangan dari entitas di luar layar memakai scan sektor (gedung/objek/kendaraan). Ped di luar
  layar mati secara default (animasi tidak di‑update GTA saat off‑screen).
* Rumput prosedural (`CPlantMgr`) belum menjadi caster (mahal, noise).
* Shader GTA yang dibuat sebelum engine aktif tidak dipatch (engine diinisialisasi di
  `JNI_OnLoad`, sebelum render thread GTA dibuat, jadi normalnya semua shader dipatch).
* Mengubah `iPcf`, `bCascadeBlend`, `bHardwarePcf`, `bWater` dan isi `glShader/*.shader` butuh restart
  game. Nilai `shaderUniform.ini` (Strength, Softness, SunBoost, ...) berubah langsung.
* Klasifikasi entitas memakai teks shader GTA: objek dunia ber‑lighting dengan specular dianggap
  Vehicle (jarang), objek skinned dianggap Character. Akibatnya hanya beda tampilan bayangan.
* Lampu jalan/neon malam belum (phase 7: point light manager).

---

## 10. Contoh gambar (simulasi) dan mockup menu
`docs/preview/*.jpg` dibuat dengan `docs/preview/simulasi.html` (three.js r128): kota contoh,
arah matahari dari rumus GTA (`CalcColoursForPoint`), nilai `Config.ini`/`eagle_timecyc.dat`, dan
rumus `SG_Shade` yang sama dengan `realtimeShadow.shader` diterapkan di atas warna "GTA" sebelum
fog. **Ini simulasi di browser, bukan screenshot game** — hasil di perangkat tergantung model,
tekstur dan timecyc GTA. Buka file HTML dengan `#shot=noon|sunset|night|rain|cascade&mode=eagle|gta`.

`docs/preview/menu_*.jpg` adalah **mockup HTML** pause menu (warna, ukuran dp dan font sama dengan
`MenuStyle.kt`, layar 914×411 dp), bukan screenshot dari HP.

---

## 11. Berikutnya
PHASE 6 (tonemap ACES + exposure + bloom quarter‑res) di batas `Render2dStuff` memakai
`RenderQueueBridge` + `ShaderManager` + `GLStateBackup` yang sudah ada, lalu fog (7), wet road (8),
SSAO (9), godray (10), optimasi (11).

**LANJUTKAN DARI FILE: `jni/graphics/PostProcess.cpp` (PHASE 6)**
