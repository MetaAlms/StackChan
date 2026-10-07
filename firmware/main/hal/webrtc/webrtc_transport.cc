#include <sdkconfig.h>

// PROBE_GUARD: Shared transport for the probes. Built only when a probe is enabled.
#if CONFIG_STACKCHAN_WEBRTC_M0 || CONFIG_STACKCHAN_WEBRTC_M1

#include "webrtc_transport.h"

#include <algorithm>
#include <atomic>
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
 * Furthest stage reached. Reported so a failure names the stage that actually
 * failed instead of blaming configuration for a handshake that never started.
 */
enum Stage {
    kStageStart = 0,
    kStageSdpGathered,        // local offer produced
    kStageSdpExchangeFailed,  // HTTP/SDP exchange did not succeed
    kStageSdpTimeout,
    kStageAnswerReceived,     // server answer fed to the peer
    kStageConnected,          // DTLS/ICE connected
    kStageDataChannel,        // DataChannel opened
};

/**
 * How long cleanup() waits for a task to leave.
 *
 * The signaling task can be inside an HTTP request whose own timeout is 20 s,
 * so this has to exceed that; a fixed short delay would be a guess, not a join.
 */
constexpr int kTaskExitWaitMs = 25000;

/**
 * Peer task stacks, in BYTES.
 *
 * ESP-IDF's xTaskCreate takes bytes, unlike vanilla FreeRTOS. The signaling
 * task performs TLS/HTTP, so it needs more than a nominal RTOS task.
 */
constexpr int kPeerTaskStackBytes = 8192;

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

    // Tasks and lifecycle. The exit semaphores are what make Stop() a real
    // join: a delay is not a join, and esp_peer must not be closed while a task
    // is still inside a poll or an HTTP request.
    TaskHandle_t loop_task = nullptr;
    TaskHandle_t sig_task = nullptr;
    SemaphoreHandle_t loop_exited = nullptr;
    SemaphoreHandle_t sig_exited = nullptr;
    SemaphoreHandle_t sdp_ready = nullptr;
    std::atomic<bool> stopping{false};
    // Only a task that was actually created and has confirmed exit may have its
    // resources released; anything else means we would free memory still in use.
    std::atomic<bool> loop_created{false};
    std::atomic<bool> sig_created{false};
    std::atomic<bool> loop_confirmed_exit{false};
    std::atomic<bool> sig_confirmed_exit{false};
    std::atomic<bool> cleaned{false};
    /**
     * Set when a task could not confirm exit. The context, the peer and the
     * semaphores are then deliberately never freed: the tasks may still be
     * dereferencing them, and a leak is strictly safer than a use-after-free.
     */
    std::atomic<bool> leaked{false};
    /** Furthest stage reached, so a failure can name the real stage (M1-6). */
    std::atomic<int> stage{0};

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

/**
 * @brief Log the media-relevant SDP lines only.
 *
 * Whitelist: m-line, direction, rtpmap/fmtp, mid, ssrc. Never ICE credentials,
 * fingerprints, Authorization or the whole SDP. Whether the server actually
 * accepts the audio m-line is the first thing to establish when no media is
 * recognised.
 */
void LogSdpMedia(const char* which, const std::string& sdp)
{
    ESP_LOGW(TAG, "----- %s media lines -----", which);
    size_t pos = 0;
    while (pos <= sdp.size()) {
        const size_t nl = sdp.find('\n', pos);
        const std::string line =
            sdp.substr(pos, nl == std::string::npos ? std::string::npos : nl - pos);
        const bool keep = line.rfind("m=", 0) == 0 || line.rfind("a=sendrecv", 0) == 0 ||
                          line.rfind("a=sendonly", 0) == 0 || line.rfind("a=recvonly", 0) == 0 ||
                          line.rfind("a=inactive", 0) == 0 || line.rfind("a=rtpmap", 0) == 0 ||
                          line.rfind("a=fmtp", 0) == 0 || line.rfind("a=mid", 0) == 0 ||
                          line.rfind("a=ssrc", 0) == 0 || line.rfind("a=rtcp-mux", 0) == 0;
        if (keep && !line.empty()) {
            ESP_LOGW(TAG, "  %s", line.c_str());
        }
        if (nl == std::string::npos) {
            break;
        }
        pos = nl + 1;
    }
    ESP_LOGW(TAG, "--------------------------");
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
    LogSdpMedia("OFFER", offer);

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
    if (!answer.empty()) {
        LogSdpMedia("ANSWER", answer);
    }
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
                p->stage.store(kStageConnected);
                break;
            case ESP_PEER_STATE_DATA_CHANNEL_OPENED:
                p->audio_ready = true;
                p->stage.store(kStageDataChannel);
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
        p->stage.store(kStageSdpGathered);
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
    while (!p->stopping.load()) {
        esp_peer_main_loop(p->peer);
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    // Nothing with a destructor is alive here, so self-deletion is safe.
    ESP_LOGI(TAG, "peer main loop exited");
    p->loop_confirmed_exit.store(true);
    xSemaphoreGive(p->loop_exited);
    vTaskDelete(nullptr);
}

/**
 * @brief Signaling body.
 *
 * A plain function so that every C++ local (`err`, `answer`) is destroyed on
 * return: FreeRTOS self-deletion does not unwind the stack and would leak them.
 */
void signal_body(Transport::Impl* p)
{
    if (xSemaphoreTake(p->sdp_ready, pdMS_TO_TICKS(kSdpWaitMs)) != pdTRUE) {
        if (p->stopping.load()) {
            ESP_LOGI(TAG, "signaling cancelled while waiting for the local SDP");
        } else {
            ESP_LOGE(TAG, "no local SDP within %d ms", kSdpWaitMs);
            p->stage.store(kStageSdpTimeout);
        }
        return;
    }
    // Stop() may have woken this wait on purpose: do not then start an HTTP
    // request against a peer that is being torn down.
    if (p->stopping.load()) {
        ESP_LOGI(TAG, "signaling cancelled after the SDP wake");
        return;
    }

    std::string answer;
    std::string err;
    if (!post_sdp(p->local_sdp, answer, &err)) {
        ESP_LOGE(TAG, "signaling failed: %s", err.c_str());
        p->stage.store(kStageSdpExchangeFailed);
        return;
    }
    // The request can take seconds; cancellation during it must be honoured.
    if (p->stopping.load()) {
        ESP_LOGI(TAG, "signaling cancelled after the SDP exchange");
        return;
    }
    p->stage.store(kStageAnswerReceived);

    esp_peer_msg_t msg = {};
    msg.type = ESP_PEER_MSG_TYPE_SDP;
    msg.data = (uint8_t*)answer.data();
    msg.size = (int)answer.size();
    const int ret = esp_peer_send_msg(p->peer, &msg);
    ESP_LOGI(TAG, "fed answer to peer -> %d", ret);
}

void signal_task(void* arg)
{
    auto* p = impl_of(arg);
    signal_body(p);          // all C++ locals destroyed here
    p->sig_confirmed_exit.store(true);
    xSemaphoreGive(p->sig_exited);
    vTaskDelete(nullptr);
}

}  // namespace

Transport::~Transport()
{
    Stop();
    // Only free the context when every task confirmed exit. If cleanup had to
    // leak it (a task could not be joined), deleting here would hand a live
    // task a dangling pointer - the leak is the safe outcome.
    if (impl_ != nullptr && !impl_->leaked.load()) {
        delete impl_;
    } else if (impl_ != nullptr) {
        ESP_LOGE(TAG, "context intentionally retained: a task did not confirm exit");
    }
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
    p->loop_exited = xSemaphoreCreateBinary();
    p->sig_exited = xSemaphoreCreateBinary();
    if (p->sdp_ready == nullptr || p->loop_exited == nullptr || p->sig_exited == nullptr) {
        *err = "semaphore alloc failed";
        cleanup(p);
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
        cleanup(p);
        return false;
    }

    // Handles are saved so Stop() can actually join instead of guessing.
    p->loop_created.store(true);
    if (xTaskCreate(loop_task, "m_peer_loop", kPeerTaskStackBytes, p, 5, &p->loop_task) != pdPASS) {
        p->loop_created.store(false);
        *err = "peer loop task create failed";
        cleanup(p);
        return false;
    }
    p->sig_created.store(true);
    if (xTaskCreate(signal_task, "m_peer_sig", kPeerTaskStackBytes, p, 5, &p->sig_task) != pdPASS) {
        p->sig_created.store(false);
        *err = "signaling task create failed";
        cleanup(p);
        return false;
    }

    vTaskDelay(pdMS_TO_TICKS(300));
    const int conn_ret = esp_peer_new_connection(p->peer);
    ESP_LOGI(TAG, "esp_peer_new_connection -> %d", conn_ret);
    if (conn_ret != 0) {
        *err = "esp_peer_new_connection failed (" + std::to_string(conn_ret) + ")";
        cleanup(p);
        return false;
    }
    return true;
}

void Transport::cleanup(Impl* p)
{
    if (p == nullptr) {
        return;
    }
    // Idempotent: Stop() and the destructor both call this, and Start() calls it
    // on every failure path.
    if (p->cleaned.exchange(true)) {
        return;
    }

    // Keep the stop request in force across repeated Stop() calls.
    p->stopping.store(true);

    // Wake a signaling task that is blocked on the local SDP so it can observe
    // the cancellation instead of waiting out its full timeout.
    if (p->sdp_ready != nullptr) {
        xSemaphoreGive(p->sdp_ready);
    }

    bool loop_exited = !p->loop_created.load();
    bool sig_exited = !p->sig_created.load();

    if (p->loop_created.load()) {
        // Wait on the semaphore, but only trust the task's own exit flag: a
        // timeout is not proof that the task stopped touching the peer.
        xSemaphoreTake(p->loop_exited, pdMS_TO_TICKS(kTaskExitWaitMs));
        loop_exited = p->loop_confirmed_exit.load();
    }
    if (p->sig_created.load()) {
        xSemaphoreTake(p->sig_exited, pdMS_TO_TICKS(kTaskExitWaitMs));
        sig_exited = p->sig_confirmed_exit.load();
    }

    if (!loop_exited || !sig_exited) {
        // A task may still be inside an HTTP request, a DNS lookup or a peer
        // poll. Freeing the peer, the semaphores *or the Impl itself* underneath
        // it would be a use-after-free, so keep everything alive, keep the stop
        // request in force, and mark the Impl as never-releasable.
        ESP_LOGE(TAG,
                 "tasks did not confirm exit (loop=%d sig=%d) within %d ms; "
                 "keeping the peer, the semaphores AND the context alive - a task "
                 "may still be dereferencing them",
                 (int)loop_exited, (int)sig_exited, kTaskExitWaitMs);
        // Do NOT clear stopping: the request must stay in force so the tasks
        // head for their own exit instead of continuing to run.
        p->leaked = true;
        return;
    }

    ESP_LOGI(TAG, "both tasks confirmed exit; releasing transport resources");

    if (p->peer != nullptr) {
        esp_peer_close(p->peer);
        p->peer = nullptr;
    }
    if (p->sdp_ready != nullptr) {
        vSemaphoreDelete(p->sdp_ready);
        p->sdp_ready = nullptr;
    }
    if (p->loop_exited != nullptr) {
        vSemaphoreDelete(p->loop_exited);
        p->loop_exited = nullptr;
    }
    if (p->sig_exited != nullptr) {
        vSemaphoreDelete(p->sig_exited);
        p->sig_exited = nullptr;
    }
    {
        std::lock_guard<std::mutex> lock(p->mtx);
        p->audio_ready = false;
    }
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

int Transport::stage() const
{
    auto* p = impl_;
    return p == nullptr ? 0 : p->stage.load();
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
    cleanup(p);
}

}  // namespace webrtc_transport

#endif  // CONFIG_STACKCHAN_WEBRTC_M0 || CONFIG_STACKCHAN_WEBRTC_M1
