#pragma once
/*
 * SHIM API kernel Windows, HANYA untuk menguji logika ExterVmic.cpp di Linux (g++ -fshort-wchar).
 * Bukan pengganti WDK: semantik disederhanakan, tetapi aturan yang penting untuk logika Exter dipertahankan:
 *   - rundown protection (acquire gagal setelah rundown; wait menunggu pemegang),
 *   - ObReferenceObjectByHandle memeriksa tipe objek,
 *   - view/MDL/section dihitung agar kebocoran terdeteksi.
 */
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdlib.h>
#include <sched.h>

#define _In_
#define _In_opt_
#define _Out_
#define _Inout_
#define _Out_writes_bytes_(n)
#define PAGED_CODE()
#define UNREFERENCED_PARAMETER(p) ((void)(p))
#define DECLSPEC_IMPORT

typedef int32_t  NTSTATUS;
typedef uint8_t  BOOLEAN, UCHAR, UINT8;
typedef uint16_t WCHAR, USHORT;
typedef WCHAR*   PWSTR;
typedef uint32_t ULONG, UINT32, DEVICE_TYPE;
typedef int32_t  LONG;
typedef int64_t  LONG64;
typedef uint64_t UINT64, ULONG_PTR;
typedef int16_t  INT16;
typedef size_t   SIZE_T;
typedef void*    PVOID;
typedef void*    HANDLE;
typedef UCHAR*   PUCHAR;
typedef ULONG_PTR* PULONG_PTR;
typedef const struct _GUID { uint32_t a; uint16_t b, c; uint8_t d[8]; } GUID;
#define TRUE 1
#define FALSE 0
#define VOID void

#define PAGE_SIZE 4096u
#define ROUND_TO_PAGES(x) (((x) + PAGE_SIZE - 1) & ~((SIZE_T)PAGE_SIZE - 1))
#define NT_SUCCESS(s) ((NTSTATUS)(s) >= 0)
#define STATUS_SUCCESS                 ((NTSTATUS)0x00000000)
#define STATUS_DEVICE_BUSY             ((NTSTATUS)0x80000011)
#define STATUS_INVALID_HANDLE          ((NTSTATUS)0xC0000008)
#define STATUS_INVALID_PARAMETER       ((NTSTATUS)0xC000000D)
#define STATUS_INVALID_DEVICE_REQUEST  ((NTSTATUS)0xC0000010)
#define STATUS_ACCESS_DENIED           ((NTSTATUS)0xC0000022)
#define STATUS_BUFFER_TOO_SMALL        ((NTSTATUS)0xC0000023)
#define STATUS_OBJECT_TYPE_MISMATCH    ((NTSTATUS)0xC0000024)
#define STATUS_INVALID_VIEW_SIZE       ((NTSTATUS)0xC000001F)
#define STATUS_REVISION_MISMATCH       ((NTSTATUS)0xC0000059)
#define STATUS_INSUFFICIENT_RESOURCES  ((NTSTATUS)0xC000009A)
#define STATUS_NOT_SUPPORTED           ((NTSTATUS)0xC00000BB)
#define STATUS_INVALID_BUFFER_SIZE     ((NTSTATUS)0xC0000206)

#define CTL_CODE(t,f,m,a) (((t)<<16)|((a)<<14)|((f)<<2)|(m))
#define METHOD_BUFFERED 0
#define FILE_READ_ACCESS 1
#define FILE_WRITE_ACCESS 2

#define IRP_MJ_CREATE 0x00
#define IRP_MJ_CLOSE 0x02
#define IRP_MJ_DEVICE_CONTROL 0x0e
#define IRP_MJ_CLEANUP 0x12
#define IRP_MJ_MAXIMUM_FUNCTION 0x1b
#define IO_NO_INCREMENT 0
#define DO_DEVICE_INITIALIZING 0x80
#define FILE_DEVICE_SECURE_OPEN 0x100
#define SECTION_MAP_WRITE 0x2
#define SECTION_MAP_READ 0x4
#define EXCEPTION_EXECUTE_HANDLER 1
#define GetExceptionCode() ((NTSTATUS)0xC0000005)
#define __try if (true)
#define __except(x) else if (false)

enum KPROCESSOR_MODE_ { KernelMode = 0, UserMode = 1 };
typedef int8_t KPROCESSOR_MODE;
enum LOCK_OPERATION { IoReadAccess, IoWriteAccess, IoModifyAccess };

typedef struct _UNICODE_STRING { USHORT Length, MaximumLength; PWSTR Buffer; } UNICODE_STRING, *PUNICODE_STRING;
#define RTL_CONSTANT_STRING(s) { (USHORT)(sizeof(s) - sizeof((s)[0])), (USHORT)sizeof(s), (PWSTR)(s) }

#define RtlZeroMemory(d,n) memset((d),0,(n))
#define RtlCopyMemory(d,s,n) memcpy((d),(s),(n))

typedef struct _FILE_OBJECT { int id; } FILE_OBJECT, *PFILE_OBJECT;
typedef struct _DEVICE_OBJECT { ULONG Flags; } DEVICE_OBJECT, *PDEVICE_OBJECT;
typedef struct _IO_STACK_LOCATION {
    UCHAR MajorFunction;
    struct { struct { ULONG OutputBufferLength, InputBufferLength, IoControlCode; } DeviceIoControl; } Parameters;
    PFILE_OBJECT FileObject;
} IO_STACK_LOCATION, *PIO_STACK_LOCATION;
typedef struct _IRP {
    KPROCESSOR_MODE RequestorMode;
    struct { PVOID SystemBuffer; } AssociatedIrp;
    struct { NTSTATUS Status; ULONG_PTR Information; } IoStatus;
    IO_STACK_LOCATION Stack;
    bool Is32;                       // khusus shim: simulasi proses 32-bit
    bool Completed;
} IRP, *PIRP;
typedef NTSTATUS DRIVER_DISPATCH(PDEVICE_OBJECT, PIRP);
typedef DRIVER_DISPATCH* PDRIVER_DISPATCH;
typedef struct _DRIVER_OBJECT { PDRIVER_DISPATCH MajorFunction[IRP_MJ_MAXIMUM_FUNCTION + 1]; } DRIVER_OBJECT, *PDRIVER_OBJECT;

inline PIO_STACK_LOCATION IoGetCurrentIrpStackLocation(PIRP i) { return &i->Stack; }
inline void IoCompleteRequest(PIRP i, int) { i->Completed = true; }
inline bool IoIs32bitProcess(PIRP i) { return i->Is32; }

/* ---- lock / rundown ---- */
typedef struct _FAST_MUTEX { volatile LONG v; } FAST_MUTEX;
inline void ExInitializeFastMutex(FAST_MUTEX* m) { m->v = 0; }
inline void ExAcquireFastMutex(FAST_MUTEX* m) { while (__sync_lock_test_and_set(&m->v, 1)) sched_yield(); }
inline void ExReleaseFastMutex(FAST_MUTEX* m) { __sync_lock_release(&m->v); }

#define EX_SHIM_ACTIVE 0x40000000
typedef struct _EX_RUNDOWN_REF { volatile LONG Count; } EX_RUNDOWN_REF;
inline void ExInitializeRundownProtection(EX_RUNDOWN_REF* r) { r->Count = 0; }
inline void ExReInitializeRundownProtection(EX_RUNDOWN_REF* r) { r->Count = 0; }
inline BOOLEAN ExAcquireRundownProtection(EX_RUNDOWN_REF* r) {
    for (;;) {
        LONG c = __atomic_load_n(&r->Count, __ATOMIC_ACQUIRE);
        if (c & EX_SHIM_ACTIVE) return FALSE;
        if (__atomic_compare_exchange_n(&r->Count, &c, c + 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) return TRUE;
    }
}
inline void ExReleaseRundownProtection(EX_RUNDOWN_REF* r) { __atomic_fetch_sub(&r->Count, 1, __ATOMIC_ACQ_REL); }
inline void ExWaitForRundownProtectionRelease(EX_RUNDOWN_REF* r) {
    __atomic_fetch_or(&r->Count, EX_SHIM_ACTIVE, __ATOMIC_ACQ_REL);
    while ((__atomic_load_n(&r->Count, __ATOMIC_ACQUIRE) & ~EX_SHIM_ACTIVE) != 0) sched_yield();
}
inline void ExRundownCompleted(EX_RUNDOWN_REF*) {}

inline PVOID InterlockedCompareExchangePointer(PVOID volatile* d, PVOID e, PVOID c) {
    __atomic_compare_exchange_n(d, &c, e, false, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST); return c;
}
inline LONG InterlockedExchange(volatile LONG* d, LONG v) { return __atomic_exchange_n(d, v, __ATOMIC_SEQ_CST); }
inline LONG64 InterlockedAdd64(volatile LONG64* d, LONG64 v) { return __atomic_add_fetch(d, v, __ATOMIC_SEQ_CST); }

/* ---- device / symlink ---- */
struct ShimStats {
    int devices = 0, symlinks = 0, views = 0, mdls = 0, lockedMdls = 0, sectionRefs = 0, pcDispatchCalls = 0;
};
inline ShimStats g_shim;

inline NTSTATUS IoCreateDeviceSecure(PDRIVER_OBJECT, ULONG, PUNICODE_STRING, DEVICE_TYPE type, ULONG chars, BOOLEAN,
                                     PUNICODE_STRING sddl, const GUID* cls, PDEVICE_OBJECT* out) {
    if (type < 0x8000 || !(chars & FILE_DEVICE_SECURE_OPEN) || !sddl || !sddl->Length || !cls) return STATUS_INVALID_PARAMETER;
    *out = (PDEVICE_OBJECT)calloc(1, sizeof(DEVICE_OBJECT)); (*out)->Flags = DO_DEVICE_INITIALIZING; ++g_shim.devices;
    return STATUS_SUCCESS;
}
inline void IoDeleteDevice(PDEVICE_OBJECT d) { free(d); --g_shim.devices; }
inline NTSTATUS IoCreateSymbolicLink(PUNICODE_STRING, PUNICODE_STRING) { ++g_shim.symlinks; return STATUS_SUCCESS; }
inline NTSTATUS IoDeleteSymbolicLink(PUNICODE_STRING) { --g_shim.symlinks; return STATUS_SUCCESS; }

/* ---- section / view / MDL ---- */
typedef struct _OBJECT_TYPE { int id; } OBJECT_TYPE, *POBJECT_TYPE;
inline OBJECT_TYPE g_shimSectionObjType{1};
inline OBJECT_TYPE g_shimOtherObjType{2};
inline POBJECT_TYPE g_shimSectionTypePtr = &g_shimSectionObjType;
struct ShimObject { OBJECT_TYPE* type; void* base; size_t size; };
inline ShimObject g_shimHandles[8];                       // handle = indeks + 1
inline HANDLE ShimRegister(OBJECT_TYPE* t, void* base, size_t size) {
    for (int i = 0; i < 8; ++i) if (!g_shimHandles[i].type) { g_shimHandles[i] = {t, base, size}; return (HANDLE)(uintptr_t)(i + 1); }
    return nullptr;
}
inline NTSTATUS ObReferenceObjectByHandle(HANDLE h, ULONG, POBJECT_TYPE type, KPROCESSOR_MODE, PVOID* obj, void*) {
    uintptr_t i = (uintptr_t)h;
    if (i < 1 || i > 8 || !g_shimHandles[i - 1].type) return STATUS_INVALID_HANDLE;
    if (g_shimHandles[i - 1].type != type) return STATUS_OBJECT_TYPE_MISMATCH;
    *obj = &g_shimHandles[i - 1]; ++g_shim.sectionRefs; return STATUS_SUCCESS;
}
inline void ObDereferenceObject(PVOID) { --g_shim.sectionRefs; }
inline NTSTATUS MmMapViewInSystemSpace(PVOID section, PVOID* base, SIZE_T* size) {
    ShimObject* o = (ShimObject*)section;
    if (*size == 0) *size = o->size;
    if (*size > o->size) return STATUS_INVALID_VIEW_SIZE;
    *base = o->base; ++g_shim.views; return STATUS_SUCCESS;
}
inline NTSTATUS MmUnmapViewInSystemSpace(PVOID) { --g_shim.views; return STATUS_SUCCESS; }
typedef struct _MDL { void* base; ULONG len; bool locked; } MDL, *PMDL;
inline PMDL IoAllocateMdl(PVOID va, ULONG len, BOOLEAN, BOOLEAN, PIRP) { PMDL m = (PMDL)calloc(1, sizeof(MDL)); m->base = va; m->len = len; ++g_shim.mdls; return m; }
inline void IoFreeMdl(PMDL m) { free(m); --g_shim.mdls; }
inline void MmProbeAndLockPages(PMDL m, KPROCESSOR_MODE, LOCK_OPERATION) { m->locked = true; ++g_shim.lockedMdls; }
inline void MmUnlockPages(PMDL m) { m->locked = false; --g_shim.lockedMdls; }
inline PVOID MmGetSystemRoutineAddress(PUNICODE_STRING n) {
    static const char want[] = "MmSectionObjectType";
    if (n->Length != 2 * (sizeof(want) - 1)) return nullptr;
    for (size_t i = 0; i + 1 < sizeof(want); ++i) if (n->Buffer[i] != (WCHAR)want[i]) return nullptr;
    return &g_shimSectionTypePtr;
}
