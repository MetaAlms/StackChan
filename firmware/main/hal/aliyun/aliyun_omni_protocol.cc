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
#include <cmath>
#include <cstring>
#include <mooncake_log.h>
#include <wifi_manager.h>
#include <audio/demuxer/ogg_demuxer.h>
#include "ogg_opus_muxer.h"
#include <sdkconfig.h>

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

// How long to wait for the server's first frame after the upgrade succeeds.
// This covers TLS application-data records rather than the handshake itself,
// which is already done by then.
constexpr int kSessionReadyTimeoutMs = 40000;

// Keep in sync with MAX_DECODE_PACKETS_IN_QUEUE in
// xiaozhi-esp32/main/audio/audio_service.h (8000 ms of 60 ms packets).
constexpr int kDecodeQueueCapacityMs = 8000;

// Energy VAD thresholds. The AFE output for normal speech sits well above the
// noise floor (~120), so this is deliberately low - it is a fallback, not a
// replacement for the AFE's VAD.
constexpr int kEnergyVadThreshold = 300;          // RMS, out of 32768
constexpr int kEnergyVadMinSpeechFrames = 3;      // 180 ms of sound starts speech
constexpr int kEnergyVadSilenceFrames = 12;       // 720 ms of quiet ends it

/* ------------------------------------------------------------- emotion tags */

// Mirrors tools/aliyun_omni/protocol.py, which asserts these strings against
// StackChanAvatarDisplay::SetEmotion(). "doubtful" not "doubt": the display's
// unknown-emotion branch would silently reset the face to neutral.
// Emotion is signalled by a leading emoji, not a bracket tag.
//
// Measured against the live endpoint: feeding a generated reply back through the
// API's own ASR showed "[happy] 你好" is *spoken* as "Happy, 你好", while
// "😊 你好" is spoken as just "你好". The bracket form leaks into the audio; the
// emoji does not. So emoji are the tag.
//
// The emotion strings must match StackChanAvatarDisplay::SetEmotion() exactly
// (main/hal/board/stackchan_display.cc); the self-test in
// tools/aliyun_omni_probe.py asserts that they do.
struct EmotionEmoji {
    const char* utf8;  // 4-byte UTF-8 sequence
    const char* emotion;
};

const EmotionEmoji kEmotionEmoji[] = {
    {"\xF0\x9F\x98\x80", "happy"},      // 😀
    {"\xF0\x9F\x98\x8A", "happy"},      // 😊
    {"\xF0\x9F\x99\x82", "happy"},      // 🙂
    {"\xF0\x9F\x98\x84", "laughing"},   // 😄
    {"\xF0\x9F\x98\x86", "laughing"},   // 😆
    {"\xF0\x9F\x98\x82", "laughing"},   // 😂
    {"\xF0\x9F\x98\xA0", "angry"},      // 😠
    {"\xF0\x9F\x98\xA1", "angry"},      // 😡
    {"\xF0\x9F\x98\xA2", "crying"},     // 😢
    {"\xF0\x9F\x98\xAD", "crying"},     // 😭
    {"\xF0\x9F\x98\x94", "sad"},        // 😔
    {"\xF0\x9F\x98\x9E", "sad"},        // 😞
    {"\xF0\x9F\x98\xB4", "sleepy"},     // 😴
    {"\xF0\x9F\x98\xAA", "sleepy"},     // 😪
    {"\xF0\x9F\xA4\x94", "doubtful"},   // 🤔
    {"\xF0\x9F\x98\x95", "doubtful"},   // 😕
    {"\xF0\x9F\x98\x90", "neutral"},    // 😐
};

constexpr size_t kEmojiBytes = 4;

/** Length in bytes of the known emoji at `p`, or 0. */
size_t emoji_at(const char* p, size_t remaining)
{
    if (remaining < kEmojiBytes) {
        return 0;
    }
    for (const auto& entry : kEmotionEmoji) {
        if (memcmp(p, entry.utf8, kEmojiBytes) == 0) {
            return kEmojiBytes;
        }
    }
    return 0;
}

/** Emotion for the emoji at `p`, or nullptr. */
const char* emotion_at(const char* p, size_t remaining)
{
    if (remaining < kEmojiBytes) {
        return nullptr;
    }
    for (const auto& entry : kEmotionEmoji) {
        if (memcmp(p, entry.utf8, kEmojiBytes) == 0) {
            return entry.emotion;
        }
    }
    return nullptr;
}

/**
 * @brief True when the bytes at `p` are an incomplete prefix of a known emoji.
 *
 * Deltas arrive cut at arbitrary byte offsets, so a UTF-8 emoji can straddle
 * two of them. Holding the fragment back avoids rendering a broken glyph.
 */
bool is_emoji_prefix(const char* p, size_t remaining)
{
    for (const auto& entry : kEmotionEmoji) {
        if (remaining <= kEmojiBytes && memcmp(p, entry.utf8, remaining) == 0) {
            return true;
        }
    }
    return false;
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

        // Walk the accumulated transcript byte by byte, splitting it into text
        // and emotion segments. Re-parsing from the start on every delta is
        // deliberate: a later delta can re-split the final text segment, so
        // byte offsets into it are not stable, but the settled text is.
        std::string text_segments;
        size_t emotion_count = 0;

        // `emit_end` is exclusive: everything before it is settled.
        size_t emit_end = 0;
        size_t pos = 0;

        while (pos < raw.size()) {
            const unsigned char c = static_cast<unsigned char>(raw[pos]);

            if (c < 0x80) {  // ASCII text
                ++pos;
                continue;
            }

            const size_t remaining = raw.size() - pos;
            const char* emotion = emotion_at(raw.data() + pos, remaining);
            if (emotion != nullptr) {
                text_segments.append(raw, emit_end, pos - emit_end);
                emit_end = pos + kEmojiBytes;
                pos = emit_end;
                ++emotion_count;
                if (emotion_count > fired) {
                    fired_emotions.emplace_back(emotion);
                }
                continue;
            }

            if (is_emoji_prefix(raw.data() + pos, remaining)) {
                // Incomplete emoji at the tail: hold everything from here back
                // until a later delta completes it.
                text_segments.append(raw, emit_end, pos - emit_end);
                emit_end = pos;
                break;
            }

            // Some other multi-byte character (Chinese text, punctuation):
            // advance one byte at a time so we never misinterpret a continuation
            // byte as the start of a sequence.
            ++pos;
        }

        if (emit_end < raw.size() && pos >= raw.size()) {
            text_segments.append(raw, emit_end, std::string::npos);
            emit_end = raw.size();
        }

        // Emit everything after the common prefix with what was already sent.
        // The settled text can also shrink back when a held-back fragment is
        // released, so appending a naive tail would duplicate output.
        size_t common = 0;
        while (common < sent_text.size() && common < text_segments.size() &&
               sent_text[common] == text_segments[common]) {
            ++common;
        }
        const std::string new_text = text_segments.substr(common);

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

/**
 * @brief Events that must be processed on the network task, not the main task.
 *
 * OpenAudioChannel() runs on the main task and blocks until session.created
 * arrives, so dispatching that event through the main-task queue deadlocks:
 * the queue is only drained by the task that is blocked. Errors are included
 * because OpenAudioChannel also needs to observe a failed handshake promptly.
 */
bool is_handshake_critical(const std::string& payload)
{
    return payload.find("\"session.created\"") != std::string::npos ||
           payload.find("\"error\"") != std::string::npos;
}

/**
 * @brief Events that must be dispatched before any audio delta that follows them.
 *
 * Application only feeds the decoder while the device state is "speaking", and
 * that state is set from the tts/start event. Dispatching response.created
 * through the main-task queue put it *behind* the audio deltas that had already
 * been queued, so every burst of audio was processed while the state was still
 * listening and was dropped wholesale. That is what made playback stutter:
 * arrival gaps of up to 1.85 s, and the packets that did arrive got discarded
 * instead of buffering.
 *
 * Handling it inline on the network task means the state change is scheduled
 * first, ahead of the audio.
 */
bool must_precede_audio(const std::string& payload)
{
    return payload.find("\"response.created\"") != std::string::npos ||
           payload.find("\"response.done\"") != std::string::npos;
}

/**
 * @brief True for events that apply backpressure and so must run on the network task.
 *
 * The server pushes audio far faster than realtime, so something has to slow it
 * down. Blocking the main task would not: the network task would keep reading
 * and simply queue more work. Blocking the task that reads the socket does -
 * the WebSocket reader stalls, the TCP window closes and the server throttles
 * itself to playback speed. Audio is therefore dispatched inline here, and
 * Application pushes it to the decoder with wait=true.
 */
bool is_audio_payload(const std::string& payload)
{
    return payload.find("\"response.audio.delta\"") != std::string::npos;
}

std::string get_setting(const char* key, const char* fallback)
{
    Settings settings(kSettingsNamespace, false);
    return settings.GetString(key, fallback);
}

/* Build-time defaults. Empty unless the corresponding Kconfig option is set. */
#ifdef CONFIG_STACKCHAN_ALIYUN_API_KEY
constexpr const char* kBuildApiKey = CONFIG_STACKCHAN_ALIYUN_API_KEY;
#else
constexpr const char* kBuildApiKey = "";
#endif

#ifdef CONFIG_STACKCHAN_ALIYUN_WORKSPACE_ID
constexpr const char* kBuildWorkspaceId = CONFIG_STACKCHAN_ALIYUN_WORKSPACE_ID;
#else
constexpr const char* kBuildWorkspaceId = "";
#endif

#ifdef CONFIG_STACKCHAN_ALIYUN_USE_WORKSPACE_DOMAIN
constexpr bool kUseWorkspaceDomain = CONFIG_STACKCHAN_ALIYUN_USE_WORKSPACE_DOMAIN;
#else
constexpr bool kUseWorkspaceDomain = false;
#endif

#ifdef CONFIG_STACKCHAN_ALIYUN_MODEL
constexpr const char* kBuildModel = CONFIG_STACKCHAN_ALIYUN_MODEL;
#else
constexpr const char* kBuildModel = "";
#endif

#ifdef CONFIG_STACKCHAN_ALIYUN_VOICE
constexpr const char* kBuildVoice = CONFIG_STACKCHAN_ALIYUN_VOICE;
#else
constexpr const char* kBuildVoice = "";
#endif

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
    std::string workspace_id;

    bool configured = false;
    bool error = false;
    bool channel_opened = false;
    int64_t connected_at_us = 0;
    std::string pending_client_event_id;

    TranscriptScanner scanner;

    // Downlink audio arrives as Ogg-framed Opus spread over many
    // response.audio.delta events, so the demuxer has to persist across them.
    std::unique_ptr<OggDemuxer> demuxer;

    // Uplink is Ogg-framed too, so the muxer owns stream state (page sequence,
    // granule position) that spans the whole session.
    OggOpusMuxer muxer;
    bool ogg_headers_sent = false;

    // True between response.created and response.done.
    bool response_active = false;

    // How much downlink audio is queued but not yet played, and when that
    // estimate was last brought up to date.
    //
    // A fixed guard window does not work here. The server delivers a whole
    // response far faster than realtime, so at response.done the decoder can
    // still hold seconds of speech. A constant window either expires too early -
    // letting the local VAD mistake the tail for a new user turn and cancel the
    // answer mid-sentence - or is needlessly long for short replies. Tracking
    // the actual queued duration stays correct for both.
    int64_t queued_audio_us = 0;
    int64_t queued_tick_us = 0;

    // Local energy VAD.
    //
    // Turn taking normally rides on the AFE's VAD. That is not always available:
    // with CONFIG_USE_AUDIO_PROCESSOR disabled the codec path uses
    // NoAudioProcessor, which accepts an OnVadStateChange callback and never
    // calls it. Without a fallback the device would then never end a turn. This
    // also serves as a sanity check on the AFE: if the energy VAD sees speech
    // while the AFE reports silence, the AFE is suppressing real audio.
#if !CONFIG_USE_AUDIO_PROCESSOR
    bool energy_speech = false;
    int silence_frames = 0;
    int speech_frames = 0;
#endif

    // A turn that ended while the model was still speaking.
    //
    // Ending a turn during playback has to wait: committing would cancel the
    // answer mid-sentence, and the local VAD hears the speaker anyway. But the
    // turn must not simply be dropped either - the user has already stopped
    // talking, so no further VAD event will arrive to replace it and their words
    // would be lost until they spoke again. Remember it and commit as soon as
    // playback finishes.
    bool pending_turn_end = false;

    // Arrival-rate instrumentation: packets decoded for the current response.
    int packets_this_response = 0;
    int64_t response_started_us = 0;

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
    } else if (kUseWorkspaceDomain && !workspace.empty()) {
        // Workspace-scoped endpoint. Kept behind a flag because the device's
        // TLS stack stalled on this host while the public one worked.
        _impl->url = "wss://" + workspace + ".cn-beijing.maas.aliyuncs.com/api-ws/v1/realtime";
    } else {
        _impl->url = kDefaultUrl;
    }
    _impl->workspace_id = workspace;

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
    return get_setting("api_key", kBuildApiKey);
}

std::string AliyunOmniProtocol::GetConfiguredWorkspaceId()
{
    return get_setting("workspace_id", kBuildWorkspaceId);
}

std::string AliyunOmniProtocol::GetConfiguredModel()
{
    const std::string configured = GetConfiguredModelRaw();
    return configured.empty() ? kDefaultModel : configured;
}

std::string AliyunOmniProtocol::GetConfiguredModelRaw()
{
    return get_setting("model", kBuildModel);
}

std::string AliyunOmniProtocol::GetConfiguredVoice()
{
    const std::string configured = GetConfiguredVoiceRaw();
    return configured.empty() ? kDefaultVoice : configured;
}

std::string AliyunOmniProtocol::GetConfiguredVoiceRaw()
{
    return get_setting("voice", kBuildVoice);
}

bool AliyunOmniProtocol::IsConfigured()
{
    // Checked by Application before the protocol is even constructed, so this
    // must work from the build config alone when NVS has not been seeded yet.
    return !GetConfiguredApiKey().empty();
}

bool AliyunOmniSeedSettingsFromKconfig()
{
    // A free function cannot call a static member unqualified.
    if (AliyunOmniProtocol::GetConfiguredApiKey().empty()) {
        ESP_LOGW(TAG, "no Aliyun API key in NVS or build config");
        return false;
    }

    Settings settings(kSettingsNamespace, true);

    struct Seed {
        const char* key;
        const char* value;
    };
    const Seed seeds[] = {
        {"api_key", kBuildApiKey},
        {"workspace_id", kBuildWorkspaceId},
        {"model", kBuildModel},
        {"voice", kBuildVoice},
    };

    for (const auto& seed : seeds) {
        if (seed.value == nullptr || seed.value[0] == '\0') {
            continue;
        }
        // Never overwrite an existing value: a credential provisioned at
        // runtime must win over whatever the last build happened to embed.
        if (!settings.GetString(seed.key, "").empty()) {
            continue;
        }
        settings.SetString(seed.key, seed.value);
        ESP_LOGI(TAG, "seeded '%s' from build config", seed.key);
    }

    return true;
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
        ESP_LOGW(TAG, "network not ready: connected=%d config_mode=%d rssi=%d",
                 (int)wifi.IsConnected(), (int)wifi.IsConfigMode(), wifi.GetRssi());
        return false;
    }
    ESP_LOGI(TAG, "opening audio channel (rssi=%d)", wifi.GetRssi());

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
    // The public endpoint needs the workspace identified out of band. Harmless
    // on the workspace-scoped host, so it is always sent.
    if (!_impl->workspace_id.empty()) {
        _impl->websocket->SetHeader("X-DashScope-WorkSpace", _impl->workspace_id.c_str());
    }

    _impl->websocket->OnData([this](const char* data, size_t len, bool binary) {
        if (binary) {
            // The Realtime API is JSON-only; binary frames are undocumented.
            ESP_LOGW(TAG, "ignoring unexpected binary frame (%u bytes)", static_cast<unsigned>(len));
            return;
        }
        last_incoming_time_ = std::chrono::steady_clock::now();

        std::string payload(data, len);

        // Handshake-critical events must be handled HERE, on the network task.
        //
        // Everything else is marshalled onto the main task so parsing and the
        // xiaozhi callbacks stay serialised. But session.created cannot go
        // through that queue: OpenAudioChannel() blocks the main task waiting
        // for it, so a deferred dispatch would be waiting on the very task that
        // is waiting for it. That deadlock cost a 40 second timeout on every
        // connection, with the frame itself arriving in 10 ms.
        if (is_handshake_critical(payload) || must_precede_audio(payload) || is_audio_payload(payload)) {
            HandleServerEvent(payload);
            return;
        }

        Application::GetInstance().Schedule(
            [this, payload = std::move(payload)]() { HandleServerEvent(payload); });
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

    // Full radio performance for the duration of the handshake. The board drops
    // to LOW_POWER once activation completes, and a sleeping radio can defer
    // the TLS records and the first server frame by whole beacon intervals.
    Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::PERFORMANCE);

    const int64_t t_connect_start = esp_timer_get_time();
    if (!_impl->websocket->Connect(connect_url.c_str())) {
        ESP_LOGE(TAG, "connect failed, code=%d", _impl->websocket->GetLastError());
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        _impl->websocket.reset();
        SetError("Aliyun connect failed");
        return false;
    }
    const int64_t handshake_ms = (esp_timer_get_time() - t_connect_start) / 1000;

    // The server announces itself with session.created. Nothing may be sent
    // before that. The budget is generous because this spans the first TLS
    // application-data records, which are noticeably slower on the device than
    // from a desktop.
    const int64_t t_wait_start = esp_timer_get_time();
    if ((xEventGroupWaitBits(_impl->events, kBitSessionReady, pdFALSE, pdFALSE, pdMS_TO_TICKS(kSessionReadyTimeoutMs)) &
         kBitSessionReady) == 0) {
        const int64_t waited = (esp_timer_get_time() - t_wait_start) / 1000;
        ESP_LOGE(TAG, "no session.created within %d ms (tls+upgrade took %d ms, waited %d ms)",
                 kSessionReadyTimeoutMs, (int)handshake_ms, (int)waited);
        Board::GetInstance().SetPowerSaveLevel(PowerSaveLevel::LOW_POWER);
        _impl->websocket.reset();
        SetError("Aliyun session timeout");
        return false;
    }
    const int64_t waited_ms = (esp_timer_get_time() - t_wait_start) / 1000;
    ESP_LOGI(TAG, "handshake %d ms, session.created after %d ms",
             (int)handshake_ms, (int)waited_ms);

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

    _impl->demuxer = std::make_unique<OggDemuxer>();
    _impl->demuxer->OnDemuxerFinished([this](const uint8_t* data, int /*sample_rate*/, size_t size) {
        // Count decoded packets so the arrival rate can be compared with the
        // audio duration they represent (each packet is kOpusFrameMs).
        ++_impl->packets_this_response;
        noteAudioQueued(kOpusFrameMs);
        // The demuxer reports 48 kHz for Opus, which is the codec's internal
        // rate. The session asked for 24 kHz output, which is what the speaker
        // path and the decoder are configured for.
        auto packet = std::make_unique<AudioStreamPacket>();
        packet->sample_rate = kOutputSampleRate;
        packet->frame_duration = kOpusFrameMs;
        packet->timestamp = 0;
        packet->payload.assign(data, data + size);
        if (on_incoming_audio_ != nullptr) {
            on_incoming_audio_(std::move(packet));
        }
    });
    _impl->demuxer->Reset();
    _impl->muxer.reset();
    _impl->ogg_headers_sent = false;

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
    // Measured against the live endpoint: raw-opus and raw-opus2 are NOT
    // usable on the uplink - the server drops the connection as soon as a bare
    // Opus frame arrives. Only pcm and opus (Ogg-framed) work. Ogg keeps the
    // on-device Opus encoding, so the uplink stays near 4 kB/s instead of the
    // ~32 kB/s that 16 kHz PCM would cost, at the price of wrapping each packet
    // in an Ogg page (see ogg_opus_muxer.h).
    cJSON_AddStringToObject(in_format, "type", "opus");
    cJSON_AddNumberToObject(in_format, "sample_rate", kInputSampleRate);
    cJSON_AddNumberToObject(in_format, "channels", 1);
    cJSON_AddNumberToObject(in_format, "frame_size", kOpusFrameMs);
    cJSON_AddItemToObject(input, "format", in_format);
    cJSON_AddItemToObject(audio, "input", input);

    cJSON* output = cJSON_CreateObject();
    cJSON_AddStringToObject(output, "voice", _impl->voice.c_str());
    cJSON* out_format = cJSON_CreateObject();
    // Measured against the live endpoint: only pcm, opus and mp3 are accepted
    // for output; raw-opus, raw-opus2 and raw-opu are all rejected with
    // CLIENT_ERROR "Unsupported encode format". Of the accepted three, "opus"
    // is Ogg-framed Opus, which the on-device decoder can consume once the Ogg
    // container is stripped by the demuxer that xiaozhi already ships.
    cJSON_AddStringToObject(out_format, "type", "opus");
    cJSON_AddNumberToObject(out_format, "sample_rate", kOutputSampleRate);
    cJSON_AddNumberToObject(out_format, "frame_size", kOpusFrameMs);
    cJSON_AddItemToObject(output, "format", out_format);
    cJSON_AddItemToObject(audio, "output", output);

    cJSON_AddItemToObject(session, "audio", audio);

    // Turn detection is disabled on purpose. Server-side semantic_vad does not
    // react to Ogg-framed Opus input: the identical audio that transcribes
    // correctly in manual mode produced no speech_started/speech_stopped at all
    // with VAD enabled, so a turn would never end and the device would hang in
    // listening forever. The local AFE VAD drives turns instead - see
    // NotifyLocalSpeechEnded().
    cJSON_AddItemToObject(session, "turn_detection", cJSON_CreateNull());

    // Drives the on-device emotion bridge; see the class comment.
    cJSON_AddStringToObject(session, "instructions",
                            "你是一个桌面机器人 StackChan，用简短口语化的中文回应。"
                            "请在每次回答的最前面加一个表情符号来表示你的情绪，"
                            "只能从以下八个里选一个："
                            "😐（平静）、😊（开心）、😄（大笑）、😠（生气）、😔（难过）、"
                            "😭（哭泣）、😴（困倦）、🤔（疑惑）。"
                            "例如：\"😊 好呀，我很乐意！\"。"
                            "只加一个表情符号，不要输出其他符号或英文单词。");

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

bool AliyunOmniProtocol::SendAudioBuffer(const uint8_t* data, size_t len)
{
    const std::string b64 = base64_encode(data, len);
    if (b64.empty()) {
        return false;
    }
    std::string json = "{\"type\":\"input_audio_buffer.append\",\"audio\":\"";
    json += b64;
    json += "\"}";
    if (!SendClientEvent(json)) {
        ESP_LOGW(TAG, "failed to send audio buffer");
        return false;
    }
    return true;
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

    // Half duplex: while the model is speaking, drop uplink audio entirely.
    //
    // There is no working echo cancellation on this hardware. The stock Kconfig
    // never enabled device-side AEC for this board, and enabling it makes things
    // worse: the ES7210's second channel is a second microphone rather than a
    // speaker loopback, so AEC cancels the user's voice along with the echo
    // (AFE output peak fell from ~4036 to ~120). Aliyun's WebSocket Realtime API
    // does not cancel echo server-side either.
    //
    // So the device simply stops listening while it talks. The cost is that
    // barge-in - interrupting the model mid-sentence - does not work; the gain
    // is that the device no longer interrupts itself with its own speaker, which
    // is the behaviour that actually matters here.
    if (IsModelSpeaking() ||
        Application::GetInstance().GetDeviceState() == kDeviceStateSpeaking) {
        // Uplink is suppressed while the model talks, so the local VAD is
        // hearing the speaker rather than the user. Nothing to do here.
        return true;  // dropped on purpose, not an error
    }

    // Playback has finished. If a turn ended while it was still running, submit
    // it now - otherwise those words would never reach the server.
    if (_impl->pending_turn_end) {
        _impl->pending_turn_end = false;
        ESP_LOGI(TAG, "flushing the turn that ended during playback");
        NotifyLocalSpeechEnded();
    }

    // Energy VAD: decide turn boundaries from frame loudness alone.
    //
    // Only a fallback. When the audio processor is compiled in its VAD is
    // authoritative - running both would end the same turn twice and could cut
    // an answer short. NoAudioProcessor, by contrast, never invokes its VAD
    // callback, so without this the device would never end a turn at all.
#if !CONFIG_USE_AUDIO_PROCESSOR
    {
        const uint8_t* pcm_bytes = packet->payload.data();
        const size_t sample_count = packet->payload.size() / sizeof(int16_t);
        int64_t sum_sq = 0;
        for (size_t i = 0; i < sample_count; ++i) {
            // The payload is raw little-endian PCM; memcpy avoids a misaligned
            // int16_t load.
            int16_t sample = 0;
            memcpy(&sample, pcm_bytes + i * sizeof(int16_t), sizeof(sample));
            sum_sq += static_cast<int64_t>(sample) * sample;
        }
        const int rms = sample_count == 0
                            ? 0
                            : static_cast<int>(std::sqrt(static_cast<double>(sum_sq) / sample_count));
        const bool loud = rms > kEnergyVadThreshold;

        if (loud) {
            ++_impl->speech_frames;
            _impl->silence_frames = 0;
            if (!_impl->energy_speech && _impl->speech_frames >= kEnergyVadMinSpeechFrames) {
                _impl->energy_speech = true;
                ESP_LOGI(TAG, "energy VAD: speech started (rms=%d)", rms);
            }
        } else {
            _impl->speech_frames = 0;
            if (_impl->energy_speech) {
                if (++_impl->silence_frames >= kEnergyVadSilenceFrames) {
                    _impl->energy_speech = false;
                    _impl->silence_frames = 0;
                    ESP_LOGI(TAG, "energy VAD: speech ended (rms=%d), ending the turn", rms);
                    NotifyLocalSpeechEnded();
                }
            }
        }
    }
#endif

    // The session is configured for "opus", i.e. Ogg-framed Opus, so each frame
    // the AFE produced is wrapped in an Ogg page. No decode and no resample are
    // involved, which keeps this cheap on the device.
    if (!_impl->ogg_headers_sent) {
        // OpusHead and OpusTags must precede the first audio page; the server
        // rejects a stream that starts mid-page. Sent lazily so a session that
        // never carries audio does not emit an empty buffer.
        const std::vector<uint8_t> headers = _impl->muxer.begin(kInputSampleRate, 1);
        if (!SendAudioBuffer(headers.data(), headers.size())) {
            return false;
        }
        _impl->ogg_headers_sent = true;
    }

    {
        static int sent_frames = 0;
        ++sent_frames;
        if (sent_frames % 50 == 1) {
            ESP_LOGW(TAG, "[up] sending frame #%d (%d opus bytes)", sent_frames,
                     (int)packet->payload.size());
        }
        // Dump a contiguous run of raw Opus payloads so the uplink can be
        // decoded off-device with ffmpeg. Guessing at the audio chain from peak
        // levels has not worked; this makes the encoder output directly
        // inspectable. Frames 200..259 are 60 ms each, i.e. 3.6 s of audio.
        if (sent_frames >= 200 && sent_frames < 260) {
            const std::string dump = base64_encode(packet->payload.data(), packet->payload.size());
            ESP_LOGW(TAG, "OPUSDUMP %d %s", (int)sent_frames, dump.c_str());
        }
    }

    const std::vector<uint8_t> page = _impl->muxer.write_packet(packet->payload.data(), packet->payload.size());
    if (page.empty()) {
        return false;
    }
    return SendAudioBuffer(page.data(), page.size());
}

/** Bring the queued-audio estimate up to date by subtracting elapsed time. */
void AliyunOmniProtocol::decayQueuedAudio()
{
    const int64_t now = esp_timer_get_time();
    if (_impl->queued_tick_us != 0) {
        _impl->queued_audio_us -= (now - _impl->queued_tick_us);
        if (_impl->queued_audio_us < 0) {
            _impl->queued_audio_us = 0;
        }
    }
    _impl->queued_tick_us = now;
}

/** Record that `ms` of audio was handed to the decoder. */
void AliyunOmniProtocol::noteAudioQueued(int ms)
{
    decayQueuedAudio();
    _impl->queued_audio_us += static_cast<int64_t>(ms) * 1000;

    // Clamp to what the decoder can physically hold.
    //
    // This is an open-loop estimate: it adds the length of every packet handed
    // over and subtracts elapsed time. It drifts high, because Application drops
    // packets while the device is not in the speaking state and those are still
    // counted, and because the microphone path - and therefore the periodic
    // decay - pauses while the device talks. Left unbounded it reached 64 s
    // against an 8 s queue, which would keep the device deaf long after it had
    // finished speaking.
    //
    // The true value cannot exceed the queue capacity: anything beyond it is
    // either blocked by backpressure or dropped by the Application gate. So
    // clamping makes the estimate safe even though it is approximate.
    constexpr int64_t kMaxQueuedUs = static_cast<int64_t>(kDecodeQueueCapacityMs) * 1000;
    if (_impl->queued_audio_us > kMaxQueuedUs) {
        _impl->queued_audio_us = kMaxQueuedUs;
    }
}

bool AliyunOmniProtocol::IsModelSpeaking()
{
    if (_impl->response_active) {
        return true;
    }
    // Playback tail: audio already queued but not yet played out.
    return queuedAudioMs() > 0;
}

int AliyunOmniProtocol::queuedAudioMs()
{
    decayQueuedAudio();
    return static_cast<int>(_impl->queued_audio_us / 1000);
}

void AliyunOmniProtocol::NotifyLocalSpeechEnded()
{
    if (!IsAudioChannelOpened()) {
        return;
    }

    // This is the gate that actually stops self-interruption.
    //
    // Dropping uplink audio is not enough: the local VAD still hears the speaker
    // through the microphone, reports end-of-speech, and committing here would
    // cancel the answer that is still being spoken. So a turn is only ended once
    // the model has genuinely finished - no response in flight and the playback
    // queue drained.
    if (IsModelSpeaking()) {
        _impl->pending_turn_end = true;
        ESP_LOGI(TAG, "deferring turn end: response_active=%d, %d ms still queued",
                 _impl->response_active ? 1 : 0, queuedAudioMs());
        return;
    }
    // Manual mode: the buffer has to be committed explicitly, then a response
    // requested. Both are cheap client events.
    SendClientEvent("{\"type\":\"input_audio_buffer.commit\"}");
    SendClientEvent("{\"type\":\"response.create\"}");
    ESP_LOGI(TAG, "local VAD ended the turn: committed and requested a response");
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
        _impl->response_active = true;
        _impl->packets_this_response = 0;
        _impl->response_started_us = esp_timer_get_time();
        // Each response is its own Ogg logical stream.
        if (_impl->demuxer) {
            _impl->demuxer->Reset();
    _impl->muxer.reset();
    _impl->ogg_headers_sent = false;
        }
        EmitTtsState("start");

    } else if (type == "response.audio_transcript.delta" || type == "response.text.delta") {
        const cJSON* delta = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(delta)) {
            HandleTranscriptDelta(delta->valuestring);
        }

    } else if (type == "response.audio.delta") {
        const cJSON* delta = cJSON_GetObjectItem(root, "delta");
        if (cJSON_IsString(delta)) {
            std::vector<uint8_t> ogg;
            if (base64_decode(delta->valuestring, ogg) && !ogg.empty() && _impl->demuxer) {
                // Yields zero or more Opus packets via the demuxer callback.
                _impl->demuxer->Process(ogg.data(), ogg.size());
            }
        }

    } else if (type == "response.audio.done") {
        ESP_LOGD(TAG, "response.audio.done");

    } else if (type == "response.done") {
        if (_impl->response_started_us != 0) {
            const int64_t wall_ms = (esp_timer_get_time() - _impl->response_started_us) / 1000;
            const int64_t audio_ms = static_cast<int64_t>(_impl->packets_this_response) * kOpusFrameMs;
            if (wall_ms > 0) {
                ESP_LOGW(TAG, "[rate] %d packets = %d ms audio arrived over %d ms wall (%.2fx realtime)",
                         _impl->packets_this_response, (int)audio_ms, (int)wall_ms,
                         (double)audio_ms / (double)wall_ms);
            }
        }
        _impl->response_active = false;
        // Note: audio for this response may still be draining from the decode
        // queue. IsModelSpeaking() keeps gating until that queued audio has
        // actually played out, which is what stops the tail from being mistaken
        // for a new user turn.
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
