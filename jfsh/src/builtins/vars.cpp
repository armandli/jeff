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
  bool        tie    = false;   // -T
  opt<int>    base{};   // -i N (output base), -F N / -E N (digits)
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
  // `name[sub]=value` sets an element of a parameter that exists in the
  // scope typeset declares in, as an assignment would.
  if (auto lb = name.find('['); lb != str::npos and lb > 0 and name.back() == ']'
                                and is_identifier(strv(name).substr(0, lb))) {
    str base = name.substr(0, lb);
    if (not attrs.global and ctx.params.local_scope() > 0 and not ctx.params.lookup_local(base))
      return builtin_error(cmd, name + ": can't create local array elements");
    if (attrs.type or not has_value)
      return builtin_error(cmd, name + ": inconsistent array element or slice assignment");
    Assignment a;
    a.name      = s::move(base);
    a.subscript = name.substr(lb + 1, name.size() - lb - 2);
    a.value     = s::move(value);
    a.append    = append;
    if (auto r = ctx.executor->assign_expanded(a); not r) {
      s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
      return 1;
    }
    return 0;
  }
  if (not is_identifier(name)) {
    bool digit = not name.empty() and s::isdigit(static_cast<unsigned char>(name[0]));
    return builtin_error(cmd, (digit ? "not an identifier: " : "not valid in this context: ")
                                  + name);
  }
  auto existing = attrs.global ? ctx.params.lookup(name) : ctx.params.lookup_local(name);
  if (existing and existing->get().is_readonly()
      and (has_value or attrs.type or (attrs.off & PM_READONLY))) {
    s::fprintf(stderr, "jfsh: read-only variable: %s\n", name.c_str());
    return 1;
  }
  Param* p = existing ? &existing->get() : nullptr;
  if (not p) {
    // A new local is not exported (unless -x), even when the parameter it
    // hides is, as in zsh: children see the local's absence.
    auto outer = ctx.params.lookup(name);
    if (attrs.global) {
      (void)ctx.params.set_scalar(name, "");
      p = &ctx.params.lookup(name)->get();
    } else {
      // A special parameter stays special in the function, as in zsh
      // (unless -h): it starts empty and has its value back at the end.
      // A view with no setter ($commands) just stays a view.
      Param* special = outer and (outer->get().flags & PM_SPECIAL) and outer->get().handlers
                           and not (attrs.on & PM_HIDE)
                       ? &outer->get() : nullptr;
      // zsh refuses a local $LINENO or $HISTCMD (its other read-only
      // specials, $status, $PPID, …, may be made local).
      if (special and special->is_readonly() and (name == "LINENO" or name == "HISTCMD")) {
        s::fprintf(stderr, "jfsh: read-only variable: %s\n", name.c_str());
        return 1;
      }
      bool settable = special and (special->handlers->set_scalar or special->handlers->set_array);
      ParamValue saved = special ? special->value : ParamValue{};
      s::uint32_t kind = special ? special->flags & (PM_SCALAR | PM_INTEGER | PM_FLOAT | PM_ARRAY
                                                     | PM_HASH)
                                 : PM_SCALAR;
      p = &ctx.params.declare_local(name, kind | (ctx.func_depth ? +PM_LOCAL : 0));
      if (special) {
        p->handlers = s::make_unique<ParamHandlers>(*special->handlers);
        p->flags |= PM_SPECIAL;
        p->value = special->value;
        if (settable) {
          p->restore = s::move(saved);
          if (p->is_array() and p->handlers->set_array) p->handlers->set_array({});
          else if (p->handlers->set_scalar)
            p->handlers->set_scalar(p->is_scalar() ? str{} : str("0"));
        }
      } else {
        p->set(str{});
      }
    }
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
    auto v = math_eval(text, ctx.params, math_options(ctx.opts),
                       &ctx.modules.math_funcs());
    if (not v) {
      s::fprintf(stderr, "jfsh: %.*s: %s\n", static_cast<int>(cmd.size()), cmd.data(),
                 v.error().message.c_str());
      return s::nullopt;
    }
    return *v;
  };
  bool was_integer = p->is_integer(), was_float = p->is_float();
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
        // A special one ($OPTIND) takes it through its setter too.
        if (has_value and p->handlers and p->handlers->set_scalar)
          p->handlers->set_scalar(p->scalar());
      }
      break;
    default:
      if (has_value) {
        if (p->is_integer() or p->is_float()) {
          auto v = math(value);
          if (not v) return 1;
          // A special one (a local $TRY_BLOCK_ERROR) takes it through its
          // setter, which its getter reads back.
          if (p->handlers and p->handlers->set_scalar)
            p->handlers->set_scalar(p->is_integer() ? s::to_string(v->as_int()) : p->scalar());
          if (p->is_integer()) p->set(v->as_int());
          else                 p->set(v->as_float());
        } else if (p->is_array() or p->is_hash()) {
          s::fprintf(stderr, "jfsh: %s: inconsistent type for assignment\n", name.c_str());
          return 1;
        } else if (p->handlers and p->handlers->set_scalar) {
          p->handlers->set_scalar(s::move(value));
        } else {
          p->set(s::move(value));
        }
      } else if (p->is_scalar() and (attrs.on & (PM_UPPER | PM_LOWER))) {
        p->set(fold_case(p->scalar(), flags));
      }
  }
  p->flags = (p->flags | attrs.on) & ~attrs.off;
  // The base or digits given stay until another is; a parameter that
  // becomes an integer or a float starts from the default.
  if (attrs.base) p->base = *attrs.base;
  else if ((attrs.type == 'i' and not was_integer) or (attrs.type == 'F' and not was_float))
    p->base = 0;
  if ((p->flags & PM_UNIQUE) and p->is_array()) p->set(unique(p->array()));
  if (has_value) ctx.params.note_assigned(name);
  return 0;
}

// `typeset -p` form: `typeset -i n=3`, `export X=1`.
// `global`: shown from a function, a parameter from outside it gets -g.
str typeset_line(const Param& p, bool global) {
  str flags = param_type_flags(p);
  str head  = flags == "-x" ? str("export")
            : "typeset" + str(global ? " -g" : "") + (flags.empty() ? str{} : " " + flags);
  if (not p.is_set() or (p.flags & PM_HIDEVAL)) return head + " " + p.name;
  return head + " " + p.name + "=" + param_value_text(p);
}

// The type words `typeset` and `typeset +` print in front of a name.
str type_words(const Param& p) {
  str out;
  auto add = [&](strv w) { out += w; out += ' '; };
  if (p.is_array())   add("array");
  if (p.is_hash())    add("association");
  if (p.is_integer()) add(p.base > 0 ? "integer " + s::to_string(p.base) : str("integer"));
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
    if (attrs.print)
      s::printf("%s\n", typeset_line(*p, p->scope < ctx.params.local_scope()).c_str());
    else if (attrs.names_only)
      s::printf("%s%s\n", attrs.on or attrs.type ? "" : type_words(*p).c_str(),
                shell_quote(p->name).c_str());
    else if (p->flags & PM_HIDEVAL)
      s::printf("%s%s\n", attrs.on or attrs.type ? "" : type_words(*p).c_str(),
                shell_quote(p->name).c_str());
    else if (attrs.on or attrs.type)
      s::printf("%s=%s\n", shell_quote(p->name).c_str(), param_value_text(*p).c_str());
    else
      s::printf("%s%s=%s\n", type_words(*p).c_str(), shell_quote(p->name).c_str(),
                param_value_text(*p).c_str());
  }
  return 0;
}

// typeset -T SCALAR[=text] array [sep]: declares both and ties them; an
// `array=(…)` value is given by the executor once this returns.
int typeset_tie(strv cmd, const vec<str>& names, const TypesetAttrs& attrs, ExecContext& ctx) {
  auto fail = [&](strv msg) {
    s::fprintf(stderr, "jfsh: %.*s: %.*s\n", static_cast<int>(cmd.size()), cmd.data(),
               static_cast<int>(msg.size()), msg.data());
    return 1;
  };
  if (names.size() < 2) return fail("-T requires names of scalar and array");
  if (names.size() > 3) return fail("too many arguments for -T");
  s::size_t eq = names[0].find('=');
  str scalar = names[0].substr(0, eq);
  str array  = names[1].substr(0, names[1].find('='));
  if (scalar == array) return fail("can't tie a variable to itself: " + scalar);
  char sep = names.size() == 3 and not names[2].empty() ? names[2][0] : ':';
  TypesetAttrs plain = attrs;
  plain.tie = false;
  plain.type = 'a';
  plain.on &= PM_UNIQUE;
  if (typeset_one(cmd, array, plain, ctx) != 0) return 1;
  plain.type = 0;
  plain.on = attrs.on & ~PM_UNIQUE;
  if (typeset_one(cmd, scalar, plain, ctx) != 0) return 1;
  auto p = ctx.params.lookup_raw(scalar);
  if (not p) return 1;
  ctx.params.tie(p->get(), array, sep);
  if (eq != str::npos) p->get().handlers->set_scalar(names[0].substr(eq + 1));
  return 0;
}

}  // namespace

// typeset [±aAEFfgilmprUuxT] [name[=value] …].  Without -g a name is
// declared in the innermost scope, which inside a function is local to it.
// A new parameter is set and empty (0 for -i/-F, () for -a/-A).  `implied`
// holds the options the variant stands for (export is `typeset -gx`).
// With no names it lists the parameters that have the attributes given.
// TODO(builtin/typeset): `-Z`/`-L`/`-R` padding, `-t`.
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
      // ±m and ±f both mean it; +f lists the names only.
      case 'm': attrs.pattern = true; return true;
      case 'f': attrs.functions = true; attrs.names_only |= not on; return true;
      case 'T': attrs.tie = on; return true;
      case 'x': (on ? attrs.on : attrs.off) |= PM_EXPORTED; return true;
      case 'r': (on ? attrs.on : attrs.off) |= PM_READONLY; return true;
      case 'u': (on ? attrs.on : attrs.off) |= PM_UPPER;    return true;
      case 'l': (on ? attrs.on : attrs.off) |= PM_LOWER;    return true;
      case 'U': (on ? attrs.on : attrs.off) |= PM_UNIQUE;   return true;
      case 'H': (on ? attrs.on : attrs.off) |= PM_HIDEVAL;  return true;
      case 'h': (on ? attrs.on : attrs.off) |= PM_HIDE;     return true;
      case 't': return true;   // accepted, no effect yet
      default:  return false;
    }
  };
  for (char c : implied) apply(c, true);
  str plus;   // the options given with `+`: a listing of names only
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "--") { ++i; break; }
    if (a == "+") { attrs.names_only = true; continue; }
    if (a.size() < 2 or (a[0] != '-' and a[0] != '+')) break;
    for (s::size_t k = 1; k < a.size(); ++k) {
      if (a[0] == '+') plus += a[k];
      if (not apply(a[k], a[0] == '-')) {
        s::fprintf(stderr, "jfsh: %.*s: bad option: %c%c\n",
                   static_cast<int>(cmd.size()), cmd.data(), a[0], a[k]);
        return 1;
      }
      // -i, -F and -E take a number, attached (`-F2`) or as the next word.
      if (a[0] != '-' or (a[k] != 'i' and a[k] != 'F' and a[k] != 'E')) continue;
      auto number = [](strv t) {
        return not t.empty() and s::all_of(t.begin(), t.end(), [](char c) {
          return s::isdigit(static_cast<unsigned char>(c));
        });
      };
      str digits = a.substr(k + 1);
      if (not number(digits)) {
        // A next word that starts as a number must be one, as in zsh.
        bool next = k + 1 == a.size() and i + 1 < args.size();
        if (next and not number(args[i + 1]) and not args[i + 1].empty()
            and s::isdigit(static_cast<unsigned char>(args[i + 1][0])))
          return builtin_error(cmd, str(a[k] == 'i' ? "bad base value: " : "bad precision value: ")
                                        + args[i + 1]);
        if (not next or not number(args[i + 1])) continue;
        digits = args[++i];
      }
      attrs.base = static_cast<int>(s::min(s::strtol(digits.c_str(), nullptr, 10), 9999L));
      break;
    }
  }
  if (attrs.type == 'i' and attrs.base and (*attrs.base < 2 or *attrs.base > 36))
    return builtin_error(cmd, "invalid base (must be 2 to 36 inclusive): "
                                  + s::to_string(*attrs.base));
  if (cmd == "local") attrs.global = false;
  vec<str> names(args.begin() + static_cast<s::ptrdiff_t>(i), args.end());

  if (attrs.functions) {
    vec<str> fargs{"functions"};
    if (attrs.names_only) fargs.emplace_back("+");
    if (attrs.pattern) fargs.emplace_back("-m");
    fargs.insert(fargs.end(), names.begin(), names.end());
    return builtin_functions(fargs, ctx);
  }
  if (names.empty() or attrs.pattern) {
    // `typeset +i`, `typeset +m pat`: the names only, of the parameters
    // with the attributes the `+` options name.
    if (not plus.empty()) {
      for (char c : plus) apply(c, true);
      attrs.names_only = true;
    }
    // `export -p` lists the exported parameters as `export X=…`; plain
    // `export` lists them as `X=…`.
    return typeset_list(attrs, names, ctx);
  }
  if (attrs.tie) return typeset_tie(cmd, names, attrs, ctx);
  int rc = 0;
  if (attrs.print) {
    for (const auto& name : names) {
      auto p = ctx.params.lookup(name);
      // A prefix assignment's copy that was unset is gone, as in zsh.
      if (p and not p->get().is_set() and ctx.params.in_prefix_scope(p->get()))
        p.reset();
      if (not p) {
        s::fprintf(stderr, "jfsh: %.*s: no such variable: %s\n",
                   static_cast<int>(cmd.size()), cmd.data(), name.c_str());
        rc = 1;
        continue;
      }
      bool global = p->get().scope < ctx.params.local_scope();
      s::printf("%s\n", typeset_line(p->get(), global).c_str());
    }
    return rc;
  }
  for (const auto& name : names)
    if (typeset_one(cmd, name, attrs, ctx) != 0) rc = 1;
  // As in zsh, a name that can't be declared or assigned ends the command
  // line once the rest are done.
  if (rc != 0) ctx.aborting = true;
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

// `readonly` is `typeset -r` (so local in a function), and `typeset -gr`
// under POSIX_BUILTINS, as in zsh.
int builtin_readonly(vec<str>& args, ExecContext& ctx) {
  return typeset_common(args, ctx, ctx.opts.get(OPT_POSIXBUILTINS) ? "gr" : "r");
}

// unfunction [-m] name …: unset -f.
int builtin_unfunction(vec<str>& args, ExecContext& ctx) {
  args.insert(args.begin() + 1, "-f");
  return builtin_unset(args, ctx);
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
        s::fprintf(stderr, "jfsh: %s: no such hash table element: %s\n", args[0].c_str(),
                   name.c_str());
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
      // A special hash with per-key handlers ($mapfile, a ztie'd db)
      // deletes just that key.
      if (auto raw = ctx.params.lookup_raw(base);
          raw and raw->get().handlers and raw->get().handlers->unset_elem) {
        (void)ctx.params.unset_element(base, key);
        continue;
      }
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
        auto v = math_eval(key, ctx.params, math_options(ctx.opts),
                           &ctx.modules.math_funcs());
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
