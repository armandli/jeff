// zsh/datetime: a port of zsh 5.9's Src/Modules/datetime.c.  zsh's
// copyright and licence:
//
// Copyright (c) 2002 Peter Stephenson, Clint Adams
// All rights reserved.
//
// Permission is hereby granted, without written agreement and without
// license or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall Peter Stephenson, Clint Adams or the Zsh Development Group
// be liable to any party for direct, indirect, special, incidental, or
// consequential damages arising out of the use of this software and its
// documentation, even if Peter Stephenson, Clint Adams and the Zsh
// Development Group have been advised of the possibility of such damage.
//
// Peter Stephenson, Clint Adams and the Zsh Development Group specifically
// disclaim any warranties, including, but not limited to, the implied
// warranties of merchantability and fitness for a particular purpose.
// The software provided hereunder is on an "as is" basis, and Peter
// Stephenson, Clint Adams and the Zsh Development Group have no obligation
// to provide maintenance, support, updates, enhancements, or modifications.

#include <jfsh/modules/datetime.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/math.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>
#include <jfsh/prompt/prompt.h>

namespace jfsh {

namespace {

int reverse_strftime(strv nam, const vec<str>& argv, const str* scalar, bool quiet,
                     ExecContext& ctx) {
  // As zsh: tm_isdst -1 lets mktime pick the zone's offset, tm_mday 1 is
  // the first of the month.
  struct tm tm{};
  tm.tm_isdst = -1;
  tm.tm_mday  = 1;
  const char* endp = ::strptime(argv[1].c_str(), argv[0].c_str(), &tm);
  if (not endp) {
    if (not quiet) mod_error(nam, "format not matched");
    return 1;
  }
  auto mytime = static_cast<s::int64_t>(::mktime(&tm));
  if (scalar) {
    (void)set_iparam(ctx, *scalar, mytime);
  } else {
    s::printf("%lld\n", static_cast<long long>(mytime));
  }
  if (*endp and not quiet) mod_error(nam, "warning: input string not completely matched");
  return 0;
}

int output_strftime(strv nam, const vec<str>& argv, const ZOps& ops, ExecContext& ctx) {
  const str* scalar = nullptr;
  if (ops.isset('s')) {
    scalar = &ops.arg('s');
    if (not zsh_isident(*scalar)) return mod_error(nam, "not an identifier: " + *scalar);
  }
  if (ops.isset('r')) {
    if (argv.size() < 2) return mod_error(nam, "timestring expected");
    return reverse_strftime(nam, argv, scalar, ops.isset('q'), ctx);
  }
  struct timespec ts{};
  struct tm tm{};
  if (argv.size() < 2) {
    ::clock_gettime(CLOCK_REALTIME, &ts);
    ::localtime_r(&ts.tv_sec, &tm);
  } else {
    errno = 0;
    char* endptr = nullptr;
    ts.tv_sec = static_cast<time_t>(s::strtoul(argv[1].c_str(), &endptr, 10));
    if (errno != 0) return mod_error(nam, argv[1] + ": " + errno_text(errno));
    if (argv[1].empty() or *endptr != '\0')
      return mod_error(nam, argv[1] + ": invalid decimal number");
    if (not ::localtime_r(&ts.tv_sec, &tm))
      return mod_error(nam, argv[1] + ": unable to convert to time");
    ts.tv_nsec = 0;
    if (argv.size() > 2) {
      auto n = zstrtol(argv[2], 0, 10);
      ts.tv_nsec = static_cast<long>(n.value);
      if (errno != 0) return mod_error(nam, argv[2] + ": " + errno_text(errno));
      if (argv[2].empty() or n.end != argv[2].size())
        return mod_error(nam, argv[2] + ": invalid decimal number");
      if (ts.tv_nsec < 0 or ts.tv_nsec > 999999999)
        return mod_error(nam, argv[2] + ": invalid nanosecond value");
    }
  }
  str buffer = ztrftime(argv[0], tm, ts.tv_nsec);
  if (scalar) {
    (void)ctx.executor->assign_var(*scalar, s::move(buffer));
  } else {
    s::fwrite(buffer.data(), 1, buffer.size(), stdout);
    if (not ops.isset('n')) s::putchar('\n');
  }
  return 0;
}

// strftime [-nqr] [-s scalar] format [epochtime [nanoseconds]]
int builtin_strftime(vec<str>& args, ExecContext& ctx) {
  ZOps ops;
  int first = zsh_options(args, "nqrs:", 1, 3, ops);
  if (first < 0) return 1;
  vec<str> argv(args.begin() + first, args.end());
  // A set $TZ is made local and exported, so the C library sees it.
  auto tz = ctx.params.lookup("TZ");
  str zone = tz and tz->get().is_set() ? tz->get().scalar() : str{};
  ScopedEnv env("TZ", zone.empty() ? nullptr : zone.c_str());
  return output_strftime(args[0], argv, ops, ctx);
}

}  // namespace

void register_datetime_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/datetime", [](ModuleFeatures& out, ExecContext& ctx) {
    out.builtins.push_back(BuiltinDef{"strftime", builtin_strftime, BINF_NONE});
    constexpr s::uint32_t SPECIAL = PM_SPECIAL | PM_READONLY | PM_HIDE | PM_HIDEVAL;
    // $EPOCHSECONDS: the time in seconds.
    Param& secs = ctx.params.assign("EPOCHSECONDS", s::int64_t{0}, PM_INTEGER | SPECIAL);
    secs.handlers = s::make_unique<ParamHandlers>();
    secs.handlers->get_scalar = [] {
      return s::to_string(static_cast<long long>(s::time(nullptr)));
    };
    // $EPOCHREALTIME: the time as a float.
    Param& real = ctx.params.assign("EPOCHREALTIME", 0.0, PM_FLOAT | SPECIAL);
    real.handlers = s::make_unique<ParamHandlers>();
    real.handlers->get_float = [] {
      struct timespec now{};
      ::clock_gettime(CLOCK_REALTIME, &now);
      return static_cast<double>(now.tv_sec) + static_cast<double>(now.tv_nsec) * 1e-9;
    };
    // $epochtime: seconds and nanoseconds.
    Param& pair = ctx.params.assign("epochtime", vec<str>{}, PM_ARRAY | SPECIAL);
    pair.handlers = s::make_unique<ParamHandlers>();
    pair.handlers->get_array = [] {
      struct timespec now{};
      ::clock_gettime(CLOCK_REALTIME, &now);
      return vec<str>{s::to_string(static_cast<long>(now.tv_sec)),
                      s::to_string(static_cast<long>(now.tv_nsec))};
    };
    out.params = {"EPOCHSECONDS", "EPOCHREALTIME", "epochtime"};
  });
}

}  // namespace jfsh
