# Vice City untuk client eagle_v50 (SA-MP 0.3.7, GTA SA 2.10 arm64)

Map [casualmind/samp-vice-city](https://github.com/casualmind/samp-vice-city) dibuat untuk SA-MP 0.3.DL (PC):
2412 model kustom (`AddSimpleModel`, ID -30000 .. -27587) dan 7498 objek. Client ini 0.3.7, jadi map-nya
dipasang oleh **client sendiri**: penempatan semua objek sudah tertanam di client, sedangkan model, tekstur
dan collision dibaca dari satu folder di HP.

    /storage/emulated/0/TESTLIT/vice_city/

Folder itu dibuat otomatis saat game pertama kali dibuka (bersama `vice_city.ini`).

> Belum pernah dijalankan di HP. Lihat bagian "Yang sudah dan belum diuji" di bawah.

## Memasang

1. **Build client** seperti biasa. Tidak ada yang perlu didaftarkan: `Android.mk` dan `CMakeLists.txt`
   mengambil semua `.cpp` di `jni/vicecity/` (folder `tests/` dikecualikan).
2. **Salin file map ke HP.** Dari repo samp-vice-city:

        models/vice_city/*.dff, *.txd   ->  TESTLIT/vice_city/        (2433 .dff + 609 .txd, sekitar 240 MB)
        models/minimap.txd              ->  TESTLIT/vice_city/        (untuk minimap)

   Menyalin folder `models/` utuh ke dalam `TESTLIT/vice_city/` juga dikenali
   (`TESTLIT/vice_city/models/vice_city/...`). File tidak perlu dikonversi: `.dff` dan `.txd` format PC dibaca
   apa adanya, huruf besar/kecil nama file bebas.
3. **Server** (SA-MP 0.3.7 atau open.mp): pasang `server/vice_city_037.amx` di `filterscripts/` dan tambahkan
   ke baris `filterscripts` di `server.cfg`. Untuk minimap tambahkan juga `vc_minimap_037`. Keduanya hanya
   butuh `a_samp` (tanpa plugin streamer). Sumbernya `server/*.pwn`.
   Di server open.mp script asli dari repo (`vice_city.pwn`, `vc_minimap.pwn`) juga bisa dipakai apa adanya;
   lihat "Memakai script asli" di bawah.
4. Buka game, masuk server, `/gotovc`.
5. Kalau ada yang tidak muncul, buka `TESTLIT/vice_city/vice_city_status.txt`: isinya apa yang terpasang, apa
   yang gagal, dan alasannya.

## Kenapa map dipasang client, bukan dikirim server

- Protokol 0.3.7 tidak punya `AddSimpleModel`, jadi model kustom tidak bisa dikirim server.
- 0.3.7 membatasi 1000 objek per pemain. Di titik terpadat map ini butuh sekitar 1650 objek sekaligus
  (script aslinya meminta 2000 dari plugin streamer). Lewat server, sebagian kota pasti hilang.
- Mesin game Android menolak menautkan objek di x/y >= 3050 ke dunia (pemeriksaan di `CPhysical::Add` yang
  tidak ada di PC), padahal Vice City ada di x 4171 .. 6963. Objek di sana tidak terlihat dan tidak punya
  collision. Client menambal empat instruksi pembanding di fungsi itu (batasnya jadi 15850).

Client membuat objek map di sekitar pemain dan menghapus yang sudah jauh, dengan jarak dan prioritas per jenis
objek seperti di script asli (daratan 1000, bangunan 800, objek dan tanaman 300, interior dan 2dfx 200, masing-
masing +50). Objek ini tidak memakai jatah 1000 objek server.

## Kapan map muncul (`Mode`)

| Mode | Arti |
|---|---|
| `auto` (bawaan) | Map dipasang setelah server menunjukkan bahwa ia memakai map ini, yaitu membuat salah satu dari: objek yang sama persis (model dan posisi) dengan salah satu penempatan map; objek ber-model Vice City (ID negatif); atau objek tanda tanya (model 18631) tepat di posisi sebuah model Vice City, yaitu pengganti yang dikirim open.mp. `vice_city_037` membuat dua objek "suar" jenis pertama (dua pohon palem yang memang bagian map, model 620, jadi aman untuk client lain). Saat koneksi putus map dilepas lagi. |
| `always` | Map dipasang di server mana pun, tanpa filterscript. |
| `server` | Client tidak memasang apa-apa; model hanya disediakan untuk objek yang dibuat server dengan ID negatif. Terkena batas 1000 objek, dan tidak berguna di open.mp (ID negatif tidak pernah sampai ke client 0.3.7). |
| `off` | Map mati total, tidak ada yang didaftarkan ke game. |

Objek kiriman server yang sama persis dengan penempatan yang sudah dipasang client tidak dibuat dobel.

### Memakai script asli

Di `Mode=auto` semua objek script asli dikenali sebagai milik map: tidak dibuat dari kiriman server, tetapi
oleh client sendiri, jadi tidak terkena batas 1000 objek.

- **open.mp.** `vice_city.pwn` dan `vc_minimap.pwn` dari repo jalan apa adanya (butuh plugin streamer dan
  zcmd, seperti di PC). open.mp mengirim objek ber-model kustom ke client 0.3.7 sebagai model 18631 (tanda
  tanya) di posisi yang sama. Tanda tanya yang berada tepat di posisi sebuah model Vice City tidak pernah
  ditampilkan; modelnya yang asli dipasang client.
- **SA-MP 0.3.7.** `AddSimpleModel` tidak ada di 0.3.7: buang `AddVcModels()` dan `AddVC2SASimpleObject()`
  dari `vice_city.pwn`. Objeknya lalu dikirim dengan ID negatif, yang dikenali client. `vc_minimap.pwn`
  cukup tanpa baris `AddSimpleModel`-nya.

## vice_city.ini

    [ViceCity]
    Mode = auto            ; auto | always | server | off
    DrawDistance = 1.0     ; 0.3 - 2.0, pengali jarak tampil. Turunkan kalau HP terasa berat
    MaxObjects = 2500      ; 200 - 8000 objek map sekaligus
    BudgetMs = 4           ; 1 - 50 ms per frame untuk membuat objek yang belum mendesak
    TxdIdleSeconds = 20    ; 5 - 3600, file tekstur yang tidak dipakai dilepas setelah sekian detik
    FirstModelId = 0       ; 0 = ID model kosong paling atas; isi angka lain kalau bentrok dengan mod lain
    SpecialFlags = 0       ; 1 = kaca bisa pecah dan flag khusus lain seperti di script PC
    WorldPatch = 1         ; 0 = jangan tambal CPhysical::Add (map tidak akan terlihat)

Perubahan terbaca setelah game ditutup lalu dibuka lagi.

## Minimap

`vc_minimap_037` menampilkan peta kecil Vice City sebagai textdraw. Teksturnya (`mdl-1500:0` .. `mdl-1500:15`
dan `mdl-1500:player_icon_*`, 25 tekstur) diambil client dari `minimap.txd`. Minimap hanya tampil selama
pemain berada di wilayah Vice City dan hanya untuk client yang melapor versi `0.3.7` persis: client ini, dan
SA-MP PC 0.3.7-R1 (yang akan melihat kotak putih). PC R2 ke atas melapor `0.3.7-R2` dan seterusnya dan tidak
diberi minimap. Hapus `MINIMAP_ONLY_FOR_VERSION` di script kalau semua pemain memakai client ini.

Radar bawaan game tetap kosong di Vice City (laut): ubin radar game hanya mencakup San Andreas.

## Yang perlu diketahui

- **Hanya arm64 dengan libGTASA.so 2.10.** Semua alamat dan susunan data diperiksa terhadap file itu. Sebelum
  menambal `CPhysical::Add`, client memeriksa keempat instruksi aslinya; kalau berbeda, tambalan tidak dipasang
  dan `vice_city_status.txt` menyebutkannya. Build 32-bit tetap bisa dikompilasi, map-nya tidak aktif.
- **ID model.** Model map memakai 2411 ID kosong paling atas di rentang tambahan 20000 .. 24999 (rentang
  yang terpakai tertulis di laporan); ID di bawah 20000 tidak pernah diambil otomatis. ID itu urusan client
  sendiri: server tidak bisa memakainya. Kalau server atau launcher mendaftarkan model Custom DL dengan ID
  yang sama, model Custom DL itu tidak terdaftar dan objeknya tidak dibuat. Pindahkan map dengan
  `FirstModelId` (pencarian naik mulai dari angka itu), atau pakai ID Custom DL dari 20000 ke atas.
- **Slot TXD.** Map butuh 604 slot TXD. Pool TXD client 5000 slot; kalau data game ditambah banyak TXD lain dan
  slotnya tidak cukup, map tidak didaftarkan dan laporan menyebutkan sebabnya.
- **Memori.** Di titik terpadat paling banyak sekitar 23 MB model dan 56 MB tekstur (DXT) ada di memori. Di
  GPU yang tidak mendukung tekstur DXT/S3TC (banyak Mali dan PowerVR lama) game sendiri membongkar DXT menjadi
  16-bit saat memuat, sehingga tekstur yang sama memakan 2-4 kali lipat. Kalau HP kehabisan memori turunkan
  `DrawDistance` dan `MaxObjects`.
- **Model siang/malam.** 184 model punya jam tampil (misal 5-22 dan 22-5) dan ikut jam game. `/day` dan
  `/night` di filterscript mengatur jam itu.
- **Tekstur.** Empat tekstur di repo (pagar dan kisi kawat: `a51_handrail`, `des_rails1`,
  `dt_bridge_rail_texture`, `grid-wire 64HV`) ditulis sebagai DXT3 padahal datanya DXT1. Di PC tekstur itu
  tampil rusak; client membacanya sebagai DXT1 sehingga tampil benar. 37 tekstur yang dipakai model tetapi
  memang tidak ada di TXD-nya (misal `black`, `white64`) dicari game di texdb San Andreas seperti biasa.
- **Yang tidak ada di repo.** `DS_SIGN.txd` tidak ada, jadi model `ds_backlight_sml_vc` (-29688) tidak dipakai.
  Satu baris script menempatkan model -1003 yang tidak pernah didefinisikan; di tempat itu tidak ada yang
  dibuat (objek kiriman server untuk tempat itu juga tidak).
- **Model San Andreas di map.** 4931 penempatan memakai 149 model bawaan (pohon, lampu jalan, bangku, ...).
  Yang modelnya tidak ada di data game HP ini, atau tidak punya collision, dilewati dan disebut di laporan
  (perintah script pembuat objek di game membaca batas collision tanpa memeriksa ada-tidaknya).
- **Collision** dibaca dari bagian SA-MP di dalam tiap `.dff` saat objek pertama model itu dibuat, dan diperiksa
  dulu (semua offset dan jumlah yang akan diikuti game). File yang rusak tidak diteruskan ke game: objeknya
  tetap tampil tanpa collision dan dicatat di laporan.
- **Bayangan** mobil/pemain yang diproyeksikan game hanya jatuh di bangunan peta, tidak di objek (sama untuk
  semua objek SA-MP). Karena seluruh Vice City adalah objek, bayangan jenis itu tidak tampil di sana.
- **Mod lewat modloader.** `<nama tekstur>.png` di folder modloader tetap menang atas tekstur map. Nama model
  dan TXD map di dalam game dibuat unik (`vcm0000`, `vct000`), jadi tidak bentrok dengan nama bawaan.
- `/select` dari script asli tidak dibawa: objek map bukan objek server, jadi tidak bisa dipilih/diedit.

## File yang berubah di luar folder ini

| File | Perubahan |
|---|---|
| `main.cpp` | Hook `RwTextureRead` juga bertanya ke `vc::FindTexture`; hook dipasang kalau map ada. |
| `game/hooks.cpp` | `NvFOpen` melayani arsip model map (`VICECITY\GTA3.IMG`, hanya ada di memori); `vc::InstallHooks()`. |
| `game/Streaming.cpp` | `vc::Tick()` di `Update`, `vc::OnMemoryPressure()` di `MakeSpaceFor`. **Perbaikan** `DeleteRwObjectsBehindCamera`: batas loop sektornya dijepit ke grid 120x120 sehingga loop tidak pernah selesai (game macet) begitu kamera berada di x >= 3100 dan memori streaming menipis. |
| `game/World.cpp` | `GetRepeatSector` membungkus dengan `& 15` seperti game (dengan `%`, indeks negatif keluar tabel). |
| `net/scriptrpc.cpp` | `ScrCreateObject`: ID model negatif dipetakan ke model map; objek yang sudah dipasang client, dan tanda tanya penggantinya, tidak dibuat lagi. |
| `net/netgame.cpp` | `vc::OnNetworkReset()` saat koneksi direset. |
| `game/textdraw.cpp` | Sprite `mdl<id>:<tekstur>`. **Perbaikan** `SetText`: slot tekstur tidak lagi ditandai kosong saat teksnya diganti (slot itu lalu direbut textdraw lain). **Perbaikan** slot -1 (ke-200 slot sprite sudah terpakai): tabel tekstur tidak lagi ditulis dan dibaca di indeks -1. |
| `modloader/ModLoader.*` | `ml::LookUpModelsByName()`: game mencari model arsip lewat nama, bukan lewat `MINFO.BIN`. |
| `modloader/TxdConvert.*` | Tekstur berlabel DXT3/DXT5 dengan data DXT1 dibaca sebagai DXT1; laporan menghitung tekstur yang gagal karena memori. |
| `Android.mk`, `CMakeLists.txt` | `vicecity/tests/` tidak ikut dikompilasi. |

## Isi folder ini

| File | Isi |
|---|---|
| `ViceCity.cpp/.h` | Penghubung ke game: pendaftaran model, hook, pembuatan objek. |
| `VcMapData.gen.cpp`, `VcMapData.h` | Tabel map hasil `tools/vc_convert.py` (model, TXD, penempatan, material). |
| `VcMap` | Tabel digabung dengan file yang benar-benar ada di HP; pencarian ID dan penempatan. |
| `VcArchive` | Arsip IMG di memori tempat game membaca `.dff`. |
| `VcCollision` | Membaca dan memeriksa collision SA-MP di dalam `.dff` (COL3). |
| `VcTextures` | Cache TXD: dibaca saat model pertama memakainya, dilepas setelah tidak dipakai. |
| `VcStreamer` | Memilih objek mana yang ada: jarak, prioritas, batas jumlah, anggaran waktu per frame. |
| `VcConfig` | `vice_city.ini`. |
| `server/` | Filterscript 0.3.7 (`.pwn` dan `.amx`, dikompilasi dengan pawncc 3.10.10). |
| `tests/` | Tes host (tidak ikut build Android). |
| `tools/vc_convert.py` | Pembuat `VcMapData.gen.cpp` dari `vice_city.pwn` dan file model. |

Kalau repo map diperbarui:

    python3 tools/vc_convert.py --repo <folder samp-vice-city> --out VcMapData.gen.cpp

## Yang sudah dan belum diuji

Sudah:

- 73 tes host (`sh tests/run.sh`, g++ dengan ASan + UBSan, juga clang++) untuk semua bagian yang tidak
  bergantung pada game. Dengan `VC_REPO=<folder repo>` tes juga membaca seluruh file asli (commit `aee29a4`):
  2433 `.dff` (1843 collision dimuat, 589 kosong, 1 tanpa collision, tidak ada yang ditolak), 604 `.txd`
  (8524 tekstur, tidak ada yang dilewati), `minimap.txd` (25 tekstur), dan arsip 2411 model dibaca balik
  utuh.
- 121 tes modloader tetap lolos.
- Semua file baru dan file yang diubah lolos pemeriksaan sintaks dan tipe untuk `aarch64-linux-android21`
  (clang 18 dengan header bionic dan libc++), terhadap header client yang sebenarnya.
- Fakta tentang game (fungsi, susunan data, instruksi yang ditambal) diperiksa dengan membongkar
  `libGTASA.so` 2.10 arm64.
- Kedua filterscript dikompilasi tanpa peringatan terhadap include SA-MP 0.3.7.

Belum:

- **Belum dijalankan di HP atau emulator**, dan belum di-build dengan NDK (NDK tidak bisa diunduh di tempat
  kode ini dibuat). Kalau build NDK menolak sesuatu atau game berperilaku lain dari yang dibaca dari
  `libGTASA.so`, `vice_city_status.txt` dan logcat (tag `AXL`, kata kunci `ViceCity`) adalah tempat pertama
  untuk dilihat.
- Filterscript belum dijalankan di server sungguhan. Cara open.mp mengirim objek ber-model kustom ke client
  0.3.7 dibaca dari source open.mp (`Shared/NetCode/object.hpp`), tidak dicoba dengan server open.mp.
