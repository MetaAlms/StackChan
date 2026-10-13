#include <sdkconfig.h>

// PROBE_GUARD
#if CONFIG_STACKCHAN_WEBRTC_M1

#include "dtls_short_probe.h"

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <mutex>

#include <esp_log.h>
#include <esp_timer.h>
#include <lwip/sockets.h>
#include <mbedtls/ssl.h>

#include "rtp_send_probe.h"

#define TAG "DTLS-PROBE"

/**
 * Socket entry points, redirected here by --wrap. Declared at global scope with
 * C language linkage, which is what `__real_<symbol>` actually names; the
 * wrapped name is never called from inside its own wrapper.
 */
extern "C" {
ssize_t __real_lwip_sendto(int s, const void* dataptr, size_t size, int flags,
                           const struct sockaddr* to, socklen_t tolen);
ssize_t __real_lwip_recvfrom(int s, void* mem, size_t len, int flags,
                             struct sockaddr* from, socklen_t* fromlen);
}

/**
 * Linker-wrapped observation. `__real_*` come from --wrap; never call the
 * wrapped name from inside its own wrapper. The prototypes are declared once in
 * `dtls_probe_core.h`, together with the wrapper bodies, so the offline check
 * exercises the same classification, lifecycle and errno/return discipline.
 *
 * Note: mbedtls_ssl_get_ciphersuite / mbedtls_ssl_is_handshake_over /
 * mbedtls_ssl_conf_get_endpoint / mbedtls_ssl_context_get_config are public
 * inline wrappers over the real getters in this Mbed TLS build, and an inline
 * cannot be intercepted by --wrap. They are therefore called directly, which is
 * correct: only the *handshake*, *setup*, *config* and *socket* calls need
 * interception.
 */
namespace dtls_probe {
namespace {

inline uint16_t Be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t Be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/** A DTLS record is only counted when its type is one of the defined four. */
bool KnownType(uint8_t t)
{
    return t == 20 || t == 21 || t == 22 || t == 23;   // CCS, alert, handshake, appdata
}

constexpr int kDtlsHeader = 13;   // classic DTLS record header

}  // namespace

/**
 * @brief Parse one bounded classic DTLS record at @p d.
 *
 * Validates the record-layer version (DTLS 1.0/1.2 record version FEFD/FEFF), the
 * declared fragment length against the bytes actually present, and the content
 * type. Only the fixed 13-byte header is read: never the fragment body, so no
 * payload, key, certificate or MKI is examined.
 *
 * @param consumed receives 13 + declared length when the record is complete
 * @return 1 parsed, 0 not a DTLS record, -1 truncated/inconsistent
 */
static int ParseOneDtls(const uint8_t* d, size_t n, uint8_t* type, uint16_t* epoch,
                        uint32_t* seq_hi, uint32_t* seq_lo, uint32_t* length,
                        size_t* consumed)
{
    if (d == nullptr || n < (size_t)kDtlsHeader) {
        return -1;
    }
    if (!KnownType(d[0])) {
        return 0;                                   // not DTLS at all
    }
    // Record-layer version: DTLS 1.0 is FEFD, DTLS 1.2 is FEFF.
    const uint16_t ver = Be16(d + 1);
    if (ver != 0xFEFD && ver != 0xFEFF) {
        return 0;
    }
    const uint32_t len = Be16(d + 11);
    if ((size_t)len > n - (size_t)kDtlsHeader) {
        return -1;                                  // declared length exceeds the datagram
    }
    *type = d[0];
    *epoch = Be16(d + 3);
    *seq_hi = ((uint32_t)d[5] << 8) | d[6];         // high 16 of the 48-bit sequence
    *seq_lo = Be32(d + 7);                          // low 32 of the 48-bit sequence
    *length = len;
    *consumed = (size_t)kDtlsHeader + (size_t)len;
    return 1;
}

/** Walk every record in one datagram, bounded by the bytes actually present. */
static void RecordDatagram(const uint8_t* d, size_t n, bool tx)
{
    State& st = Global();
    std::lock_guard<std::mutex> lock(st.mtx);
    Report& r = st.rep;
    if (d == nullptr || n < (size_t)kDtlsHeader) {
        if (tx) { ++r.tx_unparsable; } else { ++r.rx_unparsable; }
        return;
    }
    size_t off = 0;
    bool any = false;
    while (off + (size_t)kDtlsHeader <= n) {
        uint8_t type; uint16_t epoch; uint32_t hi, lo, len; size_t used = 0;
        const int rc = ParseOneDtls(d + off, n - off, &type, &epoch, &hi, &lo, &len, &used);
        if (rc == 0) {
            break;                                  // no DTLS record here
        }
        if (rc < 0) {
            if (tx) { ++r.tx_truncated; } else { ++r.rx_truncated; }
            break;
        }
        any = true;
        if (tx) {
            ++r.tx_records;
            ++r.tx_records_in_dgram;
            if (r.last_tx_epoch != 0xFFFF && r.last_tx_epoch != epoch) {
                ++r.tx_epoch_changes;
            }
            r.last_tx_epoch = epoch;
            r.last_tx_seq = hi;
            r.last_tx_seq_lo = lo;
            r.last_tx_len = len;
            r.last_tx_type = type;
            if (!r.have_first_tx) { r.have_first_tx = true; r.first_tx_type = type; }
        } else {
            ++r.rx_records;
            if (r.last_rx_epoch != 0xFFFF && r.last_rx_epoch != epoch) {
                ++r.rx_epoch_changes;
            }
            r.last_rx_epoch = epoch;
            r.last_rx_seq = hi;
            r.last_rx_seq_lo = lo;
            r.last_rx_len = len;
            r.last_rx_type = type;
            if (!r.have_first_rx) { r.have_first_rx = true; r.first_rx_type = type; }
        }
        off += used;
    }
    if (!any) {
        if (tx) { ++r.tx_unparsable; } else { ++r.rx_unparsable; }
    }
}

void NoteDtlsSocket(int fd)
{
    State& st = Global();
    std::lock_guard<std::mutex> lock(st.mtx);
    st.rep.dtls_fd = fd;
}

void NoteRtpSocket(int fd)
{
    State& st = Global();
    std::lock_guard<std::mutex> lock(st.mtx);
    st.rep.rtp_fd = fd;
}

void NotePoll(int64_t us)
{
    State& st = Global();
    std::lock_guard<std::mutex> lock(st.mtx);
    ++st.rep.poll_samples;
    st.rep.poll_last_us = us;
    if (us > st.rep.poll_max_us) {
        st.rep.poll_max_us = us;
    }
}

void Format(const Report& r, char* out, size_t out_len, const char* tag)
{
    snprintf(out, out_len,
             "[%s] gen=%u | setups=%u (datagram=%u stream=%u unknown=%u overflow=%u) | "
             "identity last=%s unknown_ctx=%u stale=%u unknown_transport=%u ctx_bound=%u "
             "ctx_overflow=%u | "
             "hs calls=%u dtls=%u ok=%u fail=%u stream_excluded=%u "
             "last_ret=%d last=%dms total=%dms over=%d role=%d | "
             "profile reads=%u skipped=%u -> %s profile=%d%s | "
             "DTLS TX records=%u in_dgrams=%u unparsable=%u truncated=%u "
             "short_write=%u failed=%u epoch_changes=%u first_type=%d last_type=%d "
             "epoch=%d seq=%u:%u len=%u | "
             "DTLS RX records=%u unparsable=%u truncated=%u epoch_changes=%u "
             "first_type=%d last_type=%d epoch=%d seq=%u:%u len=%u | "
             "socket assoc=%s (dtls_fd=%d rtp_fd=%d) | sendto=%u recvfrom=%u | "
             "calls conf_transport=%u config_defaults=%u setup=%u config_free=%u "
             "ssl_free=%u transport_updates=%u errno(entry=%d real=%d) | "
             "poll n=%u last=%dus max=%dus",
             tag, (unsigned)r.generation,
             (unsigned)r.setups, (unsigned)r.datagram_setups, (unsigned)r.stream_setups,
             (unsigned)r.unknown_setups, (unsigned)r.setup_overflow,
             r.last_identity < 0 ? "none" : IdentityName((Identity)r.last_identity),
             (unsigned)r.unknown_ctx, (unsigned)r.stale_ctx,
             (unsigned)r.unknown_transport, (unsigned)r.ctx_bound,
             (unsigned)r.ctx_overflow,
             (unsigned)r.hs_calls, (unsigned)r.dtls_hs_calls, (unsigned)r.hs_ok,
             (unsigned)r.hs_fail, (unsigned)r.stream_hs_excluded,
             r.hs_last_ret, (int)r.hs_last_ms,
             (int)r.hs_total_ms, (int)r.handshake_over, r.have_role ? r.role : -1,
             (unsigned)r.profile_reads, (unsigned)r.profile_skipped,
             r.have_cipher ? r.cipher : "(none)",
             r.have_profile ? r.profile : -1,
             r.profile_unset ? " UNSET" : "",
             (unsigned)r.tx_records, (unsigned)r.tx_records_in_dgram,
             (unsigned)r.tx_unparsable, (unsigned)r.tx_truncated,
             (unsigned)r.tx_short_write, (unsigned)r.tx_failed,
             (unsigned)r.tx_epoch_changes,
             r.have_first_tx ? r.first_tx_type : -1, (int)r.last_tx_type,
             r.last_tx_epoch == 0xFFFF ? -1 : (int)r.last_tx_epoch,
             (unsigned)r.last_tx_seq, (unsigned)r.last_tx_seq_lo, (unsigned)r.last_tx_len,
             (unsigned)r.rx_records, (unsigned)r.rx_unparsable, (unsigned)r.rx_truncated,
             (unsigned)r.rx_epoch_changes,
             r.have_first_rx ? r.first_rx_type : -1, (int)r.last_rx_type,
             r.last_rx_epoch == 0xFFFF ? -1 : (int)r.last_rx_epoch,
             (unsigned)r.last_rx_seq, (unsigned)r.last_rx_seq_lo, (unsigned)r.last_rx_len,
             r.fd_assoc == 1 ? "equal" : (r.fd_assoc == 0 ? "different" : "unknown"),
             r.dtls_fd, r.rtp_fd,
             (unsigned)r.sendto_calls, (unsigned)r.recvfrom_calls,
             (unsigned)r.conf_transport_calls, (unsigned)r.config_defaults_calls,
             (unsigned)r.setup_calls, (unsigned)r.config_free_calls,
             (unsigned)r.ssl_free_calls, (unsigned)r.transport_updates,
             r.errno_entry_last, r.errno_real_last,
             (unsigned)r.poll_samples, (int)r.poll_last_us, (int)r.poll_max_us);
}

/** Emit the report through ESP_LOG so it survives a failure path. */
void LogReport(const char* tag)
{
    const Report r = Snapshot();
    char buf[1200];
    Format(r, buf, sizeof(buf), tag);
    ESP_LOGW(TAG, "%s", buf);
    if (r.dtls_hs_calls == 0) {
        ESP_LOGE(TAG, "  no DATAGRAM (DTLS) handshake was observed at all; "
                      "%u STREAM handshakes were excluded, %u contexts were unknown "
                      "to a successful setup",
                 (unsigned)r.stream_hs_excluded, (unsigned)r.unknown_ctx);
    } else if (r.tx_records == 0 && r.tx_failed == 0 && r.tx_short_write == 0) {
        ESP_LOGE(TAG, "  no DTLS record was ever written to a socket");
    } else if (r.rx_records == 0) {
        // Only claim silence when the socket identity is actually known.
        if (r.fd_assoc == -1) {
            ESP_LOGE(TAG, "  %u DTLS records observed on TX and none on RX, but the "
                          "socket/context association is unknown: no conclusion drawn",
                     (unsigned)r.tx_records);
        } else {
            ESP_LOGE(TAG, "  %u DTLS records sent but none received on the associated "
                          "socket", (unsigned)r.tx_records);
        }
    } else if (r.hs_fail > 0 && r.hs_ok == 0) {
        ESP_LOGE(TAG, "  DTLS records flowed both ways (%u TX / %u RX) but the "
                      "handshake never returned success",
                 (unsigned)r.tx_records, (unsigned)r.rx_records);
    }
    // A wrapper that was never reached cannot explain anything: say so instead of
    // letting a zero counter look like a clean observation.
    if (r.config_free_calls == 0 || r.ssl_free_calls == 0) {
        ESP_LOGW(TAG, "  release wrappers not observed yet (config_free=%u ssl_free=%u): "
                      "no identity termination has been proven",
                 (unsigned)r.config_free_calls, (unsigned)r.ssl_free_calls);
    }
}

}  // namespace dtls_probe

namespace {

/** Real monotonic clock, only used by this translation unit. */
int64_t NowUs()
{
    return esp_timer_get_time();
}

/**
 * Public Mbed TLS getters, injected into the shared wrapper body so that
 * `dtls_probe_core.h` never assumes anything about Mbed TLS object layout.
 */
bool GetHandshakeOver(mbedtls_ssl_context* ssl)
{
    return mbedtls_ssl_is_handshake_over(ssl) != 0;
}

const char* GetCiphersuite(mbedtls_ssl_context* ssl)
{
    return mbedtls_ssl_get_ciphersuite(ssl);
}

/** MBEDTLS_TLS_SRTP_UNSET when no SRTP protection profile was negotiated. */
int GetSrtpProfile(mbedtls_ssl_context* ssl)
{
    mbedtls_dtls_srtp_info info;
    memset(&info, 0, sizeof(info));
    mbedtls_ssl_get_dtls_srtp_negotiation_result(ssl, &info);
    return (int)info.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile);
}

const dtls_probe::HandshakeGetters kGetters = {
    &GetHandshakeOver,
    &GetCiphersuite,
    &GetSrtpProfile,
};

}  // namespace

extern "C" void __wrap_mbedtls_ssl_conf_transport(mbedtls_ssl_config* conf, int transport)
{
    // Real signature is `void` (ssl.h:2143); nothing is returned, and an update
    // of an already-registered configuration is recorded rather than dropped.
    dtls_probe::WrapConfTransport(conf, transport);
}

extern "C" int __wrap_mbedtls_ssl_config_defaults(mbedtls_ssl_config* conf, int endpoint,
                                                  int transport, int preset)
{
    return dtls_probe::WrapConfigDefaults(conf, endpoint, transport, preset);
}

extern "C" int __wrap_mbedtls_ssl_setup(mbedtls_ssl_context* ssl,
                                        const mbedtls_ssl_config* conf)
{
    return dtls_probe::WrapSetup(ssl, conf);
}

extern "C" void __wrap_mbedtls_ssl_config_free(mbedtls_ssl_config* conf)
{
    // The real release symbol is mbedtls_ssl_config_free (ssl.h:5741). There is
    // no mbedtls_ssl_conf_free, so the previous wrapper observed nothing.
    dtls_probe::WrapConfigFree(conf);
}

extern "C" void __wrap_mbedtls_ssl_free(mbedtls_ssl_context* ssl)
{
    dtls_probe::WrapSslFree(ssl);
}

extern "C" int __wrap_mbedtls_ssl_handshake(mbedtls_ssl_context* ssl)
{
    const mbedtls_ssl_config* conf = (ssl != nullptr) ? mbedtls_ssl_context_get_config(ssl)
                                                      : nullptr;
    return dtls_probe::WrapHandshake(ssl, conf, kGetters, &NowUs);
}

extern "C" ssize_t __wrap_lwip_sendto(int s, const void* dataptr, size_t size, int flags,
                                      const struct sockaddr* to, socklen_t tolen)
{
    const ssize_t rc = __real_lwip_sendto(s, dataptr, size, flags, to, tolen);
    const int saved_errno = errno;      // immediately after the real call
    {
        dtls_probe::State& st = dtls_probe::Global();
        std::lock_guard<std::mutex> lock(st.mtx);
        ++st.rep.sendto_calls;
    }
    // A record only counts as sent when the real call reported the whole
    // datagram; a short or failed write is recorded as such instead.
    if (rc == (ssize_t)size) {
        dtls_probe::RecordDatagram((const uint8_t*)dataptr, size, true);
    } else {
        dtls_probe::State& st = dtls_probe::Global();
        std::lock_guard<std::mutex> lock(st.mtx);
        if (rc < 0) { ++st.rep.tx_failed; }
        else { ++st.rep.tx_short_write; }
    }
    // DTLS TX shares this socket with RTP/STUN. Each probe parses only what it
    // recognises, so one wrapper feeds both without double-counting.
    rtp_probe::ObserveSendto(dataptr, size, rc, saved_errno);
    errno = saved_errno;
    return rc;
}

extern "C" ssize_t __wrap_lwip_recvfrom(int s, void* mem, size_t len, int flags,
                                        struct sockaddr* from, socklen_t* fromlen)
{
    const ssize_t rc = __real_lwip_recvfrom(s, mem, len, flags, from, fromlen);
    const int saved_errno = errno;
    {
        dtls_probe::State& st = dtls_probe::Global();
        std::lock_guard<std::mutex> lock(st.mtx);
        ++st.rep.recvfrom_calls;
    }
    if (rc > 0) {
        // Never parse past the caller's buffer, even if rc claims more.
        const size_t n = ((size_t)rc < len) ? (size_t)rc : len;
        dtls_probe::RecordDatagram((const uint8_t*)mem, n, false);
    }
    errno = saved_errno;
    return rc;
}

#endif  // CONFIG_STACKCHAN_WEBRTC_M1
