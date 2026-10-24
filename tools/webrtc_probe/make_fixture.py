#!/usr/bin/env python3
"""Generate the M1 probe fixture: three 16 kHz mono s16 PCM clips.

Requirements (M1 task §"冻结的 fixture 与验收口径"):

- 16 kHz / mono / s16 source PCM
- at least 500 ms of leading silence and 1200 ms of trailing silence per clip
  (the trailing silence must cover the configured 800 ms VAD silence threshold
  with margin, otherwise the server never observes end-of-speech)
- reproducible locally without new cloud dependencies: macOS ``say`` + stdlib
- record voice, text, format, sample counts, SHA256 and per-clip keywords

Outputs (all under the target directory):

- ``<id>.pcm``   headerless s16le, the file embedded into the M1 firmware
- ``<id>.wav``   the same audio with a RIFF header, for listening/inspection
- ``manifest.json``  parameters, counts, hashes and keywords

Usage::

    python3 make_fixture.py [output_dir]
"""

from __future__ import annotations

import hashlib
import json
import pathlib
import struct
import subprocess
import sys
import wave

SAMPLE_RATE = 16000
CHANNELS = 1
SAMPLE_WIDTH = 2  # bytes, s16le

LEAD_MS = 500
TAIL_MS = 1200

# macOS `say` voice. Tingting is the long-standing zh_CN voice; the script
# records whatever it actually used in the manifest.
VOICE = "Tingting"

# Each clip: id, sentence, keywords the completed transcription must contain.
# Keywords are deliberately short and unambiguous so that the acceptance check
# does not depend on punctuation or number formatting by the ASR model.
CLIPS = [
    {
        "id": "zh_1",
        "text": "今天我们测试语音连接",
        "keywords": ["测试", "语音", "连接"],
        "gloss": "today we test the voice connection",
    },
    {
        "id": "zh_2",
        "text": "桌上有一本蓝色的书",
        "keywords": ["蓝色", "书"],
        "gloss": "there is a blue book on the table",
    },
    {
        "id": "zh_3",
        "text": "请回答一加一等于几",
        "keywords": ["一加一", "等于"],
        "gloss": "please answer what one plus one equals",
    },
]


def silence(samples: int) -> bytes:
    """s16le silence of the given per-channel sample count."""
    return b"\x00\x00" * samples


def synth(text: str, voice: str, dst: pathlib.Path) -> None:
    """Synthesise `text` straight to 16 kHz mono s16 WAV using macOS `say`."""
    subprocess.run(
        [
            "say",
            "-v", voice,
            "--data-format=LEI16@16000",
            "--file-format=WAVE",
            "-o", str(dst),
            text,
        ],
        check=True,
        capture_output=True,
    )


def read_wav(path: pathlib.Path) -> bytes:
    """Return headerless s16le frames, rejecting anything not 16k/mono/s16."""
    with wave.open(str(path)) as w:
        if (w.getnchannels(), w.getframerate(), w.getsampwidth()) != (
            CHANNELS, SAMPLE_RATE, SAMPLE_WIDTH
        ):
            raise SystemExit(
                f"{path}: expected {SAMPLE_RATE} Hz / {CHANNELS} ch / "
                f"{SAMPLE_WIDTH * 8} bit, got {w.getframerate()} Hz / "
                f"{w.getnchannels()} ch / {w.getsampwidth() * 8} bit"
            )
        return w.readframes(w.getnframes())


def write_wav(path: pathlib.Path, frames: bytes) -> None:
    with wave.open(str(path), "wb") as w:
        w.setnchannels(CHANNELS)
        w.setsampwidth(SAMPLE_WIDTH)
        w.setframerate(SAMPLE_RATE)
        w.writeframes(frames)


def main() -> int:
    out_dir = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "fixture")
    out_dir.mkdir(parents=True, exist_ok=True)

    lead = silence(SAMPLE_RATE * LEAD_MS // 1000)
    tail = silence(SAMPLE_RATE * TAIL_MS // 1000)

    manifest = {
        "sample_rate": SAMPLE_RATE,
        "channels": CHANNELS,
        "sample_format": "s16le",
        "lead_silence_ms": LEAD_MS,
        "tail_silence_ms": TAIL_MS,
        "voice": VOICE,
        "generator": "tools/webrtc_probe/make_fixture.py",
        "clips": [],
    }

    tmp = out_dir / "_say.wav"
    for clip in CLIPS:
        synth(clip["text"], VOICE, tmp)
        speech = read_wav(tmp)

        frames = lead + speech + tail
        pcm_path = out_dir / f"{clip['id']}.pcm"
        wav_path = out_dir / f"{clip['id']}.wav"
        pcm_path.write_bytes(frames)
        write_wav(wav_path, frames)

        total = len(frames) // SAMPLE_WIDTH
        manifest["clips"].append(
            {
                **clip,
                "speech_samples": len(speech) // SAMPLE_WIDTH,
                "total_samples": total,
                "speech_ms": round(len(speech) / SAMPLE_WIDTH / SAMPLE_RATE * 1000, 1),
                "total_ms": round(total / SAMPLE_RATE * 1000, 1),
                "pcm_bytes": len(frames),
                "sha256_pcm": hashlib.sha256(frames).hexdigest(),
            }
        )
        print(
            f"{clip['id']}: speech {manifest['clips'][-1]['speech_ms']} ms, "
            f"total {manifest['clips'][-1]['total_ms']} ms, "
            f"{len(frames)} bytes, sha256 {manifest['clips'][-1]['sha256_pcm'][:16]}…"
        )

    tmp.unlink(missing_ok=True)
    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n"
    )
    print(f"\nmanifest written to {out_dir / 'manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
