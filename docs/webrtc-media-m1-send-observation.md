# esp_peer 1.5.5 actual RTP -> SRTP -> UDP observation design

Scope: read-only follow-up to Review 5447065335 at repository commit
`9e45e63d9ce9083959a50e4420220b26a31da05e`.
The read-only audit performed no source edits, build, flash, hardware/cloud session, credential access, PR mutation, or DSH messaging. Root copied this design into the repository as a technical reference for the existing Review 5447065335 repair task.
This is a concrete implementation proposal; not a compiled or device-validated patch.

## Conclusion

Use tracked `firmware/main/hal/webrtc/rtp_send_probe.cc` + tracked main CMake
`--wrap=srtp_protect` and `--wrap=lwip_sendto`, gated on the existing M1 probe
configuration. Both calls are unresolved external references in the matching S3
objects, which is precisely what GNU ld wrap can redirect. No `peer_t`, ICE agent,
SRTP context, or UDP socket field access is needed. No managed source or binary
patch is required for the minimum direct-UDP diagnostic.

## Pinned implementation evidence

- esp_peer 1.5.5 official commit:
  `c8650846b512e6e1375e5f78c1c41619b8d645eb`, from managed idf_component.yml.
- S3 `libs/esp32s3/libpeer_default.a` SHA256:
  `25a338fc6b05f702947c2af0e924f2dd3006503908e889dcdc512782d0ea3a58`.
- Downloaded upstream source snapshots in this directory were byte-equal to local
  `src/dtls_common.h`, `src/transport/udp.{c,h}`, `CMakeLists.txt`,
  `include/esp_peer{,_types}.h`.
- `src/dtls_common.h:745-749`: `void dtls_srtp_encrypt_rtp_packet(...)` initializes
  output capacity to buf_size, calls six-argument `srtp_protect`, ignores its
  returned status, and always assigns output size to `*bytes`.
- Built `dtls_srtp.c.obj` has U srtp_protect. Its encrypt function +0x16/+0x19
  relocates/calls srtp_protect; +0x1c/+0x1e copies size without testing status.
  See `dtls-encrypt-disassembly.txt` and `built-source-symbols.txt`. Existing build
  objects establish the boundary, not a new exact-SHA firmware validation.
- Prebuilt `peer_default.c.obj` has U dtls_srtp_encrypt_rtp_packet and local/static
  `write_rtp_packet`. The latter cannot be replaced by a normal weak definition.
  Saved disassembly `/tmp/stackchan-esp-peer-review-v155/peer_default-disassembly.txt`:
  write_rtp_packet +0x10f calls void encrypt, then +0x14b/+0x176/+0x18a call agent
  send variants; it returns no write status to the RTP encoder callback.
- `src/transport/udp.c:186`: udp_socket_sendto is explicitly WEAK. It selects
  IPv4/IPv6 socket, calls sendto at :219, and returns final rc at :245. It retries
  ENOBUFS/ENOMEM twice (:222-234), can return -200 after attempted retries (:239),
  and can return -1 before any socket write if fd is invalid (:211-212).
- Built udp.c.obj has W udp_socket_sendto and U lwip_sendto. Actual local IDF
  `components/lwip/lwip/src/include/lwip/sockets.h:613-614` declares lwip_sendto;
  :663 maps sendto to lwip_sendto. See child `/tmp/stackchan-media-review-v155/udp-write-observation-9e45e63.md`.
- Public optional RTP transform API: esp_peer.h:565 and esp_peer_types.h:65-108.
  Sender callback runs before TWCC extension work and SRTP in this binary. A
  get_encoded_size observer returning ESP_PEER_ERR_NOT_SUPPORT (-4) skips
  transformation without dropping the packet. Register both callbacks before
  CONNECTED; do not attempt runtime registration after connection. This is useful
  to relate input Opus length/pts, but the srtp wrapper is later and sees the
  actual serialized RTP header including extensions, so it is sufficient for
  the minimum request.

## Exact public signatures and non-recursive forwarding

From `esp_libsrtp/libsrtp/include/srtp.h:430-435` (component 1.0.0, official
commit `f4bba1409f29901a18a3e3912f614fbd8cf0f937`):

```c
srtp_err_status_t srtp_protect(srtp_t ctx,
                             const uint8_t *rtp, size_t rtp_len,
                             uint8_t *srtp, size_t *srtp_len,
                             size_t mki_index);
```

Public IDF lwIP signature:

```c
ssize_t lwip_sendto(int s, const void *dataptr, size_t size, int flags,
                   const struct sockaddr *to, socklen_t tolen);
```

Include actual installed `srtp.h` and `lwip/sockets.h`, not copied opaque structs.
C++ definitions and __real declarations need extern "C". Core forwarding shape:

```c
extern srtp_err_status_t __real_srtp_protect(srtp_t, const uint8_t *, size_t,
                                           uint8_t *, size_t *, size_t);
extern ssize_t __real_lwip_sendto(int, const void *, size_t, int,
                                 const struct sockaddr *, socklen_t);

srtp_err_status_t __wrap_srtp_protect(srtp_t ctx, const uint8_t *rtp,
    size_t n, uint8_t *out, size_t *out_n, size_t mki)
{
    /* Snapshot bounded fixed header and input capacity BEFORE encryption. */
    /* Ensure the observer leaves errno exactly as it found it before __real. */
    srtp_err_status_t rc = __real_srtp_protect(ctx, rtp, n, out, out_n, mki);
    int saved_errno = errno;
    /* Record rc; read *out_n as valid output length ONLY when rc==srtp_err_status_ok. */
    /* Fixed metadata/counter updates only, no allocation, I/O, or blocking. */
    errno = saved_errno;
    return rc;
}

ssize_t __wrap_lwip_sendto(int fd, const void *p, size_t n, int flags,
    const struct sockaddr *to, socklen_t tolen)
{
    ssize_t rc = __real_lwip_sendto(fd, p, n, flags, to, tolen);
    int saved_errno = errno;  /* Immediately after actual socket call. */
    /* Match bounded fixed header against recent protect metadata; record */
    /* attempt rc and failure errno (success errno is stale, not a failure). */
    errno = saved_errno;
    return rc;
}
```

Call __real_*, never the ordinary wrapped function from inside its wrapper (that
would recurse). Do not provide your own __real_* implementation in the wrapper
translation unit. Preserve errno around pre-call metadata snapshots as well as
post-call updates. Return original rc and leave buffers, lengths, keys, destination,
flags, and retries untouched. This is observation, not error propagation repair.

## Minimal tracked integration and bounded correlator

1. Track diagnostic .cc/.h plus main CMake edits. In M1 configuration add
   `target_link_options(${COMPONENT_LIB} INTERFACE
   "-Wl,--wrap=srtp_protect" "-Wl,--wrap=lwip_sendto")` and explicit component
   include dependency for installed esp_libsrtp/lwip. New file is already in HAL
   glob; implementer must reconfigure to pick it up. Do not hand-edit ignored
   managed sources. This route rebuilds from the ordinary dependency fetch.
2. Arm an explicit local probe/session generation at the transport's lifecycle
   boundary. Clear pending records only when the old sender/peer is joined; do
   not have wrappers infer a session from an opaque object layout. With M1's one
   active transport, comparison of opaque srtp_t handles is optional only; never
   dereference or log them. Keep the current run/probe scope explicit.
3. Before real srtp_protect, parse byte offsets only after pointer/nonnegative
   length validation: RTP V=2, at least12 bytes, PT=data[1]&127, seq=BE16(2),
   ts=BE32(4), SSRC=BE32(8). Validate 12+4*CC and optional X extension's
   4+4*word_count fit in input length, using bounded arithmetic. Copy only these
   integers, RTP length, header length, advertised output capacity and a local
   monotonically increasing record id. Do not retain a borrowed packet pointer.
4. Keep e.g.64 pending records, with short monotonic expiry and overflow/unmatched
   counters. Key by (local generation, SSRC, seq, ts); preserve first-match age/id
   for retries. The tuple naturally handles 16-bit seq wrap within the short
   observation window. Record srtp status; on failure output bytes/size are
   undefined per srtp.h:393-395,415-418, so do not interpret them as valid SRTP.
5. Direct SRTP RTP keeps the fixed header visible; lwip_sendto can parse the same
   tuple without reading payload or keys. Match only records from the active
   probe, and require the negotiated audio PT/observed SSRC and generation. PT
   and RTP V alone are insufficient demux: RTCP also begins with V=2, and global
   lwIP writes include STUN, DTLS, other app sockets. Unmatched traffic is counted
   separately, never silently attributed to this audio stream.
6. For each matching socket attempt record requested UDP bytes, signed rc and
   errno only if rc<0. rc==n is local complete write; rc<n is short write; rc<0
   is failed attempt. Count packet-with-any-success separately from attempts,
   since the unchanged UDP implementation can retry twice. Aggregate all run
   frames and bytes, sample first few records and bounded anomalies; count ring
   overflow/expiry/unmatched and do not claim complete coverage if nonzero.
7. Use fixed storage and short synchronized metadata updates (wrappers can run
   on sender/poll threads); release locks before real SRTP/socket calls. Do not
   log while holding a lock or per packet in the real-time path. Export totals
   from a safe task, and avoid %lld with enabled nano printf. White-list fields
   above only: no IPs, SDP secrets, Authorization, ICE ufrag/pwd, keys, DTLS data,
   Opus payload, or raw packet dump.
8. Verify future implementation by link command + map/disassembly: __wrap_* are
   retained and the intended dtls/udp callsites target them, __real_* resolve to
   original functions, no duplicate implementation. Do not treat symbol presence
   alone as hook coverage. Then require first few actual RTP header samples and
   full-run SRTP/socket counters for the active generation. No build or runtime
   verification has been performed in this read-only investigation.

## Optional final UDP result / weak source-hook alternative

For pre-write failures and a true final udp_socket_sendto result, an optional
third `--wrap=udp_socket_sendto` is possible because agent.c.obj has U that symbol
and the source defines W. Use the pinned source `src/transport/udp.h:34` prototype,
pass its udp_socket_t* through unchanged, call __real_udp_socket_sendto, record
final rc, and do not inspect fields. Private include path makes this an internal
version-pinned boundary, unlike the first two public boundaries. Final rc may be
-200; do not invent errno when original returns -1 before a syscall. Because agent
can retry higher up, distinguish final-per-call result from final-per-RTP result.

If linker wrapping is unsuitable, a tracked patch to the open-source
`dtls_common.h:748` can store actual srtp status and call a new no-op weak hook,
and `transport/udp.c:219` can call a similar hook around each write. Preserve the
original void ABI and errno behavior. Apply/reapply/check that tracked patch in
an explicit build/dependency preparation step with source hashes/idempotency
checks; the existing fetch_repos.py presently patches git-fetched dependencies,
not these managed components. Changing ignored managed sources manually is not
a reproducible deliverable. Do not override the weak UDP routine by copying its
IPv4/IPv6/socket/retry implementation just to add counters.

## Limits that must remain explicit

- Wrap=srtp_protect captures the real protection status; wrapping only the void
  dtls encrypt function cannot recover that status.
- Two public wraps observe actual local writes and successful SRTP but do not
  guarantee a write occurs: ICE/agent can reject or enqueue without a syscall.
  Missing matching socket events must be reported, not labeled success.
- Tuple correlation above is deliberately direct UDP for this M1 path. TURN
  ChannelData/Send-Indication wraps the RTP packet; TCP/TLS uses other write
  paths/queues, so without explicit envelope/framing instrumentation these are
  unsupported/unmatched, not false successes. Do not parse agent memory.
- Successful UDP write only proves local socket accepted a datagram. It does not
  prove NIC transmission, NAT traversal, remote receipt, correct SDP PT/direction,
  SRTP authentication at Aliyun, Opus decode, VAD, ASR, or application response.
- If SRTP returns error, current upstream void ABI still lets peer try to send;
  diagnostic counters expose this but do not themselves repair propagation.
- Observation adds timing cost and must stay bounded; no fabricated end-to-end
  success or precise wire-delivery time from local timestamps.

References: GNU ld --wrap official documentation
https://sourceware.org/binutils/docs/ld/Options.html#index-_002d_002dwrap
and pinned Espressif source snapshots beside this file. See audit-scope.txt,
peer-symbols.txt, built-source-symbols.txt, dtls-encrypt-disassembly.txt.
