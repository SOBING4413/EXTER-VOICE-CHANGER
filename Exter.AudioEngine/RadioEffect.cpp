#include "RadioEffect.h"
#include <algorithm>
#include <cmath>
#include <cstring>

static const double kPi = 3.14159265358979323846;
static const float  kTwoPi = 6.28318530717958647692f;
static const float  kLimit = 0.891f;        // -1 dBFS

// ---- Karakter radio taktis (nilai tetap; bagian yang bisa diatur user tetap lewat ExParams) ----
static const float kPresenceDb   = 4.5f;    // dorongan "presence" mic/speaker radio di tengah band
static const float kPresenceQ    = 0.8f;
static const double kLpQ2        = 1.60;    // Q seksi LP ke-2: sedikit resonansi di tepi atas (ciri "tinny")
static const float kGateFloor    = 0.003f;  // -50 dB: gate menutup halus, bukan potong total
static const float kGateHystDb   = 5.f;     // threshold tutup = threshold buka - 5 dB (anti-chatter)
static const float kKneeDb       = 8.f;     // soft-knee kompresor
static const float kMakeupFrac   = 0.5f;    // porsi kompensasi gain otomatis
static const float kSatBias      = 0.12f;   // bias saturasi -> harmonik genap (grit amplifier radio)

void RadioEffect::Biquad::SetHighPass(double f, double sr, double q) {
    double w0 = 2.0 * kPi * f / sr, c = std::cos(w0), s = std::sin(w0), al = s / (2.0 * q);
    double a0 = 1.0 + al;
    b0 = ((1.0 + c) / 2.0) / a0;
    b1 = (-(1.0 + c)) / a0;
    b2 = b0;
    a1 = (-2.0 * c) / a0;
    a2 = (1.0 - al) / a0;
}

void RadioEffect::Biquad::SetLowPass(double f, double sr, double q) {
    double w0 = 2.0 * kPi * f / sr, c = std::cos(w0), s = std::sin(w0), al = s / (2.0 * q);
    double a0 = 1.0 + al;
    b0 = ((1.0 - c) / 2.0) / a0;
    b1 = (1.0 - c) / a0;
    b2 = b0;
    a1 = (-2.0 * c) / a0;
    a2 = (1.0 - al) / a0;
}

void RadioEffect::Biquad::SetPeaking(double f, double sr, double q, double gainDb) {
    double A = std::pow(10.0, gainDb / 40.0);
    double w0 = 2.0 * kPi * f / sr, c = std::cos(w0), s = std::sin(w0), al = s / (2.0 * q);
    double a0 = 1.0 + al / A;
    b0 = (1.0 + al * A) / a0;
    b1 = (-2.0 * c) / a0;
    b2 = (1.0 - al * A) / a0;
    a1 = b1;
    a2 = (1.0 - al / A) / a0;
}

// Cek finite berbasis bit: tetap benar walau dikompilasi dengan /fp:fast (std::isfinite / x==x boleh dioptimasi habis).
static inline bool FiniteBits(float x) {
    uint32_t u; std::memcpy(&u, &x, sizeof(u));
    return (u & 0x7F800000u) != 0x7F800000u;
}

// log(cosh(u)) yang stabil secara numerik (antiderivatif tanh, dipakai ADAA).
static inline double LogCosh(double u) {
    double a = std::fabs(u);
    return a + std::log1p(std::exp(-2.0 * a)) - 0.69314718055994531;
}

RadioEffect::RadioEffect() { Reset(); }

void RadioEffect::ClearFilters() {
    for (auto& b : hp_) b.Clear();
    for (auto& b : lp_) b.Clear();
    pres_.Clear(); pHp_.Clear(); pLp_.Clear();
}

void RadioEffect::Reset() {
    ClearFilters();
    nHp_.Clear(); nLp_.Clear();
    env_ = cenv_ = 0; gateG_ = 1.f; lim_ = 1.f; hold_ = 0; gateOpen_ = false;
    grFast_ = grSlow_ = 0;
    uPrev_ = kSatBias; fPrev_ = LogCosh(kSatBias);
    first_ = true; prevActive_ = false;
    toneRem_ = tailRem_ = 0; tonePhase_ = 0;
    lastHp_ = lastLp_ = -1; sr_ = 0;
    inPeak = 0; outPeak = 0;
}

void RadioEffect::ClearTransmitState() {
    // Filter & detektor tidak berjalan saat efek non-aktif, jadi isinya basi (dari akhir transmisi sebelumnya).
    // Tanpa ini, awal transmisi berikutnya mengeluarkan klik/ekor dari ucapan lama dan kompresor menekan
    // kata pertama dengan gain-reduction basi.
    ClearFilters();
    env_ = cenv_ = 0; grFast_ = grSlow_ = 0;
    uPrev_ = kSatBias; fPrev_ = LogCosh(kSatBias);
}

float RadioEffect::NextNoise() {
    rng_ ^= rng_ << 13; rng_ ^= rng_ >> 17; rng_ ^= rng_ << 5;   // xorshift32
    return (float)((int32_t)rng_) * (1.0f / 2147483648.0f);       // -1..1
}

void RadioEffect::StartTone(float hz, float ms) {
    toneTotal_ = toneRem_ = (int)(sr_ * ms * 0.001f);
    toneStep_ = kTwoPi * hz / (float)sr_;
    tonePhase_ = 0;
}

void RadioEffect::StartTail(float ms) {
    tailTotal_ = tailRem_ = (int)(sr_ * ms * 0.001f);
}

// Clamp yang aman terhadap NaN (NaN -> nilai default), karena std::min/max dengan NaN hasilnya tak menentu.
static inline float Clampf(float v, float lo, float hi, float dflt) {
    if (!FiniteBits(v)) return dflt;
    return v < lo ? lo : (v > hi ? hi : v);
}

void RadioEffect::UpdateCoefs(const ExParams& p) {
    float hpHz = Clampf(p.highPassHz, 80.f, 1200.f, 300.f);
    float lpHz = Clampf(p.lowPassHz, 1500.f, 8000.f, 3400.f);
    lpHz = std::min(lpHz, sr_ * 0.45f);
    if (hpHz == lastHp_ && lpHz == lastLp_) return;
    lastHp_ = hpHz; lastLp_ = lpHz;

    // Band-pass utama: HP Butterworth orde-4 (Q 0.5412 / 1.3066), LP orde-4 dengan seksi ke-2
    // sedikit diresonansikan agar tepi atas terdengar "tipis" seperti speaker/codec radio.
    hp_[0].SetHighPass(hpHz, sr_, 0.5412); hp_[1].SetHighPass(hpHz, sr_, 1.3066);
    lp_[0].SetLowPass(lpHz, sr_, 0.5412);  lp_[1].SetLowPass(lpHz, sr_, kLpQ2);

    // Presence di sekitar 1.6x rata-rata geometrik band (300..3400 Hz -> ~1.6 kHz).
    float presHz = std::sqrt(hpHz * lpHz) * 1.6f;
    presHz = std::min(std::max(presHz, 900.f), lpHz * 0.65f);
    pres_.SetPeaking(presHz, sr_, kPresenceQ, kPresenceDb);

    // Filter pasca-saturasi (orde-2): HP membuang DC/intermod rendah akibat bias asimetris,
    // LP (di lpHz) menegaskan skirt atas & membuang harmonik tinggi hasil saturasi.
    pHp_.SetHighPass(hpHz * 0.8f, sr_, 0.7071);
    pLp_.SetLowPass(lpHz, sr_, 0.7071);
}

static inline void AtomicMax(std::atomic<float>& a, float v) {
    float cur = a.load(std::memory_order_relaxed);
    while (v > cur && !a.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
}

void RadioEffect::Process(float* b, int n, const ExParams& p, bool active, int sr) {
    if (sr != sr_) {
        sr_ = sr; lastHp_ = lastLp_ = -1;
        auto coefOf = [sr](float ms) { return std::exp(-1.0f / (ms * 0.001f * (float)sr)); };
        detAtt_ = coefOf(1.f);    detRel_ = coefOf(30.f);          // detektor gate
        gateAtt_ = 1.f - coefOf(1.f); gateRel_ = 1.f - coefOf(45.f);
        holdSamples_ = (int)(sr * 0.07f);
        cdAtt_ = coefOf(0.3f);   cdRel_ = coefOf(30.f);           // detektor kompresor
        grAtt_ = coefOf(3.f);                                      // attack 3 ms
        grRelFast_ = coefOf(70.f); grRelSlow_ = coefOf(400.f);     // release ganda (auto-release)
        limRel_ = 1.f - coefOf(100.f);
        nHp_.SetHighPass(600.0, sr, 0.7071);
        nLp_.SetLowPass(3200.0, sr, 0.7071);
    }
    UpdateCoefs(p);

    if (first_) { prevActive_ = active; first_ = false; }
    if (active != prevActive_) {
        if (active) ClearTransmitState();
        if (p.beepEnabled) {
            if (active) StartTone(1000.f, 45.f);                 // klik/beep awal transmisi
            else { StartTone(750.f, 70.f); StartTail(160.f); }   // roger beep + squelch tail
        }
        prevActive_ = active;
    }

    const float inG = Clampf(p.inputGain, 0.f, 4.f, 1.f), outG = Clampf(p.outputGain, 0.f, 4.f, 1.f);
    const float thrOpen  = std::pow(10.f, Clampf(p.gateThresholdDb, -80.f, -10.f, -45.f) / 20.f);
    const float thrClose = thrOpen * std::pow(10.f, -kGateHystDb / 20.f);
    const float compThr = Clampf(p.compThresholdDb, -50.f, -5.f, -22.f);
    const float ratio = Clampf(p.compRatio, 1.f, 20.f, 4.f);
    const float slope = 1.f - 1.f / ratio;
    const float makeup = -compThr * slope * kMakeupFrac;          // makeup gain otomatis (dB)
    const float drive = Clampf(p.drive, 1.f, 8.f, 1.8f);
    const float invSqrtDrive = 1.f / std::sqrt(drive);
    const float satDc = std::tanh(kSatBias);                      // offset keluaran saat input diam
    const float staticAmp = Clampf(p.staticLevel, 0.f, 1.f, 0.25f) * 0.15f;
    const float kDbToNeper = 0.11512925f;                         // ln(10)/20

    if (!p.gateEnabled) { gateOpen_ = true; gateG_ = 1.f; }

    float inPk = 0.f, outPk = 0.f;

    for (int i = 0; i < n; ++i) {
        float raw = b[i];
        if (!FiniteBits(raw)) raw = 0.f;                          // driver/mic rusak: jangan racuni state filter
        inPk = std::max(inPk, std::fabs(raw));
        float x = raw * inG;
        float nz = nLp_.Tick(nHp_.Tick(NextNoise()));
        float y;

        if (active) {
            // 1) Band-pass
            y = hp_[1].Tick(hp_[0].Tick(x));
            y = lp_[1].Tick(lp_[0].Tick(y));

            // 2) Noise gate: detektor di sinyal band-limited (dengung/desis di luar band tidak
            //    membuka gate), hysteresis + hold agar tidak chatter / memotong akhir kata.
            float a = std::fabs(y);
            env_ = (a > env_) ? env_ * detAtt_ + a * (1.f - detAtt_) : env_ * detRel_ + a * (1.f - detRel_);

            // 3) Presence EQ (setelah detektor, agar arti threshold gate tidak bergeser)
            y = pres_.Tick(y);

            if (p.gateEnabled) {
                if (env_ > thrOpen)        { gateOpen_ = true; hold_ = holdSamples_; }
                else if (gateOpen_) {
                    if (env_ < thrClose) { if (hold_ > 0) --hold_; else gateOpen_ = false; }
                    else hold_ = holdSamples_;                      // zona hysteresis: tetap terbuka
                }
                float t = gateOpen_ ? 1.f : kGateFloor;
                gateG_ += (t - gateG_) * (t > gateG_ ? gateAtt_ : gateRel_);
                y *= gateG_;
            }

            // 4) Kompresor radio: detektor peak -> gain computer soft-knee -> smoothing di domain dB
            //    dengan release ganda (cepat 70 ms + lambat 400 ms) = auto-release ala AGC radio.
            float a2 = std::fabs(y);
            cenv_ = (a2 > cenv_) ? cenv_ * cdAtt_ + a2 * (1.f - cdAtt_) : cenv_ * cdRel_ + a2 * (1.f - cdRel_);
            float db = 20.f * std::log10(std::max(cenv_, 1e-6f));
            float over = db - compThr;
            float grT;
            if (2.f * over < -kKneeDb)      grT = 0.f;
            else if (2.f * over > kKneeDb)  grT = over * slope;
            else { float t = over + kKneeDb * 0.5f; grT = slope * t * t / (2.f * kKneeDb); }
            grFast_ = grT + (grFast_ - grT) * (grT > grFast_ ? grAtt_ : grRelFast_);
            grSlow_ = grT + (grSlow_ - grT) * (grT > grSlow_ ? grAtt_ : grRelSlow_);
            float gr = 0.6f * grFast_ + 0.4f * grSlow_;
            y *= std::exp((makeup - gr) * kDbToNeper);

            // 5) Saturasi asimetris: tanh dengan bias (harmonik genap + ganjil) memakai
            //    anti-aliasing antiderivatif orde-1 (ADAA) -> tanpa oversampling, tanpa latency
            //    (hanya setengah sampel), dan tanpa alias yang "berdesis" di band suara.
            double u = (double)drive * y + kSatBias;
            double f = LogCosh(u);
            double du = u - uPrev_;
            double s = (std::fabs(du) > 1e-6) ? (f - fPrev_) / du : std::tanh(0.5 * (u + uPrev_));
            uPrev_ = u; fPrev_ = f;
            y = ((float)s - satDc) * invSqrtDrive;

            // 6) Band-limit ulang + buang DC
            y = pLp_.Tick(pHp_.Tick(y));

            // 7) Static radio
            y += nz * staticAmp;
        } else {
            y = (p.bypassMode != 0) ? 0.f : x;
        }

        // Beep / squelch tail
        float fxs = 0.f;
        if (toneRem_ > 0) {
            int pos = toneTotal_ - toneRem_;
            int fade = std::max(1, (int)(sr_ * 0.004f));
            float env = 1.f;
            if (pos < fade) env = (float)pos / fade;
            if (toneRem_ < fade) env = std::min(env, (float)toneRem_ / fade);
            fxs += std::sin(tonePhase_) * 0.22f * env;
            tonePhase_ += toneStep_;
            if (tonePhase_ > kTwoPi) tonePhase_ -= kTwoPi;
            --toneRem_;
        }
        if (tailRem_ > 0) {
            float k = (float)tailRem_ / (float)tailTotal_;
            fxs += nz * 0.8f * k * k;
            --tailRem_;
        }
        y += fxs;

        y *= outG;

        // Limiter peak (attack instan, release 100 ms) + hard clamp terakhir
        float aa = std::fabs(y);
        float tg = aa > kLimit ? kLimit / aa : 1.f;
        if (tg < lim_) lim_ = tg; else lim_ += (1.f - lim_) * limRel_;
        y *= lim_;
        y = std::min(0.98f, std::max(-0.98f, y));

        if (!FiniteBits(y) || !FiniteBits(lim_)) {   // jaga-jaga: state rusak -> reset
            ClearTransmitState();
            nHp_.Clear(); nLp_.Clear();
            lim_ = 1.f; gateG_ = 1.f;
            y = 0.f;
        }
        outPk = std::max(outPk, std::fabs(y));
        b[i] = y;
    }
    AtomicMax(inPeak, inPk);
    AtomicMax(outPeak, outPk);
}
