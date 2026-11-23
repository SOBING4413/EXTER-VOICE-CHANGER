// Exter Audio Engine - WASAPI capture (mic) -> RadioEffect DSP -> ring buffer -> WASAPI render (virtual mic)
#include "AudioEngine.h"
#include "RadioEffect.h"
#include "RingBuffer.h"
#include "ExterDriverLink.h"
#include "ExterTransportWriter.h"

#include <windows.h>
#include <objbase.h>
#include <propidl.h>
#include <mmdeviceapi.h>
#include <functiondiscoverykeys_devpkey.h>
#include <audioclient.h>
#include <avrt.h>
#include <wrl/client.h>
#include <xmmintrin.h>
#include <pmmintrin.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "avrt.lib")

using Microsoft::WRL::ComPtr;

namespace {

constexpr size_t kRingCap = 1u << 17;   // 131072 sample (~2.7 s @ 48 kHz)

ExParams DefaultParams() {
    ExParams p{};
    p.inputGain = 1.f; p.outputGain = 1.f;
    p.highPassHz = 300.f; p.lowPassHz = 3400.f;
    p.gateEnabled = 1; p.gateThresholdDb = -45.f;
    p.compThresholdDb = -22.f; p.compRatio = 4.f;
    p.drive = 1.8f; p.staticLevel = 0.25f;
    p.beepEnabled = 1; p.bypassMode = 0;
    return p;
}

struct Engine {
    std::mutex mtx;                       // melindungi start/stop
    std::atomic<bool> running{false};
    std::thread capThread, renThread;

    RingBuffer ring{kRingCap};
    RadioEffect fx;

    // Parameter: seqlock. Penulis (UI, diserialkan pmtx) menaikkan pseq jadi ganjil, menyalin, lalu genap lagi;
    // pembaca (audio thread) mengulang bila pseq berubah/ganjil. Menggantikan double-buffer lama yang bisa
    // robek (torn) bila UI memanggil ex_set_params dua kali berturut-turut saat audio thread masih membaca.
    std::mutex pmtx;
    std::atomic<uint32_t> pseq{0};
    ExParams pcur = DefaultParams();

    std::atomic<int> active{0}, micConnected{0}, outputReady{0}, lastError{0}, underruns{0};
    std::atomic<float> latencyMs{0.f};

    int sampleRate = 48000;
    int bufferMs = 40;

    // EXTER VIRTUAL MICROPHONE (transport ke driver). Dipakai bila renderId == EX_VIRTUAL_MIC_ID.
    std::atomic<int> transportMode{0};
    ExterDriverLink link;
    ExterTransportWriter tw;

    HANDLE capInit = nullptr, renInit = nullptr;
    std::atomic<int> capHr{0}, renHr{0};
};

// Salinan konsisten parameter untuk audio thread. Gagal 4x (penulis sedang menulis) -> pakai salinan terakhir.
void ReadParams(Engine& g, ExParams& out) {
    for (int attempt = 0; attempt < 4; ++attempt) {
        uint32_t s1 = g.pseq.load(std::memory_order_acquire);
        if (s1 & 1u) continue;
        ExParams tmp = g.pcur;
        std::atomic_thread_fence(std::memory_order_acquire);
        if (g.pseq.load(std::memory_order_relaxed) == s1) { out = tmp; return; }
    }
}

// Sengaja tidak pernah di-delete: menghindari std::terminate bila DLL di-unload saat thread masih hidup.
Engine& G() { static Engine* e = new Engine(); return *e; }

HRESULT OpenDevice(EDataFlow flow, const std::wstring& id, ComPtr<IMMDevice>& dev) {
    ComPtr<IMMDeviceEnumerator> en;
    HRESULT hr = CoCreateInstance(__uuidof(MMDeviceEnumerator), nullptr, CLSCTX_ALL, IID_PPV_ARGS(&en));
    if (FAILED(hr)) return hr;
    if (!id.empty()) return en->GetDevice(id.c_str(), &dev);
    return en->GetDefaultAudioEndpoint(flow, eConsole, &dev);
}

// Mic fisik tidak boleh berupa EXTER VIRTUAL MICROPHONE sendiri: itu feedback loop.
constexpr HRESULT kErrLoopback = (HRESULT)0x8EE70001;
bool IsExterVirtualMic(IMMDevice* dev) {
    ComPtr<IPropertyStore> ps;
    if (FAILED(dev->OpenPropertyStore(STGM_READ, &ps))) return false;
    PROPVARIANT v; PropVariantInit(&v);
    bool hit = false;
    if (SUCCEEDED(ps->GetValue(PKEY_Device_FriendlyName, &v)) && v.vt == VT_LPWSTR && v.pwszVal)
        hit = (wcsstr(v.pwszVal, L"EXTER VIRTUAL MICROPHONE") != nullptr);
    PropVariantClear(&v);
    return hit;
}

// Shared mode, event-driven, float32 mono. AUTOCONVERTPCM membuat Windows mengonversi
// sample rate / jumlah channel dari format mixer perangkat ke format yang kita minta.
HRESULT InitClient(IMMDevice* dev, int sampleRate, ComPtr<IAudioClient>& client, HANDLE ev) {
    HRESULT hr = dev->Activate(__uuidof(IAudioClient), CLSCTX_ALL, nullptr, reinterpret_cast<void**>(client.GetAddressOf()));
    if (FAILED(hr)) return hr;

    WAVEFORMATEX wf{};
    wf.wFormatTag = WAVE_FORMAT_IEEE_FLOAT;
    wf.nChannels = 1;
    wf.nSamplesPerSec = (DWORD)sampleRate;
    wf.wBitsPerSample = 32;
    wf.nBlockAlign = 4;
    wf.nAvgBytesPerSec = wf.nSamplesPerSec * wf.nBlockAlign;
    wf.cbSize = 0;

    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | AUDCLNT_STREAMFLAGS_AUTOCONVERTPCM | AUDCLNT_STREAMFLAGS_SRC_DEFAULT_QUALITY;
    hr = client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, 0, 0, &wf, nullptr);
    if (FAILED(hr)) return hr;
    return client->SetEventHandle(ev);
}

void CaptureThread(std::wstring id) {
    Engine& g = G();
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
    DWORD taskIdx = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioCaptureClient> cap;

    HRESULT hr = OpenDevice(eCapture, id, dev);
    if (SUCCEEDED(hr) && IsExterVirtualMic(dev.Get())) hr = kErrLoopback;
    if (SUCCEEDED(hr)) hr = InitClient(dev.Get(), g.sampleRate, client, ev);
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioCaptureClient), reinterpret_cast<void**>(cap.GetAddressOf()));
    if (SUCCEEDED(hr)) hr = client->Start();
    g.capHr = (int)hr;
    SetEvent(g.capInit);

    if (SUCCEEDED(hr)) {
        g.micConnected = 1;
        if (g.transportMode.load()) g.tw.Start();
        std::vector<float> tmp(8192);
        ExParams p = DefaultParams();

        while (g.running.load()) {
            WaitForSingleObject(ev, 200);
            if (!g.running.load()) break;

            UINT32 packet = 0;
            hr = cap->GetNextPacketSize(&packet);
            if (FAILED(hr)) { g.lastError = (int)hr; g.running = false; break; }   // mis. mic dicabut

            while (packet > 0) {
                BYTE* data = nullptr; UINT32 frames = 0; DWORD fl = 0;
                hr = cap->GetBuffer(&data, &frames, &fl, nullptr, nullptr);
                if (FAILED(hr)) { g.lastError = (int)hr; g.running = false; break; }

                UINT32 done = 0;
                while (done < frames) {
                    UINT32 c = (UINT32)std::min<size_t>(frames - done, tmp.size());
                    if ((fl & AUDCLNT_BUFFERFLAGS_SILENT) || !data)
                        std::fill(tmp.begin(), tmp.begin() + c, 0.f);
                    else
                        memcpy(tmp.data(), reinterpret_cast<const float*>(data) + done, c * sizeof(float));

                    ReadParams(g, p);
                    g.fx.Process(tmp.data(), (int)c, p, g.active.load() != 0, g.sampleRate);
                    if (g.transportMode.load(std::memory_order_relaxed)) g.tw.Write(tmp.data(), c);
                    else g.ring.Write(tmp.data(), c);
                    done += c;
                }
                cap->ReleaseBuffer(frames);

                hr = cap->GetNextPacketSize(&packet);
                if (FAILED(hr)) { g.lastError = (int)hr; g.running = false; break; }
            }
        }
        client->Stop();
    }

    if (g.transportMode.load()) g.tw.Stop();       // driver langsung mengeluarkan senyap
    g.micConnected = 0;
    if (ev) CloseHandle(ev);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(hrCom)) CoUninitialize();
}

void RenderThread(std::wstring id) {
    Engine& g = G();
    HRESULT hrCom = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    DWORD taskIdx = 0;
    HANDLE mmcss = AvSetMmThreadCharacteristicsW(L"Pro Audio", &taskIdx);

    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    ComPtr<IMMDevice> dev;
    ComPtr<IAudioClient> client;
    ComPtr<IAudioRenderClient> render;
    UINT32 bufFrames = 0;

    HRESULT hr = OpenDevice(eRender, id, dev);
    if (SUCCEEDED(hr)) hr = InitClient(dev.Get(), g.sampleRate, client, ev);
    if (SUCCEEDED(hr)) hr = client->GetBufferSize(&bufFrames);
    if (SUCCEEDED(hr)) hr = client->GetService(__uuidof(IAudioRenderClient), reinterpret_cast<void**>(render.GetAddressOf()));
    if (SUCCEEDED(hr)) {
        BYTE* d = nullptr;                       // prefill silence
        hr = render->GetBuffer(bufFrames, &d);
        if (SUCCEEDED(hr)) hr = render->ReleaseBuffer(bufFrames, AUDCLNT_BUFFERFLAGS_SILENT);
    }
    if (SUCCEEDED(hr)) hr = client->Start();
    g.renHr = (int)hr;
    SetEvent(g.renInit);

    if (SUCCEEDED(hr)) {
        g.outputReady = 1;
        const size_t target = (size_t)g.sampleRate * (size_t)g.bufferMs / 1000;   // jitter buffer (ms -> sample)
        bool primed = false;

        while (g.running.load()) {
            WaitForSingleObject(ev, 200);
            if (!g.running.load()) break;

            UINT32 pad = 0;
            hr = client->GetCurrentPadding(&pad);
            if (FAILED(hr)) { g.lastError = (int)hr; g.running = false; break; }
            UINT32 avail = bufFrames - pad;
            if (avail == 0) continue;

            BYTE* data = nullptr;
            hr = render->GetBuffer(avail, &data);
            if (FAILED(hr)) { g.lastError = (int)hr; g.running = false; break; }
            float* out = reinterpret_cast<float*>(data);

            size_t have = g.ring.Available();
            if (!primed && have >= target) primed = true;
            if (primed && have > target * 3 + avail) {            // clock drift -> buang yang tertua
                g.ring.Skip(have - target - avail);
                have = g.ring.Available();
            }

            if (primed) {
                size_t got = g.ring.Read(out, avail);
                if (got < avail) {                                // underrun -> isi nol & isi ulang jitter buffer
                    std::fill(out + got, out + avail, 0.f);
                    g.underruns.fetch_add(1);
                    primed = false;
                }
            } else {
                std::fill(out, out + avail, 0.f);
            }
            render->ReleaseBuffer(avail, 0);

            g.latencyMs = (float)(g.ring.Available() + pad) * 1000.f / (float)g.sampleRate;
        }
        client->Stop();
    }

    g.outputReady = 0;
    if (ev) CloseHandle(ev);
    if (mmcss) AvRevertMmThreadCharacteristics(mmcss);
    if (SUCCEEDED(hrCom)) CoUninitialize();
}

void StopLocked() {
    Engine& g = G();
    g.running = false;
    if (g.capThread.joinable()) g.capThread.join();
    if (g.renThread.joinable()) g.renThread.join();
    if (g.capInit) { CloseHandle(g.capInit); g.capInit = nullptr; }
    if (g.renInit) { CloseHandle(g.renInit); g.renInit = nullptr; }
    if (g.tw.Attached()) g.tw.Detach();
    g.link.Close();
    g.transportMode = 0;
    g.micConnected = 0;
    g.outputReady = 0;
}

} // namespace

EXAPI int ex_start(const wchar_t* captureId, const wchar_t* renderId, int sampleRate, int bufferMs)
{
    Engine& g = G();
    std::lock_guard<std::mutex> lock(g.mtx);
    StopLocked();

    g.sampleRate = (sampleRate == 44100) ? 44100 : 48000;
    g.bufferMs = std::min(200, std::max(10, bufferMs));
    g.ring.Clear();
    g.fx.Reset();
    g.lastError = 0;
    g.underruns = 0;
    g.latencyMs = 0.f;
    g.capHr = 0; g.renHr = 0;
    g.capInit = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g.renInit = CreateEventW(nullptr, TRUE, FALSE, nullptr);

    const bool virt = renderId && wcscmp(renderId, EX_VIRTUAL_MIC_ID) == 0;
    if (virt) {
        // Output = driver EXTER VIRTUAL MICROPHONE. Tidak ada thread render, tidak ada perangkat pihak ketiga.
        HRESULT lhr = g.link.Open();
        if (FAILED(lhr)) {
            CloseHandle(g.capInit); g.capInit = nullptr;
            CloseHandle(g.renInit); g.renInit = nullptr;
            g.lastError = (int)lhr;
            return (int)lhr;      // 0x80070002 = driver belum terpasang; 0x80070005 = akses ditolak
        }
        g.tw.Attach(g.link.Header(), g.sampleRate);
        g.transportMode = 1;
        g.outputReady = 1;
        g.renHr = 0;
        SetEvent(g.renInit);
    }

    g.running = true;
    try {
        g.capThread = std::thread(CaptureThread, std::wstring(captureId ? captureId : L""));
        if (!virt) g.renThread = std::thread(RenderThread, std::wstring(renderId ? renderId : L""));
    } catch (...) {                       // std::system_error (kehabisan resource) tidak boleh menembus batas extern "C"
        StopLocked();
        g.lastError = (int)E_OUTOFMEMORY;
        return (int)E_OUTOFMEMORY;
    }

    DWORD w1 = WaitForSingleObject(g.capInit, 5000);
    DWORD w2 = WaitForSingleObject(g.renInit, 5000);

    HRESULT hr = S_OK;
    if (w1 != WAIT_OBJECT_0 || w2 != WAIT_OBJECT_0) hr = E_FAIL;
    else if (FAILED((HRESULT)g.capHr.load())) hr = (HRESULT)g.capHr.load();
    else if (FAILED((HRESULT)g.renHr.load())) hr = (HRESULT)g.renHr.load();

    if (FAILED(hr)) {
        StopLocked();
        g.lastError = (int)hr;
        return (int)hr;
    }
    return 0;
}

EXAPI void ex_stop(void)
{
    Engine& g = G();
    std::lock_guard<std::mutex> lock(g.mtx);
    StopLocked();
}

static float ClampParam(float v, float lo, float hi, float dflt)
{
    if (v >= lo && v <= hi) return v;
    if (v < lo) return lo;
    if (v > hi) return hi;
    return dflt;                                            // semua perbandingan false = NaN
}

EXAPI void ex_set_params(const ExParams* p)
{
    if (!p) return;
    Engine& g = G();

    // Batas nilai ditegakkan DI SINI (bukan hanya di UI): settings.xml yang diedit/rusak tidak boleh
    // sampai ke DSP sebagai NaN atau nilai di luar rentang.
    ExParams c = *p;
    c.inputGain       = ClampParam(c.inputGain, 0.f, 4.f, 1.f);
    c.outputGain      = ClampParam(c.outputGain, 0.f, 4.f, 1.f);
    c.highPassHz      = ClampParam(c.highPassHz, 80.f, 1200.f, 300.f);
    c.lowPassHz       = ClampParam(c.lowPassHz, 1500.f, 8000.f, 3400.f);
    c.gateEnabled     = c.gateEnabled ? 1 : 0;
    c.gateThresholdDb = ClampParam(c.gateThresholdDb, -80.f, -10.f, -45.f);
    c.compThresholdDb = ClampParam(c.compThresholdDb, -50.f, -5.f, -22.f);
    c.compRatio       = ClampParam(c.compRatio, 1.f, 20.f, 4.f);
    c.drive           = ClampParam(c.drive, 1.f, 8.f, 1.8f);
    c.staticLevel     = ClampParam(c.staticLevel, 0.f, 1.f, 0.25f);
    c.beepEnabled     = c.beepEnabled ? 1 : 0;
    c.bypassMode      = c.bypassMode ? 1 : 0;

    std::lock_guard<std::mutex> lk(g.pmtx);
    uint32_t s = g.pseq.load(std::memory_order_relaxed);
    g.pseq.store(s + 1, std::memory_order_relaxed);          // ganjil = sedang menulis
    std::atomic_thread_fence(std::memory_order_release);
    g.pcur = c;
    g.pseq.store(s + 2, std::memory_order_release);          // genap = selesai
}

EXAPI void ex_set_active(int active) { G().active = active ? 1 : 0; }

EXAPI void ex_get_status(ExStatus* s)
{
    if (!s) return;
    Engine& g = G();
    s->running = g.running.load() ? 1 : 0;
    s->micConnected = g.micConnected.load();
    s->outputReady = g.outputReady.load();
    s->effectActive = g.active.load();
    s->inLevel = g.fx.inPeak.exchange(0.f);
    s->outLevel = g.fx.outPeak.exchange(0.f);
    s->lastError = g.lastError.load();
    s->underruns = g.underruns.load();
    s->latencyMs = g.latencyMs.load();
    s->transportMode = g.transportMode.load();
    s->driverAttached = 0;
    s->driverStreaming = 0; s->driverUnderruns = 0;
    // try_lock: jangan memblokir UI saat ex_start/ex_stop sedang memetakan/melepas ring.
    std::unique_lock<std::mutex> lk(g.mtx, std::try_to_lock);
    if (lk.owns_lock() && g.transportMode.load() && g.link.Attached()) {
        const EXTER_RING_HEADER* h = g.link.Header();
        s->driverAttached = 1;
        s->driverStreaming = (EX_LOAD32_ACQ(&h->consumerState) == EXTER_CONSUMER_OPEN) ? 1 : 0;
        s->driverUnderruns = (int)h->underruns;
        s->underruns = s->driverUnderruns;
        if (s->driverStreaming) {
            ex_u64 w = EX_LOAD64_ACQ(&h->writePos), r = EX_LOAD64_ACQ(&h->readPos);
            s->latencyMs = (w >= r) ? (float)(w - r) * 1000.f / (float)EXTER_SAMPLE_RATE : 0.f;
        }
    }
}
