// Parameter builtins: typeset and its variants (local, declare, integer,
// float, export, readonly), and unset.

#include <cctype>
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

// Attributes typeset can give a parameter, from its option letters.
struct TypesetAttrs {
  char        type   = 0;   // 'a', 'A', 'i' or 'F'; 0 for scalar
  bool        global = false;
  bool        print  = false;   // -p
  bool        names_only = false;   // a bare `+`
  bool        pattern = false;  // -m
  bool        functions = false;   // -f
  s::uint32_t on     = 0;   // PM_EXPORTED, PM_READONLY, PM_UPPER, PM_LOWER, …
  s::uint32_t off    = 0;
};

str fold_case(str value, s::uint32_t flags) {
  if (flags & PM_UPPER)
    for (char& c : value) c = static_cast<char>(s::toupper(static_cast<unsigned char>(c)));
  if (flags & PM_LOWER)
    for (char& c : value) c = static_cast<char>(s::tolower(static_cast<unsigned char>(c)));
  return value;
}

vec<str> unique(vec<str> words) {
  vec<str> out;
  for (auto& w : words)
    if (s::find(out.begin(), out.end(), w) == out.end()) out.push_back(s::move(w));
  return out;
}

// Declares or updates one `name` / `name=value` argument of typeset.
int typeset_one(strv cmd, strv arg, const TypesetAttrs& attrs, ExecContext& ctx) {
  s::size_t eq = arg.find('=');
  bool has_value = eq != strv::npos;
  bool append    = has_value and eq > 0 and arg[eq - 1] == '+';
  str  name(arg.substr(0, has_value ? eq - (append ? 1 : 0) : arg.size()));
  str  value = has_value ? str(arg.substr(eq + 1)) : str{};
  if (not is_identifier(name)) {
    s::fprintf(stderr, "jfsh: %.*s: not valid in this context: %s\n",
               static_cast<int>(cmd.size()), cmd.data(), name.c_str());
    return 1;
  }
  auto existing = attrs.global ? ctx.params.lookup(name) : ctx.params.lookup_local(name);
  if (existing and existing->get().is_readonly()
      and (has_value or attrs.type or (attrs.off & PM_READONLY))) {
    s::fprintf(stderr, "jfsh: read-only variable: %s\n", name.c_str());
    return 1;
  }
  Param* p = existing ? &existing->get() : nullptr;
  if (not p) {
    // A new local keeps the export attribute of the parameter it hides.
    auto outer = ctx.params.lookup(name);
    bool exported = outer and outer->get().is_exported();
    if (attrs.global) {
      (void)ctx.params.set_scalar(name, "");
      p = &ctx.params.lookup(name)->get();
    } else {
      p = &ctx.params.declare_local(name, PM_SCALAR | (ctx.func_depth ? +PM_LOCAL : 0));
      p->set(str{});
    }
    if (exported) p->flags |= PM_EXPORTED;
    if (attrs.type == 'i' and not has_value) p->set(s::int64_t{0});
    if (attrs.type == 'F' and not has_value) p->set(0.0);
  } else if (not p->is_set() and not has_value) {
    // `local x` after `unset x` sets it again, empty.
    p->set(str{});
  }
  if (append and has_value) value = p->scalar() + value;
  s::uint32_t flags = (p->flags | attrs.on) & ~attrs.off;
  value = fold_case(s::move(value), flags);
  auto math = [&](strv text) -> opt<MathNum> {
    auto v = math_eval(text, ctx.params, math_options(ctx.opts));
    if (not v) {
      s::fprintf(stderr, "jfsh: %.*s: %s\n", static_cast<int>(cmd.size()), cmd.data(),
                 v.error().message.c_str());
      return s::nullopt;
    }
    return *v;
  };
  switch (attrs.type) {
    case 'a':
      if (has_value) p->set(vec<str>{value});
      else if (not p->is_array()) p->set(p->is_set() and not p->scalar().empty()
                                             ? vec<str>{p->scalar()} : vec<str>{});
      break;
    case 'A':
      if (not p->is_hash()) p->set(umap<str, str>{});
      break;
    case 'i':
    case 'F':
      if (has_value or (attrs.type == 'i' ? not p->is_integer() : not p->is_float())) {
        auto v = math(has_value ? value : p->scalar());
        if (not v) return 1;
        if (attrs.type == 'i') p->set(v->as_int());
        else                   p->set(v->as_float());
      }
      break;
    default:
      if (has_value) {
        if (p->is_integer() or p->is_float()) {
          auto v = math(value);
          if (not v) return 1;
          if (p->is_integer()) p->set(v->as_int());
          else                 p->set(v->as_float());
        } else if (p->is_array() or p->is_hash()) {
          s::fprintf(stderr, "jfsh: %s: inconsistent type for assignment\n", name.c_str());
          return 1;
        } else {
          p->set(s::move(value));
        }
      } else if (p->is_scalar() and (attrs.on & (PM_UPPER | PM_LOWER))) {
        p->set(fold_case(p->scalar(), flags));
      }
  }
  p->flags = (p->flags | attrs.on) & ~attrs.off;
  if ((p->flags & PM_UNIQUE) and p->is_array()) p->set(unique(p->array()));
  return 0;
}

// `typeset -p` form: `typeset -i n=3`, `export X=1`.
str typeset_line(const Param& p) {
  str flags = param_type_flags(p);
  str head  = flags == "-x" ? str("export") : "typeset" + (flags.empty() ? str{} : " " + flags);
  if (not p.is_set()) return head + " " + p.name;
  return head + " " + p.name + "=" + param_value_text(p);
}

// The type words `typeset` and `typeset +` print in front of a name.
str type_words(const Param& p) {
  str out;
  auto add = [&](strv w) { out += w; out += ' '; };
  if (p.is_array())   add("array");
  if (p.is_hash())    add("association");
  if (p.is_integer()) add("integer");
  if (p.is_float())   add("float");
  if (p.flags & PM_LOCAL) add("local");
  if (p.is_exported()) add("exported");
  if (p.is_readonly()) add("readonly");
  return out;
}

// Whether `p` has every attribute the options asked for (`typeset -i`
// lists the integers).
bool has_attrs(const Param& p, const TypesetAttrs& a) {
  switch (a.type) {
    case 'a': if (not p.is_array())   return false; break;
    case 'A': if (not p.is_hash())    return false; break;
    case 'i': if (not p.is_integer()) return false; break;
    case 'F': if (not p.is_float())   return false; break;
    default: ;
  }
  return (p.flags & a.on) == a.on;
}

int typeset_list(const TypesetAttrs& attrs, const vec<str>& patterns, ExecContext& ctx) {
  vec<CompiledPattern> pats;
  for (const auto& pat : patterns) pats.push_back(CompiledPattern::compile(pat));
  auto params = sorted_params(ctx.params, [&](const Param& p) {
    if (not p.is_set() and not attrs.print) return false;
    if (not has_attrs(p, attrs)) return false;
    if (pats.empty()) return true;
    return s::any_of(pats.begin(), pats.end(),
                     [&](const CompiledPattern& c) { return c.matches(p.name); });
  });
  for (const Param* p : params) {
    if (attrs.print)            s::printf("%s\n", typeset_line(*p).c_str());
    else if (attrs.names_only)  s::printf("%s%s\n", type_words(*p).c_str(), shell_quote(p->name).c_str());
    else if (attrs.on or attrs.type)
      s::printf("%s=%s\n", shell_quote(p->name).c_str(), param_value_text(*p).c_str());
    else
      s::printf("%s%s=%s\n", type_words(*p).c_str(), shell_quote(p->name).c_str(),
                param_value_text(*p).c_str());
  }
  return 0;
}

}  // namespace

// typeset [±aAEFfgilmprUux] [name[=value] …].  Without -g a name is
// declared in the innermost scope, which inside a function is local to it.
// A new parameter is set and empty (0 for -i/-F, () for -a/-A).  `implied`
// holds the options the variant stands for (export is `typeset -gx`).
// With no names it lists the parameters that have the attributes given.
// TODO(builtin/typeset): `-T`, `-Z`/`-L`/`-R` padding, `-h`, `-t`.
int typeset_common(vec<str>& args, ExecContext& ctx, strv implied) {
  strv cmd = args[0];
  TypesetAttrs attrs;
  auto apply = [&](char c, bool on) {
    switch (c) {
      case 'a': case 'A': case 'i': attrs.type = on ? c : 0; return true;
      case 'F': case 'E':
        attrs.type = on ? 'F' : 0;
        if (on) (c == 'E' ? attrs.on : attrs.off) |= PM_EFLOAT;
        return true;
      case 'g': attrs.global = on; return true;
      case 'p': attrs.print = on; return true;
      case 'm': attrs.pattern = on; return true;
      case 'f': attrs.functions = on; return true;
      case 'x': (on ? attrs.on : attrs.off) |= PM_EXPORTED; return true;
      case 'r': (on ? attrs.on : attrs.off) |= PM_READONLY; return true;
      case 'u': (on ? attrs.on : attrs.off) |= PM_UPPER;    return true;
      case 'l': (on ? attrs.on : attrs.off) |= PM_LOWER;    return true;
      case 'U': (on ? attrs.on : attrs.off) |= PM_UNIQUE;   return true;
      case 'h': case 't': return true;   // accepted, no effect yet
      default:  return false;
    }
  };
  for (char c : implied) apply(c, true);
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "--") { ++i; break; }
    if (a == "+") { attrs.names_only = true; continue; }
    if (a.size() < 2 or (a[0] != '-' and a[0] != '+')) break;
    for (s::size_t k = 1; k < a.size(); ++k) {
      if (not apply(a[k], a[0] == '-')) {
        s::fprintf(stderr, "jfsh: %.*s: bad option: %c%c\n",
                   static_cast<int>(cmd.size()), cmd.data(), a[0], a[k]);
        return 1;
      }
    }
  }
  if (cmd == "local") attrs.global = false;
  vec<str> names(args.begin() + static_cast<s::ptrdiff_t>(i), args.end());

  if (attrs.functions) {
    vec<str> fargs{"functions"};
    if (attrs.pattern) fargs.emplace_back("-m");
    fargs.insert(fargs.end(), names.begin(), names.end());
    return builtin_functions(fargs, ctx);
  }
  if (names.empty() or attrs.pattern) {
    // `export -p` lists the exported parameters as `export X=…`; plain
    // `export` lists them as `X=…`.
    return typeset_list(attrs, names, ctx);
  }
  int rc = 0;
  if (attrs.print) {
    for (const auto& name : names) {
      auto p = ctx.params.lookup(name);
      if (not p) {
        s::fprintf(stderr, "jfsh: %.*s: no such variable: %s\n",
                   static_cast<int>(cmd.size()), cmd.data(), name.c_str());
        rc = 1;
        continue;
      }
      s::printf("%s\n", typeset_line(p->get()).c_str());
    }
    return rc;
  }
  for (const auto& name : names)
    if (typeset_one(cmd, name, attrs, ctx) != 0) rc = 1;
  return rc;
}

int builtin_typeset(vec<str>& args, ExecContext& ctx) { return typeset_common(args, ctx, ""); }
int builtin_local  (vec<str>& args, ExecContext& ctx) { return typeset_common(args, ctx, ""); }
int builtin_declare(vec<str>& args, ExecContext& ctx) { return typeset_common(args, ctx, ""); }
int builtin_integer(vec<str>& args, ExecContext& ctx) { return typeset_common(args, ctx, "i"); }
// zsh's `float` is `typeset -E`.
int builtin_float  (vec<str>& args, ExecContext& ctx) { return typeset_common(args, ctx, "E"); }

// `export` is `typeset -gx`; `-n` takes the export attribute away.
int builtin_export(vec<str>& args, ExecContext& ctx) {
  for (auto& a : args)
    if (a == "-n") a = "+x";
  return typeset_common(args, ctx, "gx");
}

// `readonly` is `typeset -gr`.
int builtin_readonly(vec<str>& args, ExecContext& ctx) {
  return typeset_common(args, ctx, "gr");
}

// unset [-fmv] name …: -v (the default) removes parameters, `name[i]` or
// `name[key]` one element, -f functions, and -m takes patterns.
int builtin_unset(vec<str>& args, ExecContext& ctx) {
  OptParse o;
  if (not parse_options(args, "fmv", "", o)) return 1;
  bool funcs   = o.has('f');
  bool pattern = o.has('m');
  int  rc = 0;
  for (s::size_t i = o.next; i < args.size(); ++i) {
    const str& name = args[i];
    if (funcs) {
      vec<str> victims;
      if (pattern) {
        auto pat = CompiledPattern::compile(name);
        for (const auto& [n, body] : ctx.functions)
          if (pat.matches(n)) victims.push_back(n);
      } else if (ctx.functions.contains(name) or ctx.autoloads.contains(name)) {
        victims.push_back(name);
      } else {
        s::fprintf(stderr, "jfsh: unset: no such hash table element: %s\n", name.c_str());
        rc = 1;
        continue;
      }
      for (const auto& v : victims) {
        ctx.functions.erase(v);
        ctx.function_files.erase(v);
        ctx.autoloads.erase(v);
        // `unset -f TRAPINT` removes the trap it was.
        if (v.starts_with("TRAP"))
          if (auto sig = SignalTable::name_to_sig(strv(v).substr(4)))
            if (ctx.signals.traps().get(*sig).action == TrapAction::Function)
              ctx.signals.set_trap(*sig, TrapAction::Default);
      }
      continue;
    }
    if (pattern) {
      auto pat = CompiledPattern::compile(name);
      vec<str> victims;
      ctx.params.for_each([&](const Param& p) {
        if (pat.matches(p.name)) victims.push_back(p.name);
      });
      for (const auto& v : victims)
        if (auto r = ctx.params.unset(v); not r) {
          s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
          rc = 1;
        }
      continue;
    }
    // name[subscript]: one element of an array or associative array.
    if (auto lb = name.find('['); lb != str::npos and name.back() == ']') {
      str base = name.substr(0, lb);
      str key  = name.substr(lb + 1, name.size() - lb - 2);
      auto p = ctx.params.lookup(base);
      if (not p or not p->get().is_set()) continue;
      Param& prm = p->get();
      if (prm.is_readonly()) {
        s::fprintf(stderr, "jfsh: read-only variable: %s\n", base.c_str());
        rc = 1;
        continue;
      }
      if (prm.is_hash()) {
        auto h = prm.hash();
        h.erase(key);
        prm.set(s::move(h));
      } else if (prm.is_array()) {
        auto v = math_eval(key, ctx.params, math_options(ctx.opts));
        if (not v) {
          s::fprintf(stderr, "jfsh: unset: %s\n", v.error().message.c_str());
          rc = 1;
          continue;
        }
        auto a = prm.array();
        s::int64_t n = v->as_int();
        if (n < 0) n += static_cast<s::int64_t>(a.size()) + 1;
        if (n >= 1 and static_cast<s::size_t>(n) <= a.size()) a[static_cast<s::size_t>(n - 1)].clear();
        prm.set(s::move(a));
      }
      continue;
    }
    if (not is_identifier(name) and name != "0") {
      s::fprintf(stderr, "jfsh: unset: %s: invalid parameter name\n", name.c_str());
      rc = 1;
      continue;
    }
    if (auto r = ctx.params.unset(name); not r) {
      s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
      rc = 1;
    }
  }
  return rc;
}

}  // namespace jfsh
