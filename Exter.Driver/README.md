# Exter Virtual Audio Device (EXTER VIRTUAL MICROPHONE)

Driver audio kernel-mode yang membuat **satu mikrofon virtual**. Suara yang keluar dari mikrofon itu adalah hasil
`Exter.AudioEngine` (efek radio), dikirim lewat ring shared-memory. Discord/game/apa pun memilihnya seperti mic biasa.
Tanpa VB-CABLE, VoiceMeeter, atau driver pihak ketiga lain.

```
Exter.AudioEngine ──► section shared-memory (ring PCM16 mono 48 kHz) ──IOCTL attach──► ExterVirtualAudio.sys
                                                                                         │ WaveRT stream capture
                                       Windows Audio Engine ◄── endpoint "EXTER VIRTUAL MICROPHONE" ◄┘
```

Dibuat dari sampel **Microsoft SysVAD** (lisensi MIT, lihat `LICENSE-Microsoft-Windows-driver-samples.txt`).
SysVAD sudah menyediakan seluruh mesin PortCls/WaveRT, jadi yang diubah hanya yang perlu.

## Status (baca dulu)

| Bagian | Status |
|---|---|
| Logika modul kernel `ExterVmic.cpp` (validasi IOCTL, satu pemilik, satu consumer, detach saat pembaca berjalan, cleanup saat proses crash, tanpa kebocoran view/MDL/section) | **Diuji** di Linux terhadap shim API kernel (`Exter.Tests/DriverModuleTests.cpp`, ASan/UBSan, 1625 cek, termasuk tes thread; pengujian dengan sabotase membuktikan tes menangkap use-after-free bila detach tidak menunggu pembaca). |
| Konsistensi INF ↔ kode ↔ project ↔ installer ↔ aplikasi | **Diperiksa otomatis** (`python3 Exter.Tests/check-interface.py`). |
| Kompilasi dengan WDK (MSVC kernel, `/W4 /WX`) | **Belum pernah dikompilasi.** Tidak ada Windows/WDK di lingkungan pembuatan. Kode mengikuti pola SysVAD, tetapi build pertama bisa memunculkan error/warning kecil. Kirim teks error-nya. |
| INF, pemasangan, tampilnya endpoint di Windows, suara sampai ke Discord | **Belum pernah dijalankan.** Semua verifikasi di bawah ini dikerjakan di PC Anda. |
| Tanda tangan untuk PC lain | **Belum ada.** Lihat "Penandatanganan". |

## Yang diubah dari SysVAD

| Perubahan | File |
|---|---|
| Hanya **satu endpoint** (capture, "mic in"). Dibuang: speaker, headphone, HDMI, SPDIF, mic array, Bluetooth HFP, USB headset, A2DP, APO, keyword detector, project Package. | `ExterAudio/minipairs.h`, `adapter.cpp`, `ExterAudio.vcxproj`, `ExterVirtualAudio.inx` |
| Hanya **satu format**: 48000 Hz, mono, PCM 16-bit = format ring (Windows Audio Engine yang mengonversi untuk aplikasi). Semua mode sinyal (RAW/DEFAULT/SPEECH/COMMUNICATIONS) memakai format ini. | `ExterAudio/micinwavtable.h` |
| Sumber data capture: **ring Exter**, bukan generator nada. `WriteBytes()` memanggil `ExterVmic_FillCapture`. `KSSTATE_RUN` menjadi consumer; `PAUSE/STOP/destruktor` melepasnya. | `EndpointsCommon/minwavertstream.cpp` |
| Modul baru: control device `\\.\ExterVirtualMic`, IOCTL attach/detach/status, jalur baca lock-free (rundown protection), satu consumer stream. | `EndpointsCommon/ExterVmic.h/.cpp` |
| `DriverEntry` membuat control device (setelah `PcInitializeAdapterDriver`, membungkus dispatch CREATE/CLOSE/CLEANUP/DEVICE_CONTROL dan meneruskan IRP device audio ke `PcDispatchIrp`); `DriverUnload` dan PnP stop/remove melepas ring. Bypass Bluetooth/USB/A2DP default mati. | `adapter.cpp` |
| Target **Desktop** (bukan Universal): ApiValidator Universal menolak beberapa API yang dipakai `ExterVmic`. Build mandiri (INF/CAT dibuat project ini sendiri). Link `wdmsec.lib`. | `ExterAudio.vcxproj`, `EndpointsCommon.vcxproj` |
| INF baru: hardware ID `Root\ExterVirtualAudio`, nama endpoint "EXTER VIRTUAL MICROPHONE", layanan & file `ExterVirtualAudio.sys`. | `ExterAudio/ExterVirtualAudio.inx` |

## Build

Butuh Visual Studio 2022 dengan workload *Desktop development with C++*, **Windows SDK dan WDK versi yang sama**
(https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk), dan komponen *MSVC Spectre-mitigated libs* bila diminta.

```
powershell -File Exter.Driver\Build-Driver.ps1 -Configuration Release     # atau buka Exter.Driver.sln di VS
```

Hasil: `dist\driver\` berisi `ExterVirtualAudio.sys/.inf/.cat`. Hanya **x64** yang dirancang dan diuji logikanya
(konfigurasi ARM64 ikut dari SysVAD tetapi tidak dipakai; engine juga hanya x64).

## Penandatanganan (tak terhindarkan untuk driver kernel)

Windows 10/11 hanya memuat driver kernel bertanda tangan. Pilihan:

1. **Uji di PC Anda sendiri (test-signing).** Matikan Secure Boot di UEFI, jalankan `bcdedit /set testsigning on` sebagai
   Administrator, restart (muncul watermark "Test Mode"). Visual Studio menandatangani paket dengan sertifikat test (WDKTestCert).
   Lalu: `powershell -File Exter.Installer\Install-ExterVirtualMic.ps1 -TrustSigner`.
2. **Dipakai di PC orang lain tanpa test-signing.** Paket harus ditandatangani Microsoft: akun Windows Hardware Dev Center
   (Partner Center) dengan sertifikat code-signing EV, lalu *attestation signing* (atau WHQL). Tidak ada jalan pintas legal.

## Pasang / copot

```
powershell -File Exter.Installer\Install-ExterVirtualMic.ps1 [-DriverDir <folder>] [-TrustSigner]
powershell -File Exter.Installer\Install-ExterVirtualMic.ps1 -Uninstall
```

Skrip membuat device root (setara `devcon install`), memasang paket, lalu memverifikasi tiga hal: perangkat berstatus OK,
endpoint "EXTER VIRTUAL MICROPHONE" muncul, dan `\\.\ExterVirtualMic` bisa dibuka. Tombol **INSTALL V-MIC** di aplikasi memanggil skrip yang sama
(`installer\` di samping `ExterVoiceChanger.exe`; `Build-Release.ps1 -WithDriver` menyusunnya).

## Verifikasi manual di Windows (urutan saran)

1. Device Manager → *Sound, video and game controllers* → **Exter Virtual Audio Device** tanpa tanda seru.
2. Sound control panel → tab *Recording* → **EXTER VIRTUAL MICROPHONE (Exter Virtual Audio Device)**.
3. Jalankan Exter (output = EXTER VIRTUAL MICROPHONE, status VIRTUAL MIC READY), bicara: pada jendela *Properties → Listen / Levels* mic Exter, meter bergerak.
4. Discord → Input Device = EXTER VIRTUAL MICROPHONE; matikan Noise Suppression/Echo Cancellation/AGC.
5. Log pemasangan bila gagal: `C:\Windows\INF\setupapi.dev.log`.

## Model keamanan sisi kernel

- Control device: SDDL `SYSTEM` dan `Administrators` penuh, pengguna terautentikasi hanya read/write, `FILE_DEVICE_SECURE_OPEN`.
- Attach hanya dari user-mode 64-bit; ABI diperiksa; `ringBytes` dibatasi (64 KB … 8 MB); handle section diperiksa tipenya (`ObReferenceObjectByHandle` dengan `MmSectionObjectType`); yang dipetakan hanya `ringBytes` (bukan seluruh section, ukurannya dikontrol user-mode);
  halaman **dikunci** (pembacaan terjadi di DISPATCH_LEVEL); header divalidasi, lalu hanya nilai privat tervalidasi yang dipakai (batas tidak pernah dibaca ulang dari memori user-mode).
- Satu pemilik (file object); handle ditutup/proses crash → ring dilepas otomatis (`IRP_MJ_CLEANUP`). Satu stream capture saja yang menjadi consumer.
- Pembaca di jalur audio tidak memakai lock: `EX_RUNDOWN_REF`; detach menunggu pembaca selesai sebelum membuka kunci/unmap/dereference.
- Producer mati atau tidak ada ring → mic mengeluarkan **senyap** (bukan sampah, bukan hang).

## Batasan yang diketahui

- Satu format (48 kHz mono). Aplikasi yang meminta format lain dikonversi Windows Audio Engine; aplikasi eksklusif-mode yang menolak format ini akan gagal membuka mic.
- Tanpa APO, tanpa kontrol volume hardware, tanpa deteksi kata kunci.
- Dukungan multi-instans/multi-producer tidak ada (satu engine per mesin).
- Penghapusan driver saat Discord/game sedang membuka mic bisa meminta restart.
