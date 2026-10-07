#include <sdkconfig.h>

// PROBE_GUARD
#if CONFIG_STACKCHAN_WEBRTC_M1

#include "rtp_send_probe.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <esp_log.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <srtp.h>

#define TAG "RTP-PROBE"

/**
 * Linker-wrapped observation of the two real send boundaries.
 *
 * `__real_*` come from GNU ld --wrap. Never call the wrapped name from inside
 * its own wrapper: that would recurse.
 */
extern "C" {
srtp_err_status_t __real_srtp_protect(srtp_t ctx, const uint8_t* rtp, size_t rtp_len,
                                      uint8_t* srtp, size_t* srtp_len, size_t mki_index);
ssize_t __real_lwip_sendto(int s, const void* dataptr, size_t size, int flags,
                           const struct sockaddr* to, socklen_t tolen);
}

namespace rtp_probe {
namespace {

constexpr size_t kRing = 64;
constexpr int kMinRtpHeader = 12;
/** A record older than this is retired rather than matched (microseconds). */
constexpr int64_t kRecordTtlUs = 2 * 1000 * 1000;

enum class Slot : uint8_t {
    kFree = 0,
    kPending,        // observed at srtp_protect, result not yet recorded
    kProtected,      // srtp_protect returned ok
    kProtectFailed,  // srtp_protect returned non-ok
    kWritten,        // at least one UDP write reported full success
    kWriteFailed,    // matched writes all failed
};

struct Record {
    Slot state = Slot::kFree;
    uint32_t gen = 0;
    uint16_t seq = 0;
    uint32_t ts = 0;
    uint32_t ssrc = 0;
    int pt = -1;
    int rtp_len = 0;
    int header_len = 0;
    int out_capacity = -1;
    int srtp_len = -1;
    bool protect_ok = false;
    uint32_t attempts = 0;       // UDP write attempts for this packet
    bool any_success = false;    // at least one attempt wrote the whole datagram
    int last_rc = 0;
    int last_errno = 0;
    uint32_t id = 0;
    int64_t t_us = 0;
};

std::mutex g_mtx;
Record g_ring[kRing];
uint32_t g_id = 0;
uint32_t g_generation = 1;

Report g_rep;

inline uint16_t Be16(const uint8_t* p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}
inline uint32_t Be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/** Parse only the fixed RTP header fields, with bounded arithmetic. */
bool ParseRtp(const uint8_t* d, size_t n, Record* out)
{
    if (d == nullptr || n < (size_t)kMinRtpHeader) {
        return false;
    }
    if ((d[0] >> 6) != 2) {
        return false;
    }
    const int cc = d[0] & 0x0F;
    const bool has_ext = (d[0] & 0x10) != 0;

    size_t header = (size_t)kMinRtpHeader + 4u * (size_t)cc;
    if (header > n) {
        return false;
    }
    if (has_ext) {
        if (header + 4 > n) {
            return false;
        }
        const uint16_t words = Be16(d + header + 2);
        header += 4u + 4u * (size_t)words;
        if (header > n) {
            return false;
        }
    }

    out->pt = d[1] & 0x7F;
    out->seq = Be16(d + 2);
    out->ts = Be32(d + 4);
    out->ssrc = Be32(d + 8);
    out->rtp_len = (int)n;
    out->header_len = (int)header;
    return true;
}

/**
 * @brief Classify a record before its slot is released.
 *
 * Which counter moves depends on how far the record got. Treating every
 * non-free state as "completed" hid exactly the case this probe exists to find:
 * a packet that reached SRTP and was never written to the socket.
 */
void RetireLocked(Record& r)
{
    switch (r.state) {
        case Slot::kPending:
            ++g_rep.retired_pending;        // never saw a protect result
            break;
        case Slot::kProtected:
            ++g_rep.protected_unwritten;    // protected, no write ever observed
            break;
        default:
            break;
    }
    r.state = Slot::kFree;
    ++g_rep.retired;
}

/** Retire records that are too old to still be in flight. */
void ExpireLocked(int64_t now)
{
    for (size_t i = 0; i < kRing; ++i) {
        Record& r = g_ring[i];
        if (r.state != Slot::kFree && (now - r.t_us) > kRecordTtlUs) {
            RetireLocked(r);
        }
    }
}

/** Take a slot for a new protect observation. */
Record* AcquireLocked(int64_t now)
{
    ExpireLocked(now);
    // Prefer a free slot.
    for (size_t i = 0; i < kRing; ++i) {
        if (g_ring[i].state == Slot::kFree) {
            return &g_ring[i];
        }
    }
    // Reuse the oldest completed slot; recycling a *pending* one means a real
    // observation was lost, which is what overflow must count. Normal traffic
    // recycling a completed slot is not overflow.
    Record* oldest = &g_ring[0];
    for (size_t i = 0; i < kRing; ++i) {
        if (g_ring[i].t_us < oldest->t_us) {
            oldest = &g_ring[i];
        }
    }
    // Reusing the slot loses whatever it held, so classify it the same way a
    // retirement would be classified, and count it as overflow only when the
    // lost observation was still pending.
    const bool was_pending = (oldest->state == Slot::kPending);
    RetireLocked(*oldest);
    if (was_pending) {
        ++g_rep.overflow_pending;
    }
    return oldest;
}

Record* FindMatchLocked(uint16_t seq, uint32_t ts, uint32_t ssrc, int64_t now)
{
    ExpireLocked(now);
    for (size_t i = 0; i < kRing; ++i) {
        Record& r = g_ring[i];
        // Only a record that reached the protect stage and belongs to this
        // generation may be matched, and only while it is still current.
        if (r.state != Slot::kProtected && r.state != Slot::kProtectFailed &&
            r.state != Slot::kWritten && r.state != Slot::kWriteFailed) {
            continue;
        }
        if (r.gen != g_generation) {
            continue;
        }
        if (r.seq == seq && r.ts == ts && r.ssrc == ssrc) {
            return &r;
        }
    }
    return nullptr;
}

}  // namespace

void Arm()
{
    std::lock_guard<std::mutex> lock(g_mtx);
    for (size_t i = 0; i < kRing; ++i) {
        g_ring[i] = Record();
    }
    g_id = 0;
    ++g_generation;
    g_rep = Report();
    g_rep.generation = g_generation;
}

Report Snapshot()
{
    std::lock_guard<std::mutex> lock(g_mtx);
    // Settle the window: everything still pending or protected-but-unwritten is
    // classified now, so a short run cannot claim coverage it never had.
    for (size_t i = 0; i < kRing; ++i) {
        if (g_ring[i].state != Slot::kFree) {
            RetireLocked(g_ring[i]);
        }
    }
    return g_rep;
}

std::string Format(const Report& r)
{
    char buf[1200];
    // Cast every counter to unsigned: the fields are uint32_t but the
    // formatter is newlib-nano, so the argument types must match exactly.
    snprintf(buf, sizeof(buf),
             "gen=%u | srtp_protect calls=%u ok=%u fail=%u unparsable=%u | "
             "udp attempts=%u packets_written=%u write_incomplete=%u write_failed=%u "
             "unmatched=%u last_rc=%d last_errno=%d | retired=%u retired_pending=%u "
             "overflow_pending=%u protected_unwritten=%u length_mismatch=%u "
             "wrote_after_protect_fail=%u | first: pt=%d seq=%u ts=%u ssrc=0x%08x rtp_len=%d "
             "hdr_len=%d srtp_cap=%d srtp_len=%d | last: seq=%u ts=%u | "
             "ssrc_changes=%u pt_changes=%u",
             (unsigned)r.generation,
             (unsigned)r.protect_calls, (unsigned)r.protect_ok,
             (unsigned)r.protect_fail, (unsigned)r.protect_ignored,
             (unsigned)r.udp_attempts, (unsigned)r.packets_written,
             (unsigned)r.write_incomplete, (unsigned)r.write_failed,
             (unsigned)r.udp_unmatched, r.last_rc, r.last_errno,
             (unsigned)r.retired, (unsigned)r.retired_pending,
             (unsigned)r.overflow_pending, (unsigned)r.protected_unwritten,
             (unsigned)r.length_mismatch, (unsigned)r.wrote_after_protect_fail,
             r.first_pt, (unsigned)r.first_seq, (unsigned)r.first_ts,
             (unsigned)r.first_ssrc, r.first_rtp_len, r.first_header_len,
             r.first_out_capacity, r.first_srtp_len,
             (unsigned)r.last_seq, (unsigned)r.last_ts,
             (unsigned)r.ssrc_changes, (unsigned)r.pt_changes);
    return std::string(buf);
}

}  // namespace rtp_probe

extern "C" srtp_err_status_t __wrap_srtp_protect(srtp_t ctx, const uint8_t* rtp,
                                                 size_t rtp_len, uint8_t* srtp,
                                                 size_t* srtp_len, size_t mki_index)
{
    using namespace rtp_probe;

    // Observation must not disturb the call: keep errno exactly as it was even
    // around the pre-call snapshot.
    const int entry_errno = errno;
    bool parsed = false;
    uint16_t seq = 0;
    uint32_t ts = 0, ssrc = 0;
    int pt = -1, hdr_len = 0, rtp_n = 0;
    int capacity = (srtp_len != nullptr) ? (int)*srtp_len : -1;
    uint32_t my_gen = 0;
    uint32_t my_id = 0;

    // Parse outside the lock, then insert under it.
    {
        Record tmp;
        parsed = ParseRtp(rtp, rtp_len, &tmp);
        if (parsed) {
            seq = tmp.seq;
            ts = tmp.ts;
            ssrc = tmp.ssrc;
            pt = tmp.pt;
            hdr_len = tmp.header_len;
            rtp_n = tmp.rtp_len;
        }
    }

    if (parsed) {
        std::lock_guard<std::mutex> lock(g_mtx);
        const int64_t now = esp_timer_get_time();
        Record* slot = AcquireLocked(now);
        *slot = Record();
        slot->state = Slot::kPending;
        slot->gen = g_generation;
        slot->seq = seq;
        slot->ts = ts;
        slot->ssrc = ssrc;
        slot->pt = pt;
        slot->rtp_len = rtp_n;
        slot->header_len = hdr_len;
        slot->out_capacity = capacity;
        slot->t_us = now;
        my_gen = slot->gen;
        slot->id = ++g_id;
    }

    errno = entry_errno;
    const srtp_err_status_t rc =
        __real_srtp_protect(ctx, rtp, rtp_len, srtp, srtp_len, mki_index);
    const int saved_errno = errno;

    {
        std::lock_guard<std::mutex> lock(g_mtx);
        ++g_rep.protect_calls;
        if (parsed) {
            // Bind the protect result to *this* record, not to the tuple alone.
            // The record inserted above is the newest pending match for the tuple.
            Record* mine = nullptr;
            for (size_t i = 0; i < kRing; ++i) {
                Record& r = g_ring[i];
                if (r.state == Slot::kPending && r.gen == my_gen && r.seq == seq &&
                    r.ts == ts && r.ssrc == ssrc) {
                    if (mine == nullptr || r.t_us >= mine->t_us) {
                        mine = &r;
                    }
                }
            }
            if (mine != nullptr) {
                if (rc == srtp_err_status_ok) {
                    mine->state = Slot::kProtected;
                    mine->protect_ok = true;
                    // *srtp_len is meaningful only on success.
                    mine->srtp_len = (srtp_len != nullptr) ? (int)*srtp_len : -1;
                    ++g_rep.protect_ok;
                    if (!g_rep.have_first) {
                        g_rep.have_first = true;
                        g_rep.first_pt = mine->pt;
                        g_rep.first_seq = mine->seq;
                        g_rep.first_ts = mine->ts;
                        g_rep.first_ssrc = mine->ssrc;
                        g_rep.first_rtp_len = mine->rtp_len;
                        g_rep.first_header_len = mine->header_len;
                        g_rep.first_out_capacity = mine->out_capacity;
                        g_rep.first_srtp_len = mine->srtp_len;
                    }
                    if (g_rep.last_ssrc != 0 && g_rep.last_ssrc != mine->ssrc) {
                        ++g_rep.ssrc_changes;
                    }
                    if (g_rep.last_pt != -1 && g_rep.last_pt != mine->pt) {
                        ++g_rep.pt_changes;
                    }
                    g_rep.last_seq = mine->seq;
                    g_rep.last_ts = mine->ts;
                    g_rep.last_ssrc = mine->ssrc;
                    g_rep.last_pt = mine->pt;
                } else {
                    mine->state = Slot::kProtectFailed;
                    ++g_rep.protect_fail;
                }
            } else {
                ++g_rep.protect_fail;
            }
        } else {
            ++g_rep.protect_ignored;
        }
    }

    errno = saved_errno;
    return rc;
}

/**
 * @brief Observe one UDP write for the RTP path.
 *
 * The actual --wrap_lwip_sendto lives in dtls_short_probe.cc so that a single
 * wrapper feeds both probes; defining it twice would be a duplicate symbol.
 */
namespace rtp_probe {

void ObserveSendto(const void* dataptr, size_t size, ssize_t rc, int saved_errno)
{
    Record parsed_rec;
    const bool parsed = ParseRtp((const uint8_t*)dataptr, size, &parsed_rec);
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!parsed) {
        ++g_rep.udp_unmatched;
        return;
    }
    const int64_t now = esp_timer_get_time();
    Record* hit = FindMatchLocked(parsed_rec.seq, parsed_rec.ts, parsed_rec.ssrc, now);
    if (hit == nullptr) {
        ++g_rep.udp_unmatched;
        return;
    }
    ++hit->attempts;
    ++g_rep.udp_attempts;

    // A success needs all three: protect succeeded, the UDP request length
    // equals that record's SRTP output length, and the call wrote it all.
    const bool protect_ok = (hit->state == Slot::kProtected ||
                             hit->state == Slot::kWritten);
    const bool length_ok = (hit->srtp_len >= 0 &&
                            (ssize_t)hit->srtp_len == (ssize_t)size);
    if (!protect_ok) {
        ++g_rep.wrote_after_protect_fail;
    }
    if (protect_ok && !length_ok && rc >= 0) {
        ++g_rep.length_mismatch;
    }

    if (protect_ok && length_ok && rc == (ssize_t)size) {
        if (!hit->any_success) {          // retries must not double-count
            hit->any_success = true;
            ++g_rep.packets_written;
        }
        hit->state = Slot::kWritten;
    } else if (rc >= 0 && rc < (ssize_t)size) {
        ++g_rep.write_incomplete;
        if (!hit->any_success) {
            hit->state = Slot::kWriteFailed;
        }
    } else if (rc < 0) {
        ++g_rep.write_failed;
        g_rep.last_rc = (int)rc;
        g_rep.last_errno = saved_errno;
        if (!hit->any_success) {
            hit->state = Slot::kWriteFailed;
        }
    }
    hit->last_rc = (int)rc;
    hit->last_errno = saved_errno;
}

}  // namespace rtp_probe

#endif  // CONFIG_STACKCHAN_WEBRTC_M1
