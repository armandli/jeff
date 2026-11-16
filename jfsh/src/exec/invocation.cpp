// Parsing the shell's command line: zsh's parseargs/parseopts, with jfsh's
// router and agent options added as long options.

#include <jfsh/exec/invocation.h>

#include <algorithm>
#include <array>

#include <jfsh/core/aliases.h>

namespace jfsh {

namespace {

ShellError bad(str msg) { return ShellError{ErrorKind::ExecError, s::move(msg)}; }

// Records an option the invocation sets: the ones that decide how the shell
// starts go in their own fields, the rest in `options`.
void record(Invocation& inv, OptIndex idx, bool on) {
  switch (idx) {
    case OPT_INTERACTIVE: inv.interactive = on; break;
    case OPT_MONITOR:     inv.monitor     = on; break;
    case OPT_ZLE:         inv.zle         = on; break;
    case OPT_LOGIN:       inv.login       = on; break;
    case OPT_SHINSTDIN:   inv.shinstdin   = on; break;
    case OPT_RCS:         inv.rcs         = on; break;
    case OPT_GLOBALRCS:   inv.global_rcs  = on; break;
    default:              inv.options.emplace_back(idx, on); break;
  }
}

// `-o name`, `--name`: names follow setopt's rules (case, `_` and a `no`
// prefix don't matter); `--no-name` turns it off as well.
Result<void> named(Invocation& inv, strv name, bool on) {
  str n(name);
  s::replace(n.begin(), n.end(), '-', '_');
  bool negated = false;
  auto info = OptionState::find(n, &negated);
  if (not info) return s::unexpected(bad("no such option: " + str(name)));
  record(inv, info->get().index, on != negated);
  return {};
}

// jfsh's long options that take a value.
constexpr s::array<strv, 7> VALUE_OPTS = {
  "router", "jeff-url", "typesafe-key", "jev-model", "provider", "api-key", "llm-model",
};

Result<void> value_opt(Invocation& inv, strv name, str value) {
  if (name == "router") {
    if (value != "jeff" and value != "jev" and value != "heuristic")
      return s::unexpected(bad("--router: unknown router: " + value + " (jeff, jev or heuristic)"));
    inv.router = s::move(value);
  } else if (name == "provider") {
    if (value != "openai" and value != "anthropic" and value != "ollama" and value != "openrouter")
      return s::unexpected(bad("--provider: unknown provider: " + value
                               + " (openai, anthropic, ollama or openrouter)"));
    inv.provider = s::move(value);
  } else if (name == "jeff-url")     { inv.jeff_url     = s::move(value); }
  else if (name == "typesafe-key")   { inv.typesafe_key = s::move(value); }
  else if (name == "jev-model")      { inv.jev_model    = s::move(value); }
  else if (name == "api-key")        { inv.api_key      = s::move(value); }
  else if (name == "llm-model")      { inv.llm_model    = s::move(value); }
  return {};
}

}  // namespace

Result<Invocation> parse_invocation(const vec<str>& argv) {
  Invocation inv;
  inv.arg0  = argv.empty() ? str("jfsh") : argv[0];
  inv.login = not inv.arg0.empty() and inv.arg0[0] == '-';

  OptionState letters;   // single letters are zsh's, not ksh's
  bool want_command = false;
  s::size_t i = 1;
  for (; i < argv.size(); ++i) {
    strv a = argv[i];
    if (a == "--" or a == "-") { ++i; break; }
    if (a.size() < 2 or (a[0] != '-' and a[0] != '+')) break;

    if (a.starts_with("--")) {
      strv name = a.substr(2);
      str  value;
      bool has_value = false;
      if (auto eq = name.find('='); eq != strv::npos) {
        value     = str(name.substr(eq + 1));
        name      = name.substr(0, eq);
        has_value = true;
      }
      if (s::find(VALUE_OPTS.begin(), VALUE_OPTS.end(), name) != VALUE_OPTS.end()) {
        if (not has_value) {
          if (i + 1 >= argv.size())
            return s::unexpected(bad("--" + str(name) + ": argument expected"));
          value = argv[++i];
        }
        if (auto r = value_opt(inv, name, s::move(value)); not r) return s::unexpected(r.error());
        continue;
      }
      if (has_value) return s::unexpected(bad("bad option: " + str(a)));
      if (name == "help")          { inv.help          = true; continue; }
      if (name == "version")       { inv.version       = true; continue; }
      if (name == "norouter")      { inv.norouter      = true; continue; }
      if (name == "zsh-compat-rc") { inv.zsh_compat_rc = true; continue; }
      if (auto r = named(inv, name, true); not r) return s::unexpected(r.error());
      continue;
    }

    bool on = a[0] == '-';
    for (s::size_t j = 1; j < a.size(); ++j) {
      char c = a[j];
      if (c == 'c' and on) { want_command = true; continue; }
      if (c == 'h' and on and a.size() == 2) { inv.help = true; continue; }
      if (c == 'o') {
        strv name = a.substr(j + 1);
        if (name.empty()) {
          if (i + 1 >= argv.size()) return s::unexpected(bad("string expected after -o"));
          name = argv[++i];
        }
        if (auto r = named(inv, name, on); not r) return s::unexpected(r.error());
        break;
      }
      auto l = letters.letter(c);
      if (not l) return s::unexpected(bad(str("bad option: ") + a[0] + c));
      record(inv, l->index, on != l->negated);
    }
  }

  vec<str> rest(argv.begin() + static_cast<s::ptrdiff_t>(s::min(i, argv.size())), argv.end());
  if (want_command) {
    if (rest.empty()) return s::unexpected(bad("string expected after -c"));
    inv.command = rest[0];
    if (rest.size() > 1) {
      inv.arg0 = rest[1];
      inv.positional.assign(rest.begin() + 2, rest.end());
    }
  } else if (not inv.shinstdin and not rest.empty()) {
    inv.script = rest[0];
    inv.arg0   = rest[0];
    inv.positional.assign(rest.begin() + 1, rest.end());
  } else {
    inv.shinstdin  = true;
    inv.positional = s::move(rest);
  }
  return inv;
}

str invocation_usage(strv prog) {
  return "Usage: " + str(prog) + " [±options] [-c command [arg0 [args…]] | -s [args…] | script [args…]]\n"
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
