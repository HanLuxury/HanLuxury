# Fix clothes system EAGLE (ped 158/298)

Perbaikan untuk paket `RPEAGLE_clothes_bug.zip`: client (JNI), gamemode, dan catatan instalasi. CEF, SQL, dan TESTLIT tidak perlu diubah; semuanya sudah dicek cocok.

## Masalah yang diperbaiki

| Masalah | Akibat di game | File |
| --- | --- | --- |
| PNG karakter dibaca lewat `RtPNGImageRead` → `NvFOpen`, yang menaruh root storage dua kali (`TESTLIT//storage/emulated/0/TESTLIT/...`). | Semua tekstur gagal, karakter tidak pernah siap, pakaian tidak tampil. | `jni/game/character/CharacterTexture.*` (baru), `ClothesLoader.cpp`, `FaceManager.cpp`, `hooks.cpp` |
| Overlay wajah/kulit di-pad ke power-of-two. | Overlay skin 104x104 bergeser. | `FaceManager.cpp` |
| Log `EagleCharacter` hanya ke logcat; banyak titik gagal tanpa pesan. | Error tidak terlihat di log client. | `CharacterLog.h` (baru), `CharacterPlayer.*`, `CharacterManager.cpp`, `CharacterRenderWare.cpp`, `ClothesStreaming.cpp` |
| Setiap penolakan transaksi (uang kurang, harga beda, dll.) membuat pemain di-kick. | Beli pakaian gagal = keluar dari server. | `gamemodes/.../character_system.inc` |
| Save/buy dibuang tanpa balasan (belum berjalan kaki, jeda 500 ms, tattoo/freckles di luar creator). | Menu macet di "Menyimpan…". | `gamemodes/.../character_system.inc` |

## Pasang

1. Timpa file `jni/` ke project Android Anda, lalu build ulang `libmultiplayer.so`. File baru ikut ter-build otomatis, karena CMake dan ndk-build memakai glob.
2. Timpa `gamemodes/SERVER/player/character/character_system.inc`, compile ulang `Main.pwn`, lalu restart server.
3. Atau dari root paket asli jalankan `patch -p1 < clothes_fix.patch`. Patch ini juga memperbarui `README_INSTALL.md`.

## Cek di game

- Log client harus berisi `EagleCharacter: catalog ready revision 3 at ...`, lalu `EagleCharacter: player <id>: <n> parts on ped model 158` (atau 298).
- Baris `TESTLIT//storage/...` tidak boleh muncul lagi.
- Jika pakaian tetap tidak tampil, baris `EagleCharacter` menyebut alasannya. Jika FC, kirim baris itu dan `adb logcat -b crash -d`.

## Tes host

`bash qa/run.sh <root paket hasil fix> <folder TESTLIT>` menjalankan:

- Loader PNG (ASan/UBSan) pada 30 tekstur.
- Katalog client dan paket appearance dari server.
- Retarget 324 DFF ke `cwmofr` dan `cat`.
- Cek silang katalog client, server, dan SQL.
- Alur CEF di Chromium (butuh node + playwright).

`qa/ec_ui_check.pwn` adalah stub untuk cek compile fungsi Pawn yang diubah (pawncc 3.10.10).

Belum diuji: build NDK, compile gamemode lengkap, dan uji di HP.
