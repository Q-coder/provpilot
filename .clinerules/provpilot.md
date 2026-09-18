# ProvPilot — shared agent rules

Godot 4.7 flight simulator (custom **double-precision** engine) + C++ GDExtension
(`src/` → `provpilot/bin/libprovpilot`) + statically linked JSBSim. Agentic coding runs
on the Mac Studio; LLM inference is served by Unsloth Studio on the Windows box at
`http://192.168.178.151:8888/v1` (Mac-local fallback: `http://localhost:8888/v1`). Sync
via GitLab (`gitlab.md80.ch:gery/provpilot.git`). Full workflow:
`docs/AGENTS_TWO_MACHINE.md`.

## Build & run commands (use the ones for the machine you are running on)

### macOS (repo at /Users/gery/code/provpilot)
```bash
# editor
godot/bin/godot.macos.editor.double.arm64 --editor --path provpilot
# GDExtension (from repo root, inside .venv)
source .venv/bin/activate
scons platform=macos arch=arm64 precision=double -j$(sysctl -n hw.ncpu)
```

### Windows (run from a VS Developer Command Prompt / vcvars64)
```powershell
# editor
godot\bin\godot.windows.editor.double.x86_64.exe --editor --path provpilot
# GDExtension (from repo root)
scons arch=x86_64 precision=double
```

Full step-by-step: `docs/MACOS_SETUP.md` and `docs/WINDOWS_SETUP.md`.

## Conventions

- Engine is custom double precision — **every** GDExtension must be built with
  `precision=double` or the editor crashes at GDExtension verification.
- Built extension binaries land in `provpilot/bin/` (gitignored) and are per-machine.
- Scenery (terrain, buildings, roads, water) streams at runtime through the Scenery3D
  extension — it does **not** go through Godot's import pipeline. Don't "fix" missing
  scenery by importing assets.
- Python scenery-pipeline scripts live in `scripts/`; run them with the repo `.venv`.
- `provpilot/Assets/{buildings,roads,water}` are symlinks (macOS) / directory junctions
  (Windows) into `scenery/` — never replace them with real folders.

## Delegating tasks to the remote model (Windows box)

CLINE's primary model stays local (Mac Unsloth Studio). For heavy lifting, delegate a
subtask from the terminal to the bigger model on the Windows inference server:

```bash
python3 scripts/llm_delegate.py "Review this diff and list concrete risks: <paste diff>"
cat src/foo.cpp | python3 scripts/llm_delegate.py "Explain the invariants this code relies on"
python3 scripts/llm_delegate.py --system "You are a strict C++ reviewer." --temp 0.2 "..."
# Mechanical subtask (rename, extract, reformat, quick lookup): skip the reasoning
# phase for a much faster, cheaper call that spends its whole budget on the answer.
python3 scripts/llm_delegate.py --no-think -v "List every function defined in this file"
```

- Endpoint/model/key live in gitignored `scripts/.llm_remote.env` (env vars override).
- Model: `Qwen3.8-27B-UD-Q4_K_XL`, ~44K-token context per call — keep single delegations
  under that; pipe only the relevant files, not whole trees. It is a thinking model, so
  allow generous `--max-tokens` (default 4096) for long answers. Add `-v` to print token
  usage to stderr (useful for spotting calls that are near the context limit).
- Thinking vs no-thinking: leave thinking ON for deep reviews / hard refactors (it burns
  part of `--max-tokens` on reasoning first). Use `--no-think` for mechanical, well-scoped
  subtasks — it sends `chat_template_kwargs={"enable_thinking": false}` plus the `/no_think`
  prompt switch, so it works even if the server ignores the kwarg. Measured on a short C++
  question: 30.7s / 1325 completion tokens (thinking) vs 2.4s / ~150 (no-thinking).
- Delegate for: large refactors, reviewing big diffs, long doc/code generation, second
  opinions. Keep routine edits on the local model.
- If it fails with a connection error: Windows machine asleep or Unsloth Studio not
  running / model not loaded (check `http://192.168.178.151:8888/` from the Mac).

## Git workflow

- Work on a branch per task; **never commit to `main` directly** from an agent session.
  Small, incremental commits.
- Pull/rebase `main` before starting a task; push + merge when done.
- After any submodule pointer bump (`jsbsim`, `godot`, `provpilot/addons/scenery3d`),
  JSBSim and the GDExtensions must be rebuilt on both machines (stale `JSBSim.lib`
  causes a garbage `GetIC()` crash — see `WINDOWS_SETUP.md`).
