#include "channel_audit.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>

namespace {
using webrtc_media::ChannelAuditStats;
using webrtc_media::ToneGenerator;

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

void ToneShape()
{
    constexpr uint32_t kRate = 24000;
    constexpr size_t kFrames = 24000;  // one second
    ToneGenerator tone(1000.0, 4096.0, kRate);
    int16_t samples[kFrames];
    tone.Fill(samples, kFrames);

    int positive_edges = 0;
    int peak = 0;
    for (size_t i = 0; i < kFrames; ++i) {
        const int value = samples[i];
        const int magnitude = value < 0 ? -value : value;
        if (magnitude > peak) peak = magnitude;
        if (i > 0 && samples[i - 1] <= 0 && samples[i] > 0) ++positive_edges;
    }
    // 1 kHz for one second at 24 kHz: 1000 rising zero crossings.
    CHECK(positive_edges >= 995 && positive_edges <= 1005);
    // The sine amplitude is 4096 and rounding keeps it inside int16.
    CHECK(peak >= 4090 && peak <= 4096);

    // Frames continue the phase instead of restarting at zero.
    int16_t first[240];
    int16_t second[240];
    ToneGenerator split(1000.0, 4096.0, kRate);
    split.Fill(first, 240);
    split.Fill(second, 240);
    int max_step = 0;
    for (size_t i = 1; i < 240; ++i) {
        const int step = std::abs(second[i] - second[i - 1]);
        if (step > max_step) max_step = step;
    }
    // The per-sample slope is about 1072 for this tone; a phase reset would
    // produce a much larger jump from the frame's last sample to its first.
    CHECK(max_step < 1200);
    CHECK(std::abs(second[0] - first[239]) < 1200);
}

void StatsEnergy()
{
    ChannelAuditStats stats;
    stats.Configure(2);
    const int16_t frame[4] = {1000, 2000, 0, 0};
    for (int i = 0; i < 100; ++i) stats.AddFrame(frame);
    CHECK(stats.samples[0] == 100);
    CHECK(stats.samples[1] == 100);
    CHECK(stats.samples[2] == 0);
    CHECK(stats.peak[0] == 1000);
    CHECK(stats.peak[1] == 2000);
    CHECK(std::fabs(stats.Rms(0) - 1000.0) < 0.001);
    CHECK(std::fabs(stats.Rms(1) - 2000.0) < 0.001);
    CHECK(stats.Dbfs(2) < -119.9);
    // 1000 / 32768 = -30.3 dBFS -> -303 in tenths.
    CHECK(ChannelAuditStats::Dbfs10(stats, 0) == -303);
    CHECK(ChannelAuditStats::Dbfs10(stats, 1) == -243);
}

void SilenceAndFullScale()
{
    ChannelAuditStats silence;
    silence.Configure(2);
    const int16_t zero[4] = {0, 0, 0, 0};
    for (int i = 0; i < 10; ++i) silence.AddFrame(zero);
    CHECK(silence.peak[0] == 0);
    CHECK(ChannelAuditStats::Dbfs10(silence, 0) == -1200);

    ChannelAuditStats full;
    full.Configure(2);
    const int16_t loud[4] = {32767, -32768, 0, 0};
    full.AddFrame(loud);
    CHECK(full.peak[0] == 32767);
    CHECK(full.peak[1] == 32768);
    CHECK(ChannelAuditStats::Dbfs10(full, 0) == 0);
    CHECK(ChannelAuditStats::Dbfs10(full, 1) == 0);
}

void ReconfigureAndReset()
{
    ChannelAuditStats stats;
    stats.Configure(4);
    const int16_t frame[4] = {100, 200, 300, 400};
    stats.AddFrame(frame);
    CHECK(stats.samples[3] == 1);
    CHECK(stats.peak[3] == 400);
    stats.Configure(2);
    CHECK(stats.channels == 2);
    CHECK(stats.samples[0] == 0);
    CHECK(stats.peak[3] == 0);
    stats.Configure(9);  // clamped to the physical maximum
    CHECK(stats.channels == webrtc_media::kChannelAuditMaxChannels);
}

}  // namespace

int main()
{
    ToneShape();
    StatsEnergy();
    SilenceAndFullScale();
    ReconfigureAndReset();
    std::printf("channel_audit_check: PASS (%u assertions)\n", checks);
    return 0;
}
