#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace webrtc_media {

// Owned by one media worker. Never call from the AFE or esp_peer callbacks.
// PCM is mono s16: uplink 16k -> 48k Opus/20ms; downlink Opus/48k -> 24k.
class Codec {
public:
    ~Codec();
    Codec() = default;
    Codec(const Codec&) = delete;
    Codec& operator=(const Codec&) = delete;
    bool Open();
    void Close();
    // Exactly 320 input samples. Empty packet means the converter is priming.
    bool Encode20ms(const int16_t* pcm16, size_t samples, std::vector<uint8_t>& packet);
    // One complete raw Opus packet; accepts up to 120ms per packet.
    bool Decode(const uint8_t* packet, size_t bytes, std::vector<int16_t>& pcm24);
    // Owning worker only, between packets. Resets downlink filter and decoder.
    bool ResetDownlink();
private:
    void* encoder_ = nullptr;
    void* decoder_ = nullptr;
    void* up_ = nullptr;
    void* down_ = nullptr;
    int encoded_capacity_ = 0;
    size_t carry_ = 0;
    std::vector<int16_t> pcm48_;
    std::vector<int16_t> decoded_;
};

}  // namespace webrtc_media
