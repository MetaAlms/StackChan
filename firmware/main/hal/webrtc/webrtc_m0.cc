#include <sdkconfig.h>

// PROBE_GUARD: M0 interoperability probe. Compiled only for that build so the normal
// firmware carries none of this code.
#if CONFIG_STACKCHAN_WEBRTC_M0

#include "webrtc_m0.h"

#include <atomic>
#include <mutex>
#include <string>

#include <esp_log.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cJSON.h>

#include "webrtc_transport.h"

#define TAG "M0"

/**
 * @brief M0: Aliyun WebRTC interoperability probe.
 *
 * Shares the transport with M1 - the SDP/ICE/DTLS/DataChannel logic lives in
 * webrtc_transport and is not duplicated here. M0's own semantics are
 * unchanged: it sends **no media**, and its verdict distinguishes a failed
 * connection from a connected one instead of collapsing both into "partial".
 */

namespace {

constexpr int kWaitMs = 45000;
constexpr int kPollMs = 200;

std::atomic<bool> g_stop{false};

struct Observed {
    std::mutex mtx;
    bool session_created = false;
    bool session_updated = false;
    bool update_sent = false;
    bool channel_seen = false;
    std::string channel_label;
    uint16_t channel_stream = 0xFFFF;
    /** Stream discovered from this session's events; never hard-coded. */
    uint16_t reply_stream = 0xFFFF;
    bool reply_stream_known = false;
} g_obs;

webrtc_transport::Transport* g_transport = nullptr;

/**
 * @brief Send session.update on the stream this session discovered.
 *
 * Restored after the transport refactor dropped it: without an update the
 * server never answers session.updated, so M0 would wait out its full timeout
 * even on a perfectly healthy connection.
 *
 * The lock is released before sending: the send can synchronously reach the
 * peer, and a callback taking g_obs.mtx from that path must not deadlock.
 */
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
    if (g_transport == nullptr) {
        return;
    }

    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "event_id", "event_m0_cfg");
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

    if (g_transport->SendJson(json, stream)) {
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        g_obs.update_sent = true;
        ESP_LOGI(TAG, "[cfg] session.update sent on stream %u", (unsigned)stream);
    } else {
        ESP_LOGW(TAG, "[cfg] session.update send failed; another attempt is allowed "
                      "on the next event");
    }
}

/** Record the two events that decide the experiment. */
void HandleEvent(const std::string& json, uint16_t stream_id)
{
    const bool created = json.find("\"session.created\"") != std::string::npos;
    const bool updated = json.find("\"session.updated\"") != std::string::npos;
    ESP_LOGI(TAG, "[data stream=%u] %s", (unsigned)stream_id, json.c_str());
    {
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        if (created) {
            g_obs.session_created = true;
            if (!g_obs.reply_stream_known) {
                g_obs.reply_stream = stream_id;
                g_obs.reply_stream_known = true;
            }
        }
        if (updated) {
            g_obs.session_updated = true;
        }
    }
    if (created) {
        ESP_LOGW(TAG, ">>> session.created received on stream %u", (unsigned)stream_id);
        TrySendSessionUpdate();
    }
    if (updated) {
        ESP_LOGW(TAG, ">>> session.updated received");
    }
}

}  // namespace

void WebRtcM0Run()
{
    ESP_LOGW(TAG, "M0: probing Aliyun WebRTC interop (NO media will be sent)");
    ESP_LOGW(TAG, "model=%s", CONFIG_STACKCHAN_ALIYUN_MODEL);

    webrtc_transport::Transport transport;
    webrtc_transport::Config cfg = {};
    cfg.pcm_sample_rate = 48000;
    cfg.pcm_channels = 1;
    cfg.rtp_clock_rate = 48000;
    cfg.enable_data_channel = true;
    cfg.data_channel_label = "oai-events";

    webrtc_transport::Callbacks cb = {};
    cb.on_data = [](const std::string& json, uint16_t stream_id) {
        HandleEvent(json, stream_id);
    };
    cb.on_channel_open = [](esp_peer_data_channel_info_t* ch) {
        {
            std::lock_guard<std::mutex> lock(g_obs.mtx);
            g_obs.channel_seen = true;
            g_obs.channel_label = (ch && ch->label) ? ch->label : "?";
            g_obs.channel_stream = ch ? ch->stream_id : 0xFFFF;
        }
        // A channel opening after session.created is the retry opportunity.
        TrySendSessionUpdate();
    };
    // M0 never sends media, so there is no sender to quiesce before close.
    cb.on_audio = nullptr;
    cb.on_before_close = nullptr;

    g_transport = &transport;
    std::string err;
    if (!transport.Start(cfg, cb, &err)) {
        ESP_LOGE(TAG, "VERDICT: FAIL - transport: %s", err.c_str());
        g_transport = nullptr;
        return;
    }

    int waited = 0;
    while (waited < kWaitMs) {
        {
            std::lock_guard<std::mutex> lock(g_obs.mtx);
            if (g_obs.session_created && g_obs.session_updated) {
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(kPollMs));
        waited += kPollMs;
    }

    bool created = false, updated = false, channel = false;
    std::string label;
    uint16_t stream = 0;
    {
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        created = g_obs.session_created;
        updated = g_obs.session_updated;
        channel = g_obs.channel_seen;
        label = g_obs.channel_label;
        stream = g_obs.channel_stream;
    }
    const esp_peer_state_t st = transport.state();
    bool sent_update = false;
    {
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        sent_update = g_obs.update_sent;
    }
    transport.Stop();
    g_transport = nullptr;

    ESP_LOGW(TAG, "---------------- M0 RESULT ----------------");
    ESP_LOGW(TAG, "  peer state at end  : %d", (int)st);
    ESP_LOGW(TAG, "  data channel       : %s label='%s' stream=%u",
             channel ? "OPENED" : "not seen", label.c_str(), (unsigned)stream);
    ESP_LOGW(TAG, "  session.created    : %s", created ? "YES" : "NO");
    ESP_LOGW(TAG, "  session.update sent: %s", sent_update ? "YES" : "NO");
    ESP_LOGW(TAG, "  session.updated    : %s", updated ? "YES" : "NO");
    ESP_LOGW(TAG, "  media sent         : NONE (M0 sends no media by design)");

    // CONNECT_FAILED (8) must never be reported as a partial success.
    if (st == ESP_PEER_STATE_CONNECT_FAILED) {
        ESP_LOGE(TAG, "VERDICT: FAIL - connection failed (ICE/DTLS did not complete)");
    } else if (created && updated) {
        ESP_LOGW(TAG, "VERDICT: PASS - transport and session handshake verified");
        ESP_LOGW(TAG, "  (this says nothing about media: M0 sends none)");
    } else if (st >= ESP_PEER_STATE_CONNECTED || channel) {
        ESP_LOGW(TAG, "VERDICT: PARTIAL - connected but the session handshake did not "
                      "complete within %d ms", kWaitMs);
    } else {
        ESP_LOGE(TAG, "VERDICT: FAIL - never reached a connected state (state=%d)", (int)st);
    }
    ESP_LOGW(TAG, "-------------------------------------------");
    ESP_LOGW(TAG, "M0 probe finished; idling");
}

#endif  // CONFIG_STACKCHAN_WEBRTC_M0
