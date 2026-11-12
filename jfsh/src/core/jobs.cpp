#include <algorithm>
#include <cstdio>
#include <sys/wait.h>

#include <jfsh/core/jobs.h>

namespace jfsh {

int Job::exit_status() const noexcept {
  for (auto it = procs.rbegin(); it != procs.rend(); ++it)
    return it->exit_code;
  return 0;
}

int JobTable::find_free_slot() noexcept {
  for (int i = 1; i < static_cast<int>(mTable.size()); ++i)
    if (mTable[static_cast<std::size_t>(i)].job_id == 0) return i;
  // Slot 0 is permanently reserved so job ids start at 1.
  int n = static_cast<int>(mTable.size());
  return n < 1 ? 1 : n;
}

int JobTable::alloc(Pid pgid, std::uint32_t flags, std::string text) {
  int id = find_free_slot();
  if (id >= static_cast<int>(mTable.size()))
    mTable.resize(static_cast<std::size_t>(id + 1));
  auto& j = mTable[static_cast<std::size_t>(id)];
  j.job_id = id;
  j.pgid   = pgid;
  j.flags  = flags;
  j.text   = std::move(text);
  j.status = JobStatus::Running;
  j.procs.clear();
  mPrevious = mCurrent;
  mCurrent  = id;
  return id;
}

std::optional<std::reference_wrapper<Job>>
JobTable::get(int job_id) noexcept {
  if (job_id <= 0 or job_id >= static_cast<int>(mTable.size()))
    return std::nullopt;
  auto& j = mTable[static_cast<std::size_t>(job_id)];
  if (not j.job_id) return std::nullopt;
  return std::ref(j);
}

std::optional<std::reference_wrapper<const Job>>
JobTable::get(int job_id) const noexcept {
  if (job_id <= 0 or job_id >= static_cast<int>(mTable.size()))
    return std::nullopt;
  const auto& j = mTable[static_cast<std::size_t>(job_id)];
  if (not j.job_id) return std::nullopt;
  return std::cref(j);
}

void JobTable::add_process(int job_id, Process proc) {
  if (auto j = get(job_id)) j->get().procs.push_back(std::move(proc));
}

bool JobTable::update(Pid pid, int wait_status) noexcept {
  for (auto& j : mTable) {
    if (not j.job_id) continue;
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
      bool all_done    = true;
      bool any_stopped = false;
      for (auto const& pp : j.procs) {
        if (not pp.done())   all_done    = false;
        if (pp.stopped())    any_stopped = true;
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
  for (auto& j : mTable) {
    if (not j.job_id) continue;
    if (not (j.flags & JOB_CHANGED)) continue;
    j.flags &= ~JOB_CHANGED;
    if (j.flags & JOB_NOTIFY) {
      const char* state = j.is_done() ? "Done" : "Stopped";
      std::fprintf(stderr, "[%d]  %s\t%s\n", j.job_id, state, j.text.c_str());
    }
  }
}

void JobTable::reap() {
  for (auto& j : mTable) {
    if (not j.job_id) continue;
    if ((j.flags & JOB_DONE) or (j.flags & JOB_DISOWNED)) {
      j.job_id = 0;
      j.procs.clear();
    }
  }
}

void JobTable::set_current(int jid) noexcept {
  mPrevious = mCurrent;
  mCurrent  = jid;
}

void JobTable::for_each(std::function<void(const Job&)> fn) const {
  for (const auto& j : mTable)
    if (j.job_id) fn(j);
}

int JobTable::stopped_count() const noexcept {
  int n = 0;
  for (const auto& j : mTable)
    if (j.job_id and j.is_stopped()) ++n;
  return n;
}

int JobTable::running_count() const noexcept {
  int n = 0;
  for (const auto& j : mTable)
    if (j.job_id and j.is_running()) ++n;
  return n;
}

}  // namespace jfsh
