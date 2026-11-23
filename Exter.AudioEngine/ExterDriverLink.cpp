#include "ExterDriverLink.h"
#include "ExterDriverInterface.h"

HRESULT ExterDriverLink::Open()
{
    Close();
    const DWORD bytes = ExterRing_TotalBytes(EXTER_RING_FRAMES, EXTER_CHANNELS);
    // Ukuran SECTION dibulatkan ke halaman: driver memetakan view sistem yang selalu kelipatan halaman, dan view tidak
    // boleh melebihi ukuran section (STATUS_INVALID_VIEW_SIZE). ringBytes yang dikirim ke driver tetap `bytes`.
    const DWORD sectionBytes = (bytes + 4095u) & ~4095u;

    // Section anonim (halaman nol). Tidak bernama => tidak butuh hak Global\ dan tidak bisa dibuka proses lain.
    section_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sectionBytes, nullptr);
    if (!section_) { HRESULT hr = HRESULT_FROM_WIN32(GetLastError()); Close(); return hr; }
    hdr_ = static_cast<EXTER_RING_HEADER*>(MapViewOfFile(section_, FILE_MAP_ALL_ACCESS, 0, 0, sectionBytes));
    if (!hdr_) { HRESULT hr = HRESULT_FROM_WIN32(GetLastError()); Close(); return hr; }
    ExterRing_Init(hdr_, EXTER_RING_FRAMES);

    device_ = CreateFileW(EXTER_DEVICE_WIN32_PATH, GENERIC_READ | GENERIC_WRITE,
                          FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (device_ == INVALID_HANDLE_VALUE) { HRESULT hr = HRESULT_FROM_WIN32(GetLastError()); Close(); return hr; }

    EXTER_ATTACH_RING_IN in{};
    in.abiVersion = EXTER_ABI_VERSION;
    in.ringBytes = bytes;
    in.sectionHandle = (ex_u64)(ULONG_PTR)section_;
    DWORD ret = 0;
    if (!DeviceIoControl(device_, IOCTL_EXTER_ATTACH_RING, &in, sizeof(in), nullptr, 0, &ret, nullptr)) {
        HRESULT hr = HRESULT_FROM_WIN32(GetLastError()); Close(); return hr;
    }
    attached_ = true;
    return S_OK;
}

void ExterDriverLink::Close()
{
    if (hdr_) ExterRing_SetProducerState(hdr_, EXTER_PRODUCER_STOPPED);
    if (device_ != INVALID_HANDLE_VALUE) {
        if (attached_) { DWORD ret = 0; DeviceIoControl(device_, IOCTL_EXTER_DETACH_RING, nullptr, 0, nullptr, 0, &ret, nullptr); }
        CloseHandle(device_);                       // menutup handle juga memicu detach di driver (cleanup IRP)
        device_ = INVALID_HANDLE_VALUE;
    }
    attached_ = false;
    if (hdr_) { UnmapViewOfFile(hdr_); hdr_ = nullptr; }
    if (section_) { CloseHandle(section_); section_ = nullptr; }
}
