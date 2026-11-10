"""Experiment 006 — build RUN/AGENT data for a zsh router.

Data sources are the same downloads as exp_005 (data/raw/exp_005), but:
  - syntax validation uses `zsh -n` instead of `bash -n`
  - cli1m rows with shell == "zsh" are used (exp_005 uses shell == "bash")
  - zsh-specific builtins, globbing and interactive commands are added
  - ALL text strings that appeared in the exp_005 dataset are banned, so the
    two datasets are non-overlapping at the string level
  - the group-hash prefix "e6:" shifts train/val/test split assignments

    python/.venv/bin/python python/experiments/exp_006_zsh_router/prepare_data.py
"""

import collections
import hashlib
import json
import random
import re
import shlex
import subprocess
import sys
import tarfile
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

import pyarrow.parquet as pq
import yaml

EXP_DIR = Path(__file__).resolve().parent
REPO_ROOT = EXP_DIR.parents[2]
sys.path.insert(0, str(REPO_ROOT / "python"))

from jeff.data.zsh_router import first_word, make_example, usable_line, zsh_parses  # noqa: E402

METACHARS = re.compile(r"[|;&<>$`()\\{}*?!#]")

# Zsh builtins (superset of bash; adds zsh-specific ones)
ZSH_WORDS = """alias autoload bg bindkey break builtin caller case cd chdir command compctl compdef
compinit complete compadd continue declare dirs disown do done echo elif else emulate enable esac
eval exec exit export false fc fg fi float for function getopts hash help history if in integer
jobs kill let local logout mapfile popd print printf pushd pwd read readonly return select set
setopt shift source suspend test then time times trap true type typeset ulimit umask unalias
unfunction unhash unset unsetopt until vared wait while zle zmodload zstyle""".split()

SUBCOMMANDS = {
    "git": "add am archive bisect blame branch checkout cherry-pick clean clone commit config diff "
           "fetch grep init log merge mv pull push rebase reflog remote reset restore revert rm "
           "show stash status submodule switch tag worktree",
    "docker": "attach build commit compose container cp create exec image images inspect kill load "
              "login logs network ps pull push restart rm rmi run save start stats stop system tag "
              "top volume",
    "kubectl": "annotate apply attach autoscale cluster-info config cordon cp create delete describe "
               "drain edit exec expose get label logs patch port-forward rollout run scale taint top",
    "systemctl": "daemon-reload disable enable is-active is-enabled list-units mask reload restart "
                 "show start status stop unmask",
    "apt": "autoremove full-upgrade install list purge remove search show update upgrade",
    "npm": "audit build ci config exec init install link list outdated publish run start test "
           "uninstall update version",
    "pip": "cache check config download freeze hash index inspect install list show uninstall wheel",
    "cargo": "add bench build check clean clippy doc fetch fmt init install new publish run test "
             "tree update",
    "conda": "activate clean config create deactivate env info init install list remove run search "
             "update",
    "brew": "cleanup doctor info install leaves link list outdated search services tap uninstall "
            "unlink update upgrade",
}
FLAG_TOOLS = """ls cat cp mv rm mkdir grep sort head tail wc chmod chown du df touch ln uniq cut tr date
tar sed""".split()
MIN_OPERANDS = {"cp": 2, "mv": 2, "chmod": 2, "chown": 2, "scp": 2, "ln": 1, "grep": 1, "rm": 1,
                "mkdir": 1, "touch": 1, "kill": 1, "rmdir": 1}

FILES = """notes.txt report.pdf main.py app.log config.yaml data.csv README.md backup.tar.gz index.html
build.sh image.png server.js test.c output.json""".split()
DIRS = "src logs build /tmp /var/log ~/projects/app data ./dist /etc/nginx docs".split()
HOSTS = "example.com db01 user@server.example.com 10.0.0.5".split()
PLACEHOLDERS = [("url", "https://example.com/data"), ("host", "example.com"), ("port", "8080"),
                ("user", "alice"), ("branch", "main"), ("pattern", "error"), ("name", "app"),
                ("package", "vim"), ("image", "nginx"), ("number", "5"), ("string", "hello")]

# Real names for template slots that zsh checks at run time: an unknown option, parameter
# flag or widget is an error, so filling these from CODE_NAMES (`setopt users`) would
# label invalid lines RUN.
ZSH_OPTIONS = """autocd autopushd correct extendedglob globdots histignorealldups histignorespace
incappendhistory interactivecomments nocaseglob nomatch nullglob numericglobsort promptsubst
pushdignoredups sharehistory""".split()
PARAM_FLAGS = ["U", "L", "C", "q", "Q", "k", "v", "o", "O", "u", "f", "j:,:", "s:/:", "@"]
ZLE_BINDINGS = [("'^R'", "history-incremental-search-backward"), ("'^A'", "beginning-of-line"),
                ("'^E'", "end-of-line"), ("'^K'", "kill-line"), ("'^L'", "clear-screen"),
                ("'^W'", "backward-kill-word"), ("'^[f'", "forward-word"),
                ("'^[b'", "backward-word"), ("'^U'", "backward-kill-line")]
REAL_CMDS = "cat sort head tail wc less uniq".split()

# Zsh-specific interactive commands not in exp_005
INTERACTIVE = """setopt|unsetopt|autoload -U compinit|compinit|autoload -U colors|colors
bindkey -v|bindkey -e|bindkey '^R' history-incremental-search-backward
zle -N widget fn|zmodload zsh/complist|zstyle ':completion:*' menu select
print -P '%F{blue}%~%f'|print -l ${(f)"$(ls)"}|print -r -- $var
source ~/.zshrc|source ~/.zprofile|. ~/.zshenv|exec zsh
zsh --version|autoload -Uz vcs_info|precmd() { vcs_info }
typeset -A assoc|assoc=(key val)|echo ${assoc[key]}|echo ${(k)assoc}
a=(foo bar baz)|echo $a[1]|echo $a[-1]|echo ${#a}|echo ${(j:,:)a}
echo ${var:-default}|echo ${var:=default}|echo ${(U)var}|echo ${(L)var}
echo ${(q)var}|echo ${var##*/}|echo ${var%.*}|echo ${var:0:5}
ls **/*.py|ls **/*(.)| rm -- *(m+30)|ls *(Om[1,5])
diff =(head -5 a.txt) =(head -5 b.txt)|cat =(echo hello)
() { echo $1 } hello|function greet { echo "hello $1" }
integer x=5|float y=3.14|local -a arr|local -A map
RPROMPT='%T'|PROMPT='%n@%m %~ %# '|PS1='%B%n%b@%m:%~$ '
noglob ls *.txt|nocorrect make|nohup zsh script.sh
repeat 3 { echo hi }|while read line; do echo $line; done <<< "hello"
zsh -c 'echo hello'|zsh -n script.sh|zsh -x script.sh""".replace(
    "\n", "|").split("|")
INTERACTIVE = [t.strip() for t in INTERACTIVE]
INTERACTIVE_T = ["ls **/*.{f}", "rm -- *(m+{v})", "ls {d}/**/*.py", "print -P '%F{{green}}{n}%f'",
                 "source {d}/init.zsh", "zsh {f}", "autoload -Uz {n}", "typeset -A {n}",
                 "a=({f} {d}); echo $a[1]", "echo ${{({p}){n}}}", "diff =({c} {f}) =({c} {f})",
                 "() {{ echo $1 }} {n}", "integer {n}={v}", "setopt {o}", "unsetopt {o}",
                 "zstyle ':completion:*:{n}:*' menu yes", "repeat {v} {{ echo {n} }}",
                 "noglob cp {f} {d}", "nocorrect {c} {f}", "print -l {f} {d}",
                 "bindkey {k} {w}", "echo ${{(U){n}}}", "echo ${{(j:,:){n}}}",
                 "echo ${{(f)\"$(ls {d})\"}}"]
ENGLISH_ARGS = ['echo the build has finished', 'echo "please delete all my files"',
                'git commit -m "fix the bug in the parser"', 'git commit -am "update the readme"',
                'grep -r "find all files" {d}', 'grep "how do I" {f}', 'printf "what is the time\\n"',
                'notify-send "time to go home"', 'echo "show me all the files" > {f}',
                'git tag -a v1.0 -m "first public release"', "sed -i 's/make dinner/cook food/' {f}",
                'gh pr create --title "add the new login page" --body "see the linked issue"',
                'awk \'/kill the server/ {{print $0}}\' {f}', 'say hello there my friend']

CMDLIKE_VERBS = """find make kill sort cut time watch touch split test join cat head tail mount yes
who which more less clear date read print look link install remove shift wait file sleep rename
diff stat top free write mail tree tee open start stop restart run build help""".split()
CMDLIKE_OBJECTS = """the largest file|dinner for two|all my old photos|the server on port 80|me a
sandwich|the bug in the login page|my notes by date|flies like an arrow|the lights off please|out
why this failed|a list of open tasks|the files I changed yesterday|the logs for errors|me how this
works|everything in this folder|up the disk space|this function in half|the meeting to friday|what
went wrong|a new project for me|the latest news|some time for lunch|the current directory please|all
the python files here|the process using the most memory""".replace("\n", " ").split("|")

OTHER_CODE = """print("hello world")|print(f"{n} items")|import {n}|from {n} import load|def {n}(x): return x + 1
x = {v}|{n} = [1, 2, 3]|for i in range(10): print(i)|if x > {v}: print(x)|class {N}: pass
import numpy as np|df = pd.read_csv("{n}.csv")|lambda x: x * {v}|with open("{n}.txt") as f: data = f.read()
SELECT * FROM {n};|SELECT id, name FROM {n} WHERE id = {v};|INSERT INTO {n} VALUES ({v}, 'a');
UPDATE {n} SET value = {v};|DELETE FROM {n} WHERE id = {v};|CREATE TABLE {n} (id INT);|DROP TABLE {n};
select count(*) from {n}|console.log("{n}");|const {n} = require("{n}");|let {n} = {v};
var {n} = new Map();|function {n}() {{ return {v}; }}|document.getElementById("{n}").click();
const {n} = () => {v};|int main() {{ return 0; }}|#include <stdio.h>|printf("%d\\n", {v});
std::cout << {n} << std::endl;|public static void main(String[] args) {{}}|fmt.Println("{n}")
fn main() {{ println!("{n}"); }}|puts "{n}"|<div class="{n}"></div>|{{"{n}": {v}}}|{n}: {v}
Get-ChildItem -Recurse|Get-Process | Where-Object CPU -gt {v}|Set-Location C:\\Users\\{n}
Write-Host "{n}"|$env:PATH += ";C:\\{n}"|New-Item -ItemType File {n}.txt|dir C:\\Windows\\System32
Remove-Item -Recurse -Force .\\{n}|Install-Module {N}|copy C:\\{n}.txt D:\\|cd C:\\Users\\{n}""".replace(
    "\n", "|").replace(" | ", "\0").split("|")
OTHER_CODE = [t.replace("\0", " | ") for t in OTHER_CODE]
SHORT_ENGLISH = """hello|hi|hey|thanks|thank you|ok|okay|sure|please|sorry|why|what|how|when|where|nope
good morning|good night|goodbye|bye|cool|great|nice|awesome|perfect|wow|hmm|oops|continue|go on|again
stop that|what now|why not|how so|what happened|really|never mind|nevermind|not now|try again|got it
yep|yeah|no thanks|sounds good|looks good|what next|anything else|tell me more|and then|explain|summarize
fix it|do it|undo that|fix this|show me|what is this|who are you|help me|hello there|hi claude""".replace(
    "\n", "|").split("|")
CODE_NAMES = """users orders config items result total session data client cache events accounts
products metrics payload request response handler parser worker queue logger token buffer""".split()
MISSING_ARGS_T = ["cp {f}", "cp -r {d}", "mv {f}", "mv {d}", "chmod 755", "chmod +x", "chmod -R 644",
                  "chown alice", "chown -R www-data", "scp {f}", "scp -r {d}", "ln -s", "ln -sf {f}",
                  "ln", "grep", "grep -r", "grep -i", "grep -rn", "mkdir", "mkdir -p", "rm", "rm -r",
                  "rmdir", "touch", "kill", "kill -9", "tar -xzf", "tar -czf", "tar xvf",
                  "tar -czf {f}.tar.gz", "ssh", "ssh -p 22", "sed -i", "git clone",
                  "git checkout -b", "docker run", "docker exec -it", "pip install", "apt install",
                  "sudo apt install", "curl -o {f}", "wget -O {f}", "unzip", "head -n 5 --",
                  "cut -d,", "xargs -I"]

# Zsh-specific invalid syntax. main() keeps only the lines `zsh -n` rejects: some that
# look broken (`setopt {`, `ls **/(*`) parse fine and only fail at run time.
ZSH_SYNTAX_ERRORS = [
    "setopt {",        # unclosed brace
    "print -P '%F{",   # unclosed format
    "a=(1 2 3",        # unclosed paren
    "for f in *(.; do echo $f; done",   # bad glob + missing )
    "repeat { echo hi }",   # repeat needs count
    "echo ${(U",        # unclosed parameter flag
    "echo $((1 +",     # unclosed arithmetic
    "() { echo $1",    # unclosed function body
    "typeset -A {",    # unclosed
    "ls **/(*",        # unclosed glob paren
]


def read_rows(raw: Path, rng: random.Random) -> tuple[list[tuple[str, str, str]], set[str], list[str]]:
    """Return ((command, prompt, source) rows, tool vocabulary, standalone English lines)."""
    rows: list[tuple[str, str, str]] = []
    cm = (raw / "nl2bash/all.cm").read_text(errors="replace").split("\n")
    nl = (raw / "nl2bash/all.nl").read_text(errors="replace").split("\n")
    rows += [(c.strip(), n.strip(), "nl2bash") for c, n in zip(cm, nl)]
    for r in pq.read_table(raw / "bash6k/train.parquet").to_pylist():
        rows.append((r["completion"].strip(), r["prompt"].strip(), "bash6k"))
    for r in pq.read_table(raw / "cli1m/sample.parquet").to_pylist():
        if r["shell"] != "zsh":    # exp_005 uses bash; exp_006 uses zsh rows
            continue
        m = {x["role"]: x["content"] for x in r["messages"]}
        rows.append((m["assistant"].strip(), m["user"].strip(), "cli1m_zsh"))

    vocab = set(ZSH_WORDS)
    with tarfile.open(raw / "tldr/main.tar.gz") as tf:
        for member in tf.getmembers():
            hit = re.search(r"/pages/(common|linux|osx)/([^/]+)\.md$", member.name)
            if not hit or not member.isfile():
                continue
            page = hit.group(2)
            vocab |= {page, page.split("-")[0]}
            text = tf.extractfile(member).read().decode("utf-8", "replace")
            for desc, cmd in re.findall(r"^- (.+?):?\n\n`(.+)`$", text, re.M):
                cmd = fill_placeholders(cmd, rng)
                word = first_word(cmd)
                if "{{" in cmd or "<" in word or word not in (page, page.split("-")[0]):
                    continue
                rows.append((cmd, desc.strip(), "tldr"))

    english = []
    for line in open(raw / "dolly/dolly.jsonl"):
        s = json.loads(line)["instruction"].strip()
        if usable_line(s) and len(s) <= 200:
            english.append(s)
    return rows, vocab, english


def fill_placeholders(cmd: str, rng: random.Random) -> str:
    def sub(m: re.Match) -> str:
        body = m.group(1).split(" ...")[0].split("...")[0].strip()
        if body.startswith("[") and body.endswith("]") and "|" in body:
            return rng.choice(body[1:-1].split("|"))
        if "|" in body:
            return rng.choice(body.split("|"))
        low = body.lower()
        if "path/to" in low:
            return rng.choice(DIRS) if ("dir" in low or "folder" in low) else rng.choice(FILES)
        if "file" in low:
            return rng.choice(FILES)
        if "dir" in low or "folder" in low:
            return rng.choice(DIRS)
        for key, value in PLACEHOLDERS:
            if key in low:
                return value
        return body
    return re.sub(r"\{\{(.+?)\}\}", sub, cmd)


def typo(word: str, rng: random.Random) -> str:
    chars, i = list(word), rng.randrange(max(1, len(word) - 1))
    kind = rng.randrange(4)
    letter = rng.choice("abcdefghijklmnopqrstuvwxyz")
    if kind == 0 and len(word) > 1:
        chars[i], chars[i + 1] = chars[i + 1], chars[i]
    elif kind == 1 and len(word) > 2:
        del chars[i]
    elif kind == 2:
        chars.insert(i, letter)
    else:
        chars[i] = letter
    return "".join(chars)


def mutate_syntax(cmd: str, rng: random.Random) -> str | None:
    edits = [
        lambda c: c + " |", lambda c: c + " &&", lambda c: c + " ||", lambda c: c + " >",
        lambda c: "| " + c, lambda c: c + " )", lambda c: "( " + c, lambda c: c + ' "',
        lambda c: c + " '", lambda c: c + " ;;",
        lambda c: re.sub(r"\s*;?\s*(done|fi|esac|end)\s*$", "", c),
        lambda c: c.replace(" | ", " | | ", 1), lambda c: c.replace('"', "", 1),
        lambda c: c.replace("'", "", 1), lambda c: c.replace(")", "", 1),
    ]
    rng.shuffle(edits)
    for edit in edits[:5]:
        out = edit(cmd)
        if out != cmd and not zsh_parses(out):
            return out
    return None


def mutate_typo(cmd: str, tool: str, vocab: set[str], rng: random.Random) -> str | None:
    if not re.fullmatch(r"[a-z][a-z0-9_+.-]+", tool) or not cmd.startswith(tool):
        return None
    for _ in range(5):
        wrong = typo(tool, rng)
        if wrong not in vocab and wrong != tool:
            return wrong + cmd[len(tool):]
    return None


def mutate_subcommand(cmd: str, rng: random.Random) -> str | None:
    tokens = cmd.split(" ")
    known = SUBCOMMANDS.get(tokens[0], "").split()
    if len(tokens) < 2 or tokens[1] not in known:
        return None
    for _ in range(5):
        wrong = typo(tokens[1], rng)
        if wrong not in known and re.fullmatch(r"[a-z-]+", wrong):
            return " ".join([tokens[0], wrong, *tokens[2:]])
    return None


def tool_long_flags() -> dict[str, set[str]]:
    flags = {}
    for tool in FLAG_TOOLS:
        try:
            r = subprocess.run([tool, "--help"], capture_output=True, text=True, timeout=5,
                               env={"PATH": "/usr/bin:/bin", "LC_ALL": "C"})
        except (OSError, subprocess.TimeoutExpired):
            continue
        found = set(re.findall(r"--[a-z0-9][a-z0-9-]+", r.stdout + r.stderr))
        if len(found) >= 2:
            flags[tool] = found
    return flags


def mutate_flag(cmd: str, flags: dict[str, set[str]], rng: random.Random) -> str | None:
    tokens = cmd.split(" ")
    known = flags.get(tokens[0])
    if not known or METACHARS.search(cmd):
        return None
    for _ in range(5):
        wrong = "--" + typo(rng.choice(sorted(known))[2:], rng)
        if re.fullmatch(r"--[a-z0-9-]+", wrong) and not any(f.startswith(wrong) for f in known):
            return " ".join([tokens[0], wrong, *tokens[1:]])
    return None


def mutate_missing_args(cmd: str) -> str | None:
    if METACHARS.search(cmd):
        return None
    if re.fullmatch(r"tar -[a-zA-Z]*f \S+", cmd):
        return cmd.rsplit(" ", 1)[0]
    try:
        tokens = shlex.split(cmd)
    except ValueError:
        return None
    if not tokens or tokens[0] not in MIN_OPERANDS:
        return None
    opts = [t for t in tokens[1:] if t.startswith("-")]
    operands = [t for t in tokens[1:] if not t.startswith("-")]
    if set(opts) & {"-e", "-f", "-t", "-l", "-L", "--help", "--version", "--target-directory"}:
        return None
    if any(o.startswith("--") and "=" in o for o in opts):
        return None
    need = MIN_OPERANDS[tokens[0]]
    if len(operands) < need or (tokens[0] == "rm" and any("f" in o for o in opts)):
        return None
    return " ".join([tokens[0], *opts, *operands[:need - 1]])


def with_english_args(prompt: str, rng: random.Random) -> str | None:
    text = prompt.rstrip(".?!:").strip()
    if not re.fullmatch(r"[A-Za-z0-9 ,/_.-]{8,100}", text):
        return None
    forms = ['echo {t}', 'echo "{t}"', 'git commit -m "{t}"', 'git commit -am "{t}"',
             'grep -r "{t}" .', 'grep -i "{t}" notes.txt', 'printf "%s\\n" "{t}"',
             'notify-send "{t}"', 'git tag -a v1 -m "{t}"', 'echo "{t}" >> todo.txt',
             'gh issue create --title "{t}"', 'logger "{t}"',
             "print -P '%F{{green}}{t}%f'"]
    return rng.choice(forms).format(t=text, e=text.replace("/", " "))


def casual(text: str, rng: random.Random) -> str:
    if rng.random() < 0.5:
        text = text[0].lower() + text[1:]
    if rng.random() < 0.5:
        text = text.rstrip(".?!:")
    return text


def split_of(group: str, prefix: str = "e6:") -> str:
    h = int(hashlib.sha1((prefix + group).encode()).hexdigest(), 16) % 100
    return "train" if h < 80 else "val" if h < 90 else "test"


def load_e5_texts(out_dir: Path, e5_prefix: str) -> set[str]:
    """Return every text in the exp_005 processed files: all splits including the critical
    gate sets, both the current files and the v1 snapshot (exp_005 v1 trained on a
    different train file than v2)."""
    texts: set[str] = set()
    paths = sorted(out_dir.glob(f"{e5_prefix}_*.jsonl")) + sorted(out_dir.glob(f"*/{e5_prefix}_*.jsonl"))
    for p in paths:
        for line in p.open():
            texts.add(json.loads(line)["text"])
    print(f"exp_005 ban sources: {[str(p.relative_to(out_dir)) for p in paths]}", flush=True)
    return texts


def main():
    cfg = yaml.safe_load(open(EXP_DIR / "config.yaml"))["data"]
    raw = REPO_ROOT / cfg["raw_dir"]
    out_dir = REPO_ROOT / cfg["out_dir"]
    rng = random.Random(0)
    group_prefix = cfg.get("group_prefix", "e6:")

    # Ban all texts that appeared in exp_005 to ensure non-overlapping datasets.
    e5_texts = load_e5_texts(out_dir, cfg["e5_prefix"])
    print(f"exp_005 banned texts: {len(e5_texts)}", flush=True)

    rows, vocab, dolly = read_rows(raw, rng)
    print(f"raw rows {len(rows)}: {dict(collections.Counter(r[2] for r in rows))}", flush=True)

    # ---- valid commands ----------------------------------------------------------
    candidates = {}
    for cmd, prompt, source in rows:
        if usable_line(cmd) and len(cmd) <= 300 and cmd not in candidates:
            candidates[cmd] = (prompt, source)
    with ThreadPoolExecutor(8) as ex:
        ok = list(ex.map(zsh_parses, candidates))
    clean = {c: v for (c, v), good in zip(candidates.items(), ok) if good and first_word(c)}
    print(f"unique commands {len(candidates)}, parse OK under zsh {len(clean)}", flush=True)
    vocab |= {first_word(c) for c in clean if re.fullmatch(r"[a-z][a-z0-9_+.-]+", first_word(c))}

    tool_counts = collections.Counter(first_word(c) for c in clean)
    common = {t for t, _ in tool_counts.most_common(cfg["common_tools"])}
    print(f"top tools {tool_counts.most_common(10)}", flush=True)

    per_tool, commands = collections.Counter(), list(clean.items())
    rng.shuffle(commands)
    examples: dict[str, list[dict]] = {"train": [], "val": [], "test": []}
    negatives: dict[str, dict[str, list[dict]]] = {s: collections.defaultdict(list) for s in examples}
    flags = tool_long_flags()
    print(f"tools with --help flags: {sorted(flags)}", flush=True)

    def add_negative(text, category, source, group, seen):
        if text and usable_line(text) and len(text) <= 320:
            negatives[split_of(group, group_prefix)][category].append(make_example(
                text, False, category=category, source=source, tool_seen=seen))

    def classify_nl(text):
        word = text.split()[0].lower() if text.split() else ""
        return "nl_cmdlike" if word in vocab and zsh_parses(text) else "nl_plain"

    def add_prompt(text, source, group, seen):
        text = casual(text, rng) if text else text
        if text and 2 <= len(text.split()) and len(text) <= 200:
            add_negative(text, classify_nl(text), source, group, seen)

    for cmd, (prompt, source) in commands:
        tool = first_word(cmd)
        seen = tool in common
        group = f"cmd:{cmd}" if seen else f"tool:{tool}"
        add_prompt(prompt, source, group, seen)
        add_negative(mutate_syntax(cmd, rng), "syntax", source, group, seen)
        add_negative(mutate_typo(cmd, tool, vocab, rng), "typo_cmd", source, group, seen)
        add_negative(mutate_subcommand(cmd, rng), "bad_subcmd", source, group, seen)
        for _ in range(3):
            add_negative(mutate_flag(cmd, flags, rng), "bad_flag", source, group, seen)
        add_negative(mutate_missing_args(cmd), "missing_args", source, group, seen)
        if rng.random() < 0.06 and (hard := with_english_args(prompt, rng)) and zsh_parses(hard):
            examples[split_of(group, group_prefix)].append(make_example(
                hard, True, category="valid_english_args", source=source, tool_seen=seen))
        if per_tool[tool] < cfg["max_per_tool"]:
            per_tool[tool] += 1
            examples[split_of(group, group_prefix)].append(make_example(
                cmd, True, category="valid", source=source, tool_seen=seen))

    # ---- hand-written zsh-specific lists ----------------------------------------
    def fill(template):
        key, widget = rng.choice(ZLE_BINDINGS)
        names = {"f": rng.choice(FILES), "d": rng.choice(DIRS), "h": rng.choice(HOSTS),
                 "n": rng.choice(CODE_NAMES), "N": rng.choice(CODE_NAMES).title(),
                 "v": rng.randrange(1, 100), "o": rng.choice(ZSH_OPTIONS),
                 "p": rng.choice(PARAM_FLAGS), "k": key, "w": widget, "c": rng.choice(REAL_CMDS)}
        return template.format(**names)

    def parsed_only(texts, what):
        """Hand-written positives must at least parse; report any that don't."""
        texts = sorted(texts)
        with ThreadPoolExecutor(8) as ex:
            ok = list(ex.map(zsh_parses, texts))
        bad = [t for t, good in zip(texts, ok) if not good]
        if bad:
            print(f"dropped {len(bad)} {what} lines that fail zsh -n: {bad[:10]}", flush=True)
        return [t for t, good in zip(texts, ok) if good]

    interactive = {*INTERACTIVE, *(fill(t) for t in INTERACTIVE_T for _ in range(30))}
    for text in parsed_only(interactive, "valid_interactive"):
        examples[split_of(f"t:{text}", group_prefix)].append(make_example(
            text, True, category="valid_interactive", source="template", tool_seen=True))
    for text in parsed_only({fill(t) for t in ENGLISH_ARGS for _ in range(12)}, "valid_english_args"):
        examples[split_of(f"t:{text}", group_prefix)].append(make_example(
            text, True, category="valid_english_args", source="template", tool_seen=True))
    for text in {fill(t) for t in OTHER_CODE for _ in range(40)}:
        add_negative(text, "other_code", "template", f"t:{text}", True)
    for text in {fill(t) for t in MISSING_ARGS_T for _ in range(6)}:
        add_negative(text, "missing_args", "template", f"t:{text}", True)
    parses_anyway = [t for t in ZSH_SYNTAX_ERRORS if zsh_parses(t)]
    if parses_anyway:
        print(f"ZSH_SYNTAX_ERRORS lines that parse, skipped: {parses_anyway}", flush=True)
    for text in ZSH_SYNTAX_ERRORS:
        if text not in parses_anyway:
            add_negative(text, "syntax", "template", f"t:{text}", True)
    for verb in CMDLIKE_VERBS:
        for obj in rng.sample(CMDLIKE_OBJECTS, 8):
            text = f"{verb} {obj.strip()}"
            add_negative(text, "nl_cmdlike", "template", f"t:{text}", True)
    for text in SHORT_ENGLISH:
        add_negative(text, "nl_plain", "template", f"t:{text}", True)
    for text in dolly:
        add_prompt(text, "dolly", f"t:{text}", True)

    # ---- de-duplicate, then apply the exp_005 ban, then balance -----------------
    hand = [line.rstrip("\n").split("\t") for line in open(REPO_ROOT / cfg["hand_test"])
            if line.strip() and not line.startswith("#")]
    hand_texts = {h[2] for h in hand}

    labels = collections.defaultdict(set)
    for split in examples:
        for r in examples[split]:
            labels[r["text"]].add(0)
        for pool in negatives[split].values():
            for r in pool:
                labels[r["text"]].add(1)
    conflicts = {t for t, seen in labels.items() if len(seen) > 1}
    all_banned = conflicts | hand_texts | e5_texts
    print(f"label conflicts: {len(conflicts)}; "
          f"hand-test banned: {len(hand_texts)}; "
          f"e5 banned: {len(e5_texts)}; total banned: {len(all_banned)}", flush=True)

    out_dir.mkdir(parents=True, exist_ok=True)
    print("negative pools:", {c: sum(len(negatives[s][c]) for s in negatives)
                               for c in cfg["negative_mix"]}, flush=True)

    taken: set[str] = set(all_banned)
    for split in ("test", "val", "train"):
        n_half = cfg[f"n_{split}"] // 2
        final, seen = [], set()
        for r in examples[split]:
            if r["text"] not in taken and r["text"] not in seen:
                seen.add(r["text"])
                final.append(r)
        rng.shuffle(final)
        must = [r for r in final if r["category"] != "valid"]
        rest = [r for r in final if r["category"] == "valid"]
        chosen = (must + rest)[:n_half]
        for category, share in cfg["negative_mix"].items():
            pool, kept = negatives[split][category], []
            rng.shuffle(pool)
            for r in pool:
                if len(kept) >= round(n_half * share):
                    break
                if r["text"] not in taken and r["text"] not in seen:
                    seen.add(r["text"])
                    kept.append(r)
            chosen += kept
        missing = n_half - sum(r["correct_index"] == 1 for r in chosen)
        for r in negatives[split]["nl_plain"] + negatives[split]["syntax"]:
            if missing <= 0:
                break
            if r["text"] not in taken and r["text"] not in seen:
                seen.add(r["text"])
                chosen.append(r)
                missing -= 1
        # The exp_005 ban removes many positives from the shared sources; if fewer than
        # n_half survive, drop negatives at random so the split stays 50/50.
        pos = [r for r in chosen if r["correct_index"] == 0]
        neg = [r for r in chosen if r["correct_index"] == 1]
        if len(pos) < len(neg):
            print(f"WARNING {split}: only {len(pos)} positives for {n_half} wanted; "
                  f"downsampling negatives {len(neg)} -> {len(pos)}", flush=True)
            rng.shuffle(neg)
            for r in neg[len(pos):]:
                seen.discard(r["text"])
            chosen = pos + neg[:len(pos)]
        taken |= seen
        rng.shuffle(chosen)
        with open(out_dir / f"{cfg['prefix']}_{split}.jsonl", "w") as f:
            for r in chosen:
                f.write(json.dumps(r, ensure_ascii=False) + "\n")
        print(f"{split}: {len(chosen)}  labels "
              f"{dict(collections.Counter(r['correct_index'] for r in chosen))}\n"
              f"  categories {dict(collections.Counter(r['category'] for r in chosen))}\n"
              f"  sources {dict(collections.Counter(r['source'] for r in chosen))}\n"
              f"  unseen-tool rows {sum(not r['tool_seen'] for r in chosen)}", flush=True)

    with open(out_dir / f"{cfg['prefix']}_hand.jsonl", "w") as f:
        for label, category, text in hand:
            f.write(json.dumps(make_example(text, label == "1", category=category, source="hand",
                                            tool_seen=True), ensure_ascii=False) + "\n")
    print(f"hand test: {len(hand)}  {dict(collections.Counter(h[0] for h in hand))}", flush=True)
    (out_dir / f"{cfg['prefix']}_vocab.json").write_text(json.dumps(sorted(vocab)))


if __name__ == "__main__":
    main()
