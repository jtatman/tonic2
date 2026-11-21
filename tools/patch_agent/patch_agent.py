#!/usr/bin/env python3
"""Text-to-patch agent for tonic2: describe a sound, get parameter values for a demo patch.

Loop (step 2 of docs/ai-synthesis-survey.md):
  1. `tonic2_render --layout <demo>` gives the patch's parameters (names, ranges, units, macros).
  2. Claude proposes values as JSON, constrained by a JSON schema (structured outputs).
  3. `tonic2_render <demo> out.wav --set ... --features` renders them and measures the result
     (level, brightness, noisiness, band balance, attack, sustain, tail).
  4. The measurements go back to Claude, which revises or says it is done.

The model cannot hear the audio; the measurements are its ears. Output: a preset JSON with the same
{"params": {...}} shape tonic2_match writes, plus a WAV of the final render.

Usage:
  python patch_agent.py "warm evolving pad with a slow attack" --demo poly --out pad
  python patch_agent.py "squelchy acid bass" --demo mono --rounds 5 --effort high

Credentials: the Anthropic SDK reads ANTHROPIC_API_KEY (or an `ant auth login` profile).
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


class AgentError(RuntimeError):
    pass


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


def ask_claude(client: Any, messages: list[dict[str, Any]], schema: dict[str, Any], effort: str, model: str) -> tuple[Any, dict[str, Any]]:
    """One structured-output request. Returns (response content to append, parsed JSON)."""
    response = client.beta.messages.create(
        model=model,
        max_tokens=16000,
        system=SYSTEM_PROMPT,
        messages=messages,
        output_config={"effort": effort, "format": {"type": "json_schema", "schema": schema}},
        # A policy decline re-runs the request on a fallback model inside the same call.
        betas=["server-side-fallback-2026-07-01"],
        fallbacks="default",
    )
    if response.stop_reason == "refusal":
        details = getattr(response, "stop_details", None)
        raise AgentError(f"request declined: {getattr(details, 'explanation', None) or 'no explanation'}")
    if response.stop_reason == "max_tokens":
        raise AgentError("response hit max_tokens before the JSON was complete")
    text = next((b.text for b in response.content if b.type == "text"), None)
    if text is None:
        raise AgentError(f"no text block in response (stop_reason {response.stop_reason})")
    try:
        data = json.loads(text)
    except json.JSONDecodeError as e:
        raise AgentError(f"response was not valid JSON: {e}") from e
    if not isinstance(data, dict) or not isinstance(data.get("params"), dict):
        raise AgentError("response JSON has no params object")
    return response.content, data


def run_agent(prompt: str, demo: str, renderer: Renderer, client: Any, workdir: Path, rounds: int = 4,
              effort: str = "medium", model: str = MODEL, log=print) -> AgentResult:
    layout = renderer.layout(demo)
    schema = response_schema(layout)
    defaults = clamp_values(layout, {})
    workdir.mkdir(parents=True, exist_ok=True)
    baseline = renderer.render(demo, defaults, workdir / "baseline.wav")
    result = AgentResult(demo=demo, prompt=prompt, baseline=baseline)

    messages: list[dict[str, Any]] = [{
        "role": "user",
        "content": (
            f"Sound to make: {prompt}\n\n"
            f"Patch layout:\n{json.dumps(layout, indent=2)}\n\n"
            f"Measurements of the patch at its defaults:\n{json.dumps(baseline)}\n\n"
            "Propose values for every parameter."
        ),
    }]
    for i in range(1, rounds + 1):
        content, data = ask_claude(client, messages, schema, effort, model)
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
        messages.append({"role": "assistant", "content": content})  # unchanged, thinking blocks included
        clamped = [n for n in values if n in data["params"] and data["params"][n] != values[n]]
        note = f" These values were outside their ranges and were clamped: {', '.join(clamped)}." if clamped else ""
        messages.append({
            "role": "user",
            "content": (
                f"Measurements of your values:\n{json.dumps(features)}\n{note}\n"
                "Compare them with the description and refine, or set done=true if it is as close as it will get."
            ),
        })
    return result


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prompt", help="description of the sound")
    ap.add_argument("--demo", default="poly", help="demo patch to program (default: poly)")
    ap.add_argument("--rounds", type=int, default=4, help="maximum render-and-revise rounds (default: 4)")
    ap.add_argument("--effort", default="medium", choices=["low", "medium", "high", "xhigh", "max"])
    ap.add_argument("--model", default=MODEL)
    ap.add_argument("--out", default="patch", help="output prefix: writes PREFIX.json and PREFIX.wav")
    ap.add_argument("--render", help="path to tonic2_render (default: search build dirs, TONIC2_RENDER, PATH)")
    args = ap.parse_args(argv)

    import anthropic  # imported here so the module and its tests work without the SDK installed

    renderer = BinaryRenderer(find_render_binary(args.render))
    out = Path(args.out)
    workdir = out.parent / (out.name + "_rounds")
    try:
        result = run_agent(args.prompt, args.demo, renderer, anthropic.Anthropic(), workdir, args.rounds, args.effort, args.model)
    except AgentError as e:
        print(f"error: {e}", file=sys.stderr)
        return 1
    except TypeError as e:  # the SDK raises this at request time when it finds no credentials at all
        if "authentication" not in str(e):
            raise
        print("error: no Anthropic credentials found (set ANTHROPIC_API_KEY or run `ant auth login`)", file=sys.stderr)
        return 1
    except anthropic.AuthenticationError:
        print("error: no valid Anthropic credentials (set ANTHROPIC_API_KEY or run `ant auth login`)", file=sys.stderr)
        return 1
    except anthropic.RateLimitError:
        print("error: rate limited by the API; try again shortly", file=sys.stderr)
        return 1
    except anthropic.APIStatusError as e:
        print(f"error: API returned {e.status_code}: {e.message}", file=sys.stderr)
        return 1
    except anthropic.APIConnectionError:
        print("error: could not reach the Anthropic API", file=sys.stderr)
        return 1

    final_wav = out.with_suffix(".wav")
    shutil.copyfile(workdir / f"round{len(result.rounds)}.wav", final_wav)
    out.with_suffix(".json").write_text(json.dumps(result.preset(), indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out.with_suffix('.json')} and {final_wav} after {len(result.rounds)} round(s)")
    return 0


if __name__ == "__main__":
    sys.exit(main())
