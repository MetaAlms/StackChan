#pragma once

#include <atomic>
#include <cstdint>
#include <limits>
#include <mutex>
#include <utility>

namespace webrtc_media {

// Persistent capture tasks keep this owner alive. A token identifies one
// enabled interval; stopping waits for its in-flight hardware read/feed to end.
// Callbacks execute under the mutex and must not reenter lifecycle methods.
class CaptureLifecycle {
public:
    uint64_t generation() const noexcept
    {
        return generation_.load(std::memory_order_acquire);
    }

    template<class Fn> bool Start(Fn&& init)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (enabled_ || generation() == std::numeric_limits<uint64_t>::max()) return false;
        AdvanceGeneration();
        std::forward<Fn>(init)();
        enabled_ = true;
        return true;
    }

    template<class Fn> void Stop(Fn&& cleanup)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        enabled_ = false;
        AdvanceGeneration();
        std::forward<Fn>(cleanup)();
    }

    template<class Fn> bool Read(uint64_t expected_generation, Fn&& input)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!enabled_ || generation() != expected_generation) return false;
        return std::forward<Fn>(input)();
    }

    // Hardware power changes also need exclusion, including while disabled.
    template<class Fn> void Access(Fn&& action)
    {
        std::lock_guard<std::mutex> lock(mutex_);
        std::forward<Fn>(action)();
    }

private:
    void AdvanceGeneration()
    {
        const uint64_t current = generation();
        // Never wrap an old token back into validity. Exhaustion permanently
        // disables future starts; Stop and hardware cleanup remain available.
        if (current != std::numeric_limits<uint64_t>::max()) {
            generation_.store(current + 1, std::memory_order_release);
        }
    }

    std::mutex mutex_;
    std::atomic<uint64_t> generation_{0};
    bool enabled_ = false;
};

}  // namespace webrtc_media
