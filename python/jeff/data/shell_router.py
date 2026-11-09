"""Shared definitions for the smart-shell router (exp_005).

Decision: is a line typed at a bash prompt a valid command (RUN it) or not (send it
to the AGENT as a prompt)?

"Valid" means a bash command that could run as written on a typical Linux system,
ignoring whether specific files or installed tools exist. So `kubectl get pods` is
valid anywhere, while `find the largest file`, `git psuh` and `tar -xzf` are not.
"""

import os
import re
import subprocess

OPTIONS = [
    "RUN: it is a valid bash command, so execute it in the shell",
    "AGENT: it is not a valid bash command, so send it to the AI agent as a prompt",
]
RUN, AGENT = 0, 1

# Categories where running the text in bash would execute the user's prose.
ENGLISH_CATEGORIES = {"nl_plain", "nl_cmdlike"}
INVALID_CATEGORIES = ENGLISH_CATEGORIES | {
    "other_code", "syntax", "typo_cmd", "bad_subcmd", "bad_flag", "missing_args",
}

_PREFIXES = {"sudo", "env", "time", "nohup", "command", "exec", "builtin", "nice"}
_ASSIGNMENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(\+)?=")
_BASH_ENV = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C"}


def make_context(text: str) -> str:
    return (
        "A user typed the following line at an interactive bash prompt. Decide whether it is a "
        "valid bash command that could run as written on a typical Linux system, ignoring whether "
        "specific files or installed tools exist. Natural language, code in other languages, "
        "broken syntax, misspelled commands or subcommands, nonexistent flags and missing "
        f"required arguments are not valid.\n\nTyped line: {text}"
    )


def make_example(text: str, valid: bool, **meta) -> dict:
    return {"context": make_context(text), "options": OPTIONS,
            "correct_index": RUN if valid else AGENT, "text": text, **meta}


def usable_line(text: str) -> bool:
    """Single-line input that cannot collide with the chat template or prompt parsing."""
    return bool(text.strip()) and "\n" not in text and "\r" not in text and "<|" not in text


def bash_parses(text: str) -> bool:
    """Whether bash can parse the text. `bash -n` reads stdin and never executes it."""
    try:
        r = subprocess.run(["bash", "-n"], input=text + "\n", text=True, capture_output=True,
                           timeout=5, env=_BASH_ENV)
    except (subprocess.TimeoutExpired, UnicodeError, ValueError):
        return False
    return r.returncode == 0


def first_word(text: str, skip_prefixes: bool = True) -> str:
    """The command word: skips assignments and, optionally, wrappers such as sudo."""
    for tok in text.lstrip("({! ").split():
        if _ASSIGNMENT.match(tok):
            continue
        if skip_prefixes and tok in _PREFIXES:
            continue
        if skip_prefixes and tok.startswith("-"):      # e.g. `sudo -u bob ls`: stop looking
            return ""
        return tok.rstrip(");}&|")
    return ""
