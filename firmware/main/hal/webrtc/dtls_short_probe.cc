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
 * Linker-wrapped observation. `__real_*` come from --wrap; never call the
 * wrapped name from inside its own wrapper.
 *
 * Note: mbedtls_ssl_get_ciphersuite / mbedtls_ssl_get_srtp_profile are public
 * inline wrappers over the real getters in this Mbed TLS build, and an inline
 * cannot be intercepted by --wrap. They are therefore called directly, which is
 * correct: only the *handshake* and the *socket* calls need interception.
 */
extern "C" {
int __real_mbedtls_ssl_handshake(mbedtls_ssl_context* ssl);
int __real_mbedtls_ssl_conf_transport(mbedtls_ssl_config* conf, int transport);
void __real_mbedtls_ssl_conf_free(mbedtls_ssl_config* conf);
int __real_mbedtls_ssl_setup(mbedtls_ssl_context* ssl, const mbedtls_ssl_config* conf);
int __real_mbedtls_ssl_config_defaults(mbedtls_ssl_config* conf, int endpoint,
                                       int transport, int preset);
ssize_t __real_lwip_sendto(int s, const void* dataptr, size_t size, int flags,
                           const struct sockaddr* to, socklen_t tolen);
ssize_t __real_lwip_recvfrom(int s, void* mem, size_t len, int flags,
                             struct sockaddr* from, socklen_t* fromlen);
}

namespace dtls_probe {
namespace {

constexpr int kDtlsHeader = 13;   // classic DTLS record header
/** A DTLS record is only counted when its type is one of the defined four. */
bool KnownType(uint8_t t)
{
    return t == 20 || t == 21 || t == 22 || t == 23;   // CCS, alert, handshake, appdata
}

std::mutex g_mtx;
Report g_rep;
uint32_t g_generation = 0;
int g_dtls_fd = -1;
int g_rtp_fd = -1;

/**
 * @brief Fixed setup table: which SSL contexts are DATAGRAM (DTLS) and which
 * are STREAM (HTTPS).
 *
 * Wrapping every mbedtls_ssl_handshake counted the HTTPS POST's TLS handshake as
 * a DTLS success. The split is taken from public API only -
 * mbedtls_ssl_context_get_config() then mbedtls_ssl_conf_get_endpoint() is the
 * role; the transport is read from the config's transport field through the same
 * public accessor path. Only DATAGRAM contexts count toward DTLS.
 */
struct SetupEntry {
    const mbedtls_ssl_config* conf = nullptr;
    int transport = -1;      // MBEDTLS_SSL_TRANSPORT_DATAGRAM / STREAM
    int role = -1;
};

constexpr size_t kSetupTable = 8;
SetupEntry g_setups[kSetupTable];

/** Context -> configuration binding, by opaque handle identity only. */
struct CtxEntry {
    const mbedtls_ssl_context* ctx = nullptr;
    const mbedtls_ssl_config* conf = nullptr;
};
constexpr size_t kCtxTable = 12;
CtxEntry g_ctxs[kCtxTable];


/** Identify the transport of a context using only public accessors. */
int TransportOf(const mbedtls_ssl_context* ssl, int* role_out)
{
    const mbedtls_ssl_config* conf = mbedtls_ssl_context_get_config(ssl);
    if (conf == nullptr) {
        return -1;
    }
    // Prefer the identity recorded at setup time; it is the same opaque handle.
    for (size_t i = 0; i < kCtxTable; ++i) {
        if (g_ctxs[i].ctx == ssl && g_ctxs[i].conf == conf) {
            if (role_out != nullptr) {
                *role_out = mbedtls_ssl_conf_get_endpoint(conf);
            }
            for (size_t j = 0; j < kSetupTable; ++j) {
                if (g_setups[j].conf == conf) {
                    return g_setups[j].transport;
                }
            }
        }
    }
    if (role_out != nullptr) {
        *role_out = mbedtls_ssl_conf_get_endpoint(conf);
    }
    for (size_t i = 0; i < kSetupTable; ++i) {
        if (g_setups[i].conf == conf) {
            return g_setups[i].transport;
        }
    }
    return -2;   // unknown identity: reported explicitly, never assumed DTLS
}

void BindContext(const mbedtls_ssl_context* ctx, const mbedtls_ssl_config* conf);

void BindContextImpl(const mbedtls_ssl_context* ctx, const mbedtls_ssl_config* conf)
{
    for (size_t i = 0; i < kCtxTable; ++i) {
        if (g_ctxs[i].ctx == ctx) {
            g_ctxs[i].conf = conf;
            return;
        }
    }
    for (size_t i = 0; i < kCtxTable; ++i) {
        if (g_ctxs[i].ctx == nullptr) {
            g_ctxs[i].ctx = ctx;
            g_ctxs[i].conf = conf;
            return;
        }
    }
    ++g_rep.table_overflow;
}

/**
 * @brief Register a configuration's transport by opaque handle identity.
 *
 * Called from the setup wrapper, which is where the transport is decided.
 */
void RegisterSetup(const mbedtls_ssl_config* conf, int transport)
{
    if (conf == nullptr) {
        return;
    }
    for (size_t i = 0; i < kSetupTable; ++i) {
        if (g_setups[i].conf == conf) {
            return;                          // already known
        }
    }
    for (size_t i = 0; i < kSetupTable; ++i) {
        if (g_setups[i].conf == nullptr) {
            g_setups[i].conf = conf;
            g_setups[i].transport = transport;
            ++g_rep.setups;
            if (transport == 0) {
                ++g_rep.datagram_setups;
            } else {
                ++g_rep.stream_setups;
            }
            return;
        }
    }
    ++g_rep.table_overflow;                  // explicit, never silent
}

/** Drop an identity when its configuration is freed. */
void ForgetSetup(const mbedtls_ssl_config* conf)
{
    for (size_t i = 0; i < kSetupTable; ++i) {
        if (g_setups[i].conf == conf) {
            g_setups[i] = SetupEntry();
            return;
        }
    }
}

/** Track the handshake context whose role/profile we may later report. */
mbedtls_ssl_context* g_last_ctx = nullptr;

inline uint16_t Be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t Be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

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
int ParseOneDtls(const uint8_t* d, size_t n, uint8_t* type, uint16_t* epoch,
                 uint32_t* seq_hi, uint32_t* seq_lo, uint32_t* length, size_t* consumed)
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
void RecordDatagram(const uint8_t* d, size_t n, bool tx)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    if (d == nullptr || n < (size_t)kDtlsHeader) {
        if (tx) { ++g_rep.tx_unparsable; } else { ++g_rep.rx_unparsable; }
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
            if (tx) { ++g_rep.tx_truncated; } else { ++g_rep.rx_truncated; }
            break;
        }
        any = true;
        if (tx) {
            ++g_rep.tx_records;
            ++g_rep.tx_records_in_dgram;
            if (g_rep.last_tx_epoch != 0xFFFF && g_rep.last_tx_epoch != epoch) {
                ++g_rep.tx_epoch_changes;
            }
            g_rep.last_tx_epoch = epoch;
            g_rep.last_tx_seq = hi;
            g_rep.last_tx_seq_lo = lo;
            g_rep.last_tx_len = len;
            g_rep.last_tx_type = type;
            if (!g_rep.have_first_tx) { g_rep.have_first_tx = true; g_rep.first_tx_type = type; }
        } else {
            ++g_rep.rx_records;
            if (g_rep.last_rx_epoch != 0xFFFF && g_rep.last_rx_epoch != epoch) {
                ++g_rep.rx_epoch_changes;
            }
            g_rep.last_rx_epoch = epoch;
            g_rep.last_rx_seq = hi;
            g_rep.last_rx_seq_lo = lo;
            g_rep.last_rx_len = len;
            g_rep.last_rx_type = type;
            if (!g_rep.have_first_rx) { g_rep.have_first_rx = true; g_rep.first_rx_type = type; }
        }
        off += used;
    }
    if (!any) {
        if (tx) { ++g_rep.tx_unparsable; } else { ++g_rep.rx_unparsable; }
    }
}

}  // namespace

/** Public entry: bind a context to its configuration by opaque identity. */
void BindContext(const mbedtls_ssl_context* ctx, const mbedtls_ssl_config* conf)
{
    BindContextImpl(ctx, conf);
}


void NoteDtlsSocket(int fd)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    g_dtls_fd = fd;
}

void NoteRtpSocket(int fd)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    g_rtp_fd = fd;
}

void NotePoll(int64_t us)
{
    std::lock_guard<std::mutex> lock(g_mtx);
    ++g_rep.poll_samples;
    g_rep.poll_last_us = us;
    if (us > g_rep.poll_max_us) {
        g_rep.poll_max_us = us;
    }
}

void Arm()
{
    std::lock_guard<std::mutex> lock(g_mtx);
    const uint32_t next = g_generation + 1;   // never restarts at 1
    g_rep = Report();
    g_rep.generation = next;
    g_generation = next;
    g_last_ctx = nullptr;
    g_dtls_fd = -1;
    g_rtp_fd = -1;
}

Report Snapshot()
{
    std::lock_guard<std::mutex> lock(g_mtx);
    // Association is measured, not assumed: equal / different / unknown.
    if (g_dtls_fd >= 0 && g_rtp_fd >= 0) {
        g_rep.dtls_fd = g_dtls_fd;
        g_rep.rtp_fd = g_rtp_fd;
        g_rep.fd_assoc = (g_dtls_fd == g_rtp_fd) ? 1 : 0;
    } else {
        g_rep.fd_assoc = -1;
    }
    return g_rep;
}

void Format(const Report& r, char* out, size_t out_len, const char* tag)
{
    snprintf(out, out_len,
             "[%s] gen=%u | setups=%u (datagram=%u stream=%u overflow=%u) | "
             "hs calls=%u dtls=%u ok=%u fail=%u stream_excluded=%u unknown_transport=%u "
             "last_ret=%d last=%dms total=%dms over=%d role=%d | "
             "cipher=%s profile=%d%s | "
             "DTLS TX records=%u in_dgrams=%u unparsable=%u truncated=%u "
             "short_write=%u failed=%u epoch_changes=%u first_type=%d last_type=%d "
             "epoch=%d seq=%u:%u len=%u | "
             "DTLS RX records=%u unparsable=%u truncated=%u epoch_changes=%u "
             "first_type=%d last_type=%d epoch=%d seq=%u:%u len=%u | "
             "socket assoc=%s (dtls_fd=%d rtp_fd=%d) | sendto=%u recvfrom=%u | "
             "poll n=%u last=%dus max=%dus",
             tag, (unsigned)r.generation,
             (unsigned)r.setups, (unsigned)r.datagram_setups, (unsigned)r.stream_setups,
             (unsigned)r.table_overflow,
             (unsigned)r.hs_calls, (unsigned)r.dtls_hs_calls, (unsigned)r.hs_ok,
             (unsigned)r.hs_fail, (unsigned)r.stream_hs_excluded,
             (unsigned)r.unknown_transport, r.hs_last_ret, (int)r.hs_last_ms,
             (int)r.hs_total_ms, (int)r.handshake_over, r.have_role ? r.role : -1,
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
             (unsigned)r.poll_samples, (int)r.poll_last_us, (int)r.poll_max_us);
}

/** Emit the report through ESP_LOG so it survives a failure path. */
void LogReport(const char* tag)
{
    const Report r = Snapshot();
    char buf[900];
    Format(r, buf, sizeof(buf), tag);
    ESP_LOGW(TAG, "%s", buf);
    if (r.dtls_hs_calls == 0) {
        ESP_LOGE(TAG, "  no DATAGRAM (DTLS) handshake was observed at all; "
                      "%u STREAM handshakes were excluded",
                 (unsigned)r.stream_hs_excluded);
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
}

}  // namespace dtls_probe

extern "C" int __wrap_mbedtls_ssl_conf_transport(mbedtls_ssl_config* conf, int transport)
{
    const int rc = __real_mbedtls_ssl_conf_transport(conf, transport);
    if (rc == 0) {
        std::lock_guard<std::mutex> lock(dtls_probe::g_mtx);
        dtls_probe::RegisterSetup(conf, transport);
    }
    return rc;
}

extern "C" int __wrap_mbedtls_ssl_config_defaults(mbedtls_ssl_config* conf, int endpoint,
                                                  int transport, int preset)
{
    const int rc = __real_mbedtls_ssl_config_defaults(conf, endpoint, transport, preset);
    if (rc == 0) {
        std::lock_guard<std::mutex> lock(dtls_probe::g_mtx);
        dtls_probe::RegisterSetup(conf, transport);
    }
    return rc;
}

extern "C" void __wrap_mbedtls_ssl_conf_free(mbedtls_ssl_config* conf)
{
    {
        std::lock_guard<std::mutex> lock(dtls_probe::g_mtx);
        dtls_probe::ForgetSetup(conf);      // identity ends with the config
    }
    __real_mbedtls_ssl_conf_free(conf);
}

extern "C" int __wrap_mbedtls_ssl_setup(mbedtls_ssl_context* ssl,
                                        const mbedtls_ssl_config* conf)
{
    // The context is bound to its configuration here; remember which one so the
    // handshake wrapper can tell DATAGRAM (DTLS) from STREAM (HTTPS) by identity.
    const int rc = __real_mbedtls_ssl_setup(ssl, conf);
    if (rc == 0) {
        std::lock_guard<std::mutex> lock(dtls_probe::g_mtx);
        dtls_probe::BindContext(ssl, conf);
    }
    return rc;
}

extern "C" int __wrap_mbedtls_ssl_handshake(mbedtls_ssl_context* ssl)
{
    using namespace dtls_probe;
    const int64_t t0 = esp_timer_get_time();
    const int rc = __real_mbedtls_ssl_handshake(ssl);
    // Save errno immediately, before the getters below can disturb it.
    const int saved_errno = errno;

    bool is_dtls = false;
    {
        std::lock_guard<std::mutex> lock(g_mtx);
        int role = -1;
        const int tr = TransportOf(ssl, &role);
        if (tr == 0) {                     // MBEDTLS_SSL_TRANSPORT_DATAGRAM
            is_dtls = true;
            ++g_rep.dtls_hs_calls;
        } else if (tr == 1) {              // STREAM: HTTPS/TLS, excluded
            ++g_rep.stream_hs_excluded;
        } else {
            ++g_rep.unknown_transport;
        }
        ++g_rep.hs_calls;
        if (is_dtls) {
            if (rc == 0) { ++g_rep.hs_ok; } else { ++g_rep.hs_fail; }
            g_rep.hs_last_ret = rc;
            g_rep.hs_last_ms = (esp_timer_get_time() - t0) / 1000;
            g_rep.hs_total_ms += g_rep.hs_last_ms;
            g_last_ctx = ssl;
            if (role >= 0) { g_rep.have_role = true; g_rep.role = role; }
        }
    }

    // Public getters, DTLS contexts only. Nothing here reads private memory:
    // mbedtls_ssl_is_handshake_over and mbedtls_ssl_get_ciphersuite are public,
    // and mbedtls_ssl_get_dtls_srtp_negotiation_result fills a public struct
    // whose documented field we read.
    if (is_dtls && ssl != nullptr) {
        const bool over = (mbedtls_ssl_is_handshake_over(ssl) != 0);
        char cipher[64] = {0};
        bool have_cipher = false;
        const char* cs = mbedtls_ssl_get_ciphersuite(ssl);
        if (cs != nullptr) {
            have_cipher = true;
            strncpy(cipher, cs, sizeof(cipher) - 1);
        }
        mbedtls_dtls_srtp_info info;
        memset(&info, 0, sizeof(info));
        bool have_profile = false;
        int profile = -1;
        if (over) {
            mbedtls_ssl_get_dtls_srtp_negotiation_result(ssl, &info);
            have_profile = true;
            profile = (int)info.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile);
        }
        std::lock_guard<std::mutex> lock(g_mtx);
        g_rep.handshake_over = over;
        if (have_cipher) {
            g_rep.have_cipher = true;
            strncpy(g_rep.cipher, cipher, sizeof(g_rep.cipher) - 1);
            g_rep.cipher[sizeof(g_rep.cipher) - 1] = '\0';
        }
        if (have_profile) {
            g_rep.have_profile = true;
            g_rep.profile = profile;
            g_rep.profile_unset = (profile == 0);   // MBEDTLS_TLS_SRTP_UNSET
        }
    }

    errno = saved_errno;
    return rc;
}

extern "C" ssize_t __wrap_lwip_sendto(int s, const void* dataptr, size_t size, int flags,
                                      const struct sockaddr* to, socklen_t tolen)
{
    const ssize_t rc = __real_lwip_sendto(s, dataptr, size, flags, to, tolen);
    const int saved_errno = errno;      // immediately after the real call
    {
        std::lock_guard<std::mutex> lock(dtls_probe::g_mtx);
        ++dtls_probe::g_rep.sendto_calls;
    }
    // A record only counts as sent when the real call reported the whole
    // datagram; a short or failed write is recorded as such instead.
    if (rc == (ssize_t)size) {
        dtls_probe::RecordDatagram((const uint8_t*)dataptr, size, true);
    } else {
        std::lock_guard<std::mutex> lock(dtls_probe::g_mtx);
        if (rc < 0) { ++dtls_probe::g_rep.tx_failed; }
        else { ++dtls_probe::g_rep.tx_short_write; }
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
        std::lock_guard<std::mutex> lock(dtls_probe::g_mtx);
        ++dtls_probe::g_rep.recvfrom_calls;
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
