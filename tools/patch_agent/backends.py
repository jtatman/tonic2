"""LLM backends for the patch agent. Each one holds its own conversation and returns parsed JSON.

  anthropic    Claude through the Anthropic API (API credits; ANTHROPIC_API_KEY or `ant auth login`)
  claude-code  Claude through the Claude Code CLI in headless mode (`claude -p`), billed to the
               subscription the CLI is logged in with
  openai       any server with an OpenAI-style /chat/completions endpoint: Ollama, llama.cpp
               (llama-server), LM Studio, vLLM. Local and free.

All three ask for output that matches a JSON schema; the agent still validates and clamps the result,
because a small local model or a prompt-only fallback can return something else.
"""
from __future__ import annotations

import json
import re
import subprocess
import tempfile
import urllib.error
import urllib.request
from typing import Any, Callable, Protocol


class AgentError(RuntimeError):
    pass


class Backend(Protocol):
    name: str

    def start(self, system: str, schema: dict[str, Any]) -> None:
        """Begin a new conversation with this system prompt and reply schema."""

    def ask(self, user_text: str) -> dict[str, Any]:
        """Send one user turn; return the reply parsed as a JSON object (raises AgentError)."""


_THINK = re.compile(r"<think>.*?</think>", re.DOTALL)
_FENCE = re.compile(r"^```(?:json)?\s*|\s*```$")


def parse_json_text(text: str) -> dict[str, Any]:
    """Parse a JSON object from model text, tolerating <think> blocks, code fences and chatter around it."""
    t = _FENCE.sub("", _THINK.sub("", text).strip()).strip()
    try:
        data = json.loads(t)
    except json.JSONDecodeError:
        start, end = t.find("{"), t.rfind("}")
        if start < 0 or end <= start:
            raise AgentError(f"reply contains no JSON object: {text[:200]!r}") from None
        try:
            data = json.loads(t[start:end + 1])
        except json.JSONDecodeError as e:
            raise AgentError(f"reply was not valid JSON: {e}") from e
    if not isinstance(data, dict):
        raise AgentError("reply JSON is not an object")
    return data


class AnthropicBackend:
    """Messages API with structured outputs and server-side refusal fallback."""

    name = "anthropic"

    def __init__(self, client: Any = None, model: str = "claude-opus-5-5", effort: str = "medium"):
        self.client, self.model, self.effort = client, model, effort
        self.system, self.schema, self.messages = "", {}, []

    def start(self, system: str, schema: dict[str, Any]) -> None:
        self.system, self.schema, self.messages = system, schema, []
        if self.client is None:
            import anthropic  # only this backend needs the SDK
            self.client = anthropic.Anthropic()

    def ask(self, user_text: str) -> dict[str, Any]:
        self.messages.append({"role": "user", "content": user_text})
        try:
            response = self.client.beta.messages.create(
                model=self.model,
                max_tokens=16000,
                system=self.system,
                messages=self.messages,
                output_config={"effort": self.effort, "format": {"type": "json_schema", "schema": self.schema}},
                # A policy decline re-runs the request on a fallback model inside the same call.
                betas=["server-side-fallback-2026-07-01"],
                fallbacks="default",
            )
        except Exception as e:  # map SDK errors to one message; anything unexpected re-raises
            raise _anthropic_error(e) from e
        if response.stop_reason == "refusal":
            details = getattr(response, "stop_details", None)
            raise AgentError(f"request declined: {getattr(details, 'explanation', None) or 'no explanation'}")
        if response.stop_reason == "max_tokens":
            raise AgentError("response hit max_tokens before the JSON was complete")
        text = next((b.text for b in response.content if b.type == "text"), None)
        if text is None:
            raise AgentError(f"no text block in response (stop_reason {response.stop_reason})")
        data = parse_json_text(text)
        self.messages.append({"role": "assistant", "content": response.content})  # unchanged, thinking blocks included
        return data


def _anthropic_error(e: Exception) -> Exception:
    try:
        import anthropic
    except ImportError:
        return e
    if isinstance(e, TypeError) and "authentication" in str(e):  # raised when no credentials exist at all
        return AgentError("no Anthropic credentials found (set ANTHROPIC_API_KEY or run `ant auth login`)")
    if isinstance(e, anthropic.AuthenticationError):
        return AgentError("Anthropic credentials were rejected (check ANTHROPIC_API_KEY or `ant auth login`)")
    if isinstance(e, anthropic.RateLimitError):
        return AgentError("rate limited by the Anthropic API; try again shortly")
    if isinstance(e, anthropic.APIStatusError):
        return AgentError(f"Anthropic API returned {e.status_code}: {e.message}")
    if isinstance(e, anthropic.APIConnectionError):
        return AgentError("could not reach the Anthropic API")
    return e


Runner = Callable[..., subprocess.CompletedProcess]


class ClaudeCodeBackend:
    """Claude Code CLI in headless mode: `claude -p --output-format json --json-schema ...`.

    No tools, no MCP servers, no settings or project files: the CLI runs in an empty temporary
    directory and only answers. Later turns continue the same CLI session with --resume.
    """

    name = "claude-code"
    EFFORTS = {"low": "low", "medium": "medium", "high": "high", "xhigh": "high", "max": "max"}

    def __init__(self, binary: str = "claude", model: str | None = "opus", effort: str = "medium",
                 runner: Runner = subprocess.run, timeout: float = 600.0):
        self.binary, self.model, self.effort, self.runner, self.timeout = binary, model, effort, runner, timeout
        self.system, self.schema, self.session = "", {}, None
        self._cwd = tempfile.mkdtemp(prefix="tonic2_agent_")

    def start(self, system: str, schema: dict[str, Any]) -> None:
        self.system, self.schema, self.session = system, schema, None

    def command(self) -> list[str]:
        cmd = [self.binary, "-p", "--output-format", "json", "--json-schema", json.dumps(self.schema),
               "--system-prompt", self.system, "--tools", "", "--strict-mcp-config", "--setting-sources", "",
               "--effort", self.EFFORTS.get(self.effort, "medium")]
        if self.model:
            cmd += ["--model", self.model]
        if self.session:
            cmd += ["--resume", self.session]
        return cmd

    def ask(self, user_text: str) -> dict[str, Any]:
        try:
            proc = self.runner(self.command(), input=user_text, capture_output=True, text=True,
                               encoding="utf-8", timeout=self.timeout, cwd=self._cwd, check=False)
        except FileNotFoundError as e:
            raise AgentError(f"Claude Code CLI not found ({self.binary}); install it or pass --claude-bin") from e
        except subprocess.TimeoutExpired as e:
            raise AgentError(f"Claude Code CLI took longer than {self.timeout:.0f} s") from e
        try:
            out = json.loads(proc.stdout)
        except json.JSONDecodeError:
            raise AgentError(f"Claude Code CLI exited {proc.returncode} without JSON: {(proc.stderr or proc.stdout)[:300]}") from None
        if out.get("is_error"):
            msg = str(out.get("result", "unknown error"))
            if out.get("api_error_status") == 401:
                msg += " (run `claude` in a terminal and log in again)"
            raise AgentError(f"Claude Code CLI: {msg}")
        self.session = out.get("session_id") or self.session
        structured = out.get("structured_output")
        if isinstance(structured, dict):
            return structured
        return parse_json_text(str(out.get("result", "")))


Opener = Callable[[urllib.request.Request, float], Any]


class OpenAICompatBackend:
    """POST {base_url}/chat/completions with a json_schema response_format (Ollama, llama.cpp, LM Studio, vLLM)."""

    name = "openai"

    def __init__(self, base_url: str = "http://127.0.0.1:11434/v1", model: str = "qwen3:4b", api_key: str | None = None,
                 temperature: float = 0.4, timeout: float = 600.0, opener: Opener | None = None,
                 reasoning: str | None = "none", max_tokens: int = 4096):
        # reasoning: sent as reasoning_effort. "none" turns thinking off on Ollama's thinking models (qwen3
        # took over 10 minutes per reply with it on, 3 s without); None leaves the field out for servers
        # that reject it. max_tokens stops a runaway reply instead of hanging until the timeout.
        self.reasoning, self.max_tokens = reasoning, max_tokens
        self.base_url, self.model, self.api_key = base_url.rstrip("/"), model, api_key
        self.temperature, self.timeout = temperature, timeout
        self.opener = opener or (lambda req, timeout: urllib.request.urlopen(req, timeout=timeout))
        self.messages: list[dict[str, str]] = []
        self.schema: dict[str, Any] = {}

    def start(self, system: str, schema: dict[str, Any]) -> None:
        self.schema = schema
        # Small models follow the schema better when they also see it in the prompt.
        self.messages = [{"role": "system", "content": f"{system}\n\nReply with one JSON object matching this schema:\n{json.dumps(schema)}"}]

    def ask(self, user_text: str) -> dict[str, Any]:
        self.messages.append({"role": "user", "content": user_text})
        body = {
            "model": self.model,
            "messages": self.messages,
            "temperature": self.temperature,
            "max_tokens": self.max_tokens,
            "response_format": {"type": "json_schema", "json_schema": {"name": "patch", "schema": self.schema, "strict": True}},
        }
        if self.reasoning is not None:
            body["reasoning_effort"] = self.reasoning
        headers = {"Content-Type": "application/json"}
        if self.api_key:
            headers["Authorization"] = f"Bearer {self.api_key}"
        req = urllib.request.Request(f"{self.base_url}/chat/completions", data=json.dumps(body).encode(), headers=headers, method="POST")
        try:
            with self.opener(req, self.timeout) as resp:
                reply = json.loads(resp.read().decode("utf-8"))
        except urllib.error.HTTPError as e:
            raise AgentError(f"{self.base_url} returned {e.code}: {e.read().decode('utf-8', 'replace')[:300]}") from e
        except (urllib.error.URLError, TimeoutError, OSError) as e:
            raise AgentError(f"could not reach {self.base_url}: {e}") from e
        try:
            choice = reply["choices"][0]
            text = choice["message"].get("content") or ""
        except (KeyError, IndexError, TypeError) as e:
            raise AgentError(f"unexpected reply from {self.base_url}: {str(reply)[:300]}") from e
        if choice.get("finish_reason") == "length":
            raise AgentError("the model ran out of tokens before the JSON was complete")
        data = parse_json_text(text)
        self.messages.append({"role": "assistant", "content": text})
        return data
