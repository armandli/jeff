#include <jfsh/core/jobs.hh>
#include <algorithm>
#include <cstdio>
#include <sys/wait.h>

namespace jfsh {

// ---------------------------------------------------------------------------
// Job
// ---------------------------------------------------------------------------

int Job::exit_status() const noexcept {
    for (auto it = procs.rbegin(); it != procs.rend(); ++it)
        return it->exit_code;
    return 0;
}

// ---------------------------------------------------------------------------
// JobTable
// ---------------------------------------------------------------------------

int JobTable::find_free_slot() noexcept {
    for (int i = 1; i < static_cast<int>(table_.size()); ++i)
        if (table_[static_cast<std::size_t>(i)].job_id == 0) return i;
    return static_cast<int>(table_.size());
}

int JobTable::alloc(Pid pgid, std::uint32_t flags, std::string text) {
    int id = find_free_slot();
    if (id >= static_cast<int>(table_.size()))
        table_.resize(static_cast<std::size_t>(id + 1));
    auto& j = table_[static_cast<std::size_t>(id)];
    j.job_id = id;
    j.pgid   = pgid;
    j.flags  = flags;
    j.text   = std::move(text);
    j.status = JobStatus::Running;
    j.procs.clear();
    previous_ = current_;
    current_  = id;
    return id;
}

Job* JobTable::get(int job_id) noexcept {
    if (job_id <= 0 || job_id >= static_cast<int>(table_.size())) return nullptr;
    auto& j = table_[static_cast<std::size_t>(job_id)];
    return j.job_id ? &j : nullptr;
}

const Job* JobTable::get(int job_id) const noexcept {
    if (job_id <= 0 || job_id >= static_cast<int>(table_.size())) return nullptr;
    const auto& j = table_[static_cast<std::size_t>(job_id)];
    return j.job_id ? &j : nullptr;
}

void JobTable::add_process(int job_id, Process proc) {
    if (auto* j = get(job_id)) j->procs.push_back(std::move(proc));
}

bool JobTable::update(Pid pid, int wait_status) noexcept {
    for (auto& j : table_) {
        if (!j.job_id) continue;
        for (auto& p : j.procs) {
            if (p.pid != pid) continue;
            if (WIFEXITED(wait_status)) {
                p.status    = ProcStatus::Done;
                p.exit_code = WEXITSTATUS(wait_status);
            } else if (WIFSIGNALED(wait_status)) {
                p.status    = ProcStatus::Killed;
                p.stop_sig  = WTERMSIG(wait_status);
                p.exit_code = 128 + p.stop_sig;
            } else if (WIFSTOPPED(wait_status)) {
                p.status   = ProcStatus::Stopped;
                p.stop_sig = WSTOPSIG(wait_status);
            }
            // Update job status from all processes
            bool all_done    = true;
            bool any_stopped = false;
            for (auto const& pp : j.procs) {
                if (!pp.done())     all_done = false;
                if (pp.stopped())   any_stopped = true;
            }
            if (all_done) {
                j.status = JobStatus::Done;
                j.flags |= JOB_DONE | JOB_CHANGED;
            } else if (any_stopped) {
                j.status = JobStatus::Stopped;
                j.flags |= JOB_CHANGED;
            }
            return true;
        }
    }
    return false;
}

void JobTable::notify_changes() {
    for (auto& j : table_) {
        if (!j.job_id) continue;
        if (!(j.flags & JOB_CHANGED)) continue;
        j.flags &= ~JOB_CHANGED;
        if (j.flags & JOB_NOTIFY) {
            const char* state = j.is_done() ? "Done" : "Stopped";
            std::fprintf(stderr, "[%d]  %s\t%s\n", j.job_id, state, j.text.c_str());
        }
    }
}

void JobTable::reap() {
    for (auto& j : table_) {
        if (!j.job_id) continue;
        if ((j.flags & JOB_DONE) || (j.flags & JOB_DISOWNED)) {
            j.job_id = 0;
            j.procs.clear();
        }
    }
}

void JobTable::set_current(int jid) noexcept {
    previous_ = current_;
    current_  = jid;
}

void JobTable::for_each(std::function<void(const Job&)> fn) const {
    for (const auto& j : table_)
        if (j.job_id) fn(j);
}

int JobTable::stopped_count() const noexcept {
    int n = 0;
    for (const auto& j : table_)
        if (j.job_id && j.is_stopped()) ++n;
    return n;
}

int JobTable::running_count() const noexcept {
    int n = 0;
    for (const auto& j : table_)
        if (j.job_id && j.is_running()) ++n;
    return n;
}

} // namespace jfsh
