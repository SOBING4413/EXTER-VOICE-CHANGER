#pragma once
#include "AudioEngine.h"
#include <atomic>
#include <cstdint>

// DSP walkie-talkie / radio militer taktis. Mono float. Dipanggil dari satu thread (capture).
//
// Rantai sinyal (semua per-sample, TANPA lookahead -> nol latency algoritmik tambahan):
//   input -> band-pass (HP4 + LP4) -> detektor gate -> presence EQ -> noise gate (hysteresis)
//         -> kompresor soft-knee (auto-release) -> saturasi asimetris ADAA -> HP2/LP2 post
//         -> static radio -> beep/squelch tail -> limiter
class RadioEffect {
public:
    RadioEffect();
    void Reset();
    // Memproses in-place. active=false -> bypass (suara asli) atau mute sesuai p.bypassMode.
    void Process(float* buf, int n, const ExParams& p, bool active, int sampleRate);

    std::atomic<float> inPeak{0.f};   // peak mic (diambil & di-reset oleh UI polling)
    std::atomic<float> outPeak{0.f};

private:
    // Biquad direct-form II transposed. State & koefisien double: stabil di cutoff rendah
    // (HP 80 Hz @ 48 kHz) dan tidak menimbulkan zipper noise saat slider digeser.
    struct Biquad {
        double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
        float Tick(float x) {
            double xd = x;
            double y = b0 * xd + z1;
            z1 = b1 * xd - a1 * y + z2;
            z2 = b2 * xd - a2 * y;
            return (float)y;
        }
        void Clear() { z1 = z2 = 0; }
        void SetHighPass(double f, double sr, double q);
        void SetLowPass(double f, double sr, double q);
        void SetPeaking(double f, double sr, double q, double gainDb);
    };

    void UpdateCoefs(const ExParams& p);
    void StartTone(float hz, float ms);
    void StartTail(float ms);
    float NextNoise();
    void ClearFilters();
    void ClearTransmitState();     // dipanggil saat PTT/efek baru aktif: buang sisa state transmisi sebelumnya

    int   sr_ = 0;
    float lastHp_ = -1, lastLp_ = -1;

    // band-pass utama (Butterworth orde-4 HP, orde-4 LP dengan sedikit resonansi), presence,
    // filter pasca-saturasi, dan filter pembentuk noise static.
    Biquad hp_[2], lp_[2], pres_, pHp_, pLp_, nHp_, nLp_;

    // koefisien waktu
    float detAtt_ = 0, detRel_ = 0;                 // detektor gate
    float gateAtt_ = 0, gateRel_ = 0;               // smoothing gain gate
    float cdAtt_ = 0, cdRel_ = 0;                   // detektor level kompresor
    float grAtt_ = 0, grRelFast_ = 0, grRelSlow_ = 0; // smoothing gain reduction (dB)
    float limRel_ = 0;
    int   holdSamples_ = 0;

    // state
    float env_ = 0, gateG_ = 1.f, cenv_ = 0, lim_ = 1.f;
    float grFast_ = 0, grSlow_ = 0;                 // gain reduction kompresor (dB, positif)
    int   hold_ = 0;
    bool  gateOpen_ = false;
    double uPrev_ = 0, fPrev_ = 0;                  // state ADAA saturasi (double: selisih u kecil tidak boleh kena pembulatan float)
    bool  first_ = true, prevActive_ = false;

    // beep / squelch tail
    int   toneRem_ = 0, toneTotal_ = 0, tailRem_ = 0, tailTotal_ = 0;
    float tonePhase_ = 0, toneStep_ = 0;

    uint32_t rng_ = 0x1234ABCDu;
};
