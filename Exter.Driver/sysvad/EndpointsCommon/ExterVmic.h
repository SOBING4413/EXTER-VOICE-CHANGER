/*++

Exter Virtual Audio Device - modul "Virtual Mic"

    Menghubungkan endpoint capture WaveRT (hasil patch SysVAD) dengan ring shared-memory yang diisi
    Exter.AudioEngine di user-mode. Kontrak: Exter.Shared/ExterDriverInterface.h + ExterTransport.h.

    Thread-safety:
      - Jalur baca (ExterVmic_FillCapture) berjalan di <= DISPATCH_LEVEL tanpa lock: dijaga EX_RUNDOWN_REF.
      - Attach/detach berjalan di PASSIVE_LEVEL, diserialkan FAST_MUTEX.
      - Hanya SATU stream capture yang menjadi consumer; stream lain mendapat senyap.

--*/
#ifndef _EXTER_VMIC_H_
#define _EXTER_VMIC_H_

#ifndef EXTER_KERNEL_MODE
#define EXTER_KERNEL_MODE
#endif

#include <portcls.h>
#include "ExterDriverInterface.h"

// Dipanggil dari DriverEntry SETELAH PcInitializeAdapterDriver dan sebelum hook PnP/Unload.
// Membuat control device \Device\ExterVirtualMic + symlink dan membungkus dispatch CREATE/CLOSE/CLEANUP/DEVICE_CONTROL.
NTSTATUS ExterVmic_Initialize(_In_ PDRIVER_OBJECT DriverObject);

// Dipanggil dari DriverUnload SEBELUM unload PortCls.
VOID ExterVmic_Shutdown(VOID);

// Lepas ring (aman dipanggil kapan saja di PASSIVE_LEVEL, termasuk bila belum attach). Dipakai juga oleh PnP stop/remove.
VOID ExterVmic_Detach(VOID);

// Siklus hidup stream capture Windows. Token = alamat stream (identitas pemilik).
VOID ExterVmic_StreamRun(_In_ PVOID Token);   // KSSTATE_RUN
VOID ExterVmic_StreamStop(_In_ PVOID Token);  // KSSTATE_PAUSE / STOP / destruktor

// Isi `Bytes` byte buffer capture (PCM16 mono 48 kHz) dari ring; sisa selalu senyap. IRQL <= DISPATCH_LEVEL.
// Mengembalikan jumlah frame berisi audio nyata.
ULONG ExterVmic_FillCapture(_In_ PVOID Token, _Out_writes_bytes_(Bytes) PVOID Dst, _In_ ULONG Bytes);

#endif // _EXTER_VMIC_H_
