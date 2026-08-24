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

When both piped stdin and a prompt argument are given, the stdin content is attached
as <context> before the prompt.

Configuration (highest priority first):
  1. Environment variables LLM_REMOTE_URL / LLM_REMOTE_KEY / LLM_REMOTE_MODEL
  2. scripts/.llm_remote.env (gitignored, KEY=VALUE lines)
  3. Built-in defaults below

Output: the model's message content on stdout; diagnostics on stderr.
Exit codes: 0 = ok, 1 = HTTP/API error, 2 = config/usage error.
"""

import json
import os
import sys
import urllib.error
import urllib.request

DEFAULTS = {
    "LLM_REMOTE_URL": "http://192.168.178.151:8888/v1",
    "LLM_REMOTE_KEY": "",
    "LLM_REMOTE_MODEL": "Qwen3.8-27B-UD-Q4_K_XL",
}

ENV_FILE = os.path.join(os.path.dirname(os.path.abspath(__file__)), ".llm_remote.env")


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
            "no_think": False, "verbose": False}
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
        elif a in ("-h", "--help"):
            print(__doc__)
            return 0
        else:
            prompt_parts.append(a)
        i += 1

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
        return 1
    except (urllib.error.URLError, TimeoutError) as e:
        print(f"error: cannot reach remote model at {url}: {e}", file=sys.stderr)
        print("hint: is the Windows machine awake and Unsloth Studio running?",
              file=sys.stderr)
        return 1

    try:
        choice = data["choices"][0]
        msg = choice["message"]
    except (KeyError, IndexError):
        print(f"error: unexpected response shape: {json.dumps(data)[:500]}",
              file=sys.stderr)
        return 1

    content = (msg.get("content") or "").strip()
    reasoning = (msg.get("reasoning_content") or "").strip()
    finish = choice.get("finish_reason", "")

    if opts["verbose"]:
        usage = data.get("usage", {})
        print(f"[delegate] finish={finish!r} "
              f"prompt_tokens={usage.get('prompt_tokens')} "
              f"completion_tokens={usage.get('completion_tokens')} "
              f"reasoning_chars={len(reasoning)}", file=sys.stderr)

    if content:
        print(content)
        return 0

    # Thinking models may burn the token budget on reasoning before answering.
    print(f"warning: model returned no content (finish_reason={finish!r})",
          file=sys.stderr)
    if reasoning:
        print("reasoning_content was:", file=sys.stderr)
        print(reasoning[:2000], file=sys.stderr)
    print("hint: retry with a larger --max-tokens", file=sys.stderr)
    return 1


if __name__ == "__main__":
    sys.exit(main())
