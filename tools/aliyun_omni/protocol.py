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

DEFAULT_MODEL = "qwen3.8-omni-flash-realtime"
DEFAULT_VOICE = "Tina"
DEFAULT_URL = "wss://dashscope.aliyuncs.com/api-ws/v1/realtime"

# A complete tag: "[happy]", "[ happy ]".
_TAG_RE = re.compile(r"\[\s*([A-Za-z_]{2,20})\s*\]")

# Longest plausible "[" + tag body + "]" sequence. Bounds how long an
# unterminated "[" can suppress output; past this we assume it is literal text.
_MAX_TAG_LEN = 22

# ------------------------------------------------------------------- emotions

# These are EXACTLY the strings StackChanAvatarDisplay::SetEmotion() accepts.
#   firmware/main/hal/board/stackchan_display.cc:321
# The firmware therefore needs no translation table: whatever this module
# emits can be handed straight to the `llm` event's `emotion` field, which
# Application routes to display->SetEmotion() and on to the avatar:
#
#   happy / laughing -> avatar::Emotion::Happy
#   angry            -> Angry
#   sad / crying     -> Sad
#   sleepy           -> Sleepy  (also plays "Zzz..." and stops idle motion)
#   doubtful         -> Doubt
#   neutral          -> Neutral
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

# The model will not reliably emit our exact identifiers, so accept common
# synonyms and map them onto the six firmware emotions. A tag that matches
# nothing is left alone rather than guessed at.
EMOTION_ALIASES = {
    # neutral
    "neutral": "neutral",
    "calm": "neutral",
    "normal": "neutral",
    # happy family
    "happy": "happy",
    "joy": "happy",
    "joyful": "happy",
    "smile": "happy",
    "smiling": "happy",
    "glad": "happy",
    "cheerful": "happy",
    "excited": "happy",
    "love": "happy",
    # laughing family (distinct on the device: bigger eye curve)
    "laughing": "laughing",
    "laugh": "laughing",
    "lol": "laughing",
    "giggle": "laughing",
    "amused": "laughing",
    # angry
    "angry": "angry",
    "anger": "angry",
    "mad": "angry",
    "annoyed": "angry",
    "furious": "angry",
    # sad family
    "sad": "sad",
    "sadness": "sad",
    "unhappy": "sad",
    "down": "sad",
    "disappointed": "sad",
    # crying family (distinct on the device: tears decorator)
    "crying": "crying",
    "cry": "crying",
    "tears": "crying",
    "sobbing": "crying",
    # doubtful is the string the display matches; "doubt" alone would fall
    # through to its unknown-emotion branch and reset to neutral.
    "doubtful": "doubtful",
    "doubt": "doubtful",
    "confused": "doubtful",
    "puzzled": "doubtful",
    "thinking": "doubtful",
    "curious": "doubtful",
    # sleepy
    "sleepy": "sleepy",
    "tired": "sleepy",
    "sleep": "sleepy",
    "bored": "sleepy",
    "yawn": "sleepy",
}

EMOTION_SYSTEM_PROMPT = (
    "你是一个桌面机器人 StackChan，用简短口语化的中文回应。"
    "说话时请在句首插入一个情绪标记来驱动你的表情，"
    "格式为方括号包住的英文单词，只能从以下八选一："
    "[neutral] [happy] [laughing] [angry] [sad] [crying] [sleepy] [doubtful]。"
    "例如：\"[happy] 好呀，我很乐意！\"。"
    "每句话最多一个标记，不要在标记里加其他文字。"
)


def emotion_for(tag: str) -> Optional[str]:
    """Map an arbitrary tag to one of SUPPORTED_EMOTIONS, or None."""
    return EMOTION_ALIASES.get(tag.strip().lower())


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


def _deferrable_tail(s: str, start: int) -> bool:
    """Could the tail `s[start:]` still grow into a complete tag?"""
    inner = s[start + 1:]
    if "]" in inner or len(inner) > _MAX_TAG_LEN:
        return False
    # Tags never contain whitespace, so a space disqualifies the fragment:
    # "[hello wor" can no longer become a tag and must be shown as text.
    # An empty inner is kept: a bare "[" is genuinely ambiguous, and holding
    # it for one delta costs a literal bracket a few milliseconds of delay
    # while a real tag start never flashes on screen.
    return all(ch.isalnum() or ch == "_" for ch in inner)


def _match_tags(s: str):
    """Tag matches whose text maps to a known emotion."""
    return [m for m in _TAG_RE.finditer(s) if emotion_for(m.group(1)) is not None]


def scan_complete(s: str) -> List[Segment]:
    """Parse a *settled* string. Nothing is deferred.

    Use this for whole strings that will not grow (a finished transcript, a
    log line). An unterminated "[" is emitted as literal text, because in a
    settled string it can never become a tag.
    """
    segments: List[Segment] = []
    pos = 0
    for m in _match_tags(s):
        if m.start() > pos:
            segments.append(Segment("text", s[pos:m.start()]))
        segments.append(Segment("emotion", emotion_for(m.group(1)) or ""))
        pos = m.end()
    if pos < len(s):
        segments.append(Segment("text", s[pos:]))
    return segments


def parse_segments(s: str) -> List[Segment]:
    """Split a complete string into text and emotion segments."""
    return scan_complete(s)


def clean_text(s: str) -> str:
    """Visible text with recognised emotion tags removed."""
    return "".join(seg.value for seg in scan_complete(s) if seg.kind == "text")


# Backwards-friendly alias used by the CLI and tests.
strip_emotion_tags = clean_text


def _deferrable_tail(s: str, start: int) -> bool:
    """Could the tail `s[start:]` still grow into a complete tag?

    Only true while the fragment is still plausible: no closing bracket yet,
    only tag characters so far, and short enough. A space disqualifies it,
    which is what stops "[hello wor" from being held back forever.
    """
    inner = s[start + 1:]
    if "]" in inner or len(inner) > _MAX_TAG_LEN:
        return False
    # Tags never contain whitespace, so a space disqualifies the fragment:
    # "[hello wor" can no longer become a tag and must be shown as text.
    # An empty inner is kept: a bare "[" is genuinely ambiguous, and holding
    # it for one delta costs a literal bracket a few milliseconds of delay
    # while a real tag start never flashes on screen.
    return all(ch.isalnum() or ch == "_" for ch in inner)


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
    every delta, and act only on what has not been emitted yet. At a handful
    of deltas per turn the cost is irrelevant, and it removes all cross-delta
    bookkeeping from the C++ side.

    Deferral lives here and only here. A trailing "[" is withheld until a
    later delta either completes it into a tag or disqualifies it into text.
    """

    __slots__ = ("raw", "sent_text", "fired")

    def __init__(self) -> None:
        self.raw = ""
        # The visible text already returned to the caller. New output is
        # whatever extends this string, which sidesteps the fact that a later
        # delta can re-split or extend the last text segment.
        self.sent_text = ""
        self.fired = 0  # how many emotion segments have fired

    def cleaned_text(self) -> str:
        """Visible transcript so far, with recognised tags removed."""
        return "".join(seg.value for seg in _scan_streaming(self.raw)[0] if seg.kind == "text")


def new_stream_state() -> _StreamState:
    """Create the accumulator used by :func:`advance_emotion_events`."""
    return _StreamState()


def _scan_streaming(s: str) -> Tuple[List[Segment], str]:
    """Scan `s`, deferring a trailing fragment that may still become a tag."""
    segments: List[Segment] = []
    pos = 0

    for m in _match_tags(s):
        if m.start() > pos:
            segments.append(Segment("text", s[pos:m.start()]))
        segments.append(Segment("emotion", emotion_for(m.group(1)) or ""))
        pos = m.end()

    bracket = s.find("[", pos)
    if bracket >= 0 and _deferrable_tail(s, bracket):
        if bracket > pos:
            segments.append(Segment("text", s[pos:bracket]))
        # Everything from the candidate tag onwards stays deferred.
        return segments, s[bracket:]

    if pos < len(s):
        segments.append(Segment("text", s[pos:]))
    return segments, ""


def advance_emotion_events(state: _StreamState, delta: str) -> Tuple[List[EmotionEvent], str]:
    """Feed one transcript delta; return (emotion events, newly visible text).

    The returned text is an increment: only characters that were not returned
    before, so a caller can print it directly without de-duplicating.

    Safe when `delta` ends mid-tag. Each tag fires exactly once. Both
    guarantees rest on comparing the settled visible text against what was
    already sent, rather than on character offsets: re-parsing the whole
    transcript after every delta can re-split the final text segment, and
    offsets into it are therefore not stable.
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

    # Re-parsing the whole transcript means the settled text usually extends
    # what was already sent, but it can also *shrink back*: a lone "[" is
    # emitted as text, then absorbed into a tag one delta later. So emit
    # everything after the common prefix and never more than that. A visual
    # reflow at worst; never a duplicate or a leaked tag fragment.
    settled = "".join(text_segments)
    common = len(os.path.commonprefix([state.sent_text, settled]))
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
