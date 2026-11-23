// Job control and trap builtins: trap, jobs, fg, bg, wait, disown, kill.
// Messages and output formats follow zsh 5.9.

#include <cctype>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/parser/parser.h>

namespace jfsh {

namespace {

bool all_digits(strv s) noexcept {
  return not s.empty() and s::all_of(s.begin(), s.end(), [](char c) {
    return s::isdigit(static_cast<unsigned char>(c));
  });
}

str trap_function_name(int sig) {
  return str("TRAP") + SignalTable::sig_to_name(sig);
}

// zsh lists traps as EXIT, the real signals in order, then ZERR and DEBUG.
vec<int> trap_listing_order() {
  vec<int> order{SIGEXIT};
  for (int sig = 1; sig <= MAX_SIG; ++sig) order.push_back(sig);
  order.push_back(SIGZERR);
  order.push_back(SIGDEBUG);
  return order;
}

str lower_first(const char* msg) {
  str s = msg;
  if (not s.empty()) s[0] = static_cast<char>(s::tolower(static_cast<unsigned char>(s[0])));
  return s;
}

// Calls `fn` for each job named by `specs` (the current job when there
// are none), in order; a spec that names no job gets zsh's message, and
// the status becomes 127 (1 for the missing current job).
void for_each_named(const char* name, vec<str> specs, ExecContext& ctx, int& rc,
                    const func<void(int)>& fn) {
  bool implicit = specs.empty() or (specs.size() == 1 and specs[0] == "--");
  if (not specs.empty() and specs[0] == "--") specs.erase(specs.begin());
  if (implicit) specs = {"%%"};
  for (const auto& spec : specs) {
    auto id = ctx.jobs.find(spec);
    if (not id) {
      s::fprintf(stderr, "jfsh: %s: %s\n", name, id.error().message.c_str());
      rc = implicit ? 1 : 127;
      continue;
    }
    fn(*id);
  }
}

}  // namespace

// ---- trap ------------------------------------------------------------------

// trap                  list the traps
// trap -                reset every trap
// trap SIG …            reset these (the first word is a signal)
// trap - SIG …          reset these
// trap '' SIG …         ignore these
// trap cmd SIG …        run cmd on these
int builtin_trap(vec<str>& args, ExecContext& ctx) {
  s::size_t i = 1;
  if (i < args.size() and args[i] == "--") ++i;
  auto& traps = ctx.signals.traps();

  if (i >= args.size()) {
    for (int sig : trap_listing_order()) {
      const Trap& t = traps.get(sig);
      if (sig == SIGEXIT and not traps.exit_trap_here()) continue;
      if (t.action == TrapAction::Function) {
        str name = trap_function_name(sig);
        if (auto fn = ctx.functions.find(name); fn != ctx.functions.end())
          s::printf("%s\n", function_text(name, *fn->second).c_str());
        continue;
      }
      if (t.action != TrapAction::Command and t.action != TrapAction::Ignore) continue;
      // The command as it was parsed, as zsh's getpermtext gives it.
      str text = t.prog ? program_text(*t.prog) : t.command;
      s::printf("trap -- %s %s\n", shell_quote(text).c_str(),
                t.alias ? "ERR" : SignalTable::sig_to_name(sig));
    }
    return 0;
  }

  auto reset = [&](int sig) {
    if (traps.get(sig).action == TrapAction::Function)
      ctx.functions.erase(trap_function_name(sig));
    ctx.signals.set_trap(sig, TrapAction::Default);
  };

  str action = args[i];
  bool first_is_signal = action != "-" and SignalTable::name_to_sig(action).has_value();
  if (not first_is_signal) ++i;
  if (i >= args.size()) {
    // `trap -` resets everything; an action with no signal does nothing.
    if (action == "-")
      for (int sig : trap_listing_order())
        if (traps.get(sig).action != TrapAction::Default) reset(sig);
    return 0;
  }

  // The command is parsed now, once: one that doesn't parse sets no trap.
  ProgRef prog;
  if (not first_is_signal and action != "-" and not action.empty()) {
    auto parsed = ctx.executor->parse_string(action);
    if (not parsed) {
      ctx.executor->report_parse_error(parsed.error());
      s::fprintf(stderr, "jfsh: trap: couldn't parse trap command\n");
      return 1;
    }
    prog = s::move(*parsed);
  }
  int rc = 0;
  for (; i < args.size(); ++i) {
    auto sig = SignalTable::name_to_sig(args[i]);
    if (not sig) {
      s::fprintf(stderr, "jfsh: trap: undefined signal: %s\n", args[i].c_str());
      rc = 1;
      continue;
    }
    if (first_is_signal or action == "-") {
      reset(*sig);
      continue;
    }
    // A trap set with `trap` replaces a TRAPxxx function.
    if (traps.get(*sig).action == TrapAction::Function)
      ctx.functions.erase(trap_function_name(*sig));
    if (action.empty()) ctx.signals.set_trap(*sig, TrapAction::Ignore);
    else                ctx.signals.set_trap(*sig, TrapAction::Command, action, prog,
                                         args[i] == "ERR" or args[i] == "SIGERR");
  }
  return rc;
}

// ---- jobs ------------------------------------------------------------------

// jobs [-lprs] [job …]: -l adds pids, -p shows process groups, -r and -s
// list only running or stopped jobs.  Finished jobs are listed once.
int builtin_jobs(vec<str>& args, ExecContext& ctx) {
  ctx.ran_jobs = true;
  ctx.signals.reap(ctx.jobs);   // list what is true now
  if (not ctx.opts.monitor()) ctx.jobs.forget_finished_async();
  bool long_fmt = ctx.opts.get(OPT_LONGLISTJOBS);
  bool pgid = false, running = false, stopped = false;
  s::size_t i = 1;
  for (; i < args.size() and args[i].size() > 1 and args[i][0] == '-'; ++i) {
    if (args[i] == "--") { ++i; break; }
    for (char c : strv(args[i]).substr(1)) {
      switch (c) {
        case 'l': long_fmt = true;
        break; case 'p': pgid    = true;
        break; case 'r': running = true;
        break; case 's': stopped = true;
        break; default:
          s::fprintf(stderr, "jfsh: jobs: bad option: -%c\n", c);
          return 1;
      }
    }
  }
  int rc = 0;
  auto list = [&](int id) {
    auto found = ctx.jobs.get(id);
    if (not found) return;
    Job& j = found->get();
    if (running and not j.is_running()) return;
    if (stopped and not j.is_stopped()) return;
    s::printf("%s\n", ctx.jobs.format(j, long_fmt, pgid).c_str());
    s::fflush(stdout);
    j.flags &= ~JOB_CHANGED;
    if (j.is_done()) ctx.jobs.remove(id);
  };
  if (i < args.size()) {
    for_each_named("jobs", {args.begin() + static_cast<s::ptrdiff_t>(i), args.end()}, ctx, rc,
                   list);
  } else {
    for (int id : ctx.jobs.ids()) {
      const Job& j = ctx.jobs.get(id)->get();
      if (j.is_async() or j.is_stopped()) list(id);
    }
  }
  return rc;
}

// ---- fg / bg ---------------------------------------------------------------

static int fg_bg(vec<str>& args, ExecContext& ctx, bool fg) {
  const char* name = fg ? "fg" : "bg";
  if (not ctx.signals.job_control()) {
    s::fprintf(stderr, "jfsh: %s: no job control in this shell.\n", name);
    return 1;
  }
  int rc = 0;
  for_each_named(name, {args.begin() + 1, args.end()}, ctx, rc, [&](int id) {
    auto found = ctx.jobs.get(id);
    if (not found or ctx.aborting) return;
    if (not fg and found->get().is_running()) {
      s::fprintf(stderr, "jfsh: bg: job already in background\n");
      rc = 1;
      return;
    }
    rc = resume_job(ctx, id, fg);
  });
  return rc;
}

int builtin_fg(vec<str>& args, ExecContext& ctx) { return fg_bg(args, ctx, true); }
int builtin_bg(vec<str>& args, ExecContext& ctx) { return fg_bg(args, ctx, false); }

// ---- wait ------------------------------------------------------------------

namespace {

// A trapped signal (or Ctrl-C under job control) cut the wait short.
bool wait_interrupted(const ExecContext& ctx) {
  return ctx.signals.pending_trapped() != 0
      or (ctx.signals.job_control() and ctx.signals.pending(SIGINT));
}

// Forgets a finished job once `wait` has handed out its status.  Its
// processes' statuses stay for one `wait PID` each, as in zsh (`wait;
// wait $!`).
void drop_if_done(ExecContext& ctx, int jid) {
  auto found = ctx.jobs.get(jid);
  if (not found or not found->get().is_done()) return;
  ctx.jobs.remove(jid);
}

// Waits for every process of job `jid`; its status, or 128 + a signal.
int wait_job(ExecContext& ctx, int jid) {
  auto found = ctx.jobs.get(jid);
  if (not found) return 127;
  s::size_t n = found->get().procs.size();
  for (s::size_t i = 0; i < n; ++i) {
    int rc = wait_background_process(ctx, jid, i);
    if (wait_interrupted(ctx)) return rc;
  }
  found = ctx.jobs.get(jid);
  if (not found) return 127;
  int rc = found->get().is_stopped()
         ? 128 + found->get().procs.front().stop_sig
         : found->get().exit_status();   // the last process's, as zsh's wait
  drop_if_done(ctx, jid);
  return rc;
}

}  // namespace

// The body of `wait` (builtin_wait).
static int wait_for(vec<str>& args, ExecContext& ctx) {
  if (args.size() > 1 and args[1] == "--") args.erase(args.begin() + 1);
  if (args.size() <= 1) {
    for (int id : ctx.jobs.ids()) {
      auto found = ctx.jobs.get(id);
      if (not found or not found->get().is_async()) continue;
      int rc = wait_job(ctx, id);
      if (wait_interrupted(ctx)) return rc;
    }
    return 0;
  }
  int rc = 0;
  for (s::size_t i = 1; i < args.size(); ++i) {
    const str& arg = args[i];
    if (all_digits(arg)) {
      Pid pid = static_cast<Pid>(s::strtol(arg.c_str(), nullptr, 10));
      int jid = 0;
      s::size_t idx = 0;
      for (int id : ctx.jobs.ids()) {
        const auto& procs = ctx.jobs.get(id)->get().procs;
        for (s::size_t k = 0; k < procs.size(); ++k)
          if (procs[k].pid == pid) { jid = id; idx = k; }
      }
      if (jid) {
        rc = wait_background_process(ctx, jid, idx);
        if (wait_interrupted(ctx)) return rc;
        (void)ctx.jobs.take_remembered(pid);
        drop_if_done(ctx, jid);
      } else if (auto st = ctx.jobs.take_remembered(pid)) {
        rc = *st;
      } else {
        s::fprintf(stderr, "jfsh: wait: pid %s is not a child of this shell\n", arg.c_str());
        rc = 127;
      }
      continue;
    }
    auto id = ctx.jobs.find(arg.starts_with('%') ? arg : "%" + arg);
    if (not id) {
      s::fprintf(stderr, "jfsh: wait: %s\n", id.error().message.c_str());
      rc = 127;
      continue;
    }
    rc = wait_job(ctx, *id);
    if (wait_interrupted(ctx)) return rc;
  }
  return rc;
}

// wait [pid | job …]: with no arguments waits for every background job
// and returns 0; otherwise returns the status of the last one named.
// A trapped signal ends the wait with 128 + its number.
int builtin_wait(vec<str>& args, ExecContext& ctx) {
  bool waits = args.size() > 1;
  for (int id : ctx.jobs.ids()) waits = waits or ctx.jobs.get(id)->get().is_async();
  int rc = wait_for(args, ctx);
  if (wait_interrupted(ctx)) ctx.signal_in_builtin = true;
  // A signal whose own trap is running (this `wait` is in it) cut the wait
  // short, or was queued before the trap, and is dropped, as zsh's
  // ZSIG_IGNORED: the trap doesn't run again.  (Only when it waited.)
  if (waits and ctx.executor) ctx.executor->drop_trap_signals();
  return rc;
}

// ---- disown ----------------------------------------------------------------

// disown [job …]: forget the jobs (the current one by default), so they
// are not listed and get no SIGHUP when the shell exits.
int builtin_disown(vec<str>& args, ExecContext& ctx) {
  int rc = 0;
  for_each_named("disown", {args.begin() + 1, args.end()}, ctx, rc, [&](int id) {
    auto found = ctx.jobs.get(id);
    if (not found) return;
    if (found->get().is_stopped())
      s::fprintf(stderr,
                 "jfsh: disown: warning: job is suspended, use `kill -CONT -%d' to resume\n",
                 static_cast<int>(found->get().pgid));
    ctx.jobs.remove(id);
  });
  return rc;
}

// ---- kill ------------------------------------------------------------------

namespace {

// zsh's zstrtol in base 10: blanks, a sign, digits.  `rest` is what
// follows; `used` says whether anything at all was taken.
struct Number {
  long long value = 0;
  strv      rest;
  bool      used  = false;
};

Number read_number(strv s) {
  Number n;
  s::size_t i = 0;
  while (i < s.size() and (s[i] == ' ' or s[i] == '\t')) ++i;
  bool neg = false;
  if (i < s.size() and (s[i] == '-' or s[i] == '+')) neg = s[i++] == '-';
  for (; i < s.size() and s::isdigit(static_cast<unsigned char>(s[i])); ++i)
    if (n.value < 1'000'000'000'000LL) n.value = n.value * 10 + (s[i] - '0');
  if (neg) n.value = -n.value;
  n.used = i > 0;
  n.rest = s.substr(i);
  return n;
}

str upper(strv s) {
  str u(s);
  for (char& c : u) c = static_cast<char>(s::toupper(static_cast<unsigned char>(c)));
  return u;
}

// A real signal's name, as kill takes it: case is ignored, and so is a
// leading SIG when `sig_any_case` (`kill -sigint`), else only "SIG"
// (`kill -l SIGINT`, not `kill -l sigint`).
opt<int> kill_signal_name(strv name, bool sig_any_case) {
  strv n = name;
  if (n.size() >= 3 and (sig_any_case ? upper(n.substr(0, 3)) == "SIG" : n.starts_with("SIG")))
    n.remove_prefix(3);
  auto sig = SignalTable::bare_name_to_sig(n, /*any_case=*/true);
  if (sig and is_real_signal(*sig)) return sig;
  return s::nullopt;
}

int unknown_signal(strv name) {
  s::fprintf(stderr, "jfsh: kill: unknown signal: SIG%.*s\n",
             static_cast<int>(name.size()), name.data());
  s::fprintf(stderr, "jfsh: kill: type kill -l for a list of signals\n");
  return 1;
}

// The number of failures, as zsh's kill returns it.
int failures(int n) { return n < 126 ? n : 1; }

int kill_list(const vec<str>& args, s::size_t i) {
  if (i >= args.size()) {
    str line;
    for (int sig = 1; sig <= MAX_SIG; ++sig) {
      const char* name = SignalTable::sig_to_name(sig);
      if (s::strcmp(name, "UNKNOWN") == 0) continue;
      if (not line.empty()) line += ' ';
      line += name;
    }
    s::printf("%s\n", line.c_str());
    return 0;
  }
  int errors = 0;
  for (; i < args.size(); ++i) {
    strv a = args[i];
    Number n = read_number(a);
    if (not n.used) {
      // A name: its number.
      if (auto sig = kill_signal_name(a, false)) {
        s::printf("%d\n", *sig);
        continue;
      }
      strv rest = a.starts_with("SIG") ? a.substr(3) : a;
      s::fprintf(stderr, "jfsh: kill: unknown signal: SIG%.*s\n",
                 static_cast<int>(rest.size()), rest.data());
      ++errors;
    } else if (not n.rest.empty()) {
      s::fprintf(stderr, "jfsh: kill: unknown signal: SIG%.*s\n",
                 static_cast<int>(n.rest.size()), n.rest.data());
      ++errors;
    } else {
      // A number is an exit status, as WIFSIGNALED / WTERMSIG read it:
      // 130 names INT.
      auto sig = static_cast<int>(n.value);
      int  low = sig & 0177;
      if (low != 0 and low != 0177) sig = low;
      const char* name = sig >= 1 and sig <= MAX_SIG ? SignalTable::sig_to_name(sig)
                                                     : "UNKNOWN";
      if (s::strcmp(name, "UNKNOWN") == 0) s::printf("%d\n", sig);
      else                                 s::printf("%s\n", name);
    }
  }
  s::fflush(stdout);
  return failures(errors);
}

}  // namespace

// kill [-s SIG | -n NUM | -SIG | -NUM] [-|--] pid|job …, kill -l [sig …]
// The parsing and messages are zsh's bin_kill.
int builtin_kill(vec<str>& args, ExecContext& ctx) {
  int sig = SIGTERM;
  s::size_t i = 1;
  if (i < args.size() and args[i].starts_with('-')) {
    strv a = args[i];
    if (a == "-l") return kill_list(args, i + 1);
    if (a == "-s" or a == "-n") {
      if (i + 1 >= args.size()) {
        s::fprintf(stderr, "jfsh: kill: %s: argument expected\n", args[i].c_str());
        return 1;
      }
      strv v = args[i + 1];
      if (a == "-n") {
        Number n = read_number(v);
        if (not n.rest.empty()) {
          s::fprintf(stderr, "jfsh: kill: invalid signal number: %s\n", args[i + 1].c_str());
          return 1;
        }
        sig = static_cast<int>(n.value);
      } else if (v.empty()) {
        s::fprintf(stderr, "jfsh: kill: -: signal name expected\n");
        return 1;
      } else if (v == "0") {
        sig = 0;
      } else if (auto named = kill_signal_name(v, true)) {
        sig = *named;
      } else {
        str u = upper(v);
        return unknown_signal(strv(u).starts_with("SIG") ? strv(u).substr(3) : strv(u));
      }
      i += 2;
    } else if (a != "--") {
      strv spec = a.substr(1);
      if (spec.empty()) {
        s::fprintf(stderr, "jfsh: kill: -: signal name expected\n");
        return 1;
      }
      if (s::isdigit(static_cast<unsigned char>(spec[0]))) {
        // A number goes to kill(2) as it is: `kill -99` fails there.
        Number n = read_number(spec);
        if (not n.rest.empty()) {
          s::fprintf(stderr, "jfsh: kill: invalid signal number: %s\n", args[i].c_str());
          return 1;
        }
        sig = static_cast<int>(n.value);
      } else if (auto named = kill_signal_name(spec, true)) {
        sig = *named;
      } else {
        str u = upper(spec);
        return unknown_signal(strv(u).starts_with("SIG") ? strv(u).substr(3) : strv(u));
      }
      ++i;
    }
  }
  if (i < args.size() and (args[i] == "--" or args[i] == "-")) ++i;
  if (i >= args.size()) {
    s::fprintf(stderr, "jfsh: kill: not enough arguments\n");
    return 1;
  }

  int errors = 0;
  for (; i < args.size(); ++i) {
    const str& target = args[i];
    if (target.starts_with('%')) {
      auto id = ctx.jobs.find(target);
      if (not id) {
        s::fprintf(stderr, "jfsh: kill: %s\n", id.error().message.c_str());
        ++errors;
        continue;
      }
      const Job& j = ctx.jobs.get(*id)->get();
      if (auto r = signal_job(j, sig); not r) {
        s::fprintf(stderr, "jfsh: kill: kill %s failed: %s\n", target.c_str(),
                   lower_first(s::strerror(errno)).c_str());
        ++errors;
        continue;
      }
      // A stopped job can't act on most signals until it runs again.
      bool needs_cont = sig != 0 and sig != SIGKILL and sig != SIGCONT and sig != SIGSTOP
                    and sig != SIGTSTP and sig != SIGTTIN and sig != SIGTTOU;
      if (j.is_stopped() and needs_cont) (void)signal_job(j, SIGCONT);
      continue;
    }
    // zsh's isanum: digits and '-' only, read as atoi reads them.
    bool anum = not target.empty() and s::all_of(target.begin(), target.end(), [](char c) {
      return c == '-' or s::isdigit(static_cast<unsigned char>(c));
    });
    if (not anum) {
      s::fprintf(stderr, "jfsh: kill: illegal pid: %s\n", target.c_str());
      ++errors;
      continue;
    }
    long long value = read_number(target).value;
    if (value == ::getpid() or value == 0 or value == -::getpgrp()) ctx.signal_in_builtin = true;
    // A pid that doesn't fit is no process at all (zsh's atoi would wrap
    // it, possibly to -1: every process).
    int err = 0;
    if (value < INT32_MIN or value > INT32_MAX) err = ESRCH;
    else if (::kill(static_cast<Pid>(value), sig) != 0) err = errno;
    if (err) {
      s::fprintf(stderr, "jfsh: kill: kill %s failed: %s\n", target.c_str(),
                 lower_first(s::strerror(err)).c_str());
      ++errors;
    }
  }
  return failures(errors);
}

}  // namespace jfsh
