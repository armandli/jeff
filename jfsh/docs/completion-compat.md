# Completion system compatibility

jfsh runs zsh's own completion system (compsys) on top of its port of
the zsh/complete, zsh/computil, zsh/zutil and zsh/parameter modules.
This file covers what has been checked against zsh 5.9 and what is known
to differ.

## The function library

`share/jfsh/functions/` holds zsh 5.9's function library unchanged: all
1203 files from `/usr/share/zsh/5.9/functions`. The zsh licence is in
`share/jfsh/functions/LICENCE`. `_composer` and `_darcs` are under the GPL
and say so in their headers.

The library is installed to `JFSH_FUNCTIONS_DIR`, a CMake cache variable
that defaults to `share/jfsh/functions` under the install prefix. The
default `$fpath` holds the site-functions directory and then the library.
When the install directory does not exist, jfsh uses the source tree's
copy, so a build that has not been installed still works. With no further
setup this works as in zsh:

    autoload -Uz compinit; compinit

`compinit` is the zsh function itself:
- It writes the dump (`.zcompdump`, or `-d file`), and the dump is
  identical to the one zsh writes.
- It runs compaudit's security check, unless `-u` or `-C` is given.
- It defines `compdef`, `_comps` and `_patcomps`.
- It rebinds the completion widgets to `_main_complete`.

Before compinit has run, jfsh's built-in `compdef`, `_files` and
`_directories` (from M9) still work. Once the library's functions are
defined, they take over, because functions are looked up before builtins.

## How it was checked

Each case below was typed into an interactive `zsh -f` and an interactive
`jfsh -f` on the same 80-column pty. Both shells first ran
`fpath=(share/jfsh/functions); autoload -Uz compinit; compinit -D -u`.
The resulting line, cursor and screen were compared through a small
VT100 emulator. Where they differed, `_main_complete` was traced with
xtrace in both shells and the traces diffed. All the cases listed now
give the same result in both shells:

- Commands and options:
  - `ls -`, `cd`, command names (`ech`), `rm -`, `cp -`, `ln -`,
    `chmod u+`, `chown roo`
  - `grep --c`, `find . -na`, `diff --side`, `curl --compr`,
    `rsync --del`
  - `tar -`, `tar -xf` with files, `make` targets from a Makefile,
    `make -`
  - `kill -` (signals), `kill -s`, `trap ls INT`
  - `ssh` hosts and users, `sudo ech`, `env LANG=C ech`, `nice -n 5 ech`,
    `time ech`, `noglob ech`
  - `which ech`, `man ls`, `print -`, `typeset -`, `bindkey -`
- git, in a fresh repository: `git ch`, `git add`, `git checkout --`,
  `git commit --am`.
- Parameters and options:
  - `echo $HOM`, `echo ${PA`, `unset ZSH_VER`, `export PAT`
  - `setopt auto`, `zstyle ':completion:*'`
- Paths and redirection:
  - `ls rvt/Mak`, `ls rvt/oth`, `cd rvt/` (files and directories in a
    subdirectory, with their space, `/` and list marks)
  - `ls ~roo` (user names)
  - `cd -` (the directory stack)
  - `mv a` with files, `echo x > out` (redirection targets)
- Styles and completers:
  - `completer _complete _approximate`, with one correction and with
    several (cycling through them)
  - `matcher-list 'm:{a-z}={A-Z}'`
  - `format '%B%d%b'` with `group-name ''`
- Custom functions using:
  - `_arguments`: options with descriptions, `--long=` values, `(- *)`
    exclusions, `*::rest` arguments, `->state` actions, `{code}` actions,
    clustered single-letter options (`-s`), excluded options
  - `_describe`, `_values -s ,`
  - `compstate` set from inside the function (`insert=menu`, `list=''`)
- Listings:
  - grids, packed and row-first lists, groups
  - lists taller than the screen
- Completion widgets that run pipelines, subshells and
  `exec </dev/null`.

The same cases are covered in process by `tests/compsys_test.cpp`, with
zsh's screens as the expected values.

## Known differences

- **No `zmodload` completion.** jfsh's modules are built in, so there is
  no `$module_path` and no module files for `_zmodload` to list. It only
  offers the module aliases.
- **Menu selection.** zsh/complist, `menu select` and `list-colors` are not
  done yet; they are planned for phase 15d.
- **Library files that do not parse.** jfsh cannot parse these files yet;
  each one fails only when it is autoloaded:
  - `_growisofs`
  - `compinstall`
- **Other gaps.** The library sometimes reaches these:
  - No coprocesses: `>&p` fails as it does in zsh when no coprocess is
    running.
  - Many parameters that zsh always defines are unset in jfsh's `-c` mode
    (`IFS`, `PS1`, `LINES`, `HISTSIZE`, …). `$parameters` lists the ones
    jfsh has.
  - `$functrace` values are only approximate.
  - `typeset -p` of a tied pair is printed differently.
  - `disable -p`, `-a` and `-r` are not supported.
  - In `${(q)…}`, an array inside double quotes is joined differently.
  - xtrace output differs in places (the order of assignments, how
    `case` and `for` are traced), which only shows when tracing
    completion functions.
- **Speed.** A completion that walks many functions is several times
  slower than in zsh: `ls -<Tab>` takes about 0.14 s, against 0.02 s in
  zsh.
- **Equal sort keys.** When glob qualifiers sort files that tie on the
  key (`*(oL)` on files of equal size), zsh's qsort gives an unstable
  order. jfsh orders the ties by name.
