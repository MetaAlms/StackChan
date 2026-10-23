#include <sdkconfig.h>
#if CONFIG_STACKCHAN_WEBRTC_M15
#include "webrtc_m15.h"
#include "media_codec.h"
#include "media_admission.h"
#include "bounded_media_queue.h"
#include "console_command.h"
#include "decimal_log_value.h"
#include "session_run_control.h"
#include "webrtc_transport.h"
#include "audio_service.h"
#include "board.h"
#include "wifi_manager.h"
#include "hal/board/cores3_audio_codec.h"
#include "hal/hal.h"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <cJSON.h>
#include <driver/usb_serial_jtag.h>
#include <freertos/queue.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_timer.h>

namespace {
constexpr const char* kTag = "WebRTC-Run";
using webrtc_media::RunMode;
using webrtc_media::RunState;
using webrtc_media::StopReason;
using webrtc_media::SignedDecimal;
using webrtc_media::UnsignedDecimal;
const cJSON* Field(const cJSON* obj, const char* name) { return cJSON_GetObjectItemCaseSensitive(obj, name); }
bool StringIs(const cJSON* obj, const char* value) { return cJSON_IsString(obj) && strcmp(obj->valuestring, value) == 0; }
const char* StateName(RunState s) {
    switch (s) {
        case RunState::Idle: return "Idle";
        case RunState::Preparing: return "Preparing";
        case RunState::Running: return "Running";
        case RunState::Stopping: return "Stopping";
        case RunState::Done: return "Done";
        case RunState::Failed: return "Failed";
    }
    return "Invalid";
}
const char* ReasonName(StopReason s) {
    switch (s) {
        case StopReason::None: return "none";
        case StopReason::Requested: return "requested";
        case StopReason::PrepareTimeout: return "prepare_timeout";
        case StopReason::RunTimeout: return "run_timeout";
        case StopReason::Error: return "error";
        case StopReason::Disconnected: return "disconnected";
        case StopReason::Completed: return "completed";
    }
    return "invalid";
}
struct MicFrame {
    std::array<int16_t, 320> pcm;
    uint32_t pts_ms;
    int64_t afe_emitted_us;  // Callback time, not an ADC capture timestamp.
};
struct DownFrame {
    std::vector<uint8_t> data;
    uint32_t pts_ms;
    int64_t received_us;
};
struct Event { std::string json; uint16_t stream; int64_t received_us; };

// Persistent hardware/AFE tasks; each explicitly started run owns fresh media
// queues and transport. The capture sink is detached only after AFE quiescence.
AudioService g_audio;
bool g_audio_initialized = false, g_network_initialized = false;
struct Run {
    const webrtc_media::ConsoleCommand command;
    webrtc_media::SessionRunControl control;  // Owned only by the media worker.
    webrtc_transport::Transport transport;
    webrtc_media::Codec codec;
    webrtc_media::SpscQueue<MicFrame, 6> mic;
    webrtc_media::SpscQueue<DownFrame, 8> down;
    webrtc_media::SpscQueue<Event, 8> events;
    std::atomic<RunState> state{RunState::Preparing};
    std::atomic<bool> worker_finished{false};
    bool reusable = false;  // Published by worker_finished after RunBody returns.
    webrtc_media::MediaAdmission capture_gate;
    std::atomic<bool> accept_peer{true}, stop_requested{false}, disconnected{false};
    std::atomic<int64_t> stop_requested_us{0};
    std::atomic<unsigned> capture_callbacks{0};
    std::atomic<bool> capture_warming{false};
    std::atomic<int64_t> first_warm_afe_us{0};
    std::atomic<uint32_t> mic_full{0}, mic_invalid{0}, rx_full{0}, rx_invalid{0}, event_full{0}, event_invalid{0};
    // Written only by the sole AFE callback, read after admission closes and it exits.
    uint32_t capture_pts_ms = 0;
    int64_t first_afe_us = 0, last_afe_us = 0, max_afe_gap_us = 0;
    uint32_t session_tag = 0, sent = 0, decoded = 0, failures = 0;
    uint32_t stop_discard_mic = 0, stop_discard_down = 0, stop_discard_event = 0;
    uint32_t speech = 0, responses = 0, asr_failed = 0;
    std::array<uint32_t, 6> playback_admission{};
    uint32_t first_tx_pts = 0, last_tx_pts = 0, first_rx_pts = 0, last_rx_pts = 0;
    int64_t first_rx_us = 0, last_rx_us = 0, last_send_us = 0, max_send_gap_us = 0, max_mic_age_us = 0;
    int64_t run_stopped_us = 0;
    uint32_t initial_read_ok = 0, initial_write_ok = 0;
    uint32_t initial_read_failed = 0, initial_write_failed = 0;
    uint32_t initial_capture_cvt_failed = 0, initial_drain_failed = 0, initial_drained_frames = 0, initial_afe_reset_failed = 0;
    AudioService::WebRtcPlaybackSnapshot initial_playback{};
    bool audio_started = false, configuration_ok = false, near_observed = false;
    explicit Run(const webrtc_media::ConsoleCommand& c) : command(c) {}
};
Run* g_run = nullptr;  // Only the console owns/replaces this pointer.
QueueHandle_t g_run_commands = nullptr;
TaskHandle_t g_worker = nullptr;  // One stack for all runs, created on first START.
std::atomic<Run*> g_capture_run{nullptr};  // Detach only after capture/AFE join.

unsigned KeywordBits(const cJSON* text) {
    if (!cJSON_IsString(text)) return 0;
    const std::string s = text->valuestring;
    unsigned bits = 0;
    if (s.find("测试") != std::string::npos && s.find("语音") != std::string::npos && s.find("连接") != std::string::npos) bits |= 1;
    if (s.find("蓝色") != std::string::npos && s.find("书") != std::string::npos) bits |= 2;
    if ((s.find("一加一") != std::string::npos || s.find("1加1") != std::string::npos || s.find("1+1") != std::string::npos) && s.find("等于") != std::string::npos) bits |= 4;
    return bits;
}
uint32_t SessionTag(const cJSON* session) {
    const cJSON* id = Field(session, "id");
    if (!cJSON_IsString(id) || strlen(id->valuestring) > 128) return 0;
    uint32_t tag = 2166136261u;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(id->valuestring); *p; ++p) tag = (tag ^ *p) * 16777619u;
    return tag;  // Correlation label only; no credentials or raw session/event payloads.
}
void PublishState(Run& r) {
    if (r.control.state() == RunState::Stopping) {
        r.capture_gate.Cancel();
        if (!r.run_stopped_us) r.run_stopped_us = esp_timer_get_time();
    }
    r.state.store(r.control.state());
}
void Stop(Run& r, StopReason reason) {
    r.capture_gate.Cancel();
    r.control.Stop(r.command.run_id, reason);
    PublishState(r);
}
bool Continue(Run& r) {
    r.control.Poll(esp_timer_get_time());
    if (r.stop_requested.load()) Stop(r, StopReason::Requested);
    if (r.disconnected.load()) Stop(r, StopReason::Disconnected);
    PublishState(r);
    return r.control.state() == RunState::Preparing || r.control.state() == RunState::Running;
}
void Capture(Run& r, std::vector<int16_t>&& data) {
    ++r.capture_callbacks;
    if (r.capture_warming.load() && !r.stop_requested.load()) {
        int64_t expected = 0;
        r.first_warm_afe_us.compare_exchange_strong(expected, esp_timer_get_time());
        // Discard warm-up output. The worker opens a new sample timeline only
        // after AFE initialization and the first callback have completed.
    } else if (r.capture_gate.CanAccept()) {
        const int64_t now = esp_timer_get_time();
        if (data.empty() || data.size() % 320) ++r.mic_invalid;
        else {
            if (r.first_afe_us == 0) r.first_afe_us = now;
            if (r.last_afe_us != 0) r.max_afe_gap_us = std::max(r.max_afe_gap_us, now - r.last_afe_us);
            r.last_afe_us = now;
            for (size_t offset = 0; offset < data.size(); offset += 320) {
                MicFrame f;
                std::copy_n(data.data() + offset, 320, f.pcm.data());
                f.pts_ms = r.capture_pts_ms;
                f.afe_emitted_us = now;
                r.capture_pts_ms += 20;  // Queue loss preserves the AFE sample timeline.
                if (!r.mic.Push(std::move(f))) ++r.mic_full;
            }
        }
    }
    --r.capture_callbacks;
}
webrtc_transport::Callbacks Callbacks(Run& r) {
    webrtc_transport::Callbacks cb;
    cb.on_data = [&r](const std::string& json, uint16_t stream) {
        if (!r.accept_peer.load()) return;
        if (json.size() > 4096) { ++r.event_invalid; return; }
        cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
        if (!root) { ++r.event_invalid; return; }
        const cJSON* t = Field(root, "type");
        const bool relevant = StringIs(t, "session.created") || StringIs(t, "session.updated") ||
            StringIs(t, "input_audio_buffer.speech_started") ||
            StringIs(t, "conversation.item.input_audio_transcription.completed") ||
            StringIs(t, "conversation.item.input_audio_transcription.failed") ||
            StringIs(t, "response.created") || StringIs(t, "error");
        cJSON_Delete(root);
        if (relevant && !r.events.Push(Event{json, stream, esp_timer_get_time()})) ++r.event_full;
    };
    cb.on_audio = [&r](const uint8_t* data, int size, uint32_t pts) {
        if (!r.capture_gate.CanAccept()) return;
        if (!data || size <= 0 || size > webrtc_transport::Transport::MaxPayloadBytes()) { ++r.rx_invalid; return; }
        DownFrame f{std::vector<uint8_t>(data, data + size), pts, esp_timer_get_time()};
        if (!r.down.Push(std::move(f))) ++r.rx_full;
    };
    cb.on_state = [&r](esp_peer_state_t s) {
        if (!r.accept_peer.load()) return;
        if (s == ESP_PEER_STATE_DISCONNECTED || s == ESP_PEER_STATE_CLOSED || s == ESP_PEER_STATE_CONNECT_FAILED ||
            s == ESP_PEER_STATE_DATA_CHANNEL_CLOSED || s == ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED) r.disconnected = true;
    };
    return cb;
}
void HandleEvent(Run& r, const Event& e) {
    cJSON* root = cJSON_ParseWithLength(e.json.data(), e.json.size());
    const cJSON* t = Field(root, "type");
    if (StringIs(t, "session.created")) {
        if (r.control.state() == RunState::Preparing) {
            r.session_tag = SessionTag(Field(root, "session"));
            const char* config = R"({"type":"session.update","session":{"modalities":["text","audio"],"turn_detection":{"type":"server_vad","threshold":0.5,"silence_duration_ms":800},"input_audio_transcription":{"model":"qwen3-asr-flash-realtime"}}})";
            if (!r.transport.SendJson(config, e.stream)) { ++r.failures; Stop(r, StopReason::Error); }
        }
    } else if (StringIs(t, "session.updated") && r.control.state() == RunState::Preparing) {
        const cJSON* session = Field(root, "session");
        const cJSON* vad = Field(session, "turn_detection");
        const cJSON* threshold = Field(vad, "threshold");
        const cJSON* silence = Field(vad, "silence_duration_ms");
        r.configuration_ok = StringIs(Field(vad, "type"), "server_vad") &&
            cJSON_IsNumber(threshold) && threshold->valuedouble == 0.5 &&
            cJSON_IsNumber(silence) && silence->valueint == 800 &&
            StringIs(Field(Field(session, "input_audio_transcription"), "model"), "qwen3-asr-flash-realtime");
        if (!r.configuration_ok || !r.transport.CanSendAudio()) { ++r.failures; Stop(r, StopReason::Error); }
        else if (Continue(r) && !r.capture_warming.exchange(true)) {
            g_audio.EnableVoiceProcessing(true);
            Continue(r);  // Initialization may have consumed the prepare budget.
        }
    } else if (StringIs(t, "error")) { ++r.failures; Stop(r, StopReason::Error); }
    else if (r.control.state() == RunState::Running && e.received_us >= r.control.run_started_us()) {
        const int64_t dt = e.received_us - r.control.run_started_us();
        if (StringIs(t, "input_audio_buffer.speech_started")) ++r.speech;
        if (StringIs(t, "response.created")) ++r.responses;
        if (StringIs(t, "conversation.item.input_audio_transcription.failed")) ++r.asr_failed;
        if (StringIs(t, "conversation.item.input_audio_transcription.completed")) {
            const cJSON* id = Field(root, "item_id");
            const auto observation = r.control.ObserveAsr(r.command.run_id,
                cJSON_IsString(id) ? id->valuestring : "", KeywordBits(Field(root, "transcript")));
            ESP_LOGW(kTag, "ASR run=%s dt_ms=%s item=%u status=%d keyword_mask=%u transcript_omitted=1",
                r.command.run_id.c_str(), SignedDecimal(dt / 1000).c_str(), (unsigned)observation.ordinal,
                (int)observation.status, observation.keyword_mask);
            if (r.control.near_end_complete()) { r.near_observed = true; Stop(r, StopReason::Completed); }
        }
    }
    cJSON_Delete(root);
}
void BeginCaptureWindow(Run& r) {
    if (r.control.state() != RunState::Preparing || !r.configuration_ok || !r.first_warm_afe_us.load()) return;
    if (!r.control.Ready(r.command.run_id, esp_timer_get_time())) return;
    if (!r.capture_gate.Open()) { Continue(r); return; }
    r.capture_warming = false;
    PublishState(r);
    if (!Continue(r)) return;
    ESP_LOGW(kTag, "RUNNING run=%s mode=%s session_tag=%08lx t0_us=%s duration_s=%lu real_mic=1 speaker=%d first_warm_afe_us=%s",
        r.command.run_id.c_str(), r.command.mode == RunMode::NearEnd ? "near" : "duplex", (unsigned long)r.session_tag,
        SignedDecimal(r.control.run_started_us()).c_str(), (unsigned long)r.command.duration_seconds,
        r.command.mode == RunMode::Duplex, SignedDecimal(r.first_warm_afe_us.load()).c_str());
}
void Report(Run& r, bool final) {
    auto p = r.audio_started ? g_audio.GetWebRtcPlaybackSnapshot() : AudioService::WebRtcPlaybackSnapshot{};
    p.submitted_samples -= r.initial_playback.submitted_samples;
    p.submitted_calls -= r.initial_playback.submitted_calls;
    if (!p.submitted_calls) p.last_submit_started_us = p.last_submit_finished_us = 0;
    if (p.stop_completed_us <= r.initial_playback.stop_completed_us) p.stop_completed_us = 0;
    const int64_t now = esp_timer_get_time();
    ESP_LOGW(kTag, "%s run=%s state=%s reason=%s stage=%d session_tag=%08lx elapsed_ms=%s config=%d transport_joined=%d",
        final ? "FINAL" : "PROGRESS", r.command.run_id.c_str(), StateName(r.control.state()), ReasonName(r.control.stop_reason()),
        r.transport.stage(), (unsigned long)r.session_tag,
        SignedDecimal(r.control.run_started_us() < 0 ? -1 : ((r.run_stopped_us ? r.run_stopped_us : now) - r.control.run_started_us()) / 1000).c_str(),
        r.configuration_ok, r.transport.Stopped());
    ESP_LOGW(kTag, "MEDIA run=%s sent=%lu decoded=%lu failures=%lu mic_full=%lu mic_invalid=%lu rx_full=%lu rx_invalid=%lu event_full=%lu event_invalid=%lu",
        r.command.run_id.c_str(), (unsigned long)r.sent, (unsigned long)r.decoded, (unsigned long)r.failures,
        (unsigned long)r.mic_full.load(), (unsigned long)r.mic_invalid.load(), (unsigned long)r.rx_full.load(),
        (unsigned long)r.rx_invalid.load(), (unsigned long)r.event_full.load(), (unsigned long)r.event_invalid.load());
    ESP_LOGW(kTag, "TIMELINE run=%s tx_pts_ms=%lu:%lu rx_pts_ms=%lu:%lu rx_us=%s:%s max_send_gap_us=%s max_queue_age_us=%s",
        r.command.run_id.c_str(), (unsigned long)r.first_tx_pts, (unsigned long)r.last_tx_pts,
        (unsigned long)r.first_rx_pts, (unsigned long)r.last_rx_pts, SignedDecimal(r.first_rx_us).c_str(), SignedDecimal(r.last_rx_us).c_str(),
        SignedDecimal(r.max_send_gap_us).c_str(), SignedDecimal(r.max_mic_age_us).c_str());
    ESP_LOGW(kTag, "PLAYBACK run=%s accepted=%lu invalid=%lu stopped=%lu stale=%lu contended=%lu full=%lu queued_samples=%s inflight_samples=%s submitted_samples=%s submitted_calls=%s submit_us=%s:%s stop_requested_us=%s stop_completed_us=%s physical_end_unknown=1",
        r.command.run_id.c_str(), (unsigned long)r.playback_admission[0], (unsigned long)r.playback_admission[1],
        (unsigned long)r.playback_admission[2], (unsigned long)r.playback_admission[3], (unsigned long)r.playback_admission[4],
        (unsigned long)r.playback_admission[5], UnsignedDecimal(p.queued_samples).c_str(), UnsignedDecimal(p.inflight_samples).c_str(),
        UnsignedDecimal(p.submitted_samples).c_str(), UnsignedDecimal(p.submitted_calls).c_str(),
        SignedDecimal(p.last_submit_started_us).c_str(), SignedDecimal(p.last_submit_finished_us).c_str(),
        SignedDecimal(r.stop_requested_us.load()).c_str(), SignedDecimal(p.stop_completed_us).c_str());
    if (final) {
        ESP_LOGW(kTag, "DISCARD_AT_STOP run=%s mic=%lu down=%lu events=%lu",
            r.command.run_id.c_str(), (unsigned long)r.stop_discard_mic,
            (unsigned long)r.stop_discard_down, (unsigned long)r.stop_discard_event);
        ESP_LOGW(kTag, "EVIDENCE run=%s items=%u missing=%lu invalid=%lu duplicates=%lu overflow=%lu keyword_mask=%u near_observed=%d speech=%lu responses=%lu asr_failed=%lu T1_T4=NOT_ACCEPTED AEC=UNDETERMINED",
            r.command.run_id.c_str(), (unsigned)r.control.asr_items(), (unsigned long)r.control.asr_missing_item(),
            (unsigned long)r.control.asr_invalid_item(), (unsigned long)r.control.asr_duplicates(), (unsigned long)r.control.asr_overflow(),
            r.control.keyword_mask(), r.near_observed, (unsigned long)r.speech, (unsigned long)r.responses, (unsigned long)r.asr_failed);
        if (r.capture_callbacks.load() == 0) ESP_LOGW(kTag, "CAPTURE run=%s afe_us=%s:%s max_afe_gap_us=%s afe_samples=%lu capture_callbacks=%u adc_timestamps_available=0",
            r.command.run_id.c_str(), SignedDecimal(r.first_afe_us).c_str(), SignedDecimal(r.last_afe_us).c_str(), SignedDecimal(r.max_afe_gap_us).c_str(),
            (unsigned long)(r.capture_pts_ms * 16), r.capture_callbacks.load());
        else ESP_LOGE(kTag, "CAPTURE run=%s snapshot_unavailable=1 callback_join_timeout=1", r.command.run_id.c_str());
    }
    if (r.audio_started) {
        auto* h = static_cast<CoreS3AudioCodec*>(Board::GetInstance().GetAudioCodec());
        ESP_LOGW(kTag, "HARDWARE run=%s read_ok=%lu write_ok=%lu read_failed=%lu write_failed=%lu capture_cvt_failed=%lu drain_failed=%lu drained_source_frames=%lu afe_reset_failed=%lu dram=%u stack=%u",
            r.command.run_id.c_str(), (unsigned long)(h->ReadOk() - r.initial_read_ok),
            (unsigned long)(h->WriteOk() - r.initial_write_ok),
            (unsigned long)(h->ReadFailed() - r.initial_read_failed),
            (unsigned long)(h->WriteFailed() - r.initial_write_failed),
            (unsigned long)(g_audio.WebRtcCaptureConversionFailed() - r.initial_capture_cvt_failed),
            (unsigned long)(g_audio.WebRtcCaptureDrainFailed() - r.initial_drain_failed),
            (unsigned long)(g_audio.WebRtcCaptureDrainedFrames() - r.initial_drained_frames),
            (unsigned long)(g_audio.WebRtcAfeResetFailed() - r.initial_afe_reset_failed),
            (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT), (unsigned)uxTaskGetStackHighWaterMark(nullptr));
    }
}
void RunBody(Run& r) {
    r.control.Start(r.command.run_id, r.command.mode, r.command.duration_seconds, esp_timer_get_time());
    if (!g_network_initialized) {
        Board::GetInstance().StartNetwork();
        g_network_initialized = true;
    }
    while (Continue(r) && !WifiManager::GetInstance().IsConnected()) vTaskDelay(pdMS_TO_TICKS(20));
    if (Continue(r) && !r.codec.Open()) { ++r.failures; Stop(r, StopReason::Error); }
    if (Continue(r)) {
        auto* h = static_cast<CoreS3AudioCodec*>(Board::GetInstance().GetAudioCodec());
        r.initial_read_ok = h->ReadOk();
        r.initial_write_ok = h->WriteOk();
        r.initial_read_failed = h->ReadFailed();
        r.initial_write_failed = h->WriteFailed();
        if (!g_audio_initialized) {
            g_audio.Initialize(h);
            g_audio.SetWebRtcPcmSink([](std::vector<int16_t>&& pcm) {
                if (Run* active = g_capture_run.load()) Capture(*active, std::move(pcm));
            });
            g_audio.Start();
            g_audio_initialized = true;
        }
        r.initial_capture_cvt_failed = g_audio.WebRtcCaptureConversionFailed();
        r.initial_drain_failed = g_audio.WebRtcCaptureDrainFailed();
        r.initial_drained_frames = g_audio.WebRtcCaptureDrainedFrames();
        r.initial_afe_reset_failed = g_audio.WebRtcAfeResetFailed();
        r.initial_playback = g_audio.GetWebRtcPlaybackSnapshot();
        g_capture_run = &r;
        r.audio_started = true;
        std::string error;
        if (Continue(r) && !r.transport.Start({}, Callbacks(r), &error)) {
            ESP_LOGE(kTag, "transport start failed run=%s: %s", r.command.run_id.c_str(), error.c_str());
            ++r.failures; Stop(r, StopReason::Error);
        }
    }
    int64_t next_send_us = 0, next_report_us = esp_timer_get_time();
    const uint64_t playback_epoch = g_audio.WebRtcPlaybackEpoch();
    std::vector<uint8_t> encoded;
    std::vector<int16_t> pcm;
    while (Continue(r)) {
        // Poll before admitting events: delayed evidence cannot extend the window.
        Event e;
        if (r.events.Pop(e)) HandleEvent(r, e);
        if (!Continue(r)) break;
        BeginCaptureWindow(r);
        if (!Continue(r)) break;
        const int64_t now = esp_timer_get_time();
        if (r.audio_started && (g_audio.WebRtcCaptureDrainFailed() != r.initial_drain_failed ||
            g_audio.WebRtcCaptureConversionFailed() != r.initial_capture_cvt_failed ||
            g_audio.WebRtcAfeResetFailed() != r.initial_afe_reset_failed)) {
            ++r.failures; Stop(r, StopReason::Error); break;
        }
        if (r.control.state() == RunState::Running) {
            auto* h = static_cast<CoreS3AudioCodec*>(Board::GetInstance().GetAudioCodec());
            if (h->ReadFailed() != r.initial_read_failed || h->WriteFailed() != r.initial_write_failed ||
                r.mic_invalid.load() || r.rx_invalid.load() ||
                r.event_full.load() || r.event_invalid.load()) { ++r.failures; Stop(r, StopReason::Error); break; }
            MicFrame mic;
            if (now >= next_send_us && r.mic.Pop(mic)) {
                r.max_mic_age_us = std::max(r.max_mic_age_us, now - mic.afe_emitted_us);
                if (next_send_us == 0 || now - next_send_us > 60000) next_send_us = now;
                next_send_us += 20000;
                if (!r.codec.Encode20ms(mic.pcm.data(), mic.pcm.size(), encoded)) {
                    ++r.failures; Stop(r, StopReason::Error); break;
                }
                if (!Continue(r)) break;
                const int64_t send_called_us = esp_timer_get_time();
                if (!encoded.empty() && !r.transport.SendAudio(encoded.data(), encoded.size(), mic.pts_ms)) {
                    ++r.failures; Stop(r, StopReason::Error); break;
                }
                if (!encoded.empty()) {
                    if (!r.sent) r.first_tx_pts = mic.pts_ms;
                    r.last_tx_pts = mic.pts_ms;
                    if (r.last_send_us) r.max_send_gap_us = std::max(r.max_send_gap_us, send_called_us - r.last_send_us);
                    r.last_send_us = send_called_us;
                    ++r.sent;
                }
            }
            DownFrame down;
            if (r.down.Pop(down)) {
                if (!r.decoded) { r.first_rx_pts = down.pts_ms; r.first_rx_us = down.received_us; }
                r.last_rx_pts = down.pts_ms; r.last_rx_us = down.received_us;
                if (!r.codec.Decode(down.data.data(), down.data.size(), pcm)) { ++r.failures; Stop(r, StopReason::Error); break; }
                ++r.decoded;
                if (!Continue(r)) break;
                if (r.command.mode == RunMode::Duplex && !pcm.empty()) {
                    const auto result = g_audio.PushWebRtcPcm(std::move(pcm), playback_epoch);
                    ++r.playback_admission[static_cast<size_t>(result)];
                }
            }
        }
        if (now >= next_report_us) { Report(r, false); next_report_us = now + 5000000; }
        vTaskDelay(1);
    }
    r.capture_gate.Cancel();
    r.accept_peer = false;
    PublishState(r);
    if (r.audio_started) {
        g_audio.EnableVoiceProcessing(false);  // Joins input read/feed and AFE callbacks.
        g_audio.StopWebRtcPlayback();
    }
    g_capture_run = nullptr;
    r.transport.Stop();  // Sole worker owns sends; join before closing codecs.
    // A nonzero count violates the AFE iteration join: retain the owner on failure.
    const int64_t join_deadline = esp_timer_get_time() + 1000000;
    while (r.capture_callbacks.load() && esp_timer_get_time() < join_deadline) vTaskDelay(1);
    MicFrame discarded_mic;
    DownFrame discarded_down;
    Event discarded_event;
    while (r.mic.Pop(discarded_mic)) ++r.stop_discard_mic;
    while (r.down.Pop(discarded_down)) ++r.stop_discard_down;
    while (r.events.Pop(discarded_event)) ++r.stop_discard_event;
    r.codec.Close();
    const bool reset_ok = !r.audio_started || g_audio.WebRtcAfeResetFailed() == r.initial_afe_reset_failed;
    const bool finished_cleanly = reset_ok && !r.failures && r.transport.Stopped() && !r.capture_callbacks.load() &&
        r.control.stop_reason() != StopReason::PrepareTimeout && r.control.stop_reason() != StopReason::Disconnected;
    r.reusable = reset_ok && r.transport.Stopped() && !r.capture_callbacks.load();
    r.control.Finish(r.command.run_id, finished_cleanly);
    PublishState(r);
    Report(r, true);
    ESP_LOGW(kTag, "LIFECYCLE run=%s capture_quiescent=%d transport_joined=%d reusable=%d physical_end_unknown=1",
        r.command.run_id.c_str(), !r.capture_callbacks.load(), r.transport.Stopped(), r.reusable);
}
void Worker(void*) {
    while (true) {
        Run* run = nullptr;
        if (xQueueReceive(g_run_commands, &run, portMAX_DELAY) != pdTRUE) continue;
        RunBody(*run);  // Locals and FINAL logs complete before publication.
        run->worker_finished = true;
        // Do not dereference run again: the console may delete it immediately.
        // Keep this stack, so reopening does not await Idle task reclamation.
    }
}
}  // namespace

void WebRtcM15Run() {
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        cfg.rx_buffer_size = 512;
        if (usb_serial_jtag_driver_install(&cfg) != ESP_OK) {
            ESP_LOGE(kTag, "console unavailable; staying Idle, no capture/cloud session");
            while (true) { GetHAL().feedTheDog(); GetHAL().delay(100); }
        }
    }
    g_run_commands = xQueueCreate(1, sizeof(Run*));
    if (!g_run_commands) {
        ESP_LOGE(kTag, "command queue unavailable; staying Idle");
        while (true) { GetHAL().feedTheDog(); GetHAL().delay(100); }
    }
    ESP_LOGW(kTag, "IDLE; USB commands: START <run_id> near|duplex <5..180 seconds>, STOP <run_id>, STATUS; fresh run ID for each start");
    webrtc_media::ConsoleLineReader reader;
    while (true) {
        char bytes[64];
        const int count = usb_serial_jtag_read_bytes(bytes, sizeof(bytes), pdMS_TO_TICKS(50));
        for (int i = 0; i < count; ++i) {
            const auto result = reader.Feed(bytes[i]);
            if (result == webrtc_media::ConsoleLineReader::Result::Rejected) ESP_LOGW(kTag, "command rejected");
            if (result != webrtc_media::ConsoleLineReader::Result::Command) continue;
            const auto& c = reader.last_command();
            if (c.kind == webrtc_media::CommandKind::Status) {
                const bool finished = g_run && g_run->worker_finished.load();
                const bool idle = !g_run || (finished && g_run->reusable);
                ESP_LOGW(kTag, "STATUS run=%s state=%s restart_requires_reboot=%d", g_run ? g_run->command.run_id.c_str() : "none",
                    idle ? "Idle" : StateName(g_run->state.load()), finished && !g_run->reusable);
            } else if (c.kind == webrtc_media::CommandKind::Start) {
                if (g_run) {
                    if (!g_run->worker_finished.load()) { ESP_LOGW(kTag, "START rejected: worker still active"); continue; }
                    if (!g_run->reusable) { ESP_LOGW(kTag, "START rejected: cleanup not reusable; reboot required"); continue; }
                    if (c.run_id == g_run->command.run_id) { ESP_LOGW(kTag, "START rejected: use a fresh run ID"); continue; }
                    delete g_run;
                }
                g_run = new Run(c);
                if (!g_worker && xTaskCreate(Worker, "webrtc_media", 32768, nullptr, 5, &g_worker) != pdPASS) {
                    g_run->state = RunState::Failed;
                    g_run->reusable = true;
                    g_run->worker_finished = true;
                    ESP_LOGE(kTag, "FINAL run=%s state=Failed reason=task_creation no_capture=1", c.run_id.c_str());
                } else if (xQueueSend(g_run_commands, &g_run, 0) != pdTRUE) {
                    g_run->state = RunState::Failed;
                    g_run->reusable = false;
                    g_run->worker_finished = true;
                    ESP_LOGE(kTag, "FINAL run=%s state=Failed reason=dispatch_failed", c.run_id.c_str());
                } else ESP_LOGW(kTag, "PREPARING run=%s; wait for RUNNING before speech", c.run_id.c_str());
            } else if (c.kind == webrtc_media::CommandKind::Stop) {
                if (!g_run || c.run_id != g_run->command.run_id ||
                    (g_run->state.load() != RunState::Preparing && g_run->state.load() != RunState::Running)) {
                    ESP_LOGW(kTag, "STOP rejected: no matching active run"); continue;
                }
                g_run->stop_requested_us = esp_timer_get_time();
                g_run->capture_gate.Cancel();
                g_run->stop_requested = true;
                ESP_LOGW(kTag, "STOP acknowledged run=%s; cleanup pending", c.run_id.c_str());
            }
        }
        GetHAL().feedTheDog();
    }
}
#endif
