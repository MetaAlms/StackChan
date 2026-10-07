#pragma once

#include <sdkconfig.h>


// PROBE_GUARD
#if CONFIG_STACKCHAN_WEBRTC_M1

/**
 * @brief M1: 48 kHz PCM uplink probe.
 *
 * Streams a bundled 16 kHz mono s16 fixture through the on-device resampler
 * (16->48 kHz) and a 48 kHz mono Opus encoder, then sends one raw Opus packet
 * per RTP frame over the verified WebRTC transport.
 *
 * Uplink only. M1 verifies four separately observable layers - configuration,
 * media send, server VAD and complete ASR - and claims nothing about downlink,
 * AEC, double-talk or product integration.
 *
 * Never returns: it prints a summary and idles so the serial console keeps the
 * evidence.
 */
void WebRtcM1Run();

#endif  // CONFIG_STACKCHAN_WEBRTC_M1
