#include <sdkconfig.h>

// PROBE_GUARD: M1 media-uplink probe. Compiled only for that build so the normal
// firmware carries none of this code or the fixture.
#if CONFIG_STACKCHAN_WEBRTC_M1

#include "webrtc_m1.h"

#include <algorithm>
#include <atomic>
#include <cmath>
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

#include "decoder/impl/esp_opus_dec.h"
#include "encoder/impl/esp_opus_enc.h"
#include "rtp_send_probe.h"
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
/**
 * Per-frame sender task stack, in BYTES.
 *
 * The media path runs here, not on the main task. On device,
 * CONFIG_ESP_MAIN_TASK_STACK_SIZE (8192) was overflowed by the first frame:
 * "***ERROR*** A stack overflow in task main has been detected", with ~149 KB of
 * internal heap still free. That is the one proven fact about the failure; it
 * does not by itself prove the earlier allocation abort had the same cause.
 *
 * ESP-IDF's xTaskCreate takes the stack as a NUMBER OF BYTES, unlike vanilla
 * FreeRTOS (task.h: "differs from vanilla FreeRTOS"), and
 * uxTaskGetStackHighWaterMark also reports bytes. Passing 8192 here would have
 * reproduced the same 8 KB stack. The WebSocket audio path gives its Opus
 * worker 2048*12 = 24576 bytes; 32768 is the starting point and the real
 * requirement is reported from the measured high-water mark.
 */
constexpr int kSenderStackBytes = 32768;

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
    uint32_t oom_stop = 0;
    uint32_t clips_played = 0;
    int64_t media_us = 0;
    int64_t wall_us = 0;
    /** Frames of media produced; media_ms = frames * kFrameMs, independent of
     *  the wall clock. */
    uint64_t frames_media = 0;
    /** Frames dropped or failed, which still advance the media timeline. */
    uint64_t frames_dropped = 0;
} g_metrics;

struct Observed {
    std::mutex mtx;
    bool session_created = false;
    bool session_updated = false;
    bool config_ok = false;
    std::string config_failure;
    std::string config_echo;
    uint16_t reply_stream = 0xFFFF;
    bool reply_stream_known = false;
    bool update_sent = false;

    int speech_started = 0;
    int speech_stopped = 0;
    int asr_completed = 0;
    int asr_failed = 0;
    int server_errors = 0;
    /** Events that arrived without the fields needed to attribute them. */
    uint32_t malformed_events = 0;
    std::string last_transcript;
    std::string last_item_id;
    std::string last_error;

    // Per-turn slot. The sender clears it before each clip and waits on it, so
    // a late completion from the previous turn is never credited to this one.
    bool turn_speech_started = false;
    bool turn_speech_stopped = false;
    bool turn_completed = false;
    bool turn_failed = false;
    std::string turn_transcript;
    std::string turn_item_id;
    std::string turn_failure;
} g_obs;

// ------------------------------------------------------------ media sender

/**
 * @brief Log per-capability heap so an allocation failure is observable.
 *
 * The first device run aborted inside the C++ exception machinery when an
 * allocation on the per-frame path failed; without this the only symptom was a
 * semaphore assert far from the cause.
 */
void LogHeap(const char* where)
{
    ESP_LOGW(TAG,
             "[heap:%s] int free=%u largest=%u | int+8bit free=%u largest=%u | "
             "dma free=%u largest=%u | psram free=%u largest=%u",
             where,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM));
}

/**
 * @brief Zero-padding buffer.
 *
 * Allocated once in MediaSender::Open() so the per-frame path never grows it.
 */
std::vector<int16_t> g_pad;

/**
 * @brief Diagnostics for locating the no-VAD root cause (M1-5).
 *
 * `esp_peer_send_audio` returning 0 does not prove the payload reached the
 * socket: the library's RTP encoder calls a void packet callback and returns 0
 * unconditionally, and the SRTP/socket write result is not propagated. These
 * measurements instead answer the two questions that matter:
 *   - did the resampler produce real audio,
 *   - is the encoded packet decodable audio rather than opaque bytes.
 */
struct Diagnostics {
    std::mutex mtx;
    double resample_rms_peak = 0.0;
    double resample_rms_sum = 0.0;
    uint32_t resample_frames = 0;
    double opus_rt_rms_peak = 0.0;
    double opus_rt_rms_sum = 0.0;
    uint32_t opus_rt_frames = 0;
    uint32_t opus_rt_fail = 0;
    int pkt_min = 1 << 30;
    int pkt_max = 0;
} g_diag;

/** RMS of s16 samples, normalised to 0..1. */
double Rms16(const int16_t* s, int n)
{
    if (n <= 0) {
        return 0.0;
    }
    double acc = 0.0;
    for (int i = 0; i < n; ++i) {
        const double v = (double)s[i] / 32768.0;
        acc += v * v;
    }
    return sqrt(acc / (double)n);
}

/** Refuse to run the media path when there is not enough contiguous RAM. */
bool HeapHasRoomForFrame()
{
    // One frame needs the 48k working buffer plus the encoder's scratch; require
    // a comfortable multiple so a marginal heap fails loudly instead of
    // aborting inside a later allocation.
    const size_t need = 16 * 1024;
    return heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT) >= need;
}

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
public:
    /** Bounded, allocation-free latency accumulator. */
    class LatencyLog {
    public:
        static constexpr size_t kCap = 4096;
        /**
         * @brief Strided sampling so the percentiles describe the whole run.
         *
         * Keeping only the first N samples made the report describe the first
         * 81 s of a 20 ms run, which is not the sustained loop. This keeps at
         * most kCap samples but spreads them across every call.
         */
        /**
         * @brief Uniform sampling at one fixed rate from the very first call.
         *
         * The previous version kept every sample until full and only then
         * switched to an every-4th stride. That mixture is not a uniform sample
         * of the run: a slow spell just after the buffer filled was 4x
         * over-represented, enough to move p95 the wrong way. Sampling every
         * kStride-th call from the start keeps the rate constant, so the stored
         * set is an unbiased sample of all calls and needs no reweighting.
         */
        void Add(int32_t us)
        {
            const uint64_t idx = seen_++;
            if ((idx % kStride) != 0) {
                return;
            }
            if (n_ < kCap) {
                v_[n_++] = us;
            } else {
                // Fixed-rate sampling can still overflow on a very long run:
                // keep counting and stop storing, and report the shortfall
                // rather than silently mislabelling the window.
                ++dropped_;
            }
        }
        size_t size() const { return n_; }
        uint64_t seen() const { return seen_; }
        uint64_t dropped() const { return dropped_; }
        /** Window actually described, as a fraction of all calls. */
        double coverage() const
        {
            return seen_ == 0 ? 0.0
                              : (double)n_ / (double)((seen_ + kStride - 1) / kStride);
        }
        int32_t at(size_t i) const { return v_[i]; }
        /** Copy out for sorting; only called once, off the media path. */
        std::vector<int32_t> Copy() const { return std::vector<int32_t>(v_, v_ + n_); }

    private:
        static constexpr uint64_t kStride = 4;
        int32_t v_[kCap] = {};
        size_t n_ = 0;
        uint64_t seen_ = 0;
        uint64_t dropped_ = 0;
    };

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

        // Reserve once with generous headroom so EncodeFrame never reallocates.
        LogHeap("before-buffers");
        pcm48_.assign(in_frame_samples_ * 2 + kSlack, 0);
        encoded_.assign(out_capacity_ + kSlack, 0);
        g_pad.assign(in_frame_samples_, 0);
        LogHeap("after-buffers");
        return true;
    }

    void OpenDecoder()
    {
        esp_opus_dec_cfg_t dc = ESP_OPUS_DEC_CONFIG_DEFAULT();
        dc.sample_rate = kPcmRate;
        dc.channel = kChannels;
        if (esp_opus_dec_open(&dc, sizeof(dc), &decoder_) != ESP_AUDIO_ERR_OK) {
            decoder_ = nullptr;
        }
        rt_pcm_.assign(5760, 0);   // 120 ms at 48 kHz mono, the max Opus frame
    }

    void Close()
    {
        if (decoder_ != nullptr) {
            esp_opus_dec_close(decoder_);
            decoder_ = nullptr;
        }
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

        const int64_t t_rs0 = esp_timer_get_time();
        // `out_sample_num` is an IN/OUT parameter: on entry it must hold the
        // capacity available at that pointer, on exit the number actually
        // written. Passing 0 makes every conversion fail.
        const uint32_t capacity = (uint32_t)((int)pcm48_.size() - pcm48_used_);
        uint32_t actual_out = capacity;
        const esp_ae_err_t rc = esp_ae_rate_cvt_process(
            resampler_, (esp_ae_sample_t)const_cast<int16_t*>(in16), (uint32_t)in_samples,
            (esp_ae_sample_t)(pcm48_.data() + pcm48_used_), &actual_out);
        if (rc != ESP_AE_ERR_OK) {
            ESP_LOGE(TAG, "[rs] process failed rc=%d (in=%d samples, cap=%u)",
                     (int)rc, in_samples, (unsigned)capacity);
            ++resample_fail_;
            return false;
        }
        if (actual_out > capacity) {
            ESP_LOGE(TAG, "[rs] wrote %u samples into a %u-sample capacity",
                     (unsigned)actual_out, (unsigned)capacity);
            ++resample_fail_;
            return false;
        }
        resample_us_.Add((int32_t)(esp_timer_get_time() - t_rs0));
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

        const int64_t t_enc0 = esp_timer_get_time();
        if (esp_opus_enc_process(encoder_, &in, &enc_out) != ESP_AUDIO_ERR_OK) {
            ++encode_fail_;
            return false;
        }
        encode_us_.Add((int32_t)(esp_timer_get_time() - t_enc0));

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

        {
            const double r = Rms16(pcm48_.data(), in_frame_samples_);
            std::lock_guard<std::mutex> lock(g_diag.mtx);
            if (r > g_diag.resample_rms_peak) {
                g_diag.resample_rms_peak = r;
            }
            g_diag.resample_rms_sum += r;
            ++g_diag.resample_frames;
        }

        *out = encoded_.data();
        *out_size = (int)enc_out.encoded_bytes;

        // Decode our own packet: a packet that decodes to silence means the
        // encoder produced nothing usable, whatever the send API returned.
        RoundTripCheck(encoded_.data(), *out_size);
        return true;
    }

    /** Decode one packet locally and record its energy. */
    void RoundTripCheck(const uint8_t* pkt, int size)
    {
        if (decoder_ == nullptr) {
            OpenDecoder();
            if (decoder_ == nullptr) {
                std::lock_guard<std::mutex> lock(g_diag.mtx);
                ++g_diag.opus_rt_fail;
                return;
            }
        }
        esp_audio_dec_in_raw_t raw = {};
        raw.buffer = const_cast<uint8_t*>(pkt);
        raw.len = (uint32_t)size;
        esp_audio_dec_out_frame_t frame = {};
        frame.buffer = (uint8_t*)rt_pcm_.data();
        frame.len = (uint32_t)(rt_pcm_.size() * sizeof(int16_t));
        esp_audio_dec_info_t info = {};

        const esp_audio_err_t rc = esp_opus_dec_decode(decoder_, &raw, &frame, &info);
        std::lock_guard<std::mutex> lock(g_diag.mtx);
        if (rc != ESP_AUDIO_ERR_OK || frame.decoded_size == 0) {
            ++g_diag.opus_rt_fail;
            return;
        }
        const int n = (int)(frame.decoded_size / sizeof(int16_t));
        const double r = Rms16(rt_pcm_.data(), n);
        if (r > g_diag.opus_rt_rms_peak) {
            g_diag.opus_rt_rms_peak = r;
        }
        g_diag.opus_rt_rms_sum += r;
        ++g_diag.opus_rt_frames;
        if (size < g_diag.pkt_min) {
            g_diag.pkt_min = size;
        }
        if (size > g_diag.pkt_max) {
            g_diag.pkt_max = size;
        }
    }

    uint32_t resample_fail() const { return resample_fail_; }
    uint32_t encode_fail() const { return encode_fail_; }

    /**
     * Per-call latencies in microseconds, for the bounded percentile report.
     *
     * Fixed storage on purpose: a growable vector in the per-frame path can
     * reallocate, and an allocation failure there aborts inside the C++
     * exception machinery rather than failing softly.
     */
    const LatencyLog& resample_us() const { return resample_us_; }
    const LatencyLog& encode_us() const { return encode_us_; }

private:
    static constexpr int kSlack = 64;

    esp_ae_rate_cvt_handle_t resampler_ = nullptr;
    void* encoder_ = nullptr;
    void* decoder_ = nullptr;      // diagnostic round-trip only
    std::vector<int16_t> rt_pcm_;
    int in_frame_bytes_ = 0;
    int in_frame_samples_ = 0;
    int out_capacity_ = 0;
    int pcm48_used_ = 0;
    std::vector<int16_t> pcm48_;
    std::vector<uint8_t> encoded_;
    uint32_t resample_fail_ = 0;
    uint32_t encode_fail_ = 0;
    LatencyLog resample_us_;
    LatencyLog encode_us_;
};

MediaSender g_sender;

// ---------------------------------------------------------- event handling

/** Expected configuration, checked against the server's echo. */
constexpr double kWantThreshold = 0.5;
constexpr int kWantSilenceMs = 800;
constexpr const char* kWantTurnType = "server_vad";
constexpr const char* kWantAsrModel = "qwen3-asr-flash-realtime";

struct ConfigCheck {
    bool ok = false;
    std::string detail;   // human-readable echo, always recorded
    std::string failure;  // set when ok == false
};

/**
 * @brief Verify the session.updated echo, not merely its arrival.
 *
 * Any session.updated used to pass the gate; a missing, null or wrong field
 * must be reported as a configuration failure and must not release the fixture.
 */
ConfigCheck VerifySessionConfig(const cJSON* session)
{
    ConfigCheck c;
    if (session == nullptr) {
        c.failure = "session object missing";
        return c;
    }

    const cJSON* td = cJSON_GetObjectItemCaseSensitive(session, "turn_detection");
    if (!cJSON_IsObject(td)) {
        c.failure = "turn_detection missing or not an object";
        return c;
    }
    const cJSON* type = cJSON_GetObjectItemCaseSensitive(td, "type");
    const cJSON* thr = cJSON_GetObjectItemCaseSensitive(td, "threshold");
    const cJSON* sil = cJSON_GetObjectItemCaseSensitive(td, "silence_duration_ms");
    if (!cJSON_IsString(type)) {
        c.failure = "turn_detection.type missing";
        return c;
    }

    char buf[192];
    snprintf(buf, sizeof(buf), "turn_detection=%s thr=%.2f silence=%d", type->valuestring,
             cJSON_IsNumber(thr) ? thr->valuedouble : -1.0,
             cJSON_IsNumber(sil) ? sil->valueint : -1);
    c.detail = buf;

    if (strcmp(type->valuestring, kWantTurnType) != 0) {
        c.failure = std::string("turn_detection.type is ") + type->valuestring + ", want " +
                    kWantTurnType;
        return c;
    }
    if (!cJSON_IsNumber(thr) || thr->valuedouble != kWantThreshold) {
        c.failure = "turn_detection.threshold is not 0.5";
        return c;
    }
    if (!cJSON_IsNumber(sil) || sil->valueint != kWantSilenceMs) {
        c.failure = "turn_detection.silence_duration_ms is not 800";
        return c;
    }

    const cJSON* tr = cJSON_GetObjectItemCaseSensitive(session, "input_audio_transcription");
    if (!cJSON_IsObject(tr)) {
        c.failure = "input_audio_transcription missing or not an object";
        return c;
    }
    const cJSON* model = cJSON_GetObjectItemCaseSensitive(tr, "model");
    if (!cJSON_IsString(model)) {
        c.failure = "input_audio_transcription.model missing";
        return c;
    }
    c.detail += " transcription=";
    c.detail += model->valuestring;
    if (strcmp(model->valuestring, kWantAsrModel) != 0) {
        c.failure = std::string("transcription model is ") + model->valuestring + ", want " +
                    kWantAsrModel;
        return c;
    }

    c.ok = true;
    return c;
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
        const ConfigCheck c = VerifySessionConfig(session);
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        g_obs.session_updated = true;
        g_obs.config_echo = c.detail;
        g_obs.config_ok = c.ok;
        g_obs.config_failure = c.failure;
        if (c.ok) {
            ESP_LOGW(TAG, "[cfg] session.updated verified: %s", c.detail.c_str());
        } else {
            ESP_LOGE(TAG, "[cfg] session.updated REJECTED: %s (echo: %s)",
                     c.failure.c_str(), c.detail.c_str());
        }
    } else if (t == "input_audio_buffer.speech_started") {
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        ++g_obs.speech_started;
        g_obs.turn_speech_started = true;
        ESP_LOGW(TAG, "[vad] speech_started (#%d)", g_obs.speech_started);
    } else if (t == "input_audio_buffer.speech_stopped") {
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        ++g_obs.speech_stopped;
        g_obs.turn_speech_stopped = true;
        ESP_LOGW(TAG, "[vad] speech_stopped (#%d)", g_obs.speech_stopped);
    } else if (t == "conversation.item.input_audio_transcription.completed") {
        const cJSON* tr = cJSON_GetObjectItemCaseSensitive(root, "transcript");
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "item_id");
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        ++g_obs.asr_completed;
        // Required fields: a completed event without a transcript or item_id
        // cannot be attributed to a clip, so it must not release one.
        if (!cJSON_IsString(tr) || !cJSON_IsString(id) || id->valuestring[0] == '\0') {
            g_obs.malformed_events++;
            ESP_LOGW(TAG, "[asr] completed event missing transcript/item_id; not "
                          "attributed to any clip");
            cJSON_Delete(root);
            return;
        }
        g_obs.last_transcript = tr->valuestring;
        g_obs.last_item_id = id->valuestring;
        g_obs.turn_completed = true;
        g_obs.turn_transcript = g_obs.last_transcript;
        g_obs.turn_item_id = g_obs.last_item_id;
        ESP_LOGW(TAG, "[asr] completed #%d item=%s transcript=\"%s\"",
                 g_obs.asr_completed, g_obs.last_item_id.c_str(), g_obs.last_transcript.c_str());
    } else if (t == "conversation.item.input_audio_transcription.failed") {
        const cJSON* err = cJSON_GetObjectItemCaseSensitive(root, "error");
        const cJSON* id = cJSON_GetObjectItemCaseSensitive(root, "item_id");
        const cJSON* code = cJSON_IsObject(err) ? cJSON_GetObjectItemCaseSensitive(err, "code") : nullptr;
        const cJSON* msg = cJSON_IsObject(err) ? cJSON_GetObjectItemCaseSensitive(err, "message") : nullptr;
        const cJSON* param = cJSON_IsObject(err) ? cJSON_GetObjectItemCaseSensitive(err, "param") : nullptr;
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        ++g_obs.asr_failed;
        char b[256];
        snprintf(b, sizeof(b), "item=%s code=%s message=%s param=%s",
                 cJSON_IsString(id) ? id->valuestring : "?",
                 cJSON_IsString(code) ? code->valuestring : "?",
                 cJSON_IsString(msg) ? msg->valuestring : "?",
                 cJSON_IsString(param) ? param->valuestring : "-");
        g_obs.last_error = b;
        g_obs.turn_failed = true;
        g_obs.turn_failure = b;
        ESP_LOGE(TAG, "[asr] FAILED: %s", b);
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
 * @brief Media clock shared by every send path.
 *
 * One monotonic media timeline in milliseconds. Waiting is *not* allowed to
 * compress it: whoever waits keeps feeding silence at the frame cadence, so a
 * multi-second ASR wait still shows up as multi-second media time.
 */
struct MediaClock {
    uint32_t pts_ms = 0;
    int64_t deadline_us = 0;

    void Arm() { deadline_us = esp_timer_get_time(); }

    /** Advance one frame and sleep until its absolute deadline. */
    void Step()
    {
        pts_ms += kFrameMs;
        deadline_us += (int64_t)kFrameMs * 1000;
        const int64_t now = esp_timer_get_time();
        if (now < deadline_us) {
            vTaskDelay(pdMS_TO_TICKS((deadline_us - now) / 1000));
        } else if (now - deadline_us > 20000) {
            std::lock_guard<std::mutex> lock(g_metrics.mtx);
            ++g_metrics.pacing_late;
        }
    }
} g_clock;

/**
 * @brief Encode and send one frame from a PCM buffer (nullptr = silence).
 * @return false when the link is gone and the caller should unwind
 */
bool SendOneFrame(const uint8_t* pcm, size_t pcm_bytes, size_t off)
{
    if (g_stop.load() || !g_transport.CanSendAudio()) {
        return false;
    }

    // Observable failure path: a marginal heap must be reported here rather
    // than aborting inside a later allocation with an unrelated semaphore
    // assert. Checked once per frame because the transport's buffers are live.
    static bool first_frame = true;
    static int frame_no = 0;
    if (first_frame) {
        LogHeap("first-frame-before");
    }
    if (!HeapHasRoomForFrame()) {
        std::lock_guard<std::mutex> lock(g_metrics.mtx);
        ++g_metrics.oom_stop;
        ESP_LOGE(TAG, "[oom] contiguous internal RAM below budget at frame %d; "
                      "stopping media instead of risking an allocation abort", frame_no);
        g_stop.store(true);
        return false;
    }
    const int in_samples = kFixtureRate * kFrameMs / 1000;
    const int in_bytes = in_samples * (int)sizeof(int16_t);

    // File-scope buffer, already sized: no allocation and no static-init guard
    // on the per-frame path.
    if ((int)g_pad.size() < in_samples) {
        g_pad.assign(in_samples, 0);
    }

    const int16_t* src = nullptr;
    if (pcm != nullptr && off + (size_t)in_bytes <= pcm_bytes) {
        src = reinterpret_cast<const int16_t*>(pcm + off);
    } else if (pcm != nullptr && off < pcm_bytes) {
        memcpy(g_pad.data(), pcm + off, pcm_bytes - off);
        src = g_pad.data();
    } else {
        src = g_pad.data();  // silence
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
            const bool sent = g_transport.SendAudio(out, out_size, g_clock.pts_ms);
            std::lock_guard<std::mutex> lock(g_metrics.mtx);
            if (sent) {
                ++g_metrics.packets;
                g_metrics.bytes += (uint64_t)out_size;
                if (out_size > g_metrics.max_payload) {
                    g_metrics.max_payload = out_size;
                }
                if (g_metrics.packets == 1) {
                    g_metrics.first_pts = g_clock.pts_ms;
                }
                g_metrics.last_pts = g_clock.pts_ms;
            } else {
                ++g_metrics.send_fail;
            }
        }
    }
    if (first_frame) {
        LogHeap("first-frame-after");
        first_frame = false;
    }
    ++frame_no;
    // One frame of media, counted whether or not the send succeeded: the media
    // timeline advances regardless.
    {
        std::lock_guard<std::mutex> lock(g_metrics.mtx);
        ++g_metrics.frames_media;
    }
    g_clock.Step();
    return true;
}

/** Stream one clip at realtime pace. Returns false if the link dropped. */
bool StreamClip(const Clip& clip)
{
    const int in_bytes = (kFixtureRate * kFrameMs / 1000) * (int)sizeof(int16_t);
    g_sender.ResetCarry();
    // The deadline is NOT re-armed here: one continuous pacing timeline covers
    // clips, silence and waits, so boundary cost and accumulated drift show up
    // instead of being reset away.
    const int64_t start_us = esp_timer_get_time();

    for (size_t off = 0; off < clip.size + (size_t)in_bytes; off += (size_t)in_bytes) {
        if (off >= clip.size && off > 0 && (clip.size % (size_t)in_bytes) == 0) {
            break;
        }
        if (!SendOneFrame(clip.data, clip.size, off)) {
            return false;
        }
    }

    // Media duration is derived from frames actually encoded, never from the
    // wall clock: reporting the same expression for both made the "media == wall"
    // check tautological.
    std::lock_guard<std::mutex> lock(g_metrics.mtx);
    g_metrics.wall_us += esp_timer_get_time() - start_us;
    return true;
}

/** Keep the media clock running by sending silence for `ms`. */
bool StreamSilence(int ms)
{
    // Same continuous timeline as StreamClip: no re-arm.
    for (int elapsed = 0; elapsed < ms; elapsed += kFrameMs) {
        if (!SendOneFrame(nullptr, 0, 0)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------- per-clip result

struct ClipResult {
    const char* id = "";
    bool speech_started = false;
    bool speech_stopped = false;
    bool completed = false;
    bool failed = false;
    std::string transcript;
    std::string item_id;
    std::string failure;
    std::string keywords_hit;
    int hits = 0;
    int total = 0;
    int64_t wait_ms = 0;
    int64_t media_start_ms = 0;
};

/**
 * @brief Wait for *this* clip's transcription while keeping media flowing.
 *
 * Attribution is by item_id recorded when the VAD/turn events arrive, not by a
 * global completion counter: a late completion from a previous clip must not be
 * credited to the next one.
 */
bool WaitForClipAsr(ClipResult& r, int timeout_ms)
{
    const int64_t t0 = esp_timer_get_time();
    int64_t next_check = t0;
    while ((esp_timer_get_time() - t0) / 1000 < timeout_ms) {
        {
            std::lock_guard<std::mutex> lock(g_obs.mtx);
            if (g_obs.turn_completed || g_obs.turn_failed) {
                r.completed = g_obs.turn_completed;
                r.failed = g_obs.turn_failed;
                r.transcript = g_obs.turn_transcript;
                r.item_id = g_obs.turn_item_id;
                r.failure = g_obs.turn_failure;
                r.speech_started = g_obs.turn_speech_started;
                r.speech_stopped = g_obs.turn_speech_stopped;
                g_obs.turn_completed = false;
                g_obs.turn_failed = false;
                r.wait_ms = (esp_timer_get_time() - t0) / 1000;
                return true;
            }
        }
        // Keep sending silence so the media clock and the server's view of
        // time both keep advancing during the wait.
        if (!SendOneFrame(nullptr, 0, 0)) {
            r.wait_ms = (esp_timer_get_time() - t0) / 1000;
            return false;
        }
        (void)next_check;
    }
    r.wait_ms = (esp_timer_get_time() - t0) / 1000;
    return false;
}

/** Score the transcript against the clip's frozen keywords. */
void ScoreKeywords(ClipResult& r, const Clip& clip)
{
    r.total = clip.keyword_count;
    for (int i = 0; i < clip.keyword_count; ++i) {
        if (r.transcript.find(clip.keywords[i]) != std::string::npos) {
            ++r.hits;
            if (!r.keywords_hit.empty()) {
                r.keywords_hit += ",";
            }
            r.keywords_hit += clip.keywords[i];
        }
    }
}

// ---------------------------------------------------------------- resources

struct ResourceSample {
    int64_t t_ms;
    size_t int_free, int_min, int_largest;
    size_t int8_free, int8_min;
    size_t dma_free, dma_min, dma_largest;
    size_t psram_free, psram_min, psram_largest;
    UBaseType_t sender_stack_free;
};

void SampleResources(std::vector<ResourceSample>& out)
{
    ResourceSample s = {};
    s.t_ms = esp_timer_get_time() / 1000;
    s.int_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s.int_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
    s.int_largest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL);
    s.int8_free = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s.int8_min = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s.dma_free = heap_caps_get_free_size(MALLOC_CAP_DMA);
    s.dma_min = heap_caps_get_minimum_free_size(MALLOC_CAP_DMA);
    s.dma_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DMA);
    s.psram_free = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    s.psram_min = heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM);
    s.psram_largest = heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM);
    // Called from the sender task, so nullptr resolves to its real handle.
    // ESP-IDF returns this in BYTES.
    s.sender_stack_free = uxTaskGetStackHighWaterMark(nullptr);
    out.push_back(s);

    ESP_LOGI(TAG,
             "[res] t=%ds | int free=%u min=%u largest=%u | int8 free=%u min=%u | "
             "dma free=%u min=%u largest=%u | psram free=%u min=%u largest=%u | "
             "sender stack free=%u B",
             (int)(s.t_ms / 1000),
             (unsigned)s.int_free, (unsigned)s.int_min, (unsigned)s.int_largest,
             (unsigned)s.int8_free, (unsigned)s.int8_min,
             (unsigned)s.dma_free, (unsigned)s.dma_min, (unsigned)s.dma_largest,
             (unsigned)s.psram_free, (unsigned)s.psram_min, (unsigned)s.psram_largest,
             (unsigned)s.sender_stack_free);
}

/** Percentile of a latency sample set (microseconds). */
int32_t Percentile(std::vector<int32_t> v, double p)
{
    if (v.empty()) {
        return -1;
    }
    std::sort(v.begin(), v.end());
    const size_t i = (size_t)(p * (double)(v.size() - 1) + 0.5);
    return v[i];
}

}  // namespace

// ------------------------------------------------ cross-task state (sender)

std::vector<ClipResult> g_results;
std::vector<ResourceSample> g_samples;
int64_t g_run_start_us = 0;
TaskHandle_t g_sender_task = nullptr;
SemaphoreHandle_t g_sender_done = nullptr;
UBaseType_t g_sender_stack_free = 0;
bool g_loop_complete = true;
bool g_acceptance_incomplete = false;

void sender_task(void*)
{
    // R2-4 first observation, on the task that owns the codec stack. Running it
    // on the main task overflowed main's 8 KB stack: the codec needs this task.
    {
        ESP_LOGW(TAG, "[diag] short send-path observation starting (2 s of frames)");
        g_clock.Arm();
        for (int i = 0; i < 100; ++i) {   // 100 x 20 ms
            if (!SendOneFrame(nullptr, 0, 0)) {
                ESP_LOGW(TAG, "[diag] link dropped after %d frames", i);
                break;
            }
        }
        const rtp_probe::Report r = rtp_probe::Snapshot();
        ESP_LOGW(TAG, "---------- SEND PATH OBSERVATION (short diag) ----------");
        ESP_LOGW(TAG, "  %s", rtp_probe::Format(r).c_str());
        if (r.protect_calls == 0) {
            ESP_LOGE(TAG, "  media never reached the SRTP boundary");
        } else if (r.udp_attempts == 0) {
            ESP_LOGE(TAG, "  SRTP ran but no matching UDP write was seen");
        } else if (r.write_failed > 0) {
            ESP_LOGE(TAG, "  %u UDP write attempts failed (last rc=%d errno=%d)",
                     (unsigned)r.write_failed, r.last_rc, r.last_errno);
        } else {
            ESP_LOGW(TAG, "  %u packets fully written in %u UDP attempts",
                     (unsigned)r.packets_written, (unsigned)r.udp_attempts);
        }
        ESP_LOGW(TAG, "-------------------------------------------------------");
    }

    // Three clips, each attributed to its own turn.
    for (int i = 0; i < kClipCount && !g_stop.load(); ++i) {
        ClipResult r;
        r.id = g_clips[i].id;
        r.media_start_ms = g_clock.pts_ms;
        {
            std::lock_guard<std::mutex> lock(g_obs.mtx);
            g_obs.turn_completed = false;
            g_obs.turn_failed = false;
            g_obs.turn_transcript.clear();
            g_obs.turn_item_id.clear();
            g_obs.turn_failure.clear();
            g_obs.turn_speech_started = false;
            g_obs.turn_speech_stopped = false;
        }

        ESP_LOGW(TAG, "[play] %s \"%s\" at media t=%u ms", r.id, g_clips[i].text,
                 (unsigned)g_clock.pts_ms);
        if (!StreamClip(g_clips[i])) {
            ESP_LOGW(TAG, "[play] link dropped during %s", r.id);
            break;
        }
        if (!StreamSilence(kGapMs)) {
            break;
        }
        const bool got = WaitForClipAsr(r, kAsrTimeoutMs);
        if (!got) {
            ESP_LOGW(TAG, "[asr] no result for %s within %d ms", r.id, kAsrTimeoutMs);
        }
        ScoreKeywords(r, g_clips[i]);
        ESP_LOGW(TAG, "[clip] %s media=%u..%u ms wait=%d ms vad=%d/%d completed=%d "
                      "failed=%d kw=%d/%d hit=[%s] transcript=\"%s\"",
                 r.id, (unsigned)r.media_start_ms, (unsigned)g_clock.pts_ms,
                 (int)r.wait_ms, (int)r.speech_started, (int)r.speech_stopped,
                 (int)r.completed, (int)r.failed, r.hits, r.total,
                 r.keywords_hit.c_str(), r.transcript.c_str());
        g_results.push_back(r);

        // Minimum safe attribution: a clip that timed out OR failed ends the
        // acceptance sequence, because its late result would otherwise land in
        // the next clip's slot and be scored as that clip's transcript.
        if (!got || r.failed) {
            ESP_LOGW(TAG, "[asr] stopping the acceptance sequence after %s: a late "
                          "result must not be credited to a later clip", r.id);
            g_acceptance_incomplete = true;
            break;
        }
    }

    // Sustained loop for resource evidence.
    const int64_t loop_end =
        esp_timer_get_time() + (int64_t)CONFIG_STACKCHAN_WEBRTC_M1_LOOP_SECONDS * 1000000;
    int64_t next_sample = esp_timer_get_time() + (int64_t)kSamplePeriodMs * 1000;
    SampleResources(g_samples);

    int rr = 0;
    while (esp_timer_get_time() < loop_end) {
        if (g_stop.load()) {
            g_loop_complete = false;
            break;
        }
        if (!StreamClip(g_clips[rr])) {
            g_loop_complete = false;
            break;
        }
        if (!StreamSilence(kGapMs)) {
            g_loop_complete = false;
            break;
        }
        {
            std::lock_guard<std::mutex> lock(g_metrics.mtx);
            ++g_metrics.clips_played;
        }
        rr = (rr + 1) % kClipCount;

        if (esp_timer_get_time() >= next_sample) {
            SampleResources(g_samples);
            next_sample += (int64_t)kSamplePeriodMs * 1000;
        }
    }

    // Measured headroom for the task that actually runs the codec. The API
    // reports BYTES on ESP-IDF, not words.
    g_sender_stack_free = uxTaskGetStackHighWaterMark(nullptr);
    ESP_LOGW(TAG, "[stack] sender high-water mark: %u bytes free of %d allocated",
             (unsigned)g_sender_stack_free, kSenderStackBytes);
    LogHeap("sender-exit");

    if (g_sender_done != nullptr) {
        xSemaphoreGive(g_sender_done);
    }
    vTaskDelete(nullptr);
}

// ============================================================== entry point

void WebRtcM1Run()
{
    std::vector<ResourceSample> samples;

    ESP_LOGW(TAG, "M1 uplink probe: fixture -> 16->48k -> Opus 48k mono %d ms -> RTP", kFrameMs);
    ESP_LOGW(TAG, "payload budget %d B (library whole-packet buffer 1428 B, minus header/extension)",
             g_transport.MaxPayloadBytes());

    {
        const uint8_t* starts[] = {zh_1_pcm, zh_2_pcm, zh_3_pcm};
        const uint32_t sizes[] = {zh_1_pcm_length, zh_2_pcm_length, zh_3_pcm_length};
        for (int i = 0; i < kClipCount; ++i) {
            g_clips[i].data = starts[i];
            g_clips[i].size = (size_t)sizes[i];
            const int n = (int)(g_clips[i].size / sizeof(int16_t));
            ESP_LOGI(TAG, "[fx] %s \"%s\": %u B (%d samples, %d ms) kw=%s",
                     g_clips[i].id, g_clips[i].text, (unsigned)g_clips[i].size, n,
                     (int)((int64_t)n * 1000 / kFixtureRate), g_clips[i].keywords[0]);
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
        HandleServerEvent(json, stream_id);   // parse + book-keep only
    };
    cb.on_state = [](esp_peer_state_t s) {
        switch (s) {
            case ESP_PEER_STATE_DISCONNECTED:
            case ESP_PEER_STATE_CLOSED:
            case ESP_PEER_STATE_CONNECT_FAILED:
            case ESP_PEER_STATE_DATA_CHANNEL_CLOSED:
            case ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED:
                ESP_LOGW(TAG, "[state] link down (%d); stopping media", (int)s);
                g_stop.store(true);
                break;
            default:
                break;
        }
    };
    cb.on_channel_open = [](esp_peer_data_channel_info_t*) {};
    cb.on_audio = [](const uint8_t*, int, uint32_t) {
        static std::atomic<int> n{0};
        ++n;   // downlink is out of scope; just count, never block
    };
    cb.on_before_close = []() {
        g_stop.store(true);
        vTaskDelay(pdMS_TO_TICKS(200));
    };

    if (!g_transport.Start(tcfg, cb, &err)) {
        ESP_LOGE(TAG, "VERDICT: FAIL - transport: %s", err.c_str());
        g_sender.Close();
        return;
    }

    // Configuration gate: the *echo must verify*, not merely arrive.
    int waited = 0;
    bool gate_ok = false;
    std::string gate_detail, gate_failure;
    while (waited < kSessionTimeoutMs) {
        {
            std::lock_guard<std::mutex> lock(g_obs.mtx);
            if (g_obs.session_updated) {
                gate_ok = g_obs.config_ok;
                gate_detail = g_obs.config_echo;
                gate_failure = g_obs.config_failure;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(200));
        waited += 200;
    }
    if (!gate_ok) {
        bool created = false, sent = false;
        {
            std::lock_guard<std::mutex> lock(g_obs.mtx);
            created = g_obs.session_created;
            sent = g_obs.update_sent;
        }
        // Name the stage that actually failed. A handshake that never reached
        // CONNECTED is a transport failure, not a configuration one.
        const int stage = g_transport.stage();
        const esp_peer_state_t pst = g_transport.state();
        const char* stage_name = "unknown";
        const char* blame = "transport";
        switch (stage) {
            case 0: stage_name = "not started"; break;
            case 1: stage_name = "local SDP gathered"; break;
            case 2: stage_name = "SDP exchange failed"; break;
            case 3: stage_name = "SDP gathering timed out"; break;
            case 4: stage_name = "server answer received"; break;
            case 5: stage_name = "ICE/DTLS connected"; break;
            case 6: stage_name = "DataChannel opened"; break;
            default: break;
        }
        if (stage >= 6) {
            blame = "configuration";
        }

        ESP_LOGE(TAG,
                 "VERDICT: FAIL - %s layer. stage=%s(%d) peer_state=%d "
                 "session_created=%d update_sent=%d",
                 blame, stage_name, stage, (int)pst, (int)created, (int)sent);
        if (blame[0] == 'c') {
            ESP_LOGE(TAG, "  echo=%s reason=%s", gate_detail.c_str(),
                     gate_failure.empty() ? "session.updated not received in time"
                                          : gate_failure.c_str());
        } else {
            ESP_LOGE(TAG, "  the session handshake never completed, so no "
                          "configuration could be verified");
        }
        ESP_LOGE(TAG, "The fixture was NOT sent.");
        // Release g_obs.mtx before Stop(): a callback waiting on that lock would
        // otherwise deadlock the join.
        g_transport.Stop();
        g_sender.Close();
        return;
    }
    ESP_LOGW(TAG, "[cfg] gate passed: %s", gate_detail.c_str());

    // R2-4: fresh observation generation before any media is sent.
    rtp_probe::Arm();

    // ---- media runs on its own task ----
    g_sender_done = xSemaphoreCreateBinary();
    if (g_sender_done == nullptr) {
        ESP_LOGE(TAG, "VERDICT: FAIL - could not create the sender join semaphore");
        g_transport.Stop();
        g_sender.Close();
        return;
    }
    g_run_start_us = esp_timer_get_time();
    if (xTaskCreate(sender_task, "m1_sender", kSenderStackBytes, nullptr, 5,
                    &g_sender_task) != pdPASS) {
        ESP_LOGE(TAG, "VERDICT: FAIL - could not create the media sender task");
        g_transport.Stop();
        g_sender.Close();
        return;
    }
    // Join: the main task must not summarize or close the transport while the
    // sender is still encoding or sending.
    xSemaphoreTake(g_sender_done, portMAX_DELAY);
    g_sender_task = nullptr;

    // ---- summary ----
    const int64_t loop_elapsed_ms = (esp_timer_get_time() - g_run_start_us) / 1000;
    {
        std::lock_guard<std::mutex> mlock(g_metrics.mtx);
        std::lock_guard<std::mutex> olock(g_obs.mtx);
        ESP_LOGW(TAG, "==================== M1 SUMMARY ====================");
        ESP_LOGW(TAG, "  frame duration     : %d ms", kFrameMs);
        ESP_LOGW(TAG, "  sender stack high-water: %u bytes free of %d allocated",
                 (unsigned)g_sender_stack_free, kSenderStackBytes);
        ESP_LOGW(TAG, "  config echo        : %s", g_obs.config_echo.c_str());
        ESP_LOGW(TAG, "  clips played       : %u", g_metrics.clips_played);
        ESP_LOGW(TAG, "  opus packets       : %u", g_metrics.packets);
        ESP_LOGW(TAG, "  total payload      : %u B", (unsigned)g_metrics.bytes);
        ESP_LOGW(TAG, "  max payload        : %d B (budget %d B)",
                 g_metrics.max_payload, g_transport.MaxPayloadBytes());
        ESP_LOGW(TAG, "  media clock range  : %u .. %u ms", g_metrics.first_pts, g_metrics.last_pts);
        const int64_t media_ms = (int64_t)g_metrics.frames_media * kFrameMs;
        ESP_LOGW(TAG, "  media/wall elapsed : %d / %d ms  (media derived from %u frames)",
                 (int)media_ms, (int)(g_metrics.wall_us / 1000),
                 (unsigned)g_metrics.frames_media);
        ESP_LOGW(TAG, "  pacing late        : %u", g_metrics.pacing_late);
        ESP_LOGW(TAG, "  oversize dropped   : %u", g_metrics.oversize);
        ESP_LOGW(TAG, "  oom stops          : %u", g_metrics.oom_stop);
        ESP_LOGW(TAG, "  send/encode/resample failures: %u / %u / %u",
                 g_metrics.send_fail, g_sender.encode_fail(), g_sender.resample_fail());
        ESP_LOGW(TAG, "  sustained loop     : %s, wall %d ms (budget %d ms)",
                 g_loop_complete ? "COMPLETE" : "INTERRUPTED",
                 (int)loop_elapsed_ms, CONFIG_STACKCHAN_WEBRTC_M1_LOOP_SECONDS * 1000);
        ESP_LOGW(TAG, "  ---- four-layer evidence ----");
        ESP_LOGW(TAG, "  [cfg] created=%d updated=%d verified=%d",
                 (int)g_obs.session_created, (int)g_obs.session_updated, (int)g_obs.config_ok);
        ESP_LOGW(TAG, "  [vad] started=%d stopped=%d", g_obs.speech_started, g_obs.speech_stopped);
        ESP_LOGW(TAG, "  [asr] completed=%d failed=%d server_errors=%d",
                 g_obs.asr_completed, g_obs.asr_failed, g_obs.server_errors);
        for (const auto& r : g_results) {
            ESP_LOGW(TAG, "  [clip] %s kw=%d/%d completed=%d failed=%d wait=%d ms transcript=\"%s\"",
                     r.id, r.hits, r.total, (int)r.completed, (int)r.failed,
                     (int)r.wait_ms, r.transcript.c_str());
        }
        if (!g_obs.last_error.empty()) {
            ESP_LOGW(TAG, "  last error         : %s", g_obs.last_error.c_str());
        }
        ESP_LOGW(TAG, "====================================================");
    }

    {
        auto rs = g_sender.resample_us().Copy();
        auto en = g_sender.encode_us().Copy();
        // Sampling rate and window are reported explicitly: the percentiles
        // describe 1-in-kStride of all calls, not every call.
        constexpr uint64_t kStride = 4;
        ESP_LOGW(TAG, "[lat] uniform 1-in-%d sampling; resample n=%d p50=%d p95=%d p99=%d us | "
                      "encode n=%d p50=%d p95=%d p99=%d us",
                 (int)kStride,
                 (int)rs.size(),
                 Percentile(rs, 0.50), Percentile(rs, 0.95), Percentile(rs, 0.99),
                 (int)en.size(),
                 Percentile(en, 0.50), Percentile(en, 0.95), Percentile(en, 0.99));
        if (g_samples.size() >= 2) {
            const auto& a = g_samples.front();
            const auto& b = g_samples.back();
            ESP_LOGW(TAG, "[res] trend int free %u -> %u, int min %u -> %u, psram free %u -> %u",
                     (unsigned)a.int_free, (unsigned)b.int_free,
                     (unsigned)a.int_min, (unsigned)b.int_min,
                     (unsigned)a.psram_free, (unsigned)b.psram_free);
        }
    }

    // ---- M1-5 diagnostics: is the uplink real audio? ----
    {
        std::lock_guard<std::mutex> dlock(g_diag.mtx);
        const double rs_avg = g_diag.resample_frames
                                  ? g_diag.resample_rms_sum / g_diag.resample_frames
                                  : 0.0;
        const double rt_avg = g_diag.opus_rt_frames
                                  ? g_diag.opus_rt_rms_sum / g_diag.opus_rt_frames
                                  : 0.0;
        ESP_LOGW(TAG, "-------------- UPLINK DIAGNOSTICS --------------");
        ESP_LOGW(TAG, "  resampler 48k RMS  : peak=%.4f avg=%.4f over %u frames",
                 g_diag.resample_rms_peak, rs_avg, (unsigned)g_diag.resample_frames);
        ESP_LOGW(TAG, "  local Opus decode  : peak=%.4f avg=%.4f over %u ok, %u failed",
                 g_diag.opus_rt_rms_peak, rt_avg,
                 (unsigned)g_diag.opus_rt_frames, (unsigned)g_diag.opus_rt_fail);
        ESP_LOGW(TAG, "  packet size        : min=%d max=%d B",
                 g_diag.pkt_min == (1 << 30) ? -1 : g_diag.pkt_min, g_diag.pkt_max);
        ESP_LOGW(TAG, "  interpretation     : resampler RMS ~0 means silence was "
                       "produced; decode failures mean the payload is not valid Opus");
        ESP_LOGW(TAG, "------------------------------------------------");
    }

    // ---- R2-4: what actually crossed SRTP and the UDP socket ----
    {
        const rtp_probe::Report r = rtp_probe::Snapshot();
        const std::string line = rtp_probe::Format(r);
        ESP_LOGW(TAG, "---------- SEND PATH OBSERVATION (full run) ----------");
        ESP_LOGW(TAG, "  %s", line.c_str());
        if (r.protect_calls == 0) {
            ESP_LOGE(TAG, "  no srtp_protect call was observed: media never reached "
                          "the SRTP boundary");
        } else if (r.udp_attempts == 0) {
            ESP_LOGE(TAG, "  SRTP ran but no matching UDP write was observed");
        } else if (r.write_failed > 0) {
            ESP_LOGE(TAG, "  %u UDP write attempts failed (last rc=%d errno=%d)",
                     (unsigned)r.write_failed, r.last_rc, r.last_errno);
        } else {
            ESP_LOGW(TAG, "  %u packets fully written in %u UDP attempts",
                     (unsigned)r.packets_written, (unsigned)r.udp_attempts);
        }
        if (r.overflow_pending > 0 || r.retired_pending > 0) {
            ESP_LOGW(TAG, "  observation coverage incomplete: %u pending recycled, "
                          "%u pending retired", (unsigned)r.overflow_pending,
                     (unsigned)r.retired_pending);
        }
        ESP_LOGW(TAG, "-------------------------------------------");
    }

    // ---- R2-5: VERDICT computed from the real acceptance conditions ----
    {
        const rtp_probe::Report sp = rtp_probe::Snapshot();
        // R3-2: a clip passes only when its turn was bracketed by real VAD and
        // its own completed transcription matched every keyword. Three matching
        // transcripts without VAD are not the four-layer acceptance.
        int clips_ok = 0;
        int clips_vad_ok = 0;
        bool any_failed = false;
        for (const auto& r : g_results) {
            const bool vad_ok = r.speech_started && r.speech_stopped;
            if (vad_ok) {
                ++clips_vad_ok;
            }
            if (vad_ok && r.completed && !r.failed && r.hits == r.total && r.total > 0) {
                ++clips_ok;
            }
            if (r.failed) {
                any_failed = true;
            }
        }
        const bool cfg_ok = g_obs.config_ok;
        const bool three_ok = (clips_ok == kClipCount);
        const bool no_codec_err = (g_sender.encode_fail() == 0 &&
                                   g_sender.resample_fail() == 0 &&
                                   g_metrics.oversize == 0);
        // R3-1: a send may only count as verified when protect succeeded, the
        // successful writes actually correspond to protected packets with the
        // expected length, nothing was short-written or lost to observation,
        // and every protected packet was seen to reach the socket.
        const bool send_ok = (sp.protect_calls > 0 &&
                              sp.protect_fail == 0 &&
                              sp.packets_written > 0 &&
                              sp.write_failed == 0 &&
                              sp.write_incomplete == 0 &&
                              sp.length_mismatch == 0 &&
                              sp.protected_unwritten == 0 &&
                              sp.retired_pending == 0 &&
                              sp.overflow_pending == 0);

        ESP_LOGW(TAG, "==================== FINAL VERDICT ====================");
        ESP_LOGW(TAG, "  config verified        : %s", cfg_ok ? "yes" : "NO");
        ESP_LOGW(TAG, "  3 clips with keywords  : %d/%d", clips_ok, kClipCount);
        ESP_LOGW(TAG, "  3 clips with valid VAD : %d/%d", clips_vad_ok, kClipCount);
        ESP_LOGW(TAG, "  any clip failed        : %s", any_failed ? "yes" : "no");
        ESP_LOGW(TAG, "  zero codec/oversize    : %s", no_codec_err ? "yes" : "NO");
        ESP_LOGW(TAG, "  send path observed     : %s (protect=%u udp=%u failed=%u)",
                 send_ok ? "yes" : "NO", (unsigned)sp.protect_calls,
                 (unsigned)sp.udp_attempts, (unsigned)sp.write_failed);
        ESP_LOGW(TAG, "  sustained loop         : %s",
                 g_loop_complete ? "COMPLETE" : "INTERRUPTED");
        ESP_LOGW(TAG, "  acceptance incomplete  : %s",
                 g_acceptance_incomplete ? "yes" : "no");

        // A pass requires every acceptance condition, not merely a completed
        // capture. Diagnostics that ran are not acceptance.
        if (cfg_ok && three_ok && no_codec_err && send_ok && g_loop_complete &&
            !any_failed) {
            ESP_LOGW(TAG, "  VERDICT: PASS");
        } else {
            ESP_LOGE(TAG, "  VERDICT: FAIL");
            if (!cfg_ok) {
                ESP_LOGE(TAG, "    - configuration was not verified");
            }
            if (!three_ok) {
                ESP_LOGE(TAG, "    - only %d/%d clips had valid VAD start+stop plus a "
                              "complete keyword-matching transcription",
                         clips_ok, kClipCount);
                if (clips_vad_ok < kClipCount) {
                    ESP_LOGE(TAG, "      VAD bracketed only %d/%d clips", clips_vad_ok,
                             kClipCount);
                }
            }
            if (!no_codec_err) {
                ESP_LOGE(TAG, "    - codec/oversize errors present");
            }
            if (!send_ok) {
                ESP_LOGE(TAG, "    - send path not fully verified: protect ok=%u fail=%u, "
                          "packets_written=%u, short=%u, failed=%u, len_mismatch=%u, "
                          "protected_unwritten=%u, lost=%u",
                     (unsigned)sp.protect_ok, (unsigned)sp.protect_fail,
                     (unsigned)sp.packets_written, (unsigned)sp.write_incomplete,
                     (unsigned)sp.write_failed, (unsigned)sp.length_mismatch,
                     (unsigned)sp.protected_unwritten,
                     (unsigned)(sp.retired_pending + sp.overflow_pending));
            }
            if (!g_loop_complete) {
                ESP_LOGE(TAG, "    - the sustained loop did not complete");
            }
            ESP_LOGE(TAG, "    (a completed capture or a passing diagnostic is not a pass)");
        }
        ESP_LOGW(TAG, "=======================================================");
    }

    g_transport.Stop();
    g_sender.Close();
    ESP_LOGW(TAG, "M1 probe finished; idling");
}

#endif  // CONFIG_STACKCHAN_WEBRTC_M1
