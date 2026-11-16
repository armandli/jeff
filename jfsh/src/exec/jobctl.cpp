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
    pid_t pid = ::waitpid(-j.pgid, &status, WUNTRACED);
    if (pid < 0) {
      if (errno == EINTR) continue;
      // Nothing left to wait for (already reaped): count it as done.
      for (auto& p : j.procs)
        if (p.status == ProcStatus::Running) p.status = ProcStatus::Done;
      j.status = JobStatus::Done;
      j.flags |= JOB_DONE;
      break;
    }
    ctx.jobs.update(pid, status);
  }
  sig.take_terminal();

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

  // Programs that leave the terminal in a strange state don't take the
  // shell down with them.
  sig.restore_shell_modes();
  int rc = j.exit_status(ctx.opts.get(OPT_PIPEFAIL));
  bool interrupted = s::any_of(j.procs.begin(), j.procs.end(), [](const Process& p) {
    return p.status == ProcStatus::Killed and p.stop_sig == SIGINT;
  });
  const Process& last = j.procs.back();
  if (interrupted) {
    // Ctrl-C ends the whole command line, not just this command.
    ctx.aborting = true;
  } else if (last.status == ProcStatus::Killed and last.stop_sig != SIGPIPE) {
    s::fprintf(stderr, "jfsh: %s  %s\n", pad9(j.status_text()).c_str(), j.text.c_str());
  }
  s::fflush(stderr);
  ctx.jobs.remove(jid);
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
  ctx.jobs.set_current(jid);
  str line = ctx.jobs.format_status(j, was_stopped ? "continued" : j.status_text());
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
    pid_t pid = ::waitpid(p.pid, &status, jc ? WUNTRACED : 0);
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
    if (j.is_done() or not (j.is_async() or j.is_stopped())) continue;
    if (j.flags & (JOB_DISOWNED | JOB_NOHUP)) continue;
    (void)signal_job(j, SIGHUP);
    if (j.is_stopped()) (void)signal_job(j, SIGCONT);
    ++n;
  }
  return n;
}

}  // namespace jfsh
