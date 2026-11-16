#include <jfsh/core/jobs.h>

#include <cassert>
#include <cctype>
#include <cstdio>
#include <sys/wait.h>

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/core/signals.h>

namespace jfsh {

// ---- Job -------------------------------------------------------------------

bool Job::settled() const noexcept {
  return s::none_of(procs.begin(), procs.end(), [](const Process& p) {
    return p.status == ProcStatus::Running;
  });
}

int Job::exit_status(bool pipefail) const noexcept {
  if (procs.empty()) return 0;
  if (pipefail) {
    for (auto it = procs.rbegin(); it != procs.rend(); ++it)
      if (it->exit_code != 0) return it->exit_code;
    return 0;
  }
  return procs.back().exit_code;
}

static str proc_status_text(const Process& p) {
  switch (p.status) {
    case ProcStatus::Running: return "running";
    case ProcStatus::Stopped: return SignalTable::sig_message(p.stop_sig);
    case ProcStatus::Killed: {
      str s = SignalTable::sig_message(p.stop_sig);
      if (p.core) s += " (core dumped)";
      return s;
    }
    case ProcStatus::Done:
      return p.exit_code == 0 ? "done" : "exit " + s::to_string(p.exit_code);
    default:
      assert(false);  // should never get here
  }
  return "running";
}

str Job::status_text() const {
  if (procs.empty()) return "done";
  for (const auto& p : procs)
    if (p.status == ProcStatus::Running) return "running";
  for (const auto& p : procs)
    if (p.stopped()) return proc_status_text(p);
  return proc_status_text(procs.back());
}

void Job::continued() noexcept {
  for (auto& p : procs)
    if (p.stopped()) p.status = ProcStatus::Running;
  status = JobStatus::Running;
}

// ---- JobTable --------------------------------------------------------------

int JobTable::find_free_slot() noexcept {
  for (int i = 1; i < static_cast<int>(mTable.size()); ++i)
    if (mTable[static_cast<s::size_t>(i)].job_id == 0) return i;
  // Slot 0 is permanently reserved so job ids start at 1.
  int n = static_cast<int>(mTable.size());
  return n < 1 ? 1 : n;
}

int JobTable::alloc(Pid pgid, s::uint32_t flags, str text,
                    bool make_current) {
  int id = find_free_slot();
  if (id >= static_cast<int>(mTable.size()))
    mTable.resize(static_cast<s::size_t>(id + 1));
  auto& j = mTable[static_cast<s::size_t>(id)];
  j = Job{};
  j.job_id = id;
  j.pgid   = pgid;
  j.flags  = flags;
  j.text   = s::move(text);
  if (make_current) set_current(id);
  return id;
}

optref<Job> JobTable::get(int job_id) noexcept {
  if (not live(job_id)) return s::nullopt;
  return s::ref(mTable[static_cast<s::size_t>(job_id)]);
}

optref<const Job> JobTable::get(int job_id) const noexcept {
  if (not live(job_id)) return s::nullopt;
  return s::cref(mTable[static_cast<s::size_t>(job_id)]);
}

void JobTable::add_process(int job_id, Process proc) {
  if (auto j = get(job_id)) j->get().procs.push_back(s::move(proc));
}

void JobTable::remove(int job_id) noexcept {
  if (not live(job_id)) return;
  mTable[static_cast<s::size_t>(job_id)] = Job{};
  // Trailing free slots go, so the next job id is the lowest free one.
  while (mTable.size() > 1 and mTable.back().job_id == 0) mTable.pop_back();
  fix_current();
}

int JobTable::pick(int except) const noexcept {
  // A foreground job being waited for is never current or previous.
  auto eligible = [&](const Job& j) {
    return j.job_id != 0 and j.job_id != except
       and (j.is_async() or j.is_stopped());
  };
  for (auto it = mTable.rbegin(); it != mTable.rend(); ++it)
    if (eligible(*it) and it->is_stopped()) return it->job_id;
  for (auto it = mTable.rbegin(); it != mTable.rend(); ++it)
    if (eligible(*it)) return it->job_id;
  return 0;
}

void JobTable::fix_current() noexcept {
  if (not live(mCurrent)) {
    mCurrent  = live(mPrevious) ? mPrevious : 0;
    mPrevious = 0;
  }
  if (mCurrent == 0) mCurrent = pick(0);
  if (not live(mPrevious) or mPrevious == mCurrent) mPrevious = pick(mCurrent);
}

void JobTable::set_current(int jid) noexcept {
  if (jid == mCurrent) return;
  mPrevious = mCurrent;
  mCurrent  = jid;
  fix_current();
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
#ifdef WCOREDUMP
        p.core      = WCOREDUMP(wait_status);
#endif
      } else if (WIFSTOPPED(wait_status)) {
        p.status   = ProcStatus::Stopped;
        p.stop_sig = WSTOPSIG(wait_status);
      } else if (WIFCONTINUED(wait_status)) {
        p.status   = ProcStatus::Running;
      }
      if (p.done() and j.is_async()) remember(p.pid, p.exit_code);

      bool all_done = s::all_of(j.procs.begin(), j.procs.end(),
                                [](const Process& q) { return q.done(); });
      if (all_done) {
        if (not j.is_done()) j.flags |= JOB_DONE | JOB_CHANGED;
        j.status = j.procs.back().status == ProcStatus::Killed
                 ? JobStatus::Signalled : JobStatus::Done;
      } else if (j.settled()) {
        if (j.status != JobStatus::Stopped) {
          j.status = JobStatus::Stopped;
          j.flags |= JOB_CHANGED;
          set_current(j.job_id);
        }
      } else {
        j.status = JobStatus::Running;
      }
      return true;
    }
  }
  return false;
}

int JobTable::notify_changes(s::FILE* out) {
  int printed = 0;
  for (int id : ids()) {
    auto& j = mTable[static_cast<s::size_t>(id)];
    if (j.flags & JOB_CHANGED) {
      j.flags &= ~JOB_CHANGED;
      str line = format(j);
      s::fprintf(out, "%s\n", line.c_str());
      ++printed;
    }
    if (j.is_done()) remove(id);
  }
  if (printed) s::fflush(out);
  return printed;
}

void JobTable::reap() {
  for (int id : ids()) {
    const auto& j = mTable[static_cast<s::size_t>(id)];
    if (j.is_done() or (j.flags & JOB_DISOWNED)) remove(id);
  }
}

Result<int> JobTable::find(strv spec) const {
  auto fail = [](str msg) {
    return s::unexpected(ShellError{ErrorKind::ExecError, s::move(msg)});
  };
  strv s = spec;
  if (s.starts_with('%')) s.remove_prefix(1);
  if (s.empty() or s == "%" or s == "+") {
    if (not live(mCurrent)) return fail("no current job");
    return mCurrent;
  }
  if (s == "-") {
    if (not live(mPrevious)) return fail("no previous job");
    return mPrevious;
  }
  if (s::all_of(s.begin(), s.end(),
                [](char c) { return s::isdigit(static_cast<unsigned char>(c)); })) {
    int n = 0;
    for (char c : s) {
      n = n * 10 + (c - '0');
      if (n > 1'000'000) break;
    }
    auto j = get(n);
    // A foreground job being waited for is not one the user can name.
    if (not j or not (j->get().is_async() or j->get().is_stopped()))
      return fail(str(spec) + ": no such job");
    return n;
  }
  bool anywhere = s.starts_with('?');
  strv needle = anywhere ? s.substr(1) : s;
  for (auto it = mTable.rbegin(); it != mTable.rend(); ++it) {
    if (not it->job_id or not (it->is_async() or it->is_stopped())) continue;
    bool hit = anywhere ? it->text.find(needle) != str::npos
                        : strv(it->text).starts_with(needle);
    if (hit) return it->job_id;
  }
  return fail("job not found: " + str(s));
}

static str pad9(str s) {
  if (s.size() < 9) s.append(9 - s.size(), ' ');
  return s;
}

str JobTable::format(const Job& j, bool long_format, bool pgid_only) const {
  char mark = j.job_id == mCurrent ? '+' : j.job_id == mPrevious ? '-' : ' ';
  str prefix = "[" + s::to_string(j.job_id) + "]  " + mark + " ";
  if (not long_format and not pgid_only)
    return prefix + pad9(j.status_text()) + "  " + j.text;

  bool per_proc = j.procs.size() > 1
               and s::all_of(j.procs.begin(), j.procs.end(),
                             [](const Process& p) { return not p.text.empty(); });
  if (not per_proc) {
    Pid pid = pgid_only or j.procs.empty() ? j.pgid : j.procs.front().pid;
    return prefix + s::to_string(pid) + " " + pad9(j.status_text()) + "  " + j.text;
  }
  str out;
  str pgid_field = s::to_string(j.pgid) + " ";
  for (s::size_t i = 0; i < j.procs.size(); ++i) {
    const auto& p = j.procs[i];
    if (i > 0) out += '\n';
    out += i == 0 ? prefix : str(prefix.size(), ' ');
    if (pgid_only) out += i == 0 ? pgid_field : str(pgid_field.size(), ' ');
    else           out += s::to_string(p.pid) + " ";
    out += pad9(proc_status_text(p)) + "  " + p.text;
    if (i + 1 < j.procs.size()) out += " | ";
  }
  return out;
}

str JobTable::format_status(const Job& j, strv status) const {
  char mark = j.job_id == mCurrent ? '+' : j.job_id == mPrevious ? '-' : ' ';
  return "[" + s::to_string(j.job_id) + "]  " + mark + " "
       + pad9(str(status)) + "  " + j.text;
}

bool JobTable::has_changes() const noexcept {
  for (const auto& j : mTable)
    if (j.job_id and (j.flags & JOB_CHANGED)) return true;
  return false;
}

void JobTable::for_each(func<void(const Job&)> fn) const {
  for (const auto& j : mTable)
    if (j.job_id) fn(j);
}

vec<int> JobTable::ids() const {
  vec<int> v;
  for (const auto& j : mTable)
    if (j.job_id) v.push_back(j.job_id);
  return v;
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
    if (j.job_id and j.is_running() and j.is_async()) ++n;
  return n;
}

void JobTable::remember(Pid pid, int status) {
  s::erase_if(mRemembered, [&](const auto& e) { return e.first == pid; });
  if (mRemembered.size() >= 1024) mRemembered.erase(mRemembered.begin());
  mRemembered.emplace_back(pid, status);
}

opt<int> JobTable::take_remembered(Pid pid) noexcept {
  for (auto it = mRemembered.begin(); it != mRemembered.end(); ++it) {
    if (it->first != pid) continue;
    int status = it->second;
    mRemembered.erase(it);
    return status;
  }
  return s::nullopt;
}

}  // namespace jfsh
