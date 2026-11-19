#include <jfsh/modules/zprof.h>

#include <cstdio>

#include <algorithm>
#include <chrono>
#include <map>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

// zsh/zprof (M11): a shell-function profiler, as zsh's
// Src/Modules/zprof.c.  ExecContext::hooks.function brackets every call;
// times are wall-clock milliseconds.

namespace jfsh {

namespace {

using Clock = s::chrono::steady_clock;

struct FuncStats {
  long   calls = 0;
  double total = 0;   // ms, recursive calls counted once
  double self  = 0;   // ms, without the functions it called
};

struct ArcStats {
  long   calls = 0;
  double total = 0;
  double self  = 0;
};

struct Frame {
  str               name;
  Clock::time_point start;
  double            children = 0;   // ms spent in callees
};

struct Profile {
  s::map<str, FuncStats>                 funcs;
  s::map<s::pair<str, str>, ArcStats>    arcs;    // (caller, callee)
  vec<Frame>                             stack;
};

Profile& prof() {
  static Profile p;
  return p;
}

void on_call(const str& name, bool entering) {
  auto& p = prof();
  if (entering) {
    p.stack.push_back({name, Clock::now(), 0});
    return;
  }
  if (p.stack.empty()) return;
  Frame f = s::move(p.stack.back());
  p.stack.pop_back();
  double elapsed = s::chrono::duration<double, s::milli>(Clock::now() - f.start).count();
  double self = elapsed - f.children;
  auto& st = p.funcs[f.name];
  ++st.calls;
  st.self += self;
  // A recursive call's time is already in the outer call's total.
  bool recursive = s::any_of(p.stack.begin(), p.stack.end(),
                             [&](const Frame& g) { return g.name == f.name; });
  if (not recursive) st.total += elapsed;
  if (not p.stack.empty()) {
    p.stack.back().children += elapsed;
    auto& arc = p.arcs[{p.stack.back().name, f.name}];
    ++arc.calls;
    arc.self += self;
    if (not recursive) arc.total += elapsed;
  }
}

void report() {
  auto& p = prof();
  vec<s::pair<str, FuncStats>> rows(p.funcs.begin(), p.funcs.end());
  s::stable_sort(rows.begin(), rows.end(),
                 [](const auto& a, const auto& b) { return a.second.self > b.second.self; });
  double all = 0;
  for (const auto& [_, st] : rows) all += st.self;
  auto pct = [&](double v) { return all > 0 ? 100.0 * v / all : 0.0; };
  s::printf("num  calls                time                       self            name\n");
  s::printf("-----------------------------------------------------------------------------------\n");
  int n = 0;
  for (const auto& [name, st] : rows) {
    ++n;
    s::printf("%2d) %4ld %10.2f %8.2f %6.2f%%  %8.2f %8.2f %6.2f%%  %s\n", n, st.calls,
              st.total, st.total / static_cast<double>(st.calls), pct(st.total),
              st.self, st.self / static_cast<double>(st.calls), pct(st.self), name.c_str());
  }
  // Then, per function, who called it and what it called.
  n = 0;
  for (const auto& [name, st] : rows) {
    ++n;
    s::printf("\n-----------------------------------------------------------------------------------\n\n");
    for (const auto& [arc, a] : p.arcs)
      if (arc.second == name)
        s::printf("    %4ld/%-4ld %8.2f %8.2f  %6.2f%%  %8.2f %8.2f             %s\n",
                  a.calls, st.calls, a.total, a.total / static_cast<double>(a.calls),
                  pct(a.total), a.self, a.self / static_cast<double>(a.calls), arc.first.c_str());
    s::printf("%2d) %4ld %10.2f %8.2f %6.2f%%  %8.2f %8.2f %6.2f%%  %s\n", n, st.calls,
              st.total, st.total / static_cast<double>(st.calls), pct(st.total),
              st.self, st.self / static_cast<double>(st.calls), pct(st.self), name.c_str());
    for (const auto& [arc, a] : p.arcs)
      if (arc.first == name)
        s::printf("    %4ld/%-4ld %8.2f %8.2f  %6.2f%%  %8.2f %8.2f             %s\n",
                  a.calls, p.funcs[arc.second].calls, a.total,
                  a.total / static_cast<double>(a.calls), pct(a.total), a.self,
                  a.self / static_cast<double>(a.calls), arc.second.c_str());
  }
}

int builtin_zprof(vec<str>& args, ExecContext&) {
  for (s::size_t i = 1; i < args.size(); ++i) {
    if (args[i] == "-c") {
      auto& p = prof();
      p.funcs.clear();
      p.arcs.clear();
      return 0;
    }
    return mod_error("zprof", "bad option: " + args[i]);
  }
  report();
  return 0;
}

}  // namespace

void register_zprof_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/zprof", [](ModuleFeatures& out, ExecContext& ctx) {
    ExecContext* ctxp = &ctx;
    prof() = Profile{};
    ctx.hooks.function.emplace_back("zsh/zprof", on_call);
    out.builtins.push_back(BuiltinDef{"zprof", builtin_zprof, BINF_NONE, 0});
    out.on_unload = [ctxp] {
      ctxp->hooks.remove("zsh/zprof");
      prof() = Profile{};
    };
    out.feature_names = {"b:zprof"};
  });
}

}  // namespace jfsh
