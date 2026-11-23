#pragma once
// Producer sisi user-mode (C++): float32 mono @ 44.1/48 kHz -> int16 @ 48 kHz -> ring EXTER.
// Portabel (tanpa API Windows) sehingga bisa diuji di luar Windows. Tanpa alokasi di jalur audio.
#include "ExterTransport.h"
#include <cmath>
#include <cstddef>

class ExterTransportWriter {
public:
    // h: header ring yang sudah ExterRing_Init. inputRate: 44100 atau 48000.
    void Attach(EXTER_RING_HEADER* h, int inputRate) {
        h_ = h;
        step_ = (inputRate == (int)EXTER_SAMPLE_RATE) ? 1.0 : (double)inputRate / (double)EXTER_SAMPLE_RATE;
        resample_ = (inputRate != (int)EXTER_SAMPLE_RATE);
        pos_ = 0.0; prev_ = 0.f; n_ = 0;
    }
    void Detach() { Flush(); h_ = nullptr; }
    bool Attached() const { return h_ != nullptr; }

    void Start() { if (h_) ExterRing_SetProducerState(h_, EXTER_PRODUCER_RUNNING); }
    void Stop()  { if (h_) { Flush(); ExterRing_SetProducerState(h_, EXTER_PRODUCER_STOPPED); } }

    // Dipanggil dari thread capture setelah DSP. Semua sample di-flush sebelum return.
    void Write(const float* in, size_t count) {
        if (!h_) return;
        if (!resample_) { for (size_t i = 0; i < count; ++i) Push(in[i]); }
        else {
            for (size_t i = 0; i < count; ++i) {
                const float x = in[i];
                while (pos_ < 1.0) {
                    Push(prev_ + (x - prev_) * (float)pos_);
                    pos_ += step_;
                }
                pos_ -= 1.0;
                prev_ = x;
            }
        }
        Flush();
    }

    static ex_i16 ToPcm(float x) {
        // NaN/Inf -> senyap. Dicek lewat bit (bukan x==x) agar tetap benar di bawah /fp:fast.
        ex_u32 bits; memcpy(&bits, &x, sizeof(bits));
        if ((bits & 0x7F800000u) == 0x7F800000u) return 0;
        if (x > 1.f) x = 1.f; else if (x < -1.f) x = -1.f;
        return (ex_i16)(x * 32767.f + (x >= 0.f ? 0.5f : -0.5f));
    }

private:
    void Push(float x) { buf_[n_++] = ToPcm(x); if (n_ == kBuf) Flush(); }
    void Flush() { if (n_ && h_) { ExterRing_Write(h_, buf_, (ex_u32)n_); } n_ = 0; }

    static constexpr size_t kBuf = 1024;
    EXTER_RING_HEADER* h_ = nullptr;
    double step_ = 1.0, pos_ = 0.0;
    float prev_ = 0.f;
    bool resample_ = false;
    ex_i16 buf_[kBuf];
    size_t n_ = 0;
};
