#include <jfsh/modules/datetime.h>

#include <cstdio>
#include <cstring>
#include <ctime>

#include <jfsh/builtins/builtins.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

// zsh/datetime: $EPOCHSECONDS / $EPOCHREALTIME / $epochtime array and the
// `strftime` builtin.  The two specials are already installed by
// install_special_params(), so this module only records their names in
// `features.params` (for `zmodload -L`) and ships the `strftime` builtin.

namespace jfsh {

namespace {

// `strftime [-rs] fmt [epochtime]` — M10 Phase 1 subset.
//   -r  parse `fmt` (a strptime format) and print the epoch seconds.
//   -s VAR  store the result in VAR instead of printing.
//   fmt     an strftime(3) format (or strptime(3) under -r).
//   epochtime  defaults to EPOCHSECONDS; under -r, defaults to the second
//              positional (which is then the date string).
int builtin_strftime(vec<str>& args, ExecContext& ctx) {
  bool reverse = false;
  str  var;
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a.size() < 2 or a[0] != '-' or a == "--") { if (a == "--") ++i; break; }
    for (s::size_t k = 1; k < a.size(); ++k) {
      char c = a[k];
      if (c == 'r') { reverse = true; continue; }
      if (c == 's') {
        if (k + 1 < a.size()) { var = a.substr(k + 1); k = a.size(); break; }
        if (i + 1 >= args.size()) {
          s::fprintf(stderr, "jfsh: strftime: option -s requires an argument\n");
          return 1;
        }
        var = args[++i];
        k = a.size();
        break;
      }
      s::fprintf(stderr, "jfsh: strftime: bad option: -%c\n", c);
      return 1;
    }
  }
  if (i >= args.size()) {
    s::fprintf(stderr, "jfsh: strftime: not enough arguments\n");
    return 1;
  }
  const str& fmt = args[i++];

  str result;
  if (reverse) {
    if (i >= args.size()) {
      s::fprintf(stderr, "jfsh: strftime: not enough arguments\n");
      return 1;
    }
    const str& date = args[i];
    struct tm tm{};
    if (not ::strptime(date.c_str(), fmt.c_str(), &tm)) {
      s::fprintf(stderr, "jfsh: strftime: date not matched\n");
      return 1;
    }
    time_t t = ::mktime(&tm);
    result = s::to_string(static_cast<long long>(t));
  } else {
    time_t t = (i < args.size()) ? static_cast<time_t>(s::strtoll(args[i].c_str(), nullptr, 10))
                                 : s::time(nullptr);
    struct tm tm{};
    ::localtime_r(&t, &tm);
    char buf[512];
    s::size_t n = s::strftime(buf, sizeof buf, fmt.c_str(), &tm);
    result.assign(buf, n);
  }

  if (not var.empty()) {
    ctx.params.assign(var, result, PM_SCALAR);
  } else {
    s::printf("%s\n", result.c_str());
  }
  return 0;
}

}  // namespace

void register_datetime_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/datetime", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"strftime", builtin_strftime, BINF_NONE, 2, -1});
    out.params.push_back("EPOCHSECONDS");
    out.params.push_back("EPOCHREALTIME");
    out.params.push_back("epochtime");
  });
}

}  // namespace jfsh
