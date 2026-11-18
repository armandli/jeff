#include <jfsh/modules/example.h>

#include <cstdio>

#include <jfsh/builtins/math.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

// zsh/example: a minimal, documented module intended both as a reference
// for third-party module authors and as a target for tests that exercise
// every ModuleFeatures contribution type.

namespace jfsh {

namespace {

int builtin_example(vec<str>& args, ExecContext& ctx) {
  for (s::size_t i = 1; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "-M") {
      if (++i >= args.size()) {
        s::fprintf(stderr, "jfsh: example: option -M requires an argument\n");
        return 1;
      }
      return ctx.modules.math_funcs().find(args[i]) ? 0 : 1;
    }
    if (a == "-m") {
      if (++i >= args.size()) {
        s::fprintf(stderr, "jfsh: example: option -m requires an argument\n");
        return 1;
      }
      auto v = math_eval(args[i], ctx.params, math_options(ctx.opts),
                         &ctx.modules.math_funcs());
      if (not v) {
        s::fprintf(stderr, "jfsh: example: %s\n", v.error().message.c_str());
        return 2;
      }
      return v->truthy() ? 0 : 1;
    }
    s::fprintf(stderr, "jfsh: example: bad option: %s\n", a.c_str());
    return 1;
  }
  s::printf("jfsh zsh/example\n");
  return 0;
}

Result<MathNum> math_example(const vec<MathNum>& args) {
  if (args.size() != 1)
    return s::unexpected(ShellError{ErrorKind::BadSubstitution,
                                    "example(): wrong number of arguments"});
  if (args[0].is_float) return MathNum::of(args[0].d + 42.0);
  return MathNum::of(args[0].i + 42);
}

int cond_example_match(const str& lhs, const str& rhs, ExecContext&) {
  return lhs.find(rhs) == str::npos ? 1 : 0;
}

}  // namespace

void register_example_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/example", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"example", builtin_example, BINF_NONE, 0});
    out.mathfuncs.push_back(MathFuncDef{"example", math_example, 1, 1});
    out.conds.push_back(CondDef{"-example-match", cond_example_match});
    out.feature_names = {"b:example", "f:example", "c:-example-match"};
  });
}

}  // namespace jfsh
