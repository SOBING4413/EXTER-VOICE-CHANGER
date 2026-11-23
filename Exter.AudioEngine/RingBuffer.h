#pragma once
// Single-producer / single-consumer lock-free ring buffer (float).
#include <atomic>
#include <vector>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <algorithm>

class RingBuffer {
public:
    explicit RingBuffer(size_t capacityPow2) : buf_(capacityPow2), mask_(capacityPow2 - 1) {}

    size_t Available() const { return (size_t)(w_.load(std::memory_order_acquire) - r_.load(std::memory_order_acquire)); }

    // Producer. Mengembalikan jumlah sample yang tertulis (sisanya dibuang bila penuh).
    size_t Write(const float* src, size_t n) {
        uint64_t w = w_.load(std::memory_order_relaxed);
        uint64_t r = r_.load(std::memory_order_acquire);
        size_t freeSpace = buf_.size() - (size_t)(w - r);
        n = std::min(n, freeSpace);
        for (size_t i = 0; i < n; ++i) buf_[(w + i) & mask_] = src[i];
        w_.store(w + n, std::memory_order_release);
        return n;
    }

    // Consumer.
    size_t Read(float* dst, size_t n) {
        uint64_t r = r_.load(std::memory_order_relaxed);
        uint64_t w = w_.load(std::memory_order_acquire);
        n = std::min(n, (size_t)(w - r));
        for (size_t i = 0; i < n; ++i) dst[i] = buf_[(r + i) & mask_];
        r_.store(r + n, std::memory_order_release);
        return n;
    }

    // Consumer: buang n sample tertua (dipakai untuk koreksi drift clock).
    void Skip(size_t n) {
        uint64_t r = r_.load(std::memory_order_relaxed);
        uint64_t w = w_.load(std::memory_order_acquire);
        n = std::min(n, (size_t)(w - r));
        r_.store(r + n, std::memory_order_release);
    }

    // Hanya dipanggil saat kedua thread berhenti.
    void Clear() { r_.store(w_.load()); }

private:
    std::vector<float> buf_;
    size_t mask_;
    std::atomic<uint64_t> w_{0}, r_{0};
};
