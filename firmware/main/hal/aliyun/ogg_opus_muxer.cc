/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "ogg_opus_muxer.h"

#include <cstring>
#include <esp_random.h>

namespace {

/* Ogg's CRC-32: polynomial 0x04C11DB7, no reflection, init 0, no final xor.
 * This differs from the usual zlib CRC-32, so a shared implementation would
 * silently produce pages the server rejects. */
uint32_t ogg_crc32(const uint8_t* data, size_t len)
{
    static uint32_t table[256];
    static bool table_ready = false;

    if (!table_ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t r = i << 24;
            for (int j = 0; j < 8; ++j) {
                r = (r & 0x80000000u) ? ((r << 1) ^ 0x04C11DB7u) : (r << 1);
            }
            table[i] = r;
        }
        table_ready = true;
    }

    uint32_t crc = 0;
    for (size_t i = 0; i < len; ++i) {
        crc = (crc << 8) ^ table[((crc >> 24) & 0xFF) ^ data[i]];
    }
    return crc;
}

void put_u32le(std::vector<uint8_t>& out, uint32_t v)
{
    out.push_back(static_cast<uint8_t>(v & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
    out.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
}

void put_u64le(std::vector<uint8_t>& out, uint64_t v)
{
    for (int i = 0; i < 8; ++i) {
        out.push_back(static_cast<uint8_t>((v >> (8 * i)) & 0xFF));
    }
}

}  // namespace

void OggOpusMuxer::reset()
{
    serial_ = 0;
    sequence_ = 0;
    granule_ = 0;
}

std::vector<uint8_t> OggOpusMuxer::make_page(const uint8_t* segments_data, size_t data_len,
                                             const uint8_t* segment_table, size_t segment_count,
                                             uint8_t header_type, uint64_t granule)
{
    std::vector<uint8_t> page;
    page.reserve(27 + segment_count + data_len);

    page.push_back('O');
    page.push_back('g');
    page.push_back('g');
    page.push_back('S');
    page.push_back(0);  // stream structure version
    page.push_back(header_type);
    put_u64le(page, granule);
    put_u32le(page, serial_);
    put_u32le(page, sequence_++);
    put_u32le(page, 0);  // CRC placeholder
    page.push_back(static_cast<uint8_t>(segment_count));
    page.insert(page.end(), segment_table, segment_table + segment_count);
    page.insert(page.end(), segments_data, segments_data + data_len);

    const uint32_t crc = ogg_crc32(page.data(), page.size());
    page[22] = static_cast<uint8_t>(crc & 0xFF);
    page[23] = static_cast<uint8_t>((crc >> 8) & 0xFF);
    page[24] = static_cast<uint8_t>((crc >> 16) & 0xFF);
    page[25] = static_cast<uint8_t>((crc >> 24) & 0xFF);
    return page;
}

std::vector<uint8_t> OggOpusMuxer::begin(int sample_rate, int channels)
{
    reset();
    // A random serial distinguishes this logical stream from any previous one
    // on the same connection; reusing a serial across reconnects is legal but
    // confuses some demuxers.
    serial_ = esp_random();

    // ---- OpusHead (RFC 7845 section 5.1) ----
    std::vector<uint8_t> head;
    const char magic[] = "OpusHead";
    head.insert(head.end(), magic, magic + 8);
    head.push_back(1);                                   // version
    head.push_back(static_cast<uint8_t>(channels));      // channel count
    head.push_back(static_cast<uint8_t>(pre_skip_ & 0xFF));
    head.push_back(static_cast<uint8_t>((pre_skip_ >> 8) & 0xFF));
    put_u32le(head, static_cast<uint32_t>(sample_rate)); // original sample rate
    head.push_back(0);                                   // output gain LSB
    head.push_back(0);                                   // output gain MSB
    head.push_back(0);                                   // mapping family 0

    // One packet, <= 255 bytes per segment is not guaranteed, so split it.
    std::vector<uint8_t> head_segments;
    for (size_t left = head.size(); left > 0;) {
        const size_t chunk = left > 255 ? 255 : left;
        head_segments.push_back(static_cast<uint8_t>(chunk));
        left -= chunk;
    }
    // A packet whose length is a multiple of 255 needs a trailing 0 segment.
    if (head.size() % 255 == 0) {
        head_segments.push_back(0);
    }

    std::vector<uint8_t> out =
        make_page(head.data(), head.size(), head_segments.data(), head_segments.size(), 0x02, 0);

    // ---- OpusTags (RFC 7845 section 5.2) ----
    std::vector<uint8_t> tags;
    const char tags_magic[] = "OpusTags";
    tags.insert(tags.end(), tags_magic, tags_magic + 8);
    const char vendor[] = "StackChan";
    put_u32le(tags, sizeof(vendor) - 1);
    tags.insert(tags.end(), vendor, vendor + sizeof(vendor) - 1);
    put_u32le(tags, 0);  // no user comments

    std::vector<uint8_t> tag_segments;
    for (size_t left = tags.size(); left > 0;) {
        const size_t chunk = left > 255 ? 255 : left;
        tag_segments.push_back(static_cast<uint8_t>(chunk));
        left -= chunk;
    }
    if (tags.size() % 255 == 0) {
        tag_segments.push_back(0);
    }

    std::vector<uint8_t> tags_page =
        make_page(tags.data(), tags.size(), tag_segments.data(), tag_segments.size(), 0x00, 0);
    out.insert(out.end(), tags_page.begin(), tags_page.end());
    return out;
}

std::vector<uint8_t> OggOpusMuxer::write_packet(const uint8_t* data, size_t len)
{
    if (data == nullptr || len == 0) {
        return {};
    }

    // Segment table: 255 means "continues", a shorter value ends the packet.
    std::vector<uint8_t> segments;
    for (size_t left = len; left >= 255; left -= 255) {
        segments.push_back(255);
    }
    segments.push_back(static_cast<uint8_t>(len % 255));

    // 60 ms at 48 kHz is Opus's internal granule rate, which is what the
    // granule position is expressed in regardless of the original rate.
    granule_ += 2880;

    return make_page(data, len, segments.data(), segments.size(), 0x00, granule_);
}
