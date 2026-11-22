"""Tests for the patch agent with a scripted fake Claude client (no network, no SDK needed).

Set TONIC2_RENDER to a tonic2_render binary to also run the loop against real renders (ctest does).
"""
from __future__ import annotations

import base64
import http.server
import io
import json
import math
import os
import struct
import subprocess
import threading
import sys
import wave
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
import backends as bk  # noqa: E402
import bench  # noqa: E402
import critic as cr  # noqa: E402
import patch_agent as pa  # noqa: E402

LAYOUT = {
    "demo": "fake",
    "description": "test patch",
    "seconds": 1.0,
    "params": [
        {"name": "cutoff", "min": 100, "max": 8000, "default": 900, "unit": "Hz", "log": True, "macro_targets": []},
        {"name": "level", "min": -36, "max": 0, "default": -3, "unit": "dB", "log": False, "macro_targets": []},
    ],
}


def reply(payload, stop_reason="end_turn"):
    blocks = [SimpleNamespace(type="thinking", thinking="")]
    if payload is not None:
        blocks.append(SimpleNamespace(type="text", text=payload if isinstance(payload, str) else json.dumps(payload)))
    return SimpleNamespace(content=blocks, stop_reason=stop_reason, stop_details=None)


class FakeClient:
    """Returns scripted responses and records every request."""

    def __init__(self, replies):
        self.replies = list(replies)
        self.requests = []
        self.beta = SimpleNamespace(messages=SimpleNamespace(create=self._create))

    def _create(self, **kwargs):
        self.requests.append(json.loads(json.dumps(kwargs, default=lambda o: vars(o))))
        return self.replies.pop(0)


class FakeRenderer:
    """Brightness follows cutoff, level follows level: enough to check the loop wiring."""

    def __init__(self):
        self.renders = []

    def layout(self, demo):
        return LAYOUT

    def render(self, demo, values, out):
        self.renders.append(dict(values))
        Path(out).write_bytes(b"RIFF")
        return {"centroid_hz": values["cutoff"] * 0.6, "rms_db": values["level"] - 15.0, "attack_ms": 10.0, "sustain_ratio": 0.5}


def quiet(*_):
    pass


class SchemaAndClampTests(unittest.TestCase):
    def test_schema_requires_every_parameter(self):
        s = pa.response_schema(LAYOUT)
        self.assertEqual(s["properties"]["params"]["required"], ["cutoff", "level"])
        self.assertFalse(s["properties"]["params"]["additionalProperties"])
        self.assertEqual(set(s["required"]), {"params", "rationale", "done"})

    def test_clamp_fills_defaults_and_limits_range(self):
        v = pa.clamp_values(LAYOUT, {"cutoff": 20000, "level": "loud"})
        self.assertEqual(v, {"cutoff": 8000.0, "level": -3.0})
        self.assertEqual(pa.clamp_values(LAYOUT, {}), {"cutoff": 900.0, "level": -3.0})


class LoopTests(unittest.TestCase):
    def run_agent(self, client, renderer=None, rounds=4):
        renderer = renderer or FakeRenderer()
        with tempfile.TemporaryDirectory() as d:
            result = pa.run_agent("bright lead", "fake", renderer, pa.AnthropicBackend(client=client), Path(d), rounds=rounds, log=quiet)
        return result, renderer

    def test_revises_until_done(self):
        client = FakeClient([
            reply({"params": {"cutoff": 2000, "level": -6}, "rationale": "open up", "done": False}),
            reply({"params": {"cutoff": 9000, "level": -6}, "rationale": "brighter", "done": True}),
        ])
        result, renderer = self.run_agent(client)
        self.assertEqual(len(result.rounds), 2)
        self.assertTrue(result.final.done)
        self.assertEqual(result.final.values["cutoff"], 8000.0)  # clamped
        self.assertEqual(renderer.renders[0], {"cutoff": 900.0, "level": -3.0})  # baseline at defaults
        # Second request carries the first answer (thinking block included) and its measurements.
        msgs = client.requests[1]["messages"]
        self.assertEqual([m["role"] for m in msgs], ["user", "assistant", "user"])
        self.assertEqual(msgs[1]["content"][0]["type"], "thinking")
        self.assertIn('"centroid_hz": 1200.0', msgs[2]["content"])
        self.assertEqual(result.preset()["params"]["cutoff"], 8000.0)

    def test_request_shape(self):
        client = FakeClient([reply({"params": {"cutoff": 1000, "level": -6}, "rationale": "ok", "done": True})])
        self.run_agent(client)
        req = client.requests[0]
        self.assertEqual(req["model"], "claude-opus-5-5")
        self.assertEqual(req["output_config"]["format"]["type"], "json_schema")
        self.assertEqual(req["output_config"]["effort"], "medium")
        self.assertEqual(req["fallbacks"], "default")
        self.assertIn("server-side-fallback-2026-07-01", req["betas"])
        self.assertNotIn("thinking", req)
        self.assertIn("Sound to make: bright lead", req["messages"][0]["content"])

    def test_clamped_values_are_reported_back(self):
        client = FakeClient([
            reply({"params": {"cutoff": 20000, "level": -6}, "rationale": "x", "done": False}),
            reply({"params": {"cutoff": 7000, "level": -6}, "rationale": "y", "done": True}),
        ])
        self.run_agent(client)
        self.assertIn("clamped: cutoff", client.requests[1]["messages"][2]["content"])

    def test_stops_at_round_limit(self):
        client = FakeClient([reply({"params": {"cutoff": 1000 + i, "level": -6}, "rationale": "", "done": False}) for i in range(3)])
        result, _ = self.run_agent(client, rounds=3)
        self.assertEqual(len(result.rounds), 3)
        self.assertFalse(result.final.done)

    def test_refusal_and_bad_output_raise(self):
        for bad in (reply(None, stop_reason="refusal"), reply(None, stop_reason="max_tokens"), reply("not json"), reply({"rationale": "no params"})):
            with self.assertRaises(pa.AgentError):
                self.run_agent(FakeClient([bad]))


class ParseJsonTests(unittest.TestCase):
    def test_tolerates_think_blocks_fences_and_chatter(self):
        self.assertEqual(bk.parse_json_text('{"a": 1}'), {"a": 1})
        self.assertEqual(bk.parse_json_text('<think>hmm {"x": 0}</think>\n```json\n{"a": 2}\n```'), {"a": 2})
        self.assertEqual(bk.parse_json_text('Here you go: {"a": {"b": 3}} hope that helps'), {"a": {"b": 3}})

    def test_rejects_non_objects(self):
        for bad in ("no json here", "[1, 2]", "{broken"):
            with self.assertRaises(bk.AgentError):
                bk.parse_json_text(bad)


class FakeCli:
    """Stands in for subprocess.run on the claude CLI; replies with scripted result objects."""

    def __init__(self, outputs):
        self.outputs, self.calls = list(outputs), []

    def __call__(self, cmd, **kwargs):
        self.calls.append((cmd, kwargs))
        return subprocess.CompletedProcess(cmd, 0, stdout=json.dumps(self.outputs.pop(0)), stderr="")


class ClaudeCodeBackendTests(unittest.TestCase):
    def test_conversation_and_flags(self):
        cli = FakeCli([
            {"type": "result", "is_error": False, "result": "", "session_id": "s1",
             "structured_output": {"params": {"cutoff": 2000, "level": -6}, "rationale": "a", "done": False}},
            {"type": "result", "is_error": False, "session_id": "s1",
             "result": '```json\n{"params": {"cutoff": 3000, "level": -6}, "rationale": "b", "done": true}\n```'},
        ])
        backend = bk.ClaudeCodeBackend(binary="claude", effort="xhigh", runner=cli)
        with tempfile.TemporaryDirectory() as d:
            result = pa.run_agent("bright", "fake", FakeRenderer(), backend, Path(d), log=quiet)
        self.assertEqual([r.values["cutoff"] for r in result.rounds], [2000.0, 3000.0])
        first, second = cli.calls[0][0], cli.calls[1][0]
        self.assertEqual(first[:4], ["claude", "-p", "--output-format", "json"])
        self.assertEqual(json.loads(first[first.index("--json-schema") + 1])["required"], ["params", "rationale", "done"])
        self.assertEqual(first[first.index("--tools") + 1], "")
        self.assertEqual(first[first.index("--effort") + 1], "high")  # the CLI has no xhigh
        self.assertNotIn("--resume", first)
        self.assertEqual(second[second.index("--resume") + 1], "s1")
        self.assertIn("Sound to make: bright", cli.calls[0][1]["input"])
        self.assertIn("Measurements of your values", cli.calls[1][1]["input"])

    def test_errors(self):
        revoked = {"type": "result", "is_error": True, "api_error_status": 401, "result": "OAuth access token has been revoked."}
        with self.assertRaisesRegex(bk.AgentError, "log in again"):
            b = bk.ClaudeCodeBackend(runner=FakeCli([revoked]))
            b.start("s", {})
            b.ask("hi")

        def missing(cmd, **kwargs):
            raise FileNotFoundError(cmd[0])
        with self.assertRaisesRegex(bk.AgentError, "not found"):
            b = bk.ClaudeCodeBackend(binary="nope", runner=missing)
            b.start("s", {})
            b.ask("hi")


class FakeOpenAIServer:
    """A local /v1/chat/completions endpoint that replies with scripted message contents."""

    def __init__(self, contents):
        self.contents, self.requests = list(contents), []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                outer.requests.append((self.path, json.loads(self.rfile.read(int(self.headers["Content-Length"])))))
                body = json.dumps({"choices": [{"message": {"role": "assistant", "content": outer.contents.pop(0)},
                                                "finish_reason": "stop"}]}).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}/v1"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


class OpenAICompatBackendTests(unittest.TestCase):
    def test_conversation_over_http(self):
        server = FakeOpenAIServer([
            '<think>dark first</think>{"params": {"cutoff": 500, "level": -6}, "rationale": "a", "done": false}',
            '{"params": {"cutoff": 4000, "level": -6}, "rationale": "b", "done": true}',
        ])
        try:
            backend = bk.OpenAICompatBackend(base_url=server.url, model="tiny")
            with tempfile.TemporaryDirectory() as d:
                result = pa.run_agent("bright", "fake", FakeRenderer(), backend, Path(d), log=quiet)
        finally:
            server.close()
        self.assertEqual([r.values["cutoff"] for r in result.rounds], [500.0, 4000.0])
        path, req = server.requests[1]
        self.assertEqual(path, "/v1/chat/completions")
        self.assertEqual(req["model"], "tiny")
        self.assertEqual(req["response_format"]["type"], "json_schema")
        self.assertEqual(req["response_format"]["json_schema"]["schema"]["required"], ["params", "rationale", "done"])
        self.assertEqual([m["role"] for m in req["messages"]], ["system", "user", "assistant", "user"])
        self.assertEqual(req["reasoning_effort"], "none")
        self.assertEqual(req["max_tokens"], 4096)

    def test_unreachable_server(self):
        backend = bk.OpenAICompatBackend(base_url="http://127.0.0.1:9/v1", timeout=2)
        backend.start("s", {})
        with self.assertRaisesRegex(bk.AgentError, "could not reach"):
            backend.ask("hi")


class ScriptedBackend:
    """Backend that returns scripted replies and records the user turns it received."""

    name = "scripted"

    def __init__(self, replies):
        self.replies, self.turns = list(replies), []

    def start(self, system, schema):
        self.system = system

    def ask(self, text):
        self.turns.append(text)
        return self.replies.pop(0)


class ScriptedCritic:
    name = "scripted-critic"

    def __init__(self, matches):
        self.matches, self.heard = list(matches), []

    def judge(self, description, wav):
        self.heard.append(Path(wav).name)
        return {"brightness": "dark", "character": "soft", "envelope": "sustained", "notes": "dull", "match": self.matches.pop(0)}


def answer(cutoff, done=True):
    return {"params": {"cutoff": cutoff, "level": -6}, "rationale": "r", "done": done}


class MinRoundsAndCriticTests(unittest.TestCase):
    def test_done_is_ignored_until_min_rounds(self):
        backend = ScriptedBackend([answer(1000), answer(2000), answer(3000), answer(4000)])
        with tempfile.TemporaryDirectory() as d:
            result = pa.run_agent("bright", "fake", FakeRenderer(), backend, Path(d), rounds=4, min_rounds=3, log=quiet)
        self.assertEqual(len(result.rounds), 3)  # done on every reply, but three rounds are required
        self.assertIn("at least 3 will run", backend.turns[0])
        self.assertIn("at least 3 rounds are required", backend.turns[1])
        self.assertIn("This is round 2; at least 3 rounds are required", backend.turns[2])
        self.assertEqual(len(backend.turns), 3)  # round 3's done flag is accepted

    def test_after_min_rounds_model_may_stop(self):
        backend = ScriptedBackend([answer(1000, done=False), answer(2000, done=False), answer(3000)])
        with tempfile.TemporaryDirectory() as d:
            result = pa.run_agent("bright", "fake", FakeRenderer(), backend, Path(d), rounds=4, min_rounds=1, log=quiet)
        self.assertEqual(len(result.rounds), 3)
        self.assertIn("set done=true", backend.turns[1])

    def test_min_rounds_capped_by_rounds(self):
        backend = ScriptedBackend([answer(1000, done=False), answer(2000, done=False)])
        with tempfile.TemporaryDirectory() as d:
            result = pa.run_agent("bright", "fake", FakeRenderer(), backend, Path(d), rounds=2, min_rounds=5, log=quiet)
        self.assertEqual(len(result.rounds), 2)

    def test_critic_feedback_and_best_round(self):
        backend = ScriptedBackend([answer(1000, done=False), answer(2000, done=False), answer(3000, done=True)])
        critic = ScriptedCritic([1, 4, 2, 4])  # defaults, then rounds 1-3
        with tempfile.TemporaryDirectory() as d:
            result = pa.run_agent("bright", "fake", FakeRenderer(), backend, Path(d), rounds=3, min_rounds=1, critic=critic, log=quiet)
            self.assertTrue(Path(result.best.wav).is_file())
        self.assertEqual(critic.heard, ["baseline.wav", "round1.wav", "round2.wav", "round3.wav"])
        self.assertIn('A listener heard the defaults: sounds dark, soft, sustained ("dull"); rated 1/5', backend.turns[0])
        self.assertIn("A listener heard this render: sounds dark, soft, sustained", backend.turns[1])
        self.assertIn("rated 4/5", backend.turns[1])
        self.assertEqual(result.best_index, 2)  # rounds 1 and 3 both score 4: the later one wins
        preset = result.preset()
        self.assertEqual(preset["chosen_round"], 3)
        self.assertEqual(preset["params"]["cutoff"], 3000.0)
        self.assertEqual([h["verdict"]["match"] for h in preset["history"]], [4, 2, 4])

    def test_without_critic_last_round_is_kept(self):
        backend = ScriptedBackend([answer(1000, done=False), answer(2000)])
        with tempfile.TemporaryDirectory() as d:
            result = pa.run_agent("bright", "fake", FakeRenderer(), backend, Path(d), rounds=2, log=quiet)
        self.assertEqual(result.best_index, 1)
        self.assertIsNone(result.preset()["verdict"])


class FakeJsonServer:
    """Local HTTP server: each POST is recorded and answered with the next scripted JSON body."""

    def __init__(self, bodies):
        self.bodies, self.requests = list(bodies), []
        outer = self

        class Handler(http.server.BaseHTTPRequestHandler):
            def do_POST(self):
                outer.requests.append((self.path, json.loads(self.rfile.read(int(self.headers["Content-Length"])))))
                body = json.dumps(outer.bodies.pop(0)).encode()
                self.send_response(200)
                self.send_header("Content-Type", "application/json")
                self.send_header("Content-Length", str(len(body)))
                self.end_headers()
                self.wfile.write(body)

            def log_message(self, *args):
                pass

        self.server = http.server.HTTPServer(("127.0.0.1", 0), Handler)
        self.url = f"http://127.0.0.1:{self.server.server_address[1]}"
        threading.Thread(target=self.server.serve_forever, daemon=True).start()

    def close(self):
        self.server.shutdown()
        self.server.server_close()


def ollama_reply(content, done_reason="stop"):
    return {"message": {"role": "assistant", "content": content}, "done": True, "done_reason": done_reason}


class OllamaBackendTests(unittest.TestCase):
    def test_native_request_and_history(self):
        server = FakeJsonServer([ollama_reply(json.dumps(answer(800, done=False))), ollama_reply(json.dumps(answer(900)))])
        try:
            backend = bk.OllamaBackend(base_url=server.url, model="lfm3", think=False, num_ctx=12288)
            with tempfile.TemporaryDirectory() as d:
                result = pa.run_agent("dark", "fake", FakeRenderer(), backend, Path(d), rounds=2, log=quiet)
        finally:
            server.close()
        self.assertEqual([r.values["cutoff"] for r in result.rounds], [800.0, 900.0])
        path, req = server.requests[1]
        self.assertEqual(path, "/api/chat")
        self.assertIs(req["think"], False)
        self.assertIs(req["stream"], False)
        self.assertEqual(req["options"]["num_ctx"], 12288)
        self.assertEqual(req["format"]["required"], ["params", "rationale", "done"])
        self.assertEqual([m["role"] for m in req["messages"]], ["system", "user", "assistant", "user"])

    def test_length_cutoff_and_server_error(self):
        for body, pattern in ((ollama_reply('{"params": {', done_reason="length"), "tokens"), ({"error": "model not found"}, "model not found")):
            server = FakeJsonServer([body])
            try:
                backend = bk.OllamaBackend(base_url=server.url)
                backend.start("s", {})
                with self.assertRaisesRegex(bk.AgentError, pattern):
                    backend.ask("hi")
            finally:
                server.close()


def write_float_wav(path, samples, rate=48000, channels=2):
    data = b"".join(struct.pack("<f", s) for s in samples)
    fmt = struct.pack("<HHIIHH", 3, channels, rate, rate * channels * 4, channels * 4, 32)
    Path(path).write_bytes(b"RIFF" + struct.pack("<I", 4 + 8 + len(fmt) + 8 + len(data)) + b"WAVE"
                           + b"fmt " + struct.pack("<I", len(fmt)) + fmt + b"data" + struct.pack("<I", len(data)) + data)


class CriticTests(unittest.TestCase):
    def test_clip_is_short_mono_16k_and_normalised(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "x.wav"
            frames = 48000 * 5  # 5 s stereo at 48 kHz, quiet 440 Hz
            write_float_wav(p, [0.05 * math.sin(2 * math.pi * 440 * (i // 2) / 48000) for i in range(frames * 2)])
            clip = cr.clip_for_listening(p, seconds=3.0)
        with wave.open(io.BytesIO(clip)) as w:
            self.assertEqual((w.getnchannels(), w.getsampwidth(), w.getframerate()), (1, 2, 16000))
            self.assertEqual(w.getnframes(), 48000)
            pcm = struct.unpack(f"<{w.getnframes()}h", w.readframes(w.getnframes()))
        self.assertAlmostEqual(max(abs(s) for s in pcm) / 32767, 0.8, delta=0.02)

    def test_rejects_non_float_wav(self):
        with tempfile.TemporaryDirectory() as d:
            p = Path(d) / "pcm.wav"
            with wave.open(str(p), "wb") as w:
                w.setnchannels(1)
                w.setsampwidth(2)
                w.setframerate(16000)
                w.writeframes(b"\x00\x00" * 100)
            with self.assertRaisesRegex(bk.AgentError, "float"):
                cr.read_float_wav(p)

    def test_ollama_critic_request_and_verdict(self):
        server = FakeJsonServer([ollama_reply('{"brightness": "very bright", "character": "harsh", "envelope": "plucked", "notes": "buzzy", "match": 9}')])
        try:
            critic = cr.OllamaAudioCritic(model="lfm3-omni", base_url=server.url, num_ctx=4096)
            with tempfile.TemporaryDirectory() as d:
                p = Path(d) / "r.wav"
                write_float_wav(p, [0.3] * 9600)
                verdict = critic.judge("dark pad", p)
        finally:
            server.close()
        self.assertEqual(verdict, {"brightness": "medium", "character": "harsh", "envelope": "plucked", "notes": "buzzy", "match": 5})
        path, req = server.requests[0]
        self.assertEqual(path, "/api/chat")
        msg = req["messages"][0]
        self.assertIn("dark pad", msg["content"])
        self.assertEqual(base64.b64decode(msg["images"][0])[:4], b"RIFF")  # audio travels in `images`
        self.assertIs(req["think"], False)
        self.assertEqual(req["options"]["num_ctx"], 4096)
        self.assertEqual(req["format"]["properties"]["match"]["enum"], [1, 2, 3, 4, 5])


class BenchTests(unittest.TestCase):
    def test_proposer_and_critic_specs(self):
        p = bench.parse_proposer("ollama:ornith:9b@http://10.209.1.214:11434")
        self.assertEqual((p["backend"], p["model"], p["url"]), ("ollama", "ornith:9b", "http://10.209.1.214:11434"))
        self.assertEqual(bench.parse_proposer("claude-code:opus")["url"], None)
        self.assertEqual(bench.parse_proposer("openai:qwen3:4b@http://h:8080/v1")["model"], "qwen3:4b")
        for bad in ("ollama", "nope:model", "ollama:"):
            with self.assertRaises(ValueError):
                bench.parse_proposer(bad)
        self.assertIsNone(bench.parse_critic("none"))
        self.assertEqual(bench.parse_critic("lfm3-omni")["url"], "http://localhost:11434")
        self.assertEqual(bench.parse_critic("lfm3-omni@http://box:11434")["url"], "http://box:11434")

    def test_check_language(self):
        base = {"centroid_hz": 500.0, "rms_db": -30.0, "band_low": 0.3, "band_low_mid": 0.4}
        kept = {"centroid_hz": 800.0, "rms_db": -25.0, "band_low": 0.2, "band_low_mid": 0.3}
        values, defaults = {"resonance": 3.0, "bite": 0.0}, {"resonance": 1.2, "bite": 0.0}
        ok = lambda spec: bench.check_value(spec, kept, base, values, defaults)  # noqa: E731
        self.assertTrue(ok({"feature": "centroid_hz", "op": "ratio_gt", "value": 1.5}))
        self.assertFalse(ok({"feature": "centroid_hz", "op": "ratio_gt", "value": 1.7}))
        self.assertTrue(ok({"feature": "rms_db", "op": "delta_gt", "value": 4}))
        self.assertTrue(ok({"feature": "band_low+band_low_mid", "op": "lt", "value": 0.6}))
        self.assertTrue(ok({"param": "resonance", "op": "ratio_gt", "value": 2}))
        self.assertTrue(ok({"any": [{"param": "bite", "op": "gt", "value": 0.5}, {"param": "resonance", "op": "ge", "value": 3}]}))
        self.assertFalse(ok({"all": [{"param": "bite", "op": "gt", "value": 0.5}, {"param": "resonance", "op": "ge", "value": 3}]}))
        with self.assertRaises(KeyError):
            ok({"param": "cutoff", "op": "gt", "value": 1})

    def test_bundled_prompts_are_well_formed(self):
        prompts = json.loads((Path(bench.__file__).parent / "bench_prompts.json").read_text(encoding="utf-8"))
        self.assertGreaterEqual(len(prompts), 3)

        def walk(spec):
            if "any" in spec or "all" in spec:
                for s in spec.get("any", spec.get("all")):
                    yield from walk(s)
            else:
                self.assertIn(spec["op"], bench.OPS)
                self.assertTrue(("feature" in spec) != ("param" in spec))
                yield spec
        for p in prompts:
            self.assertTrue(p["checks"] and all("label" in c for c in p["checks"]))
            for c in p["checks"]:
                list(walk(c))

    def test_summary_table(self):
        recs = [{"proposer": "ollama:x", "critic": "none", "prompt": "a", "ok": True, "checks": {"c1": True, "c2": False},
                 "revised": True, "seconds": 10.0},
                {"proposer": "ollama:x", "critic": "none", "prompt": "a", "ok": False, "error": "boom", "seconds": 30.0}]
        table = bench.summarise(recs, {"a": 2})
        self.assertEqual(table[2], "| ollama:x | none | 1/2 | 1/4 | 1/1 | 20 |")


@unittest.skipUnless(os.environ.get("TONIC2_RENDER"), "TONIC2_RENDER not set")
class RealRendererTests(unittest.TestCase):
    def test_loop_with_real_renders(self):
        renderer = pa.BinaryRenderer(os.environ["TONIC2_RENDER"])
        layout = renderer.layout("poly")
        names = [p["name"] for p in layout["params"]]
        self.assertIn("cutoff", names)
        dark = {n: p["default"] for n, p in zip(names, layout["params"])} | {"cutoff": 200.0}
        bright = dark | {"cutoff": 6000.0, "resonance": 4.0}
        client = FakeClient([
            reply({"params": dark, "rationale": "dark", "done": False}),
            reply({"params": bright, "rationale": "bright", "done": True}),
        ])
        with tempfile.TemporaryDirectory() as d:
            result = pa.run_agent("bright pluck", "poly", renderer, pa.AnthropicBackend(client=client), Path(d), log=quiet)
            self.assertTrue((Path(d) / "round2.wav").is_file())
        self.assertGreater(result.rounds[1].features["centroid_hz"], 1.5 * result.rounds[0].features["centroid_hz"])

    def test_bundled_prompt_params_exist_in_their_demos(self):
        renderer = pa.BinaryRenderer(os.environ["TONIC2_RENDER"])
        prompts = json.loads((Path(bench.__file__).parent / "bench_prompts.json").read_text(encoding="utf-8"))
        for p in prompts:
            names = {q["name"] for q in renderer.layout(p["demo"])["params"]}

            def params(spec):
                for s in spec.get("any", spec.get("all", [])):
                    yield from params(s)
                if "param" in spec:
                    yield spec["param"]
            for c in p["checks"]:
                for name in params(c):
                    self.assertIn(name, names, f"{p['prompt']!r}: demo {p['demo']} has no parameter {name!r}")


if __name__ == "__main__":
    unittest.main()
