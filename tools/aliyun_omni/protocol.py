"""Aliyun Qwen-Omni-Realtime protocol helpers.

Covers three concerns:
  1. the event vocabulary of /api-ws/v1/realtime
  2. session configuration payloads (session.update)
  3. the emotion-tag bridge that lets the model drive StackChan's avatar

The emotion bridge exists because the Realtime API only emits text and audio.
It has no notion of M5Stack's ControlAvatar frames, so the avatar has to be
driven by convention: the system prompt asks the model to emit inline tags
like "[happy]", and both this tool and the firmware strip those tags out of
the visible text while turning them into avatar::Emotion changes.

Keeping the rules in one file means the Mac tool and the firmware cannot
drift apart.

The parsing contract, which the firmware will mirror:

    scan(full_accumulated_transcript) -> (segments, consumed, deferred)

`consumed` is how many characters of the input are settled (either emitted as
segments or recognised as complete tags). `deferred` is the tail that must be
held back because it may be the beginning of a tag that the next delta will
complete. Callers therefore never need to reason about delta boundaries.
"""

from __future__ import annotations

import os
import re
from dataclasses import dataclass
from typing import Iterable, List, Optional, Tuple

__all__ = [
    "DEFAULT_MODEL",
    "DEFAULT_VOICE",
    "DEFAULT_URL",
    "KEYCHAIN_ACCOUNT",
    "KEYCHAIN_SERVICE",
    "SUPPORTED_EMOTIONS",
    "EMOTION_ALIASES",
    "EMOTION_SYSTEM_PROMPT",
    "EmotionEvent",
    "Segment",
    "advance_emotion_events",
    "build_session_update",
    "clean_text",
    "emotion_for",
    "new_stream_state",
    "parse_segments",
    "scan",
]

# --------------------------------------------------------------------- config

# Keychain coordinates for the Mac-side credential.
#
# The "blue-" prefix groups every project secret on this machine into one
# family, so `blue-keychain-list` can show them all and nothing else. The rest
# of the name is deliberately project- and vendor-specific: a bare "dashscope"
# or "bailian" entry would collide with other projects
#
# tools/aliyun_keychain.sh reads these by parsing this file, so this is the only
# place they are defined. Change them here, nowhere else.
#
# The machine-wide tools are the blue-keychain-save / -list / -get skills
# (~/.agents/skills/), which store any project's secret under the same prefix.
KEYCHAIN_SERVICE = "blue-stackchan-bailian-key"
KEYCHAIN_ACCOUNT = "metaalms"

DEFAULT_MODEL = "qwen3.8-omni-flash-realtime"
DEFAULT_VOICE = "Tina"
DEFAULT_URL = "wss://dashscope.aliyuncs.com/api-ws/v1/realtime"

# A complete tag: "[happy]", "[ happy ]".
# These are EXACTLY the strings StackChanAvatarDisplay::SetEmotion() accepts.
#   firmware/main/hal/board/stackchan_display.cc:321
# The firmware therefore needs no translation table: whatever this module emits
# can be handed straight to the `llm` event's `emotion` field, which
# Application routes to display->SetEmotion() and on to the avatar.
SUPPORTED_EMOTIONS = (
    "neutral",
    "happy",
    "laughing",
    "angry",
    "sad",
    "crying",
    "sleepy",
    "doubtful",
)

# Kept for the legacy bracket-tag form and for any text the model writes in
# words. The live path uses emoji; see _EMOJI_TABLE.
EMOTION_ALIASES = {
    "neutral": "neutral", "calm": "neutral", "normal": "neutral",
    "happy": "happy", "joy": "happy", "smile": "happy", "glad": "happy",
    "laughing": "laughing", "laugh": "laughing", "lol": "laughing",
    "angry": "angry", "anger": "angry", "mad": "angry",
    "sad": "sad", "unhappy": "sad", "down": "sad",
    "crying": "crying", "cry": "crying", "tears": "crying",
    "doubtful": "doubtful", "doubt": "doubtful", "confused": "doubtful",
    "sleepy": "sleepy", "tired": "sleepy", "sleep": "sleepy",
}

# Emotion is signalled with a leading emoji, not a bracket tag.
#
# Measured: feeding the generated reply audio back through the API's own ASR
# showed that "[happy] 你好" is *spoken* as "Happy, 你好", while "😊 你好" is
# spoken as just "你好". The bracket form leaks into the audio; the emoji does
# not. So the emoji is the tag.
_EMOJI_TABLE = (
    ("\U0001F600", "happy"),      # 😀
    ("\U0001F60A", "happy"),      # 😊
    ("\U0001F642", "happy"),      # 🙂
    ("\U0001F604", "laughing"),   # 😄
    ("\U0001F606", "laughing"),   # 😆
    ("\U0001F602", "laughing"),   # 😂
    ("\U0001F620", "angry"),      # 😠
    ("\U0001F621", "angry"),      # 😡
    ("\U0001F622", "crying"),     # 😢
    ("\U0001F62D", "crying"),     # 😭
    ("\U0001F614", "sad"),        # 😔
    ("\U0001F61E", "sad"),        # 😞
    ("\U0001F634", "sleepy"),     # 😴
    ("\U0001F62A", "sleepy"),     # 😪
    ("\U0001F914", "doubtful"),   # 🤔
    ("\U0001F615", "doubtful"),   # 😕
    ("\U0001F610", "neutral"),    # 😐
)


def emoji_emotion(ch: str):
    """Map one emoji character to an emotion, or None."""
    for glyph, emotion in _EMOJI_TABLE:
        if ch == glyph:
            return emotion
    return None


def emotion_for(tag: str) -> "str | None":
    """Compatibility shim: accept an emoji or a legacy bracket-tag word."""
    tag = tag.strip()
    if not tag:
        return None
    direct = emoji_emotion(tag)
    if direct:
        return direct
    return EMOTION_ALIASES.get(tag.lower())


# ------------------------------------------------------------------ segments

@dataclass(frozen=True)
class Segment:
    """One settled piece of a transcript.

    Either `kind == "text"` (visible content) or `kind == "emotion"` (a tag
    that was recognised and mapped). Unrecognised tags stay inside text
    segments untouched, so they remain visible instead of vanishing.
    """

    kind: str  # "text" | "emotion"
    value: str

    @property
    def text(self) -> str:
        return self.value if self.kind == "text" else ""

    @property
    def emotion(self) -> Optional[str]:
        return self.value if self.kind == "emotion" else None


# --------------------------------------------------------------- scanning

def _emoji_at(s: str, i: int) -> int:
    """Length in characters of the known emoji starting at s[i], else 0."""
    for glyph, _emotion in _EMOJI_TABLE:
        if s.startswith(glyph, i):
            return len(glyph)
    return 0


def _is_emoji_prefix(s: str, i: int) -> bool:
    """True when s[i:] is an incomplete prefix of some known emoji.

    A transcript delta can be cut mid-emoji. Python gives us whole code points,
    but a 4-byte UTF-8 emoji may still be split across deltas at the byte level,
    arriving as a lone surrogate-ish fragment; holding it back avoids printing a
    broken glyph on screen.
    """
    tail = s[i:]
    if not tail:
        return False
    return any(glyph.startswith(tail) for glyph, _ in _EMOJI_TABLE)


def scan_complete(s: str) -> List[Segment]:
    """Parse a settled string. Nothing is deferred.

    For whole strings that will not grow (a finished transcript, a log line).
    """
    segments: List[Segment] = []
    text_start = 0
    pos = 0
    while pos < len(s):
        if ord(s[pos]) >= 0x80:
            n = _emoji_at(s, pos)
            if n:
                if pos > text_start:
                    segments.append(Segment("text", s[text_start:pos]))
                segments.append(Segment("emotion", emoji_emotion(s[pos:pos + n]) or ""))
                pos += n
                text_start = pos
                continue
        pos += 1
    if text_start < len(s):
        segments.append(Segment("text", s[text_start:]))
    return segments


def parse_segments(s: str) -> List[Segment]:
    """Split a complete string into text and emotion segments."""
    return scan_complete(s)


def clean_text(s: str) -> str:
    """Visible text with recognised emotion emoji removed."""
    return "".join(seg.value for seg in scan_complete(s) if seg.kind == "text")


# Backwards-friendly alias used by the CLI and tests.
strip_emotion_tags = clean_text


def _scan_streaming(s: str) -> Tuple[List[Segment], str]:
    """Scan `s`, deferring a trailing fragment that may become an emoji."""
    segments: List[Segment] = []
    text_start = 0
    pos = 0
    while pos < len(s):
        if ord(s[pos]) >= 0x80:
            n = _emoji_at(s, pos)
            if n:
                if pos > text_start:
                    segments.append(Segment("text", s[text_start:pos]))
                segments.append(Segment("emotion", emoji_emotion(s[pos:pos + n]) or ""))
                pos += n
                text_start = pos
                continue
            if _is_emoji_prefix(s, pos):
                if pos > text_start:
                    segments.append(Segment("text", s[text_start:pos]))
                return segments, s[pos:]
        pos += 1
    if text_start < len(s):
        segments.append(Segment("text", s[text_start:]))
    return segments, ""


# ------------------------------------------------------- streaming accumulator

@dataclass(frozen=True)
class EmotionEvent:
    emotion: str
    """Canonical emotion name, e.g. "happy"."""
    text_before: str
    """Visible text emitted before this tag fired."""
    offset: int
    """Index into the cleaned transcript where this tag appeared."""


class _StreamState:
    """Accumulator for one response's transcript.

    The firmware mirrors this: keep the full raw transcript, re-parse it on
    every delta, and act only on what has not been emitted yet. Re-parsing
    avoids all cross-delta bookkeeping: a later delta can re-split the final text
    segment, so offsets into it are not stable, but comparing the settled text
    against what was already sent is.
    """

    __slots__ = ("raw", "sent_text", "fired")

    def __init__(self) -> None:
        self.raw = ""
        self.sent_text = ""
        self.fired = 0

    def cleaned_text(self) -> str:
        """Visible transcript so far, with recognised emoji removed."""
        return "".join(seg.value for seg in _scan_streaming(self.raw)[0] if seg.kind == "text")


def new_stream_state() -> _StreamState:
    """Create the accumulator used by :func:`advance_emotion_events`."""
    return _StreamState()


def advance_emotion_events(state: _StreamState, delta: str) -> Tuple[List[EmotionEvent], str]:
    """Feed one transcript delta; return (emotion events, newly visible text).

    The returned text is an increment: only characters not returned before.
    Each emoji fires exactly once. Safe when `delta` ends mid-emoji.
    """
    state.raw += delta
    segments, _deferred = _scan_streaming(state.raw)

    events: List[EmotionEvent] = []
    text_segments: List[str] = []
    seen_emotions = 0
    seen_text = 0

    for seg in segments:
        if seg.kind == "text":
            seen_text += len(seg.value)
            text_segments.append(seg.value)
        else:
            seen_emotions += 1
            if seen_emotions > state.fired:
                events.append(
                    EmotionEvent(
                        emotion=seg.value,
                        text_before="".join(text_segments),
                        offset=seen_text,
                    )
                )

    # Emit everything after the common prefix with what was already sent. The
    # settled text can also shrink back when a held-back fragment is released,
    # so a plain "append the new tail" would duplicate output.
    settled = "".join(text_segments)
    common = 0
    for a, b in zip(state.sent_text, settled):
        if a != b:
            break
        common += 1
    new_text = settled[common:]

    state.sent_text = settled
    state.fired = seen_emotions
    return events, new_text


# ------------------------------------------------------------------- payloads

def build_session_update(
    *,
    voice: str = DEFAULT_VOICE,
    input_sample_rate: int = 16000,
    output_sample_rate: int = 24000,
    instructions: Optional[str] = None,
    turn_detection: Optional[dict] = None,
    modalities: Optional[Iterable[str]] = None,
    input_audio_format: str = "pcm",
    output_audio_format: str = "pcm",
    transcription_model: Optional[str] = "qwen3-asr-flash-realtime",
) -> dict:
    """Build the `session.update` event.

    Defaults match StackChan hardware: the ES7210 array feeds 16 kHz and the
    AW88298 plays 24 kHz, so no resampling is needed at either end.
    """
    if turn_detection is None:
        # semantic_vad filters filler words and background noise, which a
        # desktop robot in a room with people will hear constantly.
        turn_detection = {
            "type": "semantic_vad",
            "threshold": 0.5,
            "silence_duration_ms": 800,
        }

    session: dict = {
        "modalities": list(modalities or ["text", "audio"]),
        "audio": {
            "input": {
                "format": {
                    "type": input_audio_format,
                    "sample_rate": input_sample_rate,
                }
            },
            "output": {
                "voice": voice,
                "format": {
                    "type": output_audio_format,
                    "sample_rate": output_sample_rate,
                },
            },
        },
        "turn_detection": turn_detection,
    }

    if instructions:
        session["instructions"] = instructions

    if transcription_model:
        session["input_audio_transcription"] = {"model": transcription_model}

    return {"type": "session.update", "session": session}


# ----------------------------------------------------------------- event names

EV_SESSION_CREATED = "session.created"
EV_SESSION_UPDATED = "session.updated"
EV_SESSION_FINISH = "session.finish"
EV_SESSION_FINISHED = "session.finished"

EV_SPEECH_STARTED = "input_audio_buffer.speech_started"
EV_SPEECH_STOPPED = "input_audio_buffer.speech_stopped"
EV_BUFFER_COMMITTED = "input_audio_buffer.committed"

EV_RESPONSE_CREATED = "response.created"
EV_RESPONSE_DONE = "response.done"
EV_RESPONSE_AUDIO_DELTA = "response.audio.delta"
EV_RESPONSE_AUDIO_DONE = "response.audio.done"
EV_RESPONSE_TRANSCRIPT_DELTA = "response.audio_transcript.delta"
EV_RESPONSE_TRANSCRIPT_DONE = "response.audio_transcript.done"
EV_RESPONSE_TEXT_DELTA = "response.text.delta"
EV_RESPONSE_TEXT_DONE = "response.text.done"

EV_INPUT_TRANSCRIPT_DELTA = "conversation.item.input_audio_transcription.delta"
EV_INPUT_TRANSCRIPT_COMPLETED = "conversation.item.input_audio_transcription.completed"

EV_ERROR = "error"
