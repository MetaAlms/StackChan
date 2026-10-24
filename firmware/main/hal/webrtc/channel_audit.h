#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>

namespace webrtc_media {

// Channel-audit helpers for a bounded local playback/capture check.
//
// The current capture opens two TDM slots (MIC1/MIC2); the speaker-derived
// reference is wired to MIC3 and is not in that stream. This audit therefore
// measures how much of a known local tone reaches the microphones, which
// quantifies the acoustic path into the current uplink. It does not claim to
// separate acoustic from electrical coupling, and it does not enable AEC.
constexpr size_t kChannelAuditMaxChannels = 4;

struct ChannelAuditStats {
    size_t channels = kChannelAuditMaxChannels;
    uint64_t samples[kChannelAuditMaxChannels]{};
    double sum_sq[kChannelAuditMaxChannels]{};
    uint32_t peak[kChannelAuditMaxChannels]{};

    void Configure(size_t count)
    {
        channels = count > kChannelAuditMaxChannels ? kChannelAuditMaxChannels : count;
        Reset();
    }

    void Reset()
    {
        for (size_t i = 0; i < kChannelAuditMaxChannels; ++i) {
            samples[i] = 0;
            sum_sq[i] = 0.0;
            peak[i] = 0;
        }
    }

    void AddFrame(const int16_t* frame)
    {
        if (frame == nullptr) return;
        for (size_t ch = 0; ch < channels; ++ch) {
            const int32_t value = frame[ch];
            const uint32_t magnitude = value < 0
                ? static_cast<uint32_t>(-(value + 1)) + 1u
                : static_cast<uint32_t>(value);
            sum_sq[ch] += static_cast<double>(value) * static_cast<double>(value);
            ++samples[ch];
            if (magnitude > peak[ch]) peak[ch] = magnitude;
        }
    }

    double Rms(size_t ch) const
    {
        if (ch >= channels || samples[ch] == 0) return 0.0;
        return std::sqrt(sum_sq[ch] / static_cast<double>(samples[ch]));
    }

    double Dbfs(size_t ch) const { return DbfsFromRms(Rms(ch)); }

    // Full-scale int16 is 32768. Silence is clamped to -120 dBFS so the log
    // stays finite and "very quiet" remains distinguishable from "zero".
    static double DbfsFromRms(double rms)
    {
        if (!(rms > 0.0)) return -120.0;
        const double db = 20.0 * std::log10(rms / 32768.0);
        return db < -120.0 ? -120.0 : db;
    }

    static int Dbfs10(const ChannelAuditStats& stats, size_t ch)
    {
        return static_cast<int>(std::lround(DbfsFromRms(stats.Rms(ch)) * 10.0));
    }
};

// Fixed-frequency sine for a bounded local playback. The phase is continuous
// across frames so the tone has no click at frame boundaries.
class ToneGenerator {
public:
    ToneGenerator(double frequency_hz, double amplitude, uint32_t sample_rate)
        : step_(2.0 * kPi * frequency_hz / static_cast<double>(sample_rate)),
          amplitude_(amplitude)
    {
    }

    void Reset() { phase_ = 0.0; }

    void Fill(int16_t* dst, size_t frames)
    {
        if (dst == nullptr) return;
        for (size_t i = 0; i < frames; ++i) {
            double sample = std::sin(phase_) * amplitude_;
            if (sample > 32767.0) sample = 32767.0;
            if (sample < -32768.0) sample = -32768.0;
            dst[i] = static_cast<int16_t>(std::lround(sample));
            phase_ += step_;
            if (phase_ >= 2.0 * kPi) phase_ -= 2.0 * kPi;
        }
    }

private:
    static constexpr double kPi = 3.14159265358979323846;
    double phase_ = 0.0;
    double step_ = 0.0;
    double amplitude_ = 0.0;
};

}  // namespace webrtc_media
