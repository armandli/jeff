#ifndef JFSH_HISTORY_HISTORY_H
#define JFSH_HISTORY_HISTORY_H
// jfsh/history/history.h — command history management.
//
// Mirrors zsh's hist.c.  Tracks history entries, implements history
// expansion (! bang-history), and handles HISTFILE read/write with
// SHARE_HISTORY (cross-session sharing via file locking).

#include <cstdint>
#include <ctime>
#include <sys/types.h>

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
  bool        saved     = false;  // already in the history file
};

struct HistOptions {
  int  histsize      = 1000;
  int  savehist      = 1000;
  bool append        = true;   // APPEND_HISTORY
  bool inc_append    = false;  // INC_APPEND_HISTORY(_TIME)
  bool share         = false;  // SHARE_HISTORY
  bool extended      = false;  // EXTENDED_HISTORY: `: start:elapsed;cmd`
  bool ignore_dups   = false;
  bool ignore_all    = false;
  bool ignore_space  = false;
  bool no_store      = false;
  bool no_functions  = false;  // HIST_NO_FUNCTIONS
  bool reduce_blanks = false;
  bool save_no_dups  = false;
  bool find_no_dups  = false;
  bool verify        = false;
  bool bang          = true;   // BANG_HIST
  bool fcntl_lock    = true;   // HIST_FCNTL_LOCK
  str  histchars     = "!^#";  // bang, quick substitution, comment
  str  histfile;
};

// The result of history expansion on one line.
struct HistExpansion {
  str  text;
  bool changed    = false;   // something was expanded (zsh echoes the line)
  bool print_only = false;   // a :p modifier: show the line, don't run it
};

// How save() writes the file.
enum class HistSave : s::uint8_t {
  Options,   // as HistOptions say: append the new entries, or rewrite
  Append,    // `fc -A`: append the entries not yet in the file
  Rewrite,   // `fc -W`: write the whole list
};

struct HistoryManager {
  explicit HistoryManager(HistOptions opts) : mOpts(s::move(opts)) {}

  // Returns the assigned history number, or 0 if suppressed.
  s::uint32_t add(str text, s::uint32_t elapsed = 0);
  // HIST_REDUCE_BLANKS, as zsh's histreduceblanks: `line` with the blanks
  // between its `words` ([begin, end) from the lexer, zsh's chwords)
  // closed up.  The interactive loop applies it to the lines it reads.
  static str reduce_blanks(strv line, const vec<s::pair<s::size_t, s::size_t>>& words,
                           bool keep_leading);
  // A line as zsh's hend puts it in the history: reduced (with these
  // options) only when its words were read to its end (`words` set).
  static str hend_text(strv line, const opt<vec<s::pair<s::size_t, s::size_t>>>& words,
                       const HistOptions& o);

  // Bang history: !!, !N, !-N, !str, !?str?, !#, !{…}, ^old^new, word
  // designators (:0 ^ $ * x-y %) and modifiers (:h :t :r :e :s :gs :&
  // :q :x :u :l :a :A :p).  Nothing expands inside single quotes or after
  // a backslash.
  Result<HistExpansion> expand_line(strv line);
  Result<str> expand(strv line) {
    auto r = expand_line(line);
    if (not r) return s::unexpected(r.error());
    return s::move(r->text);
  }

  optref<const HistEntry> lookup(int num) const noexcept;

  optref<const HistEntry> search(strv substr) const noexcept;

  const vec<HistEntry>& entries() const noexcept { return mEntries; }

  // Records how long the newest entry took (INC_APPEND_HISTORY_TIME and
  // EXTENDED_HISTORY write it).
  void set_last_elapsed(s::uint32_t secs) {
    if (not mEntries.empty()) mEntries.back().elapsed = secs;
  }

  // Reads a history file: plain lines, zsh's EXTENDED_HISTORY lines and
  // multi-line entries (lines ending in `\`).
  Result<void> load(strv path);
  Result<void> save(strv path, HistSave mode = HistSave::Options);
  // SHARE_HISTORY: reads what other shells appended to the file since we
  // last read or wrote it.
  Result<void> sync(strv path);

  s::size_t   size()     const noexcept { return mEntries.size(); }
  s::uint32_t last_num() const noexcept { return mNextNum - 1; }

  const HistOptions& options() const noexcept { return mOpts; }
  void set_options(HistOptions opts) {
    mOpts = s::move(opts);
    trim_to_limit();
  }

  // The words of a history entry, as word designators count them: blanks
  // separate words, quotes keep them together, and ; | & < > ( ) are
  // words of their own.
  static vec<str> split_words(strv text);

protected:
  bool should_suppress(strv text) const noexcept;
  void trim_to_limit();
  // Parses history-file text into entries (marked saved).
  vec<HistEntry> parse_file(strv data) const;
  str format_entry(const HistEntry& e) const;

private:
  HistOptions     mOpts;
  vec<HistEntry>  mEntries;
  s::uint32_t     mNextNum = 1;
  off_t           mFileOffset = 0;   // how far into HISTFILE we have read
  str             mLastSubstFrom, mLastSubstTo;   // for :& and ^old^new
  str             mLastSearch;       // for !?str? and the % designator
};

}  // namespace jfsh

#endif  // JFSH_HISTORY_HISTORY_H
