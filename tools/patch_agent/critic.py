"""Listening critic for the patch agent: an audio-capable model hears each render and judges it.

The text model proposing parameters cannot hear; it reads measurements. A critic adds a second,
independent signal: a model that takes the render as audio and says how well it matches the
description. Verified with lfm3-omni on Ollama (native /api/chat, audio passed in `images`): 7/8 on
blind tone/noise/silence and bright/dark tests. Small models are biased and coarse, so the critic only
answers constrained questions (a 1-5 match score and three-way choices), and its verdict is advice
passed back to the proposer, plus the tie-breaker for picking the final round.
"""
from __future__ import annotations

import base64
import io
import struct
import wave
from pathlib import Path
from typing import Any, Protocol

from backends import AgentError, Opener, parse_json_text, post_json

CLIP_RATE = 16000

# Descriptive questions first, judgement last. In testing, lfm3-omni described clips reliably (bright or
# dark, tone or noise) but, asked whether a clip *suits* a goal, answered "about right" and 4-5/5 for
# nearly everything. So the critic reports what it hears and the proposer compares that with the goal.
VERDICT_SCHEMA: dict[str, Any] = {
    "type": "object",
    "properties": {
        "brightness": {"type": "string", "enum": ["dark", "medium", "bright"]},
        "character": {"type": "string", "enum": ["soft", "medium", "harsh"]},
        "envelope": {"type": "string", "enum": ["plucked", "sustained"]},
        "notes": {"type": "string"},
        "match": {"type": "integer", "enum": [1, 2, 3, 4, 5]},
    },
    "required": ["brightness", "character", "envelope", "notes", "match"],
    "additionalProperties": False,
}


class Critic(Protocol):
    name: str

    def judge(self, description: str, wav: Path) -> dict[str, Any]:
        """Listen to `wav` and return a verdict matching VERDICT_SCHEMA."""


def read_float_wav(path: Path) -> tuple[int, int, list[float]]:
    """(sample rate, channels, interleaved samples) from a float32 WAV written by tonic2 tools."""
    b = Path(path).read_bytes()
    if b[:4] != b"RIFF" or b[8:12] != b"WAVE":
        raise AgentError(f"{path} is not a WAV file")
    pos, rate, channels, fmt = 12, 0, 0, 0
    while pos + 8 <= len(b):
        cid, size = b[pos:pos + 4], struct.unpack_from("<I", b, pos + 4)[0]
        body = pos + 8
        if cid == b"fmt ":
            fmt, channels, rate = struct.unpack_from("<HHI", b, body)
        elif cid == b"data":
            if fmt != 3:
                raise AgentError(f"{path}: expected 32-bit float samples (format 3), got format {fmt}")
            n = min(size, len(b) - body) // 4
            return rate, channels, list(struct.unpack_from(f"<{n}f", b, body))
        pos = body + size + (size & 1)
    raise AgentError(f"{path} has no data chunk")


def clip_for_listening(path: Path, seconds: float = 3.0, rate: int = CLIP_RATE) -> bytes:
    """Mono 16-bit PCM WAV at `rate`, at most `seconds` long, peak-normalised to -2 dBFS.

    Normalising takes loudness out of the picture (the measurements report level); the critic judges
    timbre. A silent render stays silent.
    """
    src_rate, ch, x = read_float_wav(path)
    mono = [sum(x[i:i + ch]) / ch for i in range(0, len(x) - ch + 1, ch)]
    step = src_rate / rate
    n = min(int(len(mono) / step), int(seconds * rate))
    # Average each output sample's span of input samples: a crude low-pass against aliasing.
    out = []
    for j in range(n):
        a, b_ = int(j * step), max(int(j * step) + 1, int((j + 1) * step))
        seg = mono[a:b_]
        out.append(sum(seg) / len(seg))
    peak = max((abs(v) for v in out), default=0.0)
    gain = 0.8 / peak if peak > 1e-6 else 0.0
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, v * gain)) * 32767)) for v in out))
    return buf.getvalue()


class OllamaAudioCritic:
    """An audio-capable Ollama model (e.g. lfm3-omni) judging renders through the native chat API."""

    def __init__(self, model: str = "lfm3-omni", base_url: str = "http://localhost:11434", num_ctx: int = 8192,
                 seconds: float = 3.0, timeout: float = 300.0, opener: Opener | None = None):
        self.model, self.base_url, self.num_ctx = model, base_url.rstrip("/"), num_ctx
        self.seconds, self.timeout, self.opener = seconds, timeout, opener
        self.name = f"ollama:{model}"

    def judge(self, description: str, wav: Path) -> dict[str, Any]:
        audio = base64.b64encode(clip_for_listening(wav, self.seconds)).decode()
        prompt = (
            "Listen to this synthesizer clip and describe it. Is the tone dark (muffled), medium or bright? "
            "Is its character soft, medium or harsh? Are the notes plucked (fading quickly) or sustained? "
            "Add one short sentence about what you hear. Loudness has been normalised, so ignore volume. "
            f"Finally, rate from 1 to 5 how well it fits this description: {description}"
        )
        reply = post_json(f"{self.base_url}/api/chat", {
            "model": self.model,
            "messages": [{"role": "user", "content": prompt, "images": [audio]}],  # Ollama takes audio here
            "format": VERDICT_SCHEMA,
            "think": False,
            "stream": False,
            "options": {"num_ctx": self.num_ctx, "temperature": 0},
        }, self.timeout, self.opener)
        if "error" in reply:
            raise AgentError(f"critic {self.model}: {reply['error']}")
        verdict = parse_json_text((reply.get("message") or {}).get("content") or "")
        return normalise_verdict(verdict)


def normalise_verdict(v: dict[str, Any]) -> dict[str, Any]:
    """Coerce a verdict into the schema's value set so later code can rely on it."""
    try:
        match = int(v.get("match", 3))
    except (TypeError, ValueError):
        match = 3
    props = VERDICT_SCHEMA["properties"]
    pick = lambda key, fallback: v.get(key) if v.get(key) in props[key]["enum"] else fallback  # noqa: E731
    return {
        "brightness": pick("brightness", "medium"),
        "character": pick("character", "medium"),
        "envelope": pick("envelope", "sustained"),
        "notes": str(v.get("notes", ""))[:300],
        "match": min(5, max(1, match)),
    }
