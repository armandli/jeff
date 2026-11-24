// Special parameters: the ones whose value the shell computes when they are
// read, as zsh's params.c does with its getfn/setfn pairs.

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <pwd.h>
#include <sys/utsname.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <memory>
#include <random>
#include <tuple>

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
  func<double()>       get_float{};
};

// Adds `name` with `flags` and the given handlers.  A value the environment
// gave it goes through its setter, and keeps it exported, unless `import`
// is false (zsh's PM_DONTIMPORT) or it is read-only.  An integer is shown
// in base 10 (`typeset -i10`), as zsh's specials are.
void special(ParamTable& params, str name, ParamValue initial, s::uint32_t flags,
             Handlers h, bool import = true) {
  opt<str> env;
  if (auto p = params.lookup(name); p and p->get().is_exported() and p->get().is_set())
    env = p->get().scalar();
  Param& p   = params.assign(s::move(name), s::move(initial), flags | PM_SPECIAL);
  if (flags & PM_INTEGER) p.base = 10;
  p.handlers = s::make_unique<ParamHandlers>();
  p.handlers->get_scalar = s::move(h.get_scalar);
  p.handlers->set_scalar = s::move(h.set_scalar);
  p.handlers->get_array  = s::move(h.get_array);
  p.handlers->set_array  = s::move(h.set_array);
  p.handlers->get_float  = s::move(h.get_float);
  if (env and import and not (flags & PM_READONLY) and p.handlers->set_scalar) {
    p.handlers->set_scalar(s::move(*env));
    p.flags |= PM_EXPORTED;
  }
}

s::int64_t to_int(const str& v) {
  return static_cast<s::int64_t>(s::strtoll(v.c_str(), nullptr, 10));
}

// The clock $SECONDS counts from, as zsh's shtimer: whole seconds and
// nanoseconds.  The seconds wrap (unsigned) as zsh's C arithmetic does, so
// SECONDS=9223372036854775807 reads back.
struct SecondsTimer {
  s::uint64_t sec  = 0;
  s::int64_t  nsec = 0;

  static SecondsTimer now() {
    auto ns = chr::duration_cast<chr::nanoseconds>(
        chr::steady_clock::now().time_since_epoch()).count();
    return {static_cast<s::uint64_t>(ns / 1'000'000'000), ns % 1'000'000'000};
  }
  // zsh's intsecondsgetfn: the whole seconds since, rounded down.
  s::int64_t whole() const {
    SecondsTimer t = now();
    return static_cast<s::int64_t>(t.sec - sec) - (t.nsec < nsec ? 1 : 0);
  }
  // floatsecondsgetfn.
  double real() const {
    SecondsTimer t = now();
    return static_cast<double>(static_cast<s::int64_t>(t.sec - sec))
         + static_cast<double>(t.nsec - nsec) / 1e9;
  }
  // intsecondssetfn / floatsecondssetfn: `x` seconds ago.
  void set(s::int64_t x) {
    *this = now();
    sec -= static_cast<s::uint64_t>(x);
  }
  void set(double x) {
    SecondsTimer t = now();
    // (A value past the 64-bit range is held at its end, where C's cast
    // would be undefined.)
    x = s::isnan(x) ? 0.0 : s::clamp(x, -9.2e18, 9.2e18);
    auto whole_x = static_cast<s::int64_t>(x);
    sec  = t.sec - static_cast<s::uint64_t>(whole_x);
    nsec = t.nsec - static_cast<s::int64_t>((x - static_cast<double>(whole_x)) * 1e9);
    if (nsec < 0) {
      nsec += 1'000'000'000;
      --sec;
    } else if (nsec >= 1'000'000'000) {
      nsec -= 1'000'000'000;
      ++sec;
    }
  }
};

}  // namespace

FuncFrame::FuncFrame(ExecContext& c, str name) : ctx(c) {
  str caller = ctx.funcstack.empty() ? ctx.arg0 : ctx.funcstack.back().name;
  ctx.funcstack.push_back({s::move(name), caller + ":" + s::to_string(ctx.lineno)});
}

void install_special_params(ExecContext& ctx) {
  ParamTable& params = ctx.params;

  // $SECONDS: seconds since the shell started; assigning sets the count.
  // `typeset -F SECONDS` makes it count fractions of a second.
  auto timer = s::make_shared<SecondsTimer>(SecondsTimer::now());
  special(params, "SECONDS", s::int64_t{0}, PM_INTEGER, {
    .get_scalar = [timer] { return s::to_string(timer->whole()); },
    .set_scalar = [timer](str v) {
      if (v.find_first_of(".eEnN") != str::npos) timer->set(s::strtod(v.c_str(), nullptr));
      else                                       timer->set(to_int(v));
    },
    .get_float  = [timer] { return timer->real(); },
  });

  // $RANDOM: 0 … 32767; assigning seeds the sequence, so it repeats.
  auto rng = s::make_shared<s::minstd_rand>(
      static_cast<s::uint32_t>(s::time(nullptr)) ^ static_cast<s::uint32_t>(::getpid()));
  special(params, "RANDOM", s::int64_t{0}, PM_INTEGER, {
    .get_scalar = [rng] { return s::to_string((*rng)() & 0x7fff); },
    .set_scalar = [rng](str v) { rng->seed(static_cast<s::uint32_t>(to_int(v))); },
  });

  // Read-only, as in zsh (also for `local LINENO`).
  special(params, "LINENO", s::int64_t{0}, PM_INTEGER | PM_READONLY, {
    .get_scalar = [&ctx] { return s::to_string(ctx.lineno); },
  });

  // $TRY_BLOCK_ERROR / $TRY_BLOCK_INTERRUPT: set for an `always` block;
  // clearing TRY_BLOCK_ERROR there cancels the error (Executor::exec_always).
  special(params, "TRY_BLOCK_ERROR", s::int64_t{-1}, PM_INTEGER, {
    .get_scalar = [&ctx] { return s::to_string(ctx.try_error); },
    .set_scalar = [&ctx](str v) { ctx.try_error = to_int(v); },
  }, /*import=*/false);
  special(params, "TRY_BLOCK_INTERRUPT", s::int64_t{-1}, PM_INTEGER, {
    .get_scalar = [&ctx] { return s::to_string(ctx.try_interrupt); },
    .set_scalar = [&ctx](str v) { ctx.try_interrupt = to_int(v); },
  }, /*import=*/false);

  // $HISTCMD: the history number of the command being run; 0 without
  // history.
  special(params, "HISTCMD", s::int64_t{0}, PM_INTEGER | PM_READONLY, {
    .get_scalar = [&ctx] {
      return s::to_string(ctx.history ? ctx.history->last_num() : 0u);
    },
  });

  // $HISTSIZE (at least 1) and $SAVEHIST (at least 0), 30 and 0 to start
  // with, as zsh's histsizesetfn and savehistsizesetfn keep them.
  for (auto [name, initial, least] : {s::tuple{"HISTSIZE", 30, 1}, s::tuple{"SAVEHIST", 0, 0}}) {
    auto value = s::make_shared<s::int64_t>(initial);
    special(params, name, s::int64_t{initial}, PM_INTEGER, {
      .get_scalar = [value] { return s::to_string(*value); },
      .set_scalar = [value, least](str v) { *value = s::max<s::int64_t>(to_int(v), least); },
    });
  }

  special(params, "status", s::int64_t{0}, PM_INTEGER | PM_READONLY, {
    .get_scalar = [&ctx] { return s::to_string(ctx.last_exit); },
  });

  special(params, "pipestatus", vec<str>{}, PM_ARRAY, {
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

  special(params, "funcstack", vec<str>{}, PM_ARRAY | PM_READONLY, {
    .get_array = [&ctx] {
      vec<str> out;
      for (auto it = ctx.funcstack.rbegin(); it != ctx.funcstack.rend(); ++it)
        out.push_back(it->name);
      return out;
    },
  });
  special(params, "functrace", vec<str>{}, PM_ARRAY | PM_READONLY, {
    .get_array = [&ctx] {
      vec<str> out;
      for (auto it = ctx.funcstack.rbegin(); it != ctx.funcstack.rend(); ++it)
        out.push_back(it->caller);
      return out;
    },
  });

  // $signals: the signal names by number from 0 (EXIT), then ZERR and
  // DEBUG.
  {
    vec<str> names{"EXIT"};
    for (int sig = 1; sig < NSIG; ++sig) names.emplace_back(SignalTable::sig_to_name(sig));
    names.emplace_back("ZERR");
    names.emplace_back("DEBUG");
    params.assign("signals", s::move(names), PM_ARRAY | PM_READONLY | PM_SPECIAL);
  }

  // $dirstack is the directory stack, without $PWD; assigning it sets the
  // stack, as zsh's.
  special(params, "dirstack", vec<str>{}, PM_ARRAY, {
    .get_array = [&ctx] { return ctx.dirstack; },
    .set_array = [&ctx](vec<str> v) { ctx.dirstack = s::move(v); },
  });

  // $_: the last word of the command before (not taken from the
  // environment).
  special(params, "_", str{}, PM_SCALAR, {
    .get_scalar = [&ctx] { return ctx.underscore; },
    .set_scalar = [&ctx](str v) { ctx.underscore = s::move(v); },
  }, /*import=*/false);

  // $zsh_scheduled_events: the `sched` events as `time:flags:command`.
  special(params, "zsh_scheduled_events", vec<str>{}, PM_ARRAY | PM_READONLY, {
    .get_array = [&ctx] {
      vec<str> out;
      for (const auto& e : ctx.loop.sched)
        out.push_back(s::to_string(static_cast<long long>(e.when)) + (e.trash_zle ? ":-o:" : "::")
                      + e.cmd);
      return out;
    },
  });

  // $argv is the positional parameters.
  special(params, "argv", vec<str>{}, PM_ARRAY, {
    .get_array = [&ctx] { return ctx.positional; },
    .set_array = [&ctx](vec<str> v) { ctx.positional = s::move(v); },
  });

  special(params, "OPTIND", s::int64_t{1}, PM_INTEGER, {
    .get_scalar = [&ctx] { return s::to_string(ctx.optind); },
    .set_scalar = [&ctx](str v) { ctx.optind = to_int(v); },
  }, /*import=*/false);
  special(params, "OPTARG", str{}, PM_SCALAR, {
    .get_scalar = [&ctx] { return ctx.optarg; },
    .set_scalar = [&ctx](str v) { ctx.optarg = s::move(v); },
  });

  // Who and where: read when used, as zsh's are.
  special(params, "UID", s::int64_t{0}, PM_INTEGER, {
    .get_scalar = [] { return s::to_string(::getuid()); },
  });
  special(params, "EUID", s::int64_t{0}, PM_INTEGER, {
    .get_scalar = [] { return s::to_string(::geteuid()); },
  });
  special(params, "GID", s::int64_t{0}, PM_INTEGER, {
    .get_scalar = [] { return s::to_string(::getgid()); },
  });
  special(params, "EGID", s::int64_t{0}, PM_INTEGER, {
    .get_scalar = [] { return s::to_string(::getegid()); },
  });
  special(params, "USERNAME", str{}, PM_SCALAR, {
    .get_scalar = [] {
      struct passwd* pw = ::getpwuid(::getuid());
      return pw and pw->pw_name ? str(pw->pw_name) : str{};
    },
  });
  // $LOGNAME, when the environment has none, as zsh sets it: the login
  // name, else the user's name.
  if (auto l = params.lookup("LOGNAME"); not l or not l->get().is_set()) {
    const char* login = ::getlogin();
    struct passwd* pw = ::getpwuid(::getuid());
    str name = login and *login ? str(login) : pw and pw->pw_name ? str(pw->pw_name) : str{};
    params.assign("LOGNAME", s::move(name), PM_SCALAR | PM_EXPORTED);
  }
  // The version of zsh whose behaviour jfsh follows, for the functions
  // that check it (compinit, is-at-least).
  params.assign("ZSH_VERSION", str("5.9"));
  params.assign("ZSH_PATCHLEVEL", str("zsh-5.9-0-g73d3173"));
  {
    str name = ctx.arg0.substr(ctx.arg0.rfind('/') == str::npos ? 0 : ctx.arg0.rfind('/') + 1);
    if (not name.empty() and name[0] == '-') name.erase(0, 1);
    params.assign("ZSH_NAME", name.empty() ? str("jfsh") : name);
  }
  char host[256] = {};
  if (::gethostname(host, sizeof host - 1) == 0) params.assign("HOST", str(host));
  // $OSTYPE and friends from uname, in the form configure gives zsh
  // (`darwin26.0`, `linux-gnu`).
  struct utsname u{};
  if (::uname(&u) == 0) {
    str sys = u.sysname;
    for (char& c : sys) c = static_cast<char>(s::tolower(static_cast<unsigned char>(c)));
    str rel = u.release;
    if (auto dot = rel.find('.'); dot != str::npos) rel = rel.substr(0, rel.find('.', dot + 1));
    str ostype = sys == "linux" ? str("linux-gnu") : sys + rel;
    if (not params.lookup("OSTYPE"))   params.assign("OSTYPE", ostype);
    if (not params.lookup("MACHTYPE")) params.assign("MACHTYPE", str(u.machine));
    if (not params.lookup("CPUTYPE"))  params.assign("CPUTYPE", str(u.machine));
    if (not params.lookup("VENDOR"))
      params.assign("VENDOR", str(sys == "darwin" ? "apple" : sys == "linux" ? "pc" : "unknown"));
  }

  // xtrace's prefix starts as zsh's; unset, there is none.
  if (not params.lookup("PS4")) params.assign("PS4", str("+%N:%i> "));
  // What a command of nothing but redirections runs, and where `=(…)`
  // puts its files, as zsh starts them.
  if (not params.lookup("NULLCMD"))     params.assign("NULLCMD", str("cat"));
  if (not params.lookup("READNULLCMD")) params.assign("READNULLCMD", str("more"));
  if (not params.lookup("TMPPREFIX"))   params.assign("TMPPREFIX", str("/tmp/zsh"));
}

}  // namespace jfsh
