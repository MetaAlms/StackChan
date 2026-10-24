#include "session_run_control.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace {
using namespace webrtc_media;
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

void StartAndReady(SessionRunControl& run, const std::string& id = "test")
{
    CHECK(run.Start(id, RunMode::NearEnd, 10, 1000000));
    CHECK(run.Ready(id, 2000000));
}

void AuthorizationAndStates()
{
    SessionRunControl run;
    CHECK(run.state() == RunState::Idle);
    CHECK(!run.Ready("test", 0));
    CHECK(!run.Poll(100000000));
    CHECK(!run.Stop("test", StopReason::Requested));
    CHECK(!run.Finish("test", true));
    CHECK(!run.Reset(true));
    CHECK(!run.Start("", RunMode::NearEnd, 10, 0));
    CHECK(!run.Start(std::string(33, 'a'), RunMode::NearEnd, 10, 0));
    CHECK(!run.Start("test bad", RunMode::NearEnd, 10, 0));
    CHECK(!run.Start("test/dir", RunMode::NearEnd, 10, 0));
    CHECK(!run.Start("\xE4\xB8\xAD", RunMode::NearEnd, 10, 0));
    CHECK(!run.Start("test", RunMode::NearEnd, 4, 0));
    CHECK(!run.Start("test", RunMode::NearEnd, 181, 0));
    CHECK(!run.Start("test", static_cast<RunMode>(99), 10, 0));
    CHECK(!run.Start("test", RunMode::NearEnd, 10, -1));
    CHECK(run.state() == RunState::Idle);
    CHECK(run.Start("Run_1-A", RunMode::Duplex, 180, 100));
    CHECK(run.state() == RunState::Preparing);
    CHECK(run.run_id() == "Run_1-A" && run.mode() == RunMode::Duplex);
    CHECK(run.prepare_started_us() == 100 && run.run_started_us() == -1);
    CHECK(run.duration_seconds() == 180);
    CHECK(!run.Start("new", RunMode::NearEnd, 5, 101));
    CHECK(!run.Ready("wrong", 101));
    CHECK(!run.Ready("Run_1-A", 99));
    CHECK(!run.Finish("Run_1-A", true));
    CHECK(!run.Reset(true));
    CHECK(run.Ready("Run_1-A", 1000));
    CHECK(run.run_started_us() == 1000 && run.state() == RunState::Running);
    CHECK(!run.Ready("Run_1-A", 2000));
    CHECK(run.run_started_us() == 1000);
    CHECK(!run.Stop("wrong", StopReason::Requested));
    CHECK(!run.Stop("Run_1-A", StopReason::None));
    CHECK(run.Stop("Run_1-A", StopReason::Requested));
    CHECK(run.state() == RunState::Stopping);
    CHECK(!run.Stop("Run_1-A", StopReason::Error));
    CHECK(run.stop_reason() == StopReason::Requested);
    CHECK(!run.Start("new", RunMode::NearEnd, 5, 2000));
    CHECK(!run.Finish("wrong", true));
    CHECK(!run.Reset(true));
    CHECK(run.Finish("Run_1-A", true));
    CHECK(run.state() == RunState::Done);
    CHECK(!run.Start("new", RunMode::NearEnd, 5, 2000));
    CHECK(!run.Finish("Run_1-A", false));
    CHECK(!run.Reset(false));
    CHECK(run.Reset(true));
    CHECK(run.state() == RunState::Idle && run.run_id().empty());
    CHECK(run.prepare_started_us() == -1 && run.run_started_us() == -1);
}

void TimeoutBoundaries()
{
    SessionRunControl run;
    CHECK(run.Start("prepare", RunMode::NearEnd, 5, 1000000));
    CHECK(!run.Poll(999999));
    CHECK(!run.Poll(30999999));
    CHECK(run.Poll(31000000));
    CHECK(run.state() == RunState::Stopping && run.stop_reason() == StopReason::PrepareTimeout);
    CHECK(!run.Poll(32000000));
    CHECK(!run.Ready("prepare", 32000000));
    CHECK(run.Finish("prepare", false));
    CHECK(run.state() == RunState::Failed);
    CHECK(run.Reset(true));
    CHECK(run.Start("late-ready", RunMode::NearEnd, 5, 0));
    CHECK(!run.Ready("late-ready", 30000000));
    CHECK(run.stop_reason() == StopReason::PrepareTimeout);
    CHECK(run.Finish("late-ready", false));
    CHECK(run.Reset(true));
    CHECK(run.Start("run", RunMode::NearEnd, 5, 1000000));
    CHECK(run.Ready("run", 30000000));
    CHECK(!run.Poll(34999999));
    CHECK(run.state() == RunState::Running);
    CHECK(run.Poll(35000000));
    CHECK(run.stop_reason() == StopReason::RunTimeout);
    CHECK(run.run_started_us() == 30000000);
    CHECK(run.Finish("run", true));
    CHECK(!run.Poll(100000000));
    CHECK(run.Reset(true));
    CHECK(run.Start(std::string(32, 'Z'), RunMode::NearEnd, 5, 0));
    CHECK(run.Stop(std::string(32, 'Z'), StopReason::Error));
    CHECK(run.Finish(std::string(32, 'Z'), false));
    CHECK(run.state() == RunState::Failed);
}

void EvidenceBoundaries()
{
    SessionRunControl run;
    CHECK(run.ObserveAsr("test", "item", 7).status == AsrStatus::WrongRun);
    CHECK(run.Start("test", RunMode::NearEnd, 10, 0));
    CHECK(run.ObserveAsr("test", "item", 7).status == AsrStatus::NotRunning);
    CHECK(run.asr_items() == 0 && run.keyword_mask() == 0);
    CHECK(run.Ready("test", 1));
    CHECK(run.ObserveAsr("other", "item", 7).status == AsrStatus::WrongRun);
    CHECK(run.ObserveAsr("test", "", 7).status == AsrStatus::MissingItem);
    CHECK(run.asr_missing_item() == 1);
    CHECK(run.ObserveAsr("test", std::string(129, 'a'), 7).status == AsrStatus::InvalidItem);
    CHECK(run.asr_invalid_item() == 1);
    CHECK(run.asr_items() == 0 && !run.near_end_complete());
    auto observed = run.ObserveAsr("test", "ambiguous", 7);
    CHECK(observed.status == AsrStatus::Accepted && observed.ordinal == 1);
    CHECK(run.keyword_mask() != 7 && !run.near_end_complete());
    observed = run.ObserveAsr("test", "ambiguous", 7);
    CHECK(observed.status == AsrStatus::Duplicate && observed.ordinal == 1);
    CHECK(run.asr_items() == 1 && run.asr_duplicates() == 1);
    observed = run.ObserveAsr("test", "first-only", 1);
    CHECK(observed.status == AsrStatus::Accepted && observed.ordinal == 2);
    CHECK(run.keyword_mask() != 7 && !run.near_end_complete());
    observed = run.ObserveAsr("test", "second-only", 2);
    CHECK(observed.status == AsrStatus::Accepted && observed.ordinal == 3);
    CHECK(run.keyword_mask() == 7 && run.near_end_complete());
    CHECK(run.Stop("test", StopReason::Completed));
    CHECK(!run.near_end_complete());
    CHECK(run.ObserveAsr("test", "late", 7).status == AsrStatus::NotRunning);
    CHECK(run.asr_items() == 3);
    CHECK(run.Finish("test", true));
    CHECK(run.ObserveAsr("test", "late", 7).status == AsrStatus::NotRunning);
    CHECK(run.Reset(true));
    CHECK(run.asr_items() == 0 && run.keyword_mask() == 0 && run.asr_duplicates() == 0);
    CHECK(run.asr_missing_item() == 0 && run.asr_invalid_item() == 0);
    StartAndReady(run, "second");
    CHECK(run.ObserveAsr("test", "old", 7).status == AsrStatus::WrongRun);
    CHECK(run.ObserveAsr("second", "ambiguous", 4).ordinal == 1);
    CHECK(run.keyword_mask() == 4 && !run.near_end_complete());
}

void IndependentItemsAndOverflow()
{
    SessionRunControl run;
    StartAndReady(run);
    CHECK(run.ObserveAsr("test", "one", 1).status == AsrStatus::Accepted);
    CHECK(run.ObserveAsr("test", "one", 2).status == AsrStatus::Duplicate);
    CHECK(run.keyword_mask() == 1);
    CHECK(run.ObserveAsr("test", "two", 1).status == AsrStatus::Accepted);
    CHECK(run.ObserveAsr("test", "three", 1).status == AsrStatus::Accepted);
    CHECK(run.asr_items() == 3 && run.keyword_mask() == 1 && !run.near_end_complete());
    CHECK(run.ObserveAsr("test", "four", 2).status == AsrStatus::Accepted);
    CHECK(run.ObserveAsr("test", "five", 4).status == AsrStatus::Accepted);
    CHECK(run.near_end_complete());
    for (size_t i = 5; i < SessionRunControl::kMaxItems; ++i) {
        CHECK(run.ObserveAsr("test", "extra-" + std::to_string(i), 0).status == AsrStatus::Accepted);
    }
    CHECK(run.asr_items() == SessionRunControl::kMaxItems && run.near_end_complete());
    CHECK(run.ObserveAsr("test", "overflow", 7).status == AsrStatus::Overflow);
    CHECK(run.asr_overflow() == 1 && !run.near_end_complete());
    CHECK(run.ObserveAsr("test", "one", 7).status == AsrStatus::Duplicate);
    CHECK(run.asr_overflow() == 1 && !run.near_end_complete());
    CHECK(run.Stop("test", StopReason::Requested));
    CHECK(run.Finish("test", false));
    CHECK(run.Reset(true));
    CHECK(run.asr_overflow() == 0);
    CHECK(run.Start("duplex", RunMode::Duplex, 10, 0));
    CHECK(run.Ready("duplex", 0));
    CHECK(run.ObserveAsr("duplex", "a", 1).status == AsrStatus::Accepted);
    CHECK(run.ObserveAsr("duplex", "b", 2).status == AsrStatus::Accepted);
    CHECK(run.ObserveAsr("duplex", "c", 4).status == AsrStatus::Accepted);
    CHECK(run.keyword_mask() == 7 && !run.near_end_complete());
}
}  // namespace

int main()
{
    AuthorizationAndStates();
    TimeoutBoundaries();
    EvidenceBoundaries();
    IndependentItemsAndOverflow();
    std::printf("PASS: %u checks; explicit run lifecycle, independent item evidence and overflow isolation\n", checks);
}
