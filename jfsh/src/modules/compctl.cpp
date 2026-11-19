#include <jfsh/modules/compctl.h>

#include <cstdio>

#include <algorithm>
#include <map>

#include <jfsh/core/alias_table.h>
#include <jfsh/core/aliases.h>
#include <jfsh/core/options.h>
#include <jfsh/editor/completion.h>
#include <jfsh/editor/zle.h>
#include <jfsh/exec/executor.h>
#include <jfsh/glob/glob.h>
#include <jfsh/modules/support.h>

// zsh/compctl (M11): the old `compctl` completion control, as zsh's
// Src/Zle/compctl.c, reduced to what legacy plugins use.  Each spec is
// handed to the completion engine as a C++ completer for its commands.

namespace jfsh {

namespace {

int err(strv msg) { return mod_error("compctl", msg); }

struct Spec {
  str  letters;        // the flags given, in order (-f -/ -c …), for -L
  str  keys;           // -k: an array name, or a literal `(a b c)` list
  str  glob;           // -g
  str  func;           // -K
  str  prefix, suffix; // -P / -S
  str  explanation;    // -X (kept for -L; jfsh doesn't display it)
  bool has(char c) const { return letters.find(c) != str::npos; }
};

struct CompctlState {
  s::map<str, Spec> specs;   // command → spec; -C/-D/-T as "-C" … keys
};

CompctlState& state() {
  static CompctlState st;
  return st;
}

vec<str> words_of(ExecContext& ctx, const str& spec) {
  if (spec.size() >= 2 and spec.front() == '(' and spec.back() == ')') {
    vec<str> out;
    str cur;
    for (char c : spec.substr(1, spec.size() - 2)) {
      if (c == ' ' or c == '\t' or c == '\n') {
        if (not cur.empty()) out.push_back(s::move(cur));
        cur.clear();
      } else {
        cur += c;
      }
    }
    if (not cur.empty()) out.push_back(s::move(cur));
    return out;
  }
  auto p = ctx.params.lookup(spec);
  if (not p or not p->get().is_set()) return {};
  return p->get().is_array() ? p->get().array() : vec<str>{p->get().scalar()};
}

str quote(const str& w) {
  if (not w.empty() and w.find_first_of(" \t'\"\\$()") == str::npos) return w;
  str out = "'";
  for (char c : w) out += c == '\'' ? str("'\\''") : str(1, c);
  return out + "'";
}

vec<CompCandidate> complete(ExecContext& ctx, CompletionEngine& eng, const Spec& sp,
                            const CompContext& cc) {
  vec<CompCandidate> out;
  const str& pre = cc.prefix;
  auto add = [&](const str& w) {
    if (strv(w).starts_with(pre)) {
      CompCandidate c;
      c.word = sp.prefix + w + sp.suffix;
      out.push_back(s::move(c));
    }
  };
  if (sp.has('f')) for (auto& c : eng.complete_files(pre)) out.push_back(s::move(c));
  if (sp.has('/')) for (auto& c : eng.complete_files(pre, true)) out.push_back(s::move(c));
  if (sp.has('c')) for (auto& c : eng.complete_commands(pre)) out.push_back(s::move(c));
  if (sp.has('u')) for (auto& c : eng.complete_users(pre)) out.push_back(s::move(c));
  if (sp.has('v') or sp.has('A') or sp.has('e')) {
    ctx.params.for_each([&](const Param& p) {
      if (sp.has('v') or (sp.has('A') and (p.is_array() or p.is_hash()))
          or (sp.has('e') and p.is_exported()))
        add(p.name);
    });
  }
  if (sp.has('a')) for (const auto& [name, _] : ctx.aliases.regular) add(name);
  if (sp.has('F')) for (const auto& [name, _] : ctx.functions) add(name);
  if (sp.has('B') and ctx.executor) for (const auto& n : ctx.executor->builtin_names()) add(n);
  if (sp.has('o')) for (const auto& o : OPTION_INFO) add(o.name);
  if (sp.has('b') and ctx.zle) {
    for (const auto& w : Zle::builtin_widgets()) add(w);
    for (const auto& [w, _] : ctx.zle->user_widgets) add(w);
  }
  if (not sp.keys.empty()) for (const auto& w : words_of(ctx, sp.keys)) add(w);
  if (not sp.glob.empty())
    for (auto& c : eng.complete_files(pre, false, false, sp.glob)) out.push_back(s::move(c));
  if (not sp.func.empty() and ctx.executor) {
    // -K: the function gets the word's prefix and suffix and sets $reply.
    (void)ctx.params.unset("reply");
    ctx.executor->exec_string(quote(sp.func) + " " + quote(pre) + " " + quote(cc.suffix));
    if (auto r = ctx.params.lookup("reply"); r and r->get().is_set())
      for (const auto& w : r->get().is_array() ? r->get().array() : vec<str>{r->get().scalar()})
        add(w);
  }
  CompletionEngine::tidy(out);
  return out;
}


void print_spec(const str& cmd, const Spec& sp) {
  str line = "compctl";
  for (char c : sp.letters) {
    switch (c) {
      case 'k': line += " -k " + quote(sp.keys); break;
      case 'g': line += " -g " + quote(sp.glob); break;
      case 'K': line += " -K " + quote(sp.func); break;
      case 'P': line += " -P " + quote(sp.prefix); break;
      case 'S': line += " -S " + quote(sp.suffix); break;
      case 'X': line += " -X " + quote(sp.explanation); break;
      default:  line += str(" -") + c;
    }
  }
  if (cmd == "-C" or cmd == "-D" or cmd == "-T") line.insert(7, " " + cmd);
  else line += " " + quote(cmd);
  s::printf("%s\n", line.c_str());
}

void install(ExecContext& ctx, const str& cmd, Spec sp) {
  state().specs[cmd] = sp;
  if (not ctx.zle or not ctx.zle->completion or cmd.starts_with('-')) return;
  ExecContext* ctxp = &ctx;
  CompletionEngine* eng = ctx.zle->completion;
  eng->compdef_fn(cmd, [ctxp, eng, sp](const CompContext& cc) { return complete(*ctxp, *eng, sp, cc); });
}

void remove(ExecContext& ctx, const str& cmd) {
  state().specs.erase(cmd);
  if (ctx.zle and ctx.zle->completion) ctx.zle->completion->compdef_fn_remove(cmd);
}

int builtin_compctl(vec<str>& args, ExecContext& ctx) {
  // `compctl + cmd …`: delete.
  if (args.size() >= 2 and args[1] == "+") {
    for (s::size_t i = 2; i < args.size(); ++i) remove(ctx, args[i]);
    return 0;
  }
  Spec sp;
  bool list = false;
  str special;   // -C, -D or -T
  s::size_t i = 1;
  // Flags that take an argument, joined (-kfoo) or as the next word.
  static constexpr strv kWithArg = "kgsKXPSWHJVMylhiQ";
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "--") { ++i; break; }
    if (a == "+") return err("unsupported option: + (alternative completion)");
    if (a.size() < 2 or a[0] != '-') break;
    for (s::size_t k = 1; k < a.size(); ++k) {
      char c = a[k];
      if (c == 'x') return err("unsupported option: -x (extended completion)");
      if (c == 'L') { list = true; continue; }
      if (c == 'C' or c == 'D' or c == 'T') { special = str("-") + c; continue; }
      if (kWithArg.find(c) != strv::npos) {
        str v = a.substr(k + 1);
        if (v.empty()) {
          if (++i >= args.size()) return err(str("-") + c + ": argument expected");
          v = args[i];
        }
        k = a.size();
        switch (c) {
          case 'k': sp.keys = v; break;
          case 'g': sp.glob = v; break;
          case 'K': sp.func = v; break;
          case 'P': sp.prefix = v; break;
          case 'S': sp.suffix = v; break;
          case 'X': sp.explanation = v; break;
          default:  continue;   // parsed, not used by jfsh
        }
        sp.letters += c;
        continue;
      }
      static constexpr strv kPlain = "f/cvuaAeFBobqUjmwrRzEIdnpZl1";
      if (kPlain.find(c) == strv::npos) return err(str("unsupported option: -") + c);
      sp.letters += c;
    }
  }
  vec<str> cmds(args.begin() + static_cast<s::ptrdiff_t>(i), args.end());
  if (not special.empty()) cmds.insert(cmds.begin(), special);
  if (list) {
    for (const auto& [cmd, spec] : state().specs)
      if (cmds.empty() or s::find(cmds.begin(), cmds.end(), cmd) != cmds.end())
        print_spec(cmd, spec);
    return 0;
  }
  if (cmds.empty()) {
    if (sp.letters.empty()) {
      for (const auto& [cmd, spec] : state().specs) print_spec(cmd, spec);
      return 0;
    }
    return err("command name required");
  }
  for (const auto& c : cmds) install(ctx, c, sp);
  return 0;
}

}  // namespace

void register_compctl_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/compctl", [](ModuleFeatures& out, ExecContext& ctx) {
    ExecContext* ctxp = &ctx;
    state() = CompctlState{};
    out.builtins.push_back(BuiltinDef{"compctl", builtin_compctl, BINF_NONE, 0});
    out.on_unload = [ctxp] {
      auto specs = state().specs;
      for (const auto& [cmd, _] : specs) remove(*ctxp, cmd);
    };
    out.feature_names = {"b:compctl"};
  });
}

}  // namespace jfsh
