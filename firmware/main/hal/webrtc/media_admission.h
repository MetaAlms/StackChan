#pragma once

#include <atomic>

namespace webrtc_media {

// One run owns one gate. Cancellation is permanent, including when a delayed
// readiness callback tries to open admission after STOP was requested.
class MediaAdmission {
public:
    bool Open()
    {
        State expected = State::Closed;
        return state_.compare_exchange_strong(expected, State::Open);
    }

    void Cancel() { state_.store(State::Cancelled); }
    bool CanAccept() const { return state_.load() == State::Open; }

private:
    enum class State { Closed, Open, Cancelled };
    std::atomic<State> state_{State::Closed};
};

}  // namespace webrtc_media
