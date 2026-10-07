#include <sdkconfig.h>

// PROBE_GUARD: Shared transport for the probes. Built only when a probe is enabled.
#if CONFIG_STACKCHAN_WEBRTC_M0 || CONFIG_STACKCHAN_WEBRTC_M1

#include "webrtc_transport.h"

#include <algorithm>
#include <cstring>
#include <mutex>

#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "esp_peer_default.h"

#define TAG "WebRTC-TP"

namespace webrtc_transport {
namespace {

constexpr const char* kWebRtcHostFmt =
    "https://%s.cn-beijing.maas.aliyuncs.com/api/v1/webrtc/realtime";

/** How long Start() waits for the local SDP before giving up. */
constexpr int kSdpWaitMs = 30000;

/**
 * @brief Single-packet payload budget.
 *
 * The library's RTP encoder owns a 1428-byte whole-packet buffer and memcpy's
 * the payload to `buf + 12 + ext_reserve` with no length check of its own
 * (findings A9). 1428 is therefore the *whole packet*, not the payload
 * capacity: the RTP header and any extension reservation have to come out of
 * it first. The extension allowance is deliberately generous because the
 * library does not expose its reserve value.
 */
constexpr int kRtpWholePacketBuffer = 1428;
constexpr int kRtpHeaderBytes = 12;
constexpr int kRtpExtensionReserve = 64;

}  // namespace

int Transport::MaxPayloadBytes()
{
    return kRtpWholePacketBuffer - kRtpHeaderBytes - kRtpExtensionReserve;
}

struct Transport::Impl {
    esp_peer_handle_t peer = nullptr;
    Config cfg;
    Callbacks cb;

    // Tasks and lifecycle.
    TaskHandle_t loop_task = nullptr;
    TaskHandle_t signal_task = nullptr;
    SemaphoreHandle_t sdp_ready = nullptr;
    bool stopping = false;

    std::string local_sdp;

    // Guarded by mtx.
    mutable std::mutex mtx;
    esp_peer_state_t peer_state = ESP_PEER_STATE_CLOSED;
    bool audio_ready = false;   // data channel + transport up, media may flow
    bool channel_requested = false;
};

namespace {

Transport::Impl* impl_of(void* ctx)
{
    return static_cast<Transport::Impl*>(ctx);
}

const char* state_name(esp_peer_state_t s)
{
    switch (s) {
        case ESP_PEER_STATE_CLOSED: return "CLOSED";
        case ESP_PEER_STATE_DISCONNECTED: return "DISCONNECTED";
        case ESP_PEER_STATE_NEW_CONNECTION: return "NEW_CONNECTION";
        case ESP_PEER_STATE_CANDIDATE_GATHERING: return "CANDIDATE_GATHERING";
        case ESP_PEER_STATE_PAIRING: return "PAIRING";
        case ESP_PEER_STATE_PAIRED: return "PAIRED";
        case ESP_PEER_STATE_CONNECTING: return "CONNECTING";
        case ESP_PEER_STATE_CONNECTED: return "CONNECTED";
        case ESP_PEER_STATE_CONNECT_FAILED: return "CONNECT_FAILED";
        case ESP_PEER_STATE_DATA_CHANNEL_CONNECTED: return "DATA_CHANNEL_CONNECTED";
        case ESP_PEER_STATE_DATA_CHANNEL_OPENED: return "DATA_CHANNEL_OPENED";
        case ESP_PEER_STATE_DATA_CHANNEL_CLOSED: return "DATA_CHANNEL_CLOSED";
        case ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED: return "DATA_CHANNEL_DISCONNECTED";
        case ESP_PEER_STATE_REMOTE_AUDIO_TRACK_ADDED: return "REMOTE_AUDIO_TRACK_ADDED";
        default: return "?";
    }
}

/** Collect the HTTP response body during perform(). */
esp_err_t http_event_handler(esp_http_client_event_t* evt)
{
    if (evt->event_id == HTTP_EVENT_ON_DATA && evt->user_data != nullptr) {
        static_cast<std::string*>(evt->user_data)->append(
            static_cast<const char*>(evt->data), evt->data_len);
    }
    return ESP_OK;
}

bool post_sdp(const std::string& offer, std::string& answer, std::string* err)
{
    char host[160];
    snprintf(host, sizeof(host), kWebRtcHostFmt, CONFIG_STACKCHAN_ALIYUN_WORKSPACE_ID);
    char url[320];
    snprintf(url, sizeof(url), "%s?model=%s", host, CONFIG_STACKCHAN_ALIYUN_MODEL);

    // Deliberately not logging the URL's workspace id or any header value.
    ESP_LOGI(TAG, "POST realtime WebRTC SDP endpoint");
    ESP_LOGI(TAG, "offer is %d bytes", (int)offer.size());

    answer.clear();
    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 20000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 4096;
    cfg.event_handler = http_event_handler;
    cfg.user_data = &answer;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        *err = "http client init failed";
        return false;
    }

    char auth[512];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_STACKCHAN_ALIYUN_API_KEY);
    esp_http_client_set_header(client, "Content-Type", "application/sdp");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, offer.c_str(), (int)offer.size());

    const esp_err_t rc = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "SDP exchange: err=%s http=%d", esp_err_to_name(rc), status);

    const bool ok = (rc == ESP_OK && status == 200 && !answer.empty());
    ESP_LOGI(TAG, "response body is %d bytes", (int)answer.size());
    if (!ok) {
        *err = "SDP exchange failed (http ";
        *err += std::to_string(status);
        *err += ")";
        if (!answer.empty()) {
            ESP_LOGE(TAG, "server said: %s", answer.c_str());
        }
    }

    esp_http_client_cleanup(client);
    return ok;
}

// ------------------------------------------------------------- callbacks

int cb_state(esp_peer_state_t state, void* ctx)
{
    auto* p = impl_of(ctx);
    ESP_LOGI(TAG, "[state] %s (%d)", state_name(state), (int)state);
    {
        std::lock_guard<std::mutex> lock(p->mtx);
        p->peer_state = state;
        switch (state) {
            case ESP_PEER_STATE_CONNECTED:
                // Transport is up; the audio channel is not necessarily open.
                break;
            case ESP_PEER_STATE_DATA_CHANNEL_OPENED:
                p->audio_ready = true;
                break;
            case ESP_PEER_STATE_CONNECT_FAILED:
            case ESP_PEER_STATE_DISCONNECTED:
            case ESP_PEER_STATE_CLOSED:
            case ESP_PEER_STATE_DATA_CHANNEL_CLOSED:
            case ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED:
                p->audio_ready = false;
                break;
            default:
                break;
        }
    }

    if (state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED && !p->channel_requested) {
        p->channel_requested = true;
        esp_peer_data_channel_cfg_t ch = {};
        ch.type = ESP_PEER_DATA_CHANNEL_RELIABLE;
        ch.ordered = true;
        ch.label = const_cast<char*>(p->cfg.data_channel_label);
        const int ret = esp_peer_create_data_channel(p->peer, &ch);
        ESP_LOGI(TAG, "create data channel '%s' -> %d", p->cfg.data_channel_label, ret);
    }

    if (p->cb.on_state) {
        p->cb.on_state(state);
    }
    return 0;
}

int cb_msg(esp_peer_msg_t* info, void* ctx)
{
    auto* p = impl_of(ctx);
    if (info == nullptr || info->data == nullptr) {
        return 0;
    }
    if (info->type == ESP_PEER_MSG_TYPE_SDP) {
        p->local_sdp.assign((const char*)info->data, info->size);
        const bool has_candidate = p->local_sdp.find("a=candidate:") != std::string::npos;
        ESP_LOGI(TAG, "[msg] local SDP, %d bytes, candidates=%s",
                 info->size, has_candidate ? "YES" : "NO");
        if (p->sdp_ready) {
            xSemaphoreGive(p->sdp_ready);
        }
    } else if (info->type == ESP_PEER_MSG_TYPE_CANDIDATE) {
        ESP_LOGI(TAG, "[msg] remote candidate: %.*s", info->size, (const char*)info->data);
    }
    return 0;
}

int cb_channel_open(esp_peer_data_channel_info_t* ch, void* ctx)
{
    auto* p = impl_of(ctx);
    ESP_LOGI(TAG, "[channel open] label='%s' stream_id=%u",
             (ch && ch->label) ? ch->label : "?", (unsigned)(ch ? ch->stream_id : 0));
    if (p->cb.on_channel_open) {
        p->cb.on_channel_open(ch);
    }
    return 0;
}

int cb_data(esp_peer_data_frame_t* frame, void* ctx)
{
    auto* p = impl_of(ctx);
    if (frame == nullptr || frame->data == nullptr) {
        return 0;
    }
    if (p->cb.on_data) {
        // Copy into a std::string: the caller keeps it beyond the callback.
        p->cb.on_data(std::string((const char*)frame->data, frame->size), frame->stream_id);
    }
    return 0;
}

int cb_audio(esp_peer_audio_frame_t* frame, void* ctx)
{
    auto* p = impl_of(ctx);
    if (frame == nullptr || frame->data == nullptr) {
        return 0;
    }
    if (p->cb.on_audio) {
        // Borrowed pointer; documented as invalid after this returns.
        p->cb.on_audio(frame->data, frame->size, frame->pts);
    }
    return 0;
}

void loop_task(void* arg)
{
    auto* p = impl_of(arg);
    ESP_LOGI(TAG, "peer main loop started");
    // esp_peer_main_loop() is a single poll iteration, not a blocking loop.
    while (!p->stopping) {
        esp_peer_main_loop(p->peer);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "peer main loop exited");
    p->loop_task = nullptr;
    vTaskDelete(nullptr);
}

void signal_task(void* arg)
{
    auto* p = impl_of(arg);
    std::string err;

    if (xSemaphoreTake(p->sdp_ready, pdMS_TO_TICKS(kSdpWaitMs)) != pdTRUE) {
        ESP_LOGE(TAG, "no local SDP within %d ms", kSdpWaitMs);
        p->signal_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    std::string answer;
    if (!post_sdp(p->local_sdp, answer, &err)) {
        ESP_LOGE(TAG, "signaling failed: %s", err.c_str());
        p->signal_task = nullptr;
        vTaskDelete(nullptr);
        return;
    }

    esp_peer_msg_t msg = {};
    msg.type = ESP_PEER_MSG_TYPE_SDP;
    msg.data = (uint8_t*)answer.data();
    msg.size = (int)answer.size();
    const int ret = esp_peer_send_msg(p->peer, &msg);
    ESP_LOGI(TAG, "fed answer to peer -> %d", ret);

    p->signal_task = nullptr;
    vTaskDelete(nullptr);
}

}  // namespace

Transport::~Transport()
{
    Stop();
    delete impl_;
    impl_ = nullptr;
}

bool Transport::Start(const Config& cfg, const Callbacks& cb, std::string* err)
{
    if (impl_ != nullptr) {
        *err = "transport already started";
        return false;
    }
    auto* p = new Impl();
    impl_ = p;
    p->cfg = cfg;
    p->cb = cb;
    p->sdp_ready = xSemaphoreCreateBinary();
    if (p->sdp_ready == nullptr) {
        *err = "semaphore alloc failed";
        return false;
    }

    // Generating the DTLS certificate is the slow part of startup.
    const int cert_ret = esp_peer_pre_generate_cert();
    ESP_LOGI(TAG, "pre-generate DTLS cert -> %d", cert_ret);

    static esp_peer_ice_server_cfg_t ice_server = {};
    ice_server.stun_url = const_cast<char*>(cfg.stun_url);

    esp_peer_cfg_t peer_cfg = {};
    peer_cfg.server_lists = &ice_server;
    peer_cfg.server_num = 1;
    peer_cfg.role = ESP_PEER_ROLE_CONTROLLING;
    peer_cfg.ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL;
    peer_cfg.audio_info.codec = ESP_PEER_AUDIO_CODEC_OPUS;
    // RTP/SDP clock is fixed at 48 kHz for Opus regardless of the PCM rate.
    peer_cfg.audio_info.sample_rate = cfg.rtp_clock_rate;
    peer_cfg.audio_info.channel = cfg.pcm_channels;
    peer_cfg.audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV;
    peer_cfg.video_dir = ESP_PEER_MEDIA_DIR_NONE;
    peer_cfg.enable_data_channel = cfg.enable_data_channel;
    peer_cfg.manual_ch_create = true;
    peer_cfg.on_state = cb_state;
    peer_cfg.on_msg = cb_msg;
    peer_cfg.on_data = cb_data;
    peer_cfg.on_audio_data = cb_audio;
    peer_cfg.on_channel_open = cb_channel_open;
    peer_cfg.ctx = p;

    esp_peer_default_cfg_t tuning = {};
    tuning.agent_recv_timeout = 100;
    tuning.data_ch_cfg.cache_timeout = 5000;
    tuning.data_ch_cfg.send_cache_size = 16 * 1024;
    tuning.data_ch_cfg.recv_cache_size = 16 * 1024;
    tuning.rtp_cfg.audio_recv_jitter.cache_timeout = 100;
    tuning.rtp_cfg.audio_recv_jitter.resend_delay = 20;
    tuning.rtp_cfg.audio_recv_jitter.cache_size = 32 * 1024;
    tuning.rtp_cfg.send_pool_size = 32 * 1024;
    tuning.rtp_cfg.send_queue_num = 64;
    tuning.rtp_cfg.max_resend_count = 3;
    peer_cfg.extra_cfg = &tuning;
    peer_cfg.extra_size = sizeof(tuning);

    const int ret = esp_peer_open(&peer_cfg, esp_peer_get_default_impl(), &p->peer);
    ESP_LOGI(TAG, "esp_peer_open -> %d", ret);
    if (ret != 0 || p->peer == nullptr) {
        *err = "esp_peer_open failed (" + std::to_string(ret) + ")";
        return false;
    }

    if (xTaskCreate(loop_task, "m_peer_loop", 8192, p, 5, nullptr) != pdPASS) {
        *err = "peer loop task create failed";
        return false;
    }
    if (xTaskCreate(signal_task, "m_peer_sig", 8192, p, 5, nullptr) != pdPASS) {
        *err = "signaling task create failed";
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(300));
    const int conn_ret = esp_peer_new_connection(p->peer);
    ESP_LOGI(TAG, "esp_peer_new_connection -> %d", conn_ret);
    if (conn_ret != 0) {
        *err = "esp_peer_new_connection failed (" + std::to_string(conn_ret) + ")";
        return false;
    }
    return true;
}

bool Transport::SendJson(const std::string& json, uint16_t stream_id)
{
    auto* p = impl_;
    if (p == nullptr || p->peer == nullptr) {
        return false;
    }
    esp_peer_data_frame_t frame = {};
    frame.type = ESP_PEER_DATA_CHANNEL_STRING;
    frame.stream_id = stream_id;
    frame.data = (uint8_t*)json.data();
    frame.size = (int)json.size();
    return esp_peer_send_data(p->peer, &frame) == 0;
}

bool Transport::SendAudio(const uint8_t* data, int size, uint32_t pts_ms)
{
    auto* p = impl_;
    if (p == nullptr || data == nullptr || size <= 0) {
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(p->mtx);
        if (!p->audio_ready) {
            return false;
        }
    }
    // Refuse oversized packets here rather than letting the library's unchecked
    // memcpy run past its 1428-byte buffer.
    if (size > MaxPayloadBytes()) {
        return false;
    }
    esp_peer_audio_frame_t frame = {};
    frame.data = const_cast<uint8_t*>(data);
    frame.size = size;
    frame.pts = pts_ms;
    return esp_peer_send_audio(p->peer, &frame) == 0;
}

bool Transport::CanSendAudio() const
{
    auto* p = impl_;
    if (p == nullptr) {
        return false;
    }
    std::lock_guard<std::mutex> lock(p->mtx);
    return p->audio_ready;
}

esp_peer_state_t Transport::state() const
{
    auto* p = impl_;
    if (p == nullptr) {
        return ESP_PEER_STATE_CLOSED;
    }
    std::lock_guard<std::mutex> lock(p->mtx);
    return p->peer_state;
}

void Transport::Stop()
{
    auto* p = impl_;
    if (p == nullptr) {
        return;
    }

    // Let the owner stop its media sender first: esp_peer must not be closed
    // while a send is in flight.
    if (p->cb.on_before_close) {
        p->cb.on_before_close();
        p->cb.on_before_close = nullptr;
    }

    p->stopping = true;
    // The loop task clears its handle as its last act.
    for (int i = 0; i < 100 && p->loop_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    for (int i = 0; i < 100 && p->signal_task != nullptr; ++i) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }

    if (p->peer != nullptr) {
        esp_peer_close(p->peer);
        p->peer = nullptr;
    }
    if (p->sdp_ready != nullptr) {
        vSemaphoreDelete(p->sdp_ready);
        p->sdp_ready = nullptr;
    }
    std::lock_guard<std::mutex> lock(p->mtx);
    p->audio_ready = false;
}

}  // namespace webrtc_transport

#endif  // CONFIG_STACKCHAN_WEBRTC_M0 || CONFIG_STACKCHAN_WEBRTC_M1
