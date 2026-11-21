"""Tests for the patch agent with a scripted fake Claude client (no network, no SDK needed).

Set TONIC2_RENDER to a tonic2_render binary to also run the loop against real renders (ctest does).
"""
from __future__ import annotations

import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace

sys.path.insert(0, str(Path(__file__).resolve().parent))
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
            result = pa.run_agent("bright lead", "fake", renderer, client, Path(d), rounds=rounds, log=quiet)
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
            result = pa.run_agent("bright pluck", "poly", renderer, client, Path(d), log=quiet)
            self.assertTrue((Path(d) / "round2.wav").is_file())
        self.assertGreater(result.rounds[1].features["centroid_hz"], 1.5 * result.rounds[0].features["centroid_hz"])


if __name__ == "__main__":
    unittest.main()
