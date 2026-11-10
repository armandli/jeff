#pragma once
// jfsh/core/jobs.hh — job control structures.
//
// Mirrors zsh's Job/Process structs and job.c.  Every pipeline the user runs
// is a Job; each stage of the pipeline is a Process.  jfsh tracks the same
// states and signals as zsh so ^Z, fg, bg, disown, wait, etc. all work.

#include <jfsh/core/types.hh>
#include <csignal>
#include <cstdint>
#include <string>
#include <vector>

namespace jfsh {

// ---------------------------------------------------------------------------
// Process — one stage of a pipeline (mirrors zsh's Process struct)
// ---------------------------------------------------------------------------
enum class ProcStatus : std::uint8_t {
    Running,
    Stopped,
    Done,
    Killed,
};

struct Process {
    Pid         pid        = -1;
    std::string text;           // human-readable command text for jobs display
    ProcStatus  status     = ProcStatus::Running;
    int         exit_code  = 0;   // from WEXITSTATUS / WTERMSIG
    int         stop_sig   = 0;   // signal that stopped/killed it
    bool        subshell   = false; // true if forked subshell

    bool done()    const noexcept { return status == ProcStatus::Done || status == ProcStatus::Killed; }
    bool stopped() const noexcept { return status == ProcStatus::Stopped; }
};

// ---------------------------------------------------------------------------
// Job — a pipeline managed as a unit (mirrors zsh's Job struct)
// ---------------------------------------------------------------------------
enum class JobStatus : std::uint8_t {
    Running,
    Stopped,
    Done,
    Signalled,
};

// Flags on a Job (mirrors zsh STAT_* constants)
enum JobFlags : std::uint32_t {
    JOB_ASYNC     = 0x0001,  // started with & (background)
    JOB_DISOWNED  = 0x0002,  // disown'd — no longer tracked
    JOB_NOHUP     = 0x0004,  // immune to SIGHUP
    JOB_NOTIFY    = 0x0008,  // print status change notification
    JOB_CURSH     = 0x0010,  // current shell's own job (not child)
    JOB_SUBSH     = 0x0020,  // running in a subshell
    JOB_TIMED     = 0x0040,  // `time` prefix — report timing
    JOB_CHANGED   = 0x0080,  // status changed since last check
    JOB_DONE      = 0x0100,  // all processes done
};

struct Job {
    int                  job_id  = 0;
    Pid                  pgid    = -1;     // process group id
    JobStatus            status  = JobStatus::Running;
    std::uint32_t        flags   = 0;
    std::vector<Process> procs;            // pipeline stages, in order

    // Combined text shown by `jobs`
    std::string          text;
    // Wall-clock time (seconds) for `time` prefix
    double               elapsed = 0.0;

    bool is_async()    const noexcept { return flags & JOB_ASYNC; }
    bool is_done()     const noexcept { return flags & JOB_DONE; }
    bool is_stopped()  const noexcept { return status == JobStatus::Stopped; }
    bool is_running()  const noexcept { return status == JobStatus::Running; }

    // Last process exit code (exit status of the pipeline)
    int exit_status() const noexcept;
};

// ---------------------------------------------------------------------------
// JobTable — the shell's live job list (mirrors zsh's jobtab / maxjob)
// ---------------------------------------------------------------------------
class JobTable {
public:
    // Allocate a new job slot; returns job_id (1-based, like zsh).
    int  alloc(Pid pgid, std::uint32_t flags, std::string text);

    // Look up by job_id; returns nullptr if not found.
    Job*       get(int job_id)       noexcept;
    const Job* get(int job_id) const noexcept;

    // Add a process to a job.
    void add_process(int job_id, Process proc);

    // Update status after waitpid().  Returns true if status changed.
    bool update(Pid pid, int wait_status) noexcept;

    // Notify user of any changed/done jobs (for NOTIFY option).
    void notify_changes();

    // Remove done/disowned jobs.
    void reap();

    // current and previous job indices (% and %-)
    int  current()  const noexcept { return current_; }
    int  previous() const noexcept { return previous_; }
    void set_current(int jid)  noexcept;

    // Iteration over live jobs.
    void for_each(std::function<void(const Job&)>) const;

    // Number of stopped jobs (for shell exit check).
    int stopped_count() const noexcept;
    int running_count() const noexcept;

private:
    // Sparse array indexed by job_id (slot 0 unused).
    std::vector<Job>  table_;
    int               current_  = 0;
    int               previous_ = 0;

    int find_free_slot() noexcept;
};

// ---------------------------------------------------------------------------
// Global job table — one per shell instance
// ---------------------------------------------------------------------------
extern JobTable g_jobs;

} // namespace jfsh
