#ifndef JFSH_HISTORY_HISTORY_H
#define JFSH_HISTORY_HISTORY_H
// jfsh/history/history.h — command history management.
//
// Mirrors zsh's hist.c.  Tracks history entries, implements history
// expansion (! bang-history), and handles HISTFILE read/write with
// SHARE_HISTORY (cross-session sharing via file locking).

#include <cstdint>
#include <ctime>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct HistEntry {
  s::uint32_t num       = 0;
  str         text;
  s::time_t   timestamp = 0;
  s::uint32_t elapsed   = 0;  // execution time in seconds
  bool        dup       = false;
};

struct HistOptions {
  int  histsize      = 1000;
  int  savehist      = 1000;
  bool append        = true;
  bool share         = false;
  bool ignore_dups   = false;
  bool ignore_all    = false;
  bool ignore_space  = false;
  bool no_store      = false;
  bool reduce_blanks = false;
  bool save_no_dups  = false;
  bool find_no_dups  = false;
  bool verify        = false;
  str  histfile;
};

struct HistoryManager {
  explicit HistoryManager(HistOptions opts) : mOpts(s::move(opts)) {}

  // Returns the assigned history number, or 0 if suppressed.
  s::uint32_t add(str text, s::uint32_t elapsed = 0);

  Result<str> expand(strv line);

  optref<const HistEntry> lookup(int num) const noexcept;

  optref<const HistEntry> search(strv substr) const noexcept;

  const vec<HistEntry>& entries() const noexcept { return mEntries; }

  // Changes the text of the newest entry: `r` and `fc -s` leave the
  // command they ran in place of their own line, as zsh does.
  void replace_last(str text) {
    if (not mEntries.empty()) mEntries.back().text = s::move(text);
  }

  Result<void> load(strv path);
  Result<void> save(strv path);
  Result<void> sync(strv path) { return load(path); }

  s::size_t   size()     const noexcept { return mEntries.size(); }
  s::uint32_t last_num() const noexcept { return mNextNum - 1; }

  void set_options(HistOptions opts) { mOpts = s::move(opts); }

protected:
  bool should_suppress(strv text) const noexcept;
  void trim_to_limit();

private:
  HistOptions     mOpts;
  vec<HistEntry>  mEntries;
  s::uint32_t     mNextNum = 1;
};

}  // namespace jfsh

#endif  // JFSH_HISTORY_HISTORY_H
