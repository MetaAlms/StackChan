#include <sdkconfig.h>
#if CONFIG_STACKCHAN_WEBRTC_M15
#include "webrtc_m15.h"
#include "media_codec.h"
#include "media_admission.h"
#include "bounded_media_queue.h"
#include "console_command.h"
#include "channel_audit.h"
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
enum class TurnKind : uint8_t { SpeechStarted, SpeechStopped, Committed, ResponseCreated, ResponseDone, AsrAccepted };
enum class TagState : uint8_t { Missing, Valid, Invalid };
struct FieldTag { uint32_t value = 0; TagState state = TagState::Missing; };
constexpr const char* kTurnNames[] = {"speech_started", "speech_stopped", "committed", "response_created", "response_done", "asr_accepted"};
constexpr const char* kTagStates[] = {"missing", "valid", "invalid"};
constexpr const char* kDoneStatuses[] = {"not_applicable", "missing", "completed", "cancelled", "failed", "incomplete", "other"};
constexpr const char* kDetailValues[] = {"not_applicable", "missing", "other", "turn_detected", "client_cancelled", "max_output_tokens",
    "content_filter", "error", "server_error", "invalid_request_error", "interrupted", "cancelled", "failed", "incomplete"};
struct TurnEvent {
    int64_t received_us = 0;
    FieldTag item, response;
    uint32_t detail_type_tag = 0, detail_reason_tag = 0;
    TurnKind kind = TurnKind::SpeechStarted;
    uint8_t done_status = 0, detail_type = 0, detail_reason = 0;
};
struct TurnRecord {
    int64_t received_ms = 0, snapshot_ms = 0, last_submit_us = 0;
    uint64_t submitted_samples = 0, submitted_calls = 0;
    uint32_t sent = 0, decoded = 0;
    FieldTag item, response;
    uint32_t detail_type_tag = 0, detail_reason_tag = 0;
    TurnKind kind = TurnKind::SpeechStarted;
    uint8_t done_status = 0, detail_type = 0, detail_reason = 0;
};

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
    webrtc_media::SpscQueue<TurnEvent, 8> diagnostics;
    std::atomic<uint32_t> diagnostic_dropped{0};
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
    uint32_t stop_discard_mic = 0, stop_discard_down = 0, stop_discard_event = 0, stop_discard_diagnostic = 0;
    uint32_t speech = 0, responses = 0, asr_failed = 0;
    std::array<TurnRecord, 64> turns{};  // Worker only; printed after media stops.
    size_t turn_count = 0;
    uint32_t turn_overflow = 0;
    int create_response_observed = -1, interrupt_response_observed = -1;
    std::array<uint32_t, 6> playback_admission{};
    uint64_t accepted_pcm24_samples = 0;
    uint32_t first_tx_pts = 0, last_tx_pts = 0, first_rx_pts = 0, last_rx_pts = 0;
    int64_t first_rx_us = 0, last_rx_us = 0, last_send_us = 0, max_send_gap_us = 0, max_mic_age_us = 0;
    int64_t run_stopped_us = 0;
    // Duplex+AEC slowdown instrumentation: time inside the peer send call vs
    // the whole loop iteration separates blocking from scheduling.
    uint64_t send_call_total_us = 0;
    uint32_t send_call_max_us = 0, send_calls = 0, mic_empty = 0;
    int64_t loop_max_us = 0;
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
uint32_t DiagnosticTag(const char* value) {
    uint32_t tag = 2166136261u;
    for (const unsigned char* p = reinterpret_cast<const unsigned char*>(value); *p; ++p) tag = (tag ^ *p) * 16777619u;
    return tag;  // FNV32 may collide; this never assigns RTP to a response.
}
FieldTag ReadFieldTag(const cJSON* field) {
    if (!field || cJSON_IsNull(field)) return {};
    if (!cJSON_IsString(field) || !field->valuestring[0] || strlen(field->valuestring) > 128) return {0, TagState::Invalid};
    return {DiagnosticTag(field->valuestring), TagState::Valid};
}
uint8_t ReadDoneStatus(const cJSON* field) {
    if (!cJSON_IsString(field) || !field->valuestring[0]) return 1;
    for (uint8_t i = 2; i < 6; ++i) if (StringIs(field, kDoneStatuses[i])) return i;
    return 6;
}
uint8_t ReadDetail(const cJSON* field, uint32_t& unknown_tag) {
    if (!field || cJSON_IsNull(field)) return 1;
    if (!cJSON_IsString(field)) return 2;
    for (uint8_t i = 3; i < sizeof(kDetailValues) / sizeof(kDetailValues[0]); ++i) if (StringIs(field, kDetailValues[i])) return i;
    unknown_tag = DiagnosticTag(field->valuestring);  // Never print arbitrary detail strings.
    return 2;
}
int ObserveBool(const cJSON* field) { return cJSON_IsTrue(field) ? 1 : cJSON_IsFalse(field) ? 0 : -1; }
TurnEvent MakeTurnEvent(TurnKind kind, int64_t received_us, const cJSON* root) {
    TurnEvent t;
    t.kind = kind;
    t.received_us = received_us;
    const cJSON* response = Field(root, "response");
    const cJSON* response_id = Field(root, "response_id");
    t.response = ReadFieldTag(response_id ? response_id : Field(response, "id"));
    const cJSON* item_id = Field(root, "item_id");
    t.item = ReadFieldTag(item_id ? item_id : Field(Field(root, "item"), "id"));
    if (kind == TurnKind::ResponseDone) {
        t.done_status = ReadDoneStatus(Field(response, "status"));
        const cJSON* details = Field(response, "status_details");
        t.detail_type = ReadDetail(Field(details, "type"), t.detail_type_tag);
        t.detail_reason = ReadDetail(Field(details, "reason"), t.detail_reason_tag);
    }
    return t;
}
void RecordTurn(Run& r, const TurnEvent& e) {
    if (r.turn_count == r.turns.size()) { ++r.turn_overflow; return; }
    auto& t = r.turns[r.turn_count++];
    t.kind = e.kind;
    t.received_ms = (e.received_us - r.control.run_started_us()) / 1000;
    t.item = e.item;
    t.response = e.response;
    t.done_status = e.done_status;
    t.detail_type = e.detail_type;
    t.detail_reason = e.detail_reason;
    t.detail_type_tag = e.detail_type_tag;
    t.detail_reason_tag = e.detail_reason_tag;
    const auto p = g_audio.GetWebRtcPlaybackSnapshot();
    t.submitted_samples = p.submitted_samples - r.initial_playback.submitted_samples;
    t.submitted_calls = p.submitted_calls - r.initial_playback.submitted_calls;
    t.last_submit_us = t.submitted_calls ? p.last_submit_finished_us : 0;
    t.sent = r.sent;
    t.decoded = r.decoded;
    // Processing time, not the event receive time; independent atomic reads
    // are an approximate API snapshot, not an acoustic playback boundary.
    t.snapshot_ms = (esp_timer_get_time() - r.control.run_started_us()) / 1000;
}
void RecordTurn(Run& r, const Event& e, TurnKind kind, const cJSON* root) {
    RecordTurn(r, MakeTurnEvent(kind, e.received_us, root));
}
void ReportTurns(const Run& r) {
    ESP_LOGW(kTag, "SESSION_OBS run=%s create_response=%d interrupt_response=%d unknown=-1 observational_only=1",
        r.command.run_id.c_str(), r.create_response_observed, r.interrupt_response_observed);
    ESP_LOGW(kTag, "TURN_SUMMARY run=%s stored=%u capacity=%u overflow=%lu diagnostic_dropped=%lu diagnostic_stop_discard=%lu queue_order=independent_not_receive_order tags=FNV32_diagnostic_only api_snapshot_atomic=0 physical_end_unknown=1",
        r.command.run_id.c_str(), (unsigned)r.turn_count, (unsigned)r.turns.size(), (unsigned long)r.turn_overflow,
        (unsigned long)r.diagnostic_dropped.load(), (unsigned long)r.stop_discard_diagnostic);
    for (size_t i = 0; i < r.turn_count; ++i) {
        const auto& t = r.turns[i];
        ESP_LOGW(kTag, "TURN run=%s n=%u kind=%s receive_ms=%s snapshot_ms=%s item_tag=%08lx item_id=%s response_tag=%08lx response_id=%s sent=%lu decoded=%lu samples=%s calls=%s last_submit_us=%s status=%s detail_type=%s type_tag=%08lx detail_reason=%s reason_tag=%08lx",
            r.command.run_id.c_str(), (unsigned)(i + 1), kTurnNames[static_cast<size_t>(t.kind)],
            SignedDecimal(t.received_ms).c_str(), SignedDecimal(t.snapshot_ms).c_str(),
            (unsigned long)t.item.value, kTagStates[static_cast<size_t>(t.item.state)],
            (unsigned long)t.response.value, kTagStates[static_cast<size_t>(t.response.state)],
            (unsigned long)t.sent, (unsigned long)t.decoded, UnsignedDecimal(t.submitted_samples).c_str(),
            UnsignedDecimal(t.submitted_calls).c_str(), SignedDecimal(t.last_submit_us).c_str(),
            kDoneStatuses[t.done_status], kDetailValues[t.detail_type], (unsigned long)t.detail_type_tag,
            kDetailValues[t.detail_reason], (unsigned long)t.detail_reason_tag);
    }
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
        const int64_t received_us = esp_timer_get_time();
        if (StringIs(t, "input_audio_buffer.speech_stopped") || StringIs(t, "input_audio_buffer.committed") || StringIs(t, "response.done")) {
            const TurnKind kind = StringIs(t, "input_audio_buffer.speech_stopped") ? TurnKind::SpeechStopped :
                StringIs(t, "input_audio_buffer.committed") ? TurnKind::Committed : TurnKind::ResponseDone;
            // Optional observations cannot occupy the essential-event queue
            // or turn diagnostic congestion into a media/session failure.
            auto observation = MakeTurnEvent(kind, received_us, root);
            if (!r.diagnostics.Push(std::move(observation))) ++r.diagnostic_dropped;
            cJSON_Delete(root);
            return;
        }
        const bool relevant = StringIs(t, "session.created") || StringIs(t, "session.updated") ||
            StringIs(t, "input_audio_buffer.speech_started") ||
            StringIs(t, "conversation.item.input_audio_transcription.completed") ||
            StringIs(t, "conversation.item.input_audio_transcription.failed") ||
            StringIs(t, "response.created") || StringIs(t, "error");
        cJSON_Delete(root);
        if (relevant && !r.events.Push(Event{json, stream, received_us})) ++r.event_full;
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
        r.create_response_observed = ObserveBool(Field(vad, "create_response"));
        r.interrupt_response_observed = ObserveBool(Field(vad, "interrupt_response"));
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
        if (StringIs(t, "input_audio_buffer.speech_started")) { ++r.speech; RecordTurn(r, e, TurnKind::SpeechStarted, root); }
        if (StringIs(t, "response.created")) { ++r.responses; RecordTurn(r, e, TurnKind::ResponseCreated, root); }
        if (StringIs(t, "conversation.item.input_audio_transcription.failed")) ++r.asr_failed;
        if (StringIs(t, "conversation.item.input_audio_transcription.completed")) {
            const cJSON* id = Field(root, "item_id");
            const auto observation = r.control.ObserveAsr(r.command.run_id,
                cJSON_IsString(id) ? id->valuestring : "", KeywordBits(Field(root, "transcript")));
            if (observation.status == webrtc_media::AsrStatus::Accepted) RecordTurn(r, e, TurnKind::AsrAccepted, root);
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
    const uint64_t send_avg_us = r.send_calls ? r.send_call_total_us / r.send_calls : 0;
    ESP_LOGW(kTag, "TIMING run=%s send_calls=%lu send_avg_us=%s send_max_us=%s loop_max_us=%s mic_empty=%lu",
        r.command.run_id.c_str(), (unsigned long)r.send_calls, UnsignedDecimal(send_avg_us).c_str(),
        UnsignedDecimal(r.send_call_max_us).c_str(), SignedDecimal(r.loop_max_us).c_str(),
        (unsigned long)r.mic_empty);
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
        const auto& d = r.codec.downlink_stats();
        ESP_LOGW(kTag, "PCM run=%s decoded48=%s converted24=%s accepted24=%s submitted24=%s max_abs=%lu endpoints=%s endpoints_are_not_distortion_proof=1",
            r.command.run_id.c_str(), UnsignedDecimal(d.decoded_samples_48k).c_str(),
            UnsignedDecimal(d.converted_samples_24k).c_str(), UnsignedDecimal(r.accepted_pcm24_samples).c_str(),
            UnsignedDecimal(p.submitted_samples).c_str(), (unsigned long)d.pcm24_max_abs,
            UnsignedDecimal(d.pcm24_endpoint_samples).c_str());
        ESP_LOGW(kTag, "OPUS_SAMPLES run=%s n120=%s n240=%s n480=%s n960=%s n1920=%s n2880=%s n5760=%s other=%s",
            r.command.run_id.c_str(), UnsignedDecimal(d.decoded_samples_histogram[0]).c_str(),
            UnsignedDecimal(d.decoded_samples_histogram[1]).c_str(), UnsignedDecimal(d.decoded_samples_histogram[2]).c_str(),
            UnsignedDecimal(d.decoded_samples_histogram[3]).c_str(), UnsignedDecimal(d.decoded_samples_histogram[4]).c_str(),
            UnsignedDecimal(d.decoded_samples_histogram[5]).c_str(), UnsignedDecimal(d.decoded_samples_histogram[6]).c_str(),
            UnsignedDecimal(d.decoded_samples_histogram[7]).c_str());
        ReportTurns(r);
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
        const int64_t loop_start_us = esp_timer_get_time();
        // Poll before admitting events: delayed evidence cannot extend the window.
        Event e;
        if (r.events.Pop(e)) HandleEvent(r, e);
        if (!Continue(r)) break;
        BeginCaptureWindow(r);
        if (!Continue(r)) break;
        // Separate queues deliberately prioritize essential events. Trace
        // insertion order may differ from receive order; compare receive_ms,
        // while each API snapshot belongs only to its own processing time.
        TurnEvent diagnostic;
        if (r.diagnostics.Pop(diagnostic) && r.control.state() == RunState::Running &&
            diagnostic.received_us >= r.control.run_started_us()) RecordTurn(r, diagnostic);
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
            bool have_mic = false;
            if (now >= next_send_us) {
                have_mic = r.mic.Pop(mic);
                if (!have_mic) ++r.mic_empty;
            }
            if (have_mic) {
                r.max_mic_age_us = std::max(r.max_mic_age_us, now - mic.afe_emitted_us);
                if (next_send_us == 0 || now - next_send_us > 60000) next_send_us = now;
                next_send_us += 20000;
                if (!r.codec.Encode20ms(mic.pcm.data(), mic.pcm.size(), encoded)) {
                    ++r.failures; Stop(r, StopReason::Error); break;
                }
                if (!Continue(r)) break;
                const int64_t send_called_us = esp_timer_get_time();
                bool send_ok = true;
                if (!encoded.empty()) {
                    const int64_t send_enter_us = esp_timer_get_time();
                    send_ok = r.transport.SendAudio(encoded.data(), encoded.size(), mic.pts_ms);
                    const int64_t send_dt_us = esp_timer_get_time() - send_enter_us;
                    ++r.send_calls;
                    r.send_call_total_us += (uint64_t)send_dt_us;
                    if (send_dt_us > (int64_t)r.send_call_max_us) r.send_call_max_us = (uint32_t)send_dt_us;
                }
                if (!send_ok) {
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
                    const size_t samples = pcm.size();
                    const auto result = g_audio.PushWebRtcPcm(std::move(pcm), playback_epoch);
                    ++r.playback_admission[static_cast<size_t>(result)];
                    if (result == AudioService::WebRtcPcmAdmission::Accepted) r.accepted_pcm24_samples += samples;
                }
            }
        }
        if (now >= next_report_us) {
            // Full reports block this sender on the 115200-baud console long
            // enough to build a persistent mic backlog. Keep live I/O short;
            // Report below emits all statistics after media has stopped.
            if (r.control.state() == RunState::Running) {
                ESP_LOGW(kTag, "LIVE run=%s sent=%lu decoded=%lu mic_full=%lu failures=%lu",
                    r.command.run_id.c_str(), (unsigned long)r.sent, (unsigned long)r.decoded,
                    (unsigned long)r.mic_full.load(), (unsigned long)r.failures);
            } else Report(r, false);
            next_report_us = now + 5000000;
        }
        const int64_t loop_dt_us = esp_timer_get_time() - loop_start_us;
        if (loop_dt_us > r.loop_max_us) r.loop_max_us = loop_dt_us;
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
    TurnEvent discarded_diagnostic;
    while (r.mic.Pop(discarded_mic)) ++r.stop_discard_mic;
    while (r.down.Pop(discarded_down)) ++r.stop_discard_down;
    while (r.events.Pop(discarded_event)) ++r.stop_discard_event;
    while (r.diagnostics.Pop(discarded_diagnostic)) ++r.stop_discard_diagnostic;
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

// Local channel audit: play a known tone and measure how much of it reaches
// the current two-slot uplink. No network, no AFE, no AEC. Guard slots keep
// DMA latency from smearing phase attribution.
void RunChannelAudit(uint32_t seconds) {
    if (g_run && !g_run->worker_finished.load()) {
        ESP_LOGW(kTag, "AUDIT rejected: media run active");
        return;
    }
    // AudioService creates its periodic idle power timer on Initialize(); after
    // the first run it may close the codec 15 s after the last service-level IO
    // and would race a local audit that bypasses AudioService. Require a clean
    // boot so the timer does not exist.
    if (g_audio_initialized) {
        ESP_LOGW(kTag, "AUDIT rejected: audio service initialized; reboot for a clean local audit");
        return;
    }
    auto* codec = static_cast<CoreS3AudioCodec*>(Board::GetInstance().GetAudioCodec());
    if (codec == nullptr) {
        ESP_LOGE(kTag, "AUDIT rejected: codec unavailable");
        return;
    }
    constexpr uint32_t kSampleRate = 24000;
    constexpr size_t kFrameSamples = 480;  // 20 ms at 24 kHz.
    constexpr size_t kReadSamples = 480;   // 240 stereo frames = 10 ms.
    constexpr uint32_t kSlotsPerSecond = 50;
    // 200 ms guard: the RX/TX DMA path is 6 x 240 frames (~60 ms each way),
    // so a shorter guard can smear the tone into the silence denominator.
    constexpr uint32_t kGuardSlots = 10;
    constexpr uint32_t kDrainSlots = 10;  // pre-roll: discard stale DMA audio
    constexpr uint32_t kMaxFailures = 10;
    const uint32_t slots = seconds * kSlotsPerSecond;
    if (slots < 3 * kGuardSlots + 3) {
        ESP_LOGE(kTag, "AUDIT rejected: window too short");
        return;
    }
    const uint32_t phase = (slots - 3 * kGuardSlots) / 3;
    const uint32_t tone1_begin = kGuardSlots, tone1_end = tone1_begin + phase;
    const uint32_t silent_begin = tone1_end + kGuardSlots, silent_end = silent_begin + phase;
    const uint32_t tone2_begin = silent_end + kGuardSlots;
    const uint32_t tone2_end = std::min(slots, tone2_begin + phase);

    // Do not touch the persisted volume here: AudioCodec::SetOutputVolume()
    // writes NVS, and output_volume() in RAM is not the stored value. The
    // tone amplitude is reduced instead, so the user's setting is untouched.
    codec->EnableOutput(true);
    codec->EnableInput(true);
    webrtc_media::ChannelAuditStats tone1, silent, tone2;
    tone1.Configure(2);
    silent.Configure(2);
    tone2.Configure(2);
    webrtc_media::ToneGenerator tone(1000.0, 1024.0, kSampleRate);  // ~-30 dBFS
    // Heap-backed: the main task stack is 8 KB and already carries the console
    // and logging paths. These buffers are only alive for the bounded audit.
    std::vector<int16_t> out(kFrameSamples);
    std::vector<int16_t> in(kReadSamples);
    uint32_t write_fail = 0, read_fail = 0;
    bool aborted = false;
    const int64_t deadline_us = esp_timer_get_time() + (static_cast<int64_t>(seconds) + 2) * 1000000;
    // I2S reads/writes block and yield, so the idle-task TWDT stays fed without
    // Hal::feedTheDog() (which is vTaskDelay(1) = 10 ms at 100 Hz and would
    // stretch every 20 ms slot). Drain the DMA path before the measured phases.
    // The deadline is a soft bound: a single I2S call can block up to 1 s, so
    // it is checked before every blocking call, including the drain pre-roll.
    for (uint32_t i = 0; i < kDrainSlots && !aborted; ++i) {
        if (esp_timer_get_time() >= deadline_us) {
            aborted = true;
            break;
        }
        std::memset(out.data(), 0, out.size() * sizeof(int16_t));
        if (codec->AuditWrite(out.data(), static_cast<int>(kFrameSamples)) != static_cast<int>(kFrameSamples)) ++write_fail;
        for (int half = 0; half < 2; ++half) {
            if (codec->AuditRead(in.data(), static_cast<int>(kReadSamples)) != static_cast<int>(kReadSamples)) ++read_fail;
        }
        if (write_fail + read_fail > kMaxFailures) aborted = true;
    }
    for (uint32_t slot = 0; slot < slots && !aborted; ++slot) {
        if (esp_timer_get_time() >= deadline_us) {
            aborted = true;
            break;
        }
        webrtc_media::ChannelAuditStats* target = nullptr;
        bool play_tone = false;
        if (slot >= tone1_begin && slot < tone1_end) {
            target = &tone1;
            play_tone = true;
        } else if (slot >= silent_begin && slot < silent_end) {
            target = &silent;
        } else if (slot >= tone2_begin && slot < tone2_end) {
            target = &tone2;
            play_tone = true;
        }
        if (play_tone) tone.Fill(out.data(), kFrameSamples);
        else std::memset(out.data(), 0, out.size() * sizeof(int16_t));
        if (codec->AuditWrite(out.data(), static_cast<int>(kFrameSamples)) != static_cast<int>(kFrameSamples)) ++write_fail;
        for (int half = 0; half < 2; ++half) {
            const int got = codec->AuditRead(in.data(), static_cast<int>(kReadSamples));
            if (got != static_cast<int>(kReadSamples)) {
                ++read_fail;
                continue;
            }
            if (target == nullptr) continue;
            for (size_t f = 0; f + 2 <= static_cast<size_t>(got); f += 2) target->AddFrame(&in[f]);
        }
        if (write_fail + read_fail > kMaxFailures) {
            aborted = true;
            break;
        }
    }
    codec->EnableInput(false);
    codec->EnableOutput(false);

    ESP_LOGW(kTag, "AUDIT seconds=%u slots=%u write_fail=%u read_fail=%u aborted=%d volume=unchanged tone_amp=1024",
        (unsigned)seconds, (unsigned)slots, (unsigned)write_fail, (unsigned)read_fail, aborted ? 1 : 0);
    if (tone1.samples[0] == 0) {
        ESP_LOGW(kTag, "AUDIT aborted before measurement; no per-phase statistics");
        return;
    }
    const struct { const char* name; const webrtc_media::ChannelAuditStats* stats; } phases[] = {
        {"tone1", &tone1}, {"silence", &silent}, {"tone2", &tone2},
    };
    for (const auto& p : phases) {
        if (p.stats->samples[0] == 0) {
            ESP_LOGW(kTag, "AUDIT %s no samples", p.name);
            continue;
        }
        ESP_LOGW(kTag, "AUDIT %s frames=%s ch0_db10=%d ch0_peak=%u ch1_db10=%d ch1_peak=%u",
            p.name, UnsignedDecimal(p.stats->samples[0]).c_str(),
            webrtc_media::ChannelAuditStats::Dbfs10(*p.stats, 0), (unsigned)p.stats->peak[0],
            webrtc_media::ChannelAuditStats::Dbfs10(*p.stats, 1), (unsigned)p.stats->peak[1]);
    }
    if (tone1.samples[0] > 0 && silent.samples[0] > 0) {
        const int delta0 = webrtc_media::ChannelAuditStats::Dbfs10(tone1, 0) -
            webrtc_media::ChannelAuditStats::Dbfs10(silent, 0);
        const int delta1 = webrtc_media::ChannelAuditStats::Dbfs10(tone1, 1) -
            webrtc_media::ChannelAuditStats::Dbfs10(silent, 1);
        ESP_LOGW(kTag, "AUDIT echo_delta_db10 ch0=%d ch1=%d aec=undetermined local_only=1",
            delta0, delta1);
    } else {
        ESP_LOGW(kTag, "AUDIT echo_delta unavailable: incomplete phases");
    }
}

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
    ESP_LOGW(kTag, "IDLE; USB commands: START <run_id> near|duplex <5..180 seconds>, STOP <run_id>, STATUS, AUDIT <3..10 seconds>; fresh run ID for each start");
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
            } else if (c.kind == webrtc_media::CommandKind::Audit) {
                if (g_run && !g_run->worker_finished.load()) {
                    ESP_LOGW(kTag, "AUDIT rejected: worker still active");
                    continue;
                }
                RunChannelAudit(c.audit_seconds);
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
