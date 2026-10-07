# EAGLE Graphics Engine (GTA SA Android 2.10 arm64, tanpa AML)

* `jni/graphics/` — engine (salin ke `jni/graphics/` project).
* `jni/main.cpp`, `jni/app/app_game.cpp`, `jni/game/Shadow/RealTimeShadowManager.cpp`,
  `jni/CMakeLists.txt`, `jni/Android.mk` — file project yang diubah (diff: `patches/existing_files.patch`).
* `android/` — sisi Java: pause menu GTA bergaya battle‑royale dengan pengaturan EAGLE (tab GRAFIS & EFEK)
  + `GraphicsNative`, tanpa resource baru; susunan sama dengan `new_java.zip` (diff: `patches/java_files.patch`).
* `TESTLIT/graphics/` — salin ke `/storage/emulated/0/TESTLIT/graphics/`. Susunan gaya SA_DOX:
  `Config.ini`, `Advanced.ini`, `shaderUniform.ini`, `glShader/`, `data/eagle_timecyc.dat`
  (engine menulis `logOutput.log`).
* `tools/embed_glshader.py` — perbarui salinan bawaan shader setelah mengedit `glShader/`.
* `tests/run_host_tests.sh` — uji di PC: build receiver, patch shader GTA contoh, glslang, parser.
* `docs/GRAPHICS_ENGINE.md` — arsitektur, titik hook yang diverifikasi dari libGTASA, format berkas, status, cara uji.
* `docs/preview/` — contoh gambar (SIMULASI WebGL dari model lighting engine) dan mockup menu (HTML); bukan screenshot game.
