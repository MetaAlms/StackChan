#include <sdkconfig.h>

// PROBE_GUARD: M1 media-uplink probe. Compiled only for that build so the normal
// firmware carries none of this code or the fixture.
#if CONFIG_STACKCHAN_WEBRTC_M1

#include "webrtc_m1.h"

#include <atomic>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

#include <cJSON.h>
#include <esp_ae_rate_cvt.h>
#include <esp_audio_types.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include "encoder/impl/esp_opus_enc.h"
#include "webrtc_transport.h"

#define TAG "M1"

namespace {

// ------------------------------------------------------------------ fixture

// Embedded by main/CMakeLists.txt only when CONFIG_STACKCHAN_WEBRTC_M1 is on,
// so the normal firmware never carries this audio. ESP-IDF's EMBED_FILES names
// the symbols after the file's basename, and also emits a <name>_length.
extern const uint8_t zh_1_pcm[] asm("zh_1_pcm");
extern const uint32_t zh_1_pcm_length asm("zh_1_pcm_length");
extern const uint8_t zh_2_pcm[] asm("zh_2_pcm");
extern const uint32_t zh_2_pcm_length asm("zh_2_pcm_length");
extern const uint8_t zh_3_pcm[] asm("zh_3_pcm");
extern const uint32_t zh_3_pcm_length asm("zh_3_pcm_length");

const char* kKw1[] = {"测试", "语音", "连接"};
const char* kKw2[] = {"蓝色", "书"};
const char* kKw3[] = {"一加一", "等于"};

struct Clip {
    const char* id;
    const char* text;
    const char* const* keywords;
    int keyword_count;
    const uint8_t* data;
    size_t size;
};

Clip g_clips[] = {
    {"zh_1", "今天我们测试语音连接", kKw1, 3, nullptr, 0},
    {"zh_2", "桌上有一本蓝色的书", kKw2, 2, nullptr, 0},
    {"zh_3", "请回答一加一等于几", kKw3, 2, nullptr, 0},
};
constexpr int kClipCount = sizeof(g_clips) / sizeof(g_clips[0]);

// --------------------------------------------------------------- constants

constexpr int kFixtureRate = 16000;
constexpr int kPcmRate = 48000;
constexpr int kChannels = 1;
/** Gap between clips so the server sees a clean turn boundary. */
constexpr int kGapMs = 800;
constexpr int kSessionTimeoutMs = 25000;
constexpr int kAsrTimeoutMs = 20000;
constexpr int kSamplePeriodMs = 30000;
/** Per-frame sender task stack. */
constexpr int kSenderStackWords = 8192;

#if CONFIG_STACKCHAN_WEBRTC_M1_FRAME_60MS
constexpr int kFrameMs = 60;
constexpr esp_opus_enc_frame_duration_t kFrameDuration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
#else
constexpr int kFrameMs = 20;
constexpr esp_opus_enc_frame_duration_t kFrameDuration = ESP_OPUS_ENC_FRAME_DURATION_20_MS;
#endif
static_assert(kFrameMs == 20 || kFrameMs == 60, "frame duration must be 20 or 60 ms");

// ------------------------------------------------------------------ globals

webrtc_transport::Transport g_transport;
std::atomic<bool> g_stop{false};

struct Metrics {
    std::mutex mtx;
    uint32_t packets = 0;
    uint64_t bytes = 0;
    int max_payload = 0;
    uint32_t first_pts = 0;
    uint32_t last_pts = 0;
    uint32_t pacing_late = 0;
    uint32_t send_fail = 0;
    uint32_t encode_fail = 0;
    uint32_t resample_fail = 0;
    uint32_t oversize = 0;
    uint32_t clips_played = 0;
    int64_t media_us = 0;
    int64_t wall_us = 0;
} g_metrics;

struct Observed {
    std::mutex mtx;
    bool session_created = false;
    bool session_updated = false;
    std::string config_echo;
    uint16_t reply_stream = 0xFFFF;
    bool reply_stream_known = false;
    bool update_sent = false;

    int speech_started = 0;
    int speech_stopped = 0;
    int asr_completed = 0;
    int asr_failed = 0;
    int server_errors = 0;
    std::string last_transcript;
    std::string last_item_id;
    std::string last_error;
} g_obs;

// ------------------------------------------------------------ media sender

/**
 * @brief Sole owner of the uplink media path.
 *
 * One resampler and one encoder, created once and reused for every frame. The
 * resampler is never recreated, so its filter state persists; only the sample
 * surplus is carried across calls. This is the only place that encodes or
 * sends audio.
 */
class MediaSender {
public:
    bool Open(std::string* err)
    {
        esp_ae_rate_cvt_cfg_t rc = {};
        rc.src_rate = kFixtureRate;
        rc.dest_rate = kPcmRate;
        rc.channel = kChannels;
        rc.bits_per_sample = 16;
        rc.complexity = 3;
        rc.perf_type = ESP_AE_RATE_CVT_PERF_TYPE_SPEED;
        if (esp_ae_rate_cvt_open(&rc, &resampler_) != ESP_AE_ERR_OK || resampler_ == nullptr) {
            *err = "resampler open failed";
            return false;
        }

        esp_opus_enc_config_t oc = ESP_OPUS_ENC_CONFIG_DEFAULT();
        oc.sample_rate = kPcmRate;
        oc.channel = kChannels;
        oc.bits_per_sample = 16;
        oc.frame_duration = kFrameDuration;
        oc.application_mode = ESP_OPUS_ENC_APPLICATION_AUDIO;
        oc.complexity = 5;
        oc.enable_fec = false;
        // DTX off: it would punch holes in the media timeline, which must
        // advance independently of whether a frame carried energy.
        oc.enable_dtx = false;
        oc.enable_vbr = true;
        if (esp_opus_enc_open(&oc, sizeof(oc), &encoder_) != ESP_AUDIO_ERR_OK || encoder_ == nullptr) {
            *err = "opus encoder open failed";
            return false;
        }

        int in_bytes = 0;
        int out_cap = 0;
        if (esp_opus_enc_get_frame_size(encoder_, &in_bytes, &out_cap) != ESP_AUDIO_ERR_OK) {
            *err = "esp_opus_enc_get_frame_size failed";
            return false;
        }
        // get_frame_size reports BYTES; derive the per-channel sample count.
        in_frame_bytes_ = in_bytes;
        out_capacity_ = out_cap;
        in_frame_samples_ = in_bytes / (kChannels * (int)sizeof(int16_t));

        const int expected = kPcmRate * kFrameMs / 1000;
        if (in_frame_samples_ != expected) {
            ESP_LOGE(TAG, "[enc] frame size %d samples, expected %d", in_frame_samples_, expected);
            *err = "encoder frame size unexpected";
            return false;
        }

        esp_audio_enc_info_t info = {};
        const bool have_info = (esp_opus_enc_get_info(encoder_, &info) == ESP_AUDIO_ERR_OK);
        ESP_LOGI(TAG, "[enc] %d Hz %d ch %d ms | in_frame=%d B (%d samples) out_cap=%d B%s%d",
                 oc.sample_rate, oc.channel, kFrameMs, in_frame_bytes_, in_frame_samples_,
                 out_capacity_, have_info ? " bitrate=" : " bitrate=?", have_info ? info.bitrate : 0);

        pcm48_.assign(in_frame_samples_ + kSlack, 0);
        encoded_.assign(out_capacity_, 0);
        return true;
    }

    void Close()
    {
        if (encoder_ != nullptr) {
            esp_opus_enc_close(encoder_);
            encoder_ = nullptr;
        }
        if (resampler_ != nullptr) {
            esp_ae_rate_cvt_close(resampler_);
            resampler_ = nullptr;
        }
    }

    /** Reset only the carry-over, keeping resampler filter state. */
    void ResetCarry() { pcm48_used_ = 0; }

    /**
     * @brief Resample one 16 kHz frame to 48 kHz and encode exactly one packet.
     * @return true when `*out`/`*out_size` hold one complete raw Opus packet
     */
    bool EncodeFrame(const int16_t* in16, int in_samples, const uint8_t** out, int* out_size)
    {
        uint32_t max_out = 0;
        if (esp_ae_rate_cvt_get_max_out_sample_num(resampler_, in_samples, &max_out) != ESP_AE_ERR_OK) {
            ++resample_fail_;
            return false;
        }
        if ((int)max_out + pcm48_used_ + kSlack > (int)pcm48_.size()) {
            pcm48_.resize(max_out + pcm48_used_ + kSlack);
        }

        uint32_t actual_out = 0;
        if (esp_ae_rate_cvt_process(resampler_, (esp_ae_sample_t)const_cast<int16_t*>(in16),
                                    (uint32_t)in_samples,
                                    (esp_ae_sample_t)(pcm48_.data() + pcm48_used_),
                                    &actual_out) != ESP_AE_ERR_OK) {
            ++resample_fail_;
            return false;
        }
        // Only actual_out is meaningful.
        pcm48_used_ += (int)actual_out;

        if (pcm48_used_ < in_frame_samples_) {
            return false;  // need another input frame to fill one encoder frame
        }

        esp_audio_enc_in_frame_t in = {};
        in.buffer = (uint8_t*)pcm48_.data();
        // Must be exactly the frame size the getter reported, in bytes.
        in.len = (uint32_t)in_frame_bytes_;

        esp_audio_enc_out_frame_t enc_out = {};
        enc_out.buffer = encoded_.data();
        enc_out.len = (uint32_t)out_capacity_;

        if (esp_opus_enc_process(encoder_, &in, &enc_out) != ESP_AUDIO_ERR_OK) {
            ++encode_fail_;
            return false;
        }

        const int surplus = pcm48_used_ - in_frame_samples_;
        if (surplus > 0) {
            memmove(pcm48_.data(), pcm48_.data() + in_frame_samples_,
                    (size_t)surplus * sizeof(int16_t));
        }
        pcm48_used_ = surplus;

        if (enc_out.encoded_bytes == 0 || enc_out.encoded_bytes > (uint32_t)out_capacity_) {
            ESP_LOGE(TAG, "[enc] implausible length %u (cap %d)",
                     (unsigned)enc_out.encoded_bytes, out_capacity_);
            ++encode_fail_;
            return false;
        }

        *out = encoded_.data();
        *out_size = (int)enc_out.encoded_bytes;
        return true;
    }

    uint32_t resample_fail() const { return resample_fail_; }
    uint32_t encode_fail() const { return encode_fail_; }

private:
    static constexpr int kSlack = 64;

    esp_ae_rate_cvt_handle_t resampler_ = nullptr;
    void* encoder_ = nullptr;
    int in_frame_bytes_ = 0;
    int in_frame_samples_ = 0;
    int out_capacity_ = 0;
    int pcm48_used_ = 0;
    std::vector<int16_t> pcm48_;
    std::vector<uint8_t> encoded_;
    uint32_t resample_fail_ = 0;
    uint32_t encode_fail_ = 0;
};

MediaSender g_sender;

// ---------------------------------------------------------- event handling

std::string SummariseSession(const cJSON* session)
{
    std::string out;
    if (session == nullptr) {
        return out;
    }
    const cJSON* td = cJSON_GetObjectItemCaseSensitive(session, "turn_detection");
    if (cJSON_IsObject(td)) {
        const cJSON* type = cJSON_GetObjectItemCaseSensitive(td, "type");
        const cJSON* thr = cJSON_GetObjectItemCaseSensitive(td, "threshold");
        const cJSON* sil = cJSON_GetObjectItemCaseSensitive(td, "silence_duration_ms");
        char b[96];
        snprintf(b, sizeof(b), "turn_detection=%s thr=%.2f silence=%d",
                 cJSON_IsString(type) ? type->valuestring : "?",
                 cJSON_IsNumber(thr) ? thr->valuedouble : -1.0,
                 cJSON_IsNumber(sil) ? sil->valueint : -1);
        out += b;
    }
    const cJSON* tr = cJSON_GetObjectItemCaseSensitive(session, "input_audio_transcription");
    if (cJSON_IsObject(tr)) {
        const cJSON* model = cJSON_GetObjectItemCaseSensitive(tr, "model");
        out += " transcription=";
        out += cJSON_IsString(model) ? model->valuestring : "?";
    } else {
        out += " transcription=(absent)";
    }
    return out;
}

/** Send session.update on the stream this session discovered. */
void TrySendSessionUpdate()
{
    uint16_t stream = 0xFFFF;
    {
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        if (!g_obs.session_created || g_obs.update_sent || !g_obs.reply_stream_known) {
            return;
        }
        stream = g_obs.reply_stream;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event_id", "event_m1_cfg");
    cJSON_AddStringToObject(root, "type", "session.update");
    cJSON* session = cJSON_CreateObject();
    cJSON_AddItemToObject(root, "session", session);

    cJSON* modalities = cJSON_CreateArray();
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
    cJSON_AddItemToObject(session, "modalities", modalities);

    cJSON* td = cJSON_CreateObject();
    cJSON_AddStringToObject(td, "type", "server_vad");
    cJSON_AddNumberToObject(td, "threshold", 0.5);
    cJSON_AddNumberToObject(td, "silence_duration_ms", 800);
    cJSON_AddItemToObject(session, "turn_detection", td);

    // The transcription object is a JSON field; the SDK's
    // enable_input_audio_transcription is not one.
    cJSON* tr = cJSON_CreateObject();
    cJSON_AddStringToObject(tr, "model", "qwen3-asr-flash-realtime");
    cJSON_AddItemToObject(session, "input_audio_transcription", tr);

    char* text = cJSON_PrintUnformatted(root);
    const std::string json = text ? text : "";
    if (text) {
        cJSON_free(text);
    }
    cJSON_Delete(root);

    if (json.empty()) {
        ESP_LOGE(TAG, "[cfg] failed to serialise session.update");
        return;
    }
    if (g_transport.SendJson(json, stream)) {
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        g_obs.update_sent = true;
        ESP_LOGI(TAG, "[cfg] session.update sent on stream %u", (unsigned)stream);
    } else {
        ESP_LOGW(TAG, "[cfg] session.update send failed; will retry on next event");
    }
}

void HandleServerEvent(const std::string& json, uint16_t stream_id)
{
    cJSON* root = cJSON_ParseWithLength(json.c_str(), json.size());
    if (root == nullptr) {
        ESP_LOGW(TAG, "[data] unparsable JSON (%d bytes)", (int)json.size());
        return;
    }
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(root, "type");
    const std::string t = cJSON_IsString(type) ? type->valuestring : "";

    // Exact `type` matching; substring hits are not acceptance evidence.
    if (t == "session.created") {
        {
            std::lock_guard<std::mutex> lock(g_obs.mtx);
            g_obs.session_created = true;
            if (!g_obs.reply_stream_known) {
                g_obs.reply_stream = stream_id;
                g_obs.reply_stream_known = true;
            }
        }
        ESP_LOGW(TAG, "[cfg] session.created on stream %u", (unsigned)stream_id);
        TrySendSessionUpdate();
    } else if (t == "session.updated") {
        const cJSON* session = cJSON_GetObjectItemCaseSensitive(root, "session");
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        g_obs.session_updated = true;
        g_obs.config_echo = SummariseSession(session);
        ESP_LOGW(TAG, "[cfg] session.updated echo: %s", g_obs.config_echo.c_str());
    } else if (t == "input_audio_buffer.speech_started") {
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        ++g_obs.speech_started;
        ESP_LOGW(TAG, "[vad] speech_started (#%d)", g_obs.speech_started);
    } else if (t == "input_audio_buffer.speech_stopped") {
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        ++g_obs.speech_stopped;
        ESP_LOGW(TAG, "[vad] speech_stopped (#%d)", g_obs.speech_stopped);
    } else if (t == "conversation.item.input_audio_transcription.completed") {
        const cJSON* tr = cJSON_GetObjectItemCaseSensitive(root, "transcript");
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "item_id");
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        ++g_obs.asr_completed;
        g_obs.last_transcript = cJSON_IsString(tr) ? tr->valuestring : "";
        g_obs.last_item_id = cJSON_IsString(id) ? id->valuestring : "";
        ESP_LOGW(TAG, "[asr] completed #%d item=%s transcript=\"%s\"",
                 g_obs.asr_completed, g_obs.last_item_id.c_str(), g_obs.last_transcript.c_str());
    } else if (t == "conversation.item.input_audio_transcription.failed") {
        const cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON* msg = cJSON_IsObject(err)
                               ? cJSON_GetObjectItemCaseSensitive(err, "message")
                               : nullptr;
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        ++g_obs.asr_failed;
        g_obs.last_error = cJSON_IsString(msg) ? msg->valuestring : "(no message)";
        ESP_LOGE(TAG, "[asr] FAILED: %s", g_obs.last_error.c_str());
    } else if (t == "error") {
        const cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON* msg = cJSON_IsObject(err)
                               ? cJSON_GetObjectItemCaseSensitive(err, "message")
                               : nullptr;
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        ++g_obs.server_errors;
        g_obs.last_error = cJSON_IsString(msg) ? msg->valuestring : "(no message)";
        ESP_LOGE(TAG, "[err] server error: %s", g_obs.last_error.c_str());
    } else {
        ESP_LOGI(TAG, "[data] type=%s", t.c_str());
    }

    cJSON_Delete(root);
}

// -------------------------------------------------------------- send paths

/**
 * @brief Stream one clip (or silence) at realtime pace.
 *
 * Pacing uses absolute media deadlines rather than a relative sleep, so work
 * done per frame cannot drift the timeline. Dropped or failed frames still
 * advance `pts_ms`: the RTP timestamp must follow media time, not the number
 * of packets that happened to succeed.
 */
void StreamPcm(const uint8_t* pcm, size_t pcm_bytes, uint32_t* pts_ms)
{
    const int in_samples = kFixtureRate * kFrameMs / 1000;
    const int in_bytes = in_samples * (int)sizeof(int16_t);
    std::vector<int16_t> pad(in_samples, 0);

    g_sender.ResetCarry();
    int64_t deadline_us = esp_timer_get_time();
    const int64_t start_us = deadline_us;
    int frames = 0;

    // One extra frame boundary so a fixture whose length is not a multiple of
    // the frame size still gets its tail encoded (zero padded).
    for (size_t off = 0; off < pcm_bytes + (size_t)in_bytes; off += (size_t)in_bytes) {
        if (g_stop.load() || !g_transport.CanSendAudio()) {
            return;
        }

        const int16_t* src = nullptr;
        if (off + (size_t)in_bytes <= pcm_bytes) {
            src = reinterpret_cast<const int16_t*>(pcm + off);
        } else {
            const size_t avail = (pcm_bytes > off) ? (pcm_bytes - off) : 0;
            if (avail == 0) {
                break;
            }
            memcpy(pad.data(), pcm + off, avail);
            src = pad.data();
        }

        const uint8_t* out = nullptr;
        int out_size = 0;
        if (g_sender.EncodeFrame(src, in_samples, &out, &out_size)) {
            if (out_size > g_transport.MaxPayloadBytes()) {
                std::lock_guard<std::mutex> lock(g_metrics.mtx);
                ++g_metrics.oversize;
                ESP_LOGE(TAG, "[pkt] %d B exceeds budget %d B; dropped",
                         out_size, g_transport.MaxPayloadBytes());
            } else {
                const bool sent = g_transport.SendAudio(out, out_size, *pts_ms);
                std::lock_guard<std::mutex> lock(g_metrics.mtx);
                if (sent) {
                    ++g_metrics.packets;
                    g_metrics.bytes += (uint64_t)out_size;
                    if (out_size > g_metrics.max_payload) {
                        g_metrics.max_payload = out_size;
                    }
                    if (g_metrics.packets == 1) {
                        g_metrics.first_pts = *pts_ms;
                    }
                    g_metrics.last_pts = *pts_ms;
                } else {
                    ++g_metrics.send_fail;
                }
            }
        }
        *pts_ms += kFrameMs;
        ++frames;

        deadline_us += (int64_t)kFrameMs * 1000;
        const int64_t now = esp_timer_get_time();
        if (now < deadline_us) {
            vTaskDelay(pdMS_TO_TICKS((deadline_us - now) / 1000));
        } else if (now - deadline_us > 20000) {
            std::lock_guard<std::mutex> lock(g_metrics.mtx);
            ++g_metrics.pacing_late;
        }
    }

    const int64_t end_us = esp_timer_get_time();
    {
        std::lock_guard<std::mutex> lock(g_metrics.mtx);
        g_metrics.media_us += (int64_t)frames * kFrameMs * 1000;
        g_metrics.wall_us += (end_us - start_us);
    }

}

// ---------------------------------------------------------------- resources

struct ResourceSample {
    int64_t t_ms;
    size_t internal_free, internal_min, internal_largest;
    size_t dma_free, dma_largest;
    size_t psram_free, psram_min, psram_largest;
    UBaseType_t sender_stack_free;
};

void SampleResources(std::vector<ResourceSample>& out, TaskHandle_t sender)
{
    ResourceSample s = {};
    s.t_ms = esp_timer_get_time() / 1000;
    s.internal_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s.internal_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    s.internal_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    s.dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    s.dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    s.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s.psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    s.psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    s.sender_stack_free = sender ? uxTaskGetStackHighWaterMark(sender) : 0;
    out.push_back(s);

    ESP_LOGI(TAG,
             "[res] t=%llds | int free=%u min=%u largest=%u | dma free=%u largest=%u | "
             "psram free=%u min=%u largest=%u | sender stack free=%u",
             (long long)(s.t_ms / 1000),
             (unsigned)s.internal_free, (unsigned)s.internal_min, (unsigned)s.internal_largest,
             (unsigned)s.dma_free, (unsigned)s.dma_largest,
             (unsigned)s.psram_free, (unsigned)s.psram_min, (unsigned)s.psram_largest,
             (unsigned)s.sender_stack_free);
}

}  // namespace

// ============================================================== entry point

void WebRtcM1Run()
{
    std::vector<ResourceSample> samples;

    ESP_LOGW(TAG, "M1 uplink probe: fixture -> 16->48k -> Opus 48k mono %d ms -> RTP", kFrameMs);
    ESP_LOGW(TAG, "payload budget %d B (library whole-packet buffer %d B used internally)",
             g_transport.MaxPayloadBytes(), 1428);

    // Resolve the embedded ranges.
    {
        const uint8_t* starts[] = {zh_1_pcm, zh_2_pcm, zh_3_pcm};
        const uint32_t sizes[] = {zh_1_pcm_length, zh_2_pcm_length, zh_3_pcm_length};
        for (int i = 0; i < kClipCount; ++i) {
            g_clips[i].data = starts[i];
            g_clips[i].size = (size_t)sizes[i];
            ESP_LOGI(TAG, "[fx] %s %s: %u bytes (%d samples, %d ms) kw=%s,%s",
                     g_clips[i].id, g_clips[i].text, (unsigned)g_clips[i].size,
                     (int)(g_clips[i].size / 2), (int)(g_clips[i].size / 2 / kFixtureRate * 1000),
                     g_clips[i].keywords[0],
                     g_clips[i].keyword_count > 1 ? g_clips[i].keywords[1] : "-");
        }
    }

    std::string err;
    if (!g_sender.Open(&err)) {
        ESP_LOGE(TAG, "VERDICT: FAIL - media pipeline: %s", err.c_str());
        return;
    }

    webrtc_transport::Config tcfg = {};
    tcfg.pcm_sample_rate = kPcmRate;
    tcfg.pcm_channels = kChannels;
    tcfg.rtp_clock_rate = 48000;
    tcfg.enable_data_channel = true;
    tcfg.data_channel_label = "oai-events";

    webrtc_transport::Callbacks cb = {};
    cb.on_data = [](const std::string& json, uint16_t stream_id) {
        // Runs on the peer loop task: parse and book-keep only, never block.
        HandleServerEvent(json, stream_id);
    };
    cb.on_state = [](esp_peer_state_t s) {
        if (s == ESP_PEER_STATE_DISCONNECTED || s == ESP_PEER_STATE_CLOSED ||
            s == ESP_PEER_STATE_CONNECT_FAILED || s == ESP_PEER_STATE_DATA_CHANNEL_CLOSED ||
            s == ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED) {
            ESP_LOGW(TAG, "[state] link down (%d); stopping media", (int)s);
            g_stop.store(true);
        }
    };
    cb.on_channel_open = [](esp_peer_data_channel_info_t*) {};
    cb.on_audio = [](const uint8_t* data, int size, uint32_t pts) {
        // Downlink is out of scope for M1: count and drop, never block here.
        static std::atomic<int> n{0};
        const int c = ++n;
        if (c <= 3 || c % 200 == 0) {
            ESP_LOGI(TAG, "[dl] frame #%d %d B pts=%u (M1 discards downlink)",
                     c, size, (unsigned)pts);
        }
    };
    cb.on_before_close = []() {
        // Stop and join the media sender before the peer is closed; sending
        // concurrently with close is not allowed.
        g_stop.store(true);
        vTaskDelay(pdMS_TO_TICKS(200));
    };

    if (!g_transport.Start(tcfg, cb, &err)) {
        ESP_LOGE(TAG, "VERDICT: FAIL - transport: %s", err.c_str());
        g_sender.Close();
        return;
    }

    // Wait for session.updated, which is the configuration gate.
    int waited = 0;
    while (waited < kSessionTimeoutMs) {
        {
            std::lock_guard<std::mutex> lock(g_obs.mtx);
            if (g_obs.session_updated) {
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    {
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        if (!g_obs.session_updated) {
            ESP_LOGE(TAG,
                     "VERDICT: FAIL - configuration layer: session.updated not received "
                     "within %d ms (created=%d update_sent=%d). This is a configuration "
                     "failure, not a transport failure.",
                     kSessionTimeoutMs, (int)g_obs.session_created, (int)g_obs.update_sent);
            g_transport.Stop();
            g_sender.Close();
            return;
        }
        ESP_LOGW(TAG, "[cfg] gate passed: %s", g_obs.config_echo.c_str());
    }

    // Play each clip once, pacing on an absolute media timeline.
    uint32_t pts_ms = 0;
    for (int i = 0; i < kClipCount; ++i) {
        if (g_stop.load()) {
            break;
        }
        const int before = [&] {
            std::lock_guard<std::mutex> lock(g_obs.mtx);
            return g_obs.asr_completed;
        }();

        ESP_LOGW(TAG, "[play] %s -> %s", g_clips[i].id, g_clips[i].text);
        StreamPcm(g_clips[i].data, g_clips[i].size, &pts_ms);
        // Trailing gap so the server observes end-of-speech for this turn.
        {
            std::vector<int16_t> zero(kFixtureRate * kGapMs / 1000, 0);
            StreamPcm(reinterpret_cast<const uint8_t*>(zero.data()),
                      zero.size() * sizeof(int16_t), &pts_ms);
        }
        {
            std::lock_guard<std::mutex> lock(g_metrics.mtx);
            ++g_metrics.clips_played;
        }

        // Wait for this clip's transcription, bounded.
        int w = 0;
        while (w < kAsrTimeoutMs) {
            int now = 0;
            {
                std::lock_guard<std::mutex> lock(g_obs.mtx);
                now = g_obs.asr_completed;
            }
            if (now > before) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(200));
            w += 200;
        }
        if (w >= kAsrTimeoutMs) {
            ESP_LOGW(TAG, "[asr] no completed transcription for %s within %d ms",
                     g_clips[i].id, kAsrTimeoutMs);
        }
    }

    // Sustained loop for resource evidence.
    const int64_t loop_end = esp_timer_get_time() +
                             (int64_t)CONFIG_STACKCHAN_WEBRTC_M1_LOOP_SECONDS * 1000000;
    int64_t next_sample = esp_timer_get_time() + (int64_t)kSamplePeriodMs * 1000;
    SampleResources(samples, nullptr);

    int rr = 0;
    while (esp_timer_get_time() < loop_end && !g_stop.load()) {
        StreamPcm(g_clips[rr].data, g_clips[rr].size, &pts_ms);
        {
            std::vector<int16_t> zero(kFixtureRate * kGapMs / 1000, 0);
            StreamPcm(reinterpret_cast<const uint8_t*>(zero.data()),
                      zero.size() * sizeof(int16_t), &pts_ms);
        }
        {
            std::lock_guard<std::mutex> lock(g_metrics.mtx);
            ++g_metrics.clips_played;
        }
        rr = (rr + 1) % kClipCount;

        if (esp_timer_get_time() >= next_sample) {
            SampleResources(samples, nullptr);
            next_sample += (int64_t)kSamplePeriodMs * 1000;
        }
    }

    // Summary.
    {
        std::lock_guard<std::mutex> mlock(g_metrics.mtx);
        std::lock_guard<std::mutex> olock(g_obs.mtx);
        ESP_LOGW(TAG, "==================== M1 SUMMARY ====================");
        ESP_LOGW(TAG, "  frame duration     : %d ms", kFrameMs);
        ESP_LOGW(TAG, "  clips played       : %u", g_metrics.clips_played);
        ESP_LOGW(TAG, "  opus packets       : %u", g_metrics.packets);
        ESP_LOGW(TAG, "  total payload      : %llu B", (unsigned long long)g_metrics.bytes);
        ESP_LOGW(TAG, "  max payload        : %d B (budget %d B)",
                 g_metrics.max_payload, g_transport.MaxPayloadBytes());
        ESP_LOGW(TAG, "  pts range          : %u .. %u ms (%u ms media)",
                 g_metrics.first_pts, g_metrics.last_pts,
                 g_metrics.last_pts - g_metrics.first_pts);
        ESP_LOGW(TAG, "  media/wall time    : %lld / %lld ms",
                 (long long)(g_metrics.media_us / 1000), (long long)(g_metrics.wall_us / 1000));
        ESP_LOGW(TAG, "  pacing late        : %u", g_metrics.pacing_late);
        ESP_LOGW(TAG, "  oversize dropped   : %u", g_metrics.oversize);
        ESP_LOGW(TAG, "  send/encode/resample failures: %u / %u / %u",
                 g_metrics.send_fail, g_sender.encode_fail(), g_sender.resample_fail());
        ESP_LOGW(TAG, "  ---- four-layer evidence ----");
        ESP_LOGW(TAG, "  [cfg] created=%d updated=%d echo=%s",
                 (int)g_obs.session_created, (int)g_obs.session_updated,
                 g_obs.config_echo.c_str());
        ESP_LOGW(TAG, "  [vad] started=%d stopped=%d", g_obs.speech_started, g_obs.speech_stopped);
        ESP_LOGW(TAG, "  [asr] completed=%d failed=%d server_errors=%d",
                 g_obs.asr_completed, g_obs.asr_failed, g_obs.server_errors);
        ESP_LOGW(TAG, "  [asr] last item=%s transcript=\"%s\"",
                 g_obs.last_item_id.c_str(), g_obs.last_transcript.c_str());
        if (!g_obs.last_error.empty()) {
            ESP_LOGW(TAG, "  last error         : %s", g_obs.last_error.c_str());
        }
        ESP_LOGW(TAG, "  resource samples   : %d", (int)samples.size());
        ESP_LOGW(TAG, "====================================================");
    }

    g_transport.Stop();
    g_sender.Close();
    ESP_LOGW(TAG, "M1 probe finished; idling");
}

#endif  // CONFIG_STACKCHAN_WEBRTC_M1
