# Minimal DTLS short diagnostic to unblock media observation

Read-only proposal following exact 1cbd79bbe60f53f688cdcf512a0f60ec9cd8f42f review.
No code changes, build, network/device/cloud session, credentials, or packet/key
computations performed. Goal: next connection attempt leaves useful evidence even
when it never reaches CONNECTED/media. This is not a claimed DTLS bug fix.

## Verified link boundaries for pinned esp_peer1.5.5/S3

Official esp_peer source commit c8650846b512e6e1375e5f78c1c41619b8d645eb;
libpeer_default.a sha25625a338fc6b05f702947c2af0e924f2dd3006503908e889dcdc512782d0ea3a58.

Distinguish binary vs source objects precisely:
- prebuilt peer_default.c.obj has U dtls_srtp_handshake;
  prebuilt agent.c.obj has U udp_socket_sendto/udp_socket_recvfrom_nowait.
- The companion open-source compiled dtls_srtp.c.obj has U
  mbedtls_ssl_handshake, mbedtls_ssl_get_ciphersuite,
  mbedtls_ssl_get_dtls_srtp_negotiation_result, mbedtls_ssl_config_defaults,
  mbedtls_ssl_setup, mbedtls_ssl_free.
- Compiled udp.c.obj has U lwip_sendto/lwip_recvfrom and W
  udp_socket_sendto/udp_socket_recvfrom_nowait.
- Thus GNU ld --wrap works on these unresolved callsites. Do not try to replace
  static peer/dtls helpers or access private peer offsets. Existing nm evidence
  was read, no new build occurred; implementer must verify actual final link and
  callsites for the next candidate.

## Small tracked implementation path

1. Use a separate short handshake-probe generation armed **before Transport.Start**
   (before esp_peer_open/init), not the media Arm at M1:1348 after handshake/gate.
   Export the fixed metadata report on every connection/config failure path
   before it returns, and on successful connection before starting the sender.
   Stop/join old workers before clearing the next generation. No codec/resampler
   work is needed in this probe, so it does not reproduce main's Opus stack issue.
2. Reuse the existing lwip_sendto observer for transmit DTLS records, and add
   --wrap=lwip_recvfrom for actual received UDP datagrams. Both use public
   lwip/sockets.h:608-614 signatures. Capture only successful bounded input bytes
   needed for fixed headers; after real recvfrom parse at most min(rc, len) if
   rc>0. Log negative rc/errno separately. Preserve original rc/errno, flags,
   destination/source handling, buffers, retry behavior. No raw payload/IP output.
3. Add --wrap=mbedtls_ssl_handshake to record each actual raw return code and
   bounded elapsed time, including WANT_READ/WANT_WRITE, HELLO_VERIFY_REQUIRED,
   TIMEOUT and terminal failures. Call __real_mbedtls_ssl_handshake and return it
   unchanged, preserving errno. This preserves -0x6800 before
   dtls_srtp_handshake_client converts nonzero into -1 at dtls_common.h:615-617.
4. To scope this to DTLS and report real role without inspecting ssl->conf:
   observe public mbedtls_ssl_config_defaults(conf,endpoint,transport,preset)
   arguments (source dtls_srtp.c:168-181), then public
   mbedtls_ssl_setup(ssl,conf) (source :199; reconfiguration :286). A fixed small table maps successful
   DTLS conf registrations to SSL pointer identities and known endpoint role.
   Compare handles only, never dereference/log them. Include generation, table
   overflow/unregistered counters, and invalidate on public mbedtls_ssl_free or
   reset when old generation is joined. This excludes TLS/HTTPS POST handshakes
   which have STREAM transport; do not combine their successful handshakes with
   the WebRTC one. Reconfiguration can change endpoint for a known conf, so use
   the latest successful public config observation. Every observed call must
   forward unchanged; no public argument is altered.
5. On a tracked DTLS handshake observation query the **public getters**
   mbedtls_ssl_get_ciphersuite and
   mbedtls_ssl_get_dtls_srtp_negotiation_result. Store only cipher name/id,
   returned chosen SRTP profile enum and endpoint role. The getter's output is
   an officially declared mbedtls_dtls_srtp_info; use installed headers/member
   names (MBEDTLS_PRIVATE macro where required), never guessed byte offsets.
   Do not output MKI bytes, randoms, secrets, policies, certificates or fingerprints.
   Label profile as selected/UNSET plus handshake rc: a selected profile during
   a failed handshake does not prove the handshake completed. Full completion
   requires actual handshake rc0/peer CONNECTED. Cipher may be absent on failure.
6. Keep fixed aggregate counters plus a bounded chronological sample ring (e.g.
   first/last16 plus error records). No per-record printf, dynamic allocation,
   payload copy or I/O in wrappers; locks only briefly update metadata and never
   span the real socket/handshake call. Emit summary from a safe task. Runtime
   record counters and sample-ring loss must be explicit, not called packet loss.
7. Next controlled attempt should deliver source SHA, relevant link options/map
   proof, ELF fingerprint, handshake/record report and actual reached stage. A
   failed handshake still yields the report; do not call missing media evidence
   a SRTP/Opus/ASR result or repeat blind full media attempts.

## DTLS record parser whitelist and bounds

For classic DTLS1.0/1.2 records (this peer forces DTLS1.2):
- Require at least13 bytes and version FEFF or FEFD. Record type20(CCS),21(Alert),
  22(Handshake),23(ApplicationData) are sufficient for this diagnostic.
- Fixed offsets: type[0], version[1..2], epoch=BE16[3..4],
  sequence48[5..10], record_length=BE16[11..12].
- Require record_length <= remaining_datagram_bytes-13, count malformed/truncated
  otherwise. Iterate all records in the datagram using13+record_length, with
  bounded record-count and offset arithmetic. Count trailing/truncated/unsupported
  framing separately. Never examine fragment/handshake body or decrypt anything.
- Store sequence as16-bit high plus32-bit low and print unsigned fields/hex;
  do not use %llu/%llx with configured newlib-nano. Metadata also includes
  direction, local time, datagram length, actual write/read rc and errno on errors.
- Do not treat STUN/RTP/other UDP as DTLS; DTLS parsing based on header is raw
  network observation, not proof MbedTLS consumed/authenticated the record.
  With one active WebRTC DTLS transport the scoped attempt gives useful counts;
  if additional DTLS users exist, correlate the FD discovered during a tracked
  handshake's send (same task) or label association unknown. Do not infer a
  peer/session from raw IPs or opaque socket memory.
- TURN envelopes/TCP/TLS relay need separate framing. This parser deliberately
  covers current direct UDP. Unknown framing means unsupported coverage, not
  an asserted absence of server traffic.

## How this evidence narrows hypotheses, without asserting root cause

- DTLS TX writes fail => local write path has a concrete failing rc/errno.
- DTLS TX complete writes and no observedRX => local receive absence; cannot
  assign responsibility to server/network/NAT based on local success alone.
- RX DTLS records exist but handshake keeps timing out => next examine dispatch,
  BIO consumption, retransmission/timer, certificate or handshake parsing stages;
  UDP receipt alone does not prove delivery to MbedTLS. No early-drop defect is
  established by this pattern alone. If needed, a later tracked wrapper around
  the public BIO registration/callback contract can distinguish ingestion.
- Handshake rc0 + selected profile0x0001 => current default AES128-CM/SHA1-80
  policy matches; SHA384 exporter hypothesis is already exonerated by source.
- Selected0x0002/NULL orUNSET => source's unconditional default SRTP policy is a
  concrete conditional concern; still do not attribute currentzeroVAD without
  actual profile observation.
- No metadata probe is end-to-end proof of remote receipt/decryption/Opus/VAD/ASR.

Primary references:
- GNU ld --wrap: https://sourceware.org/binutils/docs/ld/Options.html
- DTLS record syntax: https://www.rfc-editor.org/rfc/rfc6347.html#section-4.3.1
- Fixed peer public source:
  https://github.com/espressif/esp-webrtc-solution/blob/c8650846b512e6e1375e5f78c1c41619b8d645eb/components/esp_peer/src/dtls_common.h
- Installed public MbedTLS signatures ssl.h:2094 (setup),5101(handshake),
  4860(cipher getter),4383(profile getter),5569(free),5733(config defaults).
