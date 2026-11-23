#pragma once
/*
 * EXTER VIRTUAL MICROPHONE - audio transport (shared-memory ring buffer)
 *
 * Header ini dipakai BERSAMA oleh:
 *   - user-mode  : Exter.AudioEngine (producer / penulis)
 *   - kernel-mode: Exter.VirtualMic driver (consumer / pembaca), definisikan EXTER_KERNEL_MODE
 *   - test       : Exter.Tests (g++/MSVC, tanpa Windows)
 *
 * Aturan: C murni yang aman untuk kernel (tanpa CRT, tanpa float, tanpa alokasi, tanpa lock).
 *
 * Format transport (tetap): PCM int16 little-endian, 48000 Hz, mono.
 * Satu producer (Exter engine), satu consumer (driver) -> SPSC lock-free.
 *
 * Kepemilikan field (mencegah dua penulis pada satu variabel):
 *   producer menulis: writePos, producerState, isi ring data
 *   consumer menulis: readPos, consumerState, penghitung diagnostik (underruns, ...)
 * Producer TIDAK PERNAH membaca readPos dan tidak pernah menunggu consumer:
 *   consumer yang bertanggung jawab atas latency (membuang data lama) dan mendeteksi data
 *   yang tertimpa (torn read) -> menghasilkan senyap, bukan suara rusak.
 */

#if defined(EXTER_KERNEL_MODE)
  #include <ntddk.h>
  typedef INT16  ex_i16;  typedef UINT8  ex_u8;
  typedef UINT32 ex_u32;  typedef UINT64 ex_u64;
  typedef SIZE_T ex_size;
  #define EX_MEMCPY(d,s,n) RtlCopyMemory((d),(s),(n))
  #define EX_MEMZERO(d,n)  RtlZeroMemory((d),(n))
#else
  #include <stdint.h>
  #include <stddef.h>
  #include <string.h>
  typedef int16_t  ex_i16;  typedef uint8_t  ex_u8;
  typedef uint32_t ex_u32;  typedef uint64_t ex_u64;
  typedef size_t   ex_size;
  #define EX_MEMCPY(d,s,n) memcpy((d),(s),(n))
  #define EX_MEMZERO(d,n)  memset((d),0,(n))
#endif

#if defined(__cplusplus)
  #define EX_INLINE inline
#else
  #define EX_INLINE static __inline
#endif

/* ---- atomik acquire/release -------------------------------------------------------------
 * Hanya x64/ARM64 (x86 32-bit tidak didukung: akses 64-bit tidak atomik).
 * MSVC x64 (TSO): acquire-load / release-store = akses volatile + barrier compiler.
 * MSVC ARM64: __ldar64/__stlr64 (+32-bit). GCC/Clang: builtin __atomic. Tidak butuh windows.h. */
#if defined(_MSC_VER)
  #include <intrin.h>
  #if defined(_M_IX86)
    #error "EXTER transport: x86 32-bit tidak didukung"
  #elif defined(_M_X64)
    EX_INLINE ex_u64 ex_load64_acq(const volatile ex_u64* p) { ex_u64 v = *p; _ReadWriteBarrier(); return v; }
    EX_INLINE void   ex_store64_rel(volatile ex_u64* p, ex_u64 v) { _ReadWriteBarrier(); *p = v; }
    EX_INLINE ex_u32 ex_load32_acq(const volatile ex_u32* p) { ex_u32 v = *p; _ReadWriteBarrier(); return v; }
    EX_INLINE void   ex_store32_rel(volatile ex_u32* p, ex_u32 v) { _ReadWriteBarrier(); *p = v; }
  #elif defined(_M_ARM64)
    EX_INLINE ex_u64 ex_load64_acq(const volatile ex_u64* p) { return __ldar64((volatile unsigned __int64*)p); }
    EX_INLINE void   ex_store64_rel(volatile ex_u64* p, ex_u64 v) { __stlr64((volatile unsigned __int64*)p, v); }
    EX_INLINE ex_u32 ex_load32_acq(const volatile ex_u32* p) { return __ldar32((volatile unsigned __int32*)p); }
    EX_INLINE void   ex_store32_rel(volatile ex_u32* p, ex_u32 v) { __stlr32((volatile unsigned __int32*)p, v); }
  #else
    #error "EXTER transport: arsitektur tidak didukung"
  #endif
  #define EX_LOAD64_ACQ(p)     ex_load64_acq(p)
  #define EX_STORE64_REL(p,v)  ex_store64_rel((p), (v))
  #define EX_LOAD32_ACQ(p)     ex_load32_acq(p)
  #define EX_STORE32_REL(p,v)  ex_store32_rel((p), (v))
#else
  #define EX_LOAD64_ACQ(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
  #define EX_STORE64_REL(p,v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)
  #define EX_LOAD32_ACQ(p)     __atomic_load_n((p), __ATOMIC_ACQUIRE)
  #define EX_STORE32_REL(p,v)  __atomic_store_n((p), (v), __ATOMIC_RELEASE)
#endif

/* ---- konstanta -------------------------------------------------------------------------- */
#define EXTER_RING_MAGIC        0x52545845u   /* 'EXTR' little-endian */
#define EXTER_ABI_VERSION       1u
#define EXTER_SAMPLE_RATE       48000u
#define EXTER_CHANNELS          1u
#define EXTER_RING_FRAMES       32768u        /* pangkat 2; ~683 ms @ 48 kHz */
#define EXTER_RING_HEADER_BYTES 256u

#define EXTER_PRODUCER_STOPPED  0u
#define EXTER_PRODUCER_RUNNING  1u
#define EXTER_CONSUMER_CLOSED   0u
#define EXTER_CONSUMER_OPEN     1u            /* ada aplikasi Windows yang sedang membaca mic */

/* Default consumer (frame @48 kHz) */
#define EXTER_DEFAULT_TARGET_FRAMES   960u    /* 20 ms prefill (jitter buffer consumer) */
#define EXTER_DEFAULT_HIGHWATER_FRAMES 4800u  /* 100 ms: lebih dari ini -> buang yang tertua */
#define EXTER_DEFAULT_STALE_FRAMES    24000u  /* 500 ms tanpa writePos bergerak -> producer dianggap mati */

#pragma pack(push, 8)
typedef struct _EXTER_RING_HEADER {
    /* --- statis, ditulis sekali oleh producer saat init (offset 0) --- */
    ex_u32 magic;
    ex_u32 abiVersion;
    ex_u32 sampleRate;
    ex_u32 channels;
    ex_u32 ringFrames;
    ex_u32 frameBytes;
    ex_u32 headerBytes;
    ex_u32 reserved0;
    ex_u8  pad0[32];
    /* --- blok producer (offset 64) --- */
    volatile ex_u64 writePos;          /* total frame yang pernah ditulis (monoton) */
    volatile ex_u32 producerState;
    ex_u32 reserved1;
    ex_u8  pad1[48];
    /* --- blok consumer (offset 128) --- */
    volatile ex_u64 readPos;           /* total frame yang pernah dibaca/dibuang */
    volatile ex_u32 consumerState;
    ex_u32 reserved2;
    volatile ex_u64 underruns;         /* consumer kehabisan data setelah primed */
    volatile ex_u64 overruns;          /* producer menyalip consumer (data hilang) */
    volatile ex_u64 latencyDrops;      /* consumer membuang data lama demi latency */
    volatile ex_u64 tornReads;         /* data tertimpa saat dibaca -> senyap */
    volatile ex_u64 staleEvents;       /* producer berhenti menulis (crash/hang) */
    ex_u8  pad2[8];
    /* --- sisa sampai 256 byte --- */
    ex_u8  pad3[64];
} EXTER_RING_HEADER;
#pragma pack(pop)

typedef char ex_assert_header_size[(sizeof(EXTER_RING_HEADER) == EXTER_RING_HEADER_BYTES) ? 1 : -1];

/* data sample (ex_i16 x ringFrames x channels) berada tepat setelah header */
#define EXTER_RING_DATA(h) ((ex_i16*)((ex_u8*)(h) + EXTER_RING_HEADER_BYTES))

EX_INLINE ex_u32 ExterRing_TotalBytes(ex_u32 ringFrames, ex_u32 channels)
{
    return EXTER_RING_HEADER_BYTES + ringFrames * channels * (ex_u32)sizeof(ex_i16);
}

/* ====================== PRODUCER ======================================================== */

/* mem harus nol-terinisialisasi sepanjang ExterRing_TotalBytes (CreateFileMapping memberi halaman nol). */
EX_INLINE void ExterRing_Init(void* mem, ex_u32 ringFrames)
{
    EXTER_RING_HEADER* h = (EXTER_RING_HEADER*)mem;
    h->abiVersion = EXTER_ABI_VERSION;
    h->sampleRate = EXTER_SAMPLE_RATE;
    h->channels   = EXTER_CHANNELS;
    h->ringFrames = ringFrames;
    h->frameBytes = EXTER_CHANNELS * (ex_u32)sizeof(ex_i16);
    h->headerBytes = EXTER_RING_HEADER_BYTES;
    EX_STORE64_REL(&h->writePos, 0);
    EX_STORE32_REL(&h->producerState, EXTER_PRODUCER_STOPPED);
    EX_STORE32_REL(&h->magic, EXTER_RING_MAGIC);   /* terakhir: tanda header valid */
}

EX_INLINE void ExterRing_SetProducerState(EXTER_RING_HEADER* h, ex_u32 state)
{
    EX_STORE32_REL(&h->producerState, state);
}

/* Menulis `frames` frame (mono int16). Tidak pernah memblokir; selalu menimpa data tertua. */
EX_INLINE void ExterRing_Write(EXTER_RING_HEADER* h, const ex_i16* src, ex_u32 frames)
{
    const ex_u32 cap = h->ringFrames, mask = cap - 1u;
    ex_i16* data = EXTER_RING_DATA(h);
    ex_u64 w = h->writePos;                       /* hanya producer yang menulis -> baca polos */
    const ex_u32 chunkMax = cap / 4u;             /* chunk kecil: jendela torn-read tetap sempit */

    while (frames > 0) {
        ex_u32 n = frames < chunkMax ? frames : chunkMax;
        ex_u32 idx = (ex_u32)(w & mask);
        ex_u32 first = cap - idx; if (first > n) first = n;
        EX_MEMCPY(data + idx, src, first * sizeof(ex_i16));
        if (n > first) EX_MEMCPY(data, src + first, (n - first) * sizeof(ex_i16));
        w += n; src += n; frames -= n;
        EX_STORE64_REL(&h->writePos, w);          /* publikasikan setelah data tertulis */
    }
}

/* ====================== CONSUMER (driver) =============================================== */

/* SEMUA nilai yang menentukan batas memori ada di sini (memori privat consumer), disalin dari header
   SETELAH ExterRing_Validate. Header berada di memori yang bisa ditulis proses user-mode kapan saja,
   jadi consumer tidak boleh membaca ringFrames/readPos dari sana lagi (cegah baca di luar batas). */
typedef struct _EXTER_CONSUMER {
    EXTER_RING_HEADER* h;
    ex_i16* data;            /* = EXTER_RING_DATA(h), dihitung sekali */
    ex_u32 ringFrames;       /* tervalidasi: pangkat 2 */
    ex_u32 pad;
    ex_u64 readPos;          /* kebenaran readPos; header->readPos hanya salinan diagnostik */
    ex_u32 targetFrames;
    ex_u32 highWaterFrames;
    ex_u32 staleLimitFrames;
    ex_u32 primed;
    ex_u32 staleFrames;
    ex_u32 reserved;
    ex_u64 lastWritePos;
} EXTER_CONSUMER;

/* Validasi memori yang dipetakan dari user-mode. TIDAK PERNAH percaya isi header:
   memori ini ditulis oleh proses user-mode. Mengembalikan 1 bila aman dipakai. */
EX_INLINE int ExterRing_Validate(const void* mem, ex_size bytes)
{
    const EXTER_RING_HEADER* h = (const EXTER_RING_HEADER*)mem;
    ex_u32 cap;
    if (!mem || bytes < EXTER_RING_HEADER_BYTES) return 0;
    if (h->magic != EXTER_RING_MAGIC || h->abiVersion != EXTER_ABI_VERSION) return 0;
    if (h->sampleRate != EXTER_SAMPLE_RATE || h->channels != EXTER_CHANNELS) return 0;
    if (h->headerBytes != EXTER_RING_HEADER_BYTES) return 0;
    if (h->frameBytes != EXTER_CHANNELS * (ex_u32)sizeof(ex_i16)) return 0;
    cap = h->ringFrames;
    if (cap < 4096u || cap > (1u << 20) || (cap & (cap - 1u)) != 0) return 0;   /* pangkat 2 */
    if (bytes < (ex_size)ExterRing_TotalBytes(cap, h->channels)) return 0;
    return 1;
}

EX_INLINE void ExterConsumer_Init(EXTER_CONSUMER* c, EXTER_RING_HEADER* h,
                                  ex_u32 targetFrames, ex_u32 highWaterFrames, ex_u32 staleLimitFrames)
{
    c->h = h;
    c->data = EXTER_RING_DATA(h);
    c->ringFrames = h->ringFrames;       /* panggil hanya setelah ExterRing_Validate(h) sukses */
    c->pad = 0;
    c->targetFrames = targetFrames;
    c->highWaterFrames = highWaterFrames;
    c->staleLimitFrames = staleLimitFrames;
    c->primed = 0;
    c->staleFrames = 0;
    c->reserved = 0;
    c->lastWritePos = EX_LOAD64_ACQ(&h->writePos);
    c->readPos = c->lastWritePos;
    EX_STORE64_REL(&h->readPos, c->readPos);
}

/* Dipanggil driver saat stream capture Windows RUN (open=1) / STOP,PAUSE (open=0).
   Saat dibuka, mulai dari posisi tulis terkini: tidak memutar ulang audio lama. */
EX_INLINE void ExterConsumer_SetOpen(EXTER_CONSUMER* c, int open)
{
    ex_u64 w = EX_LOAD64_ACQ(&c->h->writePos);
    c->primed = 0;
    c->staleFrames = 0;
    c->lastWritePos = w;
    c->readPos = w;
    EX_STORE64_REL(&c->h->readPos, w);
    EX_STORE32_REL(&c->h->consumerState, open ? EXTER_CONSUMER_OPEN : EXTER_CONSUMER_CLOSED);
}

/* Mengisi dst dengan `frames` frame. Selalu mengisi PENUH (sisa = nol/senyap).
   Mengembalikan jumlah frame yang berisi audio nyata. Aman dipanggil di IRQL <= DISPATCH_LEVEL. */
EX_INLINE ex_u32 ExterConsumer_Read(EXTER_CONSUMER* c, ex_i16* dst, ex_u32 frames)
{
    EXTER_RING_HEADER* h = c->h;
    const ex_u32 cap = c->ringFrames, mask = cap - 1u;   /* privat, bukan dari header */
    const ex_i16* data = c->data;
    ex_u64 w = EX_LOAD64_ACQ(&h->writePos);               /* nilai tak tepercaya: dipakai hanya lewat & mask */
    ex_u64 r = c->readPos;
    ex_u64 fill, w2;
    ex_u32 avail, idx, first;

    if (frames == 0) return 0;
    if (frames > cap / 4u) {                        /* permintaan tak wajar: pecah demi keamanan */
        ex_u32 got = 0, part = cap / 4u;
        while (frames > 0) {
            ex_u32 n = frames < part ? frames : part;
            got += ExterConsumer_Read(c, dst, n);
            dst += n; frames -= n;
        }
        return got;
    }

    /* deteksi producer mati: writePos tidak bergerak selama staleLimitFrames yang diminta consumer */
    {
        int wasStale = (c->staleFrames > c->staleLimitFrames);
        if (w != c->lastWritePos) { c->lastWritePos = w; c->staleFrames = 0; }
        else if (c->staleFrames < 0xFFFFFFFFu - frames) c->staleFrames += frames;
        if (!wasStale && c->staleFrames > c->staleLimitFrames &&
            EX_LOAD32_ACQ(&h->producerState) == EXTER_PRODUCER_RUNNING)
            h->staleEvents++;
    }

    if (EX_LOAD32_ACQ(&h->producerState) != EXTER_PRODUCER_RUNNING || c->staleFrames > c->staleLimitFrames) {
        EX_MEMZERO(dst, frames * sizeof(ex_i16));
        c->primed = 0;
        c->readPos = w;
        EX_STORE64_REL(&h->readPos, w);
        return 0;
    }

    fill = w - r;
    if (fill > cap) {                               /* producer menyalip consumer: data tidak bisa dipercaya */
        h->overruns++;
        r = w - c->targetFrames;
        fill = c->targetFrames;
    }

    if (!c->primed) {
        if (fill >= c->targetFrames) c->primed = 1;
        else { EX_MEMZERO(dst, frames * sizeof(ex_i16)); c->readPos = r; EX_STORE64_REL(&h->readPos, r); return 0; }
    }

    if (fill > c->highWaterFrames) {                /* latency terlalu besar (clock drift) -> buang tertua */
        ex_u64 keep = (ex_u64)c->targetFrames + frames;
        r = w - keep;
        fill = keep;
        h->latencyDrops++;
    }

    avail = (fill < frames) ? (ex_u32)fill : frames;
    idx = (ex_u32)(r & mask);
    first = cap - idx; if (first > avail) first = avail;
    EX_MEMCPY(dst, data + idx, first * sizeof(ex_i16));
    if (avail > first) EX_MEMCPY(dst + first, data, (avail - first) * sizeof(ex_i16));

    /* torn-read check: apakah producer sudah menimpa awal wilayah yang barusan kita salin? */
    w2 = EX_LOAD64_ACQ(&h->writePos);
    if (w2 - r > cap) {
        h->tornReads++;
        EX_MEMZERO(dst, frames * sizeof(ex_i16));
        c->readPos = w2 - c->targetFrames;
        EX_STORE64_REL(&h->readPos, c->readPos);
        c->primed = 0;
        return 0;
    }

    r += avail;
    c->readPos = r;
    EX_STORE64_REL(&h->readPos, r);

    if (avail < frames) {                           /* underrun: isi senyap, isi ulang jitter buffer */
        EX_MEMZERO(dst + avail, (frames - avail) * sizeof(ex_i16));
        h->underruns++;
        c->primed = 0;
    }
    return avail;
}
