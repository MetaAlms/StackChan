/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

/**
 * @brief Minimal Ogg muxer for an Opus stream.
 *
 * The Aliyun Realtime API does not accept bare Opus on the uplink: sending
 * raw-opus frames closes the connection, and only "opus" (Ogg-framed) and
 * "pcm" are accepted. Ogg framing keeps the on-device Opus encoding, so the
 * uplink stays around 4 kB/s instead of the ~32 kB/s that 16 kHz PCM would
 * cost, and no Opus decoding is needed on the device.
 *
 * This emits the two mandatory header pages (OpusHead, OpusTags) followed by
 * one page per audio packet. That is not the most compact framing possible -
 * real encoders pack many packets per page - but it is valid Ogg, and the
 * server accepts it (verified end to end against the live endpoint).
 *
 * Not a general-purpose muxer: single logical stream, one packet per page,
 * no multiplexing, no seeking.
 */
class OggOpusMuxer {
public:
    /**
     * @brief Emit the stream headers. Must be called once before write_packet.
     * @param sample_rate Input sample rate declared in OpusHead (16000 here).
     * @param channels    Channel count (1 here).
     * @return The OpusHead and OpusTags pages, ready to send.
     */
    std::vector<uint8_t> begin(int sample_rate, int channels);

    /**
     * @brief Wrap one Opus packet into a page.
     * @return A complete Ogg page carrying the packet.
     */
    std::vector<uint8_t> write_packet(const uint8_t* data, size_t len);

    /** @brief Forget all state so the next begin() starts a fresh stream. */
    void reset();

private:
    uint32_t serial_ = 0;
    uint32_t sequence_ = 0;
    uint64_t granule_ = 0;
    int pre_skip_ = 312;  // 6.5 ms at 48 kHz, the customary Opus encoder delay

    std::vector<uint8_t> make_page(const uint8_t* segments_data, size_t data_len,
                                   const uint8_t* segment_table, size_t segment_count,
                                   uint8_t header_type, uint64_t granule);
};
