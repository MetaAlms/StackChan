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
    bool channel_seen = false;
    std::string channel_label;
    uint16_t channel_stream = 0xFFFF;
} g_obs;

/** Record the two events that decide the experiment. */
void HandleEvent(const std::string& json, uint16_t stream_id)
{
    const bool created = json.find("\"session.created\"") != std::string::npos;
    const bool updated = json.find("\"session.updated\"") != std::string::npos;
    ESP_LOGI(TAG, "[data stream=%u] %s", (unsigned)stream_id, json.c_str());
    std::lock_guard<std::mutex> lock(g_obs.mtx);
    if (created) {
        g_obs.session_created = true;
        ESP_LOGW(TAG, ">>> session.created received");
    }
    if (updated) {
        g_obs.session_updated = true;
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
        std::lock_guard<std::mutex> lock(g_obs.mtx);
        g_obs.channel_seen = true;
        g_obs.channel_label = (ch && ch->label) ? ch->label : "?";
        g_obs.channel_stream = ch ? ch->stream_id : 0xFFFF;
    };
    // M0 never sends media, so there is no sender to quiesce before close.
    cb.on_audio = nullptr;
    cb.on_before_close = nullptr;

    std::string err;
    if (!transport.Start(cfg, cb, &err)) {
        ESP_LOGE(TAG, "VERDICT: FAIL - transport: %s", err.c_str());
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
    transport.Stop();

    ESP_LOGW(TAG, "---------------- M0 RESULT ----------------");
    ESP_LOGW(TAG, "  peer state at end  : %d", (int)st);
    ESP_LOGW(TAG, "  data channel       : %s label='%s' stream=%u",
             channel ? "OPENED" : "not seen", label.c_str(), (unsigned)stream);
    ESP_LOGW(TAG, "  session.created    : %s", created ? "YES" : "NO");
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
