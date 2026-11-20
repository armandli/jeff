---
name: implement
description: Implements the next piece of a project's written implementation plan (a milestone, phase or item from a TODO.md / PLAN.md roadmap such as jfsh/TODO.md) end to end. Finds the plan section, creates a new git branch, has an implementer agent write the code and tests, has a separate reviewer agent check it for bugs, non-optimal solutions, behavior that departs from the plan and format-cpp style violations, loops until the reviewer approves, then commits and pushes the branch to origin. Use when the user runs /implement, optionally followed by phrases naming what to build, e.g. "/implement", "/implement M15 phase 1", "/implement glob qualifiers and KSH_GLOB". Do NOT use for one-off bug fixes, code review of existing work, or work that is not described in a plan document.
argument-hint: "[phrases describing what to implement]"
disable-model-invocation: true
---

# Implement a planned feature

You are the **orchestrator**. You do not write the feature yourself: you
pick the plan section, set up the branch, brief an **implementer** agent,
have a separate **reviewer** agent judge the result, loop until approved,
verify, commit and push.

Arguments (optional): `$ARGUMENTS`

## Rules that always apply

- The user asked for agents by invoking this skill: spawning the
  implementer and reviewer is expected.  Spawn exactly one implementer and
  one reviewer per round; never more.
- Never commit to the default branch.  All work lands on one new branch.
- Do not trust agent reports: before committing, run the verification
  script yourself (Step 6).
- Agents run in the background and you are notified when they finish.
  Wait for the notification; do not poll, and never invent their results.
- Stop and ask the user (AskUserQuestion) only for: a dirty working tree,
  phrases that match several unrelated plan sections, or a review loop
  that has not converged after 3 rounds.

## Step 1: Preflight

Run in parallel:
- `git status --short` and `git branch --show-current`
- `git remote show origin | sed -n 's/.*HEAD branch: //p'` (the default branch)

If the working tree has changes, stop and ask the user whether to commit,
stash or discard them.  Never stash or discard on your own.

## Step 2: Find the plan section

1. Locate the plan.  Look for, in order: the file the user's phrases name;
   `TODO.md`, `PLAN.md`, `ROADMAP.md` in the repo root or a project
   subdirectory (`jfsh/TODO.md` in this repo); `docs/*plan*.md`.  Use
   `git ls-files | grep -iE '(todo|plan|roadmap)[^/]*\.md$'`.
2. Read the plan's structure.  Plans here use `### M<N>. Title` milestone
   headings, `**Phase <N><letter> — done** (branch …)` blocks for finished
   work, and `**Remaining**` / `**Left**` lists for what is still open.
3. Choose the scope:
   - **No arguments**: the first milestone or phase that has no `done`
     marker; if the last finished phase lists leftovers, those come first
     (e.g. after `Phase 13a — done`, the next is 13b).
   - **Arguments**: grep the plan for the phrases (case-insensitive, each
     word) and pick the section with the most hits.  `M15 phase 1` means
     milestone 15's first phase; `glob qualifiers` means the matching
     bullets wherever they are.
   - If the section is too large for one coherent branch (several
     subsystems, like a whole completion system), take the first coherent
     slice, name it as the next phase letter (15a), and say so in the
     final report.  Do not ask.
4. Verify the scope is still open: grep the code for the features and, if
   the plan names a reference implementation (jfsh mirrors zsh: compare
   with `zsh -f`), run a few probes.  Items that already work become
   "add tests only".  Record what you found; it goes in the brief.

## Step 3: Create the branch

```
git checkout <default> && git pull --ff-only origin <default>
git checkout -b <branch>
```

Name it after the project and scope, matching recent branches
(`git branch -r --sort=-committerdate | head`), e.g.
`jfsh-m13b-expansion`, `jfsh-m14-short-forms`.  If the name exists,
append `-2`.

## Step 4: Implement

Spawn the implementer with the Agent tool, `subagent_type: "implementer"`
(defined in `.claude/agents/implementer.md`; if that type is not listed,
use `general-purpose` and paste that file's body at the top of the
prompt).  The prompt is the **brief**:

- the plan file path and the exact section text (quote it);
- the chosen scope, what Step 2.4 found already working, and anything
  explicitly out of scope;
- the branch name, and that it must not commit, push or switch branches;
- the reference behavior to match, if any, and how to run it;
- the build and test commands (`make jfsh`, `make test-jfsh` for jfsh);
- that it must update the plan document to record the work as done in
  the plan's existing style.

Keep the agent's ID: fixes in later rounds go to the same implementer
through SendMessage (load it with ToolSearch `select:SendMessage`), so it
keeps its context.

## Step 5: Review loop (max 3 rounds)

For each round, spawn a **fresh** reviewer with `subagent_type:
"reviewer"` (`.claude/agents/reviewer.md`; fallback as above).  Give it:
the plan file path and section text, the scope, the base ref
(`<default>`), the implementer's report, and in rounds 2+ the previous
findings so it can check each was addressed.  It must not edit files.

The reviewer ends with a verdict block:

```
VERDICT: APPROVE | CHANGES_REQUESTED
FINDINGS:
1. [blocker|major|minor|style] path:line — problem. Evidence: … Fix: …
```

- **CHANGES_REQUESTED** → send the findings verbatim to the implementer
  via SendMessage, wait for its report, start the next round.
- **APPROVE** → go to Step 6.  Minor findings attached to an approval go
  in the final report; do not loop on them.
- If a finding is wrong (the reviewer misread the plan or the reference),
  you may overrule it: tell the next reviewer why, with evidence.
- After round 3 without approval, stop and ask the user whether to
  continue, push as-is with the open findings listed, or abandon.

## Step 6: Verify yourself

```
python3 .claude/skills/implement/scripts/verify.py --base <default>
```

It lists the changed files, runs the mechanical format-cpp checks on the
added lines, and runs the matching test target.  If it reports test
failures or style errors, send them to the implementer and go back to
Step 5 (this counts as a round).  Also read the diff stat
(`git diff --stat <default>`) for anything unexpected: stray files,
build output, edits outside the scope.

## Step 7: Commit and push

1. `git add -A` (after checking `git status` shows only intended files).
2. Commit message: match the repo's recent style (`git log -5
   --format=%B`).  Here that is one long line, `jfsh: implement M14
   (…)`, listing what was added, then the trailer lines the session's
   attribution instructions specify (Co-Authored-By).  Use a HEREDOC.
3. `git push -u origin <branch>`.  Do not open a PR unless asked.

If a pre-commit hook fails, fix the cause, re-stage and make a new
commit; never amend.  If the push is rejected, report it; do not force.

## Step 8: Report

Tell the user: the plan section and scope (and any slice you split off),
the branch and commit, test totals from the verify script, the number of
review rounds and the final verdict, findings fixed along the way, and
minor findings or plan items left open.

## Examples

- `/implement` with `jfsh/TODO.md` showing `Phase 13a — done` →
  scope M13 Phase 13b (the remaining expansion items), branch
  `jfsh-m13b-expansion`.
- `/implement M14` → the M14 section; probes show `test <` and the loop
  short forms already work, so the brief says "tests only" for those.
- `/implement KSH_GLOB and glob qualifiers` → the matching bullets in
  M13's Glob list; branch `jfsh-m13c-glob`.

## Resources

- `.claude/agents/implementer.md`: the implementer's standing instructions
- `.claude/agents/reviewer.md`: the reviewer's checklist and verdict format
- `scripts/verify.py`: changed-file listing, format-cpp mechanical checks,
  test-target detection and run (`--help` for options)
- `.claude/skills/format-cpp/SKILL.md`: the full style rules the reviewer
  applies
