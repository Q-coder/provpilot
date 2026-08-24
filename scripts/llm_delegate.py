#!/usr/bin/env python3
"""Delegate a task to the remote Unsloth Studio model (Windows inference server).

The Mac-local model stays CLINE's primary; use this script from inside an agent
session to hand a subtask to the bigger remote model.

Usage:
  python3 scripts/llm_delegate.py "prompt text"
  cat src/foo.cpp | python3 scripts/llm_delegate.py "Explain the invariants of this code"
  python3 scripts/llm_delegate.py --system "You are a strict C++ reviewer." \
      --temp 0.2 --max-tokens 8192 "Review: ..."
  python3 scripts/llm_delegate.py --no-think -v "Quick mechanical task: ..."

--no-think disables the Qwen3 reasoning phase for this call (much faster and
leaves the whole output budget for the answer). It sends
chat_template_kwargs={"enable_thinking": false} AND appends the "/no_think"
soft switch to the prompt, so it works even on servers that ignore the kwarg.
Good for mechanical subtasks; keep thinking on for deep reviews.
-v prints finish_reason + token usage to stderr after each call.

Every call is appended as one JSON line to scripts/.llm_remote.log.jsonl
(gitignored; override with LLM_REMOTE_LOG) so agent sessions can review usage
and answer quality afterwards:
  python3 scripts/llm_delegate.py --stats        # summary + last 10 calls
  python3 scripts/llm_delegate.py --stats 25     # ...last 25 calls

When both piped stdin and a prompt argument are given, the stdin content is attached
as <context> before the prompt.

Configuration (highest priority first):
  1. Environment variables LLM_REMOTE_URL / LLM_REMOTE_KEY / LLM_REMOTE_MODEL
  2. scripts/.llm_remote.env (gitignored, KEY=VALUE lines)
  3. Built-in defaults below

Output: the model's message content on stdout; diagnostics on stderr.
Exit codes: 0 = ok, 1 = HTTP/API error, 2 = config/usage error.
"""

import datetime
import json
import os
import sys
import time
import urllib.error
import urllib.request

DEFAULTS = {
    "LLM_REMOTE_URL": "http://192.168.178.151:8888/v1",
    "LLM_REMOTE_KEY": "",
    "LLM_REMOTE_MODEL": "Qwen3.8-27B-UD-Q4_K_XL",
}

ENV_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".llm_remote.env")
LOG_FILE = os.environ.get(
    "LLM_REMOTE_LOG",
    os.path.join(os.path.dirname(os.path.abspath(__file__)), ".llm_remote.log.jsonl"),
)


def log_call(record):
    """Append one JSON line per call so usage/quality can be reviewed later."""
    try:
        with open(LOG_FILE, "a", encoding="utf-8") as f:
            f.write(json.dumps(record, ensure_ascii=False) + "\n")
    except OSError as e:
        print(f"warning: could not write call log {LOG_FILE}: {e}", file=sys.stderr)


def _read_log():
    if not os.path.isfile(LOG_FILE):
        return []
    records = []
    with open(LOG_FILE, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line:
                continue
            try:
                records.append(json.loads(line))
            except json.JSONDecodeError:
                pass
    return records


def print_stats(last_n=10):
    """Summarise the call log: frequency, latency, tokens, quality flags."""
    records = _read_log()
    if not records:
        print(f"No calls logged yet (log file {LOG_FILE} is missing or empty).")
        return 0

    ok = [r for r in records if r.get("ok")]
    bad = [r for r in records if not r.get("ok")]
    days = {}
    for r in records:
        days.setdefault(str(r.get("ts", "?"))[:10], []).append(r)

    print(f"LLM delegate usage — {LOG_FILE}")
    print(f"Total calls: {len(records)}  (ok: {len(ok)}, failed: {len(bad)})")
    if ok:
        times = [float(r.get("elapsed_s", 0.0)) for r in ok]
        pt = sum(int(r.get("prompt_tokens") or 0) for r in ok)
        ct = sum(int(r.get("completion_tokens") or 0) for r in ok)
        print(f"Latency (ok calls): avg {sum(times) / len(times):.1f}s, "
              f"min {min(times):.1f}s, max {max(times):.1f}s")
        print(f"Tokens: {pt} prompt + {ct} completion")
    no_content = [r for r in records if int(r.get("content_chars") or 0) == 0]
    if no_content:
        print(f"Quality flags: {len(no_content)} call(s) returned empty content "
              f"(token budget burned on reasoning, or an error)")
    models = {}
    for r in records:
        m = r.get("model", "?")
        models[m] = models.get(m, 0) + 1
    print("\nBy model:")
    for m in sorted(models):
        print(f"  {m}: {models[m]} call(s)")
    print("\nBy day:")
    for d in sorted(days):
        rs = days[d]
        ro = [r for r in rs if r.get("ok")]
        avg = (sum(float(r.get("elapsed_s", 0.0)) for r in ro) / len(ro)) if ro else 0.0
        print(f"  {d}: {len(rs)} call(s), ok {len(ro)}, avg {avg:.1f}s")

    n = min(last_n, len(records))
    print(f"\nLast {n} call(s):")
    for r in records[-n:]:
        status = "ok  " if r.get("ok") else "FAIL"
        tok = ""
        if r.get("prompt_tokens") is not None or r.get("completion_tokens") is not None:
            tok = f" pt={r.get('prompt_tokens')} ct={r.get('completion_tokens')}"
        think = " no-think" if r.get("no_think") else ""
        print(f"  {r.get('ts', '?')}  {status} {float(r.get('elapsed_s', 0.0)):6.1f}s{tok}{think}")
        prompt = (r.get("prompt_excerpt") or "").replace("\n", " ")
        print(f"      prompt: {prompt[:100]}{'...' if len(prompt) > 100 else ''}")
        resp = (r.get("response") or "").strip()
        if resp:
            snippet = resp.replace("\n", "\n      ")
            print(f"      answer: {snippet[:400]}{'...' if len(resp) > 400 else ''}")
        elif not r.get("ok"):
            err = (r.get("error") or "").replace("\n", " ")
            print(f"      error:  {err[:200]}")
    return 0


def load_env_file(path):
    if not os.path.isfile(path):
        return {}
    values = {}
    with open(path, encoding="utf-8") as f:
        for line in f:
            line = line.strip()
            if not line or line.startswith("#") or "=" not in line:
                continue
            key, _, val = line.partition("=")
            values[key.strip()] = val.strip().strip('"').strip("'")
    return values


def main():
    args = sys.argv[1:]
    opts = {"model": None, "system": None, "temp": None,
            "max_tokens": 4096, "timeout": 600,
            "no_think": False, "verbose": False,
            "stats": False, "stats_count": 10}
    prompt_parts = []
    i = 0
    while i < len(args):
        a = args[i]
        if a == "--model":
            i += 1; opts["model"] = args[i]
        elif a == "--system":
            i += 1; opts["system"] = args[i]
        elif a == "--temp":
            i += 1; opts["temp"] = float(args[i])
        elif a == "--max-tokens":
            i += 1; opts["max_tokens"] = int(args[i])
        elif a == "--timeout":
            i += 1; opts["timeout"] = int(args[i])
        elif a == "--no-think":
            opts["no_think"] = True
        elif a in ("-v", "--verbose"):
            opts["verbose"] = True
        elif a == "--stats":
            opts["stats"] = True
            if i + 1 < len(args) and args[i + 1].isdigit():
                i += 1
                opts["stats_count"] = int(args[i])
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            prompt_parts.append(a)
        i += 1

    if opts["stats"]:
        return print_stats(opts["stats_count"])

    stdin_text = ""
    if not sys.stdin.isatty():
        stdin_text = sys.stdin.read().strip()
    prompt = " ".join(prompt_parts).strip()
    if stdin_text and prompt:
        prompt = f"<context>\n{stdin_text}\n</context>\n\n{prompt}"
    elif stdin_text:
        prompt = stdin_text
    if not prompt:
        print("error: no prompt given (argument or stdin)", file=sys.stderr)
        return 2

    if opts["no_think"]:
        # Qwen3 hybrid-thinking soft switch: placed in the user message it tells
        # the chat template to skip the reasoning phase for this call.
        prompt = prompt + "\n\n/no_think"

    file_cfg = load_env_file(ENV_FILE)
    cfg = {k: os.environ.get(k, file_cfg.get(k, DEFAULTS[k])) for k in DEFAULTS}
    url = cfg["LLM_REMOTE_URL"].rstrip("/") + "/chat/completions"
    key = cfg["LLM_REMOTE_KEY"]
    model = opts["model"] or cfg["LLM_REMOTE_MODEL"]
    if not key:
        print("error: no API key (set LLM_REMOTE_KEY or fill scripts/.llm_remote.env)",
              file=sys.stderr)
        return 2

    messages = []
    if opts["system"]:
        messages.append({"role": "system", "content": opts["system"]})
    messages.append({"role": "user", "content": prompt})

    body = {
        "model": model,
        "messages": messages,
        "max_tokens": opts["max_tokens"],
        "stream": False,
    }
    if opts["temp"] is not None:
        body["temperature"] = opts["temp"]
    if opts["no_think"]:
        # Primary switch for vLLM/SGLang-style servers; the "/no_think" text
        # appended to the prompt above is the fallback for plain chat templates.
        body["chat_template_kwargs"] = {"enable_thinking": False}

    t0 = time.monotonic()
    base_record = {
        "ts": datetime.datetime.now().astimezone().isoformat(timespec="seconds"),
        "model": model,
        "prompt_chars": len(prompt),
        "context_chars": len(stdin_text),
        "no_think": opts["no_think"],
        "temp": opts["temp"],
        "max_tokens": opts["max_tokens"],
        "prompt_excerpt": prompt[:120],
    }

    def finish_record(ok, error=None, **extra):
        rec = dict(base_record)
        rec.update({"ok": ok, "elapsed_s": round(time.monotonic() - t0, 2)})
        if error is not None:
            rec["error"] = str(error)[:300]
        rec.update(extra)
        log_call(rec)

    req = urllib.request.Request(
        url,
        data=json.dumps(body).encode("utf-8"),
        headers={
            "Content-Type": "application/json",
            "Authorization": f"Bearer {key}",
        },
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=opts["timeout"]) as resp:
            data = json.load(resp)
    except urllib.error.HTTPError as e:
        detail = e.read().decode("utf-8", "replace")[:500]
        print(f"error: HTTP {e.code} from remote model: {detail}", file=sys.stderr)
        finish_record(False, error=f"HTTP {e.code}: {detail}")
        return 1
    except (urllib.error.URLError, TimeoutError) as e:
        print(f"error: cannot reach remote model at {url}: {e}", file=sys.stderr)
        print("hint: is the Windows machine awake and Unsloth Studio running?",
              file=sys.stderr)
        finish_record(False, error=f"unreachable: {e}")
        return 1

    try:
        choice = data["choices"][0]
        msg = choice["message"]
    except (KeyError, IndexError):
        print(f"error: unexpected response shape: {json.dumps(data)[:500]}",
              file=sys.stderr)
        finish_record(False, error="unexpected response shape")
        return 1

    content = (msg.get("content") or "").strip()
    reasoning = (msg.get("reasoning_content") or "").strip()
    finish = choice.get("finish_reason", "")
    usage = data.get("usage", {})

    if opts["verbose"]:
        print(f"[delegate] finish={finish!r} "
              f"prompt_tokens={usage.get('prompt_tokens')} "
              f"completion_tokens={usage.get('completion_tokens')} "
              f"reasoning_chars={len(reasoning)}", file=sys.stderr)

    if content:
        print(content)
        finish_record(True, finish_reason=finish,
                      prompt_tokens=usage.get("prompt_tokens"),
                      completion_tokens=usage.get("completion_tokens"),
                      reasoning_chars=len(reasoning),
                      content_chars=len(content), response=content)
        return 0

    # Thinking models may burn the token budget on reasoning before answering.
    print(f"warning: model returned no content (finish_reason={finish!r})",
          file=sys.stderr)
    if reasoning:
        print("reasoning_content was:", file=sys.stderr)
        print(reasoning[:2000], file=sys.stderr)
    print("hint: retry with a larger --max-tokens", file=sys.stderr)
    finish_record(False, error=f"empty content (finish_reason={finish!r})",
                  finish_reason=finish,
                  prompt_tokens=usage.get("prompt_tokens"),
                  completion_tokens=usage.get("completion_tokens"),
                  reasoning_chars=len(reasoning),
                  content_chars=0, response="")
    return 1


if __name__ == "__main__":
    sys.exit(main())
