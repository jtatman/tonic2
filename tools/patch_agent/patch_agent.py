#!/usr/bin/env python3
"""Text-to-patch agent for tonic2: describe a sound, get parameter values for a demo patch.

Loop (step 2 of docs/ai-synthesis-survey.md):
  1. `tonic2_render --layout <demo>` gives the patch's parameters (names, ranges, units, macros).
  2. An LLM proposes values as JSON, constrained by a JSON schema built from the layout.
  3. `tonic2_render <demo> out.wav --set ... --features` renders them and measures the result
     (level, brightness, noisiness, band balance, attack, sustain, tail).
  4. The measurements go back to the model, which revises or says it is done.

Backends (--backend, see backends.py): anthropic (API credits), claude-code (the `claude` CLI and its
subscription), openai (any local OpenAI-style server: Ollama, llama.cpp, LM Studio).

The model cannot hear the audio; the measurements are its ears. Output: a preset JSON with the same
{"params": {...}} shape tonic2_match writes, plus a WAV of the final render.

Usage:
  python patch_agent.py "warm evolving pad with a slow attack" --demo poly --out pad
  python patch_agent.py "squelchy acid bass" --demo mono --backend claude-code --effort high
  python patch_agent.py "glassy bell" --backend openai --model qwen3:4b   # local Ollama

Credentials: anthropic reads ANTHROPIC_API_KEY or an `ant auth login` profile; claude-code uses the
CLI's own login; openai sends OPENAI_API_KEY as a bearer token if set (local servers ignore it).
"""
from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass, field
from pathlib import Path
from typing import Any, Protocol

sys.path.insert(0, str(Path(__file__).resolve().parent))
from backends import AgentError, AnthropicBackend, Backend, ClaudeCodeBackend, OpenAICompatBackend  # noqa: E402

MODEL = "claude-opus-5-5"

SYSTEM_PROMPT = """You are a sound designer programming a software synthesizer patch.

You get a sound description, the patch's parameters (name, range, unit, default, whether the range is
logarithmic, and which parameters a macro drives), and measurements of renders. You cannot hear the
audio: the measurements are your only feedback, so reason from them.

Measurements:
- peak_db, rms_db: level in dBFS. Keep peaks below -1 dBFS; around -20 to -10 dBFS RMS is a healthy level.
- centroid_hz: spectral centroid, the main "brightness" cue. rolloff_hz: 85% of energy lies below it.
- flatness: 0 for pure tones, toward 1 for noise.
- band_low (<200 Hz), band_low_mid (200 Hz-2 kHz), band_high_mid (2-8 kHz), band_high (>8 kHz): energy shares.
- attack_ms: onset to 90% of the first peak. sustain_ratio: mid-render level relative to the peak
  (near 1 = held, near 0 = plucked). tail_db: level in the last 100 ms relative to the peak.

Notes come from a fixed script you cannot change; only the parameters are yours. Macros add to the
parameters they drive, so account for both. Stay inside each range.

Each turn, return every parameter value, a short rationale tied to the measurements, and done=true
once further changes would not bring the sound closer to the description."""


class Renderer(Protocol):
    def layout(self, demo: str) -> dict[str, Any]: ...
    def render(self, demo: str, values: dict[str, float], out: Path) -> dict[str, float]: ...


class BinaryRenderer:
    """Drives the tonic2_render executable."""

    def __init__(self, binary: str):
        self.binary = binary

    def _run(self, args: list[str]) -> str:
        try:
            proc = subprocess.run([self.binary, *args], capture_output=True, text=True, check=False)
        except OSError as e:
            raise AgentError(f"could not run {self.binary}: {e}") from e
        if proc.returncode != 0:
            raise AgentError(f"tonic2_render {' '.join(args)} failed: {proc.stderr.strip()}")
        return proc.stdout

    def layout(self, demo: str) -> dict[str, Any]:
        return json.loads(self._run(["--layout", demo]))

    def render(self, demo: str, values: dict[str, float], out: Path) -> dict[str, float]:
        settings = ",".join(f"{name}={value:.6g}" for name, value in values.items())
        return json.loads(self._run([demo, str(out), "--set", settings, "--features"]))


def find_render_binary(explicit: str | None) -> str:
    if explicit:
        if not Path(explicit).is_file():
            raise SystemExit(f"--render {explicit}: no such file")
        return str(Path(explicit).resolve())
    candidates = [os.environ.get("TONIC2_RENDER")]
    root = Path(__file__).resolve().parents[2]
    for build in ("build", "build-wsl", "build-win/Release", "build-win/Debug"):
        candidates.append(str(root / build / ("tonic2_render.exe" if os.name == "nt" else "tonic2_render")))
    candidates.append(shutil.which("tonic2_render"))
    for c in candidates:
        if c and Path(c).is_file():
            return str(Path(c).resolve())  # CreateProcess on Windows rejects some relative forms
    raise SystemExit("tonic2_render not found: build tonic2 or pass --render PATH (or set TONIC2_RENDER)")


def response_schema(layout: dict[str, Any]) -> dict[str, Any]:
    names = [p["name"] for p in layout["params"]]
    return {
        "type": "object",
        "properties": {
            "params": {
                "type": "object",
                "properties": {n: {"type": "number"} for n in names},
                "required": names,
                "additionalProperties": False,
            },
            "rationale": {"type": "string"},
            "done": {"type": "boolean"},
        },
        "required": ["params", "rationale", "done"],
        "additionalProperties": False,
    }


def clamp_values(layout: dict[str, Any], values: dict[str, Any]) -> dict[str, float]:
    """Every layout parameter, clamped into its range; missing or non-numeric values fall back to the default."""
    out: dict[str, float] = {}
    for p in layout["params"]:
        v = values.get(p["name"], p["default"])
        try:
            v = float(v)
        except (TypeError, ValueError):
            v = float(p["default"])
        out[p["name"]] = min(max(v, float(p["min"])), float(p["max"]))
    return out


@dataclass
class Round:
    values: dict[str, float]
    features: dict[str, float]
    rationale: str
    done: bool


@dataclass
class AgentResult:
    demo: str
    prompt: str
    baseline: dict[str, float]
    rounds: list[Round] = field(default_factory=list)

    @property
    def final(self) -> Round:
        return self.rounds[-1]

    def preset(self) -> dict[str, Any]:
        return {
            "demo": self.demo,
            "prompt": self.prompt,
            "params": self.final.values,
            "features": self.final.features,
            "rationale": self.final.rationale,
            "rounds": len(self.rounds),
        }


def run_agent(prompt: str, demo: str, renderer: Renderer, backend: Backend, workdir: Path, rounds: int = 4,
              log=print) -> AgentResult:
    layout = renderer.layout(demo)
    schema = response_schema(layout)
    backend.start(SYSTEM_PROMPT, schema)
    defaults = clamp_values(layout, {})
    workdir.mkdir(parents=True, exist_ok=True)
    baseline = renderer.render(demo, defaults, workdir / "baseline.wav")
    result = AgentResult(demo=demo, prompt=prompt, baseline=baseline)

    message = (
        f"Sound to make: {prompt}\n\n"
        f"Patch layout:\n{json.dumps(layout, indent=2)}\n\n"
        f"Measurements of the patch at its defaults:\n{json.dumps(baseline)}\n\n"
        "Propose values for every parameter."
    )
    for i in range(1, rounds + 1):
        data = backend.ask(message)
        if not isinstance(data.get("params"), dict):
            raise AgentError("reply JSON has no params object")
        values = clamp_values(layout, data["params"])
        features = renderer.render(demo, values, workdir / f"round{i}.wav")
        rnd = Round(values=values, features=features, rationale=str(data.get("rationale", "")), done=bool(data.get("done", False)))
        result.rounds.append(rnd)
        log(f"round {i}: {json.dumps(values)}")
        log(f"  {rnd.rationale}")
        log(f"  centroid {features.get('centroid_hz', 0):.0f} Hz, rms {features.get('rms_db', 0):.1f} dBFS, "
            f"attack {features.get('attack_ms', 0):.0f} ms, sustain {features.get('sustain_ratio', 0):.2f}")
        if rnd.done:
            break
        clamped = [n for n in values if n in data["params"] and data["params"][n] != values[n]]
        note = f" These values were outside their ranges and were clamped: {', '.join(clamped)}." if clamped else ""
        message = (
            f"Measurements of your values:\n{json.dumps(features)}\n{note}\n"
            "Compare them with the description and refine, or set done=true if it is as close as it will get."
        )
    return result


def make_backend(args: argparse.Namespace) -> Backend:
    if args.backend == "anthropic":
        return AnthropicBackend(model=args.model or MODEL, effort=args.effort)
    if args.backend == "claude-code":
        return ClaudeCodeBackend(binary=args.claude_bin, model=args.model or "opus", effort=args.effort)
    reasoning = None if args.local_reasoning == "omit" else args.local_reasoning
    return OpenAICompatBackend(base_url=args.base_url, model=args.model or "qwen3:4b", api_key=os.environ.get("OPENAI_API_KEY"),
                               reasoning=reasoning)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prompt", help="description of the sound")
    ap.add_argument("--demo", default="poly", help="demo patch to program (default: poly)")
    ap.add_argument("--rounds", type=int, default=4, help="maximum render-and-revise rounds (default: 4)")
    ap.add_argument("--backend", default="anthropic", choices=["anthropic", "claude-code", "openai"],
                    help="anthropic: API credits; claude-code: the `claude` CLI and its subscription; "
                         "openai: a local OpenAI-style server such as Ollama (default: anthropic)")
    ap.add_argument("--model", help=f"model name (defaults: anthropic {MODEL}, claude-code opus, openai qwen3:4b)")
    ap.add_argument("--effort", default="medium", choices=["low", "medium", "high", "xhigh", "max"],
                    help="reasoning effort for the Claude backends (default: medium)")
    ap.add_argument("--base-url", default="http://127.0.0.1:11434/v1", help="openai backend endpoint (default: local Ollama)")
    ap.add_argument("--local-reasoning", default="none", choices=["none", "low", "medium", "high", "omit"],
                    help="openai backend: reasoning_effort to request; 'none' disables thinking (fast), "
                         "'omit' leaves the field out for servers that reject it (default: none)")
    ap.add_argument("--claude-bin", default="claude", help="Claude Code CLI executable (default: claude)")
    ap.add_argument("--out", default="patch", help="output prefix: writes PREFIX.json and PREFIX.wav")
    ap.add_argument("--render", help="path to tonic2_render (default: search build dirs, TONIC2_RENDER, PATH)")
    args = ap.parse_args(argv)

    renderer = BinaryRenderer(find_render_binary(args.render))
    out = Path(args.out)
    workdir = out.parent / (out.name + "_rounds")
    backend = make_backend(args)
    print(f"backend {backend.name}, demo {args.demo}, up to {args.rounds} round(s)")
    try:
        result = run_agent(args.prompt, args.demo, renderer, backend, workdir, args.rounds)
    except AgentError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    final_wav = out.with_suffix(".wav")
    shutil.copyfile(workdir / f"round{len(result.rounds)}.wav", final_wav)
    preset = result.preset() | {"backend": backend.name}
    out.with_suffix(".json").write_text(json.dumps(preset, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out.with_suffix('.json')} and {final_wav} after {len(result.rounds)} round(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
