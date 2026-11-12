#ifndef JFSH_CORE_JOBS_H
#define JFSH_CORE_JOBS_H
// jfsh/core/jobs.h — job control structures.
//
// Mirrors zsh's Job/Process structs and job.c.  Every pipeline the user runs
// is a Job; each stage of the pipeline is a Process.

#include <csignal>
#include <cstdint>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <jfsh/core/types.h>

namespace jfsh {

enum class ProcStatus : std::uint8_t {
  Running,
  Stopped,
  Done,
  Killed,
};

struct Process {
  Pid         pid       = -1;
  std::string text;           // human-readable command text for jobs display
  ProcStatus  status    = ProcStatus::Running;
  int         exit_code = 0;  // from WEXITSTATUS / WTERMSIG
  int         stop_sig  = 0;  // signal that stopped/killed it
  bool        subshell  = false;

  bool done()    const noexcept {
    return status == ProcStatus::Done or status == ProcStatus::Killed;
  }
  bool stopped() const noexcept { return status == ProcStatus::Stopped; }
};

enum class JobStatus : std::uint8_t {
  Running,
  Stopped,
  Done,
  Signalled,
};

enum JobFlags : std::uint32_t {
  JOB_ASYNC    = 0x0001,  // started with & (background)
  JOB_DISOWNED = 0x0002,  // disown'd — no longer tracked
  JOB_NOHUP    = 0x0004,  // immune to SIGHUP
  JOB_NOTIFY   = 0x0008,  // print status change notification
  JOB_CURSH    = 0x0010,  // current shell's own job (not child)
  JOB_SUBSH    = 0x0020,  // running in a subshell
  JOB_TIMED    = 0x0040,  // `time` prefix — report timing
  JOB_CHANGED  = 0x0080,  // status changed since last check
  JOB_DONE     = 0x0100,  // all processes done
};

struct Job {
  int                  job_id  = 0;
  Pid                  pgid    = -1;
  JobStatus            status  = JobStatus::Running;
  std::uint32_t        flags   = 0;
  std::vector<Process> procs;
  std::string          text;
  double               elapsed = 0.0;

  bool is_async()   const noexcept { return flags & JOB_ASYNC; }
  bool is_done()    const noexcept { return flags & JOB_DONE; }
  bool is_stopped() const noexcept { return status == JobStatus::Stopped; }
  bool is_running() const noexcept { return status == JobStatus::Running; }

  int exit_status() const noexcept;
};

struct JobTable {
  // Returns job_id (1-based, like zsh).
  int alloc(Pid pgid, std::uint32_t flags, std::string text);

  std::optional<std::reference_wrapper<Job>>
  get(int job_id) noexcept;

  std::optional<std::reference_wrapper<const Job>>
  get(int job_id) const noexcept;

  void add_process(int job_id, Process proc);

  // Returns true if status changed.
  bool update(Pid pid, int wait_status) noexcept;

  void notify_changes();
  void reap();

  int  current()  const noexcept { return mCurrent; }
  int  previous() const noexcept { return mPrevious; }
  void set_current(int jid) noexcept;

  void for_each(std::function<void(const Job&)>) const;

  int stopped_count() const noexcept;
  int running_count() const noexcept;

private:
  std::vector<Job> mTable;    // sparse array indexed by job_id (slot 0 unused)
  int              mCurrent  = 0;
  int              mPrevious = 0;

  int find_free_slot() noexcept;
};

extern JobTable Jobs;

}  // namespace jfsh

#endif  // JFSH_CORE_JOBS_H
