#include <jfsh/modules/mathfunc.h>

#include <cmath>
#include <cstdlib>

#include <jfsh/core/aliases.h>

// zsh/mathfunc: expose <cmath> and a few platform specials as math
// functions the arithmetic evaluator can resolve.  Each function returns a
// `Result<MathNum>`: an argument-count or domain error is flagged, otherwise
// the computed value is wrapped.

namespace jfsh {

namespace {

Result<MathNum> err(str msg) {
  return s::unexpected(ShellError{ErrorKind::BadSubstitution, s::move(msg)});
}

// Unary float function: f(double) → double.
template<double (*F)(double)>
Result<MathNum> f1(const vec<MathNum>& a) {
  if (a.size() != 1) return err("wrong number of arguments");
  return MathNum::of(F(a[0].as_float()));
}

// Binary float function: f(double, double) → double.
template<double (*F)(double, double)>
Result<MathNum> f2(const vec<MathNum>& a) {
  if (a.size() != 2) return err("wrong number of arguments");
  return MathNum::of(F(a[0].as_float(), a[1].as_float()));
}

// Unary → int function (truncation / integer-return).
Result<MathNum> fn_int(const vec<MathNum>& a) {
  if (a.size() != 1) return err("wrong number of arguments");
  return MathNum::of(a[0].as_int());
}

Result<MathNum> fn_float(const vec<MathNum>& a) {
  if (a.size() != 1) return err("wrong number of arguments");
  return MathNum::of(a[0].as_float());
}

Result<MathNum> fn_abs(const vec<MathNum>& a) {
  if (a.size() != 1) return err("wrong number of arguments");
  if (a[0].is_float) return MathNum::of(s::fabs(a[0].d));
  auto v = a[0].i;
  using U = s::uint64_t;
  U absv = v < 0 ? -static_cast<U>(v) : static_cast<U>(v);
  return MathNum::of(static_cast<s::int64_t>(absv));
}

// atan(y) or, as zsh's, atan(y, x) = atan2(y, x).
Result<MathNum> fn_atan(const vec<MathNum>& a) {
  if (a.size() == 1) return MathNum::of(s::atan(a[0].as_float()));
  if (a.size() == 2) return MathNum::of(s::atan2(a[0].as_float(), a[1].as_float()));
  return err("wrong number of arguments");
}

Result<MathNum> fn_signgam(const vec<MathNum>& a) {
  if (not a.empty()) return err("wrong number of arguments");
  return MathNum::of(static_cast<s::int64_t>(signgam));
}

Result<MathNum> fn_rand48(const vec<MathNum>& a) {
  if (not a.empty()) return err("wrong number of arguments");
  return MathNum::of(::drand48());
}

}  // namespace

void register_mathfunc_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/mathfunc", [](ModuleFeatures& out, ExecContext&) {
    auto add = [&](const char* name, MathFunc fn, int nargs) {
      out.mathfuncs.push_back(MathFuncDef{name, s::move(fn), nargs, nargs});
    };
    add("abs",     fn_abs,      1);
    add("acos",    f1<s::acos>, 1);
    add("acosh",   f1<s::acosh>,1);
    add("asin",    f1<s::asin>, 1);
    add("asinh",   f1<s::asinh>,1);
    out.mathfuncs.push_back(MathFuncDef{"atan", fn_atan, 1, 2});
    add("atan2",   f2<s::atan2>,2);
    add("cbrt",    f1<s::cbrt>, 1);
    add("ceil",    f1<s::ceil>, 1);
    add("cos",     f1<s::cos>,  1);
    add("cosh",    f1<s::cosh>, 1);
    add("erf",     f1<s::erf>,  1);
    add("erfc",    f1<s::erfc>, 1);
    add("exp",     f1<s::exp>,  1);
    add("expm1",   f1<s::expm1>,1);
    add("fabs",    f1<s::fabs>, 1);
    add("float",   fn_float,    1);
    add("floor",   f1<s::floor>,1);
    add("gamma",   f1<s::tgamma>,1);
    add("hypot",   f2<s::hypot>,2);
    add("int",     fn_int,      1);
    add("lgamma",  f1<s::lgamma>,1);
    add("log",     f1<s::log>,  1);
    add("log10",   f1<s::log10>,1);
    add("log1p",   f1<s::log1p>,1);
    add("log2",    f1<s::log2>, 1);
    add("logb",    f1<s::logb>, 1);
    add("pow",     f2<s::pow>,  2);
    add("rand48",  fn_rand48,   0);
    add("rint",    f1<s::rint>, 1);
    add("signgam", fn_signgam,  0);
    add("sin",     f1<s::sin>,  1);
    add("sinh",    f1<s::sinh>, 1);
    add("sqrt",    f1<s::sqrt>, 1);
    add("tan",     f1<s::tan>,  1);
    add("tanh",    f1<s::tanh>, 1);
    // Bessel functions: not on every libc (musl, some BSDs); guard on a
    // feature macro.  glibc and macOS libSystem have them.
#ifdef _GNU_SOURCE
    add("j0",      f1<::j0>,    1);
    add("j1",      f1<::j1>,    1);
    add("y0",      f1<::y0>,    1);
    add("y1",      f1<::y1>,    1);
#endif
  });
}

}  // namespace jfsh
