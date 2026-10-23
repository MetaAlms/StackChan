#include <sdkconfig.h>
#if CONFIG_STACKCHAN_WEBRTC_M15
#include "webrtc_m15.h"
#include "media_codec.h"
#include "bounded_media_queue.h"
#include "webrtc_transport.h"
#include "audio_service.h"
#include "board.h"
#include "hal/board/cores3_audio_codec.h"
#include <esp_heap_caps.h>
#include "hal/hal.h"
#include <algorithm>
#include <cstring>
#include <cctype>
#include <array>
#include <atomic>
#include <deque>
#include <cJSON.h>
#include <esp_log.h>
#include <esp_timer.h>

namespace {
constexpr const char* kTag = "M1.5";
#if CONFIG_STACKCHAN_WEBRTC_M15_NEAR_END_CONTROL
constexpr bool kNearEndControl = true;
#else
constexpr bool kNearEndControl = false;
#endif
extern const uint8_t zh_1_pcm[] asm("zh_1_pcm");
extern const uint32_t zh_1_pcm_length asm("zh_1_pcm_length");
struct MicFrame { std::array<int16_t, 320> pcm; uint32_t pts; };
struct Event { std::string json; uint16_t stream; };
// Each queue has one producer (AFE task or the sole peer loop) and one
// consumer (RunMediaBody). Transport joins callbacks before any future reuse.
// Long-lived services: callbacks/tasks must not outlive stack storage.
AudioService g_audio;
webrtc_transport::Transport g_transport;
webrtc_media::SpscQueue<MicFrame, 6> g_mic;
webrtc_media::SpscQueue<std::vector<uint8_t>, 8> g_down;
webrtc_media::SpscQueue<Event, 8> g_events;
std::atomic<bool> g_accept{false};
std::atomic<bool> g_disconnected{false};
std::atomic<uint32_t> g_mic_drop{0}, g_down_drop{0}, g_event_drop{0};

const cJSON* Field(const cJSON* obj, const char* name) { return cJSON_GetObjectItemCaseSensitive(obj, name); }
bool StringIs(const cJSON* obj, const char* value) { return cJSON_IsString(obj) && strcmp(obj->valuestring, value) == 0; }

std::string NormalizeTranscript(const char* text)
{
    std::string result;
    const std::string input = text ? text : "";
    for (size_t i = 0; i < input.size();) {
        bool punctuation = false;
        for (const char* mark : {"。", "，", "？", "！", "：", "；", "“", "”", "、"}) {
            const size_t bytes = strlen(mark);
            if (input.compare(i, bytes, mark) == 0) { i += bytes; punctuation = true; break; }
        }
        if (punctuation) continue;
        const unsigned char ch = input[i++];
        if (ch < 128 && (std::isspace(ch) || std::ispunct(ch))) continue;
        result.push_back(ch < 128 ? std::tolower(ch) : ch);
    }
    return result;
}

void RunMediaBody()
{
    webrtc_media::Codec codec;
    if (!codec.Open()) {
        ESP_LOGE(kTag, "codec open failed; media disabled");
        return;
    }
    std::string error;
    webrtc_transport::Callbacks cb;
    cb.on_data = [](const std::string& json, uint16_t stream) {
        if (json.size() > 4096) { ++g_event_drop; return; }
        // Delta/UI events are irrelevant to this prerequisite and can flood
        // the control queue. Retain the exact events used by its observations.
        cJSON* root = cJSON_ParseWithLength(json.data(), json.size());
        if (!root) { ++g_event_drop; return; }
        const cJSON* type = Field(root, "type");
        const bool relevant = StringIs(type, "session.created") || StringIs(type, "session.updated") ||
            StringIs(type, "input_audio_buffer.speech_started") || StringIs(type, "input_audio_buffer.speech_stopped") ||
            StringIs(type, "conversation.item.input_audio_transcription.completed") ||
            StringIs(type, "conversation.item.input_audio_transcription.failed") ||
            StringIs(type, "response.created") || StringIs(type, "response.done") ||
            StringIs(type, "response.audio.done") || StringIs(type, "response.audio_transcript.done") ||
            StringIs(type, "response.content_part.done") ||
            StringIs(type, "error");
        cJSON_Delete(root);
        if (relevant && !g_events.Push(Event{json, stream})) ++g_event_drop;
    };
    cb.on_audio = [](const uint8_t* data, int size, uint32_t) {
        if (!g_accept.load()) return;
        if (!data || size <= 0 || size > webrtc_transport::Transport::MaxPayloadBytes()) {
            ++g_down_drop;
            return;
        }
        std::vector<uint8_t> owned(data, data + size);
        if (!g_down.Push(std::move(owned))) ++g_down_drop;
    };
    cb.on_state = [](esp_peer_state_t state) {
        if (state == ESP_PEER_STATE_DISCONNECTED || state == ESP_PEER_STATE_CLOSED ||
            state == ESP_PEER_STATE_CONNECT_FAILED || state == ESP_PEER_STATE_DATA_CHANNEL_CLOSED ||
            state == ESP_PEER_STATE_DATA_CHANNEL_DISCONNECTED) g_disconnected = true;
    };
    if (!g_transport.Start({}, cb, &error)) {
        ESP_LOGE(kTag, "transport start failed: %s", error.c_str());
        g_transport.Stop();
        codec.Close();
        return;
    }
    bool ready = false;
    size_t stimulus_offset = kNearEndControl ? zh_1_pcm_length : 0;
    uint64_t playback_epoch = g_audio.WebRtcPlaybackEpoch();
    const int64_t started = esp_timer_get_time();
    int64_t next_send = 0, next_report = started;
    uint32_t sent = 0, decoded = 0, failures = 0, playback_drop = 0;
    uint32_t speech_started = 0, asr_completed = 0, responses = 0, echo_matches = 0;
    std::deque<std::string> recent_answers;
    uint32_t answer_transcripts = 0, asr_texts = 0;
    unsigned near_keywords = 0;
    auto remember_answer = [&](const cJSON* text) {
        if (!cJSON_IsString(text)) return;
        const std::string normalized = NormalizeTranscript(text->valuestring);
        if (normalized.size() < 9 || normalized.size() > 512) return;
        if (!recent_answers.empty() && recent_answers.back() == normalized) return;
        if (recent_answers.size() == 4) recent_answers.pop_front();
        recent_answers.push_back(normalized);
        ++answer_transcripts;
    };
    std::vector<uint8_t> packet;
    std::vector<int16_t> pcm;
    // This is the transport/codec/hardware prerequisite, not T1-T4 acceptance.
    while (!g_disconnected.load() && esp_timer_get_time() - started < (kNearEndControl ? 180000000 : 60000000) && !(near_keywords == 7 && asr_completed >= 3)) {
        Event event;
        if (g_events.Pop(event)) {
            cJSON* root = cJSON_ParseWithLength(event.json.data(), event.json.size());
            const cJSON* type = Field(root, "type");
            if (StringIs(type, "session.created")) {
                const std::string config = R"({"type":"session.update","session":{"modalities":["text","audio"],"turn_detection":{"type":"server_vad","threshold":0.5,"silence_duration_ms":800},"input_audio_transcription":{"model":"qwen3-asr-flash-realtime"}}})";
                if (!g_transport.SendJson(config, event.stream)) ++failures;
            } else if (StringIs(type, "session.updated")) {
                const cJSON* session = Field(root, "session");
                const cJSON* vad = Field(session, "turn_detection");
                const cJSON* trans = Field(session, "input_audio_transcription");
                const cJSON* threshold = Field(vad, "threshold");
                const cJSON* silence = Field(vad, "silence_duration_ms");
                ready = StringIs(Field(vad, "type"), "server_vad") &&
                    cJSON_IsNumber(threshold) && threshold->valuedouble == 0.5 &&
                    cJSON_IsNumber(silence) && silence->valueint == 800 &&
                    StringIs(Field(trans, "model"), "qwen3-asr-flash-realtime");
                ESP_LOGW(kTag, "session config verified=%d", ready);
                if (ready && !g_accept.exchange(true)) {
                    g_audio.EnableVoiceProcessing(true);  // once: stays on during playback
                    if (kNearEndControl) ESP_LOGW(kTag, "NEAR-END CONTROL ready: real microphone only; speaker submissions suppressed; say three test sentences");
                    else ESP_LOGW(kTag, "initial fixture stimulus, then continuous microphone (fixture ASR is not microphone ASR evidence)");
                }
            } else if (cJSON_IsString(type)) {
                // Do not log raw events (credentials/transcripts). Event names only.
                ESP_LOGI(kTag, "event %s", type->valuestring);
                if (StringIs(type, "error")) ++failures;
                if (StringIs(type, "input_audio_buffer.speech_started")) ++speech_started;
                if (StringIs(type, "response.audio_transcript.done")) {
                    remember_answer(Field(root, "transcript"));
                } else if (StringIs(type, "response.content_part.done")) {
                    const cJSON* part = Field(root, "part");
                    remember_answer(Field(part, "transcript"));
                    remember_answer(Field(part, "text"));
                } else if (StringIs(type, "response.done")) {
                    // Some gateways omit audio_transcript.done. The final
                    // response contains message content instead; generation
                    // completion still does not establish playback completion.
                    const cJSON* item = nullptr;
                    cJSON_ArrayForEach(item, Field(Field(root, "response"), "output")) {
                        const cJSON* part = nullptr;
                        cJSON_ArrayForEach(part, Field(item, "content")) {
                            remember_answer(Field(part, "transcript"));
                            remember_answer(Field(part, "text"));
                        }
                    }
                }
                if (StringIs(type, "conversation.item.input_audio_transcription.completed")) {
                    ++asr_completed;
                    const cJSON* transcript = Field(root, "transcript");
                    const std::string normalized = NormalizeTranscript(cJSON_IsString(transcript) ? transcript->valuestring : nullptr);
                    if (!normalized.empty()) ++asr_texts;
                    if (kNearEndControl) {
                        if (normalized.find("测试") != std::string::npos && normalized.find("语音") != std::string::npos && normalized.find("连接") != std::string::npos) near_keywords |= 1;
                        if (normalized.find("蓝色") != std::string::npos && normalized.find("书") != std::string::npos) near_keywords |= 2;
                        if ((normalized.find("一加一") != std::string::npos || normalized.find("1加1") != std::string::npos ||
                             (cJSON_IsString(transcript) && strstr(transcript->valuestring, "1+1"))) &&
                            normalized.find("等于") != std::string::npos) near_keywords |= 4;
                        ESP_LOGW(kTag, "NEAR-END CONTROL recognized keyword mask=%u/7 (transcript omitted)", near_keywords);
                    }
                    const auto* hardware = static_cast<CoreS3AudioCodec*>(Board::GetInstance().GetAudioCodec());
                    if (stimulus_offset == zh_1_pcm_length && hardware->WriteOk() > 0 &&
                        normalized.size() >= 9 && std::find(recent_answers.begin(), recent_answers.end(), normalized) != recent_answers.end()) {
                        ++echo_matches;
                        ESP_LOGW(kTag, "ASR text overlaps a previous answer (normalized exact match; acoustic attribution unverified; transcript omitted)");
                    }
                }
                if (StringIs(type, "response.created")) ++responses;
            }
            cJSON_Delete(root);
        }
        const int64_t now = esp_timer_get_time();
        MicFrame mic;
        if (ready && now >= next_send && g_mic.Pop(mic)) {
            if (next_send == 0 || now - next_send > 60000) next_send = now;
            next_send += 20000;  // absolute pacing; codec/task time must not accumulate drift
            // The initial known fixture elicits one deterministic server response.
            // Capture remains active; once finished, all subsequent PCM is microphone.
            if (stimulus_offset < zh_1_pcm_length) {
                mic.pcm.fill(0);
                const size_t bytes = std::min(sizeof(mic.pcm), size_t(zh_1_pcm_length) - stimulus_offset);
                memcpy(mic.pcm.data(), zh_1_pcm + stimulus_offset, bytes);
                stimulus_offset += bytes;
                if (stimulus_offset == zh_1_pcm_length) ESP_LOGW(kTag, "fixture ended; all uplink PCM now from microphone");
            }
            if (!codec.Encode20ms(mic.pcm.data(), mic.pcm.size(), packet)) ++failures;
            else if (!packet.empty()) {
                if (g_transport.SendAudio(packet.data(), packet.size(), mic.pts)) ++sent;
                else ++failures;
            }
        }
        if (ready && g_down.Pop(packet)) {
            if (!codec.Decode(packet.data(), packet.size(), pcm)) ++failures;
            else {
                ++decoded;
                if (!kNearEndControl && !pcm.empty() && !g_audio.PushWebRtcPcm(std::move(pcm), playback_epoch)) ++playback_drop;
            }
        }
        if (now >= next_report) {
            ESP_LOGW(kTag, "prerequisite sent=%lu decoded=%lu failures=%lu mic_drop=%lu rx_drop=%lu playback_drop=%lu event_drop=%lu",
                (unsigned long)sent, (unsigned long)decoded, (unsigned long)failures,
                (unsigned long)g_mic_drop.load(), (unsigned long)g_down_drop.load(),
                (unsigned long)playback_drop, (unsigned long)g_event_drop.load());
            ESP_LOGW(kTag, "observations speech_started=%lu asr_completed=%lu responses=%lu answer_transcripts=%lu asr_texts=%lu echo_matches=%lu",
                (unsigned long)speech_started, (unsigned long)asr_completed, (unsigned long)responses, (unsigned long)answer_transcripts, (unsigned long)asr_texts, (unsigned long)echo_matches);
            auto* hardware = static_cast<CoreS3AudioCodec*>(Board::GetInstance().GetAudioCodec());
            ESP_LOGW(kTag, "hardware read_ok=%lu read_failed=%lu write_ok=%lu write_failed=%lu capture_cvt_failed=%lu dram=%u stack=%u",
                (unsigned long)hardware->ReadOk(), (unsigned long)hardware->ReadFailed(),
                (unsigned long)hardware->WriteOk(), (unsigned long)hardware->WriteFailed(),
                (unsigned long)g_audio.WebRtcCaptureConversionFailed(),
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                (unsigned)uxTaskGetStackHighWaterMark(nullptr));
            next_report = now + 5000000;
        }
        vTaskDelay(1);
    }
    if (kNearEndControl) ESP_LOGW(kTag, "NEAR-END CONTROL ended: keyword mask=%u/7", near_keywords);
    g_accept = false;
    g_audio.StopWebRtcPlayback();
    g_audio.EnableVoiceProcessing(false);
    g_transport.Stop();  // same worker owns all sends; joins peer before codec closes
    codec.Close();
    if (kNearEndControl) ESP_LOGW(kTag, "bounded probe ended; T1 keyword observations only; T2-T4 NOT RUN");
    else ESP_LOGW(kTag, "bounded probe ended; T1-T4 NOT RUN; no AEC/interruption verdict");
}

void RunMedia(void*)
{
    RunMediaBody();  // destroy owning C++ locals before deleting the FreeRTOS task
    vTaskDelete(nullptr);
}
}  // namespace

void WebRtcM15Run()
{
    g_audio.Initialize(Board::GetInstance().GetAudioCodec());
    g_audio.SetWebRtcPcmSink([](std::vector<int16_t>&& data) {
        static uint32_t pts = 0;
        if (!g_accept.load()) return;
        // AFE is configured for 60ms. Do not silently drop a partial frame.
        if (data.empty() || data.size() % 320) { ++g_mic_drop; return; }
        for (size_t offset = 0; offset < data.size(); offset += 320) {
            MicFrame frame;
            std::copy_n(data.data() + offset, 320, frame.pcm.data());
            frame.pts = pts;
            pts += 20;  // dropped frames still advance the capture media clock
            if (!g_mic.Push(std::move(frame))) ++g_mic_drop;
        }
    });
    g_audio.Start();
    if (xTaskCreate(RunMedia, "webrtc_media", 32768, nullptr, 5, nullptr) != pdPASS) {
        ESP_LOGE(kTag, "media task creation failed");
    }
    while (true) { GetHAL().feedTheDog(); GetHAL().delay(1000); }
}
#endif
