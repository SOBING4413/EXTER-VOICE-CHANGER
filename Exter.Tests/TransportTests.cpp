// Tes transport EXTER tanpa Windows: g++ -std=c++17 -O2 -I../Exter.Shared TransportTests.cpp -pthread
#include "ExterTransport.h"
#include "ExterTransportWriter.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <sys/mman.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

struct Ring {
    std::vector<unsigned char> mem;
    EXTER_RING_HEADER* h;
    explicit Ring(uint32_t frames = EXTER_RING_FRAMES) : mem(ExterRing_TotalBytes(frames, 1) + 64, 0) {
        h = (EXTER_RING_HEADER*)(((uintptr_t)mem.data() + 63) & ~(uintptr_t)63);
        ExterRing_Init(h, frames);
    }
};
static EXTER_CONSUMER MakeConsumer(EXTER_RING_HEADER* h, uint32_t target = 960, uint32_t hw = 4800, uint32_t stale = 24000) {
    EXTER_CONSUMER c; ExterConsumer_Init(&c, h, target, hw, stale); return c;
}
static std::vector<int16_t> Ramp(int start, int n) { std::vector<int16_t> v(n); for (int i = 0; i < n; ++i) v[i] = (int16_t)(uint16_t)(start + i); return v; }

static void TestValidate() {
    std::puts("validate");
    Ring r; CHECK(ExterRing_Validate(r.h, ExterRing_TotalBytes(EXTER_RING_FRAMES, 1)));
    CHECK(!ExterRing_Validate(r.h, 100));                                   // terlalu kecil
    CHECK(!ExterRing_Validate(r.h, ExterRing_TotalBytes(EXTER_RING_FRAMES, 1) - 2));
    CHECK(!ExterRing_Validate(nullptr, 1 << 20));
    Ring bad; bad.h->ringFrames = 5000;  CHECK(!ExterRing_Validate(bad.h, 1 << 20));   // bukan pangkat 2
    Ring bad2; bad2.h->magic = 0;        CHECK(!ExterRing_Validate(bad2.h, 1 << 20));
    Ring bad3; bad3.h->sampleRate = 44100; CHECK(!ExterRing_Validate(bad3.h, 1 << 20));
    Ring bad4; bad4.h->ringFrames = 1u << 30; CHECK(!ExterRing_Validate(bad4.h, 1 << 20));
}

static void TestIntegrityAndWrap() {
    std::puts("integrity + wraparound");
    Ring r(4096);
    auto c = MakeConsumer(r.h, 480, 2000, 100000);
    ExterRing_SetProducerState(r.h, EXTER_PRODUCER_RUNNING);
    int next = 0, expect = 0; bool ok = true; size_t total = 0;
    std::vector<int16_t> out(333);
    for (int iter = 0; iter < 400; ++iter) {                 // ~ >30 putaran ring kecil
        auto in = Ramp(next, 300); ExterRing_Write(r.h, in.data(), 300); next += 300;
        uint32_t got = ExterConsumer_Read(&c, out.data(), 333 > 300 ? 300 : 300);
        for (uint32_t i = 0; i < got; ++i) { if (out[i] != (int16_t)(uint16_t)expect) ok = false; ++expect; }
        total += got;
    }
    CHECK(ok); CHECK(total > 100000);
    CHECK(r.h->tornReads == 0); CHECK(r.h->overruns == 0);
}

static void TestPrimingAndUnderrun() {
    std::puts("priming + underrun");
    Ring r; auto c = MakeConsumer(r.h, 960, 4800, 1000000);
    ExterRing_SetProducerState(r.h, EXTER_PRODUCER_RUNNING);
    std::vector<int16_t> out(700), in = Ramp(1, 480);
    ExterRing_Write(r.h, in.data(), 480);
    CHECK(ExterConsumer_Read(&c, out.data(), 480) == 0);       // belum mencapai target -> senyap
    for (int16_t s : out) CHECK(s == 0);
    ExterRing_Write(r.h, in.data(), 480); ExterRing_Write(r.h, in.data(), 480);   // fill 1440 >= 960
    CHECK(ExterConsumer_Read(&c, out.data(), 480) == 480);
    CHECK(ExterConsumer_Read(&c, out.data(), 480) == 480);
    CHECK(ExterConsumer_Read(&c, out.data(), 480) == 480);     // habis tepat
    CHECK(r.h->underruns == 0);
    ExterRing_Write(r.h, in.data(), 100);                      // hanya 100 frame, minta 480 (masih primed)
    CHECK(ExterConsumer_Read(&c, out.data(), 480) == 100);     // parsial: 100 nyata + sisa senyap
    for (int i = 100; i < 480; ++i) CHECK(out[i] == 0);
    CHECK(r.h->underruns == 1);                                // underrun terhitung, consumer unprimed
    ExterRing_Write(r.h, in.data(), 480);                      // 480 < target 960 -> tetap senyap
    CHECK(ExterConsumer_Read(&c, out.data(), 480) == 0);
    ExterRing_Write(r.h, in.data(), 480);                      // total 960 >= target -> primed lagi
    CHECK(ExterConsumer_Read(&c, out.data(), 480) == 480);
    uint32_t g = ExterConsumer_Read(&c, out.data(), 600);      // tersisa 480 -> parsial
    CHECK(g == 480); CHECK(r.h->underruns == 2);
    for (uint32_t i = g; i < 600; ++i) CHECK(out[i] == 0);
}

static void TestLatencyDrop() {
    std::puts("latency drop (clock drift)");
    Ring r; auto c = MakeConsumer(r.h, 960, 4800, 1000000);
    ExterRing_SetProducerState(r.h, EXTER_PRODUCER_RUNNING);
    auto in = Ramp(0, 480); std::vector<int16_t> out(480);
    int next = 0;
    for (int i = 0; i < 40; ++i) { in = Ramp(next, 480); ExterRing_Write(r.h, in.data(), 480); next += 480; }  // 19200 frame
    uint32_t g = ExterConsumer_Read(&c, out.data(), 480);
    CHECK(g == 480); CHECK(r.h->latencyDrops == 1);
    uint64_t fillAfter = r.h->writePos - r.h->readPos;
    CHECK(fillAfter <= 960);                                    // kembali ke ~target
    CHECK((uint16_t)out[0] == (uint16_t)(next - 960 - 480));    // yang dibuang adalah yang TERTUA
}

static void TestStaleAndStopped() {
    std::puts("producer stopped / stale");
    Ring r; auto c = MakeConsumer(r.h, 960, 4800, 2400);       // stale setelah 2400 frame
    std::vector<int16_t> out(480), in = Ramp(1, 4000);
    ExterRing_SetProducerState(r.h, EXTER_PRODUCER_RUNNING);
    ExterRing_Write(r.h, in.data(), 2000);
    CHECK(ExterConsumer_Read(&c, out.data(), 480) == 480);
    ExterRing_SetProducerState(r.h, EXTER_PRODUCER_STOPPED);
    CHECK(ExterConsumer_Read(&c, out.data(), 480) == 0);
    for (int16_t s : out) CHECK(s == 0);
    // producer RUNNING tapi hang (writePos beku)
    ExterRing_SetProducerState(r.h, EXTER_PRODUCER_RUNNING);
    ExterRing_Write(r.h, in.data(), 2000);
    int silent = 0; uint32_t realFrames = 0;
    for (int i = 0; i < 20; ++i) { uint32_t g = ExterConsumer_Read(&c, out.data(), 480); realFrames += g; if (g == 0) ++silent; }
    CHECK(r.h->staleEvents == 1);
    CHECK(silent >= 12);
    // producer pulih: menulis lagi -> audio kembali setelah prefill
    in = Ramp(777, 4000); ExterRing_Write(r.h, in.data(), 4000);
    uint32_t g = ExterConsumer_Read(&c, out.data(), 480);
    CHECK(g == 480);
}

static void TestOverrunLapped() {
    std::puts("overrun (producer menyalip consumer)");
    Ring r(4096); auto c = MakeConsumer(r.h, 480, 2000, 1000000);
    ExterRing_SetProducerState(r.h, EXTER_PRODUCER_RUNNING);
    auto in = Ramp(0, 3000); std::vector<int16_t> out(240);
    for (int i = 0; i < 5; ++i) ExterRing_Write(r.h, in.data(), 3000);   // 15000 frame tanpa pembaca
    uint32_t g = ExterConsumer_Read(&c, out.data(), 240);
    CHECK(r.h->overruns == 1); CHECK(g == 240);
}

static void TestSetOpenStartsFresh() {
    std::puts("SetOpen mulai dari posisi terkini");
    Ring r; auto c = MakeConsumer(r.h);
    ExterRing_SetProducerState(r.h, EXTER_PRODUCER_RUNNING);
    auto in = Ramp(0, 5000); ExterRing_Write(r.h, in.data(), 5000);
    ExterConsumer_SetOpen(&c, 1);
    CHECK(r.h->consumerState == EXTER_CONSUMER_OPEN);
    CHECK(r.h->readPos == r.h->writePos);
    std::vector<int16_t> out(480); CHECK(ExterConsumer_Read(&c, out.data(), 480) == 0);   // tidak memutar audio lama
    ExterConsumer_SetOpen(&c, 0); CHECK(r.h->consumerState == EXTER_CONSUMER_CLOSED);
}

// Dua thread: producer cepat & tak teratur, consumer jitter. Setiap chunk yang dikembalikan harus berurutan
// (tidak ada sample rusak/teracak); diskontinuitas hanya boleh terjadi antar-chunk (drop/resync).
static void TestThreadStress() {
    std::puts("thread stress (SPSC, jitter, drift)");
    Ring r(8192); auto c = MakeConsumer(r.h, 960, 4000, 1000000);
    ExterRing_SetProducerState(r.h, EXTER_PRODUCER_RUNNING);
    std::atomic<bool> stop{false};
    std::thread prod([&] {
        uint32_t next = 0; unsigned seed = 1;
        while (!stop.load()) {
            seed = seed * 1664525u + 1013904223u;
            int n = 64 + (seed >> 16) % 900;
            auto in = Ramp((int)next, n); ExterRing_Write(r.h, in.data(), n); next += n;
            if ((seed & 15) == 0) std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    });
    long chunks = 0, bad = 0; unsigned seed = 7;
    std::vector<int16_t> out(1024);
    auto t0 = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::milliseconds(1500)) {
        seed = seed * 1664525u + 1013904223u;
        uint32_t want = 100 + (seed >> 16) % 800;
        uint32_t g = ExterConsumer_Read(&c, out.data(), want);
        for (uint32_t i = 1; i < g; ++i) if ((uint16_t)out[i] != (uint16_t)(out[i - 1] + 1)) { ++bad; break; }
        for (uint32_t i = g; i < want; ++i) if (out[i] != 0) { ++bad; break; }
        ++chunks;
        std::this_thread::sleep_for(std::chrono::microseconds(100 + (seed >> 8) % 300));
    }
    stop = true; prod.join();
    std::printf("    chunks=%ld bad=%ld underruns=%llu drops=%llu overruns=%llu torn=%llu\n", chunks, bad,
        (unsigned long long)r.h->underruns, (unsigned long long)r.h->latencyDrops,
        (unsigned long long)r.h->overruns, (unsigned long long)r.h->tornReads);
    CHECK(chunks > 500); CHECK(bad == 0);
}

#ifndef _WIN32
// Dua PROSES berbagi memori (meniru engine user-mode <-> driver): fork + mmap(MAP_SHARED).
static void TestCrossProcess() {
    std::puts("cross-process (fork + shared mmap)");
    size_t bytes = ExterRing_TotalBytes(EXTER_RING_FRAMES, 1);
    void* mem = mmap(nullptr, bytes, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_ANONYMOUS, -1, 0);
    CHECK(mem != MAP_FAILED);
    EXTER_RING_HEADER* h = (EXTER_RING_HEADER*)mem; ExterRing_Init(h, EXTER_RING_FRAMES);
    pid_t pid = fork();
    if (pid == 0) {                                          // anak = "engine" (producer)
        ExterRing_SetProducerState(h, EXTER_PRODUCER_RUNNING);
        uint32_t next = 0;
        for (int i = 0; i < 300; ++i) { auto in = Ramp((int)next, 480); ExterRing_Write(h, in.data(), 480); next += 480; usleep(2000); }
        ExterRing_SetProducerState(h, EXTER_PRODUCER_STOPPED);
        _exit(0);
    }
    CHECK(ExterRing_Validate(h, bytes));
    EXTER_CONSUMER c; ExterConsumer_Init(&c, h, 960, 4800, 24000); ExterConsumer_SetOpen(&c, 1);
    std::vector<int16_t> out(480); long total = 0, bad = 0;
    for (int i = 0; i < 400; ++i) {
        uint32_t g = ExterConsumer_Read(&c, out.data(), 480);
        for (uint32_t k = 1; k < g; ++k) if ((uint16_t)out[k] != (uint16_t)(out[k - 1] + 1)) { ++bad; break; }
        total += g; usleep(2000);
    }
    int st = 0; waitpid(pid, &st, 0);
    std::printf("    frames diterima=%ld bad=%ld\n", total, bad);
    CHECK(total > 60000); CHECK(bad == 0);
    munmap(mem, bytes);
}
#endif

// Memori ring ditulis proses user-mode -> bisa dirusak/dipalsukan KAPAN SAJA setelah validasi.
// Consumer (driver) tidak boleh membaca di luar buffer apa pun isi header. Dijalankan di bawah ASan.
static void TestTamperedHeader() {
    std::puts("header dipalsukan setelah validasi (keamanan consumer)");
    Ring r(4096); auto c = MakeConsumer(r.h, 480, 2000, 1000000);
    CHECK(ExterRing_Validate(r.h, ExterRing_TotalBytes(4096, 1)));
    ExterRing_SetProducerState(r.h, EXTER_PRODUCER_RUNNING);
    std::vector<int16_t> out(1024); unsigned seed = 99; long reads = 0;
    auto in = Ramp(0, 600);
    for (int i = 0; i < 20000; ++i) {
        seed = seed * 1664525u + 1013904223u;
        switch (seed % 6) {                                         // user-mode "nakal"
            case 0: r.h->ringFrames = seed; break;                  // ukuran palsu / sangat besar / nol
            case 1: r.h->readPos = ((uint64_t)seed << 32) | seed; break;
            case 2: *(volatile uint64_t*)&r.h->writePos = ((uint64_t)seed << 31) ^ seed; break;
            case 3: r.h->frameBytes = seed; r.h->channels = seed; break;
            case 4: ExterRing_SetProducerState(r.h, EXTER_PRODUCER_RUNNING); break;
            default: ExterRing_Write(r.h, in.data(), 0); break;
        }
        uint32_t want = 1 + (seed >> 8) % 1024;
        ExterConsumer_Read(&c, out.data(), want);                   // tidak boleh crash / OOB (ASan)
        ++reads;
        if (r.h->ringFrames == 0 || r.h->ringFrames > 4096) r.h->ringFrames = 4096;   // agar Write producer uji tetap valid
    }
    CHECK(reads == 20000);
    CHECK(c.ringFrames == 4096);                                    // nilai privat tidak berubah
}

static void TestWriterConversion() {
    std::puts("writer: float->int16 + resample");
    CHECK(ExterTransportWriter::ToPcm(0.f) == 0);
    CHECK(ExterTransportWriter::ToPcm(1.f) == 32767);
    CHECK(ExterTransportWriter::ToPcm(-1.f) == -32767);
    CHECK(ExterTransportWriter::ToPcm(5.f) == 32767);          // clamp
    CHECK(ExterTransportWriter::ToPcm(-5.f) == -32767);
    CHECK(ExterTransportWriter::ToPcm(NAN) == 0);
    // 48 kHz: lolos persis, tanpa delay
    {
        Ring r; ExterTransportWriter w; w.Attach(r.h, 48000); w.Start();
        std::vector<float> in(5000); for (int i = 0; i < 5000; ++i) in[i] = 0.5f * std::sin(0.01f * i);
        w.Write(in.data(), in.size());
        CHECK(r.h->writePos == 5000);
        const int16_t* d = EXTER_RING_DATA(r.h); bool same = true;
        for (int i = 0; i < 5000; ++i) if (d[i] != ExterTransportWriter::ToPcm(in[i])) same = false;
        CHECK(same); CHECK(r.h->producerState == EXTER_PRODUCER_RUNNING);
        w.Stop(); CHECK(r.h->producerState == EXTER_PRODUCER_STOPPED);
    }
    // 44.1 kHz -> 48 kHz: 1 kHz harus tetap 1 kHz, jumlah sample benar, amplitudo terjaga; kirim bertahap (blok ganjil)
    {
        Ring r; ExterTransportWriter w; w.Attach(r.h, 44100); w.Start();
        const double fs = 44100.0; const int N = 44100;
        std::vector<float> in(N); for (int i = 0; i < N; ++i) in[i] = (float)(0.8 * std::sin(2 * M_PI * 1000.0 * i / fs));
        for (int off = 0; off < N; off += 441 + (off % 7)) w.Write(in.data() + off, std::min(441 + (off % 7), N - off));
        uint64_t n = r.h->writePos;
        std::printf("    sample keluaran = %llu (harapan ~48000)\n", (unsigned long long)n);
        CHECK(n >= 47995 && n <= 48005);
        const int16_t* d = EXTER_RING_DATA(r.h);
        int cross = 0; double peak = 0;
        for (uint64_t i = 1; i < 32768 && i < n; ++i) { if (d[i - 1] < 0 && d[i] >= 0) ++cross; peak = std::max(peak, std::fabs((double)d[i]) / 32767.0); }
        double hz = cross * 48000.0 / 32768.0;                   // ring 32768: 32768 sample terakhir tertimpa -> pakai jendela ring
        std::printf("    frekuensi terukur ~%.1f Hz, puncak %.3f\n", hz, peak);
        CHECK(std::fabs(hz - 1000.0) < 2.0 * 48000.0 / 32768.0 + 1.0);
        CHECK(peak > 0.78 && peak < 0.82);
    }
}

int main() {
    TestValidate(); TestIntegrityAndWrap(); TestPrimingAndUnderrun(); TestLatencyDrop();
    TestStaleAndStopped(); TestOverrunLapped(); TestTamperedHeader(); TestSetOpenStartsFresh(); TestWriterConversion();
    TestThreadStress();
#ifndef _WIN32
    TestCrossProcess();
#endif
    std::printf("\n%d pemeriksaan, %d gagal\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
