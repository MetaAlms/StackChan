#include "capture_lifecycle.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <mutex>
#include <thread>

namespace {
using webrtc_media::CaptureLifecycle;
std::atomic<unsigned> checks{0};

void Require(bool condition, const char* expression, int line)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::abort();
    }
}
#define CHECK(expression) Require(bool(expression), #expression, __LINE__)

// Handshakes make the tests independent of scheduler timing. A broken lock
// cannot leave the offline check waiting indefinitely.
class Signal {
public:
    void Set()
    {
        std::lock_guard<std::mutex> lock(mutex_);
        set_ = true;
        condition_.notify_all();
    }

    void Wait()
    {
        std::unique_lock<std::mutex> lock(mutex_);
        CHECK(condition_.wait_for(lock, std::chrono::seconds(3), [this] { return set_; }));
    }

private:
    std::mutex mutex_;
    std::condition_variable condition_;
    bool set_ = false;
};

void AdmissionAndGenerations()
{
    CaptureLifecycle capture;
    unsigned calls = 0;
    const uint64_t idle = capture.generation();
    CHECK(!capture.Read(idle, [&] { ++calls; return true; }));
    CHECK(calls == 0);
    capture.Access([&] { ++calls; });
    CHECK(calls == 1);
    CHECK(capture.Start([&] { ++calls; }));
    const uint64_t first = capture.generation();
    CHECK(first > idle);
    CHECK(!capture.Start([&] { ++calls; }));
    CHECK(capture.generation() == first && calls == 2);
    CHECK(capture.Read(first, [&] { ++calls; return true; }));
    CHECK(!capture.Read(first, [&] { ++calls; return false; }));
    CHECK(calls == 4);
    capture.Stop([&] { ++calls; });
    CHECK(capture.generation() > first);
    CHECK(!capture.Read(first, [&] { ++calls; return true; }));
    CHECK(!capture.Read(capture.generation(), [&] { ++calls; return true; }));
    CHECK(capture.Start([] {}));
    const uint64_t second = capture.generation();
    CHECK(second > first);
    CHECK(!capture.Read(first, [&] { ++calls; return true; }));
    CHECK(capture.Read(second, [&] { ++calls; return true; }));
    capture.Stop([] {});
    capture.Stop([&] { ++calls; });
    CHECK(!capture.Read(capture.generation(), [] { return true; }));
    CHECK(calls == 7);
}

void StopWaitsForRead()
{
    CaptureLifecycle capture;
    CHECK(capture.Start([] {}));
    const uint64_t token = capture.generation();
    Signal read_entered, release_read, stop_attempted;
    std::atomic<bool> reading{false}, read_returned{false}, stop_returned{false};
    std::thread reader([&] {
        CHECK(capture.Read(token, [&] {
            reading = true;
            read_entered.Set();
            release_read.Wait();
            reading = false;
            return true;
        }));
        read_returned = true;
    });
    read_entered.Wait();
    std::thread stopper([&] {
        stop_attempted.Set();
        capture.Stop([&] { CHECK(!reading.load()); });
        stop_returned = true;
    });
    stop_attempted.Wait();
    CHECK(!stop_returned.load());
    CHECK(capture.generation() == token);
    release_read.Set();
    reader.join();
    stopper.join();
    CHECK(read_returned.load() && stop_returned.load());
    CHECK(!capture.Read(token, [] { return true; }));
}

void OldPendingReadCannotEnterNewRun()
{
    CaptureLifecycle capture;
    CHECK(capture.Start([] {}));
    const uint64_t old = capture.generation();
    capture.Stop([] {});
    Signal init_entered, release_init, read_attempted;
    std::atomic<bool> old_read_returned{false};
    std::thread starter([&] {
        CHECK(capture.Start([&] {
            init_entered.Set();
            release_init.Wait();
        }));
    });
    init_entered.Wait();
    std::thread old_reader([&] {
        read_attempted.Set();
        CHECK(!capture.Read(old, [] { CHECK(false); return true; }));
        old_read_returned = true;
    });
    read_attempted.Wait();
    CHECK(!old_read_returned.load());
    release_init.Set();
    starter.join();
    old_reader.join();
    CHECK(old_read_returned.load());
    CHECK(capture.Read(capture.generation(), [] { return true; }));
    capture.Stop([] {});
}

// Access must not change hardware in the middle of an init, read or cleanup.
void AccessWaitsFor(const std::function<void(CaptureLifecycle&, const std::function<void()>&)>& hold)
{
    CaptureLifecycle capture;
    Signal holder_entered, release_holder, access_attempted;
    std::atomic<bool> holding{false}, accessed{false};
    std::thread holder([&] {
        hold(capture, [&] {
            holding = true;
            holder_entered.Set();
            release_holder.Wait();
            holding = false;
        });
    });
    holder_entered.Wait();
    std::thread accessor([&] {
        access_attempted.Set();
        capture.Access([&] {
            CHECK(!holding.load());
            accessed = true;
        });
    });
    access_attempted.Wait();
    CHECK(!accessed.load());
    release_holder.Set();
    holder.join();
    accessor.join();
    CHECK(accessed.load());
}

void HardwareAccessSerialization()
{
    AccessWaitsFor([](CaptureLifecycle& capture, const std::function<void()>& hold) {
        CHECK(capture.Start(hold));
    });
    AccessWaitsFor([](CaptureLifecycle& capture, const std::function<void()>& hold) {
        CHECK(capture.Start([] {}));
        CHECK(capture.Read(capture.generation(), [&] { hold(); return true; }));
    });
    AccessWaitsFor([](CaptureLifecycle& capture, const std::function<void()>& hold) {
        CHECK(capture.Start([] {}));
        capture.Stop(hold);
    });
}
}  // namespace

int main()
{
    AdmissionAndGenerations();
    StopWaitsForRead();
    OldPendingReadCannotEnterNewRun();
    HardwareAccessSerialization();
    std::printf("PASS capture lifecycle (%u checks)\n", checks.load());
}
