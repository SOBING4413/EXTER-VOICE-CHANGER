#pragma once
// Exter Audio Engine - public C API (dipakai C# via P/Invoke)
#include <stdint.h>
#include <wchar.h>

#if defined(_WIN32)
  #ifdef EXTER_EXPORTS
    #define EXAPI extern "C" __declspec(dllexport)
  #else
    #define EXAPI extern "C" __declspec(dllimport)
  #endif
#else
  #define EXAPI extern "C"
#endif

// renderId khusus: pakai EXTER VIRTUAL MICROPHONE (driver Exter) lewat shared-memory transport,
// TANPA perangkat render WASAPI dan tanpa virtual cable pihak ketiga.
#define EX_VIRTUAL_MIC_ID L"exter:virtual-mic"

struct ExDevice {
    wchar_t id[256];
    wchar_t name[256];
    int     isDefault;
};

// Urutan field HARUS sama dengan Native.ExParams di C#.
struct ExParams {
    float inputGain;        // multiplier linear (1.0 = 100%)
    float outputGain;       // multiplier linear (1.0 = 100%)
    float highPassHz;       // 100..1000
    float lowPassHz;        // 1500..8000
    int   gateEnabled;
    float gateThresholdDb;  // -80..-10
    float compThresholdDb;  // -50..-5
    float compRatio;        // 1..20
    float drive;            // 1..8  (saturasi)
    float staticLevel;      // 0..1  (noise statis radio)
    int   beepEnabled;      // click/beep saat PTT on/off
    int   bypassMode;       // saat efek OFF: 0 = suara asli, 1 = mute
};

// Urutan field HARUS sama dengan Native.ExStatus di C#.
struct ExStatus {
    int   running;
    int   micConnected;
    int   outputReady;
    int   effectActive;
    float inLevel;     // peak mic sejak pemanggilan sebelumnya (0..1)
    float outLevel;    // peak output sejak pemanggilan sebelumnya (0..1)
    int   lastError;   // HRESULT terakhir (0 = OK)
    int   underruns;
    float latencyMs;   // perkiraan latency di dalam engine (ring + buffer render) / transport
    // ---- ditambahkan untuk EXTER VIRTUAL MICROPHONE (selalu di akhir struct) ----
    int   transportMode;      // 1 = output lewat driver Exter, 0 = WASAPI render biasa
    int   driverAttached;     // ring terpasang di driver
    int   driverStreaming;    // ada aplikasi (Discord, dll.) yang sedang menangkap dari mic virtual
    int   driverUnderruns;    // underrun di sisi driver (consumer kehabisan data)
};

// flow: 0 = render (output), 1 = capture (input). Mengembalikan jumlah device.
EXAPI int  ex_enumerate_devices(int flow, ExDevice* out, int maxCount);
// captureId / renderId kosong = default device. Mengembalikan HRESULT (0 = sukses).
EXAPI int  ex_start(const wchar_t* captureId, const wchar_t* renderId, int sampleRate, int bufferMs);
EXAPI void ex_stop(void);
EXAPI void ex_set_params(const ExParams* p);
EXAPI void ex_set_active(int active);
EXAPI void ex_get_status(ExStatus* s);
