#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>

namespace webrtc_media {

enum class RunMode { NearEnd, Duplex };
enum class RunState { Idle, Preparing, Running, Stopping, Done, Failed };
enum class StopReason { None, Requested, PrepareTimeout, RunTimeout, Error, Disconnected, Completed };
enum class AsrStatus { Accepted, Duplicate, MissingItem, WrongRun, NotRunning, Overflow, InvalidItem };

struct AsrObservation {
    AsrStatus status;
    size_t ordinal;  // Run-local item number; zero means no retained item.
    unsigned keyword_mask;
};

// One worker owns this object. It controls software admission and evidence
// boundaries; it makes no claim about acoustic playback or AEC.
class SessionRunControl {
public:
    static constexpr size_t kMaxItems = 64;
    static constexpr size_t kMaxItemIdBytes = 128;
    static constexpr int64_t kPrepareTimeoutUs = 30000000;

    bool Start(const std::string& run_id, RunMode mode, uint32_t duration_seconds, int64_t now_us)
    {
        if (state_ != RunState::Idle || !ValidRunId(run_id) ||
            (mode != RunMode::NearEnd && mode != RunMode::Duplex) ||
            duration_seconds < 5 || duration_seconds > 180 || now_us < 0) return false;
        run_id_ = run_id;
        mode_ = mode;
        duration_seconds_ = duration_seconds;
        prepare_started_us_ = now_us;
        state_ = RunState::Preparing;
        return true;
    }

    bool Ready(const std::string& run_id, int64_t now_us)
    {
        if (run_id != run_id_ || state_ != RunState::Preparing || now_us < prepare_started_us_) return false;
        if (Poll(now_us)) return false;
        run_started_us_ = now_us;
        state_ = RunState::Running;
        return true;
    }

    bool Stop(const std::string& run_id, StopReason reason)
    {
        if (run_id != run_id_ || reason == StopReason::None ||
            (state_ != RunState::Preparing && state_ != RunState::Running)) return false;
        stop_reason_ = reason;
        state_ = RunState::Stopping;
        return true;
    }

    // Returns true only when this call requests timeout cleanup. The caller
    // still owns closing admission, stopping tasks and joining callbacks.
    bool Poll(int64_t now_us)
    {
        if (state_ == RunState::Preparing && now_us >= prepare_started_us_ &&
            now_us - prepare_started_us_ >= kPrepareTimeoutUs) {
            return Stop(run_id_, StopReason::PrepareTimeout);
        }
        if (state_ == RunState::Running && now_us >= run_started_us_ &&
            now_us - run_started_us_ >= int64_t(duration_seconds_) * 1000000) {
            return Stop(run_id_, StopReason::RunTimeout);
        }
        return false;
    }

    bool Finish(const std::string& run_id, bool success)
    {
        if (run_id != run_id_ || state_ != RunState::Stopping) return false;
        state_ = success ? RunState::Done : RunState::Failed;
        return true;
    }

    // This flag is an explicit caller contract, not a substitute for joining
    // the transport/audio owners before clearing their run-local state.
    bool Reset(bool lifecycle_joined)
    {
        if (!lifecycle_joined || (state_ != RunState::Done && state_ != RunState::Failed)) return false;
        *this = SessionRunControl{};
        return true;
    }

    AsrObservation ObserveAsr(const std::string& run_id, const std::string& item_id, unsigned keyword_bits)
    {
        if (run_id != run_id_) return Observation(AsrStatus::WrongRun);
        if (state_ != RunState::Running) return Observation(AsrStatus::NotRunning);
        if (item_id.empty()) {
            Increment(asr_missing_item_);
            return Observation(AsrStatus::MissingItem);
        }
        if (item_id.size() > kMaxItemIdBytes) {
            Increment(asr_invalid_item_);
            return Observation(AsrStatus::InvalidItem);
        }
        for (size_t i = 0; i < asr_items_; ++i) {
            if (items_[i] == item_id) {
                Increment(asr_duplicates_);
                return Observation(AsrStatus::Duplicate, i + 1);
            }
        }
        if (asr_items_ == kMaxItems) {
            Increment(asr_overflow_);
            return Observation(AsrStatus::Overflow);
        }
        items_[asr_items_] = item_id;
        const unsigned bits = keyword_bits & 7;
        ++asr_items_;
        // Each distinct item can satisfy at most one sentence. Use the prior
        // reachable set for this item, so a text matching all three sentences
        // cannot turn into three independent observations. Keep alternatives
        // so an ambiguous first item does not prevent a later valid matching.
        auto next = reachable_;
        for (unsigned mask = 0; mask < reachable_.size(); ++mask) {
            if (!reachable_[mask]) continue;
            for (unsigned bit : {1u, 2u, 4u}) {
                if ((bits & bit) && !(mask & bit)) next[mask | bit] = true;
            }
        }
        reachable_ = next;
        for (unsigned mask = 0; mask < reachable_.size(); ++mask) {
            if (reachable_[mask] && CountBits(mask) > CountBits(keyword_mask_)) keyword_mask_ = mask;
        }
        return Observation(AsrStatus::Accepted, asr_items_);
    }

    RunState state() const { return state_; }
    const std::string& run_id() const { return run_id_; }
    RunMode mode() const { return mode_; }
    uint32_t duration_seconds() const { return duration_seconds_; }
    int64_t prepare_started_us() const { return prepare_started_us_; }
    int64_t run_started_us() const { return run_started_us_; }
    StopReason stop_reason() const { return stop_reason_; }
    unsigned keyword_mask() const { return keyword_mask_; }
    size_t asr_items() const { return asr_items_; }
    uint32_t asr_overflow() const { return asr_overflow_; }
    uint32_t asr_missing_item() const { return asr_missing_item_; }
    uint32_t asr_invalid_item() const { return asr_invalid_item_; }
    uint32_t asr_duplicates() const { return asr_duplicates_; }
    bool near_end_complete() const
    {
        return state_ == RunState::Running && mode_ == RunMode::NearEnd &&
            keyword_mask_ == 7 && asr_overflow_ == 0;
    }

private:
    static bool ValidRunId(const std::string& id)
    {
        if (id.empty() || id.size() > 32) return false;
        for (unsigned char ch : id) {
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') ||
                  (ch >= '0' && ch <= '9') || ch == '_' || ch == '-')) return false;
        }
        return true;
    }

    static unsigned CountBits(unsigned bits)
    {
        return (bits & 1) + ((bits >> 1) & 1) + ((bits >> 2) & 1);
    }

    static void Increment(uint32_t& value)
    {
        if (value != std::numeric_limits<uint32_t>::max()) ++value;
    }

    AsrObservation Observation(AsrStatus status, size_t ordinal = 0) const
    {
        return {status, ordinal, keyword_mask_};
    }

    RunState state_ = RunState::Idle;
    std::string run_id_;
    RunMode mode_ = RunMode::NearEnd;
    uint32_t duration_seconds_ = 0;
    int64_t prepare_started_us_ = -1;
    int64_t run_started_us_ = -1;
    StopReason stop_reason_ = StopReason::None;
    std::array<std::string, kMaxItems> items_{};
    std::array<bool, 8> reachable_{{true, false, false, false, false, false, false, false}};
    size_t asr_items_ = 0;
    unsigned keyword_mask_ = 0;
    uint32_t asr_overflow_ = 0;
    uint32_t asr_missing_item_ = 0;
    uint32_t asr_invalid_item_ = 0;
    uint32_t asr_duplicates_ = 0;
};

}  // namespace webrtc_media
