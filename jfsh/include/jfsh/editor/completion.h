#ifndef JFSH_EDITOR_COMPLETION_H
#define JFSH_EDITOR_COMPLETION_H
// jfsh/editor/completion.h — tab-completion engine.
//
// Mirrors zsh's Completion/ subsystem (compsys), in much smaller form.
// The word at the cursor is found by a small shell tokenizer (quotes,
// separators, redirections, command position), then completed by context:
// after `$` parameters, after `~` user names, in command position
// commands (builtins, functions, aliases, reserved words, executables in
// $PATH), after `cd` directories, otherwise files.  `compdef fn cmd` hands
// a command's arguments to a shell function, which adds matches with
// `compadd` (or the _files / _directories helpers).
//
// Shell functions complete as zsh's zsh/complete module has them do: a
// completion widget (`zle -C`) or a compdef'd function runs with the
// special parameters ($words, $PREFIX, $compstate, …) bound to a
// CompState, compadd adds matches to it, and what the function leaves in
// $compstate says how the editor inserts and lists them.

#include <cstdint>
#include <ctime>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/param.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct CompCandidate {
  str  word;            // the match, unquoted (the part after the result's start)
  str  display;         // shown in a listing; the word when empty
  str  description;
  str  suffix = " ";    // added after a unique match: " ", "/", "}" or nothing
  char type   = 0;      // LIST_TYPES marker: / * @ | = or 0

  // What compadd adds around the word, as it goes into the line (zsh's
  // <ipre><apre><hpre>word<hsuf><asuf><isuf>): ipre is $IPREFIX and -i,
  // apre -P, hpre -p (quoted), hsuf -s (quoted), asuf -S, isuf -I and
  // $ISUFFIX.  Empty for the builtin completers.
  str  ipre, apre, hpre, hsuf, asuf, isuf;
  bool raw    = false;  // -Q: the word goes in as it is, unquoted
  bool hidden = false;  // -n: a match, but not listed
  bool line   = false;  // -l: listed on a line of its own
  bool empty  = false;  // -E: fills a place in the listing, never inserted
  bool all    = false;  // -C: inserts all the other matches
  bool exact  = false;  // the word on the line, added while compstate[exact] was set
  s::size_t group = 0;  // index in CompletionResult::groups
  // An auto-removable suffix (-q, -r, -R): the characters that remove it
  // when typed next ("\\-": any widget that inserts nothing), and a shell
  // function called with its length instead.
  str  remove_chars;
  str  remove_func;
};

// Quotes `w` for the line: backslashes, or inside an open ' or " what that
// quote needs.  `at_start`: `w` starts a word, so a leading ~ # = is quoted.
str comp_quote(strv w, char quote, bool at_start = true);
// The reverse, for a word as it is on the line (backslashes, or the text
// inside an open ' or ").
str comp_unquote(strv w, char quote);

// A group of matches (compadd -J / -V) and what is listed above it.
struct CompGroup {
  str      name;
  bool     sorted   = true;   // -J; -V keeps the order they were added in
  int      dups     = 0;      // duplicates: 0 all dropped, 1 consecutive ones (-1), 2 kept (-2)
  bool     by_match = false;  // -o: sorted by the matches, not their display strings
  vec<str> explanations;      // -X, listed when the group has matches
  vec<str> messages;          // -x, listed always
};

// One compadd call's options, with the prefixes and suffixes as they go
// into the line (compadd quotes -p and -s before they get here).
struct CompAddSpec {
  str      ipre, apre, hpre, hsuf, asuf, isuf;
  bool     has_suffix = false;   // -S given: no space after a unique match
  str      file_prefix;          // -W: where -f looks the files up
  str      group;                // -J / -V
  bool     sorted   = true;
  int      dups     = 0;
  bool     by_match = false;
  opt<str> explanation;          // -X
  opt<str> message;              // -x
  vec<str> display;              // -d
  vec<str> ignore;               // -F: patterns of words that are not matches
  bool     no_match = false;     // -U: every word is a match
  bool     raw      = false;     // -Q
  bool     files    = false;     // -f
  bool     hidden   = false;     // -n
  bool     lines    = false;     // -l
  bool     all      = false;     // -C
  int      empties  = 0;         // -E
  str      remove_chars;         // -q / -r
  str      remove_func;          // -R
  str      dir_suffix;           // -e / -f: what a directory gets instead of " "
};

// One run of a completion function: the special parameters it sees and
// changes ($words, $CURRENT, $PREFIX, $SUFFIX, $IPREFIX, $ISUFFIX,
// $QIPREFIX, $QISUFFIX, $compstate) and the matches compadd adds.
struct CompState {
  vec<str>   words;
  s::int64_t current = 0;
  str        prefix, suffix, iprefix, isuffix, qiprefix, qisuffix;
  // $compstate, less the keys computed from the matches (nmatches,
  // unambiguous, …).
  s::map<str, str> keys;
  char       quote = 0;   // the innermost quoting: 0, ', " or `

  vec<CompGroup>     groups{CompGroup{}};   // [0]: the default group
  vec<CompCandidate> matches;
  int                ignored   = 0;         // words compadd -F dropped
  int                columns   = 80;        // for list_lines
  bool               rec_exact = false;     // REC_EXACT: exact is "accept"

  // compadd: adds the words (each with the display string in
  // spec.display at the same place) that match $PREFIX and $SUFFIX, or
  // all of them with -U, and that their group doesn't have yet (-1: as
  // its last match, -2: duplicates are kept).  Returns how many words
  // matched, duplicates included.  With `matched`,
  // nothing is added: it gets each word's match, and `internal` the
  // strings that matched (compadd -O, -A, -D).
  int add(const CompAddSpec& spec, const vec<str>& list, vec<bool>* matched = nullptr,
          vec<str>* internal = nullptr);

  // $compstate as the function sees it, the computed keys included.
  umap<str, str> view() const;
  opt<str> get(const str& key) const;
  // The keys a function may not set.
  static bool readonly_key(strv key);

  s::int64_t nmatches() const;
  // The longest string all matches start with, as it goes into the line,
  // and where the cursor goes in it (0-based); a hidden suffix all
  // matches share follows the cursor.
  str unambiguous(s::size_t* cursor = nullptr) const;
  // A match was added that is accepted as exact (REC_EXACT,
  // compstate[exact]).
  bool accepts_exact() const;
  // The match that is the same as the word on the line, if any.
  opt<str> exact_string() const;
  str quoted(strv w, bool at_start = true) const { return comp_quote(w, quote, at_start); }
  // A match as it goes into the line, without the auto suffix.
  str text(const CompCandidate& m) const;
};

// What the editor puts in $compstate on entry to a completion function
// (insert, list, old_list, …); the engine adds the keys that come from
// the line (context, quote, …).
struct CompRequest {
  s::map<str, str> keys;
  bool rec_exact = false;
};

struct CompContext {
  str       line;
  s::size_t cursor = 0;
  s::size_t word_start = 0;   // where the current word starts in `line`
  s::size_t word_end   = 0;   // and ends (past the cursor, to a blank)
  str       word;             // raw text from word_start to the cursor
  str       prefix;           // that text unquoted
  str       suffix;           // raw text from the cursor to word_end
  char      quote = 0;        // the open quote the word is in: ' or "

  vec<str> args;              // the command's words so far, unquoted
  int      current_arg = 0;   // index in args of the word at the cursor
  str      command;           // args[0], or empty in command position
  bool     command_position = false;
  bool     redirection      = false;   // the word is a redirection target

  // As a completion function sees them: the command's words as they are
  // on the line (the current one whole) and the context for $compstate.
  vec<str> words;
  int      current = 0;        // 1-based index of the word at the cursor
  str      context = "command";
  str      parameter;          // for value, array_value and subscript
  str      redirect;           // the redirection operator, as typed
  // Where a completion function's word starts: after the `=` of an
  // assignment, the `[` of a subscript or the `((` of arithmetic.
  s::size_t comp_start = 0;
  bool      backtick   = false;   // inside `…`
};

struct CompletionResult {
  s::size_t start = 0;        // the text [start, end) is replaced
  s::size_t end   = 0;
  str       prefix;           // what the matches complete, unquoted
  char      quote = 0;        // the word's open quote
  vec<CompCandidate> matches;

  // From a completion function: the matches' groups (sorted, in listing
  // order), and what it left in $compstate.
  bool           function = false;
  vec<CompGroup> groups;
  str            unambiguous;          // as it goes into the line
  s::size_t      unambiguous_cursor = 0;
  s::map<str, str> keys;               // $compstate on return
};

using CompletionFn = func<vec<CompCandidate>(const CompContext&)>;

// What the engine needs from the shell.
struct CompletionSources {
  // Builtins, functions, aliases and reserved words.
  func<vec<str>()> command_names;
  // Runs shell function `fn` as a completion function, with the special
  // parameters bound to `state`; it adds matches with compadd.
  func<int(const str& fn, CompState& state)> call_function;
};

struct CompletionEngine {
  explicit CompletionEngine(const ParamTable& params) : mParams(params) {}

  CompletionResult complete(strv line, s::size_t cursor, const CompRequest& req = {});
  // A completion widget (zle -C): runs shell function `fn` for the word
  // at the cursor.
  CompletionResult complete_with(strv line, s::size_t cursor, const str& fn,
                                 const CompRequest& req);

  // compdef: `fn` (a shell function) completes `command`'s arguments.
  void compdef(str command, str fn) { mCompdefs[s::move(command)] = s::move(fn); }
  void compdef_remove(const str& command) { mCompdefs.erase(command); }
  // A completer written in C++ (tests, built-in specs).
  void compdef_fn(str command, CompletionFn fn) { mBuiltinDefs[s::move(command)] = s::move(fn); }
  void compdef_fn_remove(const str& command) { mBuiltinDefs.erase(command); }
  const s::map<str, str>& compdefs() const noexcept { return mCompdefs; }

  void set_sources(CompletionSources src) { mSources = s::move(src); }

  CompContext build_context(strv line, s::size_t cursor) const;

  // While a completion function runs: its special parameters and the
  // matches compadd adds.
  bool collecting() const noexcept { return mState != nullptr; }
  CompState* state() noexcept { return mState; }

  // Files matching the end of `prefix` (after its last /), in the
  // directory it names (~ and ~user expanded).  `dirs_only` keeps
  // directories, `exec_only` executables and directories; `pattern`
  // (a glob) keeps the files that match it, and directories.
  vec<CompCandidate> complete_files(strv prefix, bool dirs_only = false,
                                    bool exec_only = false, strv pattern = {}) const;
  vec<CompCandidate> complete_commands(strv prefix);
  vec<CompCandidate> complete_parameters(strv prefix) const;
  vec<CompCandidate> complete_users(strv prefix) const;

  static str quote_word(strv w, char quote) { return comp_quote(w, quote); }
  // Drops "" and duplicate words, then sorts.
  static void tidy(vec<CompCandidate>& v);
  // Forgets the cached $PATH listing (rehash).
  void rehash() { mPathCache.clear(); }

private:
  const ParamTable&       mParams;
  CompletionSources       mSources;
  s::map<str, str>        mCompdefs;
  HashTable<CompletionFn> mBuiltinDefs;
  CompState*              mState = nullptr;

  // The special parameters and $compstate for the word in `ctx`.
  CompState make_state(const CompContext& ctx, const CompRequest& req) const;
  // Runs `fn` on `ctx`'s word; the matches it added become the result.
  CompletionResult run_function(const CompContext& ctx, const str& fn, const CompRequest& req);

  // The executables in $PATH, per directory with its mtime.
  struct DirCache {
    str       dir;
    s::time_t mtime = 0;
    vec<str>  names;
  };
  str           mPathCacheKey;
  vec<DirCache> mPathCache;
};

}  // namespace jfsh

#endif  // JFSH_EDITOR_COMPLETION_H
