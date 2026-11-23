#pragma once
// Sisi user-mode dari transport: membuat section ring, membukanya ke driver EXTER VIRTUAL MICROPHONE.
#include "ExterTransport.h"
#include <windows.h>

class ExterDriverLink {
public:
    ~ExterDriverLink() { Close(); }
    // S_OK bila ring terpasang di driver. Gagal: HRESULT_FROM_WIN32(ERROR_FILE_NOT_FOUND) = driver belum terpasang,
    // E_ACCESSDENIED = tidak punya akses ke device, HRESULT_FROM_WIN32(ERROR_REVISION_MISMATCH) = ABI beda.
    HRESULT Open();
    void Close();
    bool Attached() const { return attached_; }
    EXTER_RING_HEADER* Header() const { return hdr_; }
private:
    HANDLE section_ = nullptr;
    HANDLE device_ = INVALID_HANDLE_VALUE;
    EXTER_RING_HEADER* hdr_ = nullptr;
    bool attached_ = false;
};
