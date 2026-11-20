// Special parameters: the ones whose value the shell computes when they are
// read, as zsh's params.c does with its getfn/setfn pairs.

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

#include <chrono>
#include <memory>
#include <random>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/history/history.h>

namespace jfsh {

namespace {

namespace chr = s::chrono;

// The getter and setter of a special parameter.
struct Handlers {
  func<str()>          get_scalar{};
  func<void(str)>      set_scalar{};
  func<vec<str>()>     get_array{};
  func<void(vec<str>)> set_array{};
};

// Adds `name` with `flags` and the given handlers, replacing any value the
// environment gave it.
void special(ParamTable& params, str name, ParamValue initial, s::uint32_t flags,
             Handlers h) {
  Param& p   = params.assign(s::move(name), s::move(initial), flags | PM_SPECIAL);
  p.handlers = s::make_unique<ParamHandlers>();
  p.handlers->get_scalar = s::move(h.get_scalar);
  p.handlers->set_scalar = s::move(h.set_scalar);
  p.handlers->get_array  = s::move(h.get_array);
  p.handlers->set_array  = s::move(h.set_array);
}

s::int64_t to_int(const str& v) {
  return static_cast<s::int64_t>(s::strtoll(v.c_str(), nullptr, 10));
}

}  // namespace

void install_special_params(ExecContext& ctx) {
  ParamTable& params = ctx.params;

  // $SECONDS: seconds since the shell started; assigning sets the count.
  auto start = s::make_shared<chr::steady_clock::time_point>(chr::steady_clock::now());
  special(params, "SECONDS", s::int64_t{0}, PM_INTEGER, {
    .get_scalar = [start] {
      auto d = chr::steady_clock::now() - *start;
      return s::to_string(chr::duration_cast<chr::seconds>(d).count());
    },
    .set_scalar = [start](str v) {
      *start = chr::steady_clock::now() - chr::seconds(to_int(v));
    },
  });

  // $RANDOM: 0 … 32767; assigning seeds the sequence, so it repeats.
  auto rng = s::make_shared<s::minstd_rand>(
      static_cast<s::uint32_t>(s::time(nullptr)) ^ static_cast<s::uint32_t>(::getpid()));
  special(params, "RANDOM", s::int64_t{0}, PM_INTEGER, {
    .get_scalar = [rng] { return s::to_string((*rng)() & 0x7fff); },
    .set_scalar = [rng](str v) { rng->seed(static_cast<s::uint32_t>(to_int(v))); },
  });

  special(params, "LINENO", s::int64_t{0}, PM_INTEGER, {
    .get_scalar = [&ctx] { return s::to_string(ctx.lineno); },
    .set_scalar = [&ctx](str v) { ctx.lineno = static_cast<s::uint32_t>(to_int(v)); },
  });

  // $HISTCMD: the history number of the command being run; 0 without
  // history.
  special(params, "HISTCMD", s::int64_t{0}, PM_INTEGER, {
    .get_scalar = [&ctx] {
      return s::to_string(ctx.history ? ctx.history->last_num() : 0u);
    },
  });

  special(params, "status", s::int64_t{0}, PM_INTEGER | PM_READONLY, {
    .get_scalar = [&ctx] { return s::to_string(ctx.last_exit); },
  });

  special(params, "pipestatus", vec<str>{"0"}, PM_ARRAY, {
    .get_array = [&ctx] {
      vec<str> out;
      for (int rc : ctx.pipestatus) out.push_back(s::to_string(rc));
      return out;
    },
    .set_array = [&ctx](vec<str> v) {
      ctx.pipestatus.clear();
      for (const auto& e : v) ctx.pipestatus.push_back(static_cast<int>(to_int(e)));
    },
  });

  // $PPID is the shell's parent when it started, also in subshells.
  params.assign("PPID", static_cast<s::int64_t>(::getppid()),
                PM_INTEGER | PM_READONLY | PM_SPECIAL);

  special(params, "EPOCHSECONDS", s::int64_t{0}, PM_INTEGER | PM_READONLY, {
    .get_scalar = [] { return s::to_string(static_cast<long long>(s::time(nullptr))); },
  });
  special(params, "EPOCHREALTIME", str{}, PM_SCALAR | PM_READONLY, {
    .get_scalar = [] {
      struct timespec ts{};
      ::clock_gettime(CLOCK_REALTIME, &ts);
      char buf[48];
      s::snprintf(buf, sizeof buf, "%lld.%09ld", static_cast<long long>(ts.tv_sec), ts.tv_nsec);
      return str(buf);
    },
  });

  // xtrace's prefix starts as zsh's; unset, there is none.
  if (not params.lookup("PS4")) params.assign("PS4", str("+%N:%i> "));
}

}  // namespace jfsh
