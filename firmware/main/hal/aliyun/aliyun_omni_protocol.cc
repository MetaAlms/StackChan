/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#include "aliyun_omni_protocol.h"

#include <application.h>
#include <board.h>
#include <settings.h>
#include <audio/audio_service.h>
#include <esp_log.h>
#include <esp_timer.h>
#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <mbedtls/base64.h>
#include <cJSON.h>
#include <cstring>
#include <mooncake_log.h>
#include <wifi_manager.h>

#define TAG "AliyunOmni"

namespace {

/* -------------------------------------------------------------- constants */

// Aliyun's public Realtime endpoint. Workspace-scoped domains look like
// wss://{WorkspaceId}.cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime and are
// selected by setting the workspace id in settings.
constexpr const char* kDefaultUrl = "wss://dashscope.aliyuncs.com/api-ws/v1/realtime";
constexpr const char* kDefaultModel = "qwen3.8-omni-flash-realtime";
constexpr const char* kDefaultVoice = "Tina";
constexpr const char* kSettingsNamespace = "aliyun";

// The ES7210 array captures at 16 kHz mono, which is exactly what the API
// wants, so no resampling is needed on the uplink.
constexpr int kInputSampleRate = 16000;
constexpr int kOutputSampleRate = 24000;

// Must match OPUS_FRAME_DURATION_MS in xiaozhi's audio_service.h. Aliyun
// accepts 10/20/40/60/100/120 ms for Opus, and 60 is what the encoder produces.
constexpr int kOpusFrameMs = 60;

// Aliyun terminates any session after 120 minutes. Recycle well before that so
// the reconnect happens on our terms rather than mid-sentence.
constexpr int64_t kMaxSessionUs = 115LL * 60 * 1000 * 1000;

constexpr EventBits_t kBitSessionReady = BIT0;

/* ------------------------------------------------------------- emotion tags */

// Mirrors tools/aliyun_omni/protocol.py, which asserts these strings against
// StackChanAvatarDisplay::SetEmotion(). "doubtful" not "doubt": the display's
// unknown-emotion branch would silently reset the face to neutral.
struct EmotionAlias {
    const char* tag;
    const char* emotion;
};

const EmotionAlias kEmotionAliases[] = {
    {"neutral", "neutral"},   {"calm", "neutral"},        {"normal", "neutral"},
    {"happy", "happy"},       {"joy", "happy"},           {"joyful", "happy"},
    {"smile", "happy"},       {"smiling", "happy"},       {"glad", "happy"},
    {"cheerful", "happy"},    {"excited", "happy"},       {"love", "happy"},
    {"laughing", "laughing"}, {"laugh", "laughing"},      {"lol", "laughing"},
    {"giggle", "laughing"},   {"amused", "laughing"},     {"angry", "angry"},
    {"anger", "angry"},       {"mad", "angry"},           {"annoyed", "angry"},
    {"furious", "angry"},     {"sad", "sad"},             {"sadness", "sad"},
    {"unhappy", "sad"},       {"down", "sad"},            {"disappointed", "sad"},
    {"crying", "crying"},     {"cry", "crying"},          {"tears", "crying"},
    {"sobbing", "crying"},    {"doubtful", "doubtful"},   {"doubt", "doubtful"},
    {"confused", "doubtful"}, {"puzzled", "doubtful"},    {"thinking", "doubtful"},
    {"curious", "doubtful"},  {"sleepy", "sleepy"},       {"tired", "sleepy"},
    {"sleep", "sleepy"},      {"bored", "sleepy"},        {"yawn", "sleepy"},
};

constexpr size_t kMaxTagBody = 20;    // longest single-word tag we accept
constexpr size_t kMaxTagLen = kMaxTagBody + 2;  // "[" + body + "]"

const char* emotion_for_tag(const std::string& tag)
{
    for (const auto& alias : kEmotionAliases) {
        if (tag == alias.tag) {
            return alias.emotion;
        }
    }
    return nullptr;
}

bool is_tag_char(char c)
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

/**
 * @brief Normalise a tag body the same way the Python reference does.
 *
 * Mirrors emotion_for() in tools/aliyun_omni/protocol.py, which applies
 * .strip().lower(). Without the strip, a model that emits "[ happy ]" would
 * fall through to the unknown-tag branch and the expression would never fire.
 */
std::string normalise_tag(const std::string& s)
{
    size_t begin = 0;
    size_t end = s.size();
    while (begin < end && (s[begin] == ' ' || s[begin] == '\t')) {
        ++begin;
    }
    while (end > begin && (s[end - 1] == ' ' || s[end - 1] == '\t')) {
        --end;
    }

    std::string out = s.substr(begin, end - begin);
    for (auto& c : out) {
        if (c >= 'A' && c <= 'Z') {
            c = static_cast<char>(c - 'A' + 'a');
        }
    }
    return out;
}

/**
 * @brief Incremental transcript scanner for emotion tags.
 *
 * Same contract as advance_emotion_events() in tools/aliyun_omni/protocol.py,
 * which is the reference implementation and is unit tested there:
 *
 *   - `new_text` is an increment, never a repeat of earlier output
 *   - a delta may end mid-tag; the fragment is held back and reported once a
 *     later delta completes it, so "[hap" never reaches the screen
 *   - each tag fires exactly once
 *
 * Re-parsing the whole transcript on every delta is deliberate. Deltas can
 * re-split the final text segment, so offsets into it are not stable; comparing
 * the settled text against what was already sent is. At a few deltas per turn
 * the cost is irrelevant.
 */
struct TranscriptScanner {
    std::string raw;
    std::string sent_text;
    size_t fired = 0;

    std::string consume(const std::string& delta, std::vector<std::string>& fired_emotions)
    {
        raw += delta;

        std::string text_segments;
        size_t emotion_count = 0;

        // `emit_end` is exclusive: text before it is settled and safe to show.
        // Advance it only at points that can never be re-interpreted later.
        size_t emit_end = 0;

        while (true) {
            const size_t open = raw.find('[', emit_end);
            if (open == std::string::npos) {
                text_segments.append(raw, emit_end, std::string::npos);
                emit_end = raw.size();
                break;
            }

            const size_t close = raw.find(']', open + 1);
            if (close == std::string::npos) {
                const std::string inner = raw.substr(open + 1);

                // Tags never contain whitespace, so a space disqualifies the
                // fragment and it becomes ordinary text again.
                bool candidate = inner.size() <= kMaxTagBody;
                for (char c : inner) {
                    if (!is_tag_char(c)) {
                        candidate = false;
                        break;
                    }
                }
                if (candidate) {
                    // Hold back from THIS bracket, even if an earlier one was
                    // already disqualified, so a bracket can never flash on
                    // screen. Matches _scan_streaming() in tools/aliyun_omni.
                    text_segments.append(raw, emit_end, open - emit_end);
                    emit_end = open;
                    break;
                }

                // Disqualified: the "[" can never become a tag now, so show it
                // and resume scanning after it.
                text_segments.append(raw, emit_end, open - emit_end + 1);
                emit_end = open + 1;
                continue;
            }

            const std::string body = raw.substr(open + 1, close - open - 1);
            const char* emotion = body.empty() ? nullptr : emotion_for_tag(normalise_tag(body));

            if (emotion == nullptr) {
                // Not an emotion we know: leave it visible, keep scanning after it.
                text_segments.append(raw, emit_end, close - emit_end + 1);
                emit_end = close + 1;
                continue;
            }

            text_segments.append(raw, emit_end, open - emit_end);
            emit_end = close + 1;
            ++emotion_count;
            if (emotion_count > fired) {
                fired_emotions.emplace_back(emotion);
            }
        }

        std::string new_text;
        if (text_segments.compare(0, sent_text.size(), sent_text) == 0) {
            new_text = text_segments.substr(sent_text.size());
        } else {
            // A held-back fragment was released, so settled text is no longer an
            // extension of what was sent. Emit it whole rather than appending
            // blindly, which would duplicate the already-visible prefix.
            new_text = text_segments;
        }

        sent_text = text_segments;
        fired = emotion_count;
        return new_text;
    }
};

/* ------------------------------------------------------------------ base64 */

std::string base64_encode(const uint8_t* data, size_t len)
{
    size_t out_len = 0;
    // First call with a null buffer to learn the encoded size.
    mbedtls_base64_encode(nullptr, 0, &out_len, data, len);
    std::string out(out_len, '\0');
    if (mbedtls_base64_encode(reinterpret_cast<unsigned char*>(&out[0]), out_len, &out_len, data, len) != 0) {
        return {};
    }
    out.resize(out_len);
    return out;
}

bool base64_decode(const std::string& in, std::vector<uint8_t>& out)
{
    size_t out_len = 0;
    mbedtls_base64_decode(nullptr, 0, &out_len, reinterpret_cast<const unsigned char*>(in.data()), in.size());
    out.resize(out_len);
    if (out_len == 0) {
        return in.empty();
    }
    if (mbedtls_base64_decode(out.data(), out_len, &out_len,
                              reinterpret_cast<const unsigned char*>(in.data()), in.size()) != 0) {
        out.clear();
        return false;
    }
    out.resize(out_len);
    return true;
}

std::string get_setting(const char* key, const char* fallback)
{
    Settings settings(kSettingsNamespace, false);
    return settings.GetString(key, fallback);
}

}  // namespace

/* ------------------------------------------------------------- Impl */

class AliyunOmniProtocol::Impl {
public:
    std::unique_ptr<WebSocket> websocket;
    EventGroupHandle_t events = nullptr;

    std::string url;
    std::string api_key;
    std::string model;
    std::string voice;

    bool configured = false;
    bool error = false;
    bool channel_opened = false;
    int64_t connected_at_us = 0;
    std::string pending_client_event_id;

    TranscriptScanner scanner;

    // The session this protocol last saw, so a reconnect can tell the server to
    // drop the previous context instead of accumulating it.
    std::string session_id;
};

/* ----------------------------------------------------------- construction */

AliyunOmniProtocol::AliyunOmniProtocol() : _impl(std::make_unique<Impl>())
{
    _impl->events = xEventGroupCreate();

    _impl->api_key = GetConfiguredApiKey();
    _impl->model = GetConfiguredModel();
    _impl->voice = GetConfiguredVoice();

    const std::string workspace = GetConfiguredWorkspaceId();
    const std::string url_override = get_setting("url", "");
    if (!url_override.empty()) {
        _impl->url = url_override;
    } else if (!workspace.empty()) {
        // Workspace-scoped domain, needed by the qwen3.x-omni-*-realtime family.
        _impl->url = "wss://" + workspace + ".cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime";
    } else {
        _impl->url = kDefaultUrl;
    }

    _impl->configured = !_impl->api_key.empty();

    server_sample_rate_ = kOutputSampleRate;
    server_frame_duration_ = 60;

    ESP_LOGI(TAG, "url=%s model=%s voice=%s configured=%d", _impl->url.c_str(), _impl->model.c_str(),
             _impl->voice.c_str(), static_cast<int>(_impl->configured));
}

AliyunOmniProtocol::~AliyunOmniProtocol()
{
    CloseAudioChannel(false);
    if (_impl && _impl->events != nullptr) {
        vEventGroupDelete(_impl->events);
        _impl->events = nullptr;
    }
}

/* -------------------------------------------------------------- settings */

std::string AliyunOmniProtocol::GetConfiguredApiKey()
{
    return get_setting("api_key", "");
}

std::string AliyunOmniProtocol::GetConfiguredWorkspaceId()
{
    return get_setting("workspace_id", "");
}

std::string AliyunOmniProtocol::GetConfiguredModel()
{
    return get_setting("model", kDefaultModel);
}

std::string AliyunOmniProtocol::GetConfiguredVoice()
{
    return get_setting("voice", kDefaultVoice);
}

bool AliyunOmniProtocol::IsConfigured()
{
    return !GetConfiguredApiKey().empty();
}

/* ------------------------------------------------------------- lifecycle */

bool AliyunOmniProtocol::Start()
{
    // Nothing to connect yet: the audio channel is opened lazily when the user
    // starts a turn, exactly like WebsocketProtocol. Network readiness is
    // handled by WifiBoard, whose StartNetwork() returns immediately and
    // reports completion through events.
    return true;
}

bool AliyunOmniProtocol::IsAudioChannelOpened() const
{
    return _impl->websocket != nullptr && _impl->websocket->IsConnected() && !_impl->error &&
           _impl->channel_opened && !IsTimeout();
}

bool AliyunOmniProtocol::IsTimeout() const
{
    // See the declaration: a quiet realtime session is not a dead one. The
    // 120-minute server-side cap is enforced in SendAudio().
    return false;
}

void AliyunOmniProtocol::SendStartListening(ListeningMode mode)
{
    // Turn taking is Aliyun's job via semantic VAD. The device cannot start or
    // stop a turn, and sending xiaozhi's listen events would be rejected.
    ESP_LOGD(TAG, "SendStartListening ignored (server VAD drives turns)");
}

void AliyunOmniProtocol::SendStopListening()
{
    ESP_LOGD(TAG, "SendStopListening ignored (server VAD drives turns)");
}

void AliyunOmniProtocol::SendWakeWordDetected(const std::string& wake_word)
{
    ESP_LOGD(TAG, "SendWakeWordDetected ignored (wake word stayed on-device)");
}

void AliyunOmniProtocol::SendMcpMessage(const std::string& message)
{
    ESP_LOGW(TAG, "MCP is not supported over the Aliyun Realtime API; message dropped");
}

void AliyunOmniProtocol::CloseAudioChannel(bool send_goodbye)
{
    if (_impl->websocket == nullptr) {
        _impl->channel_opened = false;
        return;
    }

    if (send_goodbye && _impl->websocket->IsConnected()) {
        // Ending the session server-side releases the context. Skipping this
        // lets context accumulate across turns on the same socket.
        SendClientEvent("{\"type\":\"session.finish\"}");

        // Give the server a moment to acknowledge, but never block teardown on
        // it: an established session can take seconds to wind down.
        xEventGroupWaitBits(_impl->events, kBitSessionReady, pdFALSE, pdFALSE, pdMS_TO_TICKS(300));
    }

    _impl->websocket.reset();
    _impl->channel_opened = false;
    _impl->session_id.clear();
    _impl->scanner = TranscriptScanner{};
}

/* ------------------------------------------------------------- connecting */

bool AliyunOmniProtocol::OpenAudioChannel()
{
    if (!_impl->configured) {
        ESP_LOGE(TAG, "no API key configured; set the 'api_key' key in the 'aliyun' settings namespace");
        SetError("Aliyun API key not configured");
        return false;
    }

    _impl->error = false;
    _impl->channel_opened = false;
    _impl->scanner = TranscriptScanner{};
    xEventGroupClearBits(_impl->events, kBitSessionReady);

    // Application::Initialize() calls board.StartNetwork(), which returns
    // immediately: WiFi may still be associating on the first attempt. Bail out
    // fast instead of blocking the main task for the full handshake timeout;
    // the main loop retries once the network is up.
    auto& wifi = WifiManager::GetInstance();
    if (!wifi.IsConnected() && !wifi.IsConfigMode()) {
        ESP_LOGI(TAG, "network not connected yet, will retry");
        return false;
    }

    auto network = Board::GetInstance().GetNetwork();
    _impl->websocket = network->CreateWebSocket(1);
    if (_impl->websocket == nullptr) {
        ESP_LOGE(TAG, "failed to create websocket");
        SetError("websocket create failed");
        return false;
    }

    // Auth is a plain bearer token and is validated during the HTTP upgrade, so
    // no signing or token exchange is involved.
    const std::string auth = "Bearer " + _impl->api_key;
    _impl->websocket->SetHeader("Authorization", auth.c_str());
    _impl->websocket->SetHeader("User-Agent", "stackchan-aliyun/1.0");

    _impl->websocket->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            // The Realtime API is JSON-only; binary frames are undocumented.
            ESP_LOGW(TAG, "ignoring unexpected binary frame (%u bytes)", static_cast<unsigned>(len));
            return;
        }
        last_incoming_time_ = std::chrono::steady_clock::now();

        // Marshal onto the main task so all parsing and callbacks run serially
        // and no extra locking is needed around the scanner or cJSON.
        std::string payload(data, len);
        Application::GetInstance().Schedule([this, payload = std::move(payload)]() { HandleServerEvent(payload); });
    });

    _impl->websocket->OnDisconnected([this]() {
        ESP_LOGW(TAG, "websocket disconnected");
        _impl->channel_opened = false;
        if (on_audio_channel_closed_ != nullptr) {
            on_audio_channel_closed_();
        }
    });

    const std::string connect_url = _impl->url + "?model=" + _impl->model;
    ESP_LOGI(TAG, "connecting to %s", connect_url.c_str());

    if (!_impl->websocket->Connect(connect_url.c_str())) {
        ESP_LOGE(TAG, "connect failed, code=%d", _impl->websocket->GetLastError());
        _impl->websocket.reset();
        SetError("Aliyun connect failed");
        return false;
    }

    // The server announces itself with session.created. Nothing may be sent
    // before that.
    if ((xEventGroupWaitBits(_impl->events, kBitSessionReady, pdFALSE, pdFALSE, pdMS_TO_TICKS(15000)) &
         kBitSessionReady) == 0) {
        ESP_LOGE(TAG, "no session.created within 15s");
        _impl->websocket.reset();
        SetError("Aliyun session timeout");
        return false;
    }

    if (_impl->error) {
        _impl->websocket.reset();
        return false;
    }

    if (!SendSessionUpdate()) {
        _impl->websocket.reset();
        return false;
    }

    _impl->channel_opened = true;
    _impl->connected_at_us = esp_timer_get_time();

    if (on_audio_channel_opened_ != nullptr) {
        on_audio_channel_opened_();
    }
    return true;
}

bool AliyunOmniProtocol::SendSessionUpdate()
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "session.update");

    cJSON* session = cJSON_CreateObject();
    cJSON* modalities = cJSON_CreateArray();
    cJSON_AddItemToArray(modalities, cJSON_CreateString("text"));
    cJSON_AddItemToArray(modalities, cJSON_CreateString("audio"));
    cJSON_AddItemToObject(session, "modalities", modalities);

    cJSON* audio = cJSON_CreateObject();

    cJSON* input = cJSON_CreateObject();
    cJSON* in_format = cJSON_CreateObject();
    // "raw-opus" is Ogg-free Opus, which is exactly what the on-device encoder
    // (esp_opus_enc, 16 kHz mono, 60 ms) already produces. Sending it verbatim
    // avoids an Opus-to-PCM decode on every frame and cuts uplink traffic by
    // roughly an order of magnitude versus base64 PCM.
    cJSON_AddStringToObject(in_format, "type", "raw-opus");
    cJSON_AddNumberToObject(in_format, "sample_rate", kInputSampleRate);
    cJSON_AddNumberToObject(in_format, "channels", 1);
    cJSON_AddNumberToObject(in_format, "frame_size", kOpusFrameMs);
    cJSON_AddItemToObject(input, "format", in_format);
    cJSON_AddItemToObject(audio, "input", input);

    cJSON* output = cJSON_CreateObject();
    cJSON_AddStringToObject(output, "voice", _impl->voice.c_str());
    cJSON* out_format = cJSON_CreateObject();
    // raw-opus2 = Ogg-free Opus with no per-packet header, which is the shape
    // AudioService's decoder loop feeds straight into esp_opus_dec.
    cJSON_AddStringToObject(out_format, "type", "raw-opus2");
    cJSON_AddNumberToObject(out_format, "sample_rate", kOutputSampleRate);
    cJSON_AddNumberToObject(out_format, "frame_size", kOpusFrameMs);
    cJSON_AddItemToObject(output, "format", out_format);
    cJSON_AddItemToObject(audio, "output", output);

    cJSON_AddItemToObject(session, "audio", audio);

    // semantic_vad filters filler words and background noise, which a desktop
    // robot in an occupied room hears constantly.
    cJSON* vad = cJSON_CreateObject();
    cJSON_AddStringToObject(vad, "type", "semantic_vad");
    cJSON_AddNumberToObject(vad, "threshold", 0.5);
    cJSON_AddNumberToObject(vad, "silence_duration_ms", 800);
    cJSON_AddItemToObject(session, "turn_detection", vad);

    // Drives the on-device emotion bridge; see the class comment.
    cJSON_AddStringToObject(session, "instructions",
                            "你是一个桌面机器人 StackChan，用简短口语化的中文回应。"
                            "说话时请在句首插入一个情绪标记来驱动你的表情，"
                            "格式为方括号包住的英文单词，只能从以下八选一："
                            "[neutral] [happy] [laughing] [angry] [sad] [crying] [sleepy] [doubtful]。"
                            "每句话最多一个标记，不要在标记里加其他文字。");

    cJSON* transcription = cJSON_CreateObject();
    cJSON_AddStringToObject(transcription, "model", "qwen3-asr-flash-realtime");
    cJSON_AddItemToObject(session, "input_audio_transcription", transcription);

    cJSON_AddItemToObject(root, "session", session);

    char* printed = cJSON_PrintUnformatted(root);
    const bool ok = SendClientEvent(printed != nullptr ? printed : "");
    if (printed != nullptr) {
        cJSON_free(printed);
    }
    cJSON_Delete(root);

    if (!ok) {
        SetError("session.update send failed");
    }
    return ok;
}

bool AliyunOmniProtocol::SendText(const std::string& text)
{
    return SendClientEvent(text);
}

bool AliyunOmniProtocol::SendClientEvent(const std::string& json)
{
    if (_impl->websocket == nullptr || !_impl->websocket->IsConnected()) {
        return false;
    }
    return _impl->websocket->Send(json);
}

/* ------------------------------------------------------------------ uplink */

bool AliyunOmniProtocol::SendAudio(std::unique_ptr<AudioStreamPacket> packet)
{
    if (packet == nullptr || _impl->websocket == nullptr || !_impl->websocket->IsConnected()) {
        return false;
    }

    // Recycle the socket before Aliyun's hard 120-minute session cap, so the
    // reconnect happens between turns instead of mid-sentence. Guarded on
    // channel_opened so a recycle is never triggered while audio is still
    // flowing for the current turn.
    if (_impl->channel_opened && _impl->connected_at_us != 0 &&
        esp_timer_get_time() - _impl->connected_at_us > kMaxSessionUs) {
        ESP_LOGW(TAG, "session approaching the 120 min server limit, recycling");
        _impl->connected_at_us = esp_timer_get_time();
        CloseAudioChannel(true);
        if (!OpenAudioChannel()) {
            return false;
        }
    }

    // AudioStreamPacket already holds Opus from the AFE/encoder, and the session
    // is configured for "raw-opus", so the payload is forwarded verbatim: no
    // decode, no resample, and roughly an order of magnitude less traffic than
    // base64-encoded PCM would cost.
    const std::string b64 = base64_encode(packet->payload.data(), packet->payload.size());
    if (b64.empty()) {
        return false;
    }

    std::string json = "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
    json += b64;
    json += "\"}";

    if (!SendClientEvent(json)) {
        ESP_LOGW(TAG, "failed to send audio frame");
        return false;
    }
    return true;
}

void AliyunOmniProtocol::SendAbortSpeaking(AbortReason reason)
{
    if (!IsAudioChannelOpened()) {
        return;
    }
    // Cancel the in-flight response. The device also stops playback locally via
    // its own audio queue reset.
    SendClientEvent("{\"type\":\"response.cancel\"}");
}

/* ---------------------------------------------------------------- downlink */

void AliyunOmniProtocol::HandleServerEvent(const std::string& raw)
{
    cJSON* root = cJSON_Parse(raw.c_str());
    if (root == nullptr) {
        ESP_LOGW(TAG, "unparseable server message (%u bytes)", static_cast<unsigned>(raw.size()));
        return;
    }

    const cJSON* type_item = cJSON_GetObjectItem(root, "type");
    const std::string type = cJSON_IsString(type_item) ? type_item->valuestring : "";

    if (type == "session.created") {
        const cJSON* session = cJSON_GetObjectItem(root, "session");
        const cJSON* id = session != nullptr ? cJSON_GetObjectItem(session, "id") : nullptr;
        if (cJSON_IsString(id)) {
            _impl->session_id = id->valuestring;
        }
        ESP_LOGI(TAG, "session.created id=%s", _impl->session_id.c_str());
        xEventGroupSetBits(_impl->events, kBitSessionReady);

    } else if (type == "session.updated") {
        ESP_LOGI(TAG, "session.updated");

    } else if (type == "error") {
        const cJSON* err = cJSON_GetObjectItem(root, "error");
        char* printed = err != nullptr ? cJSON_PrintUnformatted(err) : nullptr;
        ESP_LOGE(TAG, "server error: %s", printed != nullptr ? printed : "(unknown)");
        if (printed != nullptr) {
            cJSON_free(printed);
        }
        _impl->error = true;
        if (on_network_error_ != nullptr) {
            on_network_error_("Aliyun: server returned an error");
        }

    } else if (type == "input_audio_buffer.speech_started") {
        // Barge-in: the user started talking while the model was speaking.
        ESP_LOGI(TAG, "speech_started (barge-in)");
        Application::GetInstance().AbortSpeaking(kAbortReasonNone);

    } else if (type == "input_audio_buffer.speech_stopped") {
        ESP_LOGI(TAG, "speech_stopped");

    } else if (type == "conversation.item.input_audio_transcription.completed") {
        const cJSON* text = cJSON_GetObjectItem(root, "transcript");
        if (cJSON_IsString(text)) {
            ESP_LOGI(TAG, ">> %s", text->valuestring);
        }
        EmitTtsState("stop");
        EmitStt(text);

    } else if (type == "response.created") {
        EmitTtsState("start");

    } else if (type == "response.audio_transcript.delta" || type == "response.text.delta") {
        const cJSON* delta = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(delta)) {
            HandleTranscriptDelta(delta->valuestring);
        }

    } else if (type == "response.audio.delta") {
        const cJSON* delta = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(delta)) {
            std::vector<uint8_t> pcm;
            if (base64_decode(delta->valuestring, pcm) && !pcm.empty()) {
                auto packet = std::make_unique<AudioStreamPacket>();
                packet->sample_rate = kOutputSampleRate;
                packet->frame_duration = server_frame_duration_;
                packet->timestamp = 0;
                packet->payload = std::move(pcm);
                if (on_incoming_audio_ != nullptr) {
                    on_incoming_audio_(std::move(packet));
                }
            }
        }

    } else if (type == "response.audio.done") {
        ESP_LOGD(TAG, "response.audio.done");

    } else if (type == "response.done") {
        EmitTtsState("stop");

    } else if (type == "input_audio_buffer.committed") {
        ESP_LOGD(TAG, "audio buffer committed");

    } else {
        ESP_LOGD(TAG, "unhandled event: %s", type.c_str());
    }

    cJSON_Delete(root);
}

void AliyunOmniProtocol::HandleTranscriptDelta(const char* delta)
{
    if (delta == nullptr) {
        return;
    }

    std::vector<std::string> emotions;
    const std::string visible = _impl->scanner.consume(delta, emotions);

    for (const auto& emotion : emotions) {
        ESP_LOGI(TAG, "emotion -> %s", emotion.c_str());
        EmitEmotion(emotion);
    }

    if (!visible.empty()) {
        ESP_LOGI(TAG, "<< %s", visible.c_str());
        EmitSentence(visible);
    }
}

/* -------------------------------------------------- xiaozhi JSON emitters */

void AliyunOmniProtocol::EmitJson(cJSON* root)
{
    if (on_incoming_json_ == nullptr) {
        cJSON_Delete(root);
        return;
    }
    on_incoming_json_(root);
    cJSON_Delete(root);
}

void AliyunOmniProtocol::EmitTtsState(const char* state)
{
    // Application maps start -> kDeviceStateSpeaking and stop -> back to
    // listening or idle, which also gates the decode queue.
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "tts");
    cJSON_AddStringToObject(root, "state", state);
    EmitJson(root);
}

void AliyunOmniProtocol::EmitStt(const cJSON* text_item)
{
    if (!cJSON_IsString(text_item)) {
        return;
    }
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "stt");
    cJSON_AddStringToObject(root, "text", text_item->valuestring);
    EmitJson(root);
}

void AliyunOmniProtocol::EmitSentence(const std::string& text)
{
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "tts");
    cJSON_AddStringToObject(root, "state", "sentence_start");
    cJSON_AddStringToObject(root, "text", text.c_str());
    EmitJson(root);
}

void AliyunOmniProtocol::EmitEmotion(const std::string& emotion)
{
    // Application already routes this to StackChanAvatarDisplay::SetEmotion(),
    // so the avatar needs no Aliyun-specific code at all.
    cJSON* root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "type", "llm");
    cJSON_AddStringToObject(root, "emotion", emotion.c_str());
    EmitJson(root);
}
