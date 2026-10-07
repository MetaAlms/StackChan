# Bounded DTLS/SRTP exporter hypothesis audit

Read-only source audit, not a runtime fault diagnosis. No source edits, build,
hardware/cloud/key access, key dumps, or calculations on live values.

Pinned esp_peer 1.5.5 official commit:
`c8650846b512e6e1375e5f78c1c41619b8d645eb`.
`src/dtls_common.h` SHA256:
`ec2bf924bf661a2674102d22a28ddf1b6b2476411a91cad9f4e267fa7233e129`;
byte-equal to pinned upstream snapshot already saved beside this document.
libsrtp component 1.0.0 official commit:
`f4bba1409f29901a18a3e3912f614fbd8cf0f937`.
`libsrtp/srtp/srtp.c` SHA256:
`9670a16201f905f72fa187f43e68b90ea45d0480a71579dd4b0fc38340417f52`.
Local Mbed TLS is3.6.5 (include/mbedtls/build_info.h:36-38).

## Exoneration: no hardcoded SHA256 exporter

- `dtls_common.h:143-146` takes tls_prf_type from the Mbed TLS key-export callback.
  :169-170 passes that same argument to mbedtls_ssl_tls_prf. The SHA256 at :105
  is certificate fingerprint hashing, not exporter PRF selection.
- IDF local `components/mbedtls/mbedtls/include/mbedtls/ssl.h:1399-1420` documents
  callback tls_prf_type as the PRF used in that handshake.
- IDF local `library/ssl_tls.c:6999-7002` chooses TLS1.2 SHA384 PRF when suite hash
  isSHA384; :8596-8606 converts the function to SHA384/SHA256 enum;
  :8942-8947 exports master secret, the two random inputs, and that enum.
  :482-488 selects the requested SHA384/SHA256 implementation.
- Thus AES-256-GCM-SHA384 does not cause this source to secretly use SHA256.
  Public logs run7:235 and run9:234 show that cipher; they do not show an actual
  selected SRTP protection-profile ID.

## Exoneration: exporter seed and key/salt slices

- :150 uses label EXTRACTOR-dtls_srtp. :155-156 forms client_random||server_random,
  exactly the no-context exporter seed. Not server_random||client_random.
- `dtls_srtp.h:49-51` defines key16, salt14, combined exporter60 bytes. Those sizes
  are right for AES128-CM/SHA1-80 and AES128-CM/SHA1-32 profiles, irrespective of
  TLS AES256 versus AES128; TLS cipher strength does not set SRTP profile.
- Keying-material segments are client key[0,16), server key[16,32), client
  salt[32,46), server salt[46,60). These are ranges in the algorithm, not live
  values; none were read or computed here.
- :179-181 puts client key+salt in the first policy; :197-200 server key+salt in
  second. :186 directs first to srtp_in for DTLSserver, srtp_out forDTLSclient;
  :205 directs second to srtp_out forserver, srtp_in forclient. The **key material
  direction mapping is correct** for both roles. The names remote_policy,
  local_policy, send_session/recv_session alone are misleading forclient.
- :56-61 forcesDTLS1.2, so ignoring secret_type at :154 is not presently a TLS1.3
  exporter bug; MbedTLS passes TLS12 master secret at ssl_tls.c:8943.

## Concrete conditional risk: negotiated profile is not applied

- default_profiles (:43-46) advertises 0x0001 AES128-CM/SHA1-80, 0x0002
  AES128-CM/SHA1-32, 0x0005 NULL/SHA1-80 and0x0006 NULL/SHA1-32.
- :176-177 and194-195 always call libsrtp RTP/RTCP default policy setters.
  Actual libsrtp `srtp.c:3363-3382` setsAES-ICM128, HMAC-SHA1,10-byte auth tags
  and confidentiality+authentication for both. It never varies with the chosen
  profile in this esp_peer source.
- :654-655 queries mbedtls_ssl_get_dtls_srtp_negotiation_result, but the result is
  discarded; no policy update/validation/log depends on it.
- If the peer selected0x0002, outgoing RTP would carry10-byte tags while negotiated
  RTP expects4-byte tags (RTCP still10). IfNULL wasselected, cipher policy is also
  wrong. If0x0001 wasselected, this specific mismatch does not apply. If no use_srtp
  wasnegotiated, exporter callback can still create defaults; this source does
  not gate those contexts on an actual non-UNSET selected profile.
- Successful local srtp_protect and UDP writes cannot rule out a profile mismatch:
  they use the local policy, not the remote negotiated expectations.
- Minimum next evidence is **selected profile enum** plus known role and cipher,
  no keys/randoms/MKI bytes. Installed public getter declaration is ssl.h:4383-4384;
  its doc :4374-4380 says profile is IANA ID and UNSET means not negotiated.
  Current dtls.c.obj has U getter symbol, so a linker observer or tracked source
  diagnostic can obtain its existing result without peer-layout hacks. Choosing
  a source patch versus wrapper remains the implementer's decision.
- Do not treat this as the cause of run7/run9 VAD0 until the actual selected ID
  is observed. No config/cipher downgrade/A-B was requested or performed here.

## Separate role-policy semantic inconsistency, bounded impact

- :183 hardcodes first/client-key policy to ssrc_any_inbound even whenclient role
  stores it in srtp_out; :202 hardcodes second/server-key policy to
  ssrc_any_outbound even whenclient stores it in srtp_in. Server role isconsistent.
- libsrtp public srtp.h:267-275 says these wildcard types represent unprotect and
  protect directions. This is not correct policy direction in client role,
  despite correct key bytes and context mapping.
- However actual local libsrtp srtp.c:2271-2289 clones a newSSRC forprotect and
  explicitly changes direction to sender. Thus wrong template direction does
  **not by itself block this client's first outgoing RTP packet**.
- Onunprotect :2580-2584 first uses template; :2815-2819 triggers SSRC-collision
  event when direction isoutbound, but does not return a failure justfor that
  event; srtp_priv.h:240-246 calls a void event callback. Template cloning at
  :2838 inherits direction at :668-670. Default callback srtp.c:1510-1518 merely
  reports warnings. This can produce spurious direction/collision events; no
  concrete audio-drop consequence is proven from it here.
- It is a separate source-level correctness issue, not evidence of reversed
  exporter keys or proof of currentzeroVAD. Do not use it to skip actual packet,
  protection/write, profile, and remote-receive evidence.

## Primary references

- RFC5705 section4 (no-context client_random||server_random and negotiated PRF):
  https://www.rfc-editor.org/rfc/rfc5705#section-4
- RFC5764 sections4.1.2/4.2 (profile transforms, exporter label/slices/directions):
  https://www.rfc-editor.org/rfc/rfc5764#section-4.2
- Pinned Espressif public source:
  https://github.com/espressif/esp-webrtc-solution/blob/c8650846b512e6e1375e5f78c1c41619b8d645eb/components/esp_peer/src/dtls_common.h
- Pinned official libsrtp source:
  https://github.com/espressif/esp-adf-libs/blob/f4bba1409f29901a18a3e3912f614fbd8cf0f937/esp_libsrtp/libsrtp/srtp/srtp.c
- Upstream MbedTLS3.6.5 exporter callback / PRF implementation:
  https://github.com/Mbed-TLS/mbedtls/blob/v3.6.5/library/ssl_tls.c

Only standard/code-level exonerations and conditional implementation risks above
are established. No end-to-end crypto interoperability or VAD/ASR cause is proven.
