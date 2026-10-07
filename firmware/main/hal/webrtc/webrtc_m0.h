#pragma once

/**
 * @brief M0: verify that esp_peer interoperates with Aliyun's WebRTC endpoint.
 *
 * Answers one question and nothing else: can this device complete SDP exchange,
 * ICE, DTLS and SCTP against Aliyun, and receive session.created /
 * session.updated over the DataChannel? No media is sent.
 *
 * The point of stopping there is to fail cheaply. A full WebRTC port means
 * resampling, Opus at 48 kHz, RTP plumbing and rewriting the protocol layer. If
 * the handshake cannot be made to work at all, none of that is worth starting
 * and the existing WebSocket implementation stays.
 *
 * Never returns: on success and on failure alike it logs the verdict and idles,
 * so the serial console keeps the evidence.
 */
void WebRtcM0Run();
