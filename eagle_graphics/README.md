# EAGLE Graphics Engine (GTA SA Android 2.10 arm64, tanpa AML)

* `jni/graphics/` — engine baru (salin ke `jni/graphics/` project).
* `jni/main.cpp`, `jni/app/app_game.cpp`, `jni/game/Shadow/RealTimeShadowManager.cpp`,
  `jni/CMakeLists.txt`, `jni/Android.mk` — file project yang diubah (diff: `patches/existing_files.patch`).
* `TESTLIT/graphics/` — salin ke `/storage/emulated/0/TESTLIT/graphics/` (graphics.ini + shader).
* `docs/GRAPHICS_ENGINE.md` — arsitektur, titik hook yang diverifikasi dari libGTASA, status, cara uji.
* `docs/java/GraphicsNative.java` — kelas Java untuk JNI kontrol (opsional).
