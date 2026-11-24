// parse_invocation in this file is derived from zsh 5.9's parseopts and
// parseargs in Src/init.c.  zsh's copyright and licence:
//
// The Z Shell is copyright (c) 1992-2017 Paul Falstad, Richard Coleman,
// Zoltán Hidvégi, Andrew Main, Peter Stephenson, Sven Wischnowsky, and
// others.  All rights reserved.  Individual authors, whether or not
// specifically named, retain copyright in all changes; in what follows, they
// are referred to as `the Zsh Development Group'.  This is for convenience
// only and this body has no legal status.  The Z shell is distributed under
// the following licence; any provisions made in individual files take
// precedence.
//
// Permission is hereby granted, without written agreement and without
// licence or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall the Zsh Development Group be liable to any party for
// direct, indirect, special, incidental, or consequential damages arising out
// of the use of this software and its documentation, even if the Zsh
// Development Group have been advised of the possibility of such damage.
//
// The Zsh Development Group specifically disclaim any warranties, including,
// but not limited to, the implied warranties of merchantability and fitness
// for a particular purpose.  The software provided hereunder is on an "as is"
// basis, and the Zsh Development Group have no obligation to provide
// maintenance, support, updates, enhancements, or modifications.

// Parsing the shell's command line: zsh's parseargs/parseopts, with jfsh's
// router and agent options added as long options.

#include <jfsh/exec/invocation.h>

#include <algorithm>
#include <array>
#include <cctype>

#include <jfsh/core/aliases.h>

namespace jfsh {

namespace {

// Records an option the invocation sets: the ones that decide how the shell
// starts go in their own fields, the rest in `options`.
void record(Invocation& inv, OptIndex idx, bool on) {
  switch (idx) {
    case OPT_INTERACTIVE: inv.interactive = on;

    break; case OPT_MONITOR: inv.monitor = on;

    break; case OPT_ZLE: inv.zle = on;

    break; case OPT_LOGIN: inv.login = on;

    break; case OPT_SHINSTDIN: inv.shinstdin = on;

    break; case OPT_SINGLECOMMAND: inv.single = on;

    break; case OPT_RCS: inv.rcs = on;

    break; case OPT_GLOBALRCS: inv.global_rcs = on;

    break; default: inv.options.emplace_back(idx, on);
  }
}

// `-o name`, `--name`: names follow setopt's rules (case, `_` and a `no`
// prefix don't matter).  Returns false for no such option.
bool named(Invocation& inv, strv name, bool on) {
  bool negated = false;
  auto info = OptionState::find(name, &negated);
  if (not info) return false;
  record(inv, info->get().index, on != negated);
  return true;
}

// jfsh's long options that take a value.
constexpr s::array<strv, 7> VALUE_OPTS = {
  "router", "jeff-url", "typesafe-key", "jev-model", "provider", "api-key", "llm-model",
};

// Sets a value option; the error message, if the value is no good.
opt<str> value_opt(Invocation& inv, strv name, str value) {
  if (name == "router") {
    if (value != "jeff" and value != "jev" and value != "heuristic")
      return "--router: unknown router: " + value + " (jeff, jev or heuristic)";
    inv.router = s::move(value);
  } else if (name == "provider") {
    if (value != "openai" and value != "anthropic" and value != "ollama" and value != "openrouter")
      return "--provider: unknown provider: " + value
           + " (openai, anthropic, ollama or openrouter)";
    inv.provider = s::move(value);
  } else if (name == "jeff-url")     { inv.jeff_url     = s::move(value); }
  else if (name == "typesafe-key")   { inv.typesafe_key = s::move(value); }
  else if (name == "jev-model")      { inv.jev_model    = s::move(value); }
  else if (name == "api-key")        { inv.api_key      = s::move(value); }
  else if (name == "llm-model")      { inv.llm_model    = s::move(value); }
  return s::nullopt;
}

// The emulation a name stands for, by zsh's emulate(): its first letter
// (after an `r`): c, k, s or b, else zsh.
OptionState::Emulation emulation_named(strv name) {
  char c = name.empty() ? '\0' : name[0];
  if (c == 'r' and name.size() > 1) c = name[1];
  if (c == 'c') return OptionState::Emulation::CSH;
  if (c == 'k') return OptionState::Emulation::KSH;
  if (c == 's' or c == 'b') return OptionState::Emulation::SH;
  return OptionState::Emulation::ZSH;
}

}  // namespace

Result<Invocation> parse_invocation(const vec<str>& argv) {
  Invocation inv;
  inv.arg0  = argv.empty() ? str("jfsh") : argv[0];
  inv.login = not inv.arg0.empty() and inv.arg0[0] == '-';

  // M12: pick an emulation from argv[0]'s basename (zsh's init.c does the
  // same).  `-sh` means a login `sh`; `rbash` / `bash` are treated as sh;
  // `ksh` and `csh` get their own emulations.  Anything else stays zsh.
  {
    strv name = inv.arg0;
    if (not name.empty() and name[0] == '-') name = name.substr(1);
    if (auto slash = name.rfind('/'); slash != strv::npos) name = name.substr(slash + 1);
    if (name == "sh" or name == "bash" or name == "rbash" or name == "-sh"
        or name == "ash" or name == "dash")
      inv.emulation = OptionState::Emulation::SH;
    else if (name == "ksh" or name == "ksh93" or name == "mksh" or name == "pdksh")
      inv.emulation = OptionState::Emulation::KSH;
    else if (name == "csh" or name == "tcsh")
      inv.emulation = OptionState::Emulation::CSH;
  }

  // Errors are zerr's: named by $0, or by the shell once -c is seen (zsh's
  // scriptname "zsh").
  opt<str> cmd_flag;   // the cluster from its `c` on, for the message
  auto fail = [&](str msg) {
    return s::unexpected(ShellError{ErrorKind::ExecError,
                                    (cmd_flag ? str("jfsh") : inv.arg0) + ": " + msg});
  };

  OptionState letters;   // single letters are zsh's, not ksh's
  bool emulate_allowed = true;   // --emulate must come before any option
  bool option_break    = false;  // -b: options end with this argument
  s::size_t i = 1;
  // Options, as zsh's parseopts: each argument starting with `-` or `+`.  A
  // lone `-` or `+` is taken as `--`.
  while (not option_break and i < argv.size()
         and (argv[i].starts_with('-') or argv[i].starts_with('+'))) {
    str  a  = argv[i].size() == 1 ? str("--") : argv[i];
    bool on = argv[i][0] == '-';
    for (s::size_t j = 1; j < a.size(); ++j) {
      char c = a[j];
      if (c == '-') {
        // A `-` that ends the argument ends the options (`--`, `+-`, `-x-`).
        if (j + 1 == a.size()) {
          ++i;
          goto done;
        }
        // A long option: right after the first `-` or `+` only.
        if (j != 1) return fail("bad option string: '" + argv[i] + "'");
        str name = a.substr(2);
        if (name == "version") { inv.version = true; return inv; }
        if (name == "help")    { inv.help    = true; return inv; }
        if (name == "emulate") {
          if (++i >= argv.size()) return fail("--emulate: argument required");
          if (not emulate_allowed) return fail("--emulate: must precede other options");
          inv.emulation = emulation_named(argv[i]);
          break;
        }
        // jfsh's own; `--name=value` or `--name value` for those with a value.
        str value;
        auto eq = name.find('=');
        strv key = strv(name).substr(0, eq);
        if (s::find(VALUE_OPTS.begin(), VALUE_OPTS.end(), key) != VALUE_OPTS.end()) {
          if (eq != str::npos) {
            value = name.substr(eq + 1);
          } else {
            if (i + 1 >= argv.size()) return fail("--" + str(key) + ": argument expected");
            value = argv[++i];
          }
          if (auto err = value_opt(inv, key, s::move(value))) return fail(*err);
          break;
        }
        if (name == "norouter")      { inv.norouter      = true; break; }
        if (name == "zsh-compat-rc") { inv.zsh_compat_rc = true; break; }
        // `-` characters are allowed in long option names.
        s::replace(name.begin(), name.end(), '-', '_');
        emulate_allowed = false;
        if (not named(inv, name, on)) return fail("no such option: " + name);
        break;
      }
      if (c == 'b') {
        emulate_allowed = false;
        option_break    = true;
      } else if (c == 'c') {
        emulate_allowed = false;
        cmd_flag        = a.substr(j);
      } else if (c == 'h' and on and a.size() == 2) {
        inv.help = true;
      } else if (c == 'o') {
        emulate_allowed = false;
        str name = a.substr(j + 1);
        if (name.empty()) {
          if (++i >= argv.size()) return fail("string expected after -o");
          name = argv[i];
        }
        if (not named(inv, name, on)) return fail("no such option: " + name);
        break;
      } else if (s::isspace(static_cast<unsigned char>(c))) {
        // Blanks may end an argument, and nothing else may follow them.
        if (a.find_first_not_of(" \t\n\v\f\r", j) != str::npos)
          return fail("bad option string: '" + argv[i] + "'");
        break;
      } else {
        emulate_allowed = false;
        auto l = letters.letter(c);
        if (not l) return fail(str("bad option: -") + c);
        record(inv, l->index, on != l->negated);
      }
    }
    ++i;
  }
done:
  if (cmd_flag) {
    if (i >= argv.size()) return fail("string expected after -" + *cmd_flag);
    inv.command = argv[i++];
  }

  // What is left: with -s, all positional parameters; else the first is $0
  // after -c, or the script to read.  Without either, commands come from
  // standard input.
  vec<str> rest(argv.begin() + static_cast<s::ptrdiff_t>(s::min(i, argv.size())), argv.end());
  if (not rest.empty() and not inv.shinstdin) {
    // (A script becomes $0 once the startup files have been read.)
    if (inv.command) inv.arg0   = rest[0];
    else             inv.script = rest[0];
    rest.erase(rest.begin());
  } else if (rest.empty() and not inv.command) {
    inv.shinstdin = true;
  }
  inv.positional = s::move(rest);
  return inv;
}

str invocation_usage(strv prog) {
  return "Usage: " + str(prog)
      + " [±options] [-c command [arg0 [args…]] | -s [args…] | script [args…]]\n"
      "Shell options (as zsh):\n"
      "  -c                  take the first argument as the command to run\n"
      "  -s                  read commands from standard input\n"
      "  -i                  force an interactive shell\n"
      "  -l, --login         act as a login shell\n"
      "  -f                  read no startup files but /etc/jfshenv (NO_RCS)\n"
      "  -o NAME, +o NAME    set or unset an option; also --NAME, --no-NAME\n"
      "  -LETTER, +LETTER    set or unset the option with that letter (-x, -e, …)\n"
      "  --zsh-compat-rc     also read ~/.zshenv, ~/.zprofile, ~/.zshrc, ~/.zlogin, ~/.zlogout\n"
      "  --version           print the version and exit\n"
      "System 1 router and LLM agent:\n"
      "  --norouter          disable AI routing (all commands run as shell)\n"
      "  --router R          System 1 router: jeff|jev|heuristic\n"
      "  --jeff-url URL      jeff-server base URL (default: http://127.0.0.1:8765)\n"
      "  --typesafe-key K    TypeSafe API key for JEV router\n"
      "  --jev-model M       JEV model name (default: jev-latest)\n"
      "  --provider P        LLM provider: openai|anthropic|ollama|openrouter\n"
      "  --api-key K         LLM API key\n"
      "  --llm-model M       LLM model name\n";
}

}  // namespace jfsh
