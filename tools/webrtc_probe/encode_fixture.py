#!/usr/bin/env python3
"""Encode the frozen 16 kHz fixture into 20 ms raw Opus packets.

Pipeline (per docs/webrtc-media-m1-host-encoding.md):

    frozen 16k mono s16 PCM
      -> ffmpeg aresample to 48k mono s16   (the device uses its own rate converter;
                                             only the *source* PCM is shared)
      -> libopus opus_encode, 960 samples/frame (20 ms at 48 kHz)
      -> framed file: uint32 BE length + raw Opus payload, one packet per frame

Nothing is uploaded and no credentials are touched: this only reads the frozen
PCM already in the repository and the system libopus/ffmpeg.

Usage::

    python3 encode_fixture.py <out_dir>
"""

from __future__ import annotations

import ctypes as C
import hashlib
import json
import pathlib
import struct
import subprocess
import sys

LIBOPUS = "/opt/homebrew/opt/opus/lib/libopus.0.dylib"
SRC_RATE = 16000
DST_RATE = 48000
CHANNELS = 1
FRAME_SAMPLES = 960          # 20 ms at 48 kHz, per channel
FRAME_BYTES = FRAME_SAMPLES * 2
BITRATE = 90000              # matches the device encoder setting

# Frozen acceptance data. The runner refuses to score a clip whose keywords are
# missing, so they must come from here rather than from a hand-edited manifest.
CLIPS = [
    {"id": "zh_1", "text": "今天我们测试语音连接", "keywords": ["测试", "语音", "连接"]},
    {"id": "zh_2", "text": "桌上有一本蓝色的书", "keywords": ["蓝色", "书"]},
    {"id": "zh_3", "text": "请回答一加一等于几", "keywords": ["一加一", "等于"]},
]

# Silence used to keep the media timeline continuous while ASR is pending.
SILENCE_FRAMES = 60           # 1200 ms, matching the frozen trailing silence
SILENCE_MODE = "dither"       # near-silent room tone, not digital zero


def load_opus():
    lib = C.CDLL(LIBOPUS)
    lib.opus_encoder_create.restype = C.c_void_p
    lib.opus_encoder_create.argtypes = [C.c_int, C.c_int, C.c_int, C.POINTER(C.c_int)]
    lib.opus_encode.restype = C.c_int
    lib.opus_encode.argtypes = [
        C.c_void_p, C.POINTER(C.c_int16), C.c_int, C.POINTER(C.c_ubyte), C.c_int
    ]
    # opus_encoder_ctl is variadic. Without the handle typed, ctypes passes the
    # Python int as a 32-bit C int and truncates the 64-bit encoder pointer on
    # ARM64, which segfaults on the next encode.
    lib.opus_encoder_ctl.restype = C.c_int
    lib.opus_encoder_ctl.argtypes = [C.c_void_p, C.c_int]
    lib.opus_decoder_create.restype = C.c_void_p
    lib.opus_decoder_create.argtypes = [C.c_int, C.c_int, C.POINTER(C.c_int)]
    lib.opus_decode.restype = C.c_int
    lib.opus_decode.argtypes = [
        C.c_void_p, C.POINTER(C.c_ubyte), C.c_int, C.POINTER(C.c_int16), C.c_int, C.c_int
    ]
    lib.opus_get_version_string.restype = C.c_char_p
    return lib


def resample_16_to_48(pcm16: bytes) -> bytes:
    """ffmpeg does only the rate conversion; the encoder stays libopus."""
    p = subprocess.run(
        ["ffmpeg", "-hide_banner", "-loglevel", "error", "-nostdin",
         "-f", "s16le", "-ar", str(SRC_RATE), "-ac", "1", "-i", "pipe:0",
         "-af", f"aresample={DST_RATE}", "-ar", str(DST_RATE), "-ac", "1",
         "-c:a", "pcm_s16le", "-f", "s16le", "pipe:1"],
        input=pcm16, capture_output=True, check=True,
    )
    return p.stdout


def main() -> int:
    out_dir = pathlib.Path(sys.argv[1] if len(sys.argv) > 1 else "host_media")
    out_dir.mkdir(parents=True, exist_ok=True)
    fixture_dir = pathlib.Path(__file__).resolve().parents[2] / "firmware/main/hal/webrtc/fixture"

    lib = load_opus()
    ver = lib.opus_get_version_string().decode()

    manifest = {
        "libopus": ver,
        "src_rate": SRC_RATE,
        "dst_rate": DST_RATE,
        "channels": CHANNELS,
        "frame_ms": 20,
        "frame_samples": FRAME_SAMPLES,
        "frame_bytes": FRAME_BYTES,
        "bitrate": BITRATE,
        "resampler": f"ffmpeg aresample={DST_RATE}",
        "clips": [],
    }

    for clip_meta in CLIPS:
        cid = clip_meta["id"]
        src = (fixture_dir / f"{cid}.pcm").read_bytes()
        err = C.c_int(0)
        # OPUS_APPLICATION_AUDIO = 2049, matching the device encoder.
        enc = lib.opus_encoder_create(DST_RATE, CHANNELS, 2049, C.byref(err))
        if err.value != 0 or not enc:
            raise SystemExit(f"opus_encoder_create failed: {err.value}")
        # OPUS_SET_BITRATE_REQUEST = 4002, OPUS_SET_VBR_REQUEST = 4006,
        # OPUS_SET_DTX_REQUEST = 4016, OPUS_SET_COMPLEXITY_REQUEST = 4010
        for req, val in ((4002, BITRATE), (4006, 1), (4016, 0), (4010, 5)):
            lib.opus_encoder_ctl(C.c_void_p(enc), C.c_int(req), C.c_int(val))

        pcm48 = resample_16_to_48(src)

        # Pad the tail to a whole frame; the device rounds up and then adds one
        # all-silence frame, which is recorded here rather than assumed equal.
        rem = len(pcm48) % FRAME_BYTES
        pad_samples = 0
        if rem:
            pad_samples = (FRAME_BYTES - rem) // 2
            pcm48 += b"\x00" * (FRAME_BYTES - rem)

        dec = lib.opus_decoder_create(DST_RATE, CHANNELS, C.byref(err))
        if err.value != 0 or not dec:
            raise SystemExit(f"opus_decoder_create failed: {err.value}")
        frames = []
        total_samples = len(pcm48) // 2
        buf = (C.c_ubyte * 4000)()
        for off in range(0, total_samples, FRAME_SAMPLES):
            chunk = pcm48[off * 2:(off + FRAME_SAMPLES) * 2]
            if len(chunk) < FRAME_BYTES:
                chunk += b"\x00" * (FRAME_BYTES - len(chunk))
            arr = (C.c_int16 * FRAME_SAMPLES).from_buffer_copy(chunk)
            n = lib.opus_encode(C.c_void_p(enc), arr, FRAME_SAMPLES, buf, 4000)
            if n <= 0:
                raise SystemExit(f"opus_encode failed: {n}")
            pkt = bytes(buf[:n])
            # Local decodability check: a packet that will not decode is not
            # valid media, whatever the encoder returned.
            out = (C.c_int16 * (FRAME_SAMPLES * 6))()
            dn = lib.opus_decode(C.c_void_p(dec),
                                 (C.c_ubyte * len(pkt)).from_buffer_copy(pkt),
                                 len(pkt), out, FRAME_SAMPLES * 6, 0)
            if dn <= 0:
                raise SystemExit(f"local decode failed for {cid} frame {len(frames)}")
            frames.append(pkt)

        framed = b"".join(struct.pack(">I", len(p)) + p for p in frames)
        out_path = out_dir / f"{cid}.opusframes"
        out_path.write_bytes(framed)
        sizes = [len(p) for p in frames]
        manifest["clips"].append({
            "id": cid,
            "text": clip_meta["text"],
            "keywords": clip_meta["keywords"],
            "src_sha256": hashlib.sha256(src).hexdigest(),
            "src_samples": len(src) // 2,
            "pcm48_sha256": hashlib.sha256(pcm48).hexdigest(),
            "pcm48_samples": total_samples,
            "tail_pad_samples": pad_samples,
            "packets": len(frames),
            "media_ms": len(frames) * 20,
            "framed_bytes": len(framed),
            "pkt_min": min(sizes),
            "pkt_max": max(sizes),
            "framed_sha256": hashlib.sha256(framed).hexdigest(),
        })
        print(f"{cid}: {len(frames)} packets, {len(frames)*20} ms, "
              f"pkt {min(sizes)}..{max(sizes)} B, tail pad {pad_samples} samples")

    # ---- timeline-continuation silence, generated here so it is reproducible
    # ---- and carries its own decoding/energy evidence (H4).
    import random
    random.seed(7)
    err2 = C.c_int(0)
    senc = lib.opus_encoder_create(DST_RATE, CHANNELS, 2049, C.byref(err2))
    if err2.value != 0 or not senc:
        raise SystemExit(f"silence encoder create failed: {err2.value}")
    sdec = lib.opus_decoder_create(DST_RATE, CHANNELS, C.byref(err2))
    sbuf = (C.c_ubyte * 4000)()
    sout = (C.c_int16 * 5760)()
    sil = []
    energies = []
    for _ in range(SILENCE_FRAMES):
        if SILENCE_MODE == "dither":
            pcm = [random.randint(-24, 24) for _ in range(FRAME_SAMPLES)]
        else:
            pcm = [0] * FRAME_SAMPLES
        arr = (C.c_int16 * FRAME_SAMPLES)(*pcm)
        n = lib.opus_encode(C.c_void_p(senc), arr, FRAME_SAMPLES, sbuf, 4000)
        if n <= 0:
            raise SystemExit(f"silence encode failed: {n}")
        pkt = bytes(sbuf[:n])
        dn = lib.opus_decode(C.c_void_p(sdec), (C.c_ubyte * n).from_buffer_copy(pkt),
                             n, sout, 5760, 0)
        if dn != FRAME_SAMPLES:
            raise SystemExit(f"silence decode gave {dn} samples, expected {FRAME_SAMPLES}")
        peak = max(abs(sout[i]) for i in range(dn))
        energies.append(peak)
        sil.append(__import__("base64").b64encode(pkt).decode())
    (out_dir / "silence_packets.json").write_text(
        __import__("json").dumps(
            {"frames": len(sil), "ms": len(sil) * 20, "mode": SILENCE_MODE,
             "decoded_samples": FRAME_SAMPLES, "peak_abs": max(energies),
             "generator": "tools/webrtc_probe/encode_fixture.py",
             "silence": sil}) + "\n")
    manifest["silence"] = {"frames": len(sil), "ms": len(sil) * 20,
                           "mode": SILENCE_MODE, "decoded_samples": FRAME_SAMPLES,
                           "peak_abs": max(energies)}
    print(f"silence: {len(sil)} frames, mode={SILENCE_MODE}, "
          f"decoded {FRAME_SAMPLES} samples/frame, peak |x|={max(energies)}")

    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, ensure_ascii=False, indent=2) + "\n")
    print(f"\nlibopus {ver}; manifest -> {out_dir / 'manifest.json'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
