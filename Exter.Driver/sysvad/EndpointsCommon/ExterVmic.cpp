/*++

Exter Virtual Audio Device - modul "Virtual Mic" (implementasi)

--*/
#include <ntddk.h>
#include <portcls.h>
#include <wdmsec.h>
#include "ExterVmic.h"

#define EXTER_VMIC_POOLTAG      'tVxE'
#define EXTER_MAX_RING_BYTES    (8u * 1024u * 1024u)
#define EXTER_MIN_RING_FRAMES   4096u

// Class GUID khusus control device (dipakai IoCreateDeviceSecure untuk override keamanan via registry).
static const GUID kExterCtlClassGuid =
    { 0x6a1f3d52, 0x8c4b, 0x4e07, { 0x9d, 0x2a, 0x5b, 0xc1, 0x3e, 0x70, 0xa4, 0x18 } };

typedef struct _EXTER_VMIC
{
    PDEVICE_OBJECT      CtlDevice;
    BOOLEAN             SymlinkCreated;

    FAST_MUTEX          Lock;               // serialisasi attach/detach (PASSIVE_LEVEL)
    EX_RUNDOWN_REF      Rundown;            // melindungi Consumer/View dari pembaca
    BOOLEAN             Attached;
    PFILE_OBJECT        OwnerFile;          // file object pemilik (attach otomatis lepas saat cleanup-nya)
    PVOID               Section;
    PVOID               View;
    SIZE_T              ViewSize;
    PMDL                Mdl;
    EXTER_CONSUMER      Consumer;           // hanya disentuh pembaca (di dalam rundown) dan attach/detach

    PVOID volatile      StreamOwner;        // token stream yang sedang menjadi consumer
    volatile LONG       StreamRunning;
    volatile LONG64     FramesDelivered;
} EXTER_VMIC;

static EXTER_VMIC g_Vmic;

//=============================================================================
// Jalur audio (non-paged): dipanggil dari stream WaveRT di <= DISPATCH_LEVEL.
//=============================================================================
#pragma code_seg()

ULONG ExterVmic_FillCapture(PVOID Token, PVOID Dst, ULONG Bytes)
{
    ULONG frames = Bytes / (ULONG)sizeof(INT16);
    ULONG got    = 0;

    if (Token != NULL && Token == g_Vmic.StreamOwner && ExAcquireRundownProtection(&g_Vmic.Rundown))
    {
        got = ExterConsumer_Read(&g_Vmic.Consumer, (ex_i16*)Dst, frames);
        ExReleaseRundownProtection(&g_Vmic.Rundown);
        if (got != 0)
        {
            InterlockedAdd64(&g_Vmic.FramesDelivered, (LONG64)got);
        }
    }
    else
    {
        RtlZeroMemory(Dst, (SIZE_T)frames * sizeof(INT16));
    }

    // Sisa byte ganjil (tidak terjadi untuk format 16-bit, tetapi jangan biarkan sampah).
    if ((Bytes % sizeof(INT16)) != 0)
    {
        ((PUCHAR)Dst)[Bytes - 1] = 0;
    }
    return got;
}

VOID ExterVmic_StreamRun(PVOID Token)
{
    PVOID prev = InterlockedCompareExchangePointer(&g_Vmic.StreamOwner, Token, NULL);
    if (prev != NULL && prev != Token)
    {
        return;                                 // stream lain sudah menjadi consumer -> yang ini mendapat senyap
    }

    if (ExAcquireRundownProtection(&g_Vmic.Rundown))
    {
        ExterConsumer_SetOpen(&g_Vmic.Consumer, 1);   // mulai dari posisi tulis terkini
        ExReleaseRundownProtection(&g_Vmic.Rundown);
    }
    InterlockedExchange(&g_Vmic.StreamRunning, 1);
}

VOID ExterVmic_StreamStop(PVOID Token)
{
    if (g_Vmic.StreamOwner != Token)
    {
        return;
    }

    if (ExAcquireRundownProtection(&g_Vmic.Rundown))
    {
        ExterConsumer_SetOpen(&g_Vmic.Consumer, 0);
        ExReleaseRundownProtection(&g_Vmic.Rundown);
    }
    InterlockedExchange(&g_Vmic.StreamRunning, 0);
    InterlockedCompareExchangePointer(&g_Vmic.StreamOwner, NULL, Token);
}

//=============================================================================
// Attach / detach (PASSIVE_LEVEL)
//=============================================================================
#pragma code_seg("PAGE")

VOID ExterVmic_Detach(VOID)
{
    PAGED_CODE();

    ExAcquireFastMutex(&g_Vmic.Lock);

    if (g_Vmic.Attached)
    {
        g_Vmic.Attached = FALSE;

        // Tunggu pembaca yang sedang berjalan; setelah ini ExAcquireRundownProtection gagal -> senyap.
        ExWaitForRundownProtectionRelease(&g_Vmic.Rundown);
        ExRundownCompleted(&g_Vmic.Rundown);

        // Beri tahu producer (engine) bahwa tidak ada consumer lagi. Halaman masih terkunci & ter-map.
        EX_STORE32_REL(&((EXTER_RING_HEADER*)g_Vmic.View)->consumerState, EXTER_CONSUMER_CLOSED);

        MmUnlockPages(g_Vmic.Mdl);
        IoFreeMdl(g_Vmic.Mdl);
        MmUnmapViewInSystemSpace(g_Vmic.View);
        ObDereferenceObject(g_Vmic.Section);

        g_Vmic.Mdl       = NULL;
        g_Vmic.View      = NULL;
        g_Vmic.ViewSize  = 0;
        g_Vmic.Section   = NULL;
        g_Vmic.OwnerFile = NULL;
        RtlZeroMemory(&g_Vmic.Consumer, sizeof(g_Vmic.Consumer));
    }

    ExReleaseFastMutex(&g_Vmic.Lock);
}

static NTSTATUS ExterVmic_Attach(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Sp)
{
    PAGED_CODE();

    NTSTATUS                status;
    EXTER_ATTACH_RING_IN    in;
    PVOID                   section  = NULL;
    PVOID                   view     = NULL;
    SIZE_T                  viewSize = 0;
    PMDL                    mdl      = NULL;
    BOOLEAN                 locked   = FALSE;
    BOOLEAN                 valid    = FALSE;

    // Hanya dari user-mode, proses 64-bit (HANDLE 64-bit & layout struct terjamin).
    if (Irp->RequestorMode != UserMode || IoIs32bitProcess(Irp))
    {
        return STATUS_ACCESS_DENIED;
    }
    if (Sp->Parameters.DeviceIoControl.InputBufferLength != sizeof(EXTER_ATTACH_RING_IN))
    {
        return STATUS_INVALID_BUFFER_SIZE;
    }

    // METHOD_BUFFERED: SystemBuffer ada di memori kernel; salin agar nilai tidak berubah setelah divalidasi.
    in = *(EXTER_ATTACH_RING_IN*)Irp->AssociatedIrp.SystemBuffer;

    if (in.abiVersion != EXTER_ABI_VERSION)
    {
        return STATUS_REVISION_MISMATCH;        // -> ERROR_REVISION_MISMATCH (HRESULT 0x8007051A di C#)
    }
    if (in.ringBytes < ExterRing_TotalBytes(EXTER_MIN_RING_FRAMES, EXTER_CHANNELS) || in.ringBytes > EXTER_MAX_RING_BYTES)
    {
        return STATUS_INVALID_PARAMETER;
    }

    // MmSectionObjectType diekspor ntoskrnl tetapi hanya dideklarasikan di ntifs.h (mendeklarasikannya sendiri
    // berisiko C4273). Ambil alamat variabelnya lewat export table.
    UNICODE_STRING sectionTypeName = RTL_CONSTANT_STRING(L"MmSectionObjectType");
    POBJECT_TYPE*  sectionType     = (POBJECT_TYPE*)MmGetSystemRoutineAddress(&sectionTypeName);
    if (sectionType == NULL || *sectionType == NULL)
    {
        return STATUS_NOT_SUPPORTED;
    }

    ExAcquireFastMutex(&g_Vmic.Lock);

    if (g_Vmic.Attached)
    {
        status = STATUS_DEVICE_BUSY;            // satu pemilik saja
        goto Exit;
    }

    // Handle ini hanya valid di konteks proses pemanggil, yang memang berlaku untuk IOCTL.
    status = ObReferenceObjectByHandle((HANDLE)(ULONG_PTR)in.sectionHandle,
                                       SECTION_MAP_READ | SECTION_MAP_WRITE,
                                       *sectionType,
                                       UserMode,
                                       &section,
                                       NULL);
    if (!NT_SUCCESS(status))
    {
        section = NULL;
        goto Exit;
    }

    // Petakan HANYA ringBytes (bukan seluruh section; ukuran section dikontrol user-mode), dibulatkan ke halaman
    // karena view sistem selalu kelipatan halaman. Section yang lebih kecil dari ini ditolak Mm (STATUS_INVALID_VIEW_SIZE).
    viewSize = ROUND_TO_PAGES((SIZE_T)in.ringBytes);
    status = MmMapViewInSystemSpace(section, &view, &viewSize);
    if (!NT_SUCCESS(status))
    {
        view = NULL;
        goto Exit;
    }
    if (viewSize < in.ringBytes)
    {
        status = STATUS_INVALID_PARAMETER;
        goto Exit;
    }

    // Kunci halaman: section berbasis pagefile bisa di-page-out, dan pembacaan di DISPATCH_LEVEL
    // pada halaman non-resident = bugcheck.
    mdl = IoAllocateMdl(view, (ULONG)viewSize, FALSE, FALSE, NULL);
    if (mdl == NULL)
    {
        status = STATUS_INSUFFICIENT_RESOURCES;
        goto Exit;
    }
    __try
    {
        MmProbeAndLockPages(mdl, KernelMode, IoWriteAccess);
        locked = TRUE;
        valid  = (BOOLEAN)ExterRing_Validate(view, viewSize);
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        status = GetExceptionCode();
        valid  = FALSE;
    }
    if (!locked)
    {
        if (NT_SUCCESS(status)) { status = STATUS_INSUFFICIENT_RESOURCES; }
        goto Exit;
    }
    if (!valid)
    {
        status = STATUS_INVALID_PARAMETER;      // header tidak lolos validasi (magic/abi/format/ukuran)
        goto Exit;
    }

    // Consumer: hanya nilai privat tervalidasi yang disalin dari header; tidak ada pembaca saat ini
    // (rundown berstatus "completed" sampai ExReInitializeRundownProtection di bawah).
    ExterConsumer_Init(&g_Vmic.Consumer,
                       (EXTER_RING_HEADER*)view,
                       EXTER_DEFAULT_TARGET_FRAMES,
                       EXTER_DEFAULT_HIGHWATER_FRAMES,
                       EXTER_DEFAULT_STALE_FRAMES);

    g_Vmic.Section   = section;
    g_Vmic.View      = view;
    g_Vmic.ViewSize  = viewSize;
    g_Vmic.Mdl       = mdl;
    g_Vmic.OwnerFile = Sp->FileObject;
    section = NULL; view = NULL; mdl = NULL;    // kepemilikan pindah ke g_Vmic
    locked  = FALSE;

    // Bila aplikasi Windows sudah menangkap dari mic ini, langsung buka consumer.
    if (g_Vmic.StreamOwner != NULL && g_Vmic.StreamRunning != 0)
    {
        ExterConsumer_SetOpen(&g_Vmic.Consumer, 1);
    }

    g_Vmic.Attached = TRUE;
    ExReInitializeRundownProtection(&g_Vmic.Rundown);   // izinkan pembaca masuk
    status = STATUS_SUCCESS;

Exit:
    if (mdl != NULL)
    {
        if (locked) { MmUnlockPages(mdl); }
        IoFreeMdl(mdl);
    }
    if (view != NULL)    { MmUnmapViewInSystemSpace(view); }
    if (section != NULL) { ObDereferenceObject(section); }

    ExReleaseFastMutex(&g_Vmic.Lock);
    return status;
}

static NTSTATUS ExterVmic_Ioctl(_In_ PIRP Irp, _In_ PIO_STACK_LOCATION Sp, _Out_ PULONG_PTR Info)
{
    PAGED_CODE();

    *Info = 0;

    switch (Sp->Parameters.DeviceIoControl.IoControlCode)
    {
    case IOCTL_EXTER_ATTACH_RING:
        return ExterVmic_Attach(Irp, Sp);

    case IOCTL_EXTER_DETACH_RING:
        if (g_Vmic.Attached && g_Vmic.OwnerFile != Sp->FileObject)
        {
            return STATUS_ACCESS_DENIED;        // hanya pemilik yang boleh melepas
        }
        ExterVmic_Detach();
        return STATUS_SUCCESS;

    case IOCTL_EXTER_QUERY_STATUS:
    {
        if (Sp->Parameters.DeviceIoControl.OutputBufferLength < sizeof(EXTER_STATUS_OUT))
        {
            return STATUS_BUFFER_TOO_SMALL;
        }
        EXTER_STATUS_OUT* out = (EXTER_STATUS_OUT*)Irp->AssociatedIrp.SystemBuffer;
        RtlZeroMemory(out, sizeof(*out));
        out->abiVersion      = EXTER_ABI_VERSION;
        out->attached        = g_Vmic.Attached ? 1u : 0u;
        out->streamRunning   = (g_Vmic.StreamRunning != 0) ? 1u : 0u;
        out->framesDelivered = (ex_u64)g_Vmic.FramesDelivered;
        *Info = sizeof(EXTER_STATUS_OUT);
        return STATUS_SUCCESS;
    }

    default:
        return STATUS_INVALID_DEVICE_REQUEST;
    }
}

//=============================================================================
// Dispatch. PortCls mengisi SEMUA MajorFunction dengan PcDispatchIrp; kita membungkus empat di antaranya.
// IRP untuk device audio -> teruskan ke PcDispatchIrp (persis seperti PnpHandler SysVAD); IRP untuk control
// device -> ditangani di sini. Sengaja di segmen non-paged: dispatch PortCls bisa menerima IRP di IRQL tinggi.
//=============================================================================
#pragma code_seg()

static DRIVER_DISPATCH ExterVmic_Dispatch;

static NTSTATUS ExterVmic_Dispatch(_In_ PDEVICE_OBJECT DeviceObject, _Inout_ PIRP Irp)
{
    if (DeviceObject != g_Vmic.CtlDevice)
    {
        return PcDispatchIrp(DeviceObject, Irp);
    }

    PIO_STACK_LOCATION sp   = IoGetCurrentIrpStackLocation(Irp);
    NTSTATUS           st   = STATUS_SUCCESS;
    ULONG_PTR          info = 0;

    switch (sp->MajorFunction)
    {
    case IRP_MJ_CREATE:
    case IRP_MJ_CLOSE:
        break;

    case IRP_MJ_CLEANUP:
        // Handle pemilik ditutup (termasuk proses engine crash) -> lepas ring.
        if (g_Vmic.Attached && g_Vmic.OwnerFile == sp->FileObject)
        {
            ExterVmic_Detach();
        }
        break;

    case IRP_MJ_DEVICE_CONTROL:
        st = ExterVmic_Ioctl(Irp, sp, &info);
        break;

    default:
        st = STATUS_INVALID_DEVICE_REQUEST;
        break;
    }

    Irp->IoStatus.Status      = st;
    Irp->IoStatus.Information = info;
    IoCompleteRequest(Irp, IO_NO_INCREMENT);
    return st;
}

//=============================================================================
// Init / shutdown
//=============================================================================
#pragma code_seg("PAGE")

NTSTATUS ExterVmic_Initialize(PDRIVER_OBJECT DriverObject)
{
    PAGED_CODE();

    UNICODE_STRING devName  = RTL_CONSTANT_STRING(EXTER_DEVICE_NAME_NT);
    UNICODE_STRING linkName = RTL_CONSTANT_STRING(EXTER_DEVICE_SYMLINK_NT);
    // SYSTEM & Administrators: akses penuh; pengguna terautentikasi: hanya read/write (engine berjalan tanpa admin).
    UNICODE_STRING sddl     = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;AU)");
    NTSTATUS       status;

    RtlZeroMemory(&g_Vmic, sizeof(g_Vmic));
    ExInitializeFastMutex(&g_Vmic.Lock);
    ExInitializeRundownProtection(&g_Vmic.Rundown);
    ExWaitForRundownProtectionRelease(&g_Vmic.Rundown);     // keadaan awal: "tidak ada ring" -> pembaca selalu gagal acquire
    ExRundownCompleted(&g_Vmic.Rundown);

    status = IoCreateDeviceSecure(DriverObject,
                                  0,
                                  &devName,
                                  (DEVICE_TYPE)EXTER_DEVICE_TYPE,
                                  FILE_DEVICE_SECURE_OPEN,
                                  FALSE,
                                  &sddl,
                                  &kExterCtlClassGuid,
                                  &g_Vmic.CtlDevice);
    if (!NT_SUCCESS(status))
    {
        g_Vmic.CtlDevice = NULL;
        return status;
    }

    status = IoCreateSymbolicLink(&linkName, &devName);
    if (!NT_SUCCESS(status))
    {
        IoDeleteDevice(g_Vmic.CtlDevice);
        g_Vmic.CtlDevice = NULL;
        return status;
    }
    g_Vmic.SymlinkCreated = TRUE;

    g_Vmic.CtlDevice->Flags &= ~DO_DEVICE_INITIALIZING;

    DriverObject->MajorFunction[IRP_MJ_CREATE]         = ExterVmic_Dispatch;
    DriverObject->MajorFunction[IRP_MJ_CLOSE]          = ExterVmic_Dispatch;
    DriverObject->MajorFunction[IRP_MJ_CLEANUP]        = ExterVmic_Dispatch;
    DriverObject->MajorFunction[IRP_MJ_DEVICE_CONTROL] = ExterVmic_Dispatch;

    return STATUS_SUCCESS;
}

VOID ExterVmic_Shutdown(VOID)
{
    PAGED_CODE();

    ExterVmic_Detach();

    if (g_Vmic.SymlinkCreated)
    {
        UNICODE_STRING linkName = RTL_CONSTANT_STRING(EXTER_DEVICE_SYMLINK_NT);
        IoDeleteSymbolicLink(&linkName);
        g_Vmic.SymlinkCreated = FALSE;
    }
    if (g_Vmic.CtlDevice != NULL)
    {
        IoDeleteDevice(g_Vmic.CtlDevice);
        g_Vmic.CtlDevice = NULL;
    }
}

#pragma code_seg()
