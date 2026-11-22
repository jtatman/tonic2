#!/usr/bin/env python3
"""Text-to-patch agent for tonic2: describe a sound, get parameter values for a demo patch.

Loop (step 2 of docs/ai-synthesis-survey.md):
  1. `tonic2_render --layout <demo>` gives the patch's parameters (names, ranges, units, macros).
  2. An LLM proposes values as JSON, constrained by a JSON schema built from the layout.
  3. `tonic2_render <demo> out.wav --set ... --features` renders them and measures the result
     (level, brightness, noisiness, band balance, attack, sustain, tail).
  4. The measurements go back to the model, which revises or says it is done.

Backends (--backend, see backends.py): anthropic (API credits), claude-code (the `claude` CLI and its
subscription), ollama (Ollama's native API), openai (any OpenAI-style server: llama.cpp, LM Studio).
A done flag counts only from --min-rounds on. --critic MODEL adds an audio-capable model (critic.py)
that listens to every render; its verdicts go back to the proposer and pick the round that is kept.

The model cannot hear the audio; the measurements are its ears. Output: a preset JSON with the same
{"params": {...}} shape tonic2_match writes, plus a WAV of the final render.

Usage:
  python patch_agent.py "warm evolving pad with a slow attack" --demo poly --out pad
  python patch_agent.py "squelchy acid bass" --demo mono --backend claude-code --effort high
  python patch_agent.py "glassy bell" --backend ollama --model lfm3 --critic lfm3-omni --min-rounds 3

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
from backends import AgentError, AnthropicBackend, Backend, ClaudeCodeBackend, OllamaBackend, OpenAICompatBackend  # noqa: E402
from critic import Critic, OllamaAudioCritic  # noqa: E402

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
        # Absolute path: CreateProcess on Windows rejects some relative forms ("dir/sub/x.exe").
        self.binary = str(Path(binary).resolve()) if Path(binary).is_file() else binary

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
    verdict: dict[str, Any] | None = None  # listening critic's judgement of this render, if a critic ran
    wav: str = ""


@dataclass
class AgentResult:
    demo: str
    prompt: str
    baseline: dict[str, float]
    rounds: list[Round] = field(default_factory=list)

    @property
    def final(self) -> Round:
        return self.rounds[-1]

    @property
    def best_index(self) -> int:
        """The round to keep: the critic's highest match score (latest on ties), else the last round."""
        scored = [(r.verdict["match"], i) for i, r in enumerate(self.rounds) if r.verdict]
        return max(scored)[1] if scored else len(self.rounds) - 1

    @property
    def best(self) -> Round:
        return self.rounds[self.best_index]

    def preset(self) -> dict[str, Any]:
        best = self.best
        return {
            "demo": self.demo,
            "prompt": self.prompt,
            "params": best.values,
            "features": best.features,
            "rationale": best.rationale,
            "verdict": best.verdict,
            "chosen_round": self.best_index + 1,
            "rounds": len(self.rounds),
            "history": [{"values": r.values, "verdict": r.verdict, "centroid_hz": r.features.get("centroid_hz"),
                         "rms_db": r.features.get("rms_db")} for r in self.rounds],
        }


def describe_verdict(v: dict[str, Any]) -> str:
    return (f"sounds {v['brightness']}, {v['character']}, {v['envelope']}"
            + (f" (\"{v['notes']}\")" if v.get("notes") else "") + f"; rated {v['match']}/5 against the description")


def run_agent(prompt: str, demo: str, renderer: Renderer, backend: Backend, workdir: Path, rounds: int = 4,
              log=print, min_rounds: int = 1, critic: Critic | None = None) -> AgentResult:
    """Propose, render, measure (and listen), feed back. A done flag counts only from round `min_rounds` on:
    small models tend to declare victory immediately, which leaves the feedback unused."""
    min_rounds = max(1, min(min_rounds, rounds))
    layout = renderer.layout(demo)
    schema = response_schema(layout)
    backend.start(SYSTEM_PROMPT, schema)
    defaults = clamp_values(layout, {})
    workdir.mkdir(parents=True, exist_ok=True)
    baseline = renderer.render(demo, defaults, workdir / "baseline.wav")
    result = AgentResult(demo=demo, prompt=prompt, baseline=baseline)

    listener = ""
    if critic:
        verdict = critic.judge(prompt, workdir / "baseline.wav")
        listener = f"\nA listener heard the defaults: {describe_verdict(verdict)}.\n"
    message = (
        f"Sound to make: {prompt}\n\n"
        f"Patch layout:\n{json.dumps(layout, indent=2)}\n\n"
        f"Measurements of the patch at its defaults:\n{json.dumps(baseline)}\n{listener}\n"
        f"You have {rounds} rounds; at least {min_rounds} will run, so plan to refine. "
        "Propose values for every parameter."
    )
    for i in range(1, rounds + 1):
        data = backend.ask(message)
        if not isinstance(data.get("params"), dict):
            raise AgentError("reply JSON has no params object")
        values = clamp_values(layout, data["params"])
        wav = workdir / f"round{i}.wav"
        features = renderer.render(demo, values, wav)
        verdict = critic.judge(prompt, wav) if critic else None
        rnd = Round(values=values, features=features, rationale=str(data.get("rationale", "")),
                    done=bool(data.get("done", False)), verdict=verdict, wav=str(wav))
        result.rounds.append(rnd)
        log(f"round {i}: {json.dumps(values)}")
        log(f"  {rnd.rationale}")
        log(f"  centroid {features.get('centroid_hz', 0):.0f} Hz, rms {features.get('rms_db', 0):.1f} dBFS, "
            f"attack {features.get('attack_ms', 0):.0f} ms, sustain {features.get('sustain_ratio', 0):.2f}"
            + (f" | listener: {describe_verdict(verdict)}" if verdict else ""))
        if rnd.done and i >= min_rounds:
            break
        clamped = [n for n in values if n in data["params"] and data["params"][n] != values[n]]
        parts = [f"Measurements of your values (round {i}):\n{json.dumps(features)}"]
        if verdict:
            parts.append(f"A listener heard this render: {describe_verdict(verdict)}.")
        if clamped:
            parts.append(f"These values were outside their ranges and were clamped: {', '.join(clamped)}.")
        if i < min_rounds:
            parts.append(f"This is round {i}; at least {min_rounds} rounds are required, so change something: "
                         "compare each measurement with the description and move the parameters that fix the biggest gap.")
        else:
            parts.append("Compare them with the description and refine, or set done=true if it is as close as it will get.")
        message = "\n".join(parts)
    return result


def make_backend(args: argparse.Namespace) -> Backend:
    if args.backend == "anthropic":
        return AnthropicBackend(model=args.model or MODEL, effort=args.effort)
    if args.backend == "claude-code":
        return ClaudeCodeBackend(binary=args.claude_bin, model=args.model or "opus", effort=args.effort)
    if args.backend == "ollama":
        return OllamaBackend(base_url=args.ollama_url, model=args.model or "lfm3", think=args.think, num_ctx=args.num_ctx)
    reasoning = None if args.local_reasoning == "omit" else args.local_reasoning
    return OpenAICompatBackend(base_url=args.base_url, model=args.model or "qwen3:4b", api_key=os.environ.get("OPENAI_API_KEY"),
                               reasoning=reasoning)


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prompt", help="description of the sound")
    ap.add_argument("--demo", default="poly", help="demo patch to program (default: poly)")
    ap.add_argument("--rounds", type=int, default=4, help="maximum render-and-revise rounds (default: 4)")
    ap.add_argument("--min-rounds", type=int, default=2,
                    help="rounds that always run before a done flag is accepted (default: 2)")
    ap.add_argument("--backend", default="anthropic", choices=["anthropic", "claude-code", "ollama", "openai"],
                    help="anthropic: API credits; claude-code: the `claude` CLI and its subscription; "
                         "ollama: Ollama's native API; openai: any OpenAI-style server (default: anthropic)")
    ap.add_argument("--model", help=f"model name (defaults: anthropic {MODEL}, claude-code opus, ollama lfm3, openai qwen3:4b)")
    ap.add_argument("--ollama-url", default="http://localhost:11434", help="Ollama server for the ollama backend and the critic")
    ap.add_argument("--think", action="store_true", help="ollama backend: let thinking models reason (slow; off by default)")
    ap.add_argument("--num-ctx", type=int, default=8192, help="ollama: context window per request (default: 8192)")
    ap.add_argument("--critic", metavar="MODEL", help="audio-capable Ollama model that listens to each render (e.g. lfm3-omni)")
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
    critic = OllamaAudioCritic(model=args.critic, base_url=args.ollama_url, num_ctx=args.num_ctx) if args.critic else None
    print(f"backend {backend.name}, demo {args.demo}, {args.min_rounds}-{args.rounds} round(s)"
          + (f", critic {critic.name}" if critic else ""))
    try:
        result = run_agent(args.prompt, args.demo, renderer, backend, workdir, args.rounds,
                           min_rounds=args.min_rounds, critic=critic)
    except AgentError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1

    final_wav = out.with_suffix(".wav")
    shutil.copyfile(result.best.wav, final_wav)
    preset = result.preset() | {"backend": backend.name, "critic": critic.name if critic else None}
    out.with_suffix(".json").write_text(json.dumps(preset, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out.with_suffix('.json')} and {final_wav}: round {result.best_index + 1} of {len(result.rounds)}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
