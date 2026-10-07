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
    uint32_t tx_unparsable = 0;
    uint32_t rx_unparsable = 0;
    uint32_t tx_epoch_changes = 0;
    uint32_t rx_epoch_changes = 0;
    uint16_t last_tx_epoch = 0xFFFF;
    uint16_t last_rx_epoch = 0xFFFF;
    uint32_t last_tx_seq = 0;
    uint32_t last_rx_seq = 0;
    uint32_t last_tx_len = 0;
    uint32_t last_rx_len = 0;
    uint8_t last_tx_type = 0xFF;
    uint8_t last_rx_type = 0xFF;
    // First record of each direction, for the handshake-stage picture.
    bool have_first_tx = false;
    bool have_first_rx = false;
    uint8_t first_tx_type = 0xFF;
    uint8_t first_rx_type = 0xFF;

    // Only the cipher suite *name* is publicly obtainable in this Mbed TLS
    // build: mbedtls_ssl_get_ciphersuite() exists, but there is no public getter
    // for the endpoint role or for the selected SRTP profile (the field is
    // MBEDTLS_PRIVATE, and guessing at the struct layout is exactly what must
    // not be done here). Those two are therefore reported as unavailable rather
    // than read through private memory.
    bool have_cipher = false;
    char cipher[64] = {0};
    bool hs_complete = false;

    uint32_t sendto_calls = 0;
    uint32_t recvfrom_calls = 0;
};

Report Snapshot();
void Format(const Report& r, char* out, size_t out_len, const char* tag);

/** Emit the report through the logger so it survives a failure path. */
void LogReport(const char* tag);

}  // namespace dtls_probe

#endif  // CONFIG_STACKCHAN_WEBRTC_M1
