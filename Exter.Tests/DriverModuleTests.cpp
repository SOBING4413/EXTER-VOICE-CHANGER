// Tes logika modul kernel ExterVmic.cpp di Linux (shim API kernel di kernel-shim/).
//   g++ -std=c++17 -fshort-wchar -fsanitize=address,undefined -pthread -Ikernel-shim
//       -I../Exter.Driver/sysvad/EndpointsCommon -I../Exter.Shared DriverModuleTests.cpp
// Yang diuji: validasi IOCTL attach (tanpa kebocoran view/MDL/section pada SETIAP jalur gagal), satu pemilik,
// satu consumer stream, urutan data, detach saat pembaca berjalan (thread), cleanup file object, shutdown.
#include "ExterVmic.cpp"        // di-include langsung agar g_Vmic terlihat

#include <atomic>
#include <cstdio>
#include <thread>
#include <vector>

static int g_fail = 0, g_checks = 0;
#define CHECK(c) do { ++g_checks; if (!(c)) { ++g_fail; std::printf("  FAIL %s:%d  %s\n", __FILE__, __LINE__, #c); } } while (0)

static DRIVER_OBJECT g_drv;
static FILE_OBJECT   g_fileA{1}, g_fileB{2};

static NTSTATUS Dispatch(UCHAR major, FILE_OBJECT* f, ULONG ioctl = 0, void* buf = nullptr, ULONG inLen = 0, ULONG outLen = 0,
                         KPROCESSOR_MODE mode = UserMode, bool is32 = false, ULONG_PTR* info = nullptr, PDEVICE_OBJECT dev = nullptr)
{
    IRP irp{};
    irp.RequestorMode = mode; irp.Is32 = is32;
    irp.Stack.MajorFunction = major; irp.Stack.FileObject = f;
    irp.Stack.Parameters.DeviceIoControl.IoControlCode = ioctl;
    irp.Stack.Parameters.DeviceIoControl.InputBufferLength = inLen;
    irp.Stack.Parameters.DeviceIoControl.OutputBufferLength = outLen;
    irp.AssociatedIrp.SystemBuffer = buf;
    NTSTATUS st = g_drv.MajorFunction[major](dev ? dev : g_Vmic.CtlDevice, &irp);
    if (!dev) { CHECK(irp.Completed); CHECK(irp.IoStatus.Status == st); }
    if (info) *info = irp.IoStatus.Information;
    return st;
}

static NTSTATUS Attach(FILE_OBJECT* f, EXTER_ATTACH_RING_IN in, KPROCESSOR_MODE mode = UserMode, bool is32 = false, ULONG len = sizeof(EXTER_ATTACH_RING_IN))
{
    return Dispatch(IRP_MJ_DEVICE_CONTROL, f, IOCTL_EXTER_ATTACH_RING, &in, len, 0, mode, is32);
}

static void NoLeaks(const char* where)
{
    bool ok = g_shim.views == 0 && g_shim.mdls == 0 && g_shim.lockedMdls == 0 && g_shim.sectionRefs == 0;
    if (!ok) std::printf("  leak @ %s: views=%d mdls=%d locked=%d refs=%d\n", where, g_shim.views, g_shim.mdls, g_shim.lockedMdls, g_shim.sectionRefs);
    CHECK(ok);
}

struct Ring {
    void* mem; size_t bytes, sectionBytes; ex_u32 frames; HANDLE h;
    explicit Ring(ex_u32 fr = EXTER_RING_FRAMES, bool init = true, OBJECT_TYPE* t = &g_shimSectionObjType) : frames(fr) {
        bytes = ExterRing_TotalBytes(fr, EXTER_CHANNELS);
        sectionBytes = (bytes + 4095u) & ~(size_t)4095u;           // sama dengan ExterDriverLink.cpp
        mem = calloc(1, sectionBytes);
        if (init) ExterRing_Init(mem, fr);
        h = ShimRegister(t, mem, sectionBytes);
    }
    ~Ring() { for (auto& o : g_shimHandles) if (o.base == mem) o = {}; free(mem); }
    EXTER_RING_HEADER* hdr() { return (EXTER_RING_HEADER*)mem; }
    EXTER_ATTACH_RING_IN req(ex_u32 ringBytes = 0) { return { EXTER_ABI_VERSION, ringBytes ? ringBytes : (ex_u32)bytes, (ex_u64)(uintptr_t)h }; }
};

static void Produce(Ring& r, ex_i16 v, ex_u32 n) { std::vector<ex_i16> b(n, v); ExterRing_Write(r.hdr(), b.data(), n); }

static void TestInit()
{
    std::puts("init");
    CHECK(NT_SUCCESS(ExterVmic_Initialize(&g_drv)));
    CHECK(g_shim.devices == 1 && g_shim.symlinks == 1);
    CHECK(g_Vmic.CtlDevice && !(g_Vmic.CtlDevice->Flags & DO_DEVICE_INITIALIZING));
    CHECK(g_drv.MajorFunction[IRP_MJ_CREATE] && g_drv.MajorFunction[IRP_MJ_CLOSE] &&
          g_drv.MajorFunction[IRP_MJ_CLEANUP] && g_drv.MajorFunction[IRP_MJ_DEVICE_CONTROL]);
    CHECK(!g_Vmic.Attached);

    // IRP untuk device lain (device audio PortCls) harus diteruskan, BUKAN ditangani Exter.
    DEVICE_OBJECT other{}; IRP irp{}; irp.Stack.MajorFunction = IRP_MJ_DEVICE_CONTROL;
    int before = g_shim.pcDispatchCalls;
    g_drv.MajorFunction[IRP_MJ_DEVICE_CONTROL](&other, &irp);
    CHECK(g_shim.pcDispatchCalls == before + 1 && !irp.Completed);

    CHECK(NT_SUCCESS(Dispatch(IRP_MJ_CREATE, &g_fileA)));
    CHECK(NT_SUCCESS(Dispatch(IRP_MJ_CLOSE, &g_fileA)));
    // Major function lain tidak dibungkus Exter (di driver nyata tetap milik PortCls).
    CHECK(g_drv.MajorFunction[0x05] == nullptr);
}

static void TestAttachRejects()
{
    std::puts("attach: semua jalur gagal tanpa kebocoran");
    Ring good;
    CHECK(Dispatch(IRP_MJ_DEVICE_CONTROL, &g_fileA, 0xDEADBEEF) == STATUS_INVALID_DEVICE_REQUEST);
    CHECK(Attach(&g_fileA, good.req(), UserMode, false, sizeof(EXTER_ATTACH_RING_IN) - 1) == STATUS_INVALID_BUFFER_SIZE);
    CHECK(Attach(&g_fileA, good.req(), UserMode, false, sizeof(EXTER_ATTACH_RING_IN) + 8) == STATUS_INVALID_BUFFER_SIZE);
    CHECK(Attach(&g_fileA, good.req(), KernelMode) == STATUS_ACCESS_DENIED);
    CHECK(Attach(&g_fileA, good.req(), UserMode, true) == STATUS_ACCESS_DENIED);
    { auto r = good.req(); r.abiVersion = 99; CHECK(Attach(&g_fileA, r) == STATUS_REVISION_MISMATCH); }
    CHECK(Attach(&g_fileA, good.req(ExterRing_TotalBytes(EXTER_MIN_RING_FRAMES, 1) - 2)) == STATUS_INVALID_PARAMETER);
    CHECK(Attach(&g_fileA, good.req(EXTER_MAX_RING_BYTES + 4)) == STATUS_INVALID_PARAMETER);
    { auto r = good.req(); r.sectionHandle = 0xBAD; CHECK(Attach(&g_fileA, r) == STATUS_INVALID_HANDLE); }
    { Ring wrongType(EXTER_RING_FRAMES, true, &g_shimOtherObjType); CHECK(Attach(&g_fileA, wrongType.req()) == STATUS_OBJECT_TYPE_MISMATCH); }
    { // section lebih kecil dari ringBytes yang diklaim
      Ring small(EXTER_MIN_RING_FRAMES); auto r = small.req(); r.ringBytes = (ex_u32)good.bytes;
      CHECK(Attach(&g_fileA, r) == STATUS_INVALID_VIEW_SIZE); }
    { Ring blank(EXTER_RING_FRAMES, /*init=*/false); CHECK(Attach(&g_fileA, blank.req()) == STATUS_INVALID_PARAMETER); }
    { // header mengaku 32768 frame tetapi ringBytes hanya cukup untuk 8192: validasi memakai ukuran view, bukan klaim header
      Ring r2; CHECK(Attach(&g_fileA, r2.req(ExterRing_TotalBytes(8192, 1))) == STATUS_INVALID_PARAMETER); }
    { // header memuat nilai yang dirusak user-mode
      Ring bad; bad.hdr()->ringFrames = 12345 /* bukan pangkat 2 */; CHECK(Attach(&g_fileA, bad.req()) == STATUS_INVALID_PARAMETER); }
    NoLeaks("rejects");
    CHECK(!g_Vmic.Attached);
    CHECK(!ExAcquireRundownProtection(&g_Vmic.Rundown));       // tetap "tidak ada ring"
}

static ex_u32 ReadFrames(PVOID token, std::vector<ex_i16>& out, ex_u32 frames)
{
    out.assign(frames, 0x7777);
    return ExterVmic_FillCapture(token, out.data(), frames * 2);
}

static void TestStreaming()
{
    std::puts("attach sukses, satu pemilik, satu consumer, urutan data");
    Ring ring;
    CHECK(NT_SUCCESS(Attach(&g_fileA, ring.req())));
    CHECK(g_Vmic.Attached && g_shim.views == 1 && g_shim.lockedMdls == 1 && g_shim.sectionRefs == 1);

    { Ring other; CHECK(Attach(&g_fileB, other.req()) == STATUS_DEVICE_BUSY); }   // satu pemilik
    CHECK(g_shim.views == 1 && g_shim.mdls == 1 && g_shim.sectionRefs == 1);
    CHECK(Attach(&g_fileA, ring.req()) == STATUS_DEVICE_BUSY);                     // pemilik yang sama pun ditolak

    int T1, T2; std::vector<ex_i16> buf;
    // sebelum Run: senyap
    CHECK(ReadFrames(&T1, buf, 480) == 0); { bool z = true; for (auto s : buf) z &= (s == 0); CHECK(z); }

    ExterRing_SetProducerState(ring.hdr(), EXTER_PRODUCER_RUNNING);
    ExterVmic_StreamRun(&T1);
    CHECK(ring.hdr()->consumerState == EXTER_CONSUMER_OPEN);
    ExterVmic_StreamRun(&T2);                                                       // stream kedua tidak jadi pemilik
    CHECK(g_Vmic.StreamOwner == &T1);

    std::vector<ex_i16> ramp(9600); for (size_t i = 0; i < ramp.size(); ++i) ramp[i] = (ex_i16)(1 + (i % 30000));
    ExterRing_Write(ring.hdr(), ramp.data(), (ex_u32)ramp.size());

    std::vector<ex_i16> got;
    for (int i = 0; i < 20; ++i) { ReadFrames(&T1, buf, 480); got.insert(got.end(), buf.begin(), buf.end()); }
    size_t first = 0; while (first < got.size() && got[first] == 0) ++first;
    CHECK(first < got.size());
    // Ring berisi 9600 frame > high-water (4800): consumer membuang yang TERTUA sampai tersisa ~target, jadi audio
    // dimulai dari frame ke-(nilai-1), bukan frame 0. Yang harus berlaku: berurutan, tanpa lubang, tanpa duplikat.
    size_t k = first < got.size() ? (size_t)(got[first] - 1) : 0, run = 0; bool inOrder = true;
    for (size_t i = first; i < got.size() && got[i] != 0; ++i, ++k, ++run) inOrder &= (k < ramp.size() && got[i] == ramp[k]);
    CHECK(inOrder);
    CHECK(run >= 960);                                   // minimal sebanyak target (jitter buffer) keluar utuh
    CHECK(first <= 480);                                 // sudah primed: tidak ada senyap panjang di depan
    CHECK(k == ramp.size());                             // berakhir tepat di frame terakhir yang ditulis (yang terbaru dipertahankan)

    CHECK(ReadFrames(&T2, buf, 480) == 0); { bool z = true; for (auto s : buf) z &= (s == 0); CHECK(z); }   // non-pemilik: senyap
    ExterVmic_StreamStop(&T2);                                                      // bukan pemilik: tidak berpengaruh
    CHECK(g_Vmic.StreamOwner == &T1 && g_Vmic.StreamRunning == 1);

    EXTER_STATUS_OUT st{}; ULONG_PTR info = 0;
    CHECK(NT_SUCCESS(Dispatch(IRP_MJ_DEVICE_CONTROL, &g_fileB, IOCTL_EXTER_QUERY_STATUS, &st, 0, sizeof(st), UserMode, false, &info)));
    CHECK(info == sizeof(st) && st.attached == 1 && st.streamRunning == 1 && st.framesDelivered > 0 && st.abiVersion == EXTER_ABI_VERSION);
    { EXTER_STATUS_OUT tiny{}; CHECK(Dispatch(IRP_MJ_DEVICE_CONTROL, &g_fileB, IOCTL_EXTER_QUERY_STATUS, &tiny, 0, sizeof(tiny) - 1) == STATUS_BUFFER_TOO_SMALL); }

    ExterVmic_StreamStop(&T1);
    CHECK(ring.hdr()->consumerState == EXTER_CONSUMER_CLOSED && g_Vmic.StreamOwner == nullptr && g_Vmic.StreamRunning == 0);
    CHECK(ReadFrames(&T1, buf, 480) == 0);

    // Run lagi: audio lama TIDAK diputar ulang; hanya data baru.
    Produce(ring, 4242, 4800);                                                      // ditulis SAAT consumer berhenti
    ExterVmic_StreamRun(&T1);
    Produce(ring, 9999, 4800);
    got.clear(); for (int i = 0; i < 20; ++i) { ReadFrames(&T1, buf, 480); got.insert(got.end(), buf.begin(), buf.end()); }
    bool oldSeen = false, newSeen = false; for (auto s : got) { oldSeen |= (s == 4242); newSeen |= (s == 9999); }
    CHECK(!oldSeen && newSeen);
    ExterVmic_StreamStop(&T1);

    // detach oleh bukan pemilik ditolak; oleh pemilik berhasil & bersih
    CHECK(Dispatch(IRP_MJ_DEVICE_CONTROL, &g_fileB, IOCTL_EXTER_DETACH_RING) == STATUS_ACCESS_DENIED && g_Vmic.Attached);
    CHECK(NT_SUCCESS(Dispatch(IRP_MJ_DEVICE_CONTROL, &g_fileA, IOCTL_EXTER_DETACH_RING)));
    CHECK(!g_Vmic.Attached && ring.hdr()->consumerState == EXTER_CONSUMER_CLOSED);
    NoLeaks("detach");
    CHECK(!ExAcquireRundownProtection(&g_Vmic.Rundown));
    CHECK(NT_SUCCESS(Dispatch(IRP_MJ_DEVICE_CONTROL, &g_fileA, IOCTL_EXTER_DETACH_RING)));   // detach ganda aman
}

static void TestCleanupAndLateAttach()
{
    std::puts("cleanup file object (engine crash) & stream sudah berjalan sebelum attach");
    int T1; std::vector<ex_i16> buf;
    ExterVmic_StreamRun(&T1);                                   // Windows sudah menangkap, belum ada ring
    CHECK(g_Vmic.StreamOwner == &T1 && ReadFrames(&T1, buf, 480) == 0);

    Ring ring; ExterRing_SetProducerState(ring.hdr(), EXTER_PRODUCER_RUNNING);
    CHECK(NT_SUCCESS(Attach(&g_fileA, ring.req())));
    CHECK(ring.hdr()->consumerState == EXTER_CONSUMER_OPEN);    // attach langsung membuka consumer
    Produce(ring, 3131, 4800);
    bool seen = false; for (int i = 0; i < 20; ++i) { ReadFrames(&T1, buf, 480); for (auto s : buf) seen |= (s == 3131); }
    CHECK(seen);

    Dispatch(IRP_MJ_CLEANUP, &g_fileB);                         // file object lain: tidak berpengaruh
    CHECK(g_Vmic.Attached);
    Dispatch(IRP_MJ_CLEANUP, &g_fileA);                         // pemilik (proses crash)
    CHECK(!g_Vmic.Attached);
    NoLeaks("cleanup");
    CHECK(ReadFrames(&T1, buf, 480) == 0);
    ExterVmic_StreamStop(&T1);
}

static void TestConcurrentDetach()
{
    std::puts("pembaca berjalan saat attach/detach berulang (thread)");
    int T1; std::atomic<bool> stop{false}; std::atomic<long> bad{0}, reads{0};
    ExterVmic_StreamRun(&T1);

    std::thread reader([&] {
        std::vector<ex_i16> b(480);
        while (!stop) {
            ExterVmic_FillCapture(&T1, b.data(), 960);
            for (auto s : b) if (s != 0 && s != 0x1234) ++bad;         // hanya senyap atau data producer
            ++reads;
        }
    });

    for (int i = 0; i < 300; ++i) {
        Ring ring; ExterRing_SetProducerState(ring.hdr(), EXTER_PRODUCER_RUNNING);
        Produce(ring, 0x1234, 8000);
        if (!NT_SUCCESS(Attach(&g_fileA, ring.req()))) { ++bad; continue; }
        std::this_thread::yield();
        if (i % 2) Dispatch(IRP_MJ_DEVICE_CONTROL, &g_fileA, IOCTL_EXTER_DETACH_RING); else Dispatch(IRP_MJ_CLEANUP, &g_fileA);
        // setelah detach TIDAK boleh ada pembaca yang masih memegang ring: Ring (memori section) boleh dibebaskan
        CHECK(!g_Vmic.Attached);
    }
    stop = true; reader.join();
    CHECK(bad == 0);
    CHECK(reads > 0);
    NoLeaks("concurrent");
    ExterVmic_StreamStop(&T1);
}

static void TestShutdown()
{
    std::puts("shutdown");
    Ring ring; CHECK(NT_SUCCESS(Attach(&g_fileA, ring.req())));
    ExterVmic_Shutdown();                                        // harus melepas ring + hapus device & symlink
    CHECK(!g_Vmic.Attached && g_shim.devices == 0 && g_shim.symlinks == 0);
    NoLeaks("shutdown");
    ExterVmic_Shutdown();                                        // idempoten
}

int main()
{
    TestInit(); TestAttachRejects(); TestStreaming(); TestCleanupAndLateAttach(); TestConcurrentDetach(); TestShutdown();
    std::printf("\n%d pemeriksaan, %d gagal\n", g_checks, g_fail);
    return g_fail ? 1 : 0;
}
