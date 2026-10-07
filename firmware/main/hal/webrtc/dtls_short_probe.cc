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

/** Track the handshake context whose role/profile we may later report. */
mbedtls_ssl_context* g_last_ctx = nullptr;

inline uint16_t Be16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
inline uint32_t Be32(const uint8_t* p)
{
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

/**
 * @brief Parse a bounded classic DTLS record header.
 *
 * Reads only the fixed 13 bytes: type, version, epoch, 48-bit sequence, length.
 * Never touches the fragment body, so nothing sensitive is examined.
 */
bool ParseDtls(const uint8_t* d, size_t n, uint8_t* type, uint16_t* epoch,
               uint32_t* seq48_hi, uint32_t* length)
{
    if (d == nullptr || n < (size_t)kDtlsHeader) {
        return false;
    }
    const uint8_t t = d[0];
    if (!KnownType(t)) {
        return false;
    }
    *type = t;
    *epoch = Be16(d + 3);
    *seq48_hi = ((uint32_t)d[5] << 8) | d[6];          // high 16 bits of the 48-bit seq
    *length = Be16(d + 11);
    return true;
}

void RecordTx(const uint8_t* d, size_t n)
{
    uint8_t type; uint16_t epoch; uint32_t hi, len;
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!ParseDtls(d, n, &type, &epoch, &hi, &len)) {
        ++g_rep.tx_unparsable;
        return;
    }
    ++g_rep.tx_records;
    if (g_rep.last_tx_epoch != 0xFFFF && g_rep.last_tx_epoch != epoch) {
        ++g_rep.tx_epoch_changes;
    }
    g_rep.last_tx_epoch = epoch;
    g_rep.last_tx_seq = hi;
    g_rep.last_tx_len = len;
    g_rep.last_tx_type = type;
    if (!g_rep.have_first_tx) {
        g_rep.have_first_tx = true;
        g_rep.first_tx_type = type;
    }
}

void RecordRx(const uint8_t* d, size_t n)
{
    uint8_t type; uint16_t epoch; uint32_t hi, len;
    std::lock_guard<std::mutex> lock(g_mtx);
    if (!ParseDtls(d, n, &type, &epoch, &hi, &len)) {
        ++g_rep.rx_unparsable;
        return;
    }
    ++g_rep.rx_records;
    if (g_rep.last_rx_epoch != 0xFFFF && g_rep.last_rx_epoch != epoch) {
        ++g_rep.rx_epoch_changes;
    }
    g_rep.last_rx_epoch = epoch;
    g_rep.last_rx_seq = hi;
    g_rep.last_rx_len = len;
    g_rep.last_rx_type = type;
    if (!g_rep.have_first_rx) {
        g_rep.have_first_rx = true;
        g_rep.first_rx_type = type;
    }
}

}  // namespace

void Arm()
{
    std::lock_guard<std::mutex> lock(g_mtx);
    g_rep = Report();
    ++g_rep.generation;
    g_last_ctx = nullptr;
}

Report Snapshot()
{
    std::lock_guard<std::mutex> lock(g_mtx);
    return g_rep;
}

void Format(const Report& r, char* out, size_t out_len, const char* tag)
{
    snprintf(out, out_len,
             "[%s] gen=%u | handshake calls=%u ok=%u fail=%u last_ret=%d "
             "last=%dms total=%dms complete=%d | "
             "DTLS TX records=%u unparsable=%u epoch_changes=%u first_type=%d last_type=%d "
             "epoch=%d seq_hi=%u len=%u | "
             "DTLS RX records=%u unparsable=%u epoch_changes=%u first_type=%d last_type=%d "
             "epoch=%d seq_hi=%u len=%u | "
             "cipher=%s role=n/a profile=n/a | sendto=%u recvfrom=%u",
             tag, (unsigned)r.generation,
             (unsigned)r.hs_calls, (unsigned)r.hs_ok, (unsigned)r.hs_fail,
             r.hs_last_ret, (int)r.hs_last_ms, (int)r.hs_total_ms, (int)r.hs_complete,
             (unsigned)r.tx_records, (unsigned)r.tx_unparsable,
             (unsigned)r.tx_epoch_changes,
             r.have_first_tx ? r.first_tx_type : -1, (int)r.last_tx_type,
             r.last_tx_epoch == 0xFFFF ? -1 : (int)r.last_tx_epoch,
             (unsigned)r.last_tx_seq, (unsigned)r.last_tx_len,
             (unsigned)r.rx_records, (unsigned)r.rx_unparsable,
             (unsigned)r.rx_epoch_changes,
             r.have_first_rx ? r.first_rx_type : -1, (int)r.last_rx_type,
             r.last_rx_epoch == 0xFFFF ? -1 : (int)r.last_rx_epoch,
             (unsigned)r.last_rx_seq, (unsigned)r.last_rx_len,
             r.have_cipher ? r.cipher : "(none)",
             (unsigned)r.sendto_calls, (unsigned)r.recvfrom_calls);
}

/** Emit the report through ESP_LOG so it survives a failure path. */
void LogReport(const char* tag)
{
    const Report r = Snapshot();
    char buf[900];
    Format(r, buf, sizeof(buf), tag);
    ESP_LOGW(TAG, "%s", buf);
    if (r.tx_records == 0) {
        ESP_LOGE(TAG, "  no DTLS record was ever written to a socket");
    } else if (r.rx_records == 0) {
        ESP_LOGE(TAG, "  %u DTLS records sent but none received: the peer never "
                      "answered on this socket", (unsigned)r.tx_records);
    } else if (r.hs_fail > 0 && r.hs_ok == 0) {
        ESP_LOGE(TAG, "  DTLS records flowed both ways (%u TX / %u RX) but the "
                      "handshake never returned success",
                 (unsigned)r.tx_records, (unsigned)r.rx_records);
    }
}

}  // namespace dtls_probe

extern "C" int __wrap_mbedtls_ssl_handshake(mbedtls_ssl_context* ssl)
{
    using namespace dtls_probe;
    const int64_t t0 = esp_timer_get_time();
    const int rc = __real_mbedtls_ssl_handshake(ssl);
    const int64_t dt_us = esp_timer_get_time() - t0;
    const int saved_errno = errno;

    {
        std::lock_guard<std::mutex> lock(g_mtx);
        ++g_rep.hs_calls;
        if (rc == 0) {
            ++g_rep.hs_ok;
            g_rep.hs_complete = true;
        } else {
            ++g_rep.hs_fail;
        }
        g_rep.hs_last_ret = rc;
        g_rep.hs_last_ms = dt_us / 1000;
        g_rep.hs_total_ms += dt_us / 1000;
        g_last_ctx = ssl;

        // Only the cipher suite name is publicly obtainable here. There is no
        // public getter for the endpoint role or the selected SRTP profile in
        // this build, and the profile field is MBEDTLS_PRIVATE, so neither is
        // read: inferring them from private memory is not acceptable.
        if (ssl != nullptr && rc == 0) {
            const char* cs = mbedtls_ssl_get_ciphersuite(ssl);
            if (cs != nullptr) {
                g_rep.have_cipher = true;
                strncpy(g_rep.cipher, cs, sizeof(g_rep.cipher) - 1);
                g_rep.cipher[sizeof(g_rep.cipher) - 1] = '\0';
            }
        }
    }

    errno = saved_errno;
    return rc;
}

extern "C" ssize_t __wrap_lwip_sendto(int s, const void* dataptr, size_t size, int flags,
                                      const struct sockaddr* to, socklen_t tolen)
{
    const ssize_t rc = __real_lwip_sendto(s, dataptr, size, flags, to, tolen);
    const int saved_errno = errno;
    {
        std::lock_guard<std::mutex> lock(dtls_probe::g_mtx);
        ++dtls_probe::g_rep.sendto_calls;
    }
    // DTLS TX shares this socket with RTP/STUN. Each probe parses only what it
    // recognises, so one wrapper feeds both without double-counting.
    dtls_probe::RecordTx((const uint8_t*)dataptr, size);
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
        dtls_probe::RecordRx((const uint8_t*)mem, (size_t)rc);
    }
    errno = saved_errno;
    return rc;
}

#endif  // CONFIG_STACKCHAN_WEBRTC_M1
