#!/usr/bin/env python3
"""Aliyun Qwen-Omni-Realtime link verifier and emotion-rule test bench.

Two jobs:

  1. PROVE the link works before any ESP32 code is written. Sends real audio,
     receives the real reply, and reports the numbers that decide firmware
     design choices (first-audio-packet latency, bandwidth, which audio
     formats the account actually accepts).

  2. LOCK DOWN the emotion-tag rules. The avatar bridge is pure convention, so
     it is much cheaper to get the parsing right here than to reflash the
     device for every tweak.

Zero third-party dependencies: the WebSocket client and the protocol helpers
live in tools/aliyun_omni/ and use only the standard library.

Examples:
    # offline: validate the emotion parser only, no key and no network needed
    python3 tools/aliyun_omni_probe.py --self-test

    # live one-shot question using a WAV file
    export DASHSCOPE_API_KEY=sk-...
    python3 tools/aliyun_omni_probe.py --wav question.wav

    # manual turn mode (no server VAD) and Opus on the wire
    python3 tools/aliyun_omni_probe.py --wav question.wav --manual \
        --input-format raw-opus --output-format raw-opus2
"""

from __future__ import annotations

import argparse
import base64
import json
import os
import struct
import sys
import time
import wave
from pathlib import Path

if sys.version_info < (3, 10):
    sys.exit("Python 3.10+ required (uses X | Y type hints)")

sys.path.insert(0, str(Path(__file__).resolve().parent))

from aliyun_omni import protocol  # noqa: E402
from aliyun_omni.ws_client import HandshakeError, WebSocketClient, WebSocketError  # noqa: E402

# Input audio is streamed in 100 ms slices. 16 kHz * 2 bytes * 0.1 s = 3200.
SEND_CHUNK_BYTES = 3200


# --------------------------------------------------------------------- helpers

def log(msg: str) -> None:
    print(msg, flush=True)


def die(msg: str, code: int = 1) -> "None":
    print(f"error: {msg}", file=sys.stderr, flush=True)
    raise SystemExit(code)


def resample_linear(samples, src_rate: int, dst_rate: int):
    """Linear-interpolation resampler.

    Good enough to make a human voice intelligible to an ASR front end, and it
    keeps this tool dependency-free. The firmware does not need this: the
    ES7210 hands us 16 kHz directly.
    """
    if src_rate == dst_rate or not samples:
        return list(samples)
    ratio = dst_rate / src_rate
    out_len = int(len(samples) * ratio)
    out = [0] * out_len
    for i in range(out_len):
        pos = i / ratio
        i0 = int(pos)
        i1 = min(i0 + 1, len(samples) - 1)
        frac = pos - i0
        out[i] = int(samples[i0] * (1.0 - frac) + samples[i1] * frac)
    return out


def load_wav_as_pcm16(path: Path, target_rate: int):
    """Read a WAV file, return (pcm_bytes, source_rate, duration_seconds)."""
    try:
        with wave.open(str(path), "rb") as wf:
            channels = wf.getnchannels()
            width = wf.getsampwidth()
            rate = wf.getframerate()
            frames = wf.readframes(wf.getnframes())
    except wave.Error as exc:
        die(f"cannot read {path}: {exc}. Convert it first, e.g. "
            f"`afconvert -f WAVE -d LEI16@{target_rate} in.m4a out.wav`")

    if width != 2:
        die(f"expected 16-bit audio, got {width * 8}-bit")

    samples = list(struct.unpack(f"<{len(frames) // 2}h", frames))

    # Downmix to mono, keeping the left channel of each frame.
    if channels > 1:
        samples = samples[::channels]

    duration = len(samples) / float(rate)
    samples = resample_linear(samples, rate, target_rate)

    return struct.pack(f"<{len(samples)}h", *samples), rate, duration


def write_pcm16_wav(path: Path, pcm: bytes, rate: int) -> None:
    with wave.open(str(path), "wb") as wf:
        wf.setnchannels(1)
        wf.setsampwidth(2)
        wf.setframerate(rate)
        wf.writeframes(pcm)


# ------------------------------------------------------------------ self test

def run_self_test() -> int:
    """Offline checks for everything that needs no credentials."""
    failures = []

    def check(name, got, want):
        if got != want:
            failures.append(f"{name}\n     got:  {got!r}\n     want: {want!r}")

    # --- tag recognition -----------------------------------------------
    check("simple", protocol.clean_text("[happy] hello"), " hello")
    check("uppercase", protocol.clean_text("[HAPPY] hi"), " hi")
    check("padded", protocol.clean_text("[ happy ] hi"), " hi")
    check("alias joy", protocol.clean_text("[joy] hi"), " hi")
    check("alias confused", protocol.clean_text("[confused] hi"), " hi")
    check("unknown kept", protocol.clean_text("[banana] hi"), "[banana] hi")
    check("not a tag", protocol.clean_text("[hello world] hi"), "[hello world] hi")
    check("empty tag", protocol.clean_text("[] hi"), "[] hi")
    check("mid sentence", protocol.clean_text("a [sad] b"), "a  b")
    check("multiple", protocol.clean_text("[happy] a [sad] b"), " a  b")
    check("no tags", protocol.clean_text("plain text"), "plain text")
    check("brackets unbalanced", protocol.clean_text("[oops hi"), "[oops hi")

    # --- segment shape --------------------------------------------------
    segs = protocol.parse_segments("[happy] hi [sad] bye")
    check("segment kinds", [s.kind for s in segs], ["emotion", "text", "emotion", "text"])
    check("segment values", [s.value for s in segs], ["happy", " hi ", "sad", " bye"])

    # --- the regression this test was written to catch ------------------
    # A delta cut mid-tag must not leak "[hap" into visible text.
    state = protocol.new_stream_state()
    events, text = protocol.advance_emotion_events(state, "[hap")
    check("partial tag: no event", events, [])
    check("partial tag: no leak", text, "")

    events, text = protocol.advance_emotion_events(state, "py] Hel")
    check("completed tag fires", [e.emotion for e in events], ["happy"])
    check("completed tag text", text, " Hel")

    events, text = protocol.advance_emotion_events(state, "lo!")
    check("no duplicate event", events, [])
    check("trailing text", text, "lo!")
    check("clean transcript", state.cleaned_text(), " Hello!")

    # A bracketed non-tag must not be held back once it is disqualified.
    state2 = protocol.new_stream_state()
    _events, text = protocol.advance_emotion_events(state2, "[hello wor")
    check("non-tag not deferred", text, "[hello wor")

    # Second tag fires once, and text between tags is not duplicated.
    state3 = protocol.new_stream_state()
    protocol.advance_emotion_events(state3, "[happy] a")
    events, text = protocol.advance_emotion_events(state3, "[sad] b")
    check("second tag fires", [e.emotion for e in events], ["sad"])
    check("second tag text", text, " b")

    # --- settled-string parsing must never swallow text -----------------
    check("settled unterminated", protocol.clean_text("[oops hi"), "[oops hi")
    check("settled lone bracket", protocol.clean_text("hi ["), "hi [")
    check("settled trailing tag", protocol.clean_text("ok [happy]"), "ok ")
    check("settled tag then bracket", protocol.clean_text("[happy] ok ["), " ok [")

    # --- streaming: a disqualified fragment is released, not lost --------
    state4 = protocol.new_stream_state()
    _events, text = protocol.advance_emotion_events(state4, "[hello wor")
    check("disqualified released", text, "[hello wor")
    _events, text = protocol.advance_emotion_events(state4, "ld] hi")
    check("disqualified stays text", text, "ld] hi")

    # --- streaming: no duplication across many small deltas -------------
    # Every character arrives alone. A lone "[" is held back for one delta,
    # so the visible text only appears once the tag resolves.
    state5 = protocol.new_stream_state()
    collected = ""
    emotions = []
    for piece in ["[", "h", "a", "p", "p", "y", "]", " H", "i", "!"]:
        events, text = protocol.advance_emotion_events(state5, piece)
        collected += text
        emotions.extend(e.emotion for e in events)
    check("char-by-char text", collected, " Hi!")
    check("char-by-char emotions", emotions, ["happy"])
    check("char-by-char no leak", "[" not in collected, True)

    # --- streaming: a bracket that never becomes a tag is released -------
    state7 = protocol.new_stream_state()
    out = ""
    for piece in ["[", "s", "e", "e", " ", "t", "h", "i", "s"]:
        _events, text = protocol.advance_emotion_events(state7, piece)
        out += text
    check("literal bracket released", out, "[see this")
    check("literal bracket no emotion", state7.fired, 0)

    # --- streaming: emission is never duplicated ------------------------
    state8 = protocol.new_stream_state()
    transcript = "[happy] Hello there, friend!"
    emitted = ""
    for i in range(1, len(transcript) + 1):
        _events, text = protocol.advance_emotion_events(state8, transcript[i - 1:i])
        emitted += text
    check("no duplication", emitted, " Hello there, friend!")
    check("full transcript", state8.cleaned_text(), " Hello there, friend!")

    # --- streaming: tag then text then tag ------------------------------
    state6 = protocol.new_stream_state()
    protocol.advance_emotion_events(state6, "[happy] a")
    events, text = protocol.advance_emotion_events(state6, " b [sad]")
    check("mid tag then text", text, " b ")
    check("mid tag fires", [e.emotion for e in events], ["sad"])

    # --- vocabulary must match the firmware's display mapping ------------
    # Ground truth: StackChanAvatarDisplay::SetEmotion() in
    # firmware/main/hal/board/stackchan_display.cc. If a string here is not
    # accepted there, the device logs "Unknown emotion" and resets to neutral.
    firmware_vocabulary = {
        "neutral", "happy", "laughing", "angry",
        "sad", "crying", "sleepy", "doubtful",
    }
    check("vocabulary matches firmware",
          set(protocol.SUPPORTED_EMOTIONS), firmware_vocabulary)
    bad = {alias: target for alias, target in protocol.EMOTION_ALIASES.items()
           if target not in firmware_vocabulary}
    check("no alias points outside firmware vocabulary", bad, {})

    # --- session payload shape ------------------------------------------
    payload = protocol.build_session_update(instructions="test")
    check("payload type", payload["type"], "session.update")
    check("vad default", payload["session"]["turn_detection"]["type"], "semantic_vad")
    check("input rate", payload["session"]["audio"]["input"]["format"]["sample_rate"], 16000)
    check("output rate", payload["session"]["audio"]["output"]["format"]["sample_rate"], 24000)
    check("modalities", payload["session"]["modalities"], ["text", "audio"])
    check("instructions set", payload["session"]["instructions"], "test")

    # --- resampler ------------------------------------------------------
    check("resample passthrough", app_resample([1, 2, 3], 16000, 16000), [1, 2, 3])
    up = app_resample([0, 100], 8000, 16000)
    check("resample upsamples", len(up), 4)

    if failures:
        log(f"self-test FAILED ({len(failures)} case(s)):")
        for f in failures:
            log(f"  - {f}")
        return 1

    log("self-test passed")
    log(f"  emotion rules      : {len(protocol.EMOTION_ALIASES)} aliases -> "
        f"{len(protocol.SUPPORTED_EMOTIONS)} firmware emotions")
    log("  streaming parser   : mid-tag splits held back, no leaks, no duplicates")
    log("  session payload    : 16 kHz in / 24 kHz out / semantic_vad")
    log("  resampler          : passthrough and upsample")
    return 0


def app_resample(samples, src_rate, dst_rate):
    """Thin alias so the self-test reads clearly."""
    return resample_linear(samples, src_rate, dst_rate)


# ----------------------------------------------------------------- live probe

class Probe:
    def __init__(self, args):
        self.args = args
        self.ws: WebSocketClient | None = None

        self.audio_out = bytearray()
        self.transcript_raw = ""
        self.transcript_clean = ""
        self.stream_state = protocol.new_stream_state()
        self.input_transcript = ""

        self.t_session_created = None
        self.t_session_updated = None
        self.t_send_done = None
        self.t_speech_stopped = None
        self.t_first_audio = None
        self.t_response_done = None
        self.bytes_sent = 0
        self.bytes_received = 0

    # ------------------------------------------------------------- lifecycle

    def build_url(self) -> str:
        url = self.args.url
        if "?" not in url:
            url += f"?model={self.args.model}"
        return url

    def run(self) -> int:
        pcm, src_rate, duration = load_wav_as_pcm16(Path(self.args.wav), 16000)
        log(f"input    : {self.args.wav} ({src_rate} Hz -> 16000 Hz, {duration:.2f}s, "
            f"{len(pcm)} bytes PCM)")
        log(f"model    : {self.args.model}")
        log(f"url      : {self.build_url()}")
        log(f"mode     : {'manual' if self.args.manual else 'server VAD'}, "
            f"in={self.args.input_format}, out={self.args.output_format}")
        log("")

        headers = {
            "Authorization": f"Bearer {self.args.api_key}",
            "User-Agent": "stackchan-omni-probe/1.0",
        }
        if self.args.workspace_id:
            headers["X-DashScope-WorkSpace"] = self.args.workspace_id

        self.ws = WebSocketClient(self.build_url(), headers=headers)

        try:
            self.ws.connect()
        except HandshakeError as exc:
            log(f"handshake rejected: {exc.status}")
            if exc.body:
                log(f"body: {exc.body[:500].decode('utf-8', 'replace')}")
            if exc.status_code in ("401", "403"):
                log("")
                log("Endpoint and TLS path are correct; the credential was refused.")
                log("Check that the API key matches the region and that the model")
                log("is enabled in the Bailian console.")
            return 2
        except (WebSocketError, OSError) as exc:
            die(f"cannot connect: {exc}")

        log(f"connected: TLS established, upgrade {self.ws.response_headers.get('upgrade', '?')}")

        try:
            self._await_session_created()
            self._send_session_update()
            self._stream_audio(pcm)
            self._collect_response()
        except KeyboardInterrupt:
            log("\ninterrupted")
        finally:
            try:
                self.ws.close()
            except Exception:
                pass

        return self._report()

    # ---------------------------------------------------------------- stages

    def _pump(self, timeout: float):
        """Yield parsed events until timeout expires."""
        deadline = time.monotonic() + timeout
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return
            try:
                for opcode, payload in self.ws.messages(timeout=remaining):
                    if opcode == 0x2:  # binary frame: undocumented on this API
                        log(f"  [binary frame, {len(payload)} bytes]")
                        continue
                    try:
                        yield json.loads(payload.decode("utf-8"))
                    except (UnicodeDecodeError, json.JSONDecodeError):
                        log(f"  [non-JSON text, {len(payload)} bytes]")
            except (TimeoutError, getattr(__import__("socket"), "timeout")):
                return
            except BlockingIOError:
                # A partial frame was read but no more bytes are available yet;
                # the next pump call resumes it.
                return
            except OSError as exc:
                die(f"socket error: {exc}")

    def _await_session_created(self) -> None:
        for event in self._pump(self.args.timeout):
            etype = event.get("type", "")
            if etype == protocol.EV_SESSION_CREATED:
                self.t_session_created = time.monotonic()
                log("session.created")
                return
            if etype == protocol.EV_ERROR:
                die(f"server error: {json.dumps(event.get('error', event))}")
        die("timed out waiting for session.created")

    def _send_session_update(self) -> None:
        payload = protocol.build_session_update(
            voice=self.args.voice,
            instructions=self.args.instructions or protocol.EMOTION_SYSTEM_PROMPT,
            turn_detection=None if self.args.manual else {
                "type": "semantic_vad",
                "threshold": self.args.vad_threshold,
                "silence_duration_ms": self.args.silence_ms,
            },
            input_audio_format=self.args.input_format,
            output_audio_format=self.args.output_format,
        )
        if self.args.manual:
            payload["session"]["turn_detection"] = None

        self.ws.send_text(json.dumps(payload))

        for event in self._pump(self.args.timeout):
            etype = event.get("type", "")
            if etype == protocol.EV_SESSION_UPDATED:
                self.t_session_updated = time.monotonic()
                log("session.updated")
                return
            if etype == protocol.EV_ERROR:
                die(f"session.update rejected: {json.dumps(event.get('error', event))}")
        die("timed out waiting for session.updated")

    def _stream_audio(self, pcm: bytes) -> None:
        total = len(pcm)
        interval = SEND_CHUNK_BYTES / (16000 * 2)  # 100 ms of audio per chunk
        log(f"streaming {total} bytes in {SEND_CHUNK_BYTES}-byte chunks...")

        started = time.monotonic()
        for offset in range(0, total, SEND_CHUNK_BYTES):
            chunk = pcm[offset:offset + SEND_CHUNK_BYTES]
            b64 = base64.b64encode(chunk).decode("ascii")
            self.ws.send_text(json.dumps({
                "type": "input_audio_buffer.append",
                "audio": b64,
            }))
            self.bytes_sent += len(chunk)

            # Drain anything the server already sent (speech_started, etc).
            for event in self._pump(0.0):
                self._handle_event(event)

            # Pace to real time so server-side VAD sees a natural cadence.
            target = started + (offset + len(chunk)) / (16000 * 2)
            slack = target - time.monotonic()
            if slack > 0:
                time.sleep(slack)

        self.t_send_done = time.monotonic()
        elapsed = self.t_send_done - started
        log(f"audio sent: {self.bytes_sent} bytes in {elapsed:.2f}s "
            f"({self.bytes_sent / elapsed:.0f} B/s)")

        if self.args.manual:
            log("manual mode: committing buffer and requesting a response")
            self.ws.send_text(json.dumps({"type": "input_audio_buffer.commit"}))
            self.ws.send_text(json.dumps({"type": "response.create"}))

    def _collect_response(self) -> None:
        deadline = time.monotonic() + self.args.response_timeout
        while time.monotonic() < deadline:
            got_any = False
            for event in self._pump(1.0):
                got_any = True
                if self._handle_event(event):
                    return
            if not got_any and self.t_first_audio is None and \
                    time.monotonic() > (self.t_send_done or 0) + self.args.timeout:
                log("no response yet; server VAD may need more audio or a longer pause")
                return
        log("response timeout reached")

    # -------------------------------------------------------------- dispatch

    def _handle_event(self, event: dict) -> bool:
        """Handle one event. Returns True when the turn is finished."""
        etype = event.get("type", "")

        if etype == protocol.EV_ERROR:
            log(f"  error: {json.dumps(event.get('error', event), ensure_ascii=False)}")
            return False

        if etype == protocol.EV_SPEECH_STARTED:
            log("  input_audio_buffer.speech_started  <- barge-in trigger")
        elif etype == protocol.EV_SPEECH_STOPPED:
            self.t_speech_stopped = time.monotonic()
            log("  input_audio_buffer.speech_stopped")
        elif etype == protocol.EV_RESPONSE_CREATED:
            log("  response.created")
        elif etype in (protocol.EV_RESPONSE_AUDIO_DELTA,):
            chunk = base64.b64decode(event.get("delta", ""))
            if self.t_first_audio is None:
                self.t_first_audio = time.monotonic()
                if self.t_send_done:
                    log(f"  FIRST AUDIO after {self.t_first_audio - self.t_send_done:.2f}s "
                        f"from end of input")
            self.audio_out += chunk
            self.bytes_received += len(chunk)
        elif etype in (protocol.EV_RESPONSE_TRANSCRIPT_DELTA, protocol.EV_RESPONSE_TEXT_DELTA):
            delta = event.get("delta", "")
            self.transcript_raw += delta
            events, new_text = protocol.advance_emotion_events(self.stream_state, delta)
            self.transcript_clean = self.stream_state.cleaned
            for ev in events:
                log(f"  EMOTION -> {ev.emotion}")
            if new_text:
                print(new_text, end="", flush=True)
        elif etype in (protocol.EV_RESPONSE_TRANSCRIPT_DONE, protocol.EV_RESPONSE_TEXT_DONE):
            done = event.get("transcript") or event.get("text") or ""
            if done and not self.transcript_raw:
                self.transcript_raw = done
            print()
        elif etype == protocol.EV_INPUT_TRANSCRIPT_DELTA:
            self.input_transcript = (event.get("text", "") + event.get("stash", ""))
        elif etype == protocol.EV_INPUT_TRANSCRIPT_COMPLETED:
            self.input_transcript = event.get("transcript", self.input_transcript)
            log(f"  [heard] {self.input_transcript}")
        elif etype == protocol.EV_RESPONSE_AUDIO_DONE:
            log("  response.audio.done")
        elif etype == protocol.EV_RESPONSE_DONE:
            self.t_response_done = time.monotonic()
            status = (event.get("response") or {}).get("status", "?")
            log(f"  response.done (status={status})")
            return True
        else:
            log(f"  {etype}")

        return False

    # ---------------------------------------------------------------- report

    def _report(self) -> int:
        log("")
        log("=" * 62)
        log("RESULT")
        log("=" * 62)
        log(f"input transcript : {self.input_transcript or '(none)'}")
        log(f"reply (raw)      : {self.transcript_raw or '(none)'}")
        log(f"reply (visible)  : {self.transcript_clean.strip() or '(none)'}")
        log(f"audio received   : {len(self.audio_out)} bytes")

        if self.audio_out:
            out_path = Path(self.args.out)
            write_pcm16_wav(out_path, bytes(self.audio_out), 24000)
            log(f"reply audio      : {out_path}  (play with: afplay {out_path})")
        else:
            log("reply audio      : none - check --output-format and the account's model access")

        if self.t_send_done and self.t_first_audio:
            log(f"first audio lag  : {self.t_first_audio - self.t_send_done:.2f}s")
        if self.t_response_done:
            log(f"total turn time  : {self.t_response_done - self.t_send_done:.2f}s")

        log(f"bandwidth        : sent {self.bytes_sent} B, recv {self.bytes_received} B")

        ok = bool(self.transcript_raw or self.audio_out)
        log("")
        log("PASS - link verified end to end" if ok else "FAIL - no reply content received")
        return 0 if ok else 3


# ----------------------------------------------------------------------- main

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Verify the Aliyun Qwen-Omni-Realtime link and lock down emotion rules.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    parser.add_argument("--wav", help="16-bit PCM WAV holding one spoken question")
    parser.add_argument("--out", default="reply.wav", help="where to write reply audio")
    parser.add_argument("--api-key", default=os.environ.get("DASHSCOPE_API_KEY", ""),
                        help="defaults to $DASHSCOPE_API_KEY")
    parser.add_argument("--workspace-id", default=os.environ.get("DASHSCOPE_WORKSPACE_ID", ""),
                        help="Bailian workspace ID; sent as X-DashScope-WorkSpace when set")
    parser.add_argument("--url", default=protocol.DEFAULT_URL)
    parser.add_argument("--model", default=protocol.DEFAULT_MODEL)
    parser.add_argument("--voice", default=protocol.DEFAULT_VOICE)
    parser.add_argument("--instructions", default=None,
                        help="system prompt; defaults to the emotion-tag prompt")
    parser.add_argument("--manual", action="store_true",
                        help="disable server VAD and drive turns explicitly")
    parser.add_argument("--input-format", default="pcm",
                        choices=["pcm", "opus", "raw-opus"])
    parser.add_argument("--output-format", default="pcm",
                        choices=["pcm", "mp3", "opus", "raw-opus", "raw-opus2",
                                 "raw-opu", "raw-opu2"])
    parser.add_argument("--vad-threshold", type=float, default=0.5)
    parser.add_argument("--silence-ms", type=int, default=800)
    parser.add_argument("--timeout", type=float, default=20.0,
                        help="seconds to wait for a single protocol milestone")
    parser.add_argument("--response-timeout", type=float, default=60.0,
                        help="seconds to wait for the whole reply")
    parser.add_argument("--self-test", action="store_true",
                        help="run offline checks only; no key or network needed")
    args = parser.parse_args()

    if args.self_test:
        return run_self_test()

    if not args.wav:
        parser.error("--wav is required unless --self-test is used")
    if not Path(args.wav).exists():
        die(f"no such file: {args.wav}")
    if not args.api_key:
        die("no API key: pass --api-key or set DASHSCOPE_API_KEY")

    return Probe(args).run()


if __name__ == "__main__":
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        raise SystemExit(130)
