#include "AudioEngine.h"
#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <wrl/client.h>
#include <string>

using Microsoft::WRL::ComPtr;

EXAPI int ex_enumerate_devices(int flow, ExDevice* out, int maxCount)
{
    if (!out || maxCount <= 0) return 0;
    // UI thread WPF = STA; RPC_E_CHANGED_MODE tidak masalah, COM tetap bisa dipakai.
    HRESULT hrInit = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    bool uninit = SUCCEEDED(hrInit);
    int count = 0;
    {
        ComPtr<IMMDeviceEnumerator> en;
        if (SUCCEEDED(CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en)))) {
            EDataFlow f = (flow == 0) ? eRender : eCapture;

            std::wstring defId;
            {
                ComPtr<IMMDevice> d;
                if (SUCCEEDED(en->GetDefaultAudioEndpoint(f, eConsole, &d))) {
                    LPWSTR id = nullptr;
                    if (SUCCEEDED(d->GetId(&id)) && id) { defId = id; CoTaskMemFree(id); }
                }
            }

            ComPtr<IMMDeviceCollection> col;
            UINT n = 0;
            if (SUCCEEDED(en->EnumAudioEndpoints(f, DEVICE_STATE_ACTIVE, &col)) && SUCCEEDED(col->GetCount(&n))) {
                for (UINT i = 0; i < n && count < maxCount; ++i) {
                    ComPtr<IMMDevice> d;
                    if (FAILED(col->Item(i, &d))) continue;
                    LPWSTR id = nullptr;
                    if (FAILED(d->GetId(&id)) || !id) continue;

                    ExDevice& e = out[count];
                    ZeroMemory(&e, sizeof(e));
                    wcsncpy_s(e.id, 256, id, _TRUNCATE);
                    e.isDefault = (defId == id) ? 1 : 0;
                    CoTaskMemFree(id);

                    ComPtr<IPropertyStore> ps;
                    if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                        PROPVARIANT v; PropVariantInit(&v);
                        if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
                            wcsncpy_s(e.name, 256, v.pwszVal, _TRUNCATE);
                        PropVariantClear(&v);
                    }
                    if (e.name[0] == L'\0') wcsncpy_s(e.name, 256, L"(unknown device)", _TRUNCATE);
                    ++count;
                }
            }
        }
    }
    if (uninit) CoUninitialize();
    return count;
}
