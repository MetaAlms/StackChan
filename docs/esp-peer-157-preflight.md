# esp_peer 1.5.7 U1 compatibility audit

Read-only audit anchored to StackChan HEAD `b45aaa70ee324e2f9997412c1d573dd31b1269be`, compared with its installed/pinned esp_peer1.5.5/S3 package. Only official public release material and public API/platform sources were read. No workspace changes, dependency installation, firmware build/link, device/cloud session, private runtime, credentials or keys.

## Conclusion

No new source/API incompatibility was found in the audited wrapper, media and dependency surfaces. U1 can retain the existing IDF5.5.4, esp_libsrtp1.0.0 and media configuration for its single-variable comparison. This is a static compatibility conclusion, not adoption approval or proof that1.5.7 fixes this device topology. The existing U1-0 diagnostic repair is still required before either version's baseline.

## Authenticity and dependencies

- Official registry version1.5.7 object ID `f875c146-12ae-411a-bdd4-1969ea2c0ee1`, published `2026-10-08T11:44:47.072518Z`.
- Registry **component_hash**: `92bc1b80772acf8a0e2dc36193ec24278e0793140eca2a4060d8ce7b45c47845`.
- Release manifest `idf_component.yml:8`: repository source commit `33a863f1a6c57bdcb7821f6f7a8e159035a0acf6`.
- Download archive SHA256: `86c0c28a320f79c05fbe21477b519e4e7cf666264acb4147339be5908c678b9f`.
- New **S3 library SHA256**: `e6fa4ccb3256007105bf7eff1e313dc5c06669f7d3b6df192571ebdba4496285`.
- Old S3 library SHA256: `25a338fc6b05f702947c2af0e924f2dd3006503908e889dcdc512782d0ea3a58`.
- Component hash, archive hash and library hash are separate identifiers; do not substitute one for another.
- Release `idf_component.yml:1-2` still declares only private `espressif/esp_libsrtp:^1.0.0`, identical to the old manifest. Current lock entry is esp_libsrtp1.0.0/component_hash `131560184c73ec09f77d8904a2db709307803f0f9f111898921b6966776eac15`; no dependency change is required by1.5.7 metadata.
- CMakeLists.txt is byte-identical. IDF5.5.4 continues to choose `src/dtls_srtp.c` rather than the v6 variant and target `libs/esp32s3/libpeer_default.a`.

## Public declarations and wrapper coverage

All released `src/` files, `include/esp_peer_types.h`, `include/esp_peer_default.h` and CMakeLists.txt are byte-identical to the currently installed1.5.5 copies. The only public-header difference is documentation in `esp_peer.h`; enum values, function declarations, public structures and callback types are unchanged. Candidate comments clarify bare `candidate:` input for trickle; no new media/config field is required.

The S3 archive preserves unresolved `peer_default.c.obj -> dtls_srtp_handshake/encrypt_rtp_packet` and `agent.c.obj -> udp_socket_sendto/recvfrom_nowait` references. Matching public DTLS/UDP sources remain compiled companion objects, so the documented GNU ld wrap boundaries remain viable:

- `dtls_common.h:745-749`: `srtp_protect(ctx, packet, rtp_bytes, packet, &out_capacity_or_length, 0)`; public libsrtp `srtp.h:430-435` declares the same six arguments. Output length remains meaningful only after success. The caller still ignores protection return status, so the real protect observer is necessary.
- `dtls_common.h:59,553`: public transport setter and handshake call.
- `dtls_srtp.c:169/176/199`: public config_defaults/setup; `:213-214` free/config_free; `:250` session_reset; `:284-286` role-change free/setup.
- `transport/udp.c:219/286`: public sendto/recvfrom; IDF/lwIP mapping and final compiled callsites must be verified in the implementation's actual ELF/map, not assumed from source alone.

Pre-existing b45 U1-0 prerequisites persist: MbedTLS `mbedtls_ssl_conf_transport` returns **void** (`ssl.h:2143`), DATAGRAM=1/STREAM=0 (`:301-302`); free API is `mbedtls_ssl_config_free` (`:5741`), not `mbedtls_ssl_conf_free`. These signatures/names were not changed by the peer upgrade. Successful public setup/context association, free/reset lifecycle and immutable generation binding remain mandatory; the new version cannot repair a flawed observer.

Normalized archive undefined-symbol comparison adds only `pthread_mutexattr_init/destroy/settype` in agent.c.obj and removes `ice_candidate_create` there. Installed IDF implements the three new functions (`components/pthread/pthread.c:813,824,842`) and recursive mutexes. Final-link resolution should still be checked. There are no added exported T/W/D symbols in the normalized defined-symbol comparison.

## Media compatibility and unchanged limitations

Whole S3 objects `rtp.c.obj`, `rtp_jitter.c.obj`, `rtp_rolling_buffer.c.obj`, `rtp_jpeg.c.obj`, `rtcp.c.obj`, `buf_mngr.c.obj` and `utils.c.obj` are byte-identical between1.5.5 and1.5.7. Independent media audit also compares selected changed-container peer_default functions' actual section bytes and relocations; see `audio-contracts/README.md` and `audio-contracts/function-section-evidence.txt`.

Thus retain the verified media contracts: `RTP_TS=(48*pts_ms) mod 2^32`, so20ms advances960 ticks; raw single Opus packet; 1428-byte whole RTP buffer with the existing conservative application payload guard1352B (12B header+64B application extension reserve deducted); borrowed receive callback and application pacing/clock handling. The reserve is application policy, not a new public peer payload ceiling or negotiated PMTU guarantee. Public send_audio return0 still does not certify real protection/socket success. The unchanged DTLS source also retains existing profile/policy selection behavior; no new exporter defect or media root cause is inferred here.

## Required static checks for the delivered upgrade candidate

1. Pin exact1.5.7 in the manifest and preserve the lock's unrelated entries. Verify resolved component_hash, release source SHA, actual S3 library SHA, matching official headers and source. Do not delete the lock and incidentally upgrade wildcard/caret dependencies; any necessary dependency drift goes to review.
2. Check corrected wrapper declarations against installed public headers, including void conf_transport/config_free/profile-result getter and six-argument srtp_protect. Observe free/reset identity lifecycle with unchanged args/rc/errno and no opaque layout access.
3. Verify final link map/undefined call relocations route the compiled DTLS/SRTP/UDP calls through the intended wrappers and retain all newly imported pthread functions. Inline public getters are direct queries, not wrap targets. Build success alone does not prove observation coverage.
4. Carry forward real-opus packet/pts/capacity checks and confirm the actual candidate's RTP metadata/protect result/UDP full-write accounting. No ptime/bitrate/rate/fixture/cipher/STUN change is implied by this upgrade.
5. Keep the repaired1.5.5 baseline and1.5.7 run on the same code/config/fixtures/topology. Report reached stages separately: ICE, actual completed DTLS+profile, SCTP/config, real protected full local write, VAD and ASR. Local UDP success cannot prove remote receipt/decryption, and a release-note claim is not device A/B evidence.

## Evidence and execution boundary

`provenance.json` records package hashes, every compared public file and extracted object hashes. `registry-versions-selected.json` preserves official1.5.5/1.5.7 registry metadata. `v155-/v157-undefined.txt` and `defined.txt` preserve normalized symbol inventories; extracted public objects are in `v155/` and `v157/`.

Existing controller ledger was reused/read with status: `/Users/amtf/.codex/dev-harness/tasks/stackchan-webrtc-media/ledger.sqlite`, epoch1 active, used1/24, held0. No compatible offline pass gate was launched or replanned by this audit. Public network/download/archive operations and nm/ar/objdump subprocess inspection are outside that controller's immutable-input single-process coverage; no compile/link subprocess check was run. `inspect_public_release.py` only reads public/current source, inventories hashes and invokes public archive/symbol inspection tools; it is preserved for provenance, not represented as a controller-controlled acceptance test.

Primary public sources:
- https://components-file.espressif.com/components/espressif/esp_peer.json
- https://components.espressif.com/components/espressif/esp_peer/versions/1.5.7/changelog?language=en
- https://components.espressif.com/components/espressif/esp_peer/versions/1.5.7/dependencies?language=en
- https://github.com/espressif/esp-webrtc-solution/tree/33a863f1a6c57bdcb7821f6f7a8e159035a0acf6/components/esp_peer
