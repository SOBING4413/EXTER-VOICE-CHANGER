# Exter.Installer

| File | Fungsi |
|------|--------|
| `Build-Release.ps1` | Build Release x64 seluruh solution aplikasi, hasil di `dist\ExterVoiceChanger\`. `-WithDriver` ikut membangun driver (butuh WDK) dan menyusun `installer\` (skrip + paket driver). |
| `Install-ExterVirtualMic.ps1` | Memasang/mencopot driver EXTER VIRTUAL MICROPHONE (device root `Root\ExterVirtualAudio`) dan memverifikasinya. Meminta UAC sendiri. Opsi: `-DriverDir`, `-Uninstall`, `-TrustSigner` (hanya untuk sertifikat test buatan sendiri), `-NoPause`. Aplikasi memanggilnya dari tombol **INSTALL V-MIC**. |

Exter tidak memakai VB-CABLE, VB-Audio, VoiceMeeter, atau virtual cable pihak ketiga mana pun. Detail driver dan penandatanganan: `..\Exter.Driver\README.md`.
