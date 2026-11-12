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
#include <vector>

#include <jfsh/core/types.h>

namespace jfsh {

struct HistEntry {
  std::uint32_t num       = 0;
  std::string   text;
  std::time_t   timestamp = 0;
  std::uint32_t elapsed   = 0;  // execution time in seconds
  bool          dup       = false;
};

struct HistOptions {
  int         histsize      = 1000;
  int         savehist      = 1000;
  bool        append        = true;
  bool        share         = false;
  bool        ignore_dups   = false;
  bool        ignore_all    = false;
  bool        ignore_space  = false;
  bool        no_store      = false;
  bool        reduce_blanks = false;
  bool        save_no_dups  = false;
  bool        find_no_dups  = false;
  bool        verify        = false;
  std::string histfile;
};

struct HistoryManager {
  explicit HistoryManager(HistOptions opts);

  // Returns the assigned history number, or 0 if suppressed.
  std::uint32_t add(std::string text, std::uint32_t elapsed = 0);

  Result<std::string> expand(std::string_view line);

  std::optional<std::reference_wrapper<const HistEntry>>
  lookup(int num) const noexcept;

  std::optional<std::reference_wrapper<const HistEntry>>
  search(std::string_view substr) const noexcept;

  const std::vector<HistEntry>& entries() const noexcept { return mEntries; }

  Result<void> load(std::string_view path);
  Result<void> save(std::string_view path);
  Result<void> sync(std::string_view path);

  std::size_t   size()     const noexcept { return mEntries.size(); }
  std::uint32_t last_num() const noexcept { return mNextNum - 1; }

  void set_options(HistOptions opts) { mOpts = std::move(opts); }

protected:
  bool should_suppress(std::string_view text) const noexcept;
  void trim_to_limit();

private:
  HistOptions             mOpts;
  std::vector<HistEntry>  mEntries;
  std::uint32_t           mNextNum = 1;
};

}  // namespace jfsh

#endif  // JFSH_HISTORY_HISTORY_H
