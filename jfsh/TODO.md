# jfsh — remaining work

Status as of 2026-11-18: M1 (simple commands), M2 (pipelines, redirections, lists,
background jobs), M3 (word expansion), M4 (compound commands and functions), M5
(arithmetic, `[[ ]]`, `test`), M6 (job control, signals, traps), M7 (builtins), M8
(startup files, invocation, the interactive loop's hooks) and M9 (line editor,
completion, prompt, history) are done, along with the part of M0 they needed.

**Goal: full zsh feature compatibility.** M10–M20 extend the plan to cover every
remaining component of zsh (every module zsh ships, every builtin, every option,
every special parameter, the full compsys completion system, ZLE hooks and
highlighting, emulation modes, PCRE/`BASH_REMATCH`, coprocesses and named
references, zsh's shipped function library, and `.zshrc` compatibility). M20
runs the upstream `zsh/Test` suite plus a hand-curated corpus as the final pass
criterion. Every milestone from M10 on is a cumulative step toward "a `.zshrc`
that works under `zsh -f` works under `jfsh -f`, with the same output".

jfsh builds on macOS and Linux and has gtest coverage for the pieces that work. It runs
**simple commands**, pipelines and background jobs joined by `;`, newlines, `&&`, `||` and
`!`, with redirections, here-documents, `NAME=value` and `NAME=(…)` assignments, and the
full word expansion pipeline (parameters, command and arithmetic substitution, splitting,
braces, tilde, globbing), and compound commands: `if`, `while`/`until`, `for` (all three
forms), `case`, `select`, `repeat`, subshells, brace groups, `always` blocks, `(( ))` and
shell functions with `local`, and conditions: `[[ … ]]`, `test`/`[` and integer and
floating-point arithmetic. Commands that span lines work in scripts, on stdin and at the
prompt (with `PS2`). Interactively it has job control (Ctrl-Z, `fg`, `bg`, `jobs`, job
notices, Ctrl-C that ends the command line but not the shell), and `trap`, `kill` and
`wait` work in every shell. The builtins are real (M7): options (`set`, `setopt`,
`emulate`), sourcing, aliases, the directory stack, `print`/`printf`, history,
`whence` and friends; only the ones that need modules (`zmodload`, `zpty`) say `not
implemented`. It reads zsh-style startup files (`.jfshenv`, `.jfshrc`, …), takes zsh's
command-line options, and runs `precmd`/`preexec`/`periodic`/`zshexit` hooks, `sched`
events and mail checks (M8). The line editor has zsh's emacs and vi keymaps,
`bindkey`, `zle` user widgets and `vared`; Tab completes commands, files, parameters and
users, and `compdef` functions; prompts have zsh's escapes; `!` history expansion works
anywhere in a line, and the history file is zsh's format (M9). The System 1 router and
the LLM agent layer work end to end.

This file has two parts:

1. **[Missing components](#part-1--missing-components)**: what each subsystem must do,
   in the order it has to be built.
2. **[Bugs](#part-2--bugs-in-the-current-implementation)**: defects in code that
   already exists. Items marked **confirmed** were reproduced with a runtime probe (most
   under ASan); the rest come from code reading.

Code-level `TODO(area/topic)` comments in the sources carry implementation notes; this
file names them in brackets so the two stay linked.

---

## Build order

```mermaid
flowchart TD
  M0["M0 Bytecode contract<br/>+ exec_string"] --> M1["M1 Simple commands<br/>words, assignments, PATH exec"]
  M1 --> M2["M2 Pipelines, redirections,<br/>lists, background jobs"]
  M1 --> M3["M3 Expansion<br/>quotes, $var, ~, globs"]
  M2 --> M4["M4 Compound commands<br/>if/while/for/case/functions"]
  M3 --> M4
  M4 --> M5["M5 Arithmetic + [[ ]] + test"]
  M2 --> M6["M6 Job control + signals + traps"]
  M4 --> M7["M7 Builtins (full set)"]
  M5 --> M7
  M7 --> M8["M8 Startup files + REPL hooks"]
  M6 --> M8
  M8 --> M9["M9 Line editor, completion,<br/>prompt, history polish"]
  M9 --> M10["M10 Modules framework<br/>+ core modules"]
  M10 --> M11["M11 Specialized modules<br/>(pcre, curses, net, zpty, ...)"]
  M7 --> M12["M12 Emulation modes<br/>(sh/ksh/zsh, invocation name)"]
  M3 --> M13["M13 Expansion + glob<br/>completeness"]
  M4 --> M14["M14 Short compound forms<br/>+ PCRE =~ + BASH_REMATCH"]
  M10 --> M15["M15 Full compsys<br/>(_arguments, menu-select,<br/>shipped functions)"]
  M9 --> M16["M16 ZLE hooks, $region_highlight,<br/>syntax highlighting, autosuggest"]
  M7 --> M17["M17 Remaining builtins, specials,<br/>history options, PRIVILEGED"]
  M15 --> M18["M18 Default function library<br/>+ .zshrc compatibility"]
  M2 --> M19["M19 Coproc, fd allocation,<br/>named references, time"]
  M11 --> M20["M20 zsh compatibility corpus<br/>+ CI + packaging"]
  M12 --> M20
  M13 --> M20
  M14 --> M20
  M16 --> M20
  M17 --> M20
  M18 --> M20
  M19 --> M20
  M1 --> R["R System 1 router / agent<br/>(independent track)"]
```

Milestones M0–M4 are the minimum for jfsh to be usable as a shell; M0–M9 are the
daily-driver subset already in main. Milestones M10–M20 bring jfsh to full zsh
feature compatibility, culminating in M20's upstream-test corpus. The router/agent
track (R) can proceed in parallel at any time.

---

## Part 1 — Missing components

### M0. Bytecode contract and `exec_string`

**Done** (with M1): the prefix layout for lists, sublists, pipelines, simple commands and
assignments is written down in `include/jfsh/core/wordcode.h`, with skip lengths on
`WC_LIST` and `WC_SUBLIST` so un-taken branches are jumped over; the parser and executor
follow it (bug B1). `StringInputSource`/`FileInputSource` are public in
`include/jfsh/lexer/input.h`. `Parser::parse_string` and `Executor::exec_string` work,
parse errors print `jfsh:LINE: parse error near `X'` with status 1, and leftover tokens
are an error (B9). `disassemble()` (`src/parser/text.cpp`) backs `tests/parser_test.cpp`.

**Remaining**
- Each construct that lands later adds its layout to the comment in `wordcode.h`, its
  own `disassemble()` formatting, and parser tests asserting the exact bytecode, as
  redirections (M2), compound commands (M4) and `[[ ]]` (M5) did.
- The source-text decompiler for `functions`/`which` [text/decompile] is still to do.

### M1. Simple commands

**Done.** Implemented in `src/exec/executor.cpp` (`exec_simple`, `run_external`,
`find_command`), with tests in `tests/exec_test.cpp`:
- Words go through quote removal (the only M3 step so far), then dispatch to a builtin
  or an external command.
- Leading `NAME=value` / `NAME+=value` words (unquoted identifier before the `=`) are
  assignments:
  - with no command they set shell parameters through `ParamTable::set_scalar`, which
    updates the existing parameter wherever it lives, keeps its attributes, refuses
    readonly parameters, and creates new ones as globals;
  - before an external command they go only into that child's environment;
  - before a builtin they last only while it runs. Following zsh rather than POSIX, they
    persist after a **special** builtin only when `POSIX_BUILTINS` is set.
- External commands: the `$PATH` search reads the shell's own `PATH` parameter, and an
  empty component means `.`. Results are cached while `HASH_CMDS` is set, and the cache
  is dropped whenever `PATH` changes. The command runs with `execve` and an environment
  from `ParamTable::make_envp()`. A file with no `#!` line runs under `/bin/sh`. Exit
  statuses: 127 for not found, 126 for not executable, `128+sig` for signalled.
- `$?` (`ExecContext::last_exit`) is updated after every pipeline, and `exit` stops the
  program.
- `xtrace` prints `PS4` (default `+ `) plus the expanded command, quoted where needed,
  **before** running it.

**Left to other milestones**
- `PS4` is printed raw; prompt escapes such as zsh's default `+%N:%i> ` need M9.
- Assigning to an integer or float parameter evaluates the value arithmetically
  (`i+=1` adds) since M5, for `name=value` words; `read`, `for` and `${x=v}` still
  store text into such a parameter.
- Array assignments `a=(x y)` landed with M3; subscripted assignments `a[i]=x` are still
  to do [parser/assign], [exec/assign].

### M2. Pipelines, redirections, lists, background

**Done.** Implemented in the M2 PR:
- Pipelines `a | b | c` and `|&` (`WC_PIPE` now encodes `n` plus a per-connector
  stderr bitmask). All stages fork, dup pipe ends, share one process group, and
  are collected in order; `PIPEFAIL` picks the worst exit instead of the last.
  `! pipeline` still comes out of `WC_SUBLIST_NOT`.
- `&` forks a child that re-enters `exec_sublist`, registers the pid in
  `JobTable` with `JOB_ASYNC|JOB_NOTIFY`, sets `$!`, and prints `[N] pid` on
  stderr in interactive shells.
- Full redirection set (`src/parser/parser.cpp::par_redir`,
  `src/exec/executor.cpp::apply_redirects`): `<`, `>`, `>>`, `>|`, `<>`, `<<`,
  `<<-`, `<<<`, `>&N`, `<&N`, `>&-`, `&>`, `&>>`, plus the fd prefix (`2>`,
  `12<`, `2>&1`). The lexer now bakes digit prefixes into the `TK_REDIR`
  token's text (bug B10 fixed), recognizes `&>>` and `<&`, and slurps balanced
  parens for `<(…)` / `>(…)` / `=(…)`.
- Here-documents: `par_redir` registers the delimiter with
  `Lexer::register_heredoc` and stores the body in the string table as the
  `WC_REDIR + WC_STRING` target (since M3 the lexer reads it at the end of the
  line, see B62).  `<<-` strips leading tabs; quoted delimiters mark the body
  as literal.
- `NOCLOBBER`: `>` on an existing file fails with `EEXIST`; `>|` always
  truncates.
- fds are saved and restored around builtins via `SavedFds` (RAII).
- `MULTIOS`: multiple output redirs targeting the same fd fork an in-shell
  tee helper that reads from a pipe and writes to all targets.
- Process substitution `<(cmd)`, `>(cmd)`, `=(cmd)`: the parser passes
  proc-sub tokens through as arguments; `Executor::expand_procsubst` forks
  subshells, exposes their pipe fds through `/dev/fd/N` (or a tmp path for
  `=(…)`), and releases the resources after the command finishes.
- `EF_NOFORK`: `run_external` honours the flag by `execve`-ing in place.
- SIGCHLD is handled with a self-pipe.  `SignalManager::init` opens
  `pipe2(..., O_CLOEXEC|O_NONBLOCK)` and the handler writes one byte; M6 made
  the pending set atomic and has the executor reap between commands (B22).
- Bug B4 fix: `JobTable::alloc` never returns `id=0` (slot 0 reserved).

**Left to other milestones**
- The builtin `wait` lands as a minimal version (pid list or no args);
  full `%jobid`, `-n`, `-f` wait for M6.
- Expansion inside here-doc bodies and command substitution landed with M3.
- With MULTIOS, zsh concatenates several input redirections to one fd
  (`cat <a <b`); jfsh still lets the last one win.

### M3. Word expansion [exec/expand], [subst/*]

**Done.** The expander is `src/exec/subst.cpp` (`Expander`, documented in
`include/jfsh/exec/subst.h`); `Executor::expand_command` runs it over a simple command's
assignments, words and redirection targets. Tests: `tests/subst_test.cpp`.
- **Lexer (bug B8 fixed):** `$(…)`, `${…}`, `$((…))`, `$[…]`, backquotes and `$'…'` stay
  in one word, nested to any depth and with quotes tracked inside them. `{`/`}` are
  reserved only as words of their own, so `a{b,c}`, `{1..3}` and `{}` are words; `(…)`
  inside a word (`*.(c|h)`, `*(.)`, `name=(…)`) and `<lo-hi>` stay in the word.
- **Here-documents:** the lexer now reads bodies when it reaches the end of the line
  (`Lexer::take_heredocs`), so the rest of the line and the following lines still parse
  (they used to be swallowed, bug B62). Bodies are expanded unless the delimiter was quoted
  (new `literal` bit in `WC_REDIR`).
- **Quotes:** `'…'`, `"…"` (with `\$ \` \" \\ \newline`), `$'…'` C escapes, `\`.
- **Parameters:** `$name`, `${name}`, `$1`…`${10}`, `$#`, `$@`, `$*`, `$argv`, `$?`, `$$`,
  `$!`, `$-`, `$0`, `$PPID`, `$RANDOM`; `$name[sub]` without braces; `$#name`, `$=name`,
  `$~name`. Positional parameters live in `ExecContext::positional`/`arg0`, set from
  `jfsh script args…` and `jfsh -c cmd name args…`.
  - Operators: `- := = ::= ? + ` (with and without `:`), `# ## % %%`, `/ // /# /% :/`,
    `:offset:length`, the modifiers `:h :t :r :e :u :l :q :Q :a :A` (chainable), `${#x}`,
    `${+x}`, `${=x}`/`${==x}`, `${~x}`.
  - Subscripts `[i]`, `[i,j]`, negative indices, `[@]`, `[*]`, scalar character
    ranges, associative-array keys; nested `${${x#a}%b}` and `${(f)"$(cmd)"}`.
  - Flags `U L C j:…: s:…: f F q qq q- Q k v o O i n u @ P t z p`.
  - Unset parameters expand to nothing, or fail under `NO_UNSET`.
- **Command substitution:** `$(…)` and `` `…` `` fork, capture stdout, strip trailing
  newlines and set `$?` (so `x=$(false)` returns 1).
- **Arithmetic:** `$((…))` and `$[…]` through a new integer evaluator
  (`include/jfsh/builtins/math.h`) with zsh's operator precedence, assignments, `++`/`--`,
  `0x`, `0b` and `base#n` literals and recursive evaluation of names. `let` uses it.
- **Splitting:** unquoted arrays become separate words with empty elements removed; command
  substitutions split on `$IFS`; scalars only with `SH_WORD_SPLIT` or `${=x}`; `"$@"` and
  `"${a[@]}"` keep elements apart; empty unquoted words disappear.
- **Brace expansion:** `{a,b}`, nested, `{1..5}`, `{5..1}`, `{01..10..2}`, `{a..e}`,
  `BRACE_CCL` `{abc}`, `IGNORE_BRACES`. Braces from quotes or expansions are literal.
- **Tilde and equals:** `~`, `~/x`, `~user`, `~+`, `~-`; after `=` and `:` in assignments,
  and in arguments under `MAGIC_EQUAL_SUBST`. `=cmd` expands to the command's path (`EQUALS`).
- **Globbing (bug B30 fixed):** see "Glob" below. Characters that came from quotes or
  expansions are literal unless `GLOB_SUBST` or `${~x}`.
- Redirection targets are expanded like words; several results (`> {a,b}`) become several
  redirections that MULTIOS fans out. `<<<` and `>&$fd` targets stay one word.

**Left**
- `${^x}`/`RC_EXPAND_PARAM`, `${x:#pat}`, subscript flags `[(r)pat]`/`[(i)pat]`, padding
  flags `(l:n:)`/`(r:n:)`, `(%)`, `(e)`, `(D)`, `(V)`, `(c)`/`(w)`, the `:s/x/y/` modifier
  and `KSH_ARRAYS` indexing [subst/param].
- A failed `${x?}` should make a non-interactive shell exit; jfsh only fails the
  command [subst/errexit].
- In `${x:-word}`, unquoted pattern characters in `word` should still glob.
- Floating-point arithmetic and `OCTAL_ZEROES` landed with M5.
- `NO_GLOB` (`set -f`) has no option slot yet; `CSH_NULL_GLOB` is not honoured.
- History expansion still happens in the REPL before parsing (`HistoryManager::expand`).

### M4. Compound commands and functions [parser/*], [exec/*]

**Done.** Bytecode layouts are in `include/jfsh/core/wordcode.h`: each compound header
carries a type and a skip length, bodies are nested `list* WC_END` programs, and two
opcodes were added, `WC_CURSH` and `WC_WORDS`. The parser is in `src/parser/parser.cpp`,
the executor in `src/exec/executor.cpp`. Tests: `tests/parser_test.cpp` (exact bytecode)
and `tests/compound_test.cpp` (behaviour, with zsh's output as the expected value).
- `if`/`elif`/`else`, `while`/`until`, `for name… in words`, `for name (words)`,
  `for name` (over `"$@"`), `for ((init; cond; step))`, `case` with `;;`, `;&` and `;|`,
  `select` (menu and `PS3` on stderr, `$REPLY`), `repeat N`, `{ … } always { … }`, and
  `(( expr ))` as a command (status 2 on an arithmetic error). Loop bodies may be
  `do … done`, `{ … }` or a single sublist (`for x (a b) echo $x`, `repeat 3 cmd`).
- Several names per `for` (`for k v in …`). The value list goes through full expansion;
  `case` patterns use `Expander::pattern`, so characters from quotes or `$v` are literal.
- `break N` and `continue N` (bug B18): `ExecContext::break_level` counts the loops left
  to unwind and `continuing` marks a `continue`; `loop_depth` gives the "not in while,
  until, select, or repeat loop" error. As in zsh, `break` in a function leaves the
  caller's loop.
- Subshells `( … )` fork; brace groups `{ … }` run in the current shell (bug B11). `))`
  is now two tokens, so `(a; (b))` closes both; `(( … ))` is read raw by
  `Lexer::read_arith`.
- Redirections after any compound command (`while …; done < f`, `{ … } 2>&1 | x`) are
  moved in front of it in the bytecode and applied around it in-process.
- Functions: `name () body`, `name1 name2 () body`, `function name [()] body`, with a
  `{ … }` or single-command body, and anonymous `() { … } args`. `ExecContext::functions`
  maps names to a `ProgRef` copied out of the defining program, so bodies outlive it.
  Functions are looked up before builtins. Each call scopes `$1…`/`$#`/`$@`, sets `$0`
  under `FUNCTION_ARGZERO`, pushes a parameter scope, and honours `return N`,
  `LOCAL_OPTIONS`, `LOCAL_TRAPS` and `FUNCNEST` (default 700, as in zsh). Assignments in
  front of a function call are exported to what it runs. `return` outside a function ends
  the script or `-c` string, as in zsh.
- `typeset`, `local`, `declare`, `integer`, `float`, `export` and `readonly` are parsed
  as zsh's reserved-word typeset (`WC_TYPESET`), so `local x=$(cmd)` isn't split and
  `local a=(1 2)` makes an array. The builtins handle `-a -A -i -F/-E -g -l -u -r -x` and
  `+` to turn attributes off. A new local is set but empty, and keeps the export attribute
  of the parameter it hides. A builtin with no prefix assignments no longer gets its
  own parameter scope, so `local` lands in the function's scope.
- `read [-rA] [name…]` (needed by `while read` loops): IFS splitting, with the rest of the
  line in the last name, `$REPLY` with no name, backslash handling without `-r`, and
  status 1 at end of input.
- Keywords only act as keywords in command position (bug B7): wherever the parser
  expects words (`for … in` lists, `case` patterns and subjects, `repeat` counts,
  anonymous function arguments), it takes a reserved word as a word.
- Multi-line input (bug B31, and the continuation item of M8): `is_incomplete()`
  (`include/jfsh/parser/parser.h`) reports input cut off inside a construct, quote or
  here-document. stdin mode collects lines until a command is complete, and the prompt
  reads more lines after `PS2` (`Editor::read_line(true)`). The editor now switches raw
  mode with `TCSADRAIN`, so pasted multi-line input isn't discarded.

**Left**
- zsh's other short forms: `if [[ x ]] { … }`, `while cond { … }`, `foreach … end`,
  `for x in a b; cmd` [parser/short].
- Redirections written after a function definition (`f() { … } > log`) should become part
  of the function; they are a parse error.
- `EF_NOFORK` for a subshell that is already a forked pipeline stage [exec/subsh].
- The function call depth is bounded by the C++ stack: a Debug build reaches the default
  `FUNCNEST=700` with about 2× headroom but crashes somewhere between 1500 and 3000; under
  ASan, 700 already overflows.

### M5. Arithmetic, `[[ ]]`, `test`

**Done.** Arithmetic is `src/builtins/math.cpp` (`math_eval`, documented in
`include/jfsh/builtins/math.h`); the conditions shared by `[[ ]]` and `test` are
`src/builtins/cond.cpp` (`include/jfsh/builtins/cond.h`), which also holds the `test`
builtin; `[[ ]]` is parsed by `Parser::par_cond` and run by `Executor::exec_cond`. Tests:
`tests/math_test.cpp`, `tests/cond_test.cpp` (expected values from zsh 5.9), and bytecode
and lexer tests in `tests/parser_test.cpp` and `tests/lexer_test.cpp`.
- **Arithmetic** (`$(( ))`, `(( ))`, `let`, `for (( ))`, `[[ -eq ]]`, subscripts,
  `typeset -i/-F/-E`): values are 64-bit integers or doubles (`MathNum`). Float literals
  (`1.5`, `1.`, `.5`, `1e3`), `Inf`/`NaN`, float parameters, and an integer `**` a
  negative exponent give floats; mixed operands promote; comparisons and bitwise operators
  give integers. `$(( ))` prints floats as zsh does (`10.`, `0.30000000000000004`,
  `1e+20`). Integers wrap on overflow; integer division by zero is an error, float
  division gives `Inf`.
- The evaluator is now precedence climbing over two tables: zsh's default (shifts and
  bitwise operators bind tighter than `**`, `*` and `+`, which the old evaluator had
  backwards) and C's under `C_PRECEDENCES`. All assignment operators, including `^^=`;
  `&&`, `||`, `&&=`, `||=` and `?:` don't evaluate the un-taken side.
- Literals: `0x`, `0b`, `base#digits`, `_` between digits, `0` octal under
  `OCTAL_ZEROES`; `FORCE_FLOAT` makes integer constants floats. `name[i]` reads and
  assigns array elements (negative indexes, growing the array) and `name[key]`
  associative-array elements.
- Assigning to a new name gives it the value's type (`(( n = 5 ))` is an integer,
  `(( g = 2.5 ))` a float); an existing integer or float keeps its type, and a scalar stays
  a scalar. `name=value` and `name+=value` on an integer or float parameter are evaluated
  arithmetically. Float parameters print as `typeset -F` (`2.5000000000`) or, with the new
  `PM_EFLOAT`, as `typeset -E` (`2.500000000e+00`); `Param::scalar()` renders integer and
  float values.
- Errors match zsh's messages (`operator expected at `…'`, `lvalue required`,
  `invalid base`, `math recursion limit exceeded`) and set status 1, or 2 for `(( ))`.
- **`[[ … ]]`:** `[[` is a command word that switches the lexer to `LEX_COND`, where `<`
  and `>` are words and `((` is two `(`. Bytecode is a `WC_COND` tree (`wordcode.h`):
  `!`, `&&`, `||` and `( )` with zsh's precedence, newlines allowed between parts.
  Operands are expanded without splitting or globbing. The right side of `=`, `==`, `!=`
  is a pattern read raw by `Lexer::read_cond_pattern`, so `(a|b)*` stays one operand;
  quoted parts and parameter values are literal unless `${~x}`. `=~` is a POSIX extended
  regex that sets `$MATCH`, `$MBEGIN`, `$MEND`, `$match`, `$mbegin`, `$mend` (character
  offsets, -1 for a group that took no part). `<`/`>` compare strings; `-eq` … `-ge`
  compare arithmetic expressions (`[[ n+1 -gt 2.5 ]]`).
- Unary tests `-a -b -c -d -e -f -g -h -k -n -o -p -r -s -t -u -v -w -x -z -L -O -G -S -N`
  and `-nt -ot -ef` (false unless both files exist). Status 2 and a message for an unknown
  condition, 3 for `-o` with no such option, as in zsh.
- **`test` / `[`** (bug B19 for these two): POSIX's rules for up to four arguments (so
  `test = = =` and `test ! = !` compare strings), then `!`, `-a`, `-o` and `( )` by
  precedence. Integers must be decimal (`integer expression expected`, status 2), and `[`
  needs its `]`.
- `OptionState::find` matches exact names before stripping `no` and reports the negation
  (bug B15), so `[[ -o nomatch ]]` and `[[ -o nonomatch ]]` both work.
- A `!` that isn't a word of its own (`!=`, `!x`) is now part of a word; it used to split
  `test a != b` into `a ! = b`. `${~x}` and `GLOB_SUBST` now make a value active in `case`
  and `[[ = ]]` patterns, as they already did in arguments.

**Left**
- The zsh/mathfunc functions (`sqrt`, `int`, …) [math/mathfunc], which need `zmodload`;
  output bases `$(( [#16] x ))` and `C_BASES` [math/output]; `##x` character codes.
- `REMATCH_PCRE` and `BASH_REMATCH` for `=~`, and module conditions (`-pcre-match`, …).
  zsh calls a lone `[[ -n ]]` an unknown condition; jfsh tests the string `-n`.
- `test` accepts `<` and `>` (POSIX 2024) and `test -o option`, where zsh errors.
- xtrace doesn't print `[[ … ]]` or `(( … ))` commands yet.

### M6. Job control, signals, traps [signals/*], [builtin/bg|fg|jobs|wait|disown|kill]

**Done.** Signals and traps are `src/core/signals.cpp` (`include/jfsh/core/signals.h`),
the job table `src/core/jobs.cpp`, foreground waits and fg/bg `src/exec/jobctl.cpp`
(declared at the end of `include/jfsh/exec/executor.h`), the builtins
`src/builtins/jobctl.cpp`, and the one-line job text `pipeline_text`/`sublist_text` in
`src/parser/text.cpp`. Tests: `tests/jobs_test.cpp` (in-process) and
`tests/interactive_test.cpp`, which drives the `jfsh` binary through a pty; expected
output is from zsh 5.9 (`zsh -f -c`, `zsh -f -i`).
- **Signals:** one handler for every signal the shell catches sets a bit in a lock-free
  atomic pending set and writes a byte to the self-pipe (B22). The executor acts on them
  between commands (`Executor::handle_signals`): it reaps children, runs traps, turns an
  untrapped Ctrl-C into "abort the command line" and SIGHUP into an exit, and follows
  SIGWINCH into `$COLUMNS`/`$LINES`. Dispositions come from the trap table plus job
  control (`SignalManager::install`); signals ignored when the shell started stay ignored.
- **Interactive setup:** the shell takes `/dev/tty`, waits to be in the foreground,
  puts itself in its own process group, takes the terminal and saves its modes. It
  catches SIGINT, SIGHUP and SIGWINCH, catches and drops SIGQUIT, SIGTERM and SIGTSTP
  (caught rather than ignored so a child forked a moment ago still gets them), and
  ignores SIGTTIN and SIGTTOU (B23). Without a terminal job control stays off and
  `fg`/`bg` say `no job control in this shell.`
- **Children:** `Executor::fork_child` blocks every signal across `fork`; the child joins
  the job's process group, takes the terminal if it's in the foreground, resets
  dispositions and traps (`SignalManager::child_reset`: ignored signals stay ignored,
  `( … )` keeps ZERR and DEBUG), forgets the parent's jobs, then unblocks. Command and
  process substitution and here-doc writers reset too. A background job without job
  control ignores SIGINT and SIGQUIT; with it and BG_NICE it runs at nice 5. A lone
  background pipeline and every pipeline stage `exec` their command instead of forking
  again, so `$!` and `kill %1` reach the command itself.
- **Foreground jobs** (only under job control; otherwise the old pid waits): each
  pipeline, external command and `( … )` is a job; `wait_foreground_job` gives it the
  terminal, waits with WUNTRACED and takes the terminal back (B14). A stopped job keeps
  its terminal modes for `fg`, becomes the current job and prints
  `jfsh: suspended  cmd` (status 128 + signal); a stop inside a loop also ends the
  command line. A job killed by Ctrl-C ends the whole command line, as in zsh; one killed
  by another signal prints `jfsh: killed     cmd` (not for SIGPIPE).
- **Job table:** ids start at 1, `%+`/`%-` follow zsh (a job that stops becomes
  current; removing a job promotes the previous one; B41), statuses say `running`,
  `done`, `exit 1`, `terminated`, `suspended (tty input)`, `segmentation fault (core
  dumped)` …, padded as zsh does. `jobs -l` puts each process of a pipeline on its own
  line. Finished background jobs are reported before the prompt and, with NOTIFY (now on
  by default, as BG_NICE is), right away while a line is being typed. Exit statuses of
  finished background processes are kept for `wait $!`.
- **Job specs:** `%N`, `%%`, `%+`, `%`, `%-`, `%string`, `%?string`, with zsh's messages
  (`%3: no such job`, `job not found: x`, `no current job`).
- **Builtins:** `jobs [-lprs]`, `fg`, `bg` (`job already in background`), `wait [pid|job
  …]` (127 and `pid N is not a child of this shell`; a trapped signal ends it with 128 +
  the signal), `disown` (warns about a suspended job), `kill [-s SIG|-n N|-SIG] pid|job`
  (a stopped job also gets SIGCONT) and `kill -l [sig|status …]` (B19 for these).
- **Traps:** `trap`, `trap cmd SIG…`, `trap '' SIG`, `trap - SIG`, `trap SIG…`, `trap -`,
  signal numbers, `0` for EXIT, `--`, `undefined signal: X`; the listing is zsh's
  (`trap -- 'echo hi' INT`, in signal order). Traps run between commands, and right
  after a builtin that raised the signal, with `$?` preserved (B16). `TRAPINT() { … }`
  functions get the signal number as `$1`; one that returns non-zero aborts the command
  line; `trap` and `TRAPxxx` replace each other. EXIT runs when the shell exits (all
  of `-c`, scripts, stdin and the REPL), when a `( … )` subshell that set it exits, and
  at the end of a function that set it. DEBUG runs before each command, ZERR after a
  failing one; neither runs inside another trap. LOCAL_TRAPS is decided when the
  function returns, and only the traps the function changed are put back.
- **ZERR, ERR_EXIT, ERR_RETURN** are the executor's (B33): set off by a failing simple
  command, function call, pipeline, `( … )`, `[[ ]]` or `(( ))` (not by `{ }`, if,
  loops or case, whose failing command already did), except in if/while/until
  conditions, `!`, and all but the last pipeline of a `&&`/`||` chain. ZERR and
  ERR_EXIT act once per failure and are off anywhere under such a context; ERR_RETURN
  minds only the command's own place (so a function called from a condition still
  returns) and acts at every level.
- **Exit:** CHECK_JOBS warns `you have suspended jobs.` (or, with
  CHECK_RUNNING_JOBS, `running jobs`) once; exiting again, or after `jobs`, goes ahead.
  With HUP, jobs left behind get SIGHUP (and SIGCONT if stopped):
  `jfsh: warning: N jobs SIGHUPed`.
- **Line editor:** Ctrl-C drops the line, or at PS2 the whole command (B24); EOF or a
  hung-up terminal ends the read instead of spinning (B29); the editor polls the signal
  self-pipe so traps run and job notices print while it waits, then redraws.

**Left**
- zsh turns the rest of a loop whose command was suspended into a suspended job of its
  own; jfsh ends the command line instead.
- A background pipeline is one process (the forked shell) in the job table, so
  `jobs -l` shows one pid where zsh lists each stage.
- TRAPS_ASYNC, POSIX_TRAPS, POSIX_JOBS, AUTO_CONTINUE and AUTO_RESUME are not
  implemented; neither are `suspend`, `time` (JOB_TIMED) or `$ZSH_SUBSHELL`.
- A non-interactive shell keeps finished background jobs in its table until `jobs` or
  `wait` reports them.
- Job text shows a here-document as `<<EOF` whatever its delimiter.
- The editor doesn't re-layout on SIGWINCH yet: `$COLUMNS`/`$LINES` are updated, but
  the editor doesn't use them (B26, B27).

### M7. Builtins [builtin/*]

**Done.** The builtins are in `src/builtins/` by group: `vars.cpp` (typeset and its
variants, unset), `setopt.cpp` (set, setopt, unsetopt, emulate, shift), `dirs.cpp`,
`print.cpp` (echo, print, printf), `read.cpp`, `eval.cpp` (`.`, source, eval, exit,
return, autoload, functions), `lookup.cpp` (whence, which, type, command -v, builtin,
hash, rehash, alias, unalias), `limits.cpp` (ulimit, umask, times), `hist.cpp` (fc,
history, r), with shared helpers (option parsing, zsh-style quoting) in `util.cpp`. The
ones that call the executor are compiled into `jfsh_exec`. Tests: `tests/builtins_test.cpp`;
expected output is from zsh 5.9 (`zsh -f -c`, `zsh -f -i`).
- **Options:** the option table is regenerated from zsh 5.9 itself: every option zsh
  has (185), with each emulation's defaults, which ones a plain `emulate` resets and
  which are special. Single letters follow zsh (`set -e`, `$-` = `569X…`) or, with
  SH_OPTION_LETTERS, ksh. `set` handles `-o`/`+o` (both listings), `--`, `-`, `-s`,
  `-A`/`+A` and lists parameters; `setopt`/`unsetopt` take names with `no`, `_` and any
  case, `-m` patterns and letters, and list what differs from the defaults (all options
  under KSH_OPTION_PRINT). Invocation options (`interactive`, `shinstdin`, …) can't be
  changed. `emulate [-LR] mode [-c cmd]`. ERR_EXIT, ERR_RETURN, NO_UNSET, XTRACE and the
  rest are now reachable from scripts.
- **Errors abort:** an expansion error (`${x?}`, NO_UNSET, `$((1/0))`) or an assignment
  to a readonly parameter ends the command line, so a non-interactive shell exits, as
  zsh's errflag does.
- **Variables:** `typeset -p`/`-m`/`+`, the bare listings (`typeset -i`, `export`,
  `export -p`, `readonly -p`), `-f`, `-U`, with `-u`/`-l`/`-U` kept in force by later
  assignments; `float` is `typeset -E` as in zsh. `unset [-fmv]`, `name[i]`/`name[key]`,
  locals that stay local when unset, `unset -f TRAPxxx` removing the trap.
- **Sourcing:** `.` searches `$PATH`, `source` the current directory first; arguments
  become the positional parameters meanwhile, `$0` is the file, `return` ends the file,
  127 when it isn't found. Scripts and sourced files are parsed a line at a time
  (`Parser::parse_line`), so aliases defined on one line apply on the next; `-c` and
  `eval` parse the whole text first, as zsh does.
- **Precommand modifiers** are handled by `exec_simple`: `builtin`, `command [-pvV]`
  (bypasses functions and builtins), `exec [-cl] [-a name]` (replaces the shell;
  alone, keeps its redirections), `noglob`, `-`, `nocorrect`.
- **Aliases** (`include/jfsh/core/alias_table.h`): regular, global (`-g`) and suffix
  (`-s`) aliases expanded by the parser in command position (after assignments, and
  after an alias whose text ends in a blank), global ones anywhere; the alias text is
  pushed onto the lexer's input (`LexInput`) and each token remembers the aliases it came
  from, so `alias ls='ls -l'` and `a=b b=a` don't recurse. Reserved words can be aliased.
  `alias` lists (`-L`, `-m`, `-g`, `-s`), `unalias [-ams]`.
- **Directories:** `cd`/`chdir` with `-`, `±N`, `old new`, CDPATH, CDABLE_VARS, `-L`/`-P`
  (logical `$PWD` by default), CHASE_LINKS/CHASE_DOTS, errors as zsh words them, and the
  `chpwd` function; `pushd`/`popd` (`±N`, PUSHD_MINUS, PUSHD_SILENT, PUSHD_TO_HOME,
  PUSHD_IGNORE_DUPS, AUTO_PUSHD), `dirs [-clpv]`, `$dirstack`, `~N`/`~+N`/`~-N`, `pwd
  [-rLP]`. Like zsh, they print the stack or new directory only when interactive (B20).
- **Output:** `echo` interprets escapes unless `-E` or BSD_ECHO; `print` has `-r -R -n
  -l -N -c -C -a -o -O -i -m -u -v -f -x -X -D -P -s`; `printf` has every conversion
  (`%b %q %c %s %d %i %o %u %x %X %e %f %g %a`), flags, `*` widths, `%N$`, arithmetic
  arguments, `'c` character codes, `\c`, reuse of the format and `-v`.
- **read:** `-r -A -k N -q -s -d delim -t secs -u fd -e -E` and `name?prompt`; `$REPLY`
  is trimmed like any other name, as in zsh.
- **Lookup:** `whence [-vcwpafmsS]`, `type`, `which`, `command -v/-V` with zsh's wording,
  `hash [-dfLmrv] [name[=path]]` and `rehash` on the executor's command hash.
- **Functions:** `functions [-mu+]` and `whence -f`/`which` print definitions as zsh does
  (`function_text` in `src/parser/text.cpp`, the multi-line decompiler); `trap` lists
  TRAPxxx functions; `autoload [-U] name|/path/name` loads zsh-style (the file is the
  body) or ksh-style (the file defines the function) from `$fpath` on first call.
- **History:** `fc -l [-nrdfiD]`, `fc -s`/`-e -`/`r` with `old=new`, `fc -e editor`,
  `fc -W/-R/-A`; `history` is `fc -l`. Event numbers, `-N` offsets and prefixes.
- **Limits:** `ulimit [-HSa] -tfdscmunlvxiqerN [value]` with zsh's labels, `umask [-S]`
  octal or symbolic, `times` from `getrusage`.
- **Control:** `exit`/`return` take arithmetic expressions and reject extra arguments.
- **Stubs** that need later milestones print `jfsh: NAME: not implemented` and return 1:
  `bindkey`, `zle`, `vared` (M9), `zpty` (now registered, B21),
  `compinit`; `zmodload` alone lists nothing.

**Left**
- `typeset -T`, `-Z`/`-L`/`-R` padding, `-h`, `-t`; `emulate -l`; `autoload -X`;
  `read -z -c -l -n -p`; `print -z` and `-p`; `fc -p/-P`; `hash -d` (named directories,
  no table yet).
- A command that runs in a forked pipeline stage hashes its path in the child, so
  HASH_CMDS doesn't remember it in the shell.
- zsh errors in special builtins (`set -o nosuch`, `unset readonly`) exit a
  non-interactive shell; jfsh's only return 1.
- Messages name the shell as `jfsh:` and leave out zsh's line numbers and `(eval)`.
- `f() {}` is parsed as a function whose body runs a command `{}`.
- `< <(cmd)` (process substitution as a redirection target) is a parse error.
- zsh's default aliases `run-help` and `which-command` are not defined.

### M8. Startup, REPL loop, invocation [loop/*]

**Done.** Invocation is parsed in `src/exec/invocation.cpp` (`parse_invocation`), the
startup files, hooks, `sched` events and mail checks are in `src/exec/loop.cpp`
(`include/jfsh/exec/loop.h`), the special parameters in `src/exec/context.cpp`, and
`src/main.cpp` strings them together. Tests: `tests/startup_test.cpp` (in-process, plus
the `jfsh` binary run with arguments) and the M8 cases in `tests/interactive_test.cpp`.
- **Invocation:** zsh's rules: letter clusters (`-xe`, `+x`, `-xc cmd`), `-o name`/`+o
  name`/`-oname`, `--name`/`--no-name` for any option, `-c` taking the first argument
  after the options (later ones are `$0` and the positional parameters), `script
  args…`, `-s args…`, `-i`, `-l`/`--login` (or a `$0` starting with `-`), `-f`
  (NO_RCS), `-d` (NO_GLOBAL_RCS), `--` and `-`. `--help`, `--version`. Errors are
  zsh's and exit 1: `bad option: -q`, `no such option: x`, `string expected after
  -c`/`-o`; an unknown `--router` or `--provider` value is an error too (B32). A
  script that can't be opened: `can't open input file: x`, 127.
- **Startup files**, in zsh's order: `SYSCONFDIR/jfshenv` (always), `$ZDOTDIR/.jfshenv`,
  then for a login shell `jfsh_profile`/`.jfsh_profile`, for an interactive one
  `jfshrc`/`.jfshrc`, for a login shell `jfsh_login`/`.jfsh_login`; a login shell reads
  `.jfsh_logout` then `SYSCONFDIR/jfsh_logout` on exit. RCS and GLOBAL_RCS are checked
  before each file, `$ZDOTDIR` (default `$HOME`) is re-read before each, `exit` in a
  file ends the shell. `SYSCONFDIR` is the CMake cache variable `JFSH_SYSCONFDIR`
  (default `/etc`).
  - **Decision on `.zshrc`:** not read by default, since a real `.zshrc` usually needs
    ZLE, `compinit` and modules jfsh doesn't have yet. `--zsh-compat-rc` reads the
    user's `.zshenv`, `.zprofile`, `.zshrc`, `.zlogin` and `.zlogout` right after the
    matching jfsh file (not `/etc/zsh*`).
- **Exit**, as zsh's `zexit`: the history is saved (when `HISTFILE` is set and
  `SAVEHIST` isn't 0), a login shell reads its logout files, then the EXIT trap runs,
  then the `zshexit` hook.
- **The interactive loop**, each iteration: traps and job reaping, job notices,
  `precmd` (and `precmd_functions`), `periodic` when `$PERIOD` seconds have passed,
  due `sched` events, mail (`$mailpath`, `$MAILPATH` or `$MAIL`, every `$MAILCHECK`
  seconds, default 60; `file?message` with `$_` set to the file); the prompt from
  `$PS1` as it is now (B38); the line is added to the history **before** it runs, so
  `$HISTCMD` is its number, `fc`/`history`/`r` leave it out, and `r`/`fc -s` replace it
  with the command they run; `preexec` (and `preexec_functions`) with the line as `$1`,
  `$2` and `$3`; then the line. Hooks keep `$?` and `$pipestatus`. History settings
  (`HISTFILE`, `HISTSIZE`, `SAVEHIST`) are read after the startup files.
- **`sched`** (`src/builtins/sched.cpp`): `sched [-o] [+]hh:mm[:ss]|[+]secs cmd…`,
  `sched` lists in zsh's format, `sched -N` removes.
- **Special parameters** (getters and setters in `ParamHandlers`, which `ParamTable` now
  calls on every lookup and assignment, so expansion, arithmetic, `typeset` and the
  environment all see them): `$SECONDS` (assignable), `$RANDOM` (assigning seeds it),
  `$LINENO` (lines of the script, `-c` string, sourced file or `eval` text; in a
  function, counted from the line its definition starts on), `$HISTCMD`, `$status`,
  `$pipestatus`, `$PPID` (fixed at startup, readonly), `$EPOCHSECONDS`,
  `$EPOCHREALTIME`. `$SHLVL` is incremented and `$PWD`/`$OLDPWD` set and exported at
  startup. Each command's first word now records the line the command starts on (the
  parser emits a command's words after the lexer has read past the end of its line).
- **Scripts** were already run a command at a time (`Parser::parse_line`), so a syntax
  error at the end no longer stops the lines before it from running.

**Left**
- `sched` events only run before a prompt; zsh also runs them while the line editor
  waits for input. `sched -o` is accepted but there is no line to clear.
- Mail: maildir directories and `MAIL_WARNING` aren't handled; the message is expanded
  for parameters but not prompt escapes.
- `preexec`'s `$2` should be a size-limited single-line version of the command (zsh
  elides function bodies); it is the whole line.
- Some zsh specials are still missing: `$TTYIDLE`, `$UID`/`$EUID`/`$GID`/`$EGID`
  (assignable in zsh), `$USERNAME`, `$ZSH_SUBSHELL`, `$funcstack`, `$funcfiletrace`.
  `local SECONDS` doesn't make a local copy of a special; `unset` drops the special
  meaning for good, as in zsh.
- `$LINENO` in a one-line function (`f() { echo $LINENO; }`) is 1; not checked against
  zsh. `$pipestatus` after a function call is that call's status, not the last
  pipeline inside it; also not checked against zsh.
- `PRIVILEGED` doesn't skip the user's startup files.
- An error in a startup file of a non-interactive shell ends the shell, as one in a
  script does; zsh only abandons that file.
- zsh couldn't be run on this machine while M8 was written (it isn't installed, and
  running a downloaded copy was blocked), so the expected outputs come from zsh's
  documentation and source rather than from running it, except `$LINENO` in a function
  and `$SHLVL`.

### M9. Line editor (ZLE), completion, prompt, history

**Done.** Expected behaviour comes from zsh 5.9 (`zsh -f`, `zsh -f -i`), which could be
run on the machine M9 was written on: default keymaps and `bindkey` listings, `print -P`
output, bang expansions and xtrace lines were compared with it.
- **Keymaps and widgets** (`src/editor/keymap.cpp`, `include/jfsh/editor/zle.h`): keys
  are looked up in data-driven keymaps (`emacs`, `viins`, `vicmd`, `visual`, `viopp`,
  `isearch`, `command`, `.safe`; `main` is another name for emacs or viins) holding
  zsh's default bindings, plus Home/End/Delete, Shift-Tab and Ctrl-X Ctrl-E. A
  sequence that is a prefix of a longer binding waits `$KEYTIMEOUT`; unbound escape
  sequences are swallowed, not inserted. `main` is viins when `$VISUAL`/`$EDITOR`
  mentions vi, as in zsh. Keymaps live in the `Zle` object (one per shell, on
  `ExecContext::zle`), so a .jfshrc can set them up before the first prompt.
- **Emacs widgets** (`src/editor/editor.cpp`): movement by character, word (zsh's
  `forward-word`, with `$WORDCHARS`) and line; deleting and killing with a kill ring
  (kills in a row join, `yank`/`yank-pop`); transpose and case; mark and region;
  undo/redo (typing is one step); numeric arguments (`Alt-digits`, `neg-argument`,
  `universal-argument`); `quoted-insert`; overwrite mode; bracketed paste; history
  (`up/down-line-or-history`, `history-search-*`, `history-beginning-search-*`,
  incremental search with `^R`/`^S`/`^G`/backspace, `insert-last-word`,
  `copy-prev-word`, `infer-next-history`); `push-line`/`get-line`, `accept-and-hold`,
  `accept-line-and-down-history`; `expand-history`/`magic-space`;
  `edit-command-line` (`$VISUAL`/`$EDITOR`); `execute-named-cmd`; `run-help` and
  `which-command`; `clear-screen`. Ctrl-D on an empty line is EOF (IGNORE_EOF says
  "use 'exit' to exit").
- **Vi mode** (`src/editor/vi.cpp`): insert/normal/visual (`v`, `V`)/replace (`R`);
  motions `h l w W b B e E ge gE 0 ^ $ | f F t T ; , %`, history `k j + - G / ? n N`;
  operators `d c y gu gU g~ > <` with a motion, doubled for lines, or a text object
  (`iw aw iW aW i" a" i' a' i( a( i[ a[ i{ a{ i< a<`); `x X D C S s Y p P r ~ J o O i a
  I A`; counts on both the operator and the motion (`2d3w`); registers `"a`–`"z`
  (`"A` appends, `"_` discards); marks `m` `` ` `` `'`; `u` and `^R`; `.` repeats the
  last change, an insert included. Leaving insert mode steps back a character, and the
  cursor stays on the line's last character in vicmd.
- **UTF-8** (`src/core/utf8.cpp`, B25): the cursor moves by character (a base
  character and its combining marks are one), widths come from `wcwidth()` with a
  built-in table for characters the locale doesn't know. `main` sets `LC_CTYPE` from
  the environment.
- **Rendering** (`src/editor/refresh.cpp`, B26, B27): the prompt is expanded once per
  line; each refresh draws prompt and buffer again from the prompt's first row,
  breaking rows itself (so it always knows where the cursor is): multi-line prompts and
  buffers, wrapped lines, wide characters, control characters as `^X`, the region in
  standout, `RPROMPT`/`RPS1` on the last prompt row when the line fits
  (TRANSIENT_RPROMPT drops it on accept), messages and completion lists below the
  line. The width is read on every refresh, so SIGWINCH means drawing again.
  PROMPT_CR/PROMPT_SP mark output that didn't end in a newline with
  `$PROMPT_EOL_MARK`, as zsh does. A job notice or trap output while typing clears the
  line first and draws it again below (`Editor::trash`).
- **`bindkey`** (`src/builtins/zle.cpp`): `-e -v -a -M -l -L -N -A -D -d -r -s -R -p`,
  listing in zsh's format (ranges like `" "-"~" self-insert`, `bindkey -L` commands),
  zsh's key notation (`^X`, `\e`, `\M-x`, `\C-x`, `\NNN`, `\xNN`).
- **`zle`**: `-N` (user widgets, which may replace builtin ones; `.name` is always the
  builtin), `-D`, `-A`, `-l [-L] [-a]`, `-M`, `-U`, `-K`, `-R`, `-I`, and `zle widget`
  from inside a widget. A user widget's function sees `$BUFFER`, `$CURSOR`, `$LBUFFER`,
  `$RBUFFER`, `$PREBUFFER`, `$WIDGET`, `$LASTWIDGET`, `$KEYS`, `$KEYMAP`, `$NUMERIC`,
  `$HISTNO`, `$MARK`, `$CUTBUFFER`, `$BUFFERLINES`, `$REGION_ACTIVE` as special
  parameters bound to the editor, so assigning them edits the line at once.
- **`vared [-p prompt] [-r rprompt] [-c] [-a|-A] [-h] [-M keymap] name`**.
- **Completion** (`src/editor/completion.cpp`, B28, B52): the word at the cursor is
  found by a small tokenizer (quotes, backslashes, separators, redirections, `$(`,
  precommand modifiers, assignments). Contexts: `$name` and `${name` → parameters (with
  the closing `}`); `~user` → users; command position → builtins, functions, aliases,
  reserved words and `$PATH` executables (executable only, each name once, sorted,
  cached per directory mtime), or files when the word has a `/`; after `cd`, `pushd`,
  `rmdir` → directories; otherwise files, with `/` after directories, LIST_TYPES
  markers in lists, and quoting (backslashes, or inside an open `'`/`"`, closed on a
  unique match). Tab inserts the longest common prefix; a Tab that adds nothing lists
  (AUTO_LIST, LIST_AMBIGUOUS, LIST_ROWS_FIRST, columns, `$LISTMAX` with "do you wish to
  see all N possibilities"); the next one starts menu completion (AUTO_MENU,
  MENU_COMPLETE, Shift-Tab goes back). COMPLETE_IN_WORD is honoured.
  `compdef fn cmd…` / `compdef -d` hand a command's arguments to a shell function,
  which sees `$words`, `$CURRENT`, `$PREFIX` and adds matches with `compadd [-S suf] [-P
  pre] [-d disp] [-a] [-U] -- words`, or `_files [-/] [-g pat]`, `_directories` and
  `_command_names`. `compinit` is accepted and does nothing.
- **Prompt** (`src/prompt/prompt.cpp`, B34–B37, B61): numeric arguments (`%2~`,
  `%-1/`, `%3c`, `%2m`); `%~` replaces `$HOME` (from the shell's parameters) only at a
  component boundary; `%B %b %U %u %S %s %E`, `%F{…}`/`%K{…}` with names, 0–255 and
  `#rrggbb`, `%f %k` (with zsh's re-emission of colours after `%B`/`%b`); `%? %j %L %h
  %! %i %I %N %x %e %y %l %n %m %M %# %v %d %/ %c %. %C`; `%D{fmt}` (with zsh's `%f %K
  %L`), `%D %T %t %@ %* %w %W`; `%(x.true.false)` with every test zsh has; truncation
  `%N<…<`, `%N>…>`, `%N[…]`, ended by `%<<` or the enclosing group; `%{…%}`; unknown
  escapes expand to nothing. PROMPT_SUBST (parameter, command and arithmetic
  expansion first; quotes kept), PROMPT_BANG and PROMPT_PERCENT. Zero-width parts are
  marked so the editor knows the prompt's width. `print -P`, PS3 and PS4 use the same
  expander; PS4 now defaults to zsh's `+%N:%i> ` (and an assignment's trace ends in a
  space, as zsh's does).
- **History** (`src/history/history.cpp`): bang expansion anywhere in the line: `!!`,
  `!N`, `!-N`, `!str`, `!?str?`, `!#`, `!{…}`, `^old^new^`, word designators (`:0 ^ $ * %
  x-y -y x* x-`, the colon optional before `^ $ * %`), modifiers (`:h :t :r :e :s :gs :&
  :g& :q :x :u :l :a :A :p`), `$histchars`; nothing expands inside single quotes,
  after a backslash, or before a blank, `=` or `(`. The interactive loop prints an
  expanded line before running it; HIST_VERIFY puts it back in the editor; `:p` only
  prints it. Errors are zsh's (`no such event: N`, `event not found: str`, `no such
  word in event`, `substitution failed`, `illegal modifier: z`).
- **History file**: reads and writes zsh's format: EXTENDED_HISTORY lines (`: start:
  elapsed;cmd`), multi-line entries as `\` continuations, and zsh's metafied bytes.
  APPEND_HISTORY appends only entries not yet in the file (B5) and then cuts the file
  back to `$SAVEHIST`; without it the file is rewritten (HIST_SAVE_NO_DUPS honoured).
  `fcntl` locks while reading and writing. INC_APPEND_HISTORY writes each line as it is
  entered, INC_APPEND_HISTORY_TIME after it ran (with its time), SHARE_HISTORY also
  reads what other shells wrote before each prompt. `fc -W` rewrites, `fc -A` appends.
- **History options**, read from the options and `$HISTSIZE` (default 1000),
  `$SAVEHIST` (default `$HISTSIZE`), `$HISTFILE` before each prompt: HIST_IGNORE_SPACE,
  HIST_IGNORE_DUPS, HIST_IGNORE_ALL_DUPS (drops the older copy, B58), HIST_NO_STORE
  (drops `history`/`fc -l` lines, B40), HIST_REDUCE_BLANKS, HIST_FIND_NO_DUPS (in the
  editor's searches), HIST_VERIFY, BANG_HIST.
- Without a terminal (`jfsh -i < file`) the prompt goes to stderr and the line is read
  unbuffered (B54).
- Tests: `tests/editor_test.cpp` drives an `Editor` through a pty in process and feeds
  its output to a small terminal emulator, so the tests check the screen and cursor;
  `tests/prompt_test.cpp`; bang expansion and history-file cases in
  `tests/history_test.cpp`; Tab, `^R`, bang expansion and .jfshrc bindings end to end in
  `tests/interactive_test.cpp`; history file and prompts without a terminal in
  `tests/startup_test.cpp`.

**Left**
- Syntax highlighting and autosuggestions (optional in the plan) aren't done.
- Completion: no `_arguments`/`_describe`-style specs, no option or `--long` completion,
  no reading of zsh's own completion functions, no menu selection (`menuselect`
  keymap, arrow keys in the list), no named directories (`hash -d`) or `cdpath`; a
  compdef function's `compadd` options beyond `-S -P -d -a -U` are accepted and
  ignored. `expand-word`, `list-expand` and `spell-word` only beep, so Tab doesn't
  expand globs or `$vars` in place.
- `zle -F` (fd handlers) and `zle -T` say `not implemented`; `zle-line-init`,
  `zle-line-finish` and `zle-keymap-select` hooks aren't called; `$zle_highlight` and
  `$region_highlight` are ignored; `vared -i/-f` (init/finish widgets) are accepted and
  ignored; vared can't be used inside a widget.
- Vi: no `:` ex commands beyond `execute-named-cmd`; `.` repeats with the original
  count only; `aa`/`ia` (shell words) are treated as blank-delimited words.
- Rendering redraws everything on every key: fine for normal lines, slow for a buffer
  of thousands of characters over a slow link. After SIGWINCH, a terminal that reflows
  the old lines can leave stale rows above a multi-row prompt. `%G` is ignored.
- `sched -o` still has no line to clear (the events don't run while the editor waits).
- History: HIST_EXPIRE_DUPS_FIRST, HIST_LEX_WORDS, HIST_SAVE_BY_COPY (the file is
  rewritten in place, under the lock), HIST_NO_FUNCTIONS and CSH_JUNKIE_HISTORY aren't
  honoured; `fc -p`/`-P` (history stacks) aren't implemented; HIST_IGNORE_SPACE drops
  the line at once instead of keeping it until the next one.
- `%_` (parser state) is always empty, so PS2's default `%_> ` is `> `.

### M10. Modules framework and core modules [modules/*]

Builds the module subsystem that every other zsh module, most of the compsys
completion code (M15), and the shipped function library (M18) depend on. Fixes
bug B53 (`ModuleManager` never instantiated).

**Framework**
- Wire `ModuleManager` into `Shell`/`Executor`; own it on `ExecContext`. Expose
  `$module_path` (default `/usr/lib/zsh/$ZSH_VERSION`, plus jfsh's own module
  dir, configurable via `JFSH_MODULE_DIR` CMake cache). Walk it to resolve
  `zsh/x` → `$module_path/zsh/x.so` / `.bundle` / `.dylib`.
- `zmodload [-abcdeFiIlLmpuUR] [-A|-R alias=name] [name [params…]]`: load,
  unload, list (`-L` in sourceable form), features (`-F`, `-l -F`), aliases
  (`-A`, `-R`), autoload builtins (`-a`, `-ab`, `-ac`, `-ap`), parameters
  (`-p`), conditions (`-c`), math functions (`-f`), unload with `-u`. Match
  zsh's error wording (`failed to load module: x`, `no such feature: y`).
- Module ABI: a C API — `boot_<module>`, `cleanup_<module>`, `features_<module>`,
  `enables_<module>`, `finish_<module>`. Built-in modules link into the binary
  and register through the same table as loaded ones, so a jfsh built without
  `dlopen` support still has every shipped module available. Loaded builtins
  are visible to `whence`, `which`, `builtin`, `hash`, `type`, `command -v`.
- Decision recorded in code: shipped modules are compiled in by default; the
  `dlopen` path is for third-party modules only. A CMake option
  `JFSH_MODULES_STATIC=ON|OFF` picks.
- `autoload` finds module-provided functions through `$fpath` once
  `zsh/parameter` (below) exposes it.

**Core modules** (each compiled in; each also loadable at runtime)
- `zsh/datetime`: `strftime [-rs] fmt [epochtime]`, `$EPOCHSECONDS`,
  `$EPOCHREALTIME`, `$epochtime` array. Wire the existing specials through
  the module's parameter table so `unset` drops them as zsh does.
- `zsh/mathfunc`: `sqrt sin cos tan asin acos atan atan2 exp log log10 pow
  abs int float ceil floor rint logb erf erfc gamma lgamma j0 j1 y0 y1 hypot
  cosh sinh tanh asinh acosh atanh cbrt expm1 log1p` as math-function
  names via a `MathEvaluator::register_function` hook. Also `rand48`.
- `zsh/parameter`: expose shell state as live associative-array / array
  parameters. Each of these must read through to the live table: `$parameters`,
  `$commands`, `$functions`, `$dis_functions`, `$aliases`, `$dis_aliases`,
  `$galiases`, `$dis_galiases`, `$saliases`, `$dis_saliases`, `$builtins`,
  `$dis_builtins`, `$reswords`, `$dis_reswords`, `$patchars`, `$dis_patchars`,
  `$options`, `$modules`, `$widgets`, `$jobdirs`, `$jobstates`, `$jobtexts`,
  `$nameddirs`, `$userdirs`, `$history`, `$historywords`, `$funcstack`,
  `$funcfiletrace`, `$funcsourcetrace`, `$functrace`. Needed by compsys
  (`_arguments`, `_describe` scan these).
- `zsh/zutil`: `zstyle [-eLs delim name param|…]` with pattern contexts;
  `zformat -f var spec args…` and `zformat -a var sep key=val…`;
  `zregexparse [-c] [-d] [-A arr] [-M arr] subject regex`;
  `zparseopts [-D] [-K] [-M] [-E] [-F] [-A arr] [-a arr] spec…` with
  `name+=` / `name:` / `name:-` / `name::` forms; `zcompile [-UMcma]`
  bytecode cache (writes `.zwc`, read by `autoload`); the `is-at-least`
  comparison builtin. Needed by compsys and every non-trivial zsh script.
- `zsh/system`: `syserror [-e var] [-p prefix] errno`, `sysopen -o flags
  -u fd file`, `sysread [-c count] [-i fd] [-o fd] [-s size] [-t secs]
  [name]`, `sysseek [-u fd] [-w start|end|current] offset`, `syswrite
  [-c count] [-o fd] data`, `zsystem flock [-t secs] [-f fd] [-r] file`,
  `zsystem supports feature`, `$errnos` associative array.
- `zsh/files`: builtin `chmod`, `chown`, `chgrp`, `ln`, `mkdir`, `mv`, `rm`,
  `rmdir`, `sync`. Register through the module so they override the externals
  only when enabled, as zsh does.
- `zsh/stat` (alias `zstat`): `zstat [-glLnoNrstT] [+element] [-A arr | -H
  hash] file…` with every stat field; a format string with zsh's placeholders.
- `zsh/pcre`: `pcre_compile [-aimsx] pattern`, `pcre_study`,
  `pcre_match [-v var] [-a arr] [-n offset] string`, `$PCRE_MATCH`,
  `$pcre_match`; `[[ string -pcre-match pattern ]]` condition (wired into
  `[[ ]]` in M14). Uses PCRE2.

**Tests**
- `tests/modules_test.cpp`: `zmodload` listing, loading, unloading, aliases,
  autoload, `-F`, dependencies, and failure wording.
- One test file per core module (`tests/modules/datetime_test.cpp`,
  `mathfunc_test.cpp`, `parameter_test.cpp`, `zutil_test.cpp`, `system_test.cpp`,
  `files_test.cpp`, `stat_test.cpp`, `pcre_test.cpp`) with expected output from
  `zsh -f -c` captured once zsh is installable on the test host (today it is
  not; the sanitizer/clang/g++ test matrix still applies).

### M11. Specialized modules [modules/*]

The modules that round out zsh's feature set but aren't required by the core
runtime. Each lands with its own tests; group ordering is suggested, not
required.

**Networking and IPC**
- `zsh/net/tcp`: `ztcp [-acflLv] [-d fd] [host [port]]`, `ztcp -c fd [count]`;
  `$REPLY` for the accepted fd.
- `zsh/net/socket`: `zsocket -a listenfd` (accept), `zsocket -d fd`,
  `zsocket -l path`, `zsocket path`.
- `zsh/zftp`: `zftp open|cd|ls|dir|get|put|append|getat|putat|appendat|delete
  |mkdir|rmdir|remote|local|type|ascii|binary|mode|rename|quote|site|close
  |test|session|rmsession`.
- `zsh/zselect`: `zselect [-rwe] [-t timeout] fd…` wrapping `pselect`.
- `zsh/socket`: older name for `zsocket`, kept for compat with old scripts.

**Terminal, display, i18n**
- `zsh/termcap`: `echotc capability`, `$termcap` associative array.
- `zsh/terminfo`: `echoti capability`, `$terminfo` associative array.
- `zsh/langinfo`: `$langinfo` associative array (`CODESET`, `YESEXPR`,
  `NOEXPR`, `D_FMT`, `T_FMT`, `D_T_FMT`, `AM_STR`, `PM_STR`, `ABDAY_1`…).
- `zsh/curses`: `zcurses init|end|addwin|delwin|refresh|move|char|string|
  clear|attr|bg|border|scroll|input|mouse|timeout|touch|resize|querychar|
  position`.
- `zsh/nearcolor`: approximate truecolor `%F{#rrggbb}` down to 256 or 16
  colors based on `$TERM`; hooked into `src/prompt/prompt.cpp`.

**Data and regex**
- `zsh/regex`: `[[ string -regex-match pattern ]]`, sets `$MATCH`, `$match`,
  `$MBEGIN`/`$MEND`. Uses POSIX ERE (already on hand through `<regex.h>`).
- `zsh/mapfile`: `$mapfile` associative array whose keys are file paths and
  values the files' contents (read on lookup, write on assignment, delete
  on `unset mapfile[path]`).
- `zsh/db/gdbm`: `ztie -d db/gdbm -f file name`, `zuntie [-u] name`,
  `zgdbmpath name`; associative-array semantics backed by GDBM.
- `zsh/private`: `private [name[=value]…]`: parameter visible only to the
  function that declared it, not to callees. Needs a small change to the
  parameter-scope lookup rules (new flag `PM_PRIVATE`).

**Pty, profiling, attributes, ZLE extras**
- `zsh/zpty`: replace the "not implemented" stub with `zpty [-e] [-b] name
  cmd`, `zpty -d name`, `zpty -r name [param [pattern]]`, `zpty -w name
  string`, `zpty -t name`, `zpty -L`, `$REPLY`. Used by zsh's own tests.
- `zsh/zprof`: `zprof [-c] [-n]` profiler; per-function cumulative and
  self times via a hook on function entry/exit in `ExecContext`.
- `zsh/attr`: `zgetattr`, `zsetattr`, `zdelattr`, `zlistattr` via Linux
  `sys/xattr.h` and BSD `sys/extattr.h`.
- `zsh/cap`: `cap [name…]`, `getcap`, `setcap` on Linux capability-aware
  builds (`sys/capability.h`).
- `zsh/zleparameter`: expose `$widgets` and `$keymaps` to ZLE scripts.
- `zsh/deltochar`: `delete-to-char` and `zap-to-char` ZLE widgets.
- `zsh/compctl`: legacy `compctl`-style completion. Parse the full syntax
  but implement only the subset `oh-my-zsh`/`prezto` legacy plugins still
  reach for, forwarding to compsys internally; mark unsupported combinations
  with `jfsh: compctl: unsupported option`.
- `zsh/example`: the tiny loadable example that exercises the module ABI;
  kept as a build/test target only.
- `zsh/clone`: `clone tty` (re-parents the shell onto a different tty).
- `zsh/watch`: `$watch`, `$WATCHFMT`, the `log` builtin, periodic
  who/last scan driven off the same timer as `$PERIOD` (M8).
- `zsh/newuser`: run the equivalent of `zsh-newuser-install` on first
  interactive login with no rc files (prompt theme picker, basic rc stub).

**Tests**
- One test per module under `tests/modules/`. Network modules use loopback
  on an ephemeral port; `zpty` tests go through `forkpty`; `curses`,
  `terminfo`, `termcap` tests spin up a pty with a stub terminfo entry.

### M12. Emulation modes [core/options], [exec/invocation]

Already in place: zsh's full option table (M7) with each emulation's defaults.

**Remaining**
- `emulate [-LR] sh|ksh|zsh [-c cmd]`: switch the option set, special-parameter
  semantics, keyword rules and reserved words for the duration of the script,
  the function (`-L`) or the `-c` command. On function return under `-L`,
  restore the option set the function saw.
- `emulate -l`: list in a sourceable form the options that differ from the
  emulation's defaults.
- Invocation-name behavior: when `argv[0]` is `sh`, `ksh`, `csh` or starts with
  `-` followed by any of those (login shell), pick the matching emulation at
  startup, read the right rc files (`/etc/profile`, `~/.profile`, `$ENV`, …),
  and skip zsh's.
- Every option below gets its code path wired through, not just the flag:
  `POSIX_BUILTINS`, `POSIX_IDENTIFIERS`, `POSIX_STRINGS`, `POSIX_ALIASES`,
  `POSIX_CD`, `POSIX_JOBS`, `POSIX_TRAPS`, `KSH_ARRAYS` (0-based indexing,
  `$a` means `$a[0]`, `${a[*]}` needed for all elements), `KSH_AUTOLOAD`,
  `KSH_GLOB` (M13), `KSH_OPTION_PRINT`, `KSH_TYPESET`, `KSH_ZERO_SUBSCRIPT`,
  `SH_FILE_EXPANSION`, `SH_GLOB`, `SH_NULLCMD`, `SH_OPTION_LETTERS`,
  `SH_WORD_SPLIT`, `BSD_ECHO`, `BASH_REMATCH` (M14), `TRAPS_ASYNC`,
  `CSH_JUNKIE_HISTORY`/`LOOPS`/`QUOTES`, `CSH_NULL_GLOB`, `CSH_NULLCMD`.
- `sh` and `ksh` entry: set emulation + option set + rc-file paths + prompt
  defaults (`$` for `sh`) up front, before reading the rc files.

**Tests**
- `tests/emulate_test.cpp`: under each emulation, `[[ -o option ]]` reports the
  right defaults; `KSH_ARRAYS` indexing switches base; `SH_WORD_SPLIT` splits
  scalars; `POSIX_BUILTINS` keeps assignments after special builtins;
  `POSIX_TRAPS` resets EXIT between functions; `sh -c` reads `/etc/profile`.

### M13. Expansion and glob completeness [exec/expand], [glob/*]

**Expansion**
- `${^x}` / `RC_EXPAND_PARAM`: array in a word becomes "prefix $x_i suffix"
  for each element; `${^^x}` also splits on `$IFS`.
- `${x:#pat}` and `${(M)x:#pat}`: filter array elements by pattern (match /
  non-match).
- Subscript flags `[(r)pat]`, `[(R)pat]`, `[(i)pat]`, `[(I)pat]`, `[(e)]`,
  `[(n)]` (numeric order), `[(b)n]`.
- Padding flags `(l:n::s::f:)`, `(r:n::s::f:)`.
- `(%)` prompt-escape expansion; `(e)` evaluate as a shell expression;
  `(D)` abbreviate home-relative paths; `(V)` make non-printables visible;
  `(c)`/`(w)` count characters / words; `(#)` arithmetic evaluate each
  element; `:s/x/y/`, `:s|x|y|`, `:gs/x/y/`, `:&` (repeat), `:G` (all),
  `:x` (quote-remove).
- A failed `${x?}` in a non-interactive shell exits the shell (subst/errexit).
- In `${x:-word}`, unquoted pattern characters in `word` glob (subst/glob).
- `KSH_ARRAYS` indexing (tied to M12).

**Glob**
- Globbing flags `(#i)` (case-insensitive subpattern), `(#l)` (letter-case-
  insensitive), `(#I)` (case-sensitive scope), `(#b)` (named backreferences
  → `$match`), `(#B)` (no backref), `(#m)` (sets `$MATCH`), `(#M)` (no
  `$MATCH`), `(#a1)` / `(#a2)` approximate matching (Damerau–Levenshtein,
  bounded).
- `KSH_GLOB`: `?(…)`, `*(…)`, `+(…)`, `@(…)`, `!(…)`.
- Qualifier modifiers: add `(e:expr:)` (file passes a shell expression),
  `(+function)` (file passes a function test), size `(L[+-]n[kmp])`, age
  `(m[Mwhms][+-]n)`, link count `(l[+-]n)`, owner `(u:name:)`, group
  `(g:group:)`, `(U)` user-owned, `(G)` group-owned, `(f:perm:)` permission
  mask, `(d:dev:)` device, `(k:nlinks:)`, `(:modifier:)` (apply history
  modifier to each result).
- `CSH_NULL_GLOB`: unmatched patterns empty only if **every** pattern on the
  command line is unmatched.
- `NO_GLOB` option slot (`set -f`).

**Tests**
- Extend `tests/subst_test.cpp` and `tests/glob_test.cpp`; add
  `tests/glob_quals_test.cpp` with every qualifier under temp-file setups
  checked against `zsh -f -c`.

### M14. Compound short forms, PCRE `=~`, `BASH_REMATCH` [parser/short], [builtins/cond]

**Short compound forms**
- `if [[ … ]] { … }` / `if cond { … }` with `{ } else { } elif cond { }`.
- `while cond { … }`, `until cond { … }`, `for name (words) { … }`,
  `for ((;;)) { … }`, `select name (words) { … }`, `repeat N { … }`.
- `foreach name (words); body; end`.
- `for x in a b; cmd` (no `do … done`; single command after `;`).
- Redirections on a function definition (`f() { … } > log`) become part of
  the function body (M4 left item).
- `f() {}` parses as a function with an empty body (today it parses as "run
  the command `{}`"; M7 left item).
- `< <(cmd)` (process substitution as a redirection target) parses (M7 left
  item).
- `EF_NOFORK` for a `( … )` subshell that is already a forked pipeline stage
  (M4 left item).

**Regex**
- `[[ s =~ p ]]` under `REMATCH_PCRE`: compile `p` with PCRE2 (through
  `zsh/pcre` from M10); fall back to POSIX ERE without the option.
- `BASH_REMATCH` option: write `$BASH_REMATCH` array with full-match at
  index 0 (for ksh/bash emulations).
- `[[ -pcre-match ]]` and `[[ -regex-match ]]` conditions wired in from
  M10/M11.
- `[[ -n ]]` alone becomes an unknown condition (zsh's wording), not a
  string test (M5 left item).
- `test` / `[` accepts `<` and `>` (POSIX 2024) and `test -o option`
  (M5 left item).
- xtrace prints `[[ … ]]` and `(( … ))` commands (M5 left item).

**Tests**
- `tests/compound_test.cpp`: every short form under `zsh -f -c`.
- `tests/cond_test.cpp`: PCRE under feature flag; `BASH_REMATCH`.

### M15. Full completion system (compsys) [completion/*]

Replaces the hand-rolled context code (M9) with the full compsys framework.

**Core framework** (`src/completion/compsys.cpp`, headers under
`include/jfsh/completion/`)
- `_main_complete` dispatcher: propose a set of `_tag`s, then fill them with
  `_complete` / `_approximate` / `_correct` / `_expand` / `_history` /
  `_oldlist` / `_prefix` / `_match` / `_ignored` / `_menu` / `_user_expand`.
- `compstate` associative array: `context`, `nmatches`, `unambiguous`,
  `exact_string`, `list`, `insert`, `old_insert`, `old_list`, `last_prompt`,
  `pattern_insert`, `pattern_match`, `ignored`, `restore`, `to_end`,
  `vared`, `redirect`, `parameter`.
- Live editable `$words`, `$CURRENT`, `$PREFIX`, `$SUFFIX`, `$IPREFIX`,
  `$ISUFFIX`, `$QIPREFIX`, `$QISUFFIX`.
- `compadd` with every option (`-S`, `-s`, `-r`, `-R func`, `-q`, `-Q`,
  `-X`, `-x`, `-J`, `-V`, `-O`, `-M matcher`, `-W`, `-F ignore`, `-n`,
  `-U`, `-l`, `-o`, `-E`, `-k`, `-K`, `-a`, `-d`, `-i`, `-p`, `-P`, `-f`,
  `-A`).
- `compdescribe`, `compfiles`, `compgroups`, `compquote`, `comptags`,
  `comptry`, `compvalues`.
- `_arguments [-s] [-A pattern] [-C] [-R] [-O arr] [-M spec] [-w] [-W]
  specs…`: option specs (`-opt[desc]`, `--long=VALUE[desc]:action`,
  `*::rest::action`), action syntax (`(a b c)`, `(( a\:desc b\:desc ))`,
  `->state`, `->state=cond`, `{compl-code}`, `*`, `(-)`).
- `_describe`, `_values`, `_wanted`, `_requested`, `_alternative`,
  `_message`, `_tags`, `_prefix`, `_suffix`, `_complete`, `_default`,
  `_normal`, `_files`, `_directories`, `_hosts`, `_users`, `_groups`,
  `_commands`, `_parameters`, `_options`, `_cd`, `_path_files`.
- `_approximate` / `_correct` using M13's `(#a1)`.

**Menu selection** (`zsh/complist`)
- `menu-select` ZLE widget: a key at the list opens a selection navigator;
  arrow keys and vi keys move, Enter inserts; completion is replaced as the
  highlight moves.
- `menuselect` keymap with zsh's defaults (accept, cancel, accept-and-infer-
  next-history, down/up-line-or-history, left/right, forward/backward-word,
  undo, history-incremental-search-forward).
- `$MENUSELECT`, `$LIST_PACKED`, `$LIST_ROWS_FIRST`.
- `LIST_COLORS` and `list-colors` zstyle for colored listings.

**Named directories and cdpath in completion**
- `hash -d name=path` (M17 ties this into the builtin), `~name` and `%~`
  display, `_cdpath` walking `$cdpath`.

**Shipped function library** (`share/jfsh/functions/Completion/`)
- Install zsh's shipped completion functions under `JFSH_FUNCTIONS_DIR`
  (CMake cache, default `share/jfsh/functions`), autoload them on
  `compinit`. Start with Base (`_main_complete`, `_arguments`,
  `_description`, `_tags`, `_message`, `_alternative`, …), Unix
  (`_cd`, `_ls`, `_rm`, `_mv`, `_cp`, `_ssh`, `_scp`, `_rsync`, `_grep`,
  `_find`, `_tar`, `_make`, `_sudo`, `_git`), and iteratively ship the
  rest; publish `completion-compat.md` listing what is supported.
- `compinit [-D] [-u] [-U] [-d dump] [-i] [-C]`: build `.zcompdump` of
  autoload definitions with zsh's security check (fpath directory
  ownership, 022 umask, insecure directory warning).

**Tests**
- `tests/completion_test.cpp`: `_arguments` dispatch, `_describe` display,
  `compstate` feedback, menu selection in a pty.
- The M20 compat corpus runs the function-library tests zsh ships.

### M16. ZLE polish and highlighting [editor/*]

**Hooks and parameters**
- User-widget hooks: `zle-line-init`, `zle-line-finish`, `zle-keymap-select`,
  `zle-isearch-exit`, `zle-isearch-update`, `zle-history-line-set`,
  `zle-finish`. Each gets the right environment (`$KEYMAP`, `$OLDKEYMAP`,
  `$BUFFER`, …) on entry.
- `$zle_highlight` array (default `(isearch region special suffix paste)`
  plus `default:none`); `$region_highlight` array of entries
  `start end spec [memo:tag]` with spec `fg=…,bg=…,bold,standout,underline,
  blink,reverse`. Redraw groups per entry; keep per-key state.
- `zle -F fd [handler]`: install fd handlers invoked between keystrokes
  (needed by `zftp` async and by async completion).
- `zle -T tc widget`: tie a terminal cap to a widget.

**Widgets and keymaps**
- `menuselect` keymap (from M15) and `expand-word`, `list-expand`,
  `spell-word` fully implemented (M9 only beeps).
- Vi `:` ex commands: `:w`, `:q`, `:wq`, `:q!`, `:s/.../.../`, `:set opt`,
  `:map`.
- Vi `.` repeat with its own count (not just the recorded one); `aa`/`ia`
  shell-word text objects using the completion-side tokenizer.

**Rendering**
- SIGWINCH redraw of multi-row prompts fully reflowed, not just updated
  (M9 left item).
- Dirty-line tracking in `src/editor/refresh.cpp`: redraw only the differences
  for very long buffers, so a 50 k-character line stays usable over a slow
  link (M9 left item).
- `%G` prompt escape honoured.
- `sched -o` clears the editor line and runs events while the editor is
  waiting for input (M8/M9 left item).

**Syntax highlighting and autosuggestions**
- Optional, built-in: tokenize the buffer as the user types, drive colors
  through `$region_highlight` (so themeable via `.zshrc`). Fast path:
  incremental re-tokenization on insert/delete around the cursor.
- Autosuggestions (a la `zsh-autosuggestions`): dim-text suggestion after
  the cursor drawn from history or completion; accept with `forward-char`
  / End; configurable through `$ZSH_AUTOSUGGEST_STRATEGY`.

**`vared` extras**
- `vared -i init-widget -f finish-widget`.
- `vared` inside a ZLE widget: push/pop the editor state.

**Tests**
- `tests/editor_test.cpp`: hooks fire with the right parameters;
  `$region_highlight` colors a region; syntax highlight stays correct across
  edits; autosuggest doesn't leak past Enter.

### M17. Remaining builtins, specials, history options, PRIVILEGED

**Builtins**
- `typeset -T NAME SCALAR[=val] [sep]`: tied scalar/array pair
  (`PATH`/`path`), separator, two-way updates.
- `typeset -Z N`, `-L N`, `-R N` padding; `-h` hidden from listing;
  `-t` tagged (per-param tracing).
- `typeset -n ref=name`: nameref semantics (M19 implements the lookup; this
  milestone wires the flag through the typeset builtin).
- `emulate -l` listing (M12 adds the full behavior; this is the output
  format).
- `autoload -X` (mark as "file is a function body, parse as body on first
  call"; current `-U` is already there).
- `read -z` (one `NUL`-terminated record), `-c`, `-l`, `-n`, `-p`
  (co-process input from M19).
- `print -z` (push to the editor buffer), `-p` (co-process output).
- `fc -p [file [histsize [savehist]]]`, `fc -P`: history stack.
- `hash -d name=path`: named directories. Builtin lists `-d` entries in
  zsh's format; fills `$nameddirs`.
- `suspend [-f]`.
- `time pipeline` and `time` with no pipeline; `$TIMEFMT`; `JOB_TIMED` flag
  on jobs; `$REPORTTIME` threshold.
- Special-builtin errors (`set -o nosuch`, `unset readonly`) exit a
  non-interactive shell under `POSIX_BUILTINS` (M7 left item).
- A forked pipeline stage's `HASH_CMDS` hit flows back to the parent through
  the self-pipe, so a path hashed inside a `|` is visible after (M7 left
  item).
- zsh's default aliases (`run-help`, `which-command`) and functions added by
  `zsh -f` are defined (M7 left item; the function bodies are M18's problem).
- Error messages name the shell as `zsh:` under emulation; add zsh's line
  numbers and `(eval)` where it uses them (M7 left item).

**Specials**
- `$TTYIDLE` (seconds since last keystroke).
- `$UID`, `$EUID`, `$GID`, `$EGID`: readable; assignable through
  setuid/setgid.
- `$USERNAME`: readable; assignable through setuid.
- `$ZSH_SUBSHELL` auto-incremented on fork.
- `$funcstack`, `$funcfiletrace`, `$funcsourcetrace`, `$functrace`,
  `$ZSH_EVAL_CONTEXT`, `$ZSH_SCRIPT`, `$ZSH_ARGZERO`, `$HISTCHARS` (already
  have `$histchars`).
- `local SECONDS` makes a local copy of the special and preserves live-
  update semantics (M8 left item).
- `unset` on a special drops its special meaning for good; next assignment
  is a plain scalar (M8 left item).
- `$LINENO` in a one-line function (`f() { echo $LINENO; }`) is 1; check
  against zsh (M8 left item).
- `$pipestatus` after a function call is the function body's last pipeline
  (M8 left item).

**History options**
- `HIST_EXPIRE_DUPS_FIRST`: when trimming to `$SAVEHIST`, drop duplicates
  before unique lines.
- `HIST_LEX_WORDS`: lex-based word splitting for `fc` / `!$`.
- `HIST_SAVE_BY_COPY`: write to a temp file and rename under the lock.
- `HIST_NO_FUNCTIONS`: don't save a function-definition line.
- `CSH_JUNKIE_HISTORY`: `!x` is `!?x?`, not `!x*`.
- `HIST_IGNORE_SPACE` keeps the line until the next prompt so `!!` still
  finds it (M9 left item).

**PRIVILEGED mode**
- Under `-p` / `PRIVILEGED`, do not read the user's rc files; use
  `/etc/zshenv.privileged` if present, else `/etc/zshenv`.
- Drop `IFS` back to the default at startup and after assignment.

**Tests**
- `tests/builtins_test.cpp`, `tests/vars_test.cpp`, `tests/history_test.cpp`,
  `tests/privileged_test.cpp`. Expected outputs from `zsh -f -c`.

### M18. Default function library and `.zshrc` compatibility [share/*]

- Install `share/jfsh/functions/` (CMake cache `JFSH_FUNCTIONS_DIR`) with a
  subset of zsh's shipped function library: `Base/`, `Completion/`, `Misc/`,
  `Prompts/`, `VCS_Info/`, `Zle/`, `Zftp/`, `Zutil/`, `Chpwd/`, `MIME/`.
- Default aliases `run-help`, `which-command`, `history` (`fc -l 1`) and
  whatever else `zsh -f` sets up (M7 left item).
- `run-help` function dispatching to `man` or format-specific handlers;
  `run-help-git`, `run-help-sudo`, `run-help-svn`.
- `promptinit`, `prompt -l`, `prompt theme [args]` and a handful of bundled
  themes (`adam1`, `clint`, `fade`, `walters`, `redhat`, `bart`).
- `vcs_info` with git support at minimum (`hg`, `svn` optional):
  `zstyle ':vcs_info:*' …` keys, `$vcs_info_msg_N_`.
- `add-zsh-hook`, `add-zle-hook-widget` (compose the hook arrays
  `precmd_functions`, `preexec_functions`, …).
- `.zshrc` compatibility: the current `--zsh-compat-rc` opt-in becomes the
  default once M10–M16 are landed, with `--no-zsh-compat-rc` to turn it
  off. Read `/etc/zsh/zshenv`, `/etc/zsh/zprofile`, `/etc/zsh/zshrc`,
  `/etc/zsh/zlogin`, `/etc/zsh/zlogout` alongside the `jfsh*` ones.
- A shipped `jfsh/zsh-compat-check` shell script scans a `.zshrc` for
  constructs jfsh doesn't yet support and prints a summary.

**Tests**
- `tests/compat_rc_test.cpp`: a corpus of real-world `.zshrc` lines
  (oh-my-zsh, prezto, starship, powerlevel10k) parse and set the expected
  state.
- `tests/prompt_themes_test.cpp`: every bundled theme renders without errors.
- Manual-run suite: start jfsh under a fresh user with oh-my-zsh's default
  `.zshrc`; the prompt renders, Tab works, `git` completion works.

### M19. Coprocesses, fd allocation, named references, async traps [exec/*]

- `coproc cmd`: fork the command into a subshell with its stdin/stdout
  connected to shell-accessible fds; set `$COPROC` to the fd pair and
  `$!` to the pid. Multiple concurrent coprocs allowed (one named per fd
  pair).
- `print -p`, `read -p`: coproc I/O (M17 adds the flags; this milestone
  wires them to the coproc fds).
- `cmd |& cmd` (stderr merge) stays as-is, separate from `coproc`.
- Fd allocation `{var}>file`, `{var}<file`, `{var}>&-`, `{var}<&fd`:
  parser allocates a shell-reserved fd ≥ 10 and stores it in `$var`; close
  on last use unless the user explicitly closes it.
- `typeset -n ref=name`: nameref semantics; assignment to `$ref` writes
  through to `name`; scope-aware lookup for `$name` resolution.
- `anonymous () { … } args`: `$0 = (anon)`, correct scoping, arguments
  through full expansion; `always` block after `return`.
- `( )` subshell honours `EF_NOFORK` when it is already a forked pipeline
  stage, so `cmd | ( body )` doesn't double-fork (M4 left item).
- Background pipeline reports each stage's pid in `jobs -l` (M6 left item).
- Suspended command's remaining pipeline becomes its own suspended job
  (M6 left item).
- `TRAPS_ASYNC`, `POSIX_TRAPS`, `POSIX_JOBS`, `AUTO_CONTINUE`, `AUTO_RESUME`.
- `time pipeline` with `$TIMEFMT`; `$REPORTTIME` threshold emits the time
  report when a pipeline runs longer than N seconds (M17 adds the builtin;
  this milestone adds the per-pipeline timing hook in the executor).

**Tests**
- `tests/coproc_test.cpp`, `tests/fdalloc_test.cpp`, `tests/nameref_test.cpp`,
  `tests/async_trap_test.cpp`.

### M20. zsh compatibility corpus, CI, packaging

The final pass criterion: jfsh runs the upstream zsh test suite, plus a hand-
curated real-world corpus, and ships as a drop-in `/bin/zsh` for interactive
use.

**Corpus**
- `tests/compat/` zsh scripts: each runnable under `zsh -f` and `jfsh -f`,
  with outputs diffed in CI. Grows per milestone.
- Import zsh's own `Test/` suite: `A*.ztst` (basic), `B*` (builtins),
  `C*` (compat), `D*` (prompts/ZLE), `E*` (modules), `V*` (variables),
  `Y*` (completion). Write a `.ztst` parser/runner (`tests/ztst_runner.cpp`);
  mark unsupported cases `SKIP`; grow the pass set per milestone. A green
  run is the "full zsh compatibility" line.
- Real-world corpus: oh-my-zsh, prezto, starship and powerlevel10k `.zshrc`s
  parse and start a prompt.

**CI**
- Matrix: g++-15, clang-21, Apple clang; Debug + Release + ASan+UBSan + LSan;
  on every PR.
- Benchmark harness: compare jfsh and zsh on startup time, prompt latency,
  a 50 k-line history read, and a 1 MB pipeline. Thresholds in CI: jfsh
  within 20% of zsh on each.
- Warn-free under `-Wall -Wextra -Wpedantic -Werror`; B56's
  `-Wno-unused-parameter` comes off.

**Packaging**
- `make install` lays out `bin/jfsh`, `share/jfsh/functions/*`,
  `lib/jfsh/modules/*.so`, `share/man/man1/jfsh.1` (generated from
  `jfsh --help` and the per-builtin `--help` strings), `share/doc/jfsh/`.
- Debian, RPM and Homebrew formulas once the compat corpus is green.
- `update-shells` integration: install adds `/usr/local/bin/jfsh` to
  `/etc/shells`.

**Pass criteria**
- `zsh/Test` A, B, C, D, E, V, Y run at ≥ 95% pass rate.
- A fresh user's `.zshrc` from `oh-my-zsh` or `prezto` starts a prompt in
  under 500 ms on reference hardware.
- Running `jfsh` with no flags against the same `.zshrc` as `zsh` shows the
  same prompt, the same completion for a dozen common commands, and the
  same output for the compat corpus.

### R. System 1 router and LLM agent (jfsh-specific track)

**Routing**
- **Ordering:** route **after** a cheap local check, so lines that obviously run are not
  sent to a model:
  - the first word is a builtin, function, alias or `PATH` executable;
  - or the line contains shell syntax (pipes, redirections, assignments).
  The Python side already has a "known-command cascade"; mirror it.
- **Heuristic router:** stop shelling out to `zsh -n` through `system()` (bug B42). Once
  jfsh's own parser works, use `Parser` in syntax-check mode.
- **Recovery and failure handling:**
  - a manual override: a prefix to force each verdict (e.g. `,question` → agent and
    `\cmd` → run, or a configurable key binding);
  - "did you mean to ask the agent?" recovery when a RUN-routed line fails with exit 127;
  - backoff or a circuit breaker when jeff-server or JEV is down (bug B43).
- **Builtins:** `jfsh-router [on|off|status|threshold N|backend jeff|jev|heuristic]` and
  an `explain` mode that prints the verdict and confidence for a line without running it.

**Agent**
- **Streaming:** finish streaming for Anthropic (SSE) and Ollama (NDJSON)
  [anthropic/stream], [ollama/stream]. `parse_sse_chunk` and `parse_ndjson_chunk` are
  implemented and tested; only the curl write-callback plumbing is left. Copy the OpenAI
  backend's `StreamState`/`LineSplitter` pattern.
- **Shell context:** give the agent `$PWD`, the last command, its exit status and
  (opt-in) its last N lines of output, plus `uname`, `$SHELL` and the history tail.
- **Proposing commands:** the agent may answer with a command. Show it highlighted and
  ask `Run? [y/N/e(dit)]`; on edit, push it into the editor buffer. Never run commands
  without confirmation. Record an agent-run command in history as the command itself.
- **Builtins:**
  - `agent reset` (clear the conversation), `agent model NAME` and `agent provider NAME`;
  - `agent history`;
  - reading provider config from `~/.config/jfsh/agent.toml` as well as the environment.
- **Requests:** send the configured `temperature`, `top_p` and `seed` (bug B47) and the
  OpenRouter attribution headers (bug B48).
- **Timeouts:** use a connect timeout plus an inactivity timeout for streams instead of
  a 30-second total timeout (bug B46).
- **Ctrl-C** while streaming aborts the request (`CURLOPT_XFERINFOFUNCTION` checking a
  flag) and keeps the shell alive.
- Call `curl_global_init` once at startup.

### Testing

- Parser tests that assert bytecode for every construct: `tests/parser_test.cpp` covers
  what M1 parses; extend it as constructs land.
- Executor tests that run scripts through `exec_string` and capture stdout/stderr/exit
  status: `tests/exec_test.cpp` has the in-process helper (`Shell::run`, fds 1 and 2
  redirected to temp files); extend it per milestone.
- A zsh compatibility corpus: `tests/compat/*.zsh` scripts run under both `zsh` and
  `jfsh`, with their outputs compared. Generate expected outputs from zsh in CI.
- Interactive tests: drive the editor through a pty (`forkpty`) and assert on screen
  output. Since M9, `tests/editor_test.cpp` does this in process (an `Editor` on a pty
  plus a small terminal emulator) and `tests/interactive_test.cpp` with the binary.
- The two former `DISABLED_` tests (`GlobPatternMatch.BracketExpressions`,
  `Lexer.SubstitutionsStayOneWord`) are enabled since M3.
- `tests/compound_test.cpp` covers M4 end to end, and `tests/cond_test.cpp` and
  `tests/math_test.cpp` cover M5; their expected outputs were checked against zsh 5.9
  (`zsh -f`).
- `tests/jobs_test.cpp` covers M6 in-process; `tests/interactive_test.cpp` runs the
  `jfsh` binary in a pty (`forkpty`) and drives Ctrl-Z, Ctrl-C, `fg`/`bg`, job notices,
  traps at the prompt and the exit checks with keystrokes. Expected output is zsh 5.9's
  (`zsh -f -i`). That is the start of the "interactive tests" item above.
- The suite passed under ASan + UBSan + LeakSanitizer on Linux when M6 landed (scratch
  build with `-fsanitize=address,undefined`, clang 20).
- `tests/shell_fixture.h` holds the in-process `Shell`/`TempDir` helpers shared by
  `exec_test.cpp`, `subst_test.cpp` and `compound_test.cpp`.
- Run the suite under ASan/UBSan in CI. jfsh's Debug build has no sanitizer flags yet,
  although the top-level build does. (The suite passed under ASan + UBSan + LeakSanitizer
  when M1 landed, using a scratch build with `-fsanitize=address,undefined`.)
- When M9 landed the whole suite (448 tests) passed under ASan + UBSan on macOS (Apple
  clang, Debug, scratch build). Homebrew's clang 23 couldn't build it on that machine
  (its libc++ headers don't find the SDK's), and g++ has no ASan on macOS.
- On macOS under ASan + UBSan (checked when M5 landed), every test passes except
  `Subst.ProcessSubstitutionIsLeftToTheExecutor`, and that one only in a full run: alone,
  or with any single other test left out, it passes, and it passes in the plain Debug
  build. When it fails, the shell's forked children die with SIGKILL before they run
  (`cat` exits 137, `<(…)` and `$(…)` produce nothing), with no sanitizer report. It is not
  an fd leak, zombies, the working directory or ASan's quarantine size; the cause is still
  unknown.

---

## Part 2 — Bugs in the current implementation

Severity:
- **Critical:** breaks basic use, or will as soon as the code is wired up.
- **High:** wrong results or a crash in a reachable path.
- **Medium:** incorrect but survivable.
- **Low:** cosmetic or a latent issue.

### Critical

| ID | Location | Bug | Evidence |
|---|---|---|---|
| B4 | `src/core/jobs.cpp:18` | On an empty table, `find_free_slot()` returns `mTable.size()` (0), so the first job gets id 0. Id 0 means "free slot", so that job is invisible to `get()`, `for_each()`, notifications and `reap()`. | **confirmed**: first `alloc` → id 0, `get(0)` = missing |

### High

| ID | Location | Bug | Evidence |
|---|---|---|---|
| B10 | `src/lexer/lexer.cpp:192` | Fd-number prefixes are not part of the redirection: `2>&1` lexes as word `2`, redir `>&`, word `1`, so `2` becomes a command argument. | code reading |
| B13 | `src/core/params.cpp:83` | Shell assignments now go through `set_scalar`, which updates the right scope and enforces readonly. The lower-level `assign()` still writes into the innermost scope, ignores `PM_READONLY`, and with `flags` 0 makes the parameter `PM_SCALAR` even for an integer or array value; `typeset`/`local`/`export` (M7) must not use it for plain updates. | code reading |
| B44 | `src/main.cpp:134`, `src/agent/provider.cpp:30` | `ProviderConfig::from_env()` picks the default model **before** `--provider` is applied, so `--provider anthropic` with no `--llm-model` sends `llama3` to Anthropic. An unknown `JFSH_PROVIDER` leaves the kind at its default silently. | code reading |

### Medium

| ID | Location | Bug | Evidence |
|---|---|---|---|
| B42 | `src/router/router.cpp:20` | `HeuristicRouter` runs `system("zsh -n -c …")` for **every** line, which costs a fork plus exec of `/bin/sh` and `zsh`. If zsh isn't installed, `system` returns 127 and **every line is routed to the LLM**. `zsh -n` also accepts most natural language (`what is ls`), so only lines that happen to be invalid syntax, such as an unbalanced apostrophe in `what's ls`, reach the agent. | code reading |
| B43 | `src/router/jeff.cpp:83`, `src/router/jev.cpp:111` | When the service is down, each line waits for the full timeout (1 s for jeff, 10 s for JEV), prints a warning and falls back. There's no backoff, so the shell becomes unusable. JEV also sends `Authorization: Bearer ` with an empty key. | code reading |
| B45 | `src/agent/provider.cpp:35`, `include/jfsh/agent/provider.h:43` | The default Anthropic model `claude-opus-4-7` (in `from_env()` and the `default_anthropic` default argument) is outdated; it should probably be `claude-opus-5-5`. | code reading |
| B46 | `include/jfsh/agent/provider.h:37` | `timeout_ms = 30000` is applied as `CURLOPT_TIMEOUT_MS`, a **total** transfer limit, so any streamed answer longer than 30 s is cut off. | code reading |
| B47 | `include/jfsh/agent/provider.h:25` | `temperature`, `top_p` and `seed` in `ModelParams` are never sent to any provider. | code reading |
| B48 | `include/jfsh/agent/openrouter.h:24` | The `HTTP-Referer`/`X-Title` headers the header comment promises are never sent. `OpenAIBackend::http_post` supports `extra_headers` (`src/agent/openai.cpp:92`), but `OpenRouterBackend` never passes any, and `chat_stream` makes its own curl request (`openai.cpp:187`) with no way to add headers, so both paths need it. | code reading |
| B59 | `src/lexer/lexer.cpp:126`, `:192` | Redirection lexing is incomplete. `read_token` routes `&>` to `read_redir('&')`, but it has already consumed the `>`, so `&>file` lexes as a bare `&` redirection and `&>>` as `&>` followed by `>`. `>&-`/`<&-` lex as `>&` plus a word `-`, and `>>\|` is split. Conversely, any `<`/`>` followed by `<`, `>`, `\|`, `&` or `-` is kept as one token, so invalid pairs like `<-`, `><` and `>-` are accepted. | code reading |

### Low

| ID | Location | Bug |
|---|---|---|
| B49 | `src/agent/agent.cpp` | The non-streaming path of `Agent::handle` prints a newline even when the request failed. |
| B51 | `include/jfsh/core/param.h:63` | It uses `std::unique_ptr` without `#include <memory>` and only compiles through `types.h`. |
| B53 | `src/modules/module.cpp` | `ModuleManager` is never instantiated. Its `resolve_path` maps `zsh/x` → `zsh_x.so` but has no default search path, so `zmodload` can't work even once implemented. |
| B56 | `cmake/CompilerOptions.cmake` | Release builds use `-march=native`, so binaries only run on CPUs like the build machine's. simdjson already dispatches at runtime. `-Wno-unused-parameter` is still on "for the stub phase". |
| B57 | `src/main.cpp` | `curl_global_init` is never called, and the HTTP code relies on `curl_easy_init`'s implicit, non-thread-safe init. |

### Fixed

| ID | Fixed by |
|---|---|
| B5 | M9: with APPEND_HISTORY only the entries not yet in `HISTFILE` are appended. |
| B25 | M9: the editor moves by UTF-8 character and measures widths with `wcwidth()` (`src/core/utf8.cpp`). |
| B26 | M9: the prompt is expanded once per line; each refresh redraws from the prompt's first row, wrapping rows itself. |
| B27 | M9: the terminal width is read on every refresh, and the editor redraws after a SIGWINCH. |
| B28 | M9: completion replaces the word at the cursor, found by a shell-aware tokenizer; the engine is wired in. |
| B34 | M9: numeric prompt arguments (`%1~`, `%2d`, `%-1/`, `%3c`, `%2m`). |
| B35 | M9: `%F{…}`/`%K{…}` take names, 0–255 and `#rrggbb`; `%B %U %S` work; unknown escapes expand to nothing. |
| B36 | M9: `%~` replaces `$HOME` only at a component boundary. |
| B37 | M9: `%h`/`%!` are the history number (`$HISTCMD`). |
| B39 | M9: the history file's EXTENDED_HISTORY lines and `\`-continued multi-line entries are read; `sync()` reads only what is new. |
| B40 | M9: HIST_NO_STORE drops `history`/`fc -l` lines and no longer skips the duplicate checks. |
| B52 | M9: command completion lists executables only, each once, sorted; file completion hides dotfiles unless asked and adds `/` to directories. |
| B54 | M9: without a terminal the prompt goes to stderr. |
| B55 | M9: re-checked on macOS: no duplicate-library warnings once `jfsh_tests` stopped naming `jfsh_history` (which `jfsh_exec` brings). |
| B58 | M9: HIST_IGNORE_ALL_DUPS drops the older copy and keeps the new one. |
| B61 | M9: the prompt reads `$HOME` and `$PWD` from the shell's parameters. |
| B6 | M7: `HistoryManager::lookup(0)` finds nothing instead of reading past the end. |
| B32 | M8: `parse_invocation` rejects unknown options, `-c`/`-o` with no argument, and unknown `--router`/`--provider` values. |
| B38 | M8: the interactive loop reloads `PS1`/`PROMPT` and friends before every prompt. |
| B19 | M7: every builtin is real, or prints `not implemented` and returns 1. |
| B20 | M7: `cd` reports errors as zsh does and keeps `$PWD`/`$OLDPWD`. |
| B21 | M7: `zpty` is registered (as `not implemented`). |
| B50 | M7: the option table is generated from zsh 5.9; `Options.TableIsInIndexOrder` checks every row sits at its index. |
| B63 | M7: the signal self-pipe sat on fds 3/4, so `exec 3<file` closed its read end and the next SIGCHLD killed the shell with SIGPIPE; it now lives on fds ≥ 10. |
| B64 | M7: a redirection whose new fd landed on the target number itself (`3<<<x` with fd 3 closed) closed it again; it is moved off first. |
| B65 | M7: a reserved word as a redirection target (`< in`) and `cmd & ;` were parse errors. |
| B1 | M0/M1: parser and executor share the prefix layout documented in `wordcode.h`. |
| B2 | M0: `exec_string` lexes, parses and executes. |
| B3 | `StringInputSource`/`FileInputSource::unget(-1)` are no-ops; regression test `Lexer.TrailingOperatorAtEofTerminates`. |
| B4 | M2: `JobTable::find_free_slot` reserves slot 0 so the first `alloc` returns id 1. |
| B9 | `Parser::parse` fails on leftover tokens and on an empty error message. |
| B10 | M2: the lexer bakes digit fd prefixes into the `TK_REDIR` token (`2>`, `12<`, `2>&1`). |
| B11 | M4: `{ … }` is a brace group run in the current shell; function definitions are `name ()` / `function name`. |
| B12 | External commands use `execve` with `make_envp()`, which now emits each name once with its innermost value. |
| B22 | M2/M6: one handler for every caught signal sets a bit in a lock-free atomic pending set and writes a byte to the self-pipe; the executor drains it, reaps and runs traps between commands (`Executor::handle_signals`). |
| B14 | M6: foreground jobs under job control are waited for with `WUNTRACED`; a stopped one is recorded as a job and the shell takes the terminal back. |
| B16 | M6: setting a trap installs the handler (`SignalManager::install`), and traps run between commands; SIGKILL/SIGSTOP are recorded but not installed. |
| B17 | M6: `SignalManager::kill_job` refuses `pgid <= 1`; jobs without a process group of their own are signalled pid by pid. |
| B23 | M6: an interactive shell has its own process group and the terminal, and catches or ignores the job-control signals, so Ctrl-C stops the command (and the command line), not jfsh. |
| B24 | M6: Ctrl-C in the line editor drops the line (at PS2, the whole command). |
| B29 | M6: the editor ends the read on EOF, a read error or a hung-up terminal instead of looping. |
| B33 | M6: ERR_EXIT is applied by the executor, with zsh's exemptions; the REPL no longer exits on every failed line. |
| B41 | M6: removing a job promotes the previous one, killed processes give `Signalled`, `exit_status` honours PIPEFAIL, and changed jobs are reported whatever their flags. |
| B8 | M3: the lexer keeps `$(…)`, `${…}`, `$((…))`, backquotes, `$'…'`, brace expansions, `(…)` glob groups, `<lo-hi>` and `name=(…)` inside one word. |
| B30 | M3: `CompiledPattern` implements `[…]`, escapes and groups; `glob_expand` walks every `/`-separated segment. |
| B62 | M3: `Lexer::read_heredoc` threw away the rest of the line after `<<EOF`, newline included, so `cat <<EOF \| x` lost `\| x` and the command after the body became extra arguments. Bodies are now read when the lexer reaches the newline. |
| B60 | M2: `par_redir` consumes the target word and emits `WC_REDIR + WC_STRING`, so redirections no longer turn their target into an argument. |
| B7 | M4: wherever the parser expects words (`for … in` lists, `case` patterns and subjects, `repeat` counts, anonymous function arguments, command arguments), it takes a reserved word as a word, so keywords only act in command position. |
| B18 | M4: `break N` / `continue N` unwind N loops through `ExecContext::break_level` and `continuing`; `loop_depth` reports a `break` outside any loop. |
| B15 | M5: `OptionState::find` tries the exact name first and only then strips `no`, reporting the negation through an out-parameter. |
| B31 | M4: stdin mode gathers lines until `is_incomplete()` says the command is whole, so constructs that span lines work and a line longer than the 4096-byte buffer is no longer split into two commands. |
