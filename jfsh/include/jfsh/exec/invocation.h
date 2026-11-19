#ifndef JFSH_EXEC_INVOCATION_H
#define JFSH_EXEC_INVOCATION_H
// jfsh/exec/invocation.h — the shell's command line, parsed as zsh parses
// its own (init.c parseargs), plus jfsh's router and agent options.
//
//   jfsh [±options…] [-c command [arg0 [args…]]]
//   jfsh [±options…] [-s] [args…]
//   jfsh [±options…] script [args…]
//
// Options are single letters in clusters (`-xe`, `+x`), `-o name`/`+o name`
// (or `-oname`), `--name` and `--no-name` for any shell option, and `--`
// or `-` to end them.  `-c` takes the first argument after the options as
// the command; the arguments after that become $0 and the positional
// parameters.

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/options.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct Invocation {
  opt<str> command;            // -c: the command string
  str      script;             // the script file, when not -c or -s
  str      arg0;               // $0
  vec<str> positional;         // $1 …

  // Options the invocation decides; each is set only when asked for, so the
  // caller can fall back to what the terminal says.
  opt<bool> interactive;       // -i, +i, -o interactive
  opt<bool> monitor;           // -m, +m
  opt<bool> zle;               // -Z, +Z
  bool      login     = false; // -l, --login, or an $0 starting with '-'
  bool      shinstdin = false; // -s
  bool      rcs       = true;  // -f / -o norcs: read no startup files but jfshenv
  bool      global_rcs = true; // -d / -o noglobalrcs: skip the /etc files

  // Emulation picked from argv[0] (`sh`, `ksh`, `csh`, or `-sh`/`-ksh`/`-csh`
  // for a login shell).  main() applies this before reading the rc files,
  // and `emulate` can later override it.  ZSH when no name match.
  OptionState::Emulation emulation = OptionState::Emulation::ZSH;

  // Every other option, in the order given.
  vec<s::pair<OptIndex, bool>> options;

  // jfsh's own long options.
  bool zsh_compat_rc = false;  // --zsh-compat-rc: also read ~/.zshenv, ~/.zshrc, …
  bool help          = false;  // --help, -h
  bool version       = false;  // --version
  bool norouter      = false;  // --norouter
  str  router;                 // --router jeff|jev|heuristic
  str  jeff_url;               // --jeff-url URL
  str  typesafe_key;           // --typesafe-key KEY
  str  jev_model;              // --jev-model NAME
  str  provider;               // --provider openai|anthropic|ollama|openrouter
  str  api_key;                // --api-key KEY
  str  llm_model;              // --llm-model NAME
};

// Parses argv (argv[0] included).  An unknown option, `-c` or `-o` with
// nothing after it, or an unknown router or provider is an error; its
// message is what follows "jfsh: ".
Result<Invocation> parse_invocation(const vec<str>& argv);

// The usage text for --help.
str invocation_usage(strv prog);

}  // namespace jfsh

#endif  // JFSH_EXEC_INVOCATION_H
