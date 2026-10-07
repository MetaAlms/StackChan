#include <sdkconfig.h>

// PROBE_GUARD
#if CONFIG_STACKCHAN_WEBRTC_M1

#include <cstddef>
#include <cstdint>

/**
 * @brief Short DTLS diagnostic for the device's intermittent handshake timeout.
 *
 * The device has repeatedly stopped at `stage=server answer received`,
 * `peer_state=CONNECTING` with no way to tell whether DTLS records were sent,
 * received, or never arrived. This observes the three real boundaries using
 * linker wrapping of unresolved externals in the pinned esp_peer 1.5.5 objects:
 *
 *   - `mbedtls_ssl_handshake`  : the real return code and monotonic duration
 *   - `lwip_sendto`            : actual UDP writes (shared with the RTP probe)
 *   - `lwip_recvfrom`          : actual UDP reads, so RX can be distinguished
 *                                from "never reached the socket"
 *
 * Only bounded, non-sensitive metadata is recorded: a 13-byte classic DTLS
 * record header (content type, version, epoch, sequence, length), the return
 * codes, errno, and the public getters (role / cipher suite name / selected
 * SRTP profile enum). No payload, keys, random, certificate, MKI, IP or SDP
 * credentials are read or printed.
 */
namespace dtls_probe {

/** Start a fresh short diagnostic generation; clears pending registrations. */
void Arm();

struct Report {
    uint32_t generation = 0;

    // mbedtls_ssl_handshake
    uint32_t hs_calls = 0;
    uint32_t hs_ok = 0;
    uint32_t hs_fail = 0;
    int hs_last_ret = 0;
    int64_t hs_last_ms = 0;
    int64_t hs_total_ms = 0;

    // DTLS records seen on the socket
    uint32_t tx_records = 0;
    uint32_t rx_records = 0;
    uint32_t tx_unparsable = 0;     // not a DTLS record at all
    uint32_t tx_truncated = 0;      // header present, declared length exceeds data
    uint32_t tx_short_write = 0;    // real call returned < requested
    uint32_t tx_failed = 0;         // real call returned < 0
    uint32_t tx_records_in_dgram = 0;  // total records parsed across datagrams
    uint32_t rx_truncated = 0;
    uint32_t rx_unparsable = 0;
    uint32_t tx_epoch_changes = 0;
    uint32_t rx_epoch_changes = 0;
    uint16_t last_tx_epoch = 0xFFFF;
    uint16_t last_rx_epoch = 0xFFFF;
    uint32_t last_tx_seq = 0;
    uint32_t last_tx_seq_lo = 0;
    uint32_t last_rx_seq = 0;
    uint32_t last_rx_seq_lo = 0;
    uint32_t last_tx_len = 0;
    uint32_t last_rx_len = 0;
    uint8_t last_tx_type = 0xFF;
    uint8_t last_rx_type = 0xFF;
    // First record of each direction, for the handshake-stage picture.
    bool have_first_tx = false;
    bool have_first_rx = false;
    uint8_t first_tx_type = 0xFF;
    uint8_t first_rx_type = 0xFF;

    // All of these come from public API: mbedtls_ssl_get_ciphersuite,
    // mbedtls_ssl_conf_get_endpoint (via mbedtls_ssl_context_get_config),
    // mbedtls_ssl_is_handshake_over, and
    // mbedtls_ssl_get_dtls_srtp_negotiation_result whose documented out-parameter
    // is mbedtls_dtls_srtp_info. Reading that struct's documented field is using
    // the public contract, not guessing at private layout.
    bool have_cipher = false;
    char cipher[64] = {0};
    bool have_role = false;
    int role = -1;                 // MBEDTLS_SSL_IS_CLIENT / SERVER
    bool handshake_over = false;   // mbedtls_ssl_is_handshake_over()
    bool have_profile = false;
    int profile = -1;              // chosen_dtls_srtp_profile (SRTP protection profile)
    bool profile_unset = false;
    /** DTLS contexts only: STREAM (HTTPS) handshakes are counted separately. */
    uint32_t dtls_hs_calls = 0;
    uint32_t stream_hs_excluded = 0;
    uint32_t unknown_transport = 0;
    /** Setup identities seen, to prove the DATAGRAM/STREAM split is real. */
    uint32_t setups = 0;
    uint32_t datagram_setups = 0;
    uint32_t stream_setups = 0;
    uint32_t table_overflow = 0;
    /** Transport.poll() enter->exit duration, monotonic. */
    uint32_t poll_samples = 0;
    int64_t poll_last_us = 0;
    int64_t poll_max_us = 0;

    uint32_t sendto_calls = 0;
    uint32_t recvfrom_calls = 0;
    /** Socket/context association: equal / different / unknown, anonymous ids. */
    int fd_assoc = -1;              // 1 equal, 0 different, -1 unknown
    int dtls_fd = -1;
    int rtp_fd = -1;
};

Report Snapshot();
void Format(const Report& r, char* out, size_t out_len, const char* tag);

/** Emit the report through the logger so it survives a failure path. */
void LogReport(const char* tag);

/** Record the socket the DTLS/ICE transport actually uses (for association). */
void NoteDtlsSocket(int fd);

/** Record the socket the RTP sender uses. */
void NoteRtpSocket(int fd);

/** Wrap-and-time one transport poll iteration (monotonic). */
void NotePoll(int64_t us);

}  // namespace dtls_probe

#endif  // CONFIG_STACKCHAN_WEBRTC_M1
