// Tes DSP RadioEffect tanpa Windows: g++ -std=c++17 -O2 -I../Exter.AudioEngine DspTests.cpp ../Exter.AudioEngine/RadioEffect.cpp
// Memastikan output SELALU finite & di bawah limiter, termasuk untuk input NaN/Inf, parameter ekstrem
// dan pergantian aktif/non-aktif berulang (transisi PTT).
#include "RadioEffect.h"
#include <cmath>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static ExParams Defaults() {
    ExParams p{};
    p.inputGain = 1.f; p.outputGain = 1.f; p.highPassHz = 300.f; p.lowPassHz = 3400.f;
    p.gateEnabled = 1; p.gateThresholdDb = -45.f; p.compThresholdDb = -22.f; p.compRatio = 4.f;
    p.drive = 1.8f; p.staticLevel = 0.25f; p.beepEnabled = 1; p.bypassMode = 0;
    return p;
}

static bool Bounded(const std::vector<float>& v, float lim = 0.9801f) {
    for (float x : v) if (!std::isfinite(x) || std::fabs(x) > lim) return false;
    return true;
}

static std::vector<float> Tone(int n, float hz, float amp, int sr, int off = 0) {
    std::vector<float> v(n);
    for (int i = 0; i < n; ++i) v[i] = amp * std::sin(2.0 * 3.14159265358979 * hz * (i + off) / sr);
    return v;
}

static void TestExtremes() {
    std::puts("parameter ekstrem");
    const int srs[2] = { 48000, 44100 };
    const float loud[3] = { 0.001f, 0.5f, 1.0f };
    for (int sr : srs) for (float amp : loud) for (int mode = 0; mode < 3; ++mode) {
        RadioEffect fx; ExParams p = Defaults();
        if (mode == 1) { p.highPassHz = 1200; p.lowPassHz = 1500; p.drive = 8; p.compRatio = 20; p.compThresholdDb = -50; p.inputGain = 2; p.outputGain = 2; p.staticLevel = 1; }
        if (mode == 2) { p.highPassHz = 80; p.lowPassHz = 8000; p.drive = 1; p.compRatio = 1; p.gateEnabled = 0; p.gateThresholdDb = -80; }
        bool ok = true;
        for (int blk = 0; blk < 200; ++blk) {
            auto b = Tone(480, 1000.f, amp, sr, blk * 480);
            fx.Process(b.data(), 480, p, (blk / 20) % 2 == 0, sr);
            ok = ok && Bounded(b);
        }
        CHECK(ok);
    }
}

static void TestNanInf() {
    std::puts("input NaN / Inf");
    RadioEffect fx; ExParams p = Defaults();
    std::vector<float> b(480, 0.1f);
    b[10] = std::numeric_limits<float>::quiet_NaN();
    b[200] = std::numeric_limits<float>::infinity();
    b[300] = -std::numeric_limits<float>::infinity();
    fx.Process(b.data(), 480, p, true, 48000);
    CHECK(Bounded(b));
    // pulih: blok berikutnya yang normal harus bersih
    auto c = Tone(4800, 800.f, 0.3f, 48000);
    fx.Process(c.data(), 4800, p, true, 48000);
    CHECK(Bounded(c));
    float pk = 0; for (int i = 2400; i < 4800; ++i) pk = std::fmax(pk, std::fabs(c[i]));
    CHECK(pk > 0.02f);                       // tidak macet di senyap setelah NaN
}

static void TestBadParams() {
    std::puts("parameter di luar rentang / NaN (dari settings.xml rusak)");
    const float nan = std::numeric_limits<float>::quiet_NaN();
    ExParams q = Defaults();
    q.gateThresholdDb = nan; q.compThresholdDb = nan; q.compRatio = nan; q.drive = nan; q.staticLevel = nan;
    q.highPassHz = nan; q.lowPassHz = nan; q.inputGain = nan; q.outputGain = nan;
    RadioEffect fx; bool ok = true;
    for (int blk = 0; blk < 50; ++blk) {
        auto b = Tone(480, 1000.f, 0.4f, 48000, blk * 480);
        fx.Process(b.data(), 480, q, true, 48000);
        ok = ok && Bounded(b);
    }
    CHECK(ok);
}

static void TestActivationEdge() {
    std::puts("transisi PTT: tanpa lonjakan");
    RadioEffect fx; ExParams p = Defaults(); p.beepEnabled = 0; p.staticLevel = 0.f;
    // bicara keras -> lepas PTT (bypass) -> hening panjang -> tekan PTT lagi dengan sinyal kecil
    for (int blk = 0; blk < 40; ++blk) { auto b = Tone(480, 900.f, 0.9f, 48000, blk * 480); fx.Process(b.data(), 480, p, true, 48000); }
    for (int blk = 0; blk < 100; ++blk) { std::vector<float> z(480, 0.f); fx.Process(z.data(), 480, p, false, 48000); }
    std::vector<float> quiet(48, 0.f);
    fx.Process(quiet.data(), 48, p, true, 48000);          // 1 ms pertama setelah PTT, input senyap
    float pk = 0; for (float x : quiet) pk = std::fmax(pk, std::fabs(x));
    CHECK(pk < 0.01f);                                     // sisa state lama tidak boleh bocor sebagai klik
}

static void TestBypassAndMute() {
    std::puts("bypass / mute saat efek off");
    ExParams p = Defaults(); p.beepEnabled = 0;
    {
        RadioEffect fx; auto b = Tone(4800, 500.f, 0.3f, 48000); auto ref = b;
        fx.Process(b.data(), 4800, p, false, 48000);
        float err = 0; for (int i = 2400; i < 4800; ++i) err = std::fmax(err, std::fabs(b[i] - ref[i]));
        CHECK(err < 0.02f);                                // bypass ~ suara asli (limiter -1 dBFS tidak menyentuh 0.3)
    }
    {
        RadioEffect fx; p.bypassMode = 1; auto b = Tone(4800, 500.f, 0.3f, 48000);
        fx.Process(b.data(), 4800, p, false, 48000);
        float pk = 0; for (float x : b) pk = std::fmax(pk, std::fabs(x));
        CHECK(pk < 1e-6f);
    }
}

static void TestChangingSampleRate() {
    std::puts("ganti sample rate di tengah jalan");
    RadioEffect fx; ExParams p = Defaults(); bool ok = true;
    for (int i = 0; i < 10; ++i) {
        int sr = (i % 2) ? 44100 : 48000;
        auto b = Tone(960, 1200.f, 0.4f, sr, i * 960);
        fx.Process(b.data(), 960, p, true, sr);
        ok = ok && Bounded(b);
    }
    CHECK(ok);
}

int main() {
    TestExtremes(); TestNanInf(); TestBadParams(); TestActivationEdge(); TestBypassAndMute(); TestChangingSampleRate();
    std::printf("\n%d pemeriksaan, %d gagal\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
