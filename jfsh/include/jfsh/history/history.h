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
  // Kept only until the next line comes (zsh's HIST_TMPSTORE).
  bool        transient = false;
};

struct HistOptions {
  int  histsize      = 1000;
  int  savehist      = 1000;
  bool append        = true;   // APPEND_HISTORY
  bool inc_append    = false;  // INC_APPEND_HISTORY(_TIME)
  bool share         = false;  // SHARE_HISTORY
  bool extended      = false;  // EXTENDED_HISTORY (or SHARE_HISTORY): `: start:elapsed;cmd`
  bool extended_set  = false;  // EXTENDED_HISTORY itself
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
  bool csh_junkie    = false;  // CSH_JUNKIE_HISTORY
  bool comments      = false;  // INTERACTIVE_COMMENTS: `#` at a word's start
  bool fcntl_lock    = true;   // HIST_FCNTL_LOCK
  str  histchars     = "!^#";  // bang, quick substitution, comment
  str  histfile;
};

// The result of history expansion on one line.
// Where zsh's lexer is at the end of the text read so far, as far as
// history expansion cares (its STOPHIST toggles and where a comment can
// start): a stack of the contexts open, innermost last.  `q` '…' and `Q`
// $'…' (history off), `d` "…", `b` `…` and `B` a '…' inside it (history
// off), `c` $(…) or a process substitution read as a command (`depth` its
// subshells' open parentheses), `e` ${…} at command level, `E` ${…} inside
// "…" or math, `m` $((…)) or ((…)) (`depth` its open parentheses), `M`
// $[…] (`depth` its open brackets), `a` an array assignment's `=(…)`.  At
// command level (none open, `c` or `a`) gettok's and gettokstr's state:
// the word being read and whether a command can start, for the rule that
// a comment begins a token; and the here-documents opened, whose bodies
// are read without history.
struct HistQuoting {
  // gettokstr's state for the word being read.
  struct Word {
    str  text;
    int  pct    = 0;   // its open glob-group parentheses
    int  bct    = 0;   // its open braces
    int  brct   = 0;   // its open brackets
    int  intpos = 0;   // 1 at the word's first character (`=(` and `<(` start there)
    bool active    = false;   // a word is being read
    bool envstring = false;   // it holds an assignment's `=`
  };
  struct Context {
    Word word;             // for `c` and `a`: the command level's, to go back to
    int  depth  = 0;
    char kind   = 0;
    bool cmdpos = false;
    bool intypeset = false;
    bool casepat   = false;
  };
  struct HereDoc {
    str  end;
    bool strip_tabs = false;   // <<-
  };
  vec<Context> open;
  vec<HereDoc> heredocs;
  Word         word;
  int          condw     = 0;       // in [[ … ]], the words of the test so far
  char         forstate  = 0;       // after for/foreach/select, `n` its name next and `l` its
                                    // list; after function, `f` its names; after
                                    // repeat, `r` its count; after case, `C` its word
                                    // and `i` its `in`
  bool         stophist  = false;   // after !"
  bool         cmdpos    = true;    // zsh's incmdpos
  bool         intypeset = false;   // the command is typeset or the like
  bool         infor     = false;   // the token after `for`
  bool         incond    = false;   // inside [[ … ]]
  bool         casepat   = false;   // a case pattern is next (par_case's incasepat)
  bool         inredir   = false;   // the word after a redirection is next
  bool         redir_cmdpos = false;   // incmdpos before that redirection
};

struct HistExpansion {
  str         text;
  HistQuoting quoting;              // the lexer's state at the end of `text`
  bool        changed    = false;   // something was expanded (zsh echoes the line)
  bool        print_only = false;   // a :p modifier: show the line, don't run it
};

// How save() writes the file.
enum class HistSave : s::uint8_t {
  Options,   // as HistOptions say: append the new entries, or rewrite
  Incremental,   // INC_APPEND_HISTORY, SHARE_HISTORY: append the new entries
  Append,    // `fc -A`: append the whole list (-I: only entries not written)
  Rewrite,   // `fc -W`: write the whole list (-I: only entries not written)
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

  // Bang history, as zsh's histsubchar: !!, !N, !-N, !str, !?str?, !#,
  // !{…}, ^old^new (on a `first` line), word designators (:0 ^ $ * x-y %)
  // and modifiers (:h :t :r :e :s :gs :& :G :q :Q :x :u :l :a :A :P :p).
  // Nothing expands where zsh's lexer stops history (in '…', $'…', a '…'
  // inside backquotes), after a backslash, in an INTERACTIVE_COMMENTS
  // comment, or after `!"`.  `before` is the command's earlier lines (for
  // !#).  `first` is whether a line may be ^old^new: every line read is
  // (zsh's inputline sets isfirstch for each).  `quoting` is where the
  // lexer was at the end of the command's earlier lines; the result
  // carries it on to the next line.  Text holding several lines (a paste,
  // a recalled entry) is read a line at a time as the lexer reads it:
  // each newline ends a command, a comment or a here-document's line, and
  // only the first line may be ^old^new.
  Result<HistExpansion> expand_line(strv line, strv before = {}, bool first = true,
                                    HistQuoting quoting = {}) {
    return expand_lines(line, before, first, s::move(quoting), true);
  }
  // expand-history's (zsh's doexpandhist): the buffer lexed as a whole,
  // where no here-document body is read.
  Result<HistExpansion> expand_buffer(strv text) { return expand_lines(text, {}, true, {}, false); }
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
  // zsh's readhistfile; `skip_old` (fc -RI, HIST_MAKEUNIQUE) leaves out
  // the lines the list already has.
  Result<void> load(strv path, bool skip_old = false);
  // zsh's savehistfile; `skip_old` (fc -WI, fc -AI, HFILE_SKIPOLD) writes
  // only the entries not read or written yet.
  Result<void> save(strv path, HistSave mode = HistSave::Options, bool skip_old = false);
  // SHARE_HISTORY: reads what other shells appended to the file since we
  // last read or wrote it.
  Result<void> sync(strv path);

  // fc -p and fc -P (zsh's pushhiststack, pophiststack): the history
  // so far is put aside and a new, empty one numbered from 1 begins;
  // popping brings the old one back.  `level` is the function depth for
  // fc -a (-1 for none).
  void push_stack(int level);
  bool pop_stack();
  // The function depth the newest pushed history goes with, or -1.
  int  stack_level() const noexcept { return mStack.empty() ? -1 : mStack.back().level; }
  s::size_t stack_depth() const noexcept { return mStack.size(); }

  s::size_t   size()     const noexcept { return mEntries.size(); }
  s::uint32_t last_num() const noexcept { return mNextNum - 1; }

  // The :c modifier's command lookup (zsh's equalsubstr): the path a
  // command name runs, or nothing.
  using CommandLookup = func<opt<str>(const str& name)>;
  void set_command_lookup(CommandLookup f) { mFindCommand = s::move(f); }

  const HistOptions& options() const noexcept { return mOpts; }
  // Marks entry `num` as written (one that is never to be).
  void mark_written(s::uint32_t num) {
    for (auto& e : mEntries)
      if (e.num == num) e.saved = true;
  }
  void set_options(HistOptions opts) {
    mOpts = s::move(opts);
    trim_to_limit();
  }

  // The words of a history entry, as word designators count them: blanks
  // separate words, quotes keep them together, and ; | & < > ( ) are
  // words of their own.
  static vec<str> split_words(strv text);
  // Where those words are in `text`, [begin, end) each.
  static vec<s::pair<s::size_t, s::size_t>> word_positions(strv text);

protected:
  bool should_suppress(strv text) const noexcept;
  void trim_to_limit();
  // Parses history-file text into entries (marked saved).
  vec<HistEntry> parse_file(strv data) const;
  str format_entry(const HistEntry& e, bool extended) const;
  str format_entry(const HistEntry& e) const { return format_entry(e, mOpts.extended); }

private:
  Result<HistExpansion> expand_lines(strv line, strv before, bool first, HistQuoting quoting,
                                     bool bodies);
  Result<HistExpansion> expand_one(strv line, strv before, bool first, HistQuoting quoting);
  struct Saved {
    vec<HistEntry> entries;
    off_t          file_offset = 0;
    s::uint32_t    next_num = 1;
    int            level = -1;
  };
  HistOptions     mOpts;
  vec<HistEntry>  mEntries;
  vec<Saved>      mStack;
  CommandLookup   mFindCommand;
  off_t           mFileOffset = 0;   // how far into HISTFILE we have read
  // zsh's histfile_linect: lines in HISTFILE as this shell counts them (read
  // at the start or since a rewrite, and appended since).
  s::size_t       mFileLines  = 0;
  str             mLastSubstFrom, mLastSubstTo;   // for :& and ^old^new
  str             mLastSearch;       // for !?str? and the % designator
  // zsh's mev and marg: the event and word the last !?str? found.
  long            mMev  = -1;
  long            mMarg = -1;
  s::uint32_t     mNextNum = 1;
  // zsh's hsubl and hsubr are set.
  bool            mSubstLeft  = false;
  bool            mSubstRight = false;
};

}  // namespace jfsh

#endif  // JFSH_HISTORY_HISTORY_H
