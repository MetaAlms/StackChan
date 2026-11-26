# Public API and linker feasibility: minimal DTLS observation

Read-only preparation at application HEAD050a2f43410d5246239b1a1357078e3b52c33412. No repository implementation, build, device/cloud session, credential reading or packet/key computation. Existing objects and installed public headers only; provenance.json freezes their hashes.

## Conclusion

The existing S3 objects provide enough external call boundaries to observe configured DTLS role, actual Mbed TLS handshake entry/return/elapsed/completion, negotiated cipher/SRTP profile after completion, BIO delivery and raw UDP TX/RX metadata. No peer or SSL context layout/offset access is required.

Use the public config/setup map for transport scoping. The shortest supported core is:

`config_defaults / conf_transport -> successful setup -> set_bio -> handshake -> free`

and the existing `lwip_sendto` wrapper plus a `lwip_recvfrom` wrapper. Public getters run inside the tracked handshake wrapper; do not wrap getters that are inline.

## Evidence and exact API contracts

Pinned peer source SHA c8650846b512e6e1375e5f78c1c41619b8d645eb. Current S3 precompiled libpeer_default.a SHA25625a338fc6b05f702947c2af0e924f2dd3006503908e889dcdc512782d0ea3a58. Existing compiled dtls_srtp.c.obj has undefined references to config_defaults,conf_transport,setup,set_bio,handshake,get_ciphersuite,get_dtls_srtp_negotiation_result,session_reset,free,config_free,set_timer_cb. Compiled udp.c.obj references lwip_sendto/lwip_recvfrom. The current Mbed TLS archive defines corresponding external strong T symbols. Evidence files: dtls-object-undefined.txt,udp-object-undefined.txt,mbedtls-external-symbols.txt.

Installed Mbed TLS is3.6.5. Public header `/Users/amtf/esp/esp-idf-5.5/components/mbedtls/mbedtls/include/mbedtls/ssl.h`:

| Boundary/API | Public signature or behavior | Header line |
|---|---|---:|
| config_defaults | int(config*,int endpoint,int transport,int preset) |5733|
| conf_transport | void(config*,int transport), observe later updates |2143|
| setup | int(ssl*,const config*); associate only rc0 |2094|
| context_get_config | const config*(const ssl*), public static inline |2298|
| conf_get_endpoint | int(const config*), public static inline |2124|
| set_bio | void(ssl*,void*,send_t*,recv_t*,recv_timeout_t*) |2334|
| handshake | int(ssl*) |5101|
| is_handshake_over | int(ssl*), public static inline,1/0 |5114|
| get_ciphersuite | const char*(const ssl*) |4860|
| get_dtls_srtp_negotiation_result | void(const ssl*,mbedtls_dtls_srtp_info*) |4383|
| free | void(ssl*) |5569|
| session_reset | int(ssl*) |2106|

BIO typedefs:

- send_t is `int(void *ctx,const unsigned char *buf,size_t len)` (813).
- recv_t is `int(void *ctx,unsigned char *buf,size_t len)` (837).
- recv_timeout_t additionally takes `uint32_t timeout` (863).

Use the installed public typedefs in extern-C declarations. SRTP getter returns void, not an integer. Its declaration/output type is guarded by MBEDTLS_SSL_DTLS_SRTP. Access only `info.MBEDTLS_PRIVATE(chosen_dtls_srtp_profile)` in the officially declared output structure; do not access MKI bytes/values. Direct `.chosen_dtls_srtp_profile` does not compile under the current private-member macro.

There is no public numeric `mbedtls_ssl_get_state`, endpoint getter directly on ssl, or `conf_get_transport`/`ssl_get_transport` in this version. Do not invent these functions. Use the public inline config and endpoint getters for role, and is_handshake_over for completion. None of those inline getters has an external symbol to wrap.

## Minimal observation steps

1. Arm a fixed probe generation before Transport.Start/peer open. On successful config_defaults, retain conf pointer identity and endpoint/transport from public arguments. conf_transport updates the stored transport (the pinned peer re-pins DATAGRAM before handshake). Successful setup associates an ssl pointer with its conf. Map only DATAGRAM contexts into the DTLS probe; STREAM contexts from HTTPS remain outside. Unknown mappings/table overflow must be reported as coverage failure, with the original call still forwarded unchanged.
2. In `__wrap_mbedtls_ssl_handshake`, identify the mapped DTLS ssl; obtain actual role using context_get_config then conf_get_endpoint on that same context. Record an attempt ID, entry/exit esp_timer_get_time monotonic timestamps or bounded uint32 elapsed milliseconds, original signed return code and is_handshake_over before/after. Call only `__real_mbedtls_ssl_handshake` for the real operation. Do not alter or translate WANT_READ/WANT_WRITE,HELLO_VERIFY_REQUIRED,TIMEOUT or terminal returns. The pinned peer client helper translates all nonzero returns into -1 at dtls_common.h615-617, so wrapping below that boundary retains the diagnostic value.
3. Only after is_handshake_over reports true query public cipher getter and SRTP getter. Header4369 explicitly warns negotiated SRTP output is not trustworthy before handshake completes. Before completion report cipher/profile as unavailable/unconfirmed; do not treat an earlier chosen field as a selected profile. rc0/completion still differs from peer CONNECTED: SRTP derivation or later peer steps can fail.
4. `__wrap_mbedtls_ssl_set_bio` sees the actual original callback functions and p_bio. For mapped DTLS, save them in a fixed stable slot and pass that slot plus typed forwarding trampolines to `__real_mbedtls_ssl_set_bio`. A trampoline calls the saved original callback with the saved original p_bio, not with the slot. Keep each original NULL callback NULL: if both recv callbacks are non-NULL, timeout callback wins (ssl.h2314-2316). The pinned3.6.5 peer passes recv_timeout=NULL and its own CH-reassembly recv callback at dtls_common.h547. Record original requested length, raw result, elapsed and bounded metadata after a positive recv; no ret translation, sleeps or new retry behavior. Register updates happen repeatedly (before each attempt and after successful handshake at common650), so refresh the original callback pair and do not accidentally store the probe's own trampoline as its original.
5. Observe raw UDP separately through existing lwip_sendto and new lwip_recvfrom wrappers. This records network datagrams, while BIO records bytes actually handed to Mbed TLS; they are different stages. For successful RX parse at most min(rc,requested_capacity). For TX distinguish attempt/negative/short/full result; only rc==requested_len proves a complete local write. Parse only bounded13B DTLS1.0/1.2 record headers and their declared lengths, including multiple records; type,version,epoch,sequence48 split16+32,length are enough. No payload/certificate/random/key/MKI/address output.
6. For per-context raw UDP association, mark a per-task probe context during the saved BIO send/recv callback. A nested lwip call then supplies an actual FD via public arguments; retain fd+generation+ssl-slot association. If no nested call occurs, label association unknown. Header matching alone does not identify an SSL session; with multiple DTLS users, do not merge them. This setup mapping keeps TLS/HTTPS handshake reports out, but raw UDP counts still need the FD/context association or an explicit single-DTLS-user coverage claim.
7. Retire ssl/config identities via public free/config_free observations. Successful session_reset starts a new attempt epoch; it is not handshake success. Do not recycle a live BIO userdata slot while any callback or SSL context still holds it. Recording disarm must continue forwarding original callbacks; clear/reuse the table only after the owner has stopped/joined/free completed. Preserve fixed generation,unknown-context,table/ring overflow and dropped-sample counters. Locks cover only metadata snapshots/updates, never real handshake/socket/BIO calls.
8. Save/restore errno before observation around a real call, capture it immediately after the real call, and restore it after all observation/getter work. Keep original return values/buffers/flags/sockaddr handling unchanged. No logging/allocations/IO in wrappers. Emit reports on every connection failure and on success before the media gate; M1's existing media-only Arm is too late for handshake failures.

The pump prestudy's internal BIO skipped/pending/CH-assembly counters cannot all be recovered from this public setter wrapper. It records the original recv callback's final entry/return/delivery only. Raw UDP was read but BIO returned no DTLS supports a dispatch/assembly hypothesis, not an established drop cause. Numeric Mbed TLS internal state similarly has no public getter; use completion+return+BIO/timing chronology.

## Concrete compile/link risks

- All wrapper definitions and __real declarations must be extern-C in C++ and exactly match the installed public signatures, including const config*,size_t,uint32 timeout and function typedef pointer syntax.
- GNU ld wrapping covers unresolved external references, not inlined/static helpers or same-translation-unit calls. The peer object's U handshake/set_bio/config/setup references are supported; do not promise to intercept every library-internal handshake_step call. Do not add `--wrap=is_handshake_over` expecting events.
- Call __real_* rather than the wrapped original name; do not define a local replacement __real_* body alongside the wrapper. That risks recursion or assembler resolution before wrapping.
- Main CMake311-312 already wraps srtp_protect and lwip_sendto. A second __wrap_lwip_sendto implementation causes a duplicate definition; add DTLS observation to the existing wrapper/fanout, not a competing symbol. New wrappers require final-link flags and reconfigure/source discovery as normal. Confirm final map/disassembly contains the redirected callsites after implementation; no new build/link was attempted here.
- Transport has no public getter: keep the config_defaults/conf_transport mapping current. HTTPS/STREAM successful handshakes must never satisfy the DTLS report.
- Public SRTP output must be guarded and read after completion only. Cipher getter may returnNULL on failure. None of this accesses opaque SSL or peer fields.
- Preserve real recv_timeout NULL semantics and existing blocking behavior. The probe must not silently become a new packet pump or timeout policy.
- Existing newlib-nano configuration has a known64-bit format limitation: report sequence48 as uint16 high+uint32 low and time as bounded uint32ms, not %llu/%llx. Use monotonic int64 internally for esp_timer deltas.
- DTLS record parser scope is current direct UDP/classic DTLS. TURN envelopes/TCP or alternative framing require explicit unsupported-coverage reporting; absence of parsed records there is not proof of no traffic.

## Optional next layer, not required for the first chain

If raw UDP/BIO/handshake chronology leaves a timer question, public set_timer_cb is also an external U/T boundary. A typed transparent timer trampoline can preserve original p_timer/set/get functions and record int/final delays plus original get return0/1/2. The source's direct timing_set_delay reset before setter registration is a separate call; do not pretend the setter trampoline captured it. No timer policy change is proposed.

## Limits

This is static public-API/link feasibility, not a compiled probe or runtime result. It cannot identify run12's cause, prove early HELLO loss, show encrypted handshake contents, prove remote SRTP authentication/Opus/VAD/ASR, or guarantee the legacy source's synchronous handshake returns promptly. Raw socket receipt does not prove BIO delivery; BIO delivery does not prove Mbed TLS acceptance; local full UDP write does not prove server receipt. Actual selected profile and completion must be measured before applying the conditional SRTP-policy concern.

Primary API reference: https://github.com/Mbed-TLS/mbedtls/blob/mbedtls-3.6.5/include/mbedtls/ssl.h.
GNU wrap semantics: https://sourceware.org/binutils/docs/ld/Options.html#Options.
Pinned peer implementation: https://github.com/espressif/esp-webrtc-solution/blob/c8650846b512e6e1375e5f78c1c41619b8d645eb/components/esp_peer/src/dtls_common.h.
Independent public API inventory: /tmp/stackchan-media-review-v155/mbedtls-365-public-api.md.
