#include "webrtc_m0.h"

#include <cstring>
#include <string>

#include <cJSON.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/semphr.h>
#include <freertos/task.h>

#include "esp_peer.h"
#include "esp_peer_default.h"

#define TAG "WebRTC-M0"

namespace {

// ---------------------------------------------------------------- constants

// Aliyun hands back the answer SDP from this endpoint. The path is fixed for
// Realtime models; `inference` is the one used by TTS/ASR models.
constexpr const char* kWebRtcHostFmt = "https://%s.cn-beijing.maas.aliyuncs.com/api/v1/webrtc/realtime";
constexpr const char* kDataChannelLabel = "oai-events";

// The DataChannel event the Realtime protocol expects first.
constexpr const char* kSessionUpdate =
    "{"
    "\"event_id\":\"event_m0\","
    "\"type\":\"session.update\","
    "\"session\":{"
    "\"modalities\":[\"text\",\"audio\"],"
    "\"instructions\":\"你是一个测试用的助手，只用一句话回应。\","
    "\"turn_detection\":{\"type\":\"server_vad\",\"threshold\":0.5,\"silence_duration_ms\":800}"
    "}"
    "}";

constexpr int kSdpWaitMs = 30000;  // how long to wait for the local SDP / answer

// ------------------------------------------------------------------- state

struct M0 {
    esp_peer_handle_t peer = nullptr;
    std::string local_sdp;
    SemaphoreHandle_t sdp_ready = nullptr;
    SemaphoreHandle_t answer_ready = nullptr;
    std::string answer_sdp;
    bool channel_requested = false;
    bool update_sent = false;
    bool saw_session_created = false;
    bool saw_session_updated = false;
    int peer_state = -1;
};

M0 g;

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

bool post_sdp(const std::string& offer, std::string& answer)
{
    char host[160];
    snprintf(host, sizeof(host), kWebRtcHostFmt, CONFIG_STACKCHAN_ALIYUN_WORKSPACE_ID);

    char url[320];
    snprintf(url, sizeof(url), "%s?model=%s", host, CONFIG_STACKCHAN_ALIYUN_MODEL);

    ESP_LOGI(TAG, "POST %s", url);
    ESP_LOGI(TAG, "offer is %d bytes", (int)offer.size());

    esp_http_client_config_t cfg = {};
    cfg.url = url;
    cfg.method = HTTP_METHOD_POST;
    cfg.timeout_ms = 20000;
    cfg.crt_bundle_attach = esp_crt_bundle_attach;
    cfg.buffer_size = 4096;
    cfg.buffer_size_tx = 4096;

    esp_http_client_handle_t client = esp_http_client_init(&cfg);
    if (client == nullptr) {
        ESP_LOGE(TAG, "http client init failed");
        return false;
    }

    char auth[512];
    snprintf(auth, sizeof(auth), "Bearer %s", CONFIG_STACKCHAN_ALIYUN_API_KEY);
    esp_http_client_set_header(client, "Content-Type", "application/sdp");
    esp_http_client_set_header(client, "Authorization", auth);
    esp_http_client_set_post_field(client, offer.c_str(), (int)offer.size());

    const esp_err_t err = esp_http_client_perform(client);
    const int status = esp_http_client_get_status_code(client);
    ESP_LOGI(TAG, "SDP exchange: err=%s http=%d", esp_err_to_name(err), status);

    bool ok = false;
    if (err == ESP_OK && status == 200) {
        answer.clear();
        char buf[1025];
        int read = 0;
        // Read until the body ends; the answer SDP is a few KB.
        while ((read = esp_http_client_read(client, buf, sizeof(buf) - 1)) > 0) {
            buf[read] = '\0';
            answer += buf;
        }
        ok = !answer.empty();
        ESP_LOGI(TAG, "answer is %d bytes", (int)answer.size());
        if (!ok) {
            ESP_LOGE(TAG, "answer body empty");
        }
    } else {
        // Surface the server's reason - a rejected SDP is the most likely
        // outcome of this whole experiment.
        char buf[513];
        const int read = esp_http_client_read(client, buf, sizeof(buf) - 1);
        if (read > 0) {
            buf[read] = '\0';
            ESP_LOGE(TAG, "server said: %s", buf);
        }
    }

    esp_http_client_cleanup(client);
    return ok;
}

// --------------------------------------------------------------- callbacks

int on_state(esp_peer_state_t state, void* /*ctx*/)
{
    ESP_LOGI(TAG, "[state] %s (%d)", state_name(state), (int)state);
    g.peer_state = (int)state;

    if (state == ESP_PEER_STATE_DATA_CHANNEL_CONNECTED && !g.channel_requested) {
        // Aliyun expects a client-created channel labelled `oai-events`; the
        // server opens its own channel (`txt`) for pushes.
        g.channel_requested = true;
        esp_peer_data_channel_cfg_t ch = {};
        ch.type = ESP_PEER_DATA_CHANNEL_RELIABLE;
        ch.ordered = true;
        // The field is a non-const char*; the literal outlives the call.
        ch.label = const_cast<char*>(kDataChannelLabel);
        const int ret = esp_peer_create_data_channel(g.peer, &ch);
        ESP_LOGI(TAG, "create data channel '%s' -> %d", kDataChannelLabel, ret);
    }

    if (state == ESP_PEER_STATE_DATA_CHANNEL_OPENED && !g.update_sent) {
        g.update_sent = true;
        esp_peer_data_frame_t frame = {};
        frame.type = ESP_PEER_DATA_CHANNEL_STRING;
        frame.stream_id = 0;
        frame.data = (uint8_t*)kSessionUpdate;
        frame.size = (int)strlen(kSessionUpdate);
        const int ret = esp_peer_send_data(g.peer, &frame);
        ESP_LOGI(TAG, "sent session.update -> %d", ret);
    }

    return 0;
}

int on_msg(esp_peer_msg_t* info, void* /*ctx*/)
{
    if (info == nullptr || info->data == nullptr) {
        return 0;
    }
    if (info->type == ESP_PEER_MSG_TYPE_SDP) {
        g.local_sdp.assign((const char*)info->data, info->size);
        ESP_LOGI(TAG, "[msg] local SDP, %d bytes", info->size);
        if (g.sdp_ready) {
            xSemaphoreGive(g.sdp_ready);
        }
    } else if (info->type == ESP_PEER_MSG_TYPE_CANDIDATE) {
        // Aliyun does not trickle candidates back in the documented flow; log
        // and ignore so a surprise here is at least visible.
        ESP_LOGI(TAG, "[msg] remote candidate: %.*s", info->size, (const char*)info->data);
    }
    return 0;
}

int on_data(esp_peer_data_frame_t* frame, void* /*ctx*/)
{
    if (frame == nullptr || frame->data == nullptr) {
        return 0;
    }
    const std::string text((const char*)frame->data, frame->size);
    ESP_LOGI(TAG, "[data] %s", text.c_str());

    // The two events that decide the experiment.
    if (text.find("session.created") != std::string::npos) {
        g.saw_session_created = true;
        ESP_LOGW(TAG, ">>> session.created received");
    }
    if (text.find("session.updated") != std::string::npos) {
        g.saw_session_updated = true;
        ESP_LOGW(TAG, ">>> session.updated received");
    }
    if (text.find("\"error\"") != std::string::npos) {
        ESP_LOGE(TAG, ">>> server error event");
    }
    return 0;
}

int on_audio(esp_peer_audio_frame_t* frame, void* /*ctx*/)
{
    // M0 does not consume media; count a few frames so it is clear whether the
    // server started talking even without an uplink track.
    static int n = 0;
    if (++n <= 3) {
        ESP_LOGI(TAG, "[audio] frame %d, %d bytes", n, frame ? frame->size : 0);
    }
    return 0;
}

// ------------------------------------------------------------------ tasks

void peer_loop_task(void* /*arg*/)
{
    ESP_LOGI(TAG, "peer main loop started");
    esp_peer_main_loop(g.peer);
    ESP_LOGW(TAG, "peer main loop exited");
    vTaskDelete(nullptr);
}

void signaling_task(void* /*arg*/)
{
    if (xSemaphoreTake(g.sdp_ready, pdMS_TO_TICKS(kSdpWaitMs)) != pdTRUE) {
        ESP_LOGE(TAG, "VERDICT: FAIL - no local SDP generated");
        vTaskDelete(nullptr);
        return;
    }

    if (!post_sdp(g.local_sdp, g.answer_sdp)) {
        ESP_LOGE(TAG, "VERDICT: FAIL - SDP exchange rejected by the server");
        vTaskDelete(nullptr);
        return;
    }

    esp_peer_msg_t msg = {};
    msg.type = ESP_PEER_MSG_TYPE_SDP;
    msg.data = (uint8_t*)g.answer_sdp.data();
    msg.size = (int)g.answer_sdp.size();
    const int ret = esp_peer_send_msg(g.peer, &msg);
    ESP_LOGI(TAG, "fed answer to peer -> %d", ret);

    // Wait for the handshake to finish, then report.
    const int kWaitStepMs = 500;
    const int kWaitTotalMs = 30000;
    for (int waited = 0; waited < kWaitTotalMs; waited += kWaitStepMs) {
        vTaskDelay(pdMS_TO_TICKS(kWaitStepMs));
        if (g.saw_session_created && g.saw_session_updated) {
            break;
        }
    }

    ESP_LOGW(TAG, "==================== M0 VERDICT ====================");
    ESP_LOGW(TAG, "  last peer state   : %s (%d)", state_name((esp_peer_state_t)g.peer_state),
             g.peer_state);
    ESP_LOGW(TAG, "  session.created   : %s", g.saw_session_created ? "YES" : "no");
    ESP_LOGW(TAG, "  session.updated   : %s", g.saw_session_updated ? "YES" : "no");
    if (g.saw_session_created && g.saw_session_updated) {
        ESP_LOGW(TAG, "  => INTEROP OK: esp_peer talks to Aliyun.");
        ESP_LOGW(TAG, "     The WebRTC port is worth building.");
    } else if (g.peer_state >= ESP_PEER_STATE_CONNECTED) {
        ESP_LOGW(TAG, "  => PARTIAL: transport came up but the model never answered.");
    } else {
        ESP_LOGW(TAG, "  => INTEROP FAILED: stop here, keep the WebSocket path.");
    }
    ESP_LOGW(TAG, "===================================================");
    vTaskDelete(nullptr);
}

}  // namespace

void WebRtcM0Run()
{
    ESP_LOGW(TAG, "M0: probing Aliyun WebRTC interop (no media will be sent)");
    ESP_LOGW(TAG, "workspace=%s model=%s", CONFIG_STACKCHAN_ALIYUN_WORKSPACE_ID,
             CONFIG_STACKCHAN_ALIYUN_MODEL);

    g.sdp_ready = xSemaphoreCreateBinary();
    g.answer_ready = xSemaphoreCreateBinary();

    // Generating the DTLS certificate is the slow part of startup and must not
    // happen inside the connection setup.
    ESP_LOGI(TAG, "pre-generating DTLS certificate...");
    const int cert_ret = esp_peer_pre_generate_cert();
    ESP_LOGI(TAG, "cert -> %d", cert_ret);

    esp_peer_cfg_t cfg = {};
    cfg.role = ESP_PEER_ROLE_CONTROLLING;
    cfg.ice_trans_policy = ESP_PEER_ICE_TRANS_POLICY_ALL;
    cfg.audio_info.codec = ESP_PEER_AUDIO_CODEC_OPUS;
    // WebRTC Opus is clocked at 48 kHz regardless of the capture rate.
    cfg.audio_info.sample_rate = 48000;
    cfg.audio_info.channel = 1;
    cfg.audio_dir = ESP_PEER_MEDIA_DIR_SEND_RECV;
    cfg.video_dir = ESP_PEER_MEDIA_DIR_NONE;
    cfg.enable_data_channel = true;
    // The label matters: the Realtime protocol expects `oai-events`.
    cfg.manual_ch_create = true;
    cfg.on_state = on_state;
    cfg.on_msg = on_msg;
    cfg.on_data = on_data;
    cfg.on_audio_data = on_audio;

    // Trim the buffers: the defaults are sized for video and would be wasteful
    // in a device that only ever sends a mono voice track.
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
    cfg.extra_cfg = &tuning;
    cfg.extra_size = sizeof(tuning);

    const int ret = esp_peer_open(&cfg, esp_peer_get_default_impl(), &g.peer);
    ESP_LOGI(TAG, "esp_peer_open -> %d", ret);
    if (ret != 0 || g.peer == nullptr) {
        ESP_LOGE(TAG, "VERDICT: FAIL - esp_peer_open failed (%d)", ret);
        return;
    }

    xTaskCreate(peer_loop_task, "peer_loop", 8192, nullptr, 5, nullptr);
    xTaskCreate(signaling_task, "peer_signal", 8192, nullptr, 5, nullptr);

    vTaskDelay(pdMS_TO_TICKS(300));
    const int conn_ret = esp_peer_new_connection(g.peer);
    ESP_LOGI(TAG, "esp_peer_new_connection -> %d", conn_ret);
}
