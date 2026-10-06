/*
 * SPDX-FileCopyrightText: 2026 M5Stack Technology CO LTD
 *
 * SPDX-License-Identifier: MIT
 */
#pragma once

#include <protocol.h>
#include <web_socket.h>
#include <memory>
#include <string>

/**
 * @brief Aliyun Bailian Qwen-Omni-Realtime protocol.
 *
 * A drop-in replacement for WebsocketProtocol that talks to Aliyun's
 * session-based Realtime API instead of a xiaozhi-compatible backend:
 *
 *     wss://dashscope.aliyuncs.com/api-ws/v1/realtime?model=qwen3.8-omni-flash-realtime
 *
 * Why WebSocket and not WebRTC/AOQ: Alibaba's own protocol matrix lists AOQ as
 * Android/iOS/HarmonyOS only and WebRTC as browser/mobile, leaving WebSocket as
 * the only transport that supports "any environment that speaks WebSocket".
 * The matrix also notes WebSocket has no built-in echo cancellation, which this
 * device already solves in hardware: the ES7210 array provides a reference
 * channel and the AFE processor runs AEC locally. See docs/aliyun-omni-v2v-plan.md.
 *
 * The firmware layer this sits on is reused unchanged: AudioService, the AFE,
 * Opus encode/decode, the LVGL avatar and WiFi provisioning are untouched.
 *
 * Emotion bridging: the Realtime API only emits text and audio, so expressions
 * are driven by convention. The system prompt asks the model to prefix a turn
 * with "[happy]", and this class parses those tags out of the streaming
 * transcript, forwarding them as {"type":"llm","emotion":"..."} which
 * Application already routes to StackChanAvatarDisplay::SetEmotion().
 * The tag vocabulary is defined in tools/aliyun_omni/protocol.py and asserted
 * against the firmware in that tool's self-test.
 */
/**
 * @brief Seed the "aliyun" NVS namespace from build-time configuration.
 *
 * Must run before Application::InitializeProtocol() decides which protocol to
 * instantiate, so it is called from app_main right after HAL init. Values are
 * only written when the corresponding key is still empty, so a credential
 * provisioned at runtime (over BLE, or by a previous build) is never clobbered.
 *
 * @return true when an API key is available from NVS or the build config.
 */
bool AliyunOmniSeedSettingsFromKconfig();

class AliyunOmniProtocol : public Protocol {
public:
    AliyunOmniProtocol();
    ~AliyunOmniProtocol() override;

    bool Start() override;
    bool OpenAudioChannel() override;
    void CloseAudioChannel(bool send_goodbye = true) override;
    bool IsAudioChannelOpened() const override;
    bool SendAudio(std::unique_ptr<AudioStreamPacket> packet) override;

    /**
     * @brief Tell the server the local VAD saw the end of a spoken turn.
     *
     * The session runs with turn_detection disabled: server-side VAD does not
     * fire on Ogg-framed Opus input (verified - the same audio transcribes
     * correctly in manual mode but produces no speech_started/stopped events
     * with semantic_vad enabled). The device's own AFE VAD is reliable, so it
     * decides when to commit the buffer and ask for a response.
     */
    void NotifyLocalSpeechEnded();

    // Accepted and ignored: turn taking is manual here, and xiaozhi's listen
    // events are not part of the Realtime protocol.
    void SendStartListening(ListeningMode mode) override;
    void SendStopListening() override;
    void SendWakeWordDetected(const std::string& wake_word) override;

    // Rejects speech locally and asks the server to cancel the current response.
    void SendAbortSpeaking(AbortReason reason) override;

    // Not supported: Aliyun has its own tool-calling mechanism, delivered as
    // response events rather than xiaozhi's MCP envelope. Overridden as a no-op
    // so it does not emit xiaozhi's MCP envelope over the Aliyun socket.
    void SendMcpMessage(const std::string& message) override;

    /**
     * @brief Configuration accessors, backed by the "aliyun" settings namespace.
     *
     * Public because Application::CheckNewVersion() and InitializeProtocol()
     * both consult IsConfigured() to decide whether the device talks to Aliyun
     * or falls back to the stock xiaozhi backend.
     */
    static std::string GetConfiguredApiKey();
    static std::string GetConfiguredWorkspaceId();
    static std::string GetConfiguredModel();
    static std::string GetConfiguredVoice();
    /** Raw NVS/build values, before defaults are applied. */
    static std::string GetConfiguredModelRaw();
    static std::string GetConfiguredVoiceRaw();
    static bool IsConfigured();

protected:
    // The base class treats 120 s without server traffic as a dead channel.
    // For a realtime session that is simply a quiet room, and the session can
    // legitimately stay open for up to 120 minutes. Liveness is tracked with
    // heartbeats and reconnects instead.
    bool IsTimeout() const override;

private:
    class Impl;
    std::unique_ptr<Impl> _impl;

    /** Send one client event, returning false when the socket is not usable. */
    bool SendClientEvent(const std::string& json);

    /** Base64 one buffer and send it as input_audio_buffer.append. */
    bool SendAudioBuffer(const uint8_t* data, size_t len);

    /** Build and send session.update. */
    bool SendSessionUpdate();

    /** Dispatch one parsed server event. Runs on the main task. */
    void HandleServerEvent(const std::string& raw);

    /** Feed a transcript delta through the tag scanner and emit the results. */
    void HandleTranscriptDelta(const char* delta);

    // Pure virtual in Protocol. For this transport a "text message" is simply a
    // client event, so it forwards to SendClientEvent.
    bool SendText(const std::string& text) override;

    /* Emitters that speak xiaozhi's JSON dialect, which Application parses. */
    void EmitJson(cJSON* root);
    void EmitTtsState(const char* state);
    void EmitStt(const cJSON* text_item);
    void EmitSentence(const std::string& text);
    void EmitEmotion(const std::string& emotion);
};
