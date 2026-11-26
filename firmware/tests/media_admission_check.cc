#include "media_admission.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <thread>

namespace {
using webrtc_media::MediaAdmission;
unsigned checks = 0;
void Require(bool condition, const char* expression, int line)
{
    ++checks;
    if (!condition) {
        std::fprintf(stderr, "FAIL line %d: %s\n", line, expression);
        std::abort();
    }
}
#define CHECK(expression) Require(bool(expression), #expression, __LINE__)

void InitialAndOpen()
{
    MediaAdmission gate;
    CHECK(!gate.CanAccept());
    CHECK(gate.Open());
    CHECK(gate.CanAccept());
    CHECK(!gate.Open());  // Open is a single transition, not a reset operation.
    CHECK(gate.CanAccept());
}

void CancellationIsPermanent()
{
    MediaAdmission cancelled_before_ready;
    cancelled_before_ready.Cancel();
    CHECK(!cancelled_before_ready.CanAccept());
    CHECK(!cancelled_before_ready.Open());
    cancelled_before_ready.Cancel();
    CHECK(!cancelled_before_ready.Open());
    CHECK(!cancelled_before_ready.CanAccept());

    MediaAdmission cancelled_after_ready;
    CHECK(cancelled_after_ready.Open());
    cancelled_after_ready.Cancel();
    CHECK(!cancelled_after_ready.CanAccept());
    CHECK(!cancelled_after_ready.Open());
    cancelled_after_ready.Cancel();
    CHECK(!cancelled_after_ready.CanAccept());

    MediaAdmission separate_run;
    CHECK(!separate_run.CanAccept());
    CHECK(separate_run.Open());
    CHECK(separate_run.CanAccept());
    CHECK(!cancelled_before_ready.CanAccept());
    CHECK(!cancelled_after_ready.CanAccept());
}

void ReadinessAndStopRace()
{
    // Two threads race the one allowed Open with permanent cancellation.
    // Neither permitted interleaving may reopen after cancellation returns.
    for (unsigned iteration = 0; iteration < 128; ++iteration) {
        MediaAdmission gate;
        std::atomic<bool> begin{false}, cancelled{false};
        std::atomic<unsigned> errors{0}, successful_opens{0};
        std::thread ready([&] {
            while (!begin.load()) std::this_thread::yield();
            if (gate.Open()) ++successful_opens;
            while (!cancelled.load()) {
                if (gate.Open()) ++successful_opens;
            }
            for (unsigned n = 0; n < 100; ++n) {
                if (gate.Open() || gate.CanAccept()) ++errors;
            }
        });
        std::thread stop([&] {
            while (!begin.load()) std::this_thread::yield();
            gate.Cancel();
            if (gate.CanAccept()) ++errors;
            cancelled = true;
            for (unsigned n = 0; n < 100; ++n) {
                gate.Cancel();
                if (gate.Open() || gate.CanAccept()) ++errors;
            }
        });
        begin = true;
        ready.join();
        stop.join();
        CHECK(errors.load() == 0);
        CHECK(successful_opens.load() <= 1);
        CHECK(!gate.CanAccept());
        CHECK(!gate.Open());
    }
}
}  // namespace

int main()
{
    InitialAndOpen();
    CancellationIsPermanent();
    ReadinessAndStopRace();
    std::printf("media_admission_check: PASS (%u assertions)\n", checks);
}
