# Fix clothes system: tekstur karakter "file not found"

Perbaikan untuk paket `RPEAGLE_clothes_bug.zip` (EAGLE clothes revision 3, ped 158/298).

Gejala di log:

```
NVFOpen hook | Error: file not found (/storage/emulated/0/TESTLIT//storage/emulated/0/TESTLIT/character/textures/user_eg_skin_m.png)
```

Penyebab: `ClothesLoader` dan `FaceManager` membaca PNG dengan `RtPNGImageRead(<path lengkap>)`. Fungsi itu membuka file lewat `NvFOpen` milik game, dan `BuildGameFilePath` selalu menaruh root storage di depan nama file, sehingga root tertulis dua kali. Akibatnya semua tekstur dan semua aset modular (body, face, hair, top, pants, shoes) gagal, karakter tidak pernah siap, dan client mencoba ulang tiap 6 detik.

## Isi folder

| File | Perubahan |
| --- | --- |
| `jni/game/character/CharacterTexture.h/.cpp` | Baru. PNG dibaca langsung dengan stb_image dari path lengkap, lalu dibuat raster 32-bit seukuran gambar. |
| `jni/game/clothes/ClothesLoader.cpp` | Memakai loader baru. Error dicatat ke log `EagleCharacter` beserta path. |
| `jni/game/character/FaceManager.cpp` | Memakai loader baru. Overlay tidak di-pad ke power-of-two, karena padding menggeser UV skin 104x104. |
| `jni/game/hooks.cpp` | `BuildGameFilePath` tidak menambahkan root storage pada path yang sudah diawali root itu. |
| `clothes_texture_path_fix.patch` | Semua perubahan di atas (juga README_INSTALL.md), untuk `patch -p1` di root paket. |

## Pasang

Timpa file di atas ke folder `jni/` project Anda (file baru ikut ter-build otomatis, karena CMake dan ndk-build memakai glob), atau dari root paket jalankan:

```sh
patch -p1 < clothes_texture_path_fix.patch
```

Lalu build ulang `libmultiplayer.so`. Gamemode, CEF, SQL, dan TESTLIT tidak berubah.

## Yang sudah dan belum diuji

- Host: 30 PNG TESTLIT terbaca dan byte RGBA-nya identik dengan Pillow. Pembuatan raster ukuran persis, ukuran dibulatkan, dan stride lebar bersih di AddressSanitizer/UBSan tanpa raster bocor. 324 DFF lolos `ValidateDff` dan tag frame `EAGLE_r*` cocok dengan tekstur katalog.
- Belum: build NDK ARM64 dan uji di HP.

Setelah build baru, baris `TESTLIT//storage/...` tidak boleh muncul lagi. Jika masih FC, ambil `adb logcat -b crash -d` dan baris log `EagleCharacter`.
