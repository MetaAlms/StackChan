#include <sdkconfig.h>
#if CONFIG_STACKCHAN_WEBRTC_M15
#include "media_codec.h"

#include <cstring>
#include "esp_opus_enc.h"
#include "esp_opus_dec.h"
#include "esp_ae_rate_cvt.h"

namespace webrtc_media {
namespace {
constexpr size_t kFrameSamples = 960;
constexpr size_t kMaxDecodedSamples = 5760;
constexpr size_t kMaxPayload = 1352;

bool OpenConverter(uint32_t src, uint32_t dst, void** handle)
{
    esp_ae_rate_cvt_cfg_t cfg = {};
    cfg.src_rate = src;
    cfg.dest_rate = dst;
    cfg.channel = 1;
    cfg.bits_per_sample = 16;
    cfg.complexity = 3;
    cfg.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
    return esp_ae_rate_cvt_open(&cfg, handle) == ESP_AE_ERR_OK && *handle;
}
}  // namespace

Codec::~Codec() { Close(); }

void Codec::Close()
{
    if (encoder_) esp_opus_enc_close(encoder_);
    if (decoder_) esp_opus_dec_close(decoder_);
    if (up_) esp_ae_rate_cvt_close(up_);
    if (down_) esp_ae_rate_cvt_close(down_);
    encoder_ = decoder_ = up_ = down_ = nullptr;
    carry_ = 0;
    encoded_capacity_ = 0;
}

bool Codec::Open()
{
    Close();
    downlink_stats_ = {};
    esp_opus_enc_config_t enc = ESP_OPUS_ENC_CONFIG_DEFAULT();
    enc.sample_rate = 48000;
    enc.channel = 1;
    enc.bits_per_sample = 16;
    enc.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
    enc.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
    enc.complexity = 5;
    enc.enable_dtx = false;
    enc.enable_fec = false;
    enc.enable_vbr = true;
    esp_opus_dec_cfg_t dec = ESP_OPUS_DEC_CONFIG_DEFAULT();
    dec.sample_rate = 48000;
    dec.channel = 1;
    dec.frame_duration = ESP_OPUS_DEC_FRAME_DURATION_120_MS;
    int in_bytes = 0;
    if (esp_opus_enc_open(&enc, sizeof(enc), &encoder_) != ESP_AUDIO_ERR_OK || !encoder_ ||
        esp_opus_enc_get_frame_size(encoder_, &in_bytes, &encoded_capacity_) != ESP_AUDIO_ERR_OK ||
        in_bytes != kFrameSamples * sizeof(int16_t) || encoded_capacity_ <= 0 ||
        esp_opus_dec_open(&dec, sizeof(dec), &decoder_) != ESP_AUDIO_ERR_OK || !decoder_ ||
        !OpenConverter(16000, 48000, &up_) || !OpenConverter(48000, 24000, &down_)) {
        Close();
        return false;
    }
    pcm48_.resize(kFrameSamples * 2);
    decoded_.resize(kMaxDecodedSamples);
    return true;
}

bool Codec::Encode20ms(const int16_t* pcm16, size_t samples, std::vector<uint8_t>& packet)
{
    packet.clear();
    if (!encoder_ || !up_ || !pcm16 || samples != 320) return false;
    uint32_t capacity = 0;
    if (esp_ae_rate_cvt_get_max_out_sample_num(up_, samples, &capacity) != ESP_AE_ERR_OK ||
        capacity == 0 || capacity > kFrameSamples * 2 || carry_ >= kFrameSamples) return false;
    if (carry_ + capacity > pcm48_.size()) pcm48_.resize(carry_ + capacity);
    uint32_t written = capacity;
    if (esp_ae_rate_cvt_process(up_, const_cast<int16_t*>(pcm16), samples,
                              pcm48_.data() + carry_, &written) != ESP_AE_ERR_OK ||
        written > capacity) return false;
    carry_ += written;
    if (carry_ < kFrameSamples) return true;
    // One 20ms input must not build an unbounded conversion backlog.
    if (carry_ >= kFrameSamples * 2) return false;
    packet.resize(encoded_capacity_);
    esp_audio_enc_in_frame_t in = {};
    in.buffer = reinterpret_cast<uint8_t*>(pcm48_.data());
    in.len = kFrameSamples * sizeof(int16_t);
    esp_audio_enc_out_frame_t out = {};
    out.buffer = packet.data();
    out.len = packet.size();
    const auto rc = esp_opus_enc_process(encoder_, &in, &out);
    carry_ -= kFrameSamples;
    memmove(pcm48_.data(), pcm48_.data() + kFrameSamples, carry_ * sizeof(int16_t));
    if (rc != ESP_AUDIO_ERR_OK || out.encoded_bytes == 0 ||
        out.encoded_bytes > packet.size() || out.encoded_bytes > kMaxPayload) {
        packet.clear();
        return false;
    }
    packet.resize(out.encoded_bytes);
    return true;
}

bool Codec::Decode(const uint8_t* packet, size_t bytes, std::vector<int16_t>& pcm24)
{
    pcm24.clear();
    if (!decoder_ || !down_ || !packet || bytes == 0 || bytes > kMaxPayload) return false;
    esp_audio_dec_in_raw_t raw = {};
    raw.buffer = const_cast<uint8_t*>(packet);
    raw.len = bytes;
    raw.frame_recover = ESP_AUDIO_DEC_RECOVERY_NONE;
    esp_audio_dec_out_frame_t frame = {};
    frame.buffer = reinterpret_cast<uint8_t*>(decoded_.data());
    frame.len = decoded_.size() * sizeof(int16_t);
    esp_audio_dec_info_t info = {};
    if (esp_opus_dec_decode(decoder_, &raw, &frame, &info) != ESP_AUDIO_ERR_OK ||
        raw.consumed != bytes || frame.needed_size > frame.len ||
        frame.decoded_size == 0 || frame.decoded_size > frame.len ||
        frame.decoded_size % sizeof(int16_t) || info.sample_rate != 48000 || info.channel != 1) return false;
    const uint32_t samples = frame.decoded_size / sizeof(int16_t);
    uint32_t capacity = 0;
    if (esp_ae_rate_cvt_get_max_out_sample_num(down_, samples, &capacity) != ESP_AE_ERR_OK ||
        capacity == 0 || capacity > kMaxDecodedSamples) return false;
    pcm24.resize(capacity);
    uint32_t written = capacity;
    if (esp_ae_rate_cvt_process(down_, decoded_.data(), samples, pcm24.data(), &written) != ESP_AE_ERR_OK ||
        written > capacity) {
        pcm24.clear();
        return false;
    }
    pcm24.resize(written);
    downlink_stats_.decoded_samples_48k += samples;
    downlink_stats_.converted_samples_24k += written;
    size_t bucket = 7;
    switch (samples) {
        case 120: bucket = 0; break;
        case 240: bucket = 1; break;
        case 480: bucket = 2; break;
        case 960: bucket = 3; break;
        case 1920: bucket = 4; break;
        case 2880: bucket = 5; break;
        case 5760: bucket = 6; break;
    }
    ++downlink_stats_.decoded_samples_histogram[bucket];
    for (const int16_t sample : pcm24) {
        const int32_t value = sample;
        const uint32_t magnitude = value < 0 ? -value : value;
        if (magnitude > downlink_stats_.pcm24_max_abs) downlink_stats_.pcm24_max_abs = magnitude;
        if (value == -32768 || value == 32767) ++downlink_stats_.pcm24_endpoint_samples;
    }
    return true;
}

bool Codec::ResetDownlink()
{
    if (!decoder_ || esp_opus_dec_reset(decoder_) != ESP_AUDIO_ERR_OK) return false;
    if (down_) esp_ae_rate_cvt_close(down_);
    down_ = nullptr;
    return OpenConverter(48000, 24000, &down_);
}
}  // namespace webrtc_media
#endif
