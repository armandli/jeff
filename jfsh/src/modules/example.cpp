// zsh/example: a port of zsh 5.9's Src/Modules/example.c, the module that
// shows each kind of feature.  zsh's copyright and licence:
//
// Copyright (c) 1996-1997 Zoltán Hidvégi
// All rights reserved.
//
// Permission is hereby granted, without written agreement and without
// license or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall Zoltán Hidvégi or the Zsh Development Group be liable
// to any party for direct, indirect, special, incidental, or consequential
// damages arising out of the use of this software and its documentation,
// even if Zoltán Hidvégi and the Zsh Development Group have been advised of
// the possibility of such damage.
//
// Zoltán Hidvégi and the Zsh Development Group specifically disclaim any
// warranties, including, but not limited to, the implied warranties of
// merchantability and fitness for a particular purpose.  The software
// provided hereunder is on an "as is" basis, and Zoltán Hidvégi and the
// Zsh Development Group have no obligation to provide maintenance,
// support, updates, enhancements, or modifications.

#include <jfsh/modules/example.h>

#include <cstdio>

#include <jfsh/builtins/math.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

namespace jfsh {

namespace {

// The three parameters' values (zsh's intparam, strparam and arrparam):
// the parameters read and set them, the builtin too.
struct ExampleState {
  vec<str>   arr{"example", "array"};
  str        text = "example";
  s::int64_t num  = 42;
};

// example [-flags] [args …]: prints its options, arguments and the three
// parameters, then sets them from the arguments.
int builtin_example(vec<str>& args, ExampleState& st) {
  ZOps ops;
  int first = zsh_options(args, "flags", 0, -1, ops);
  if (first < 0) return 1;
  vec<str> rest(args.begin() + first, args.end());
  s::printf("Options: ");
  for (int c = 33; c < 128; ++c)
    if (ops.isset(static_cast<char>(c))) s::putchar(c);
  s::printf("\nArguments:");
  for (const auto& a : rest) s::printf(" %s", a.c_str());
  s::printf("\nName: %s\n", args[0].c_str());
  s::printf("\nInteger Parameter: %lld\n", static_cast<long long>(st.num));
  s::printf("String Parameter: %s\n", st.text.c_str());
  s::printf("Array Parameter:");
  for (const auto& e : st.arr) s::printf(" %s", e.c_str());
  s::printf("\n");
  st.num  = static_cast<s::int64_t>(rest.size());
  st.text = rest.empty() ? str{} : rest[0];
  st.arr  = rest;
  return 0;
}

// [[ -len str [n] ]]: str is n characters long, or empty.
int cond_p_len(const vec<str>& a, ExecContext& ctx) {
  if (a.size() > 1) {
    auto v = math_eval(a[1], ctx.params, math_options(ctx), &ctx.modules.math_funcs());
    s::int64_t n = v ? v->as_int() : 0;
    return static_cast<s::int64_t>(a[0].size()) == n ? 0 : 1;
  }
  return a[0].empty() ? 0 : 1;
}

// [[ a -ex b ]]: a and b make "example".
int cond_i_ex(const str& l, const str& r, ExecContext&) { return l + r == "example" ? 0 : 1; }

// sum(n …): the sum, a float once a float is added.
Result<MathNum> math_sum(const vec<MathNum>& argv) {
  bool f = false;
  s::int64_t l = 0;
  double d = 0;
  for (const auto& a : argv) {
    if (not a.is_float) {
      if (f) d += static_cast<double>(a.i);
      else   l += a.i;
    } else if (f) {
      d += a.d;
    } else {
      d = static_cast<double>(l) + a.d;
      f = true;
    }
  }
  return f ? MathNum::of(d) : MathNum::of(l);
}

}  // namespace

void register_example_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/example", [](ModuleFeatures& out, ExecContext& ctx) {
    s::printf("The example module has now been set up.\n");
    s::fflush(stdout);
    auto st = s::make_shared<ExampleState>();
    out.builtins.push_back(BuiltinDef{
        "example", [st](vec<str>& a, ExecContext&) { return builtin_example(a, *st); }, BINF_NONE});
    CondDef ex;
    ex.name = "ex";
    ex.fn   = cond_i_ex;
    out.conds.push_back(s::move(ex));
    CondDef len;
    len.name      = "len";
    len.infix     = false;
    len.min_args  = 1;
    len.max_args  = 2;
    len.prefix_fn = cond_p_len;
    out.conds.push_back(s::move(len));
    MathFuncDef length;
    length.name   = "length";
    length.str_fn = [](strv arg) -> Result<MathNum> {
      return MathNum::of(static_cast<s::int64_t>(arg.size()));
    };
    out.mathfuncs.push_back(s::move(length));
    out.mathfuncs.push_back(MathFuncDef{"sum", math_sum, {}, 1, -1});
    // Bound to the state, as zsh's ARRPARAMDEF, INTPARAMDEF and STRPARAMDEF.
    Param& arr = ctx.params.assign("exarr", st->arr, PM_ARRAY);
    arr.handlers = s::make_unique<ParamHandlers>();
    arr.handlers->get_array = [st] { return st->arr; };
    arr.handlers->set_array = [st](vec<str> v) { st->arr = s::move(v); };
    Param& num = ctx.params.assign("exint", st->num, PM_INTEGER);
    num.handlers = s::make_unique<ParamHandlers>();
    num.handlers->get_scalar = [st] { return s::to_string(st->num); };
    num.handlers->set_scalar = [st](str v) { st->num = s::strtoll(v.c_str(), nullptr, 10); };
    Param& text = ctx.params.assign("exstr", st->text, PM_SCALAR);
    text.handlers = s::make_unique<ParamHandlers>();
    text.handlers->get_scalar = [st] { return st->text; };
    text.handlers->set_scalar = [st](str v) { st->text = s::move(v); };
    out.params = {"exarr", "exint", "exstr"};
    // Functions whose names start with `example` run with GLOB_DOTS set
    // (zsh's ex_wrapper).
    auto saved = s::make_shared<vec<bool>>();
    ExecContext* c = &ctx;
    ctx.hooks.function.emplace_back("zsh/example", [c, saved](const str& name, bool entering) {
      if (not name.starts_with("example")) return;
      if (entering) {
        saved->push_back(c->opts.get(OPT_GLOBDOTS));
        c->opts.set(OPT_GLOBDOTS);
      } else if (not saved->empty()) {
        c->opts.assign(OPT_GLOBDOTS, saved->back());
        saved->pop_back();
      }
    });
    out.on_unload = [c] {
      c->hooks.remove("zsh/example");
      s::printf("Thank you for using the example module.  Have a nice day.\n");
      s::fflush(stdout);
    };
  });
}

}  // namespace jfsh
