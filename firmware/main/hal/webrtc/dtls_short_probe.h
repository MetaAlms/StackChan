#include <sdkconfig.h>

// PROBE_GUARD
#if CONFIG_STACKCHAN_WEBRTC_M1

#include <cstddef>
#include <cstdint>

#include "dtls_probe_core.h"

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
 *
 * Identity, classification and the errno/return discipline live in
 * `dtls_probe_core.h`, which is ESP-independent so the same code is executed by
 * `firmware/tests/dtls_probe_offline_check.cc`. See that header for the D3-1 /
 * D3-2 defects this shape fixes.
 */
namespace dtls_probe {

/** Emit the report through the logger so it survives a failure path. */
void Format(const Report& r, char* out, size_t out_len, const char* tag);

void LogReport(const char* tag);

/** Record the socket the DTLS/ICE transport actually uses (for association). */
void NoteDtlsSocket(int fd);

/** Record the socket the RTP sender uses. */
void NoteRtpSocket(int fd);

/** Wrap-and-time one transport poll iteration (monotonic). */
void NotePoll(int64_t us);

}  // namespace dtls_probe

#endif  // CONFIG_STACKCHAN_WEBRTC_M1
