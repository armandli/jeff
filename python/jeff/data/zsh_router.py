"""Shared definitions for the zsh router (exp_006).

Decision: is a line typed at an interactive zsh prompt a valid zsh command
(RUN it) or not (send it to the AGENT as a prompt)?

"Valid" means a zsh command that could run as written on a typical Linux/macOS
system, ignoring whether specific files or installed tools exist. So `kubectl get
pods` is valid anywhere, while `find the largest file`, `git psuh` and `tar -xzf`
are not.

Unlike exp_005 there is no safety override: a dangerous but valid command such as
`rm -rf /` is labelled RUN. The router decides only whether the line is valid zsh.
"""

import os
import re
import shutil
import subprocess

OPTIONS = [
    "RUN: it is a valid zsh command, so execute it in the shell",
    "AGENT: it is not a valid zsh command, so send it to the AI agent as a prompt",
]
RUN, AGENT = 0, 1

# Categories where running the text in zsh would execute the user's prose.
ENGLISH_CATEGORIES = {"nl_plain", "nl_cmdlike"}
INVALID_CATEGORIES = ENGLISH_CATEGORIES | {
    "other_code", "syntax", "typo_cmd", "bad_subcmd", "bad_flag", "missing_args",
}

_PREFIXES = {"sudo", "env", "time", "nohup", "command", "exec", "builtin", "nice"}
_ASSIGNMENT = re.compile(r"^[A-Za-z_][A-Za-z0-9_]*(\+)?=")
_ZSH_ENV = {"PATH": os.environ.get("PATH", "/usr/bin:/bin"), "LC_ALL": "C"}


def make_context(text: str) -> str:
    return (
        "A user typed the following line at an interactive zsh prompt. Decide whether it is a "
        "valid zsh command that could run as written on a typical Linux system, ignoring whether "
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


def zsh_parses(text: str) -> bool:
    """Whether zsh can parse the text. `zsh -n` reads stdin and never executes it."""
    if not shutil.which("zsh", path=_ZSH_ENV["PATH"]):
        raise RuntimeError("zsh is not installed (or not on PATH); exp_006 needs it: "
                           "sudo apt install zsh")
    try:
        r = subprocess.run(
            ["zsh", "-n"],
            input=text + "\n",
            text=True,
            capture_output=True,
            timeout=5,
            env=_ZSH_ENV,
        )
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
        if skip_prefixes and tok.startswith("-"):
            return ""
        return tok.rstrip(");}&|")
    return ""


def command_word(text: str) -> str:
    """The command word for a known-command check, or "" when the line has none: (( ))
    arithmetic and [[ ]] tests are not commands, which first_word would return as x++ or -f."""
    if text.lstrip().startswith(("((", "[[")):
        return ""
    return first_word(text)


def pick_threshold_accuracy(rows: list[dict], p_run: list[float], cap: float,
                            n_candidates: int = 400) -> float:
    """P(RUN) threshold with the best accuracy among those keeping english_run_rate <= cap.

    pick_threshold returns the lowest threshold meeting the cap, which maximises RUN
    decisions and so executes other invalid input (typos, other languages) whenever the
    model is confident on English; this one trades that off against command_to_agent."""
    lowest, _ = pick_threshold(rows, p_run, cap)
    cands = sorted(set(p for p in p_run if p >= lowest))
    if len(cands) > n_candidates:
        cands = [cands[int(i * (len(cands) - 1) / (n_candidates - 1))] for i in range(n_candidates)]
    cands = [lowest] + cands + [1.0 + 1e-9]
    valid = [r["correct_index"] == RUN for r in rows]
    best_t, best_acc = lowest, -1.0
    for t in cands:
        acc = sum((p >= t) == v for p, v in zip(p_run, valid))
        if acc > best_acc:
            best_t, best_acc = t, acc
    return best_t


def pick_threshold(rows: list[dict], p_run: list[float], cap: float) -> tuple[float, bool]:
    """Lowest P(RUN) threshold that keeps english_run_rate <= cap on these rows.

    Returns (threshold, cap_met). With no English rows any threshold meets the cap.
    """
    english = sorted(p for r, p in zip(rows, p_run) if r["category"] in ENGLISH_CATEGORIES)
    allowed = int(cap * len(english))
    if allowed >= len(english):
        return 0.0, True
    threshold = english[-allowed - 1] + 1e-9
    return threshold, threshold <= 1.0


# "line" context mode (exp_006 short-context variant): the encoder sees only the typed line
# and one-word options, no instruction prompt, so a decision encodes ~10x fewer tokens.
LINE_OPTIONS = ["RUN", "AGENT"]
CONTEXT_MODES = ("prompt", "line")


def apply_context_mode(row: dict, mode: str) -> dict:
    """Return the row as the model sees it in this context mode (rows are not modified)."""
    if mode == "prompt":
        return row
    if mode == "line":
        return {**row, "context": row["text"], "options": list(LINE_OPTIONS)}
    raise ValueError(f"unknown context mode {mode!r}; choices: {CONTEXT_MODES}")


def read_context_mode(checkpoint_dir) -> str:
    """Context mode a checkpoint was trained with: <run dir>/context_mode.txt, default prompt."""
    from pathlib import Path
    f = Path(checkpoint_dir).parent / "context_mode.txt"
    return f.read_text().strip() if f.exists() else "prompt"

