"""Tests for the patch agent with a scripted fake Claude client (no network, no SDK needed).

Set TONIC2_RENDER to a tonic2_render binary to also run the loop against real renders (ctest does).
"""
from __future__ import annotations

import http.server
import json
import os
import subprocess
import threading
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
import backends as bk  # noqa: E402
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


if __name__ == "__main__":
    unittest.main()
