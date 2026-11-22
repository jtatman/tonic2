#!/usr/bin/env python3
"""Benchmark the patch agent across models, and test whether an audio model can hear.

  agent   Run every proposer x critic combination on a prompt set and score the kept render with
          direction checks against the patch defaults (brighter, quieter, more resonant ...).
  listen  Blind A/B questions with known answers (tone or noise, silent or not, bright or dark synth)
          to see whether an audio-capable model actually hears clips. A model that cannot hear scores
          about half: every question is asked once with each answer being right.

Proposer spec: BACKEND:MODEL[@URL], e.g.
  ollama:ornith:9b@http://10.209.1.214:11434   ollama:lfm3   claude-code:opus   openai:qwen3:4b@http://host:8080/v1
  anthropic:claude-opus-5-5
Critic spec: MODEL[@URL] (an audio-capable Ollama model), or "none".

  python bench.py agent --proposer ollama:ornith:9b@http://10.209.1.214:11434 --proposer claude-code:opus \\
                        --critic none --critic lfm3-omni --repeats 2 --out results.json
  python bench.py listen lfm3-omni

Prompts and checks live in bench_prompts.json (or --prompts FILE). A check compares a measured feature
or a parameter value: {"label", "feature" | "param", "op", "value"}; "feature" may sum features with "+".
Ops: gt, ge, lt, le (absolute), ratio_gt, ratio_lt (kept / defaults), delta_gt, delta_lt (kept - defaults).
{"label", "any": [...]} and {"label", "all": [...]} combine checks.
"""
from __future__ import annotations

import argparse
import itertools
import json
import math
import random
import statistics
import sys
import tempfile
import time
from pathlib import Path
from typing import Any

sys.path.insert(0, str(Path(__file__).resolve().parent))
import backends as bk  # noqa: E402
import critic as cr  # noqa: E402
import patch_agent as pa  # noqa: E402

HERE = Path(__file__).resolve().parent
DEFAULT_OLLAMA = "http://localhost:11434"
OPS = {
    "gt": lambda v, b, x: v > x, "ge": lambda v, b, x: v >= x, "lt": lambda v, b, x: v < x, "le": lambda v, b, x: v <= x,
    "ratio_gt": lambda v, b, x: v > x * b, "ratio_lt": lambda v, b, x: v < x * b,
    "delta_gt": lambda v, b, x: v - b > x, "delta_lt": lambda v, b, x: v - b < x,
}


# ---- specs -------------------------------------------------------------------------------------------

def split_url(spec: str) -> tuple[str, str | None]:
    """'name@http://host:port' -> ('name', 'http://host:port'); the URL is everything after the last '@'."""
    if "@" in spec:
        head, url = spec.rsplit("@", 1)
        return head, url
    return spec, None


def parse_proposer(spec: str) -> dict[str, Any]:
    head, url = split_url(spec)
    backend, _, model = head.partition(":")  # the model name may itself contain ':' (ornith:9b)
    if backend not in ("ollama", "openai", "claude-code", "anthropic") or not model:
        raise ValueError(f"bad proposer spec {spec!r}: expected BACKEND:MODEL[@URL]")
    return {"backend": backend, "model": model, "url": url, "label": spec}


def parse_critic(spec: str) -> dict[str, Any] | None:
    if spec.lower() == "none":
        return None
    model, url = split_url(spec)
    return {"model": model, "url": url or DEFAULT_OLLAMA, "label": spec}


def make_proposer(p: dict[str, Any], effort: str, num_ctx: int, timeout: float) -> bk.Backend:
    if p["backend"] == "ollama":
        return bk.OllamaBackend(base_url=p["url"] or DEFAULT_OLLAMA, model=p["model"], think=False, num_ctx=num_ctx, timeout=timeout)
    if p["backend"] == "openai":
        return bk.OpenAICompatBackend(base_url=p["url"] or DEFAULT_OLLAMA + "/v1", model=p["model"], timeout=timeout)
    if p["backend"] == "claude-code":
        return bk.ClaudeCodeBackend(model=p["model"], effort=effort, timeout=timeout)
    return bk.AnthropicBackend(model=p["model"], effort=effort)


# ---- checks ------------------------------------------------------------------------------------------

def check_value(spec: dict[str, Any], features: dict[str, float], baseline: dict[str, float], values: dict[str, float],
                defaults: dict[str, float]) -> bool:
    if "any" in spec:
        return any(check_value(s, features, baseline, values, defaults) for s in spec["any"])
    if "all" in spec:
        return all(check_value(s, features, baseline, values, defaults) for s in spec["all"])
    if "feature" in spec:
        names = spec["feature"].split("+")
        v = sum(features[n] for n in names)
        b = sum(baseline[n] for n in names)
    else:
        name = spec["param"]
        if name not in values:
            raise KeyError(f"check uses parameter {name!r}, which the demo does not have")
        v, b = values[name], defaults[name]
    return bool(OPS[spec["op"]](v, b, float(spec["value"])))


def score(checks: list[dict[str, Any]], result: pa.AgentResult, defaults: dict[str, float], rnd: pa.Round) -> dict[str, bool]:
    return {c["label"]: check_value(c, rnd.features, result.baseline, rnd.values, defaults) for c in checks}


# ---- agent benchmark ---------------------------------------------------------------------------------

def run_one(prompt_spec: dict[str, Any], proposer: dict[str, Any], critic_spec: dict[str, Any] | None, renderer: pa.Renderer,
            args: argparse.Namespace) -> dict[str, Any]:
    backend = make_proposer(proposer, args.effort, args.num_ctx, args.timeout)
    critic = cr.OllamaAudioCritic(model=critic_spec["model"], base_url=critic_spec["url"], num_ctx=args.num_ctx,
                                  timeout=args.timeout) if critic_spec else None
    rec: dict[str, Any] = {"proposer": proposer["label"], "critic": critic_spec["label"] if critic_spec else "none",
                           "demo": prompt_spec["demo"], "prompt": prompt_spec["prompt"]}
    t0 = time.time()
    try:
        layout = renderer.layout(prompt_spec["demo"])
        defaults = pa.clamp_values(layout, {})
        with tempfile.TemporaryDirectory() as d:
            r = pa.run_agent(prompt_spec["prompt"], prompt_spec["demo"], renderer, backend, Path(d), rounds=args.rounds,
                             min_rounds=args.min_rounds, critic=critic, log=lambda *a: None)
        rec |= {
            "ok": True,
            "rounds": len(r.rounds),
            "chosen_round": r.best_index + 1,
            "checks": score(prompt_spec["checks"], r, defaults, r.best),
            "last_round_checks": score(prompt_spec["checks"], r, defaults, r.final),
            "revised": any(rd.values != r.rounds[0].values for rd in r.rounds[1:]),
            "centroid_hz": [round(r.baseline["centroid_hz"])] + [round(rd.features["centroid_hz"]) for rd in r.rounds],
            "rms_db": [round(r.baseline["rms_db"], 1)] + [round(rd.features["rms_db"], 1) for rd in r.rounds],
            "verdicts": [rd.verdict for rd in r.rounds],
            "values": [rd.values for rd in r.rounds],
            "rationales": [rd.rationale for rd in r.rounds],
            "models_used": sorted(getattr(backend, "models_used", [])),
            "cost_usd": round(getattr(backend, "cost_usd", 0.0), 4),
        }
    except (bk.AgentError, KeyError) as e:
        rec |= {"ok": False, "error": str(e)[:300]}
    rec["seconds"] = round(time.time() - t0, 1)
    return rec


def summarise(records: list[dict[str, Any]], checks_per_prompt: dict[str, int]) -> list[str]:
    lines = ["| Proposer | Critic | Runs ok | Checks passed | Revised | Median s/run |", "|-|-|-|-|-|-|"]
    for (prop, crit), group in itertools.groupby(records, key=lambda r: (r["proposer"], r["critic"])):
        rs = list(group)
        ok = [r for r in rs if r["ok"]]
        possible = sum(checks_per_prompt[r["prompt"]] for r in rs)
        passed = sum(sum(r["checks"].values()) for r in ok)
        lines.append(f"| {prop} | {crit} | {len(ok)}/{len(rs)} | {passed}/{possible} | {sum(r['revised'] for r in ok)}/{len(ok)} | "
                     f"{statistics.median(r['seconds'] for r in rs):.0f} |")
    return lines


def cmd_agent(args: argparse.Namespace) -> int:
    prompts = json.loads(Path(args.prompts).read_text(encoding="utf-8"))
    proposers = [parse_proposer(s) for s in args.proposer]
    critics = [parse_critic(s) for s in (args.critic or ["none"])]
    renderer = pa.BinaryRenderer(pa.find_render_binary(args.render))
    records = []
    for proposer, critic_spec in itertools.product(proposers, critics):
        for prompt_spec in prompts:
            for _ in range(args.repeats):
                rec = run_one(prompt_spec, proposer, critic_spec, renderer, args)
                records.append(rec)
                status = (f"{sum(rec['checks'].values())}/{len(rec['checks'])} checks, {rec['rounds']} rounds, kept {rec['chosen_round']}, "
                          f"centroid {rec['centroid_hz']}" if rec["ok"] else "ERROR " + rec["error"][:160])
                print(f"{proposer['label']} | critic {critic_spec['label'] if critic_spec else 'none'} | {prompt_spec['prompt'][:30]} | "
                      f"{status} | {rec['seconds']} s", flush=True)
                if args.out:  # write as we go, so a long run that dies keeps its results
                    Path(args.out).write_text(json.dumps(records, indent=1), encoding="utf-8")
    print()
    print("\n".join(summarise(records, {p["prompt"]: len(p["checks"]) for p in prompts})))
    return 0 if all(r["ok"] for r in records) else 1


# ---- listening test ----------------------------------------------------------------------------------

def pcm16(samples: list[float], rate: int = cr.CLIP_RATE) -> bytes:
    import io
    import struct
    import wave
    buf = io.BytesIO()
    with wave.open(buf, "wb") as w:
        w.setnchannels(1)
        w.setsampwidth(2)
        w.setframerate(rate)
        w.writeframes(b"".join(struct.pack("<h", int(max(-1.0, min(1.0, s)) * 32767)) for s in samples))
    return buf.getvalue()


def ask_audio(model: str, url: str, clip: bytes, question: str, options: list[str], timeout: float) -> str:
    import base64
    reply = bk.post_json(f"{url}/api/chat", {
        "model": model, "stream": False, "think": False, "options": {"temperature": 0},
        "format": {"type": "object", "properties": {"answer": {"type": "string", "enum": options}}, "required": ["answer"]},
        "messages": [{"role": "user", "content": f"{question} Answer with one of: {', '.join(options)}.",
                      "images": [base64.b64encode(clip).decode()]}],
    }, timeout)
    if "error" in reply:
        raise bk.AgentError(str(reply["error"]))
    return bk.parse_json_text(reply["message"]["content"]).get("answer", "")


def cmd_listen(args: argparse.Namespace) -> int:
    model, url = split_url(args.model)
    url = url or DEFAULT_OLLAMA
    renderer = pa.BinaryRenderer(pa.find_render_binary(args.render))
    rate, n = cr.CLIP_RATE, 2 * cr.CLIP_RATE
    rnd = random.Random(1)
    tone = pcm16([0.6 * math.sin(2 * math.pi * 440 * i / rate) for i in range(n)])
    noise = pcm16([rnd.uniform(-0.6, 0.6) for _ in range(n)])
    silence = pcm16([0.0] * n)
    synth = {
        "dark": "cutoff=150,resonance=0.5,env amount=0,tone=0,bite=0",
        "bright": "cutoff=8000,resonance=6,env amount=6000,tone=1,bite=1",
        "darkish": "cutoff=300,resonance=0.5,env amount=300,tone=0,bite=0",
        "brightish": "cutoff=5000,resonance=2,env amount=4000,tone=0.6,bite=0.2",
    }
    q_tone, q_sound, q_bright = "Is this sound a pure tone or noise?", "Is there any sound in this clip?", "Is this synthesizer sound bright or dark (muffled)?"
    cases = [("tone", tone, q_tone, ["pure tone", "noise"], "pure tone"), ("noise", noise, q_tone, ["pure tone", "noise"], "noise"),
             ("silence", silence, q_sound, ["sound", "silence"], "silence"), ("tone present", tone, q_sound, ["sound", "silence"], "sound")]
    with tempfile.TemporaryDirectory() as d:
        for name, settings in synth.items():
            wav = Path(d) / f"{name}.wav"
            vals = dict(kv.split("=") for kv in settings.split(","))
            renderer.render("poly", {k: float(v) for k, v in vals.items()}, wav)
            truth = "dark" if name.startswith("dark") else "bright"
            cases.append((f"{name} synth", cr.clip_for_listening(wav, seconds=2.0), q_bright, ["bright", "dark"], truth))
    right = 0
    for name, clip, question, options, truth in cases:
        try:
            got = ask_audio(model, url, clip, question, options, args.timeout)
        except bk.AgentError as e:
            got = f"ERROR {str(e)[:120]}"
        right += got == truth
        print(f"{name:16} truth {truth:10} answer {got:10} {'ok' if got == truth else 'WRONG'}", flush=True)
    print(f"{model}: {right}/{len(cases)} right (a model that cannot hear scores about {len(cases) // 2})")
    return 0


def main(argv: list[str] | None = None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("agent", help="benchmark proposer x critic combinations")
    a.add_argument("--proposer", action="append", required=True, help="BACKEND:MODEL[@URL]; repeat for several")
    a.add_argument("--critic", action="append", help="MODEL[@URL] or none; repeat for several (default: none)")
    a.add_argument("--prompts", default=str(HERE / "bench_prompts.json"))
    a.add_argument("--repeats", type=int, default=2)
    a.add_argument("--rounds", type=int, default=4)
    a.add_argument("--min-rounds", type=int, default=3)
    a.add_argument("--effort", default="medium", help="Claude backends only")
    a.add_argument("--num-ctx", type=int, default=8192)
    a.add_argument("--timeout", type=float, default=600.0)
    a.add_argument("--out", help="write every run's record to this JSON file as it goes")
    a.add_argument("--render", help="path to tonic2_render")
    a.set_defaults(func=cmd_agent)
    li = sub.add_parser("listen", help="blind A/B listening test for an audio-capable Ollama model")
    li.add_argument("model", help="MODEL[@URL], e.g. lfm3-omni or lfm3-omni@http://host:11434")
    li.add_argument("--timeout", type=float, default=300.0)
    li.add_argument("--render", help="path to tonic2_render")
    li.set_defaults(func=cmd_listen)
    args = ap.parse_args(argv)
    return args.func(args)


if __name__ == "__main__":
    sys.exit(main())
