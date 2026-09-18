# Agentic Development: Mac Studio + Windows Inference Server

Agentic coding runs on the **Mac Studio** (VSCodium + CLINE). LLM inference runs on the
**Windows 11 Pro** machine, which hosts Unsloth Studio as a network inference server.
No cloud API is involved — CLINE talks to Unsloth over the LAN.

| Role | Machine |
|---|---|
| Dev workstation (editor, agent, build) | Mac Studio — `/Users/gery/code/provpilot` |
| Inference server (Unsloth Studio) | Windows 11 Pro — `http://192.168.178.151:8888/v1` |

**Verified from the Mac:** UI at `http://192.168.178.151:8888/` answers HTTP 200 in
~30 ms, and `/v1/models` returns `401 Not authenticated` without a key — i.e. the server
is bound to the LAN and Windows Firewall already lets TCP 8888 through.

The Mac can *also* keep its own local Unsloth Studio (`http://localhost:8888/v1`, model
`Qwen3.8-27B-Uncensored-Q6_K`) as a fallback / second model — CLINE supports multiple API
configs and you can switch models per chat.

## 1. How the two LLMs are used

- **CLINE's primary model stays local** (`http://localhost:8888/v1` on the Mac Studio).
  No CLINE reconfiguration is needed for remote inference — your existing auto-approve
  rules in `.vscode/settings.json` keep working unchanged.
- **Heavy subtasks are delegated** from inside an agent session to the Windows model via
  `scripts/llm_delegate.py` (usage in `.clinerules/provpilot.md`). Verified facts:
  - Loaded model: `Qwen3.8-27B-UD-Q4_K_XL`; a second model
    (`Qwen3.8-27B-Uncensored-Q6_K`) is registered but not loaded.
  - Context per call: ~44,544 tokens (native 262K — Unsloth sizes the loaded context to
    VRAM). Keep single delegations under that; pipe only relevant files.
  - Latency when warm: sub-second round trip on the LAN.

Optional: register a second OpenAI Compatible config in CLINE pointing at
`http://192.168.178.151:8888/v1` (key from the Windows Studio UI) if you ever want to run
CLINE directly against the remote model instead of delegating.

## 2. Keeping the Windows inference server healthy

- Keep the machine **awake** (Settings → Power → sleep: never, or at least while an
  agent session runs) and keep Unsloth Studio running with the model loaded.
- If you change models on the Windows box, update `LLM_REMOTE_MODEL` in
  `scripts/.llm_remote.env` to match what `/v1/models` reports.
- If connectivity breaks, check in this order: Windows power state → firewall rule for
  TCP 8888 (private profile) → that the Unsloth server is bound to all interfaces, not
  just localhost → `ping 192.168.178.151` from the Mac.
- Model choice for agentic coding: tool-calling quality matters more than raw size —
  Qwen3.x-class models work well with CLINE. Agentic tasks burn a lot of context, so
  prefer the largest quant that fits comfortably in VRAM and raise the context length
  in Unsloth if headroom allows.

## 3. Git discipline for agent sessions

- Work on a branch per task (e.g. `terrain-tiles`, `c172p-fuel`); agents **never commit
  to `main` directly**. Small, incremental commits; use CLINE checkpoints to review
  before pushing.
- Pull/rebase `main` before starting a session; push + merge when done (fast-forward
  preferred, or a GitLab MR).
- Submodule pointer bumps (`jsbsim`, `godot`, `provpilot/addons/scenery3d`) require
  rebuilding JSBSim + GDExtensions on every machine that builds the project — see the
  ⚠️ note in `WINDOWS_SETUP.md` about stale `JSBSim.lib`.

If you also develop directly on the Windows box (VSCodium + CLINE there), follow
`WINDOWS_SETUP.md` for the toolchain and keep the same branch discipline; split work by
subsystem when both machines run agents at once.

## 4. State that does NOT sync through git

| Item | Note |
|---|---|
| `.vscode/` | gitignored — per-OS copy of `settings.json` (only relevant if Windows also develops) |
| `.venv/`, `provpilot/bin/*.{dylib,dll}`, `jsbsim/build/`, `godot/bin/` | built per machine that compiles the project |
| CLINE API configs + auto-approve rules | configured in the UI on each machine |
| Unsloth Studio model files + API keys | per machine — Mac key ≠ Windows key |

## 5. Repo hygiene (do once)

- **Untracked directories that currently only exist on the Mac:** `aircraft/`,
  `schmerlat/`, `scenery/scenery`, `provpilot/Assets/custom`, `provpilot/Assets/trees/`.
  Decide track-vs-ignore for each so any future clone (e.g. on Windows) starts identical.
- **No `.gitattributes` in the repo.** Add one to pin line endings (e.g. `*.ps1` → CRLF,
  `*.sh`, `*.gd`, `*.cpp`, `*.h` → LF) before any second machine starts committing.
