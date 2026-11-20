#!/usr/bin/env python3
"""Verification pass for the /implement skill.

Lists what the branch changed against a base ref, runs the mechanical
format-cpp checks on the C++ lines the change added, and runs the test
target(s) that cover the changed files.  Exits 1 when a test fails or a
style error is found, 0 otherwise.  Style warnings are heuristics for the
reviewer to confirm; they never fail the run.

Usage:
  verify.py [--base main] [--test-cmd 'make test-jfsh' ...] [--no-tests]
            [--no-style]
"""

import argparse
import os
import re
import subprocess
import sys

CPP_EXT = ('.cpp', '.cc', '.cxx', '.h', '.hh', '.hpp', '.hxx')
HEADER_EXT = ('.h', '.hh', '.hpp', '.hxx')
TEST_TIMEOUT = 1800


def git(*args):
    return subprocess.run(['git', *args], capture_output=True, text=True,
                          check=True).stdout


def default_base():
    try:
        ref = git('symbolic-ref', '--short', 'refs/remotes/origin/HEAD').strip()
        return ref.split('/', 1)[1]
    except subprocess.CalledProcessError:
        return 'main'


# ---------------------------------------------------------------------------
# Changed files and added lines.
# ---------------------------------------------------------------------------

def changed_files(merge_base):
    """(status, path) for every file changed since merge_base, committed or
    not, plus untracked files as 'A'."""
    out = []
    for line in git('diff', '--name-status', '--no-renames', merge_base).splitlines():
        status, path = line.split('\t', 1)
        out.append((status[0], path))
    for path in git('ls-files', '--others', '--exclude-standard').splitlines():
        out.append(('A', path))
    return out


def added_lines(merge_base, files):
    """{path: [(line_no, text)]} of lines added since merge_base."""
    added = {}
    diff = git('diff', '-U0', '--no-color', '--no-ext-diff', '--no-renames', merge_base)
    path, line_no = None, 0
    for line in diff.splitlines():
        if line.startswith('+++ '):
            path = None if line[4:] == '/dev/null' else line[6:]
        elif line.startswith('@@'):
            m = re.match(r'@@ -\S+ \+(\d+)', line)
            line_no = int(m.group(1)) if m else 0
        elif line.startswith('+') and path:
            added.setdefault(path, []).append((line_no, line[1:]))
            line_no += 1
    tracked = set(git('ls-files').splitlines())
    for status, p in files:
        if status == 'A' and p not in tracked and os.path.isfile(p):
            with open(p, encoding='utf-8', errors='replace') as f:
                added[p] = [(i, t.rstrip('\n')) for i, t in enumerate(f, 1)]
    return added


# ---------------------------------------------------------------------------
# format-cpp mechanical checks.
# ---------------------------------------------------------------------------

def code_only(text):
    """The line without string/char literals and // comments."""
    text = re.sub(r'"(?:\\.|[^"\\])*"', '""', text)
    text = re.sub(r"'(?:\\.|[^'\\])*'", "''", text)
    cut = text.find('//')
    return text if cut < 0 else text[:cut]


ERROR_RULES = [
    ('A1', 'tab character', lambda raw, code: '\t' in raw),
    ('A1', 'trailing whitespace', lambda raw, code: raw != raw.rstrip()),
    ('C1', '#pragma once (use an #ifndef guard)',
     lambda raw, code: re.match(r'\s*#\s*pragma\s+once', raw)),
    ('D1', 'using namespace at file scope',
     lambda raw, code: re.match(r'using\s+namespace\s+[\w:]+\s*;', code)),
    ('E4', 'class in a template parameter list (use typename)',
     lambda raw, code: re.search(r'template\s*<[^>]*\bclass\b', code)),
    ('E6', 'enum without an explicit underlying type',
     lambda raw, code: re.search(r'\benum\b(\s+(class|struct))?(\s+\w+)?\s*(\{|;|$)', code)
     and not re.search(r'\benum\b[^:{;]*:', code)),
    ('G2', 'quoted #include (use angle brackets)',
     lambda raw, code: re.match(r'\s*#\s*include\s*"', raw)),
]

WARNING_RULES = [
    ('D1', 'using namespace in a local scope',
     lambda raw, code: re.match(r'\s+using\s+namespace\s+[\w:]+\s*;', code)),
    ('E1', 'class keyword (prefer struct)',
     lambda raw, code: re.search(
         r'\bclass\s+\w+',
         re.sub(r'template\s*<[^>]*>|enum\s+class', '', code))),
    ('E7', 'inline keyword: only where linkage needs it',
     lambda raw, code: re.search(r'\binline\b', code)),
    ('F4', '&& / || / ! (use and / or / not)',
     lambda raw, code: re.search(r'\s&&\s|\|\||(?<![!=<>\w])!(?!=)', code)),
    ('A4', 'line over 100 columns', lambda raw, code: len(raw) > 100),
]


def style_check(files, added):
    errors, warnings, long_lines = [], [], 0
    tracked_headers = [p for p in git('ls-files').splitlines() if p.endswith(HEADER_EXT)]
    exts = {}
    for p in tracked_headers:
        e = os.path.splitext(p)[1]
        exts[e] = exts.get(e, 0) + 1
    usual = max(exts, key=exts.get) if exts else '.h'
    for status, p in files:
        if status == 'A' and p.endswith(HEADER_EXT) and not p.endswith(usual):
            errors.append(f'{p}: [headers] new header uses {os.path.splitext(p)[1]}, '
                          f'the project uses {usual}')
    for p, lines in sorted(added.items()):
        if not p.endswith(CPP_EXT):
            continue
        in_block = False
        for no, raw in lines:
            stripped = raw.strip()
            if in_block or stripped.startswith('/*'):
                in_block = '*/' not in stripped
                continue
            if 80 < len(raw) <= 100:
                long_lines += 1
            code = code_only(raw)
            for rule, msg, test in ERROR_RULES:
                if test(raw, code):
                    errors.append(f'{p}:{no}: [{rule}] {msg}: {stripped[:70]}')
            for rule, msg, test in WARNING_RULES:
                if test(raw, code):
                    warnings.append(f'{p}:{no}: [{rule}] {msg}: {stripped[:70]}')
    return errors, warnings, long_lines


# ---------------------------------------------------------------------------
# Tests.
# ---------------------------------------------------------------------------

def detect_test_cmds(files):
    """make test-<dir> for each top-level directory with such a target, or
    make test when some changed code has none."""
    if not os.path.isfile('Makefile'):
        return []
    with open('Makefile', encoding='utf-8') as f:
        targets = set(re.findall(r'^([A-Za-z0-9_.-]+)\s*:', f.read(), re.M))
    code = [p for _, p in files
            if not p.startswith('.claude/') and not p.endswith(('.md', '.txt'))]
    if not code:
        return []
    cmds, uncovered = [], False
    for top in sorted({p.split('/')[0] if '/' in p else '' for p in code}):
        if top and f'test-{top}' in targets:
            cmds.append(f'make test-{top}')
        else:
            uncovered = True
    if uncovered and 'test' in targets:
        return ['make test']
    return cmds


def run_tests(cmds):
    ok = True
    for cmd in cmds:
        print(f'$ {cmd}', flush=True)
        try:
            r = subprocess.run(cmd, shell=True, capture_output=True, text=True,
                               timeout=TEST_TIMEOUT)
        except subprocess.TimeoutExpired:
            print(f'  TIMEOUT after {TEST_TIMEOUT}s')
            ok = False
            continue
        out = r.stdout + r.stderr
        summary = [l for l in out.splitlines()
                   if re.search(r'tests passed|tests failed|\(Failed\)|\(Timeout\)'
                                r'|\berror:|FAILED', l)]
        for l in summary[:40]:
            print(f'  {l.strip()}')
        if r.returncode != 0:
            ok = False
            print(f'  exit status {r.returncode}; last lines:')
            for l in out.splitlines()[-40:]:
                print(f'  | {l}')
        else:
            print('  PASS')
    return ok


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--base', help='base branch or ref (default: origin HEAD branch)')
    ap.add_argument('--test-cmd', action='append',
                    help='test command to run instead of the detected one (repeatable)')
    ap.add_argument('--no-tests', action='store_true', help='skip the tests')
    ap.add_argument('--no-style', action='store_true', help='skip the style checks')
    args = ap.parse_args()

    os.chdir(git('rev-parse', '--show-toplevel').strip())
    base = args.base or default_base()
    try:
        merge_base = git('merge-base', base, 'HEAD').strip()
    except subprocess.CalledProcessError:
        sys.exit(f'verify: unknown base ref {base!r}')

    files = changed_files(merge_base)
    print(f'== Changed files vs {base} ({len(files)})')
    for status, p in files:
        print(f'  {status} {p}')
    if not files:
        print('  (none)')

    failed = False
    if not args.no_style:
        errors, warnings, long_lines = style_check(files, added_lines(merge_base, files))
        print(f'\n== format-cpp mechanical checks: {len(errors)} errors, '
              f'{len(warnings)} warnings')
        for e in errors:
            print(f'  ERROR   {e}')
        for w in warnings:
            print(f'  warning {w}')
        if long_lines:
            print(f'  info    {long_lines} added lines are 81-100 columns (A4 target is 80)')
        failed |= bool(errors)

    if not args.no_tests:
        cmds = args.test_cmd or detect_test_cmds(files)
        print('\n== Tests')
        if not cmds:
            print('  no test command detected (no code changes, or pass --test-cmd)')
        elif not run_tests(cmds):
            failed = True

    print(f'\n== Result: {"FAIL" if failed else "PASS"}')
    sys.exit(1 if failed else 0)


if __name__ == '__main__':
    main()
