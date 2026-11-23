#pragma once
/*
 * Kontrak antarmuka user-mode <-> driver EXTER VIRTUAL MICROPHONE (dipakai kedua sisi).
 *
 * Alur attach:
 *   1. Engine membuat section anonim (CreateFileMapping), memetakannya, ExterRing_Init.
 *   2. Engine membuka \\.\ExterVirtualMic dan mengirim IOCTL_EXTER_ATTACH_RING berisi HANDLE section
 *      (nilai handle milik proses pemanggil; IOCTL METHOD_BUFFERED berjalan di konteks pemanggil).
 *   3. Driver: ObReferenceObjectByHandle(*MmSectionObjectType) -> MmMapViewInSystemSpace ->
 *      ExterRing_Validate -> ExterConsumer_Init. Setelah itu driver membaca ring dari konteks mana pun.
 *   4. Detach (IOCTL_EXTER_DETACH_RING) atau handle device ditutup (termasuk proses crash) -> driver
 *      melepas mapping & section dan mulai mengeluarkan senyap.
 */
#include "ExterTransport.h"

#if defined(EXTER_KERNEL_MODE)
  /* ntddk.h sudah menyediakan CTL_CODE */
#elif defined(_WIN32)
  #include <windows.h>
  #include <winioctl.h>
#endif
#ifndef CTL_CODE   /* hanya untuk tes non-Windows */
  #define CTL_CODE(t,f,m,a) (((t)<<16)|((a)<<14)|((f)<<2)|(m))
  #define METHOD_BUFFERED 0
  #define FILE_WRITE_ACCESS 2
  #define FILE_READ_ACCESS 1
#endif

#define EXTER_DEVICE_TYPE            0x8E7Eu              /* rentang vendor: >= 0x8000 */
#define EXTER_DEVICE_SYMLINK_NT      L"\\DosDevices\\ExterVirtualMic"
#define EXTER_DEVICE_NAME_NT         L"\\Device\\ExterVirtualMic"
#define EXTER_DEVICE_WIN32_PATH      L"\\\\.\\ExterVirtualMic"
#define EXTER_ENDPOINT_FRIENDLY_NAME L"EXTER VIRTUAL MICROPHONE"

#define IOCTL_EXTER_ATTACH_RING  CTL_CODE(EXTER_DEVICE_TYPE, 0x800, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_EXTER_DETACH_RING  CTL_CODE(EXTER_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)
#define IOCTL_EXTER_QUERY_STATUS CTL_CODE(EXTER_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS)

#pragma pack(push, 8)
typedef struct _EXTER_ATTACH_RING_IN {
    ex_u32 abiVersion;      /* EXTER_ABI_VERSION */
    ex_u32 ringBytes;       /* ExterRing_TotalBytes(...) */
    ex_u64 sectionHandle;   /* HANDLE section di proses pemanggil */
} EXTER_ATTACH_RING_IN;

typedef struct _EXTER_STATUS_OUT {
    ex_u32 abiVersion;
    ex_u32 attached;        /* ring terpasang */
    ex_u32 streamRunning;   /* ada aplikasi Windows yang sedang menangkap dari EXTER VIRTUAL MICROPHONE */
    ex_u32 reserved;
    ex_u64 framesDelivered; /* total frame yang diserahkan ke Windows Audio Engine */
} EXTER_STATUS_OUT;
#pragma pack(pop)
