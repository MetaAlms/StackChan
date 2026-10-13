#pragma once

/**
 * @brief U1-S short comparison mode: frozen plan, interval accounting, outcomes.
 *
 * This header is deliberately free of ESP-IDF dependencies so that the *same*
 * code that runs on the device is compiled and executed by the offline check in
 * `firmware/tests/m1_short_offline_check.cc`. Nothing here is a copy of the
 * device logic.
 *
 * The mode exists so both arms of the U1 comparison (esp_peer 1.5.5 and 1.5.7)
 * run an identical, bounded sequence instead of the three-clip plus 300 s
 * resource loop:
 *
 *     2 s leading silence -> zh_1 only -> tail silence -> bounded ASR wait
 *
 * Structural guarantees, all of them testable rather than asserted in prose:
 *
 *  - The sequencer has **no loop step at all**. The long resource loop is not
 *    merely skipped by a flag; it is unreachable from this state machine, so
 *    "the short mode never runs the 300 s loop" holds on every path including
 *    the failure paths.
 *  - At most **one** clip is ever entered, and it is `zh_1`.
 *  - Every terminal path goes through `Step::Settle` exactly once, so the
 *    observation window cannot be reported without first being settled after
 *    the sender stopped and was joined.
 *  - Interval accounting is derived from two *non-destructive* snapshots. The
 *    window is never `Arm()`-ed mid-send, which is what would otherwise let a
 *    half-finished send be reported against a fresh window.
 *  - Leading silence can never be presented as clip activity:
 *    `HasClipWriteActivity()` is only ever fed the clip interval, and reports
 *    that the clip interval reached the SRTP and UDP boundaries - not that
 *    non-silent audio reached the server.
 */

#include <cstddef>
#include <cstdint>

namespace m1_short {

// ------------------------------------------------------------ frozen plan ---
//
// These values are part of the frozen comparison contract: both arms must be
// built from identical source, so any change here invalidates the comparison.

/** Leading silence before the clip. 100 frames at the frozen 20 ms frame. */
constexpr int kLeadingSilenceMs = 2000;
/** The only clip the short mode plays. */
constexpr const char* kClipId = "zh_1";
/** Tail silence after the clip, so the server sees a clean turn boundary. */
constexpr int kTailSilenceMs = 800;
/** Bounded ASR wait; a timeout is a valid, reported outcome. */
constexpr int kAsrTimeoutMs = 20000;
/** The comparison is pinned to 20 ms frames (enforced where the frame is chosen). */
constexpr int kRequiredFrameMs = 20;

/** Steps of the short mode. There is deliberately no loop step. */
enum class Step {
    LeadingSilence = 0,
    PlayClip,
    Tail,
    AsrWait,
    Settle,
    Done,
};

/** Result of the step that just ran. */
enum class Event {
    Ok = 0,
    SendFailed,
    NoAsr,
    AsrFailed,
};

/** How the short mode ended. "Failed" outcomes are reported, not hidden. */
enum class Outcome {
    None = 0,
    MediaCompleted,
    SendFailed,
    AsrTimeout,
    AsrFailed,
    LinkLost,
};

inline const char* StepName(Step s)
{
    switch (s) {
        case Step::LeadingSilence: return "leading-silence";
        case Step::PlayClip: return "play-clip";
        case Step::Tail: return "tail-silence";
        case Step::AsrWait: return "asr-wait";
        case Step::Settle: return "settle";
        default: return "done";
    }
}

inline const char* OutcomeName(Outcome o)
{
    switch (o) {
        case Outcome::MediaCompleted: return "media-completed";
        case Outcome::SendFailed: return "send-failed";
        case Outcome::AsrTimeout: return "asr-timeout";
        case Outcome::AsrFailed: return "asr-failed";
        case Outcome::LinkLost: return "link-lost";
        default: return "none";
    }
}

/**
 * @brief The short-mode state machine.
 *
 * `Advance()` is called with the result of the step returned by `current()`.
 * Any failure sends the sequence straight to `Settle`, so cleanup and the
 * post-join window settlement happen on every path, including the ones that
 * never reach the clip.
 */
class Sequencer {
public:
    Step current() const { return step_; }
    Outcome outcome() const { return outcome_; }
    /** Times the clip step was entered. Never exceeds 1. */
    int clip_entered() const { return clip_entered_; }
    /** Times the settle step completed. Exactly 1 once Done. */
    int settle_done() const { return settle_done_; }

    void Advance(Event e)
    {
        switch (step_) {
            case Step::LeadingSilence:
                if (e == Event::SendFailed) { Finish(Outcome::SendFailed); }
                else { step_ = Step::PlayClip; ++clip_entered_; }
                break;
            case Step::PlayClip:
                if (e == Event::SendFailed) { Finish(Outcome::SendFailed); }
                else { step_ = Step::Tail; }
                break;
            case Step::Tail:
                if (e == Event::SendFailed) { Finish(Outcome::SendFailed); }
                else { step_ = Step::AsrWait; }
                break;
            case Step::AsrWait:
                if (e == Event::SendFailed) { Finish(Outcome::LinkLost); }
                else if (e == Event::NoAsr) { Finish(Outcome::AsrTimeout); }
                else if (e == Event::AsrFailed) { Finish(Outcome::AsrFailed); }
                else { Finish(Outcome::MediaCompleted); }
                break;
            case Step::Settle:
                ++settle_done_;
                step_ = Step::Done;
                break;
            case Step::Done:
                break;
        }
    }

private:
    void Finish(Outcome o)
    {
        outcome_ = o;
        step_ = Step::Settle;
    }

    Step step_ = Step::LeadingSilence;
    Outcome outcome_ = Outcome::None;
    int clip_entered_ = 0;
    int settle_done_ = 0;
};

// ------------------------------------------------- interval accounting -----

/**
 * @brief Probe counters needed for interval accounting.
 *
 * A neutral POD so this header stays ESP-free; the device fills it from
 * `rtp_probe::Report`, the offline check fills it directly.
 */
struct Counters {
    uint32_t protect_calls = 0;
    uint32_t protect_ok = 0;
    uint32_t protect_fail = 0;
    uint32_t protect_ignored = 0;
    uint32_t udp_attempts = 0;
    uint32_t packets_written = 0;
    uint32_t write_incomplete = 0;
    uint32_t write_failed = 0;
    uint32_t udp_unmatched = 0;
    uint32_t length_mismatch = 0;
    uint32_t protected_unwritten = 0;
    uint32_t wrote_after_protect_fail = 0;
    uint32_t retired_pending = 0;
    uint32_t overflow_pending = 0;
    uint32_t ssrc_changes = 0;
    uint32_t pt_changes = 0;
    /** Last packet seen in the window, for the sequence/timestamp picture. */
    uint16_t seq = 0;
    uint32_t ts = 0;
    uint32_t ssrc = 0;
    int pt = -1;
    bool have_packet = false;
};

/**
 * @brief One measured interval, derived from two non-destructive snapshots.
 *
 * Boundaries and activity are deliberately separate:
 *
 *  - `has_start_boundary` says a packet existed *before* the interval. The
 *    leading silence normally starts before the first packet of the window, so
 *    it has no start boundary while still producing plenty of new packets.
 *  - `has_new_activity` says this interval itself produced packets.
 *  - `has_end_boundary` says a packet existed at the end of it.
 *
 * A range is only presented when the interval both has an end boundary and saw
 * new activity; `seq_at_start` is the last packet *before* the interval
 * (exclusive), never the interval's own first packet. Reporting an inherited
 * range for an interval that carried nothing is exactly the confusion this
 * separation exists to prevent.
 */
struct Interval {
    const char* label = "";
    uint32_t protect_calls = 0;
    uint32_t protect_ok = 0;
    uint32_t protect_fail = 0;
    uint32_t protect_ignored = 0;
    uint32_t udp_attempts = 0;
    uint32_t packets_written = 0;
    uint32_t write_incomplete = 0;
    uint32_t write_failed = 0;
    uint32_t udp_unmatched = 0;
    uint32_t length_mismatch = 0;
    uint32_t protected_unwritten = 0;
    uint32_t wrote_after_protect_fail = 0;
    uint32_t retired_pending = 0;
    uint32_t overflow_pending = 0;
    uint32_t ssrc_changes = 0;
    uint32_t pt_changes = 0;
    bool has_start_boundary = false;
    bool has_end_boundary = false;
    bool has_new_activity = false;
    uint32_t new_packets = 0;
    uint16_t seq_at_start = 0;
    uint16_t seq_at_end = 0;
    uint32_t ts_at_start = 0;
    uint32_t ts_at_end = 0;
    uint32_t ssrc = 0;
    int pt = -1;
};

/** Saturating subtraction: a counter that went backwards is not a huge delta. */
inline uint32_t Delta(uint32_t after, uint32_t before)
{
    return (after >= before) ? (after - before) : 0u;
}

/**
 * @brief Difference of two snapshots.
 *
 * This is the whole reason the short mode never calls `Arm()` mid-send: the
 * same window spans silence and speech, and the intervals are carved out of it
 * by subtraction instead of by restarting observation.
 */
inline Interval Diff(const Counters& before, const Counters& after, const char* label)
{
    Interval d;
    d.label = label;
    d.protect_calls = Delta(after.protect_calls, before.protect_calls);
    d.protect_ok = Delta(after.protect_ok, before.protect_ok);
    d.protect_fail = Delta(after.protect_fail, before.protect_fail);
    d.protect_ignored = Delta(after.protect_ignored, before.protect_ignored);
    d.udp_attempts = Delta(after.udp_attempts, before.udp_attempts);
    d.packets_written = Delta(after.packets_written, before.packets_written);
    d.write_incomplete = Delta(after.write_incomplete, before.write_incomplete);
    d.write_failed = Delta(after.write_failed, before.write_failed);
    d.udp_unmatched = Delta(after.udp_unmatched, before.udp_unmatched);
    d.length_mismatch = Delta(after.length_mismatch, before.length_mismatch);
    d.protected_unwritten = Delta(after.protected_unwritten, before.protected_unwritten);
    d.wrote_after_protect_fail =
        Delta(after.wrote_after_protect_fail, before.wrote_after_protect_fail);
    d.retired_pending = Delta(after.retired_pending, before.retired_pending);
    d.overflow_pending = Delta(after.overflow_pending, before.overflow_pending);
    d.ssrc_changes = Delta(after.ssrc_changes, before.ssrc_changes);
    d.pt_changes = Delta(after.pt_changes, before.pt_changes);

    d.new_packets = d.packets_written;
    d.has_new_activity = (d.packets_written > 0) || (d.protect_calls > 0) ||
                         (d.udp_attempts > 0);
    d.has_start_boundary = before.have_packet;
    d.has_end_boundary = after.have_packet;
    d.seq_at_start = before.seq;
    d.seq_at_end = after.seq;
    d.ts_at_start = before.ts;
    d.ts_at_end = after.ts;
    d.ssrc = after.ssrc;
    d.pt = after.pt;
    return d;
}

/** True only when this interval's own packet range can be shown. */
inline bool HasRange(const Interval& i)
{
    return i.has_new_activity && i.has_end_boundary;
}

/**
 * @brief Clip-interval write activity.
 *
 * This is *write activity*, not proof that non-silent audio reached the server.
 * The fixture begins with 500 ms of lead silence (fixture/manifest.json), so a
 * single packet written at the very start of the clip interval would satisfy a
 * naive "packets > 0" test even if the link then dropped before any speech was
 * encoded. What it does show is that the clip interval reached the SRTP and UDP
 * boundaries at all; the evidence for the audio itself is kept separate, in the
 * VAD/turn events and the transcript (and in the input/encode energy report).
 */
inline bool HasClipWriteActivity(const Interval& clip_interval)
{
    return clip_interval.protect_ok > 0 && clip_interval.packets_written > 0;
}

/** True when an interval shows a write that the probe could not attribute. */
inline bool HasUncoveredItems(const Interval& i)
{
    return i.protected_unwritten > 0 || i.udp_unmatched > 0 || i.length_mismatch > 0 ||
           i.wrote_after_protect_fail > 0 || i.retired_pending > 0 ||
           i.overflow_pending > 0 || i.protect_ignored > 0;
}

}  // namespace m1_short
