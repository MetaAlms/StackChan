#pragma once

#include <sdkconfig.h>

// PROBE_GUARD
#if CONFIG_STACKCHAN_WEBRTC_M1

#include <cstdint>
#include <string>

/**
 * @brief Actual RTP -> SRTP -> UDP send observation (R2-4).
 *
 * `esp_peer_send_audio()` returning 0 does not prove anything reached the
 * socket: the library's RTP encoder calls a void packet callback and returns 0
 * unconditionally, and `write_rtp_packet` never propagates the SRTP or agent
 * write result. These wrappers sit on two real boundaries instead:
 *
 *   - `--wrap=srtp_protect`  sees the fully serialized RTP header (including
 *     any extensions) immediately before encryption, and the SRTP status.
 *   - `--wrap=lwip_sendto`   sees the actual UDP write result and errno.
 *
 * Both are unresolved externals in the matching esp32s3 objects, which is what
 * makes linker wrapping possible without touching managed sources or guessing
 * at opaque object layouts. Observation only: every call is forwarded
 * unchanged, including its return value and errno.
 */
namespace rtp_probe {

/** Start a new observation generation; drops pending records. */
void Arm();

/** Results of one observation window. */
struct Report {
    uint32_t generation = 0;
    // SRTP boundary.
    uint32_t protect_calls = 0;
    uint32_t protect_ok = 0;
    uint32_t protect_fail = 0;
    uint32_t protect_ignored = 0;   // too short / malformed to parse
    // UDP boundary. Attempts and packets are separate: the UDP path can retry,
    // so attempts > packets even without loss.
    uint32_t udp_attempts = 0;
    uint32_t packets_written = 0;   // records with at least one full write
    uint32_t write_incomplete = 0;  // short write (0 <= rc < size)
    uint32_t write_failed = 0;      // rc < 0
    uint32_t udp_unmatched = 0;
    int last_rc = 0;
    int last_errno = 0;
    // Record lifecycle. Retiring a completed record is normal; recycling or
    // retiring a *pending* one means an observation was lost.
    uint32_t retired = 0;
    uint32_t retired_pending = 0;
    uint32_t overflow_pending = 0;

    // R3-1: a "send succeeded" claim needs these distinctions, not just counts
    // of calls that happened.
    /** Protect succeeded but no matching write was ever observed. */
    uint32_t protected_unwritten = 0;
    /** A write whose length disagreed with that record's SRTP output length. */
    uint32_t length_mismatch = 0;
    /** A write followed a protect failure - the real path to diagnose. */
    uint32_t wrote_after_protect_fail = 0;

    // First packet observed, for the RTP header sanity check.
    bool have_first = false;
    uint16_t first_seq = 0;
    uint32_t first_ts = 0;
    uint32_t first_ssrc = 0;
    int first_pt = -1;
    int first_rtp_len = 0;
    int first_header_len = 0;
    int first_out_capacity = -1;
    int first_srtp_len = -1;

    // Last packet observed, so a wrap, SSRC change or PT change is visible.
    uint16_t last_seq = 0;
    uint32_t last_ts = 0;
    uint32_t last_ssrc = 0;
    int last_pt = -1;
    uint32_t ssrc_changes = 0;
    uint32_t pt_changes = 0;
};

/** Snapshot the counters. Safe to call from the sender task. */
Report Snapshot();

/** One-line summary, non-sensitive metadata only. */
std::string Format(const Report& r);

/**
 * @brief Record one UDP write seen by the shared lwip_sendto wrapper.
 * @param rc          the real call's return value
 * @param saved_errno errno immediately after the real call
 */
void ObserveSendto(const void* dataptr, size_t size, ssize_t rc, int saved_errno);

}  // namespace rtp_probe

#endif  // CONFIG_STACKCHAN_WEBRTC_M1
