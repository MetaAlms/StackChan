/**
 * @brief Offline behavioural check for the U1-S short comparison mode.
 *
 * This is the *actual* source under test: the harness includes the same
 * `firmware/main/hal/webrtc/m1_short_plan.h` that the device firmware compiles,
 * and drives the same `m1_short::Sequencer` the sender task drives.
 *
 * What it establishes, per the U1 task's counterexample requirement:
 *   - success, no-ASR and every send-failure path take the zh_1 short path only;
 *   - no path reaches the sustained resource loop (the sequencer has no loop
 *     step at all, so this is structural rather than a flag being respected);
 *   - every terminal path settles the window exactly once after the media
 *     sequence, so cleanup/join cannot be skipped;
 *   - leading-silence writes can never be reported as speech evidence, even
 *     when the silence interval is full of successful writes;
 *   - interval accounting is a pure subtraction of two snapshots, so no
 *     interval can be produced by restarting the window mid-send.
 *
 * Scope limits, stated rather than implied: this is a C++ compile/run check of
 * the plan/state machine and its interval arithmetic ONLY. It does not run the
 * device, the transport, the codec, the real probe wrappers, the media clock or
 * the task join - a model test cannot detect a missing `g_clock.Arm()` or a
 * mis-placed `SettleWindow()`, and it does not claim to. Those wiring facts are
 * asserted against the real source by `firmware/tests/check_u1_short_wiring.py`.
 * Compilation is done by a subprocess (outside the controller); execution runs
 * as a pre-built artefact through the ledger.
 */

#include "m1_short_plan.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

using m1_short::Event;
using m1_short::Interval;
using m1_short::Outcome;
using m1_short::Step;

// ---------------------------------------------------------------------------
// Frozen-contract assertions: these values are part of what both arms share.
// Changing one silently would invalidate the 1.5.5 / 1.5.7 comparison.
// ---------------------------------------------------------------------------

static_assert(m1_short::kLeadingSilenceMs == 2000, "frozen: 2 s leading silence");
static_assert(m1_short::kTailSilenceMs == 800, "frozen: 800 ms tail");
static_assert(m1_short::kAsrTimeoutMs == 20000, "frozen: bounded ASR wait");
static_assert(m1_short::kRequiredFrameMs == 20, "frozen: 20 ms frames");
static_assert(m1_short::kClipId[0] == 'z' && m1_short::kClipId[1] == 'h' &&
                  m1_short::kClipId[2] == '_' && m1_short::kClipId[3] == '1' &&
                  m1_short::kClipId[4] == '\0',
              "frozen: the clip is zh_1");

namespace {

int g_failures = 0;
int g_checks = 0;

void Check(bool ok, const char* what, const char* file, int line)
{
    ++g_checks;
    if (!ok) {
        ++g_failures;
        std::printf("  FAIL %s:%d: %s\n", file, line, what);
    }
}

#define CHECK(cond) Check((cond), #cond, __FILE__, __LINE__)
#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        const long long va_ = (long long)(a);                                \
        const long long vb_ = (long long)(b);                                \
        ++g_checks;                                                          \
        if (va_ != vb_) {                                                    \
            ++g_failures;                                                    \
            std::printf("  FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__,  \
                        __LINE__, #a, #b, va_, vb_);                         \
        }                                                                    \
    } while (0)

struct Trace {
    std::vector<Step> steps;
    Outcome outcome = Outcome::None;
    int clip_entered = 0;
    int settle_done = 0;
};

/** Drive the real sequencer with the scripted result of each step. */
Trace Drive(const std::vector<Event>& events)
{
    m1_short::Sequencer seq;
    Trace t;
    size_t i = 0;
    while (seq.current() != Step::Done && t.steps.size() < 16) {
        t.steps.push_back(seq.current());
        const Event e = (i < events.size()) ? events[i] : Event::Ok;
        ++i;
        seq.Advance(e);
    }
    t.outcome = seq.outcome();
    t.clip_entered = seq.clip_entered();
    t.settle_done = seq.settle_done();
    return t;
}

/** Every path must be short, settled exactly once, and finished. */
void CheckShortInvariants(const Trace& t, const char* label)
{
    CHECK_EQ(t.clip_entered <= 1, 1);          // never a second clip
    CHECK_EQ(t.settle_done, 1);                // window settled exactly once
    CHECK(t.steps.back() == Step::Settle);     // ...and it is the last real step
    CHECK(t.steps.size() >= 2);
    // The only steps that may ever appear. A loop step does not exist in the
    // enum, so any future addition shows up here as a failure.
    for (Step s : t.steps) {
        const bool known = s == Step::LeadingSilence || s == Step::PlayClip ||
                           s == Step::Tail || s == Step::AsrWait || s == Step::Settle;
        Check(known, label, __FILE__, __LINE__);
    }
    // The clip, when entered, is always entered after the leading silence and
    // never more than once.
    int seen_clip = 0;
    for (Step s : t.steps) {
        if (s == Step::PlayClip) { ++seen_clip; }
    }
    CHECK_EQ(seen_clip, t.clip_entered);
}

void CheckTraceEquals(const Trace& t, const std::vector<Step>& want, const char* label)
{
    bool same = t.steps.size() == want.size();
    if (same) {
        for (size_t i = 0; i < want.size(); ++i) {
            if (t.steps[i] != want[i]) { same = false; break; }
        }
    }
    ++g_checks;
    if (!same) {
        ++g_failures;
        std::printf("  FAIL %s: unexpected step trace (", label);
        for (Step s : t.steps) { std::printf("%s ", m1_short::StepName(s)); }
        std::printf(")\n");
    }
}

// ---------------------------------------------------------------------------

/** 1. Success takes exactly the frozen short sequence. */
void ScenarioSuccess()
{
    std::printf("[1] success: silence -> zh_1 -> tail -> asr-ok -> settle -> done\n");
    const Trace t = Drive({Event::Ok, Event::Ok, Event::Ok, Event::Ok, Event::Ok});
    CheckTraceEquals(t, {Step::LeadingSilence, Step::PlayClip, Step::Tail, Step::AsrWait,
                         Step::Settle},
                     "success");
    CHECK_EQ(t.outcome, (int)Outcome::MediaCompleted);
    CHECK_EQ(t.clip_entered, 1);
    CheckShortInvariants(t, "success");
}

/** 2. A bounded ASR timeout ends the run; it does not fall through to more clips. */
void ScenarioNoAsr()
{
    std::printf("[2] no ASR: bounded timeout ends the run, no further clips\n");
    const Trace t = Drive({Event::Ok, Event::Ok, Event::Ok, Event::NoAsr, Event::Ok});
    CheckTraceEquals(t, {Step::LeadingSilence, Step::PlayClip, Step::Tail, Step::AsrWait,
                         Step::Settle},
                     "no-asr");
    CHECK_EQ(t.outcome, (int)Outcome::AsrTimeout);
    CHECK_EQ(t.clip_entered, 1);
    CheckShortInvariants(t, "no-asr");
}

/** 3. A server-reported transcription failure is reported, not retried. */
void ScenarioAsrFailed()
{
    std::printf("[3] server transcription failure: reported, no retry, no more clips\n");
    const Trace t = Drive({Event::Ok, Event::Ok, Event::Ok, Event::AsrFailed, Event::Ok});
    CHECK_EQ(t.outcome, (int)Outcome::AsrFailed);
    CHECK_EQ(t.clip_entered, 1);
    CheckShortInvariants(t, "asr-failed");
}

/** 4. Send failures at every stage end at that stage; none reaches the loop. */
void ScenarioSendFailures()
{
    std::printf("[4] send failures: each stage ends there, never a second clip or loop\n");

    // (a) failure during the leading silence: not even one clip is entered.
    Trace t = Drive({Event::SendFailed, Event::Ok});
    CheckTraceEquals(t, {Step::LeadingSilence, Step::Settle}, "fail-at-silence");
    CHECK_EQ(t.outcome, (int)Outcome::SendFailed);
    CHECK_EQ(t.clip_entered, 0);
    CheckShortInvariants(t, "fail-at-silence");

    // (b) failure inside the clip: the clip is entered once and there is no
    // second clip, which is exactly where the old code would have continued.
    t = Drive({Event::Ok, Event::SendFailed, Event::Ok});
    CheckTraceEquals(t, {Step::LeadingSilence, Step::PlayClip, Step::Settle},
                     "fail-in-clip");
    CHECK_EQ(t.outcome, (int)Outcome::SendFailed);
    CHECK_EQ(t.clip_entered, 1);
    CheckShortInvariants(t, "fail-in-clip");

    // (c) failure during the tail silence.
    t = Drive({Event::Ok, Event::Ok, Event::SendFailed, Event::Ok});
    CHECK_EQ(t.outcome, (int)Outcome::SendFailed);
    CHECK_EQ(t.clip_entered, 1);
    CheckShortInvariants(t, "fail-in-tail");

    // (d) link lost while waiting for ASR.
    t = Drive({Event::Ok, Event::Ok, Event::Ok, Event::SendFailed, Event::Ok});
    CHECK_EQ(t.outcome, (int)Outcome::LinkLost);
    CHECK_EQ(t.clip_entered, 1);
    CheckShortInvariants(t, "link-lost");
}

/** 5. Interval accounting is subtraction of two snapshots. */
void ScenarioIntervalDiff()
{
    std::printf("[5] intervals are snapshot differences, never a mid-send reset\n");

    m1_short::Counters before;
    before.protect_calls = 100;
    before.protect_ok = 100;
    before.packets_written = 100;
    before.udp_attempts = 100;
    before.seq = 99;
    before.ts = 1980;
    before.ssrc = 0x00000006;
    before.pt = 111;
    before.have_packet = true;

    m1_short::Counters after = before;
    after.protect_calls += 48;
    after.protect_ok += 48;
    after.packets_written += 48;
    after.udp_attempts += 48;
    after.protected_unwritten += 1;
    after.seq = 147;
    after.ts = 2940;
    after.ssrc_changes = 1;

    const Interval d = m1_short::Diff(before, after, "clip");
    CHECK_EQ(d.protect_calls, 48);
    CHECK_EQ(d.protect_ok, 48);
    CHECK_EQ(d.packets_written, 48);
    CHECK_EQ(d.udp_attempts, 48);
    CHECK_EQ(d.protected_unwritten, 1);
    CHECK_EQ(d.protect_fail, 0);
    CHECK_EQ(d.seq_at_start, 99);       // last packet before the interval
    CHECK_EQ(d.seq_at_end, 147);        // last packet inside it
    CHECK_EQ(d.ts_at_start, 1980);
    CHECK_EQ(d.ts_at_end, 2940);
    CHECK_EQ(d.ssrc, 0x00000006);
    CHECK_EQ(d.pt, 111);
    CHECK_EQ(d.ssrc_changes, 1);
    CHECK(d.has_start_boundary);
    CHECK(d.has_end_boundary);
    CHECK(d.has_new_activity);
    CHECK_EQ(d.new_packets, 48);
    CHECK(m1_short::HasRange(d));
    CHECK_EQ(m1_short::HasUncoveredItems(d), 1);

    // A counter that went backwards is not reported as a huge delta: that would
    // be a fabricated interval, which is why Delta() saturates.
    m1_short::Counters weird = after;
    weird.protect_ok = before.protect_ok - 5;
    const Interval w = m1_short::Diff(before, weird, "weird");
    CHECK_EQ(w.protect_ok, 0);

    // No packet in the window: no range is claimed.
    m1_short::Counters empty;
    const Interval e = m1_short::Diff(empty, empty, "empty");
    CHECK(!m1_short::HasRange(e));
    CHECK(!e.has_new_activity);
    CHECK_EQ(e.protect_calls, 0);
}

/**
 * 5b. The two confusions correction 4 names, in both directions.
 *
 * (a) The leading silence starts before the window's first packet, so it has no
 *     start boundary while producing plenty of new packets. It must NOT be
 *     reported as "no packet observed".
 * (b) An interval with no new packets must NOT inherit the previous range just
 *     because both snapshots still hold packet metadata.
 */
void ScenarioBoundaryVersusActivity()
{
    std::printf("[5b] boundary absence and no-new-activity are distinct\n");

    // (a) nothing before the interval, 100 new packets inside it.
    m1_short::Counters before;               // no packet yet
    m1_short::Counters after;
    after.protect_calls = 100;
    after.protect_ok = 100;
    after.packets_written = 100;
    after.udp_attempts = 100;
    after.seq = 99;
    after.ts = 1980;
    after.have_packet = true;

    const Interval lead = m1_short::Diff(before, after, "leading-silence");
    CHECK(!lead.has_start_boundary);
    CHECK(lead.has_end_boundary);
    CHECK(lead.has_new_activity);
    CHECK_EQ(lead.new_packets, 100);
    CHECK(m1_short::HasRange(lead));          // range is shown, with no start edge

    // (b) no new packets, but both snapshots know a packet from earlier.
    const Interval idle = m1_short::Diff(after, after, "idle");
    CHECK(idle.has_start_boundary);
    CHECK(idle.has_end_boundary);
    CHECK(!idle.has_new_activity);
    CHECK_EQ(idle.new_packets, 0);
    CHECK(!m1_short::HasRange(idle));         // must not reuse the old range
}

/** 6. Silence writes can never be substituted for speech evidence. */
void ScenarioSpeechEvidence()
{
    std::printf("[6] leading-silence writes are not speech evidence\n");

    // The observed 100-frame silence pattern: lots of successful writes, but in
    // the interval that carries no fixture audio.
    Interval silence;
    silence.label = "leading-silence";
    silence.protect_calls = 100;
    silence.protect_ok = 100;
    silence.packets_written = 100;
    silence.udp_attempts = 100;

    // The clip interval is empty because the send died at the clip boundary.
    Interval clip;
    clip.label = "zh_1 clip audio";

    CHECK(!m1_short::HasClipWriteActivity(clip));
    CHECK_EQ(silence.protect_ok + silence.packets_written > 0, 1);

    // Clip write activity requires protect + a full write in the clip interval.
    clip.protect_ok = 1;
    CHECK(!m1_short::HasClipWriteActivity(clip));   // protected but never written
    clip.packets_written = 1;
    CHECK(m1_short::HasClipWriteActivity(clip));
    clip.protect_ok = 0;
    CHECK(!m1_short::HasClipWriteActivity(clip));   // written without protection
    clip.protect_ok = 48;
    clip.packets_written = 48;
    CHECK(m1_short::HasClipWriteActivity(clip));

    // Correction 3, stated as a test: a SINGLE write early in the clip interval
    // already satisfies this predicate. The fixture begins with 500 ms of lead
    // silence, so that write can precede any speech. The predicate therefore
    // reports write activity and nothing about the audio; the audio claim lives
    // in the VAD/turn events and the transcript, which this predicate cannot see.
    Interval one_early_write;
    one_early_write.protect_ok = 1;
    one_early_write.packets_written = 1;
    CHECK(m1_short::HasClipWriteActivity(one_early_write));

    // A protected-but-unwritten interval is uncovered, not silently accepted.
    Interval partial;
    partial.protect_ok = 10;
    partial.packets_written = 9;
    partial.protected_unwritten = 1;
    CHECK(m1_short::HasUncoveredItems(partial));
}

/**
 * 7. End to end of the decision the device makes: silence full of writes, clip
 * interval empty, send failure inside the clip -> no speech evidence claimed,
 * outcome reported as a failure, and still no loop.
 */
void ScenarioSilenceOnlyRun()
{
    std::printf("[7] silence-only run: no speech claim, failure reported, no loop\n");

    const Trace t = Drive({Event::Ok, Event::SendFailed, Event::Ok});
    CHECK_EQ(t.clip_entered, 1);
    CHECK_EQ(t.outcome, (int)Outcome::SendFailed);
    CheckShortInvariants(t, "silence-only");

    // The clip interval measured for that run carries nothing.
    m1_short::Counters before;
    before.protect_calls = 100;
    before.protect_ok = 100;
    before.packets_written = 100;
    before.have_packet = true;
    before.seq = 99;
    const m1_short::Counters after = before;   // the clip wrote nothing
    const Interval clip = m1_short::Diff(before, after, "zh_1 clip audio");

    CHECK_EQ(clip.protect_ok, 0);
    CHECK_EQ(clip.packets_written, 0);
    CHECK(!m1_short::HasClipWriteActivity(clip));
    CHECK(!m1_short::HasUncoveredItems(clip));
}

}  // namespace

int main()
{
    std::printf("U1-S short-mode offline check (actual source: m1_short_plan.h)\n");
    std::printf("frozen plan: %d ms silence, clip=%s, %d ms tail, %d ms ASR bound, "
                "%d ms frames\n",
                m1_short::kLeadingSilenceMs, m1_short::kClipId,
                m1_short::kTailSilenceMs, m1_short::kAsrTimeoutMs,
                m1_short::kRequiredFrameMs);

    ScenarioSuccess();
    ScenarioNoAsr();
    ScenarioAsrFailed();
    ScenarioSendFailures();
    ScenarioIntervalDiff();
    ScenarioBoundaryVersusActivity();
    ScenarioSpeechEvidence();
    ScenarioSilenceOnlyRun();

    std::printf("checks=%d failures=%d -> %s\n", g_checks, g_failures,
                g_failures == 0 ? "PASS" : "FAIL");
    return g_failures == 0 ? 0 : 1;
}
