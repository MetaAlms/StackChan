#pragma once
#include <array>
#include <atomic>
#include <cstddef>
#include <utility>

namespace webrtc_media {

// Exactly one producer and one consumer; owns each admitted payload.
// Full queues drop immediately. No mutex contention can discard an otherwise
// admissible audio frame, and neither side waits for the other.
template<typename T, size_t Capacity> class SpscQueue {
    static_assert(Capacity > 0);
public:
    bool Push(T&& item) {
        const size_t write = write_.load(std::memory_order_relaxed);
        const size_t next = (write + 1) % slots_.size();
        if (next == read_.load(std::memory_order_acquire)) return false;
        slots_[write] = std::move(item);
        write_.store(next, std::memory_order_release);
        return true;
    }
    bool Pop(T& item) {
        const size_t read = read_.load(std::memory_order_relaxed);
        if (read == write_.load(std::memory_order_acquire)) return false;
        item = std::move(slots_[read]);
        read_.store((read + 1) % slots_.size(), std::memory_order_release);
        return true;
    }
private:
    std::array<T, Capacity + 1> slots_{};
    std::atomic<size_t> read_{0};
    std::atomic<size_t> write_{0};
};
}  // namespace webrtc_media
