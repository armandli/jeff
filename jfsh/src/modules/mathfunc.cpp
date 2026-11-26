// zsh/mathfunc: a port of zsh 5.9's Src/Modules/mathfunc.c.  zsh's
// copyright and licence:
//
// Copyright (c) 1999 Peter Stephenson
// All rights reserved.
//
// Permission is hereby granted, without written agreement and without
// license or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall Peter Stephenson or the Zsh Development Group be liable
// to any party for direct, indirect, special, incidental, or consequential
// damages arising out of the use of this software and its documentation,
// even if Peter Stephenson and the Zsh Development Group have been advised of
// the possibility of such damage.
//
// Peter Stephenson and the Zsh Development Group specifically disclaim any
// warranties, including, but not limited to, the implied warranties of
// merchantability and fitness for a particular purpose.  The software
// provided hereunder is on an "as is" basis, and Peter Stephenson and the
// Zsh Development Group have no obligation to provide maintenance,
// support, updates, enhancements, or modifications.

#include <jfsh/modules/mathfunc.h>

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

namespace jfsh {

namespace {

// How a function takes its arguments and gives its result (zsh's TF_).
enum TypeFlags : int {
  TF_NOCONV = 1,   // arguments not made floats
  TF_INT1   = 2,   // the first argument is an integer
  TF_INT2   = 4,   // the second argument is an integer
  TF_NOASS  = 8,   // the result isn't the double computed
};

enum Func : int {
  MF_ABS, MF_ACOS, MF_ACOSH, MF_ASIN, MF_ASINH, MF_ATAN, MF_ATANH, MF_CBRT, MF_CEIL,
  MF_COPYSIGN, MF_COS, MF_COSH, MF_ERF, MF_ERFC, MF_EXP, MF_EXPM1, MF_FABS, MF_FLOAT,
  MF_FLOOR, MF_FMOD, MF_GAMMA, MF_HYPOT, MF_ILOGB, MF_INT, MF_J0, MF_J1, MF_JN, MF_LDEXP,
  MF_LGAMMA, MF_LOG, MF_LOG10, MF_LOG1P, MF_LOG2, MF_LOGB, MF_NEXTAFTER, MF_RINT, MF_SCALB,
  MF_SIGNGAM, MF_SIN, MF_SINH, MF_SQRT, MF_TAN, MF_TANH, MF_Y0, MF_Y1, MF_YN,
};

struct Entry {
  const char* name;
  int         min, max;
  int         id;
  int         tflags = 0;
};

// zsh's mftab, rand48 (a string function) aside.
constexpr Entry MFTAB[] = {
  {"abs", 1, 1, MF_ABS, TF_NOCONV | TF_NOASS}, {"acos", 1, 1, MF_ACOS},
  {"acosh", 1, 1, MF_ACOSH}, {"asin", 1, 1, MF_ASIN}, {"asinh", 1, 1, MF_ASINH},
  {"atan", 1, 2, MF_ATAN}, {"atanh", 1, 1, MF_ATANH}, {"cbrt", 1, 1, MF_CBRT},
  {"ceil", 1, 1, MF_CEIL}, {"copysign", 2, 2, MF_COPYSIGN}, {"cos", 1, 1, MF_COS},
  {"cosh", 1, 1, MF_COSH}, {"erf", 1, 1, MF_ERF}, {"erfc", 1, 1, MF_ERFC},
  {"exp", 1, 1, MF_EXP}, {"expm1", 1, 1, MF_EXPM1}, {"fabs", 1, 1, MF_FABS},
  {"float", 1, 1, MF_FLOAT}, {"floor", 1, 1, MF_FLOOR}, {"fmod", 2, 2, MF_FMOD},
  {"gamma", 1, 1, MF_GAMMA}, {"hypot", 2, 2, MF_HYPOT},
  {"ilogb", 1, 1, MF_ILOGB, TF_NOASS}, {"int", 1, 1, MF_INT, TF_NOASS},
  {"j0", 1, 1, MF_J0}, {"j1", 1, 1, MF_J1}, {"jn", 2, 2, MF_JN, TF_INT1},
  {"ldexp", 2, 2, MF_LDEXP, TF_INT2}, {"lgamma", 1, 1, MF_LGAMMA}, {"log", 1, 1, MF_LOG},
  {"log10", 1, 1, MF_LOG10}, {"log1p", 1, 1, MF_LOG1P}, {"log2", 1, 1, MF_LOG2},
  {"logb", 1, 1, MF_LOGB}, {"nextafter", 2, 2, MF_NEXTAFTER},
};
// After rand48, in zsh's order.
constexpr Entry MFTAB_REST[] = {
  {"rint", 1, 1, MF_RINT}, {"scalb", 2, 2, MF_SCALB, TF_INT2},
  {"signgam", 0, 0, MF_SIGNGAM, TF_NOASS}, {"sin", 1, 1, MF_SIN}, {"sinh", 1, 1, MF_SINH},
  {"sqrt", 1, 1, MF_SQRT}, {"tan", 1, 1, MF_TAN}, {"tanh", 1, 1, MF_TANH},
  {"y0", 1, 1, MF_Y0}, {"y1", 1, 1, MF_Y1}, {"yn", 2, 2, MF_YN, TF_INT1},
};

s::int64_t to_zlong(const MathNum& n) {
  return n.is_float ? MathNum::of(n.d).as_int() : n.i;
}

// zsh's math_func.
MathNum math_func(const Entry& e, const vec<MathNum>& argv) {
  double argd = 0, argd2 = 0, retd = 0;
  int argi = 0;
  auto argc = argv.size();
  if (argc and not (e.tflags & TF_NOCONV)) {
    if (e.tflags & TF_INT1) argi = static_cast<int>(to_zlong(argv[0]));
    else                    argd = argv[0].as_float();
    if (argc > 1) {
      if (e.tflags & TF_INT2) argi = static_cast<int>(to_zlong(argv[1]));
      else                    argd2 = argv[1].as_float();
    }
  }
  MathNum ret = MathNum::of(0.0);
  switch (e.id) {
    case MF_ABS:
      if (not argv[0].is_float) ret = MathNum::of(argv[0].i < 0 ? -argv[0].i : argv[0].i);
      else                      ret = MathNum::of(s::fabs(argv[0].d));

    break; case MF_ACOS:      retd = s::acos(argd);

    break; case MF_ACOSH:     retd = s::acosh(argd);

    break; case MF_ASIN:      retd = s::asin(argd);

    break; case MF_ASINH:     retd = s::asinh(argd);

    break; case MF_ATAN:      retd = argc == 2 ? s::atan2(argd, argd2) : s::atan(argd);

    break; case MF_ATANH:     retd = s::atanh(argd);

    break; case MF_CBRT:      retd = s::cbrt(argd);

    break; case MF_CEIL:      retd = s::ceil(argd);

    break; case MF_COPYSIGN:  retd = s::copysign(argd, argd2);

    break; case MF_COS:       retd = s::cos(argd);

    break; case MF_COSH:      retd = s::cosh(argd);

    break; case MF_ERF:       retd = s::erf(argd);

    break; case MF_ERFC:      retd = s::erfc(argd);

    break; case MF_EXP:       retd = s::exp(argd);

    break; case MF_EXPM1:     retd = s::expm1(argd);

    break; case MF_FABS:      retd = s::fabs(argd);

    break; case MF_FLOAT:     retd = argd;

    break; case MF_FLOOR:     retd = s::floor(argd);

    break; case MF_FMOD:      retd = s::fmod(argd, argd2);

    break; case MF_GAMMA:     retd = s::tgamma(argd);

    break; case MF_HYPOT:     retd = s::hypot(argd, argd2);

    break; case MF_ILOGB:     ret = MathNum::of(static_cast<s::int64_t>(s::ilogb(argd)));

    break; case MF_INT:       ret = MathNum::of(MathNum::of(argd).as_int());

    break; case MF_J0:        retd = ::j0(argd);

    break; case MF_J1:        retd = ::j1(argd);

    break; case MF_JN:        retd = ::jn(argi, argd2);

    break; case MF_LDEXP:     retd = s::ldexp(argd, argi);

    break; case MF_LGAMMA:    retd = s::lgamma(argd);

    break; case MF_LOG:       retd = s::log(argd);

    break; case MF_LOG10:     retd = s::log10(argd);

    break; case MF_LOG1P:     retd = s::log1p(argd);

    break; case MF_LOG2:      retd = s::log2(argd);

    break; case MF_LOGB:      retd = s::logb(argd);

    break; case MF_NEXTAFTER: retd = s::nextafter(argd, argd2);

    break; case MF_RINT:      retd = s::rint(argd);

    break; case MF_SCALB:     retd = s::scalbn(argd, argi);

    break; case MF_SIGNGAM:   ret = MathNum::of(static_cast<s::int64_t>(::signgam));

    break; case MF_SIN:       retd = s::sin(argd);

    break; case MF_SINH:      retd = s::sinh(argd);

    break; case MF_SQRT:      retd = s::sqrt(argd);

    break; case MF_TAN:       retd = s::tan(argd);

    break; case MF_TANH:      retd = s::tanh(argd);

    break; case MF_Y0:        retd = ::y0(argd);

    break; case MF_Y1:        retd = ::y1(argd);

    break; case MF_YN:        retd = ::yn(argi, argd2);

    break; default:;
  }
  if (not (e.tflags & TF_NOASS)) ret = MathNum::of(retd);
  return ret;
}

bool is_blank(char c) { return c == ' ' or c == '\t'; }

// zsh's math_string for rand48(param): the seed is kept in the parameter
// named, as twelve hex digits, else in one of its own.
MathNum rand48(strv arg, ExecContext& ctx) {
  static unsigned short seedbuf[3];
  static bool seedbuf_init = false;
  while (not arg.empty() and is_blank(arg.front())) arg.remove_prefix(1);
  while (not arg.empty() and is_blank(arg.back())) arg.remove_suffix(1);
  unsigned short tmp_seedbuf[3];
  unsigned short* seedbufptr;
  bool do_init = true;
  if (not arg.empty()) {
    seedbufptr = tmp_seedbuf;
    str seedstr = param_or(ctx.params, arg);
    if (seedstr.size() >= 12) {
      do_init = false;
      s::size_t at = 0;
      for (int i = 0; i < 3 and not do_init; ++i) {
        unsigned short& seed = seedbufptr[i];
        seed = 0;
        for (int j = 0; j < 4; ++j) {
          auto c = static_cast<unsigned char>(seedstr[at]);
          if (s::isdigit(c)) {
            seed = static_cast<unsigned short>(seed + (c - '0'));
          } else if (s::tolower(c) >= 'a' and s::tolower(c) <= 'f') {
            seed = static_cast<unsigned short>(seed + (s::tolower(c) - 'a' + 10));
          } else {
            do_init = true;
            break;
          }
          ++at;
          if (j < 3) seed = static_cast<unsigned short>(seed * 16);
        }
      }
    }
  } else {
    seedbufptr = seedbuf;
    if (not seedbuf_init) seedbuf_init = true;
    else                  do_init = true;
  }
  if (do_init) {
    for (int i = 0; i < 3; ++i) seedbufptr[i] = static_cast<unsigned short>(s::rand());
    (void)::seed48(seedbufptr);
  }
  MathNum ret = MathNum::of(::erand48(seedbufptr));
  if (not arg.empty()) {
    char outbuf[13];
    s::snprintf(outbuf, sizeof outbuf, "%04x%04x%04x", seedbufptr[0], seedbufptr[1],
                seedbufptr[2]);
    (void)ctx.executor->assign_var(str(arg), outbuf);
  }
  return ret;
}

void add(ModuleFeatures& out, const Entry& e) {
  out.mathfuncs.push_back(MathFuncDef{
      e.name, [&e](const vec<MathNum>& argv) -> Result<MathNum> { return math_func(e, argv); },
      {}, e.min, e.max});
}

}  // namespace

void register_mathfunc_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/mathfunc", [](ModuleFeatures& out, ExecContext& ctx) {
    for (const auto& e : MFTAB) add(out, e);
    ExecContext* c = &ctx;
    MathFuncDef r48;
    r48.name   = "rand48";
    r48.str_fn = [c](strv arg) -> Result<MathNum> { return rand48(arg, *c); };
    out.mathfuncs.push_back(s::move(r48));
    for (const auto& e : MFTAB_REST) add(out, e);
  });
}

}  // namespace jfsh
