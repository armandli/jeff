// Job control: foreground waits under a terminal, fg/bg, signalling jobs,
// `wait` on background processes, and SIGHUP for jobs left behind.
// Mirrors the waitjob/killjb/bin_fg parts of zsh's jobs.c.

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

namespace jfsh {

static str pad9(str s) {
  if (s.size() < 9) s.append(9 - s.size(), ' ');
  return s;
}

int wait_foreground_job(ExecContext& ctx, int jid) {
  auto found = ctx.jobs.get(jid);
  if (not found) return 0;
  Job& j = found->get();
  auto& sig = ctx.signals;

  sig.give_terminal(j.pgid);
  while (not j.settled()) {
    int status = 0;
    sig.let_kept_through(true);
    pid_t pid = ::waitpid(-j.pgid, &status, WUNTRACED);
    sig.let_kept_through(false);
    if (pid < 0) {
      if (errno == EINTR) {
        // Hung up: as zsh, the shell goes at once; the job is SIGHUPed with
        // the others on the way out (hangup_jobs).
        if (sig.pending(SIGHUP) and not sig.traps().trapped(SIGHUP)
            and sig.traps().get(SIGHUP).action != TrapAction::Ignore and ctx.executor) {
          ctx.executor->handle_signals();
          if (ctx.hangup and ctx.exiting) {
            // The terminal stays the job's, so the hangup reaches it (even
            // under NO_HUP).  A job started inside a function is zsh's
            // STAT_NOPRINT one (pline_level): killrunjobs leaves it to that
            // hangup and doesn't count it.
            if (ctx.func_depth > 0) j.flags |= JOB_NOHUP;
            return 128 + SIGHUP;
          }
        }
        continue;
      }
      // Nothing left to wait for (already reaped): count it as done.
      for (auto& p : j.procs)
        if (p.status == ProcStatus::Running) p.status = ProcStatus::Done;
      j.status = JobStatus::Done;
      j.flags |= JOB_DONE;
      break;
    }
    ctx.jobs.update(pid, status);
  }
  if (ctx.executor) ctx.executor->drop_trap_signals();
  // Inside the last stage of a pipeline, which runs in the shell, the
  // terminal goes back to the pipeline's other stages.
  if (ctx.pipeline_pgid and not j.is_stopped() and ::kill(-ctx.pipeline_pgid, 0) == 0)
    sig.give_terminal(ctx.pipeline_pgid);
  else
    sig.take_terminal();

  // $pipestatus: what each stage of the foreground pipeline ended with.
  ctx.pipestatus.clear();
  for (const auto& p : j.procs)
    ctx.pipestatus.push_back(p.stopped() ? 128 + p.stop_sig : p.exit_code);

  if (j.is_stopped()) {
    // Keep the job's terminal modes for `fg`, and put the shell's back.
    j.has_tmodes = sig.read_modes(j.tmodes);
    sig.restore_shell_modes();
    j.flags &= ~JOB_CHANGED;   // reported here, not before the next prompt
    int stop = SIGTSTP;
    for (const auto& p : j.procs)
      if (p.stopped()) { stop = p.stop_sig; break; }
    // The terminal echoed ^Z without a newline.
    if (stop == SIGTSTP) s::fputc('\n', stderr);
    s::fprintf(stderr, "jfsh: %s  %s\n", pad9(j.status_text()).c_str(), j.text.c_str());
    s::fflush(stderr);
    ctx.jobs.set_current(jid);
    // Stopped inside a loop: give the prompt back rather than run the next
    // iteration in the foreground.  (zsh goes further and makes the rest
    // of the loop a suspended job of its own.)
    if (ctx.loop_depth > 0) ctx.aborting = true;
    return 128 + stop;
  }

  // As zsh: a job killed by a signal may have left the terminal in a
  // strange state, so the shell's modes go back; one that finished keeps
  // what it set (`stty intr ^X` lasts).
  bool signalled = s::any_of(j.procs.begin(), j.procs.end(), [](const Process& p) {
    return p.status == ProcStatus::Killed;
  });
  if (signalled) sig.restore_shell_modes();
  else           sig.save_shell_modes();
  int rc = j.exit_status(ctx.opts.get(OPT_PIPEFAIL));
  auto killed_by = [&](int signal) {
    return s::any_of(j.procs.begin(), j.procs.end(), [&](const Process& p) {
      return p.status == ProcStatus::Killed and p.stop_sig == signal;
    });
  };
  // As zsh's update_job: a job that Ctrl-C (or Ctrl-\) killed acts as if
  // the shell had the signal too: its trap runs, and without one the whole
  // command line ends.
  int by = killed_by(SIGINT) ? SIGINT : killed_by(SIGQUIT) ? SIGQUIT : 0;
  // A process killed by a signal other than SIGINT or SIGPIPE is reported.
  bool report = by != SIGINT
            and s::any_of(j.procs.begin(), j.procs.end(), [](const Process& p) {
                  return p.status == ProcStatus::Killed and p.stop_sig != SIGINT
                     and p.stop_sig != SIGPIPE;
                });
  if (report) s::fprintf(stderr, "%s\n", ctx.jobs.format_foreground(j).c_str());
  s::fflush(stderr);
  ctx.jobs.remove(jid);
  // (Inside the INT trap itself, nothing more: zsh's ZSIG_IGNORED.)
  if (by and not (sig.running_trap(by) and sig.traps().trapped(by))) {
    if (sig.traps().trapped(by) and ctx.executor) {
      // The terminal echoed ^C without a newline.
      if (by == SIGINT) s::fputc('\n', stderr);
      s::fflush(stderr);
      // The trap's commands don't change the job's $? and $pipestatus.
      ctx.last_exit = rc;
      vec<int> pipestatus = ctx.pipestatus;
      ctx.executor->run_trap(by);
      ctx.pipestatus = s::move(pipestatus);
      // A TRAPINT that returned non-zero ends the line with its status.
      if (ctx.aborting) rc = ctx.last_exit;
    } else {
      ctx.aborting = ctx.interrupt = true;
      if (by == SIGINT) ctx.intr_echoed = true;
    }
  }
  return rc;
}

Result<void> signal_job(const Job& job, int sig) {
  if (job.flags & JOB_PGRP) return SignalManager::kill_job(job.pgid, sig);
  for (const auto& p : job.procs) {
    if (p.done() or p.pid <= 1) continue;
    if (::kill(p.pid, sig) != 0)
      return s::unexpected(ShellError::from_errno(ErrorKind::ExecError, "kill"));
  }
  return {};
}

int resume_job(ExecContext& ctx, int jid, bool foreground) {
  auto found = ctx.jobs.get(jid);
  if (not found) return 1;
  Job& j = found->get();
  bool was_stopped = j.is_stopped();
  // The line shows the job's mark from before it became current (`fg %-`
  // says `-`), as zsh's.
  str line = ctx.jobs.format_status(j, was_stopped ? "continued" : j.status_text());
  ctx.jobs.set_current(jid);
  s::fprintf(stderr, "%s\n", line.c_str());
  s::fflush(stderr);

  if (not foreground) {
    if (auto r = signal_job(j, SIGCONT); not r) {
      s::fprintf(stderr, "jfsh: bg: %s\n", r.error().message.c_str());
      return 1;
    }
    j.continued();
    j.flags |= JOB_ASYNC;
    return 0;
  }

  j.flags &= ~JOB_ASYNC;
  ctx.signals.give_terminal(j.pgid);
  if (j.has_tmodes) ctx.signals.write_modes(j.tmodes);
  if (was_stopped) {
    if (auto r = signal_job(j, SIGCONT); not r) {
      ctx.signals.take_terminal();
      s::fprintf(stderr, "jfsh: fg: %s\n", r.error().message.c_str());
      return 1;
    }
    j.continued();
  }
  return wait_foreground_job(ctx, jid);
}

int wait_background_process(ExecContext& ctx, int jid, s::size_t proc_index) {
  bool jc = ctx.signals.job_control();
  for (;;) {
    auto found = ctx.jobs.get(jid);
    if (not found or proc_index >= found->get().procs.size()) return 127;
    Process& p = found->get().procs[proc_index];
    if (p.done())    return p.exit_code;
    if (p.stopped()) return 128 + p.stop_sig;
    // A trapped signal ends the wait, and its trap runs right after.
    if (int s = ctx.signals.pending_trapped()) return 128 + s;
    if (jc and ctx.signals.pending(SIGINT)) return 128 + SIGINT;
    int status = 0;
    ctx.signals.let_kept_through(true);
    pid_t pid = ::waitpid(p.pid, &status, jc ? WUNTRACED : 0);
    ctx.signals.let_kept_through(false);
    if (pid < 0) {
      if (errno == EINTR) continue;
      // Reaped already, without a status we know.
      p.status = ProcStatus::Done;
      return p.exit_code;
    }
    ctx.jobs.update(pid, status);
  }
}

int hangup_jobs(ExecContext& ctx) {
  if (not ctx.opts.get(OPT_HUP)) return 0;
  int n = 0;
  for (int id : ctx.jobs.ids()) {
    const Job& j = ctx.jobs.get(id)->get();
    // After a hangup, the foreground job the shell left too.
    if (j.is_done() or not (j.is_async() or j.is_stopped() or ctx.hangup)) continue;
    // As zsh's killrunjobs: a stopped job is left to the kernel (an
    // orphaned stopped group gets SIGHUP and SIGCONT) unless the shell
    // itself was hung up.
    if (j.is_stopped() and not ctx.hangup) continue;
    if (j.flags & (JOB_DISOWNED | JOB_NOHUP)) continue;
    (void)signal_job(j, SIGHUP);
    if (j.is_stopped()) (void)signal_job(j, SIGCONT);
    ++n;
  }
  return n;
}

}  // namespace jfsh
