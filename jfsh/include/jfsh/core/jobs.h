#ifndef JFSH_CORE_JOBS_H
#define JFSH_CORE_JOBS_H
// jfsh/core/jobs.h — job control structures.
//
// Mirrors zsh's Job/Process structs and jobs.c.  A background command, and
// under job control every foreground pipeline, is a Job; each stage of the
// pipeline is a Process.

#include <csignal>
#include <cstdint>
#include <cstdio>
#include <termios.h>

#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>

namespace jfsh {

enum class ProcStatus : s::uint8_t {
  Running,
  Stopped,
  Done,
  Killed,
};

struct Process {
  Pid        pid       = -1;
  str        text;           // this stage's command text, for `jobs -l`
  ProcStatus status    = ProcStatus::Running;
  int        exit_code = 0;  // WEXITSTATUS, or 128 + the signal
  int        stop_sig  = 0;  // signal that stopped or killed it
  bool       core      = false;

  bool done()    const noexcept {
    return status == ProcStatus::Done or status == ProcStatus::Killed;
  }
  bool stopped() const noexcept { return status == ProcStatus::Stopped; }
};

enum class JobStatus : s::uint8_t {
  Running,
  Stopped,
  Done,
  Signalled,
};

enum JobFlags : s::uint32_t {
  JOB_ASYNC    = 0x0001,  // started with & (background)
  JOB_DISOWNED = 0x0002,  // disown'd — no longer tracked
  JOB_NOHUP    = 0x0004,  // immune to SIGHUP
  JOB_NOTIFY   = 0x0008,  // report status changes
  JOB_CURSH    = 0x0010,  // current shell's own job (not child)
  JOB_SUBSH    = 0x0020,  // running in a subshell
  JOB_TIMED    = 0x0040,  // `time` prefix — report timing
  JOB_CHANGED  = 0x0080,  // status changed since it was last reported
  JOB_DONE     = 0x0100,  // all processes done
  JOB_PGRP     = 0x0200,  // runs in a process group of its own (job control)
};

struct Job {
  int            job_id  = 0;
  Pid            pgid    = -1;
  JobStatus      status  = JobStatus::Running;
  s::uint32_t    flags   = 0;
  vec<Process>   procs;
  str            text;
  double         elapsed = 0.0;
  // Terminal modes saved when the job stopped, put back by `fg`.
  struct termios tmodes{};
  bool           has_tmodes = false;

  bool is_async()   const noexcept { return flags & JOB_ASYNC; }
  bool is_done()    const noexcept { return flags & JOB_DONE; }
  bool is_stopped() const noexcept { return status == JobStatus::Stopped; }
  bool is_running() const noexcept { return status == JobStatus::Running; }
  // No process is still running: all are done or stopped.
  bool settled()    const noexcept;

  // The last process's status, or with `pipefail` the last non-zero one.
  int exit_status(bool pipefail = false) const noexcept;
  // "running", "done", "exit 1", "suspended", "terminated",
  // "segmentation fault (core dumped)" …, as zsh words it.
  str status_text() const;
  // Marks every process running again after SIGCONT.
  void continued() noexcept;
};

struct JobTable {
  // Returns job_id (1-based, like zsh).  A background job becomes the
  // current job; a foreground one only does if it stops.
  int alloc(Pid pgid, s::uint32_t flags, str text,
            bool make_current = true);

  optref<Job> get(int job_id) noexcept;

  optref<const Job> get(int job_id) const noexcept;

  void add_process(int job_id, Process proc);

  // Frees the slot and picks new current and previous jobs.
  void remove(int job_id) noexcept;
  // Forgets every job (a forked subshell starts with none).
  void clear() noexcept {
    mTable.clear();
    mCurrent = mPrevious = 0;
    mRemembered.clear();
  }

  // Records a waitpid status.  Returns true if the pid belongs to a job.
  // A job that stops becomes the current job.
  bool update(Pid pid, int wait_status) noexcept;

  // Prints, as zsh does before a prompt, every background job whose state
  // changed and removes the ones that finished.  Returns how many lines it
  // printed.
  int  notify_changes(s::FILE* out = stderr);
  // Removes finished and disowned jobs without printing.
  void reap();

  int  current()  const noexcept { return mCurrent; }
  int  previous() const noexcept { return mPrevious; }
  void set_current(int jid) noexcept;

  // Resolves a job spec: %N, %%, %+, %-, %string, %?string.  The error is
  // zsh's message ("%3: no such job", "job not found: x", "no current
  // job").
  Result<int> find(strv spec) const;

  // A `jobs` line: "[1]  + running    sleep 10".  `long_format` adds pids
  // and puts each process of a pipeline on its own line; `pgid_only`
  // shows the process group id instead.
  str format(const Job& j, bool long_format = false,
             bool pgid_only = false) const;
  // A short line with `status` in place of the job's own ("continued").
  str format_status(const Job& j, strv status) const;
  // Some job changed state since it was last reported.
  bool has_changes() const noexcept;

  void for_each(func<void(const Job&)>) const;
  // Live job ids in ascending order.
  vec<int> ids() const;

  int stopped_count() const noexcept;
  int running_count() const noexcept;

  // Exit statuses of finished background processes, so `wait $!` works
  // after the job has been reported and removed.  Each is handed out once.
  void     remember(Pid pid, int status);
  opt<int> take_remembered(Pid pid) noexcept;

protected:
  int  find_free_slot() noexcept;
  bool live(int jid) const noexcept {
    return jid > 0 and jid < static_cast<int>(mTable.size())
       and mTable[static_cast<s::size_t>(jid)].job_id != 0;
  }
  // Picks the most recent stopped job, else the most recent job, other
  // than `except`.
  int  pick(int except) const noexcept;
  void fix_current() noexcept;

private:
  vec<Job> mTable;    // sparse array indexed by job_id (slot 0 unused)
  int      mCurrent  = 0;
  int      mPrevious = 0;
  vec<s::pair<Pid,   int>> mRemembered;
};

extern JobTable Jobs;

}  // namespace jfsh

#endif  // JFSH_CORE_JOBS_H
