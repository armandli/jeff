// Option and positional-parameter builtins: set, setopt, unsetopt,
// emulate, shift.

#include <cstdio>

#include <algorithm>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/math.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/glob/glob.h>

namespace jfsh {

namespace {

// Options the shell's invocation decides.  Monitor and ZLE can change
// only in an interactive shell.
bool can_change(const OptionInfo& info, const OptionState& opts) {
  switch (info.index) {
    case OPT_INTERACTIVE: case OPT_SHINSTDIN: case OPT_SINGLECOMMAND:
      return false;
    case OPT_MONITOR: case OPT_ZLE:
      return opts.interactive();
    default:
      return true;
  }
}

// The name an option is listed under: `noX` when X is on by default.
str display_name(const OptionInfo& info, const OptionState& opts) {
  return opts.default_of(info) ? "no" + str(info.name) : str(info.name);
}

// `set -o`, and setopt under KSH_OPTION_PRINT: every option, on or off,
// as zsh's ksh-style listing shows it.
void print_ksh_style(const OptionState& opts) {
  for (const auto& info : OPTION_INFO) {
    bool shown_on = opts.get(info.index) != opts.default_of(info);
    s::printf("%-22s%s\n", display_name(info, opts).c_str(), shown_on ? "on" : "off");
  }
}

// `set +o`: commands that restore the current options.
void print_restorable(const OptionState& opts) {
  for (const auto& info : OPTION_INFO) {
    bool shown_on = opts.get(info.index) != opts.default_of(info);
    s::printf("set %co %s\n", shown_on ? '-' : '+', display_name(info, opts).c_str());
  }
}

// Sets option `info` to `on` (already accounting for a `no` prefix).
bool set_option(strv cmd, const OptionInfo& info, bool on, OptionState& opts,
                strv shown) {
  if (opts.get(info.index) == on) return true;
  // An interactive shell can't be made NO_EXEC (zsh's dosetopt).
  if (not can_change(info, opts) or (info.index == OPT_EXEC and not on and opts.interactive())) {
    s::fprintf(stderr, "jfsh: %.*s: can't change option: %.*s\n",
               static_cast<int>(cmd.size()), cmd.data(),
               static_cast<int>(shown.size()), shown.data());
    return false;
  }
  opts.assign(info.index, on);
  return true;
}

// A single-letter option: `set -e`, `setopt -x`.
bool set_letter(strv cmd, char c, bool on, OptionState& opts) {
  auto l = opts.letter(c);
  if (not l) {
    s::fprintf(stderr, "jfsh: %.*s: bad option: %c%c\n", static_cast<int>(cmd.size()),
               cmd.data(), on ? '-' : '+', c);
    return false;
  }
  char shown[3] = {on ? '-' : '+', c, '\0'};
  return set_option(cmd, OPTION_INFO[l->index], on != l->negated, opts, shown);
}

// A named option: `extendedglob`, `NO_EXTENDED_GLOB`.
bool set_named(strv cmd, strv name, bool on, OptionState& opts) {
  bool negated = false;
  auto info = OptionState::find(name, &negated);
  if (not info) {
    s::fprintf(stderr, "jfsh: %.*s: no such option: %.*s\n", static_cast<int>(cmd.size()),
               cmd.data(), static_cast<int>(name.size()), name.data());
    return false;
  }
  return set_option(cmd, info->get(), on != negated, opts, name);
}

// Every option whose name matches `pattern` (`setopt -m 'hist*'`), with
// zsh's normalization: case and underscores don't matter.
bool set_matching(strv cmd, strv pattern, bool on, OptionState& opts) {
  str pat;
  for (char c : pattern)
    if (c != '_') pat += static_cast<char>(s::tolower(static_cast<unsigned char>(c)));
  auto compiled = CompiledPattern::compile(pat);
  bool ok = true;
  for (const auto& info : OPTION_INFO)
    if (compiled.matches(info.name) and can_change(info, opts))
      ok = set_option(cmd, info, on, opts, info.name) and ok;
  return ok;
}

// setopt/unsetopt: `on` is what a plain name does.
int setopt_common(vec<str>& args, ExecContext& ctx, bool on) {
  strv cmd = args[0];
  if (args.size() == 1) {
    if (ctx.opts.get(OPT_KSH_OPTION_PRINT)) {
      print_ksh_style(ctx.opts);
      return 0;
    }
    // The options whose state differs from the default (setopt), or the
    // ones that are as they are by default (unsetopt), in `no` form for
    // the ones on by default.
    for (const auto& info : OPTION_INFO) {
      bool differs = ctx.opts.get(info.index) != ctx.opts.default_of(info);
      if (differs == on) s::printf("%s\n", display_name(info, ctx.opts).c_str());
    }
    return 0;
  }
  int rc = 0;
  bool match = false;
  for (s::size_t i = 1; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "--") { match = false; continue; }
    if (a.size() > 1 and (a[0] == '-' or a[0] == '+')) {
      bool flag_on = (a[0] == '-') == on;
      for (s::size_t k = 1; k < a.size(); ++k) {
        char c = a[k];
        if (c == 'm') { match = true; continue; }
        if (c == 'o') {
          if (i + 1 >= args.size())
            return builtin_error(cmd, "string expected after -o");
          if (not set_named(cmd, args[++i], flag_on, ctx.opts)) rc = 1;
          break;
        }
        if (not set_letter(cmd, c, flag_on, ctx.opts)) rc = 1;
      }
      continue;
    }
    bool ok = match ? set_matching(cmd, a, on, ctx.opts) : set_named(cmd, a, on, ctx.opts);
    if (not ok) rc = 1;
  }
  return rc;
}

}  // namespace

int builtin_setopt(vec<str>& args, ExecContext& ctx)   { return setopt_common(args, ctx, true); }
int builtin_unsetopt(vec<str>& args, ExecContext& ctx) { return setopt_common(args, ctx, false); }

// set [±options] [±o name] [±A name] [-s] [--] [arg …]: with no arguments
// lists every parameter; otherwise sets options and, when arguments (or
// `--`) follow, the positional parameters (or with -A an array).
int builtin_set(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  if (args.size() == 1) {
    for (const Param* p : sorted_params(ctx.params, [](const Param& p) { return p.is_set(); })) {
      // As printparamnode: the name alone for a hidden value or a mark.
      if (p->flags & (PM_HIDEVAL | PM_AUTOLOAD)) s::printf("%s\n", shell_quote(p->name).c_str());
      else s::printf("%s=%s\n", shell_quote(p->name).c_str(), param_value_text(*p).c_str());
    }
    return 0;
  }
  bool sort = false, reverse = false, positional_given = false, failed = false;
  bool plus_seen = false;   // zsh's hadplus
  opt<str> array_name;
  bool array_merge = false;
  s::size_t i = 1;
  for (; i < args.size() and not array_name; ++i) {
    const str& a = args[i];
    if (a == "--") { ++i; positional_given = true; break; }
    if (a == "-" or a == "+") {
      // `set -`: the end of the options, and (but under zsh emulation)
      // off with -x and -v, as in sh.
      if (ctx.opts.emulation() != OptionState::Emulation::ZSH) {
        ctx.opts.unset(OPT_XTRACE);
        ctx.opts.unset(OPT_VERBOSE);
      }
      ++i;
      positional_given = true;
      break;
    }
    if (a.size() < 2 or (a[0] != '-' and a[0] != '+')) break;
    bool on = a[0] == '-';
    plus_seen = plus_seen or not on;
    for (s::size_t k = 1; k < a.size(); ++k) {
      char c = a[k];
      if (c == 'o') {
        str name = a.substr(k + 1);
        if (name.empty()) {
          if (i + 1 >= args.size()) {
            // As printoptionstates(hadplus): a `+` anywhere asks for
            // the restorable form.  An earlier error still ends the line,
            // with status 0.
            if (plus_seen) print_restorable(ctx.opts);
            else           print_ksh_style(ctx.opts);
            if (failed) ctx.aborting = true;
            return 0;
          }
          name = args[++i];
        }
        if (not set_named(cmd, name, on, ctx.opts)) failed = true;
        break;
      }
      if (c == 'A') {
        str name = a.substr(k + 1);
        if (name.empty()) {
          if (i + 1 >= args.size()) {
            // `set -A`: list the arrays (`set +A` their names).
            for (const Param* p : sorted_params(ctx.params,
                                                [](const Param& p) { return p.is_array(); })) {
              if (plus_seen) s::printf("%s\n", shell_quote(p->name).c_str());
              else           s::printf("%s=%s\n", shell_quote(p->name).c_str(),
                                       param_value_text(*p).c_str());
            }
            if (failed) ctx.aborting = true;
            return 0;
          }
          name = args[++i];
        }
        array_name  = name;
        array_merge = not on;
        break;
      }
      if (c == 's') { sort = true; reverse = not on; continue; }
      if (not set_letter(cmd, c, on, ctx.opts)) failed = true;
    }
  }
  // As zsh's bin_set, whose errors are zerrnam's: the command line ends
  // (once every option was tried).
  if (failed) {
    ctx.aborting = true;
    return 1;
  }
  vec<str> rest(args.begin() + static_cast<s::ptrdiff_t>(i), args.end());
  if (rest.empty() and not positional_given and not array_name) return 0;
  if (sort) {
    s::sort(rest.begin(), rest.end());
    if (reverse) s::reverse(rest.begin(), rest.end());
  }
  if (array_name) {
    if (array_merge) {
      // `set +A name …`: replaces the first elements, keeps the rest.
      vec<str> old;
      if (auto p = ctx.params.lookup(*array_name); p and p->get().is_array()) old = p->get().array();
      for (s::size_t k = 0; k < rest.size(); ++k) {
        if (k < old.size()) old[k] = rest[k];
        else                old.push_back(rest[k]);
      }
      rest = s::move(old);
    }
    // zsh's setaparam fails as an assignment does: no builtin's name, and
    // the command line ends, but bin_set still returns 0.
    if (auto r = ctx.params.set_array(*array_name, s::move(rest)); not r) {
      s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
      ctx.aborting = true;
    }
    return 0;
  }
  ctx.positional = s::move(rest);
  return 0;
}

// emulate [-lLR] [zsh|sh|ksh|csh|bash] [-c cmd]: with no mode prints the
// current one, or (-l) a sourceable restoration script.  -L makes the
// change local to the calling function; -R resets every non-special
// option; -c runs `cmd` under the emulation and puts the options back.
int builtin_emulate(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  bool local = false;
  bool full  = false;
  bool list  = false;
  s::size_t i = 1;
  for (; i < args.size() and args[i].size() > 1 and args[i][0] == '-'; ++i) {
    bool done = false;
    for (char c : strv(args[i]).substr(1)) {
      if      (c == 'L') local = true;
      else if (c == 'R') full  = true;
      else if (c == 'l') list  = true;
      else if (c == 'c') { done = true; break; }
      else {
        s::fprintf(stderr, "jfsh: emulate: bad option: -%c\n", c);
        return 1;
      }
    }
    if (done) break;
  }
  static const char* const NAMES[] = {"zsh", "sh", "ksh", "csh"};
  if (i >= args.size()) {
    if (list) {
      // Print a sourceable `emulate -R MODE\nsetopt …\nunsetopt …` that
      // restores the current emulation + option state.
      s::printf("emulate -R %s\n", NAMES[static_cast<int>(ctx.opts.emulation())]);
      for (const auto& info : OPTION_INFO) {
        if (info.special) continue;
        bool now = ctx.opts.get(info.index);
        bool def = ctx.opts.default_of(info);
        if (now == def) continue;
        s::printf("%s %s\n", now ? "setopt" : "unsetopt", info.name);
      }
    } else {
      s::printf("%s\n", NAMES[static_cast<int>(ctx.opts.emulation())]);
    }
    return 0;
  }
  const str& mode = args[i++];
  OptionState::Emulation e;
  if (mode == "zsh")                       e = OptionState::Emulation::ZSH;
  else if (mode == "sh" or mode == "bash") e = OptionState::Emulation::SH;
  else if (mode == "ksh")                  e = OptionState::Emulation::KSH;
  else if (mode == "csh")                  e = OptionState::Emulation::CSH;
  else return builtin_error(cmd, "unknown emulation: " + mode);
  // After the mode: options set on top of it (`-o name`, `+o name`, `-x`)
  // and `-c cmd`, which must come last.
  opt<str> run;
  struct Change { bool on; bool named; str name; };
  vec<Change> changes;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (run or a.size() < 2 or (a[0] != '-' and a[0] != '+'))
      return builtin_error(cmd, "unknown argument " + a);
    bool on = a[0] == '-';
    for (s::size_t k = 1; k < a.size(); ++k) {
      char c = a[k];
      if (c == 'c' and on) {
        if (k + 1 < a.size() or i + 1 >= args.size())
          return builtin_error(cmd, "argument expected: -c");
        run = args[++i];
      } else if (c == 'o') {
        if (k + 1 < a.size()) {
          changes.push_back({on, true, a.substr(k + 1)});
        } else if (i + 1 < args.size()) {
          changes.push_back({on, true, args[++i]});
        } else {
          return builtin_error(cmd, str("argument expected: ") + a[0] + "o");
        }
        break;
      } else {
        changes.push_back({on, false, str(1, c)});
      }
    }
  }

  // `emulate -l MODE` lists what `-R MODE` would do, without changing
  // the live state.
  if (list and not run) {
    OptionState probe = ctx.opts;
    probe.apply_emulation(e, full);
    s::printf("emulate -R %s\n", NAMES[static_cast<int>(e)]);
    for (const auto& info : OPTION_INFO) {
      if (info.special) continue;
      bool target = probe.get(info.index);
      bool here   = ctx.opts.get(info.index);
      if (target == here) continue;
      s::printf("%s %s\n", target ? "setopt" : "unsetopt", info.name);
    }
    return 0;
  }

  OptionState saved = ctx.opts;
  ctx.opts.apply_emulation(e, full);
  for (const auto& c : changes) {
    bool ok = c.named ? set_named(cmd, c.name, c.on, ctx.opts)
                      : set_letter(cmd, c.name[0], c.on, ctx.opts);
    if (not ok) {
      ctx.opts = s::move(saved);
      return 1;
    }
  }
  if (run) {
    int rc = ctx.executor->exec_string(*run);
    ctx.opts = s::move(saved);
    return rc;
  }
  if (local) {
    ctx.opts.set(OPT_LOCALOPTIONS);
    ctx.opts.set(OPT_LOCALTRAPS);
    ctx.opts.set(OPT_LOCALPATTERNS);
  }
  return 0;
}

// getopts optstring name [arg …]: the next option of the args (or the
// positional parameters) into `name`, as zsh's bin_getopts: `$OPTIND` is
// the argument being read, a position inside it is kept for clustered
// options; `+x` gives `+x`.  A leading `:` in optstring reports errors
// through `name` (`?`, `:`) and $OPTARG instead of a message.
int builtin_getopts(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  if (args.size() < 3) return builtin_error(cmd, "not enough arguments");
  strv optstr = args[1];
  const str& var = args[2];
  bool quiet = not optstr.empty() and optstr[0] == ':';
  if (quiet) optstr.remove_prefix(1);
  vec<str> words = args.size() > 3 ? vec<str>(args.begin() + 3, args.end()) : ctx.positional;
  if (ctx.optind < 1) {
    ctx.optind  = 1;
    ctx.optcind = 0;
  }
  auto arg = [&](s::int64_t i) -> const str* {
    return i >= 1 and static_cast<s::size_t>(i) <= words.size()
               ? &words[static_cast<s::size_t>(i - 1)] : nullptr;
  };
  const str* word = arg(ctx.optind);
  if (not word or word->empty()) return 1;
  if (ctx.optcind >= word->size()) {
    ctx.optcind = 0;
    if (not arg(ctx.optind++)) return 1;
    word = arg(ctx.optind);
    if (not word) return 1;
  }
  if (ctx.optcind == 0) {
    if (word->size() < 2 or ((*word)[0] != '-' and (*word)[0] != '+')) return 1;
    if (*word == "--") {
      ++ctx.optind;
      return 1;
    }
    ++ctx.optcind;
  }
  char ch = (*word)[ctx.optcind++];
  str opt = (*word)[0] == '+' ? str{'+', ch} : str(1, ch);
  auto set = [&](str v) { (void)ctx.params.set_scalar(var, s::move(v)); };
  auto fail = [&](char kind) {
    set(quiet ? str(1, kind) : str("?"));
    if (quiet) {
      ctx.optarg = opt;
    } else {
      str shown{(*word)[0] == '+' ? '+' : '-', ch};
      builtin_error(cmd, kind == '?' ? "bad option: " + shown
                                     : "argument expected after " + shown + " option");
      ctx.optarg.clear();
    }
    return 0;
  };
  auto at = optstr.find(ch);
  if (ch == ':' or at == strv::npos) return fail('?');
  if (at + 1 < optstr.size() and optstr[at + 1] == ':') {
    if (ctx.optcind == word->size()) {
      const str* next = arg(ctx.optind + 1);
      if (not next) return fail(':');
      ctx.optarg = *next;
      ++ctx.optind;
    } else {
      ctx.optarg = word->substr(ctx.optcind);
    }
    ctx.optcind = 0;
    ++ctx.optind;
  } else {
    ctx.optarg.clear();
  }
  set(opt);
  return 0;
}

// shift [-p] [n] [array …]: drops the first n positional parameters (or
// array elements); -p drops the last ones.
int builtin_shift(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  bool from_end = false;
  s::size_t i = 1;
  if (i < args.size() and args[i] == "-p") { from_end = true; ++i; }
  if (i < args.size() and args[i] == "--") ++i;
  s::int64_t n = 1;
  // The count, unless the word names an array (`shift arr`).
  auto names_array = [&](const str& w) {
    auto p = ctx.params.lookup(w);
    return is_identifier(w) and p and p->get().is_array();
  };
  if (i < args.size() and not names_array(args[i])) {
    auto v = arith_eval(args[i], ctx.params, math_options(ctx),
                        &ctx.modules.math_funcs());
    if (not v) return builtin_error(cmd, v.error().message);
    n = *v;
    ++i;
  }
  if (n < 0) return builtin_error(cmd, "argument to shift must be non-negative");
  auto drop = [&](vec<str>& v) -> bool {
    if (static_cast<s::size_t>(n) > v.size()) {
      builtin_error(cmd, "shift count must be <= $#");
      return false;
    }
    if (from_end) v.erase(v.end() - n, v.end());
    else          v.erase(v.begin(), v.begin() + n);
    return true;
  };
  if (i >= args.size()) return drop(ctx.positional) ? 0 : 1;
  for (; i < args.size(); ++i) {
    auto p = ctx.params.lookup(args[i]);
    if (not p or not p->get().is_array()) continue;
    auto v = p->get().array();
    if (not drop(v)) return 1;
    if (auto r = ctx.params.set_array(args[i], s::move(v)); not r)
      return builtin_error(cmd, r.error().message);
  }
  return 0;
}

}  // namespace jfsh
