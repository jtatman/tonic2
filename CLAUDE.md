# Project Instructions for AI Agents

This file provides instructions and context for AI coding agents working on this project.

<!-- BEGIN BEADS INTEGRATION v:1 profile:minimal hash:1105d646 -->
## Beads Issue Tracker

This project uses **bd (beads)** for issue tracking. Run `bd prime` to see full workflow context and commands.

### Quick Reference

```bash
bd ready              # Find available work
bd show <id>          # View issue details
bd update <id> --claim  # Claim work
bd close <id>         # Complete work
```

### Rules

- Use `bd` for ALL task tracking — do NOT use TodoWrite, TaskCreate, or markdown TODO lists
- Run `bd prime` for detailed command reference and session close protocol
- Use `bd remember` for persistent knowledge — do NOT use MEMORY.md files

**Architecture in one line:** issues live in a local Dolt DB; sync uses `refs/dolt/data` on your git remote; `.beads/issues.jsonl` is a passive export. See https://github.com/gastownhall/beads/blob/main/docs/core-concepts/sync-concepts.md for details and anti-patterns.

## Agent Context Profiles

The managed Beads block is task-tracking guidance, not permission to override repository, user, or orchestrator instructions.

- **Conservative (default)**: Use `bd` for task tracking. Do not run git commits, git pushes, or Dolt remote sync unless explicitly asked. At handoff, report changed files, validation, and suggested next commands.
- **Minimal**: Keep tool instruction files as pointers to `bd prime`; use the same conservative git policy unless active instructions say otherwise.
- **Team-maintainer**: Only when the repository explicitly opts in, agents may close beads, run quality gates, commit, and push as part of session close. A current "do not commit" or "do not push" instruction still wins.

## Session Completion

This protocol applies when ending a Beads implementation workflow. It is subordinate to explicit user, repository, and orchestrator instructions.

1. **File issues for remaining work** - Create beads for anything that needs follow-up
2. **Run quality gates** (if code changed) - Tests, linters, builds
3. **Update issue status** - Close finished work, update in-progress items
4. **Handle git/sync by active profile**:
   ```bash
   # Conservative/minimal/default: report status and proposed commands; wait for approval.
   git status

   # Team-maintainer opt-in only, unless current instructions forbid it:
   git pull --rebase
   git push
   git status
   ```
5. **Hand off** - Summarize changes, validation, issue status, and any blocked sync/commit/push step

**Critical rules:**
- Explicit user or orchestrator instructions override this Beads block.
- Do not commit or push without clear authority from the active profile or the current user request.
- If a required sync or push is blocked, stop and report the exact command and error.
<!-- END BEADS INTEGRATION -->



<!-- BEGIN PROJECT SECTION (keep identical in CLAUDE.md and AGENTS.md) -->
## Workflow and tooling

- **Tasks / short-term memory:** `bd` (beads). Start with `bd ready`. Phase 1 milestones are also
  tracked in the Tonic repo (epic `Tonic-cno`).
- **Long-term memory and development process:** `mnemoria` (store in `./mnemoria`, committed).
  Read: `mnemoria search "<topic>"`. Write: `mnemoria add -a <agent> -t decision|discovery|intent --summary "..." "..."`.
  Process rules live in mnemoria, not in this file.
- **Code analysis:** `codegraph` (index in `.codegraph/`, not committed). Refresh with `codegraph sync`.
- **Git:** commit after every significant change and at checkpoints. `origin` is jtatman/tonic2.
- Keep this section identical in `CLAUDE.md` and `AGENTS.md`.

## Build & Test

Builds run in WSL (g++ 15, cmake 4.x) or on the Mac `osx` over ssh (Apple clang 14, no cmake: compile
directly). CI covers Linux, Windows (MSVC) and macOS.

Windows (this machine, Visual Studio 2022): `tools/build_windows.ps1` builds everything into `build-win/`
(plugins in `build-win/bin/tonic2_poly.{vst3,clap,lv2}`) and runs ctest. Real-host test:
`tools/host_render.ps1` renders a song through the VST3 with the songengine Go VST3 host
(`C:/Users/stora/Downloads/VSTHostSynthesizerEngine`, override with `TONIC2_SONGENGINE`). Reaper is not
used anywhere; use the songengine host for VST3 checks.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && cmake --build build -j && ctest --test-dir build --output-on-failure
build/tonic2_golden --update tests/golden   # only for an intentional change to a reference render
```

## Architecture Overview

- Design: `docs/phase1-core-design.md` in the Tonic repo (branch `phase1-design`).
- Layers (bottom up): DSP kernels (`include/tonic/dsp`, M1) -> graph builder / compiled graph
  (`include/tonic/graph`, M2) and engine with lock-free graph swap (`include/tonic/engine`, M2) ->
  parameters, macros, note events as per-sample control buffers + offline render/WAV (`include/tonic/render`, M3) ->
  patching syntax over signal nodes (`include/tonic/dsl`, `graph/signal_nodes.hpp`, M4) ->
  voices (`graph/voice_node.hpp`, `Patch::voices`, M5) ->
  mixer + FX matrix (`dsl/mixer.hpp`, `graph/mixer_nodes.hpp`, M6). Offline analysis: `include/tonic/analysis` (STFT distance, CMA-ES,
  sound matching, audio features; CLI `tools/match.cpp` -> `tonic2_match`); text-to-patch agent in Python
  (`tools/patch_agent/`, pluggable LLM backends: Anthropic API, Claude Code CLI, native Ollama, OpenAI-style server; optional audio listening critic `critic.py`; drives `tonic2_render --layout/--set/--features`). Plugin formats live in the DPF adapter (`adapters/dpf`, M7;
  build with -DTONIC2_BUILD_DPF=ON after `git submodule update --init third_party/DPF`).

## Conventions & Patterns

- Kernels: plain structs with `prepare(sampleRate)`, `reset()`, `process(...)`; no virtuals, no
  allocation after prepare, no globals, no exceptions. `float` audio, `double` for phase/state.
- Every kernel gets a behavior test plus a golden render in `tests/renders.hpp`.
- Golden files must stay bit-stable across compilers; if a change breaks that, find out why first.
- Audio-thread code (`Node::process`, `Engine::process`) must not allocate, free, lock or throw;
  `tests/graph_tests.cpp` counts allocations on the audio thread. Graphs are immutable once compiled.
<!-- END PROJECT SECTION -->
