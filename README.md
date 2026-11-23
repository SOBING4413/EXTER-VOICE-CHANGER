# EXTER VOICE CHANGER

Voice changer **walkie-talkie / radio militer** real-time untuk Windows 10/11 (x64).
Satu fungsi: mengubah suara mic jadi suara tactical radio, lalu menyediakannya sebagai **microphone virtual** untuk Discord, game, Steam Voice, dll.

```
Mic fisik ──WASAPI──► Exter.AudioEngine (C++ DSP radio) ──shared-memory ring──► driver EXTER VIRTUAL MICROPHONE
                                                                                   │
Discord / Game / Voice chat  ◄── memilih "EXTER VIRTUAL MICROPHONE" sebagai Input ◄┘
```

## Status jujur (baca dulu)

Isi paket ini: engine + aplikasi + transport (Tahap 1, 2, 6) **dan sekarang driver + installer** (Tahap 3 dan 5).
**Tidak satu pun bagian Windows-nya pernah dikompilasi/dijalankan di Windows** oleh pembuatnya (lingkungan pembuatan tidak punya Windows/MSVC/WDK/.NET).
Yang diuji adalah logika yang bisa diuji di Linux; sisanya ditulis mengikuti pola resmi dan diperiksa silang secara otomatis.

| Bagian | Status |
|---|---|
| DSP radio (band-pass orde-4 + presence EQ, noise gate hysteresis, kompresor soft-knee auto-release, saturasi asimetris ADAA, static, click/roger beep, limiter) | **Diuji** (g++ + ASan/UBSan, `Exter.Tests/DspTests.cpp`): output selalu finite & di bawah limiter untuk parameter ekstrem, input NaN/Inf, parameter NaN, ganti sample rate, transisi PTT berulang. |
| Transport shared-memory (ring SPSC lock-free, resampler 44,1→48 kHz, consumer anti torn-read) | **Diuji** (`TransportTests.cpp`, termasuk stress thread dan lintas proses). |
| Logika modul kernel driver (`ExterVmic.cpp`): validasi IOCTL, satu pemilik, satu consumer, detach saat pembaca berjalan, cleanup saat proses crash, tanpa kebocoran | **Diuji** terhadap shim API kernel (`DriverModuleTests.cpp`, ASan/UBSan, tes thread). |
| Konsistensi C++ ↔ driver ↔ INF ↔ installer ↔ C# (struct, nama endpoint, HRESULT, IOCTL, batas parameter, format audio) | **Diperiksa otomatis**: `python3 Exter.Tests/check-interface.py` (60 pemeriksaan). |
| WASAPI capture/render, device enumeration, global hotkey, GUI WPF | Ditulis lengkap, **belum dikompilasi**. Kompilasi pertama mungkin memunculkan error kecil; laporkan teks error-nya. |
| **Driver EXTER VIRTUAL MICROPHONE** (`Exter.Driver/`, turunan Microsoft SysVAD) | Ditulis lengkap, **belum dikompilasi dengan WDK dan belum dijalankan**. Lihat `Exter.Driver/README.md`. |
| **Penandatanganan driver** | **Belum ada.** Untuk uji di PC sendiri cukup test-signing; untuk PC lain butuh tanda tangan Microsoft (lihat `Exter.Driver/README.md`). |

Tanpa driver terpasang, output default (EXTER VIRTUAL MICROPHONE) menampilkan *DRIVER NOT INSTALLED*; Anda tetap bisa memilih perangkat playback WASAPI lain di **OUTPUT VIRTUAL MICROPHONE**.
Exter tidak pernah memilih speaker secara otomatis (mencegah feedback).

## Persyaratan

- Windows 10 / 11 x64
- Visual Studio 2022 (17.8+) dengan workload: **.NET desktop development** dan **Desktop development with C++** (termasuk Windows 10/11 SDK)
- .NET 8 SDK (ikut terpasang dengan VS 2022 17.8+)
- Untuk membangun driver: **Windows Driver Kit (WDK)** yang cocok dengan Windows SDK-nya. Aplikasi sendiri tidak butuh WDK.
- Untuk memakai virtual mic: driver terpasang (test-signing di PC sendiri, atau paket bertanda tangan Microsoft) - lihat `Exter.Driver/README.md`

## Build

1. Buka `Exter.sln` di Visual Studio 2022.
2. Pilih konfigurasi **Debug** atau **Release** dan platform **x64** (dropdown di toolbar).
3. **Build → Build Solution.** Urutan otomatis: `Exter.AudioEngine` (DLL native) → `Exter.Core` → `Exter.VirtualAudio` → `Exter.App`. Post-build menyalin `Exter.AudioEngine.dll` ke folder output aplikasi.
4. Set `Exter.App` sebagai startup project, tekan **F5**.

Build command line + paket siap pakai: `powershell -File Exter.Installer\Build-Release.ps1` → `dist\ExterVoiceChanger\ExterVoiceChanger.exe` (tambahkan `-WithDriver` untuk ikut membangun dan menyertakan driver; butuh WDK).

### Driver virtual mic

```
powershell -File Exter.Driver\Build-Driver.ps1                      # build driver (WDK) -> dist\driver
powershell -File Exter.Installer\Install-ExterVirtualMic.ps1 -TrustSigner   # PC sendiri dengan test-signing aktif
```

Rincian (test-signing, penandatanganan untuk PC lain, verifikasi, batasan): `Exter.Driver/README.md`.

> Jika muncul peringatan *"Exter.AudioEngine.dll tidak ditemukan"*, build ulang seluruh solution dengan platform **x64** (bukan Any CPU). Kalau aplikasi menampilkan *OUT OF DATE* / *NOT x64*, DLL native lama atau salah arsitektur — rebuild seluruh solution.

## Tes (tanpa Windows)

```
sh Exter.Tests/run-tests.sh            # transport + DSP + logika modul kernel driver (butuh g++)
python3 Exter.Tests/check-interface.py # konsistensi C++ / header driver / C#
```

## Cara pakai

| Kontrol | Fungsi |
|---|---|
| **MAIN POWER** | ON = radio bekerja sesuai mode. OFF (STANDBY) = efek mati: suara asli diteruskan, atau di-mute bila *MUTE WHEN RADIO OFF* dicentang. |
| **TRANSMIT MODE** | *Push To Talk*: efek aktif selama tombol ditahan. *Always ON*: efek selalu aktif. *Always OFF*: efek mati. |
| **PTT KEYBIND** | Hotkey **global** (hook keyboard low-level, bekerja walau Exter tidak di foreground, termasuk key-up; tombol tidak ditelan, jadi tetap terketik di aplikasi lain — pilih tombol yang tidak dipakai untuk mengetik). **REBIND** lalu tekan tombol baru (Esc = batal); **RESET** kembali ke `V`. Bila key-up terlewat (mis. Win+L saat tombol ditahan), PTT dilepas otomatis. |
| **VOICE VOLUME** | Slider 0–200 %, tombol +/− (5 %), nilai numerik. Limiter menjaga puncak di −1 dBFS, jadi tidak clipping. |
| **AUDIO DEVICES** | Mic input, output virtual mic, sample rate (44.1/48 kHz), audio buffer (10–120 ms), input volume. Perangkat dideteksi ulang tiap 1,5 dtk. |
| **RADIO TUNING** | Noise gate + threshold, static, drive distorsi, click/roger beep. |

Catatan perilaku PTT: pada mode Push To Talk, saat tombol **tidak** ditekan suara Anda lewat apa adanya (atau senyap jika opsi mute dicentang) — jadi orang lain hanya mendengar efek radio saat Anda menahan tombol.

Setelan disimpan otomatis (±1 dtk setelah perubahan) di `%AppData%\Exter\settings.xml`. Error tak terduga dicatat di `%AppData%\Exter\error.log`. Hanya satu instance Exter yang boleh berjalan.

## Arsitektur

```
Exter.sln
├── Exter.AudioEngine/   C++ native DLL (x64, /MT di Release, /fp:precise)
│   ├── AudioEngine.h/.cpp   API C (ex_*), thread capture & render WASAPI event-driven (shared mode, float32 mono,
│   │                        AUTOCONVERTPCM), MMCSS "Pro Audio", jitter buffer + koreksi drift, deteksi device putus,
│   │                        parameter lewat seqlock + clamp di sisi native
│   ├── RadioEffect.h/.cpp   DSP radio (tanpa alokasi/lock di jalur audio)
│   ├── ExterDriverLink.*    section shared-memory + IOCTL attach ke driver
│   ├── RingBuffer.h         SPSC lock-free (jalur WASAPI render)
│   └── DeviceEnum.cpp       enumerasi endpoint (IMMDeviceEnumerator)
├── Exter.Shared/        header bersama user-mode / driver / tes (transport ring, writer, kontrak IOCTL)
├── Exter.Core/          C#: NativeMethods (P/Invoke), EngineController, DeviceManager, HotkeyManager, Settings (XML)
├── Exter.VirtualAudio/  C#: VirtualAudioManager (deteksi driver, pesan error, peluncur installer)
├── Exter.App/           C# WPF/XAML: tema tactical, MVVM, waveform & meter custom (diskalakan turun di layar kecil)
├── Exter.Config/        settings.xml default (salinan pengguna: %AppData%\Exter\settings.xml)
├── Exter.Driver/        driver kernel EXTER VIRTUAL MICROPHONE (turunan Microsoft SysVAD, MIT): Exter.Driver.sln,
│                        sysvad\ExterAudio (project + INF), sysvad\EndpointsCommon\ExterVmic.* (modul kernel), Build-Driver.ps1
├── Exter.Installer/     Build-Release.ps1, Install-ExterVirtualMic.ps1 (pasang/copot driver + verifikasi)
└── Exter.Tests/         tes transport, DSP, logika modul kernel (shim), pemeriksa antarmuka
```

Semua audio berjalan di thread native terpisah (UI thread tidak pernah memblokir; start engine dijalankan di background task, dan permintaan restart yang bertumpuk digabung). Bila mic/output dicabut, engine berhenti bersih dan monitor mencoba menyalakan ulang otomatis saat perangkat kembali (jeda 5 dtk antar percobaan bila daftar perangkat tidak berubah). Memilih *EXTER VIRTUAL MICROPHONE* sebagai mic ditolak (loop feedback).

## Latency & kualitas

- WASAPI shared mode: Windows menambah ±10 ms per sisi. Dengan buffer default 40 ms, perkiraan total ±60–80 ms (estimasi, belum diukur di perangkat nyata). Indikator `LATENCY ~` di GUI menampilkan estimasi live.
- Suara patah-patah/crackle → naikkan AUDIO BUFFER (60–80 ms). Ingin lebih responsif → turunkan ke 20 ms.
- Sample rate: pakai 48000 Hz (sama dengan Discord dan format transport driver; 44,1 kHz di-resample linear).

## Troubleshooting

| Gejala | Solusi |
|---|---|
| *DRIVER NOT INSTALLED* / VIRTUAL MIC: NOT READY | Driver belum terpasang: bangun & pasang (`Exter.Driver/README.md`). Sementara, pilih perangkat playback lain di OUTPUT. |
| Installer: *Windows tidak akan memuat driver bertanda tangan test* | Aktifkan test-signing (`bcdedit /set testsigning on`, Secure Boot mati, restart), lalu pasang dengan `-TrustSigner`. |
| Driver terpasang tetapi endpoint tidak muncul / kode 52 / kode 39 | Lihat `C:\Windows\INF\setupapi.dev.log` dan Device Manager; kirim isinya agar INF/driver bisa diperbaiki. |
| Discord tidak mendengar apa pun | Input Device Discord harus **EXTER VIRTUAL MICROPHONE**. Cek level meter Exter bergerak. Pastikan mic Windows tidak di-mute / izin mikrofon aktif (*Settings → Privacy → Microphone*). Matikan Krisp/Echo Cancellation/AGC di Discord agar efek radio tidak dirusak. |
| Efek tidak terdengar | Mode PTT: tahan tombol. Atau pilih *Always ON*. Cek MAIN POWER = ACTIVE. |
| Suara pecah | Turunkan OUTPUT/INPUT VOLUME atau DISTORTION DRIVE; naikkan buffer. |
| PTT tidak jalan di game tertentu | Game yang berjalan sebagai Administrator tidak mengirim input ke proses non-admin; jalankan Exter sebagai Administrator juga. |
| Status ENGINE ERROR 0x8889xxxx | Perangkat dipakai mode eksklusif aplikasi lain, atau format tidak didukung. Tutup aplikasi tsb / ganti device. Engine mencoba ulang otomatis. |
| Jendela terlalu besar di laptop kecil | Konsol otomatis diskalakan turun mengikuti area kerja layar. |
