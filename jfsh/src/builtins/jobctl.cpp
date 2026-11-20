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

// The trap action as `trap` lists it: bare when nothing in it needs
// quoting, otherwise in single quotes.
str trap_quote(strv s) {
  bool plain = not s.empty() and s::all_of(s.begin(), s.end(), [](char c) {
    return s::isalnum(static_cast<unsigned char>(c)) or s::strchr("_-./,:@%+", c);
  });
  if (plain) return str(s);
  str out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else           out += c;
  }
  return out + "'";
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

// The job ids named by `specs`, or the current job when there are none.
// Prints an error for each spec that names no job.
vec<int> jobs_named(const char* name, vec<str> specs,
                    ExecContext& ctx, int& rc) {
  if (specs.empty()) specs.push_back("%%");
  vec<int> ids;
  for (const auto& spec : specs) {
    auto id = ctx.jobs.find(spec);
    if (not id) {
      s::fprintf(stderr, "jfsh: %s: %s\n", name, id.error().message.c_str());
      rc = 1;
      continue;
    }
    ids.push_back(*id);
  }
  return ids;
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
      if (t.action == TrapAction::Function) {
        str name = trap_function_name(sig);
        if (auto fn = ctx.functions.find(name); fn != ctx.functions.end())
          s::printf("%s\n", function_text(name, *fn->second).c_str());
        continue;
      }
      if (t.action != TrapAction::Command and t.action != TrapAction::Ignore) continue;
      s::printf("trap -- %s %s\n", trap_quote(t.command).c_str(),
                SignalTable::sig_to_name(sig));
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
    else                ctx.signals.set_trap(*sig, TrapAction::Command, action);
  }
  return rc;
}

// ---- jobs ------------------------------------------------------------------

// jobs [-lprs] [job …]: -l adds pids, -p shows process groups, -r and -s
// list only running or stopped jobs.  Finished jobs are listed once.
int builtin_jobs(vec<str>& args, ExecContext& ctx) {
  ctx.ran_jobs = true;
  ctx.signals.reap(ctx.jobs);   // list what is true now
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
  vec<int> ids;
  if (i < args.size()) {
    ids = jobs_named("jobs", {args.begin() + static_cast<s::ptrdiff_t>(i), args.end()}, ctx, rc);
  } else {
    for (int id : ctx.jobs.ids()) {
      const Job& j = ctx.jobs.get(id)->get();
      if (j.is_async() or j.is_stopped()) ids.push_back(id);
    }
  }
  for (int id : ids) {
    auto found = ctx.jobs.get(id);
    if (not found) continue;
    Job& j = found->get();
    if (running and not j.is_running()) continue;
    if (stopped and not j.is_stopped()) continue;
    s::printf("%s\n", ctx.jobs.format(j, long_fmt, pgid).c_str());
    j.flags &= ~JOB_CHANGED;
    if (j.is_done()) ctx.jobs.remove(id);
  }
  s::fflush(stdout);
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
  for (int id : jobs_named(name, {args.begin() + 1, args.end()}, ctx, rc)) {
    auto found = ctx.jobs.get(id);
    if (not found) continue;
    if (not fg and found->get().is_running()) {
      s::fprintf(stderr, "jfsh: bg: job already in background\n");
      rc = 1;
      continue;
    }
    rc = resume_job(ctx, id, fg);
    if (ctx.aborting) break;
  }
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

// Forgets a finished job once `wait` has handed out its status.
void drop_if_done(ExecContext& ctx, int jid) {
  auto found = ctx.jobs.get(jid);
  if (not found or not found->get().is_done()) return;
  for (const auto& p : found->get().procs) (void)ctx.jobs.take_remembered(p.pid);
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

// wait [pid | job …]: with no arguments waits for every background job
// and returns 0; otherwise returns the status of the last one named.
// A trapped signal ends the wait with 128 + its number.
int builtin_wait(vec<str>& args, ExecContext& ctx) {
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

// ---- disown ----------------------------------------------------------------

// disown [job …]: forget the jobs (the current one by default), so they
// are not listed and get no SIGHUP when the shell exits.
int builtin_disown(vec<str>& args, ExecContext& ctx) {
  int rc = 0;
  for (int id : jobs_named("disown", {args.begin() + 1, args.end()}, ctx, rc)) {
    auto found = ctx.jobs.get(id);
    if (not found) continue;
    if (found->get().is_stopped())
      s::fprintf(stderr,
                 "jfsh: disown: warning: job is suspended, use `kill -CONT -%d' to resume\n",
                 static_cast<int>(found->get().pgid));
    ctx.jobs.remove(id);
  }
  return rc;
}

// ---- kill ------------------------------------------------------------------

namespace {

// A signal for kill: a number 0 … MAX_SIG or a real signal's name.
opt<int> kill_signal(strv s) {
  if (all_digits(s)) {
    if (s.size() > 3) return s::nullopt;
    int n = s::atoi(str(s).c_str());
    if (n > MAX_SIG) return s::nullopt;
    return n;
  }
  auto sig = SignalTable::name_to_sig(s);
  if (sig and is_real_signal(*sig)) return sig;
  return s::nullopt;
}

int unknown_signal(strv s, bool hint) {
  s::fprintf(stderr, "jfsh: kill: unknown signal: SIG%.*s\n",
             static_cast<int>(s.size()), s.data());
  if (hint) s::fprintf(stderr, "jfsh: kill: type kill -l for a list of signals\n");
  return 1;
}

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
  int rc = 0;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (all_digits(a)) {
      // An exit status such as 130 names the signal that caused it.
      long n = s::strtol(a.c_str(), nullptr, 10);
      if (n > 128) n -= 128;
      const char* name = n >= 1 and n <= MAX_SIG ? SignalTable::sig_to_name(static_cast<int>(n))
                                                 : "UNKNOWN";
      if (s::strcmp(name, "UNKNOWN") == 0) s::printf("%ld\n", n);
      else                                 s::printf("%s\n", name);
    } else if (auto sig = kill_signal(a)) {
      s::printf("%d\n", *sig);
    } else {
      rc = unknown_signal(a, false);
    }
  }
  return rc;
}

}  // namespace

// kill [-s SIG | -n NUM | -SIG] pid|job …, kill -l [sig …]
int builtin_kill(vec<str>& args, ExecContext& ctx) {
  int sig = SIGTERM;
  s::size_t i = 1;
  if (i < args.size() and (args[i] == "-l" or args[i] == "-L")) return kill_list(args, i + 1);
  if (i < args.size() and (args[i] == "-s" or args[i] == "-n")) {
    if (i + 1 >= args.size()) {
      s::fprintf(stderr, "jfsh: kill: -%c requires an argument\n", args[i][1]);
      return 1;
    }
    auto s = kill_signal(args[i + 1]);
    if (not s or (args[i] == "-n" and not all_digits(args[i + 1])))
      return unknown_signal(args[i + 1], true);
    sig = *s;
    i += 2;
  } else if (i < args.size() and args[i].size() > 1 and args[i][0] == '-' and args[i] != "--") {
    auto s = kill_signal(strv(args[i]).substr(1));
    if (not s) return unknown_signal(strv(args[i]).substr(1), true);
    sig = *s;
    ++i;
  }
  if (i < args.size() and args[i] == "--") ++i;
  if (i >= args.size()) {
    s::fprintf(stderr, "jfsh: kill: not enough arguments\n");
    return 1;
  }

  int rc = 0;
  for (; i < args.size(); ++i) {
    const str& target = args[i];
    if (target.starts_with('%')) {
      auto id = ctx.jobs.find(target);
      if (not id) {
        s::fprintf(stderr, "jfsh: kill: %s\n", id.error().message.c_str());
        rc = 1;
        continue;
      }
      const Job& j = ctx.jobs.get(*id)->get();
      if (auto r = signal_job(j, sig); not r) {
        s::fprintf(stderr, "jfsh: kill: kill %s failed: %s\n", target.c_str(),
                   lower_first(s::strerror(errno)).c_str());
        rc = 1;
        continue;
      }
      // A stopped job can't act on most signals until it runs again.
      bool needs_cont = sig != 0 and sig != SIGKILL and sig != SIGCONT and sig != SIGSTOP
                    and sig != SIGTSTP and sig != SIGTTIN and sig != SIGTTOU;
      if (j.is_stopped() and needs_cont) (void)signal_job(j, SIGCONT);
      continue;
    }
    strv digits = target;
    if (digits.starts_with('-')) digits.remove_prefix(1);
    if (not all_digits(digits) or digits.size() > 10) {
      s::fprintf(stderr, "jfsh: kill: illegal pid: %s\n", target.c_str());
      rc = 1;
      continue;
    }
    Pid pid = static_cast<Pid>(s::strtol(target.c_str(), nullptr, 10));
    if (::kill(pid, sig) != 0) {
      s::fprintf(stderr, "jfsh: kill: kill %s failed: %s\n", target.c_str(),
                 lower_first(s::strerror(errno)).c_str());
      rc = 1;
    }
  }
  return rc;
}

}  // namespace jfsh
