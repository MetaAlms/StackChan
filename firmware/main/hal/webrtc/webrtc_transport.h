#pragma once

#include <sdkconfig.h>


// PROBE_GUARD
#if CONFIG_STACKCHAN_WEBRTC_M0 || CONFIG_STACKCHAN_WEBRTC_M1

/**
 * @brief Shared WebRTC transport for the M0/M1 probes.
 *
 * Owns the esp_peer instance, the SDP exchange, the peer main loop and the
 * DataChannel bookkeeping that both probes need. Extracted from the verified M0
 * implementation so that M1 does not carry a second copy of the transport
 * stack; the transport logic itself is unchanged from M0.
 *
 * Threading contract:
 *  - callbacks are invoked from the peer main loop task or the signaling task
 *  - callbacks must not block and must not call Stop()
 *  - on_audio receives a *borrowed* pointer that is invalid after the callback
 *    returns; consumers must copy before returning
 */

#include <cstdint>
#include <functional>
#include <string>

#include "esp_peer.h"

namespace webrtc_transport {

/** Peer configuration shared by the probes. */
struct Config {
    /** PCM rate handed to the Opus codec. Independent of the RTP clock. */
    int pcm_sample_rate = 48000;
    int pcm_channels = 1;
    /** RTP/SDP clock; fixed at 48000 by RFC 7587 for Opus. */
    int rtp_clock_rate = 48000;
    bool enable_data_channel = true;
    const char* data_channel_label = "oai-events";
    /** STUN server used to gather a server-reflexive candidate. */
    const char* stun_url = "stun:stun.miwifi.com:3478";
};

/** Observation hooks. See the threading contract above. */
struct Callbacks {
    std::function<void(esp_peer_state_t state)> on_state;
    std::function<void(esp_peer_data_channel_info_t* ch)> on_channel_open;
    /** Complete JSON text received on a DataChannel, with its stream id. */
    std::function<void(const std::string& json, uint16_t stream_id)> on_data;
    /** Borrowed encoded audio frame received over RTP/SRTP. */
    std::function<void(const uint8_t* data, int size, uint32_t pts_ms)> on_audio;
    /**
     * Invoked by Stop() *before* the peer is closed, so an owner can stop and
     * join its media sender first. Sending concurrently with close is not
     * allowed by esp_peer.
     */
    std::function<void()> on_before_close;
};

class Transport {
public:
    Transport() = default;
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;

    /**
     * @brief Open the peer, exchange SDP and start the main loop.
     * @param err  receives a short reason when the call fails
     * @return true when the peer is running and the offer was accepted
     */
    bool Start(const Config& cfg, const Callbacks& cb, std::string* err);

    /** Send a JSON string on an already discovered DataChannel stream. */
    bool SendJson(const std::string& json, uint16_t stream_id);

    /**
     * @brief Send one encoded audio packet.
     *
     * @param data    encoded payload, one packet, no container framing
     * @param size    payload length in bytes
     * @param pts_ms  media presentation time in milliseconds
     *
     * Returns false when the audio channel is not open yet or the peer rejects
     * the frame; callers must keep advancing their media timeline regardless.
     */
    bool SendAudio(const uint8_t* data, int size, uint32_t pts_ms);

    /** True once the audio channel is usable for sending. */
    bool CanSendAudio() const;

    /** Last observed peer state. */
    esp_peer_state_t state() const;

    /** Stop the sender via on_before_close, then tear everything down. */
    void Stop();

    /** Maximum single-packet payload the transport will accept, in bytes. */
    static int MaxPayloadBytes();

    /**
     * @brief Opaque state, defined in the .cc.
     *
     * Only forward-declared here; it is public so the file-local callback
     * trampolines can name it. No implementation detail is exposed.
     */
    struct Impl;

    /** Stop the tasks, close the peer and release every resource. */
    static void cleanup(Impl* p);

private:
    Impl* impl_ = nullptr;
};

}  // namespace webrtc_transport

#endif  // CONFIG_STACKCHAN_WEBRTC_M0 || CONFIG_STACKCHAN_WEBRTC_M1
