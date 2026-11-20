#ifndef JFSH_EDITOR_COMPMATCH_H
#define JFSH_EDITOR_COMPMATCH_H
// jfsh/editor/compmatch.h — completion matching control (compadd -M).
//
// Mirrors zsh's Src/Zle/compmatch.c and the parts of compcore.c and
// compresult.c that use it: match specifications are parsed into
// matchers, a word on the line is matched against each possible match
// with them (match_str), and every match added leaves a list of "clines"
// describing which of its parts came from the line, from anchors or from
// the match.  The lists of all matches are joined as they are added, and
// the unambiguous string is read from the joined list, so `c.s.u` with
// `r:|.=*` gives `comp.s.unix` and leaves the cursor where the matches
// differ, as in zsh.  This is done for every match (without matchers the
// lists are plain prefixes), as zsh does.
//
// Characters are bytes here (zsh's code is mostly byte-based too; its
// patterns decode multibyte characters, ours compare bytes).

#include <deque>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <jfsh/core/aliases.h>

namespace jfsh {

// One element of a matcher's pattern: a character, `?`, `[...]` or an
// equivalence class `{...}`.  A class is a list of items: a character, a
// range or a `[:name:]` class (zsh's PP_* numbers).
struct CPattern {
  enum Type : int { CClass, NClass, Equiv, Any, Char };
  struct Item {
    int kind = 0;   // 0: the character lo; PP_RANGE: lo-hi; else a [:class:]
    int lo = 0, hi = 0;
  };
  Type            tp  = Char;
  int             chr = 0;
  vec<Item>       items;
  const CPattern* next = nullptr;
};

// One matcher of a specification (`m:`, `l:`, `r:`, `b:`, `e:` and the
// upper-case forms).
struct Matcher {
  enum Flags : int { Line = 1, Left = 2, Right = 4, Inter = 8 };
  int             flags = 0;
  const CPattern* line  = nullptr;   // what matches on the line
  int             llen  = 0;
  const CPattern* word  = nullptr;   // what matches in the word
  int             wlen  = 0;         // -1: `*`, -2: `**`
  const CPattern* left  = nullptr;   // the anchors
  int             lalen = 0;
  const CPattern* right = nullptr;
  int             ralen = 0;
  const Matcher*  next  = nullptr;
};

// A parsed specification: its matchers, in order, up to an `x:`.
struct MatcherSpec {
  const Matcher* first() const noexcept { return mMatchers.empty() ? nullptr : &mMatchers.front(); }

  // Parses `spec`; an error leaves zsh's message in `err` and returns null.
  // An empty specification (or one starting with x:) has no matchers.
  static sptr<MatcherSpec> parse(strv spec, str& err);

private:
  s::deque<CPattern> mPatterns;
  s::deque<Matcher>  mMatchers;
};

// The pattern tests matchers are built on, for zsh/computil's compfiles:
// whether `p` matches the start of `s`; whether `c` matches the element
// `p` (for a {...} class, 1 + its index; `mtp` gets the kind of item that
// matched); the character an equivalence class `lp` pairs with a word
// character matched at `wind`; and a class written back as a string.
bool cpattern_match(const CPattern* p, const char* s);
int  cpattern_match1(const CPattern* p, int c, int* mtp);
int  cpattern_equivalence(const CPattern* lp, int wind, int wmtp, int wchr);
bool cclass_match(const vec<CPattern::Item>& items, int ch, int* ind, int* mtp);
str  cclass_string(const vec<CPattern::Item>& items);

struct Cline;

// The matching state of one completion run: the matchers of the compadd
// call being run, those kept for building the unambiguous string, and the
// joined cline list of the matches added so far.
struct MatchRun {
  MatchRun();
  ~MatchRun();
  MatchRun(const MatchRun&) = delete;
  MatchRun& operator=(const MatchRun&) = delete;

  // A compadd call: its matchers apply to its words.  `nmatches` is the
  // number of matches added so far in this run.
  void begin_add(sptr<MatcherSpec> spec, s::int64_t nmatches);
  void end_add();

  // compadd -p / -s: matches the hidden prefix `w` against the start of
  // `line` (the end, for a suffix) as far as the line goes.  Returns how
  // many characters of the line were used, or -1; `parts` gets the clines.
  int match_part(strv line, strv w, bool sfx, Cline** parts);

  // Matches word `w` (quoted for the line) against `pfx` and `sfx`, the
  // line's prefix and suffix.  Returns the string that goes in the line —
  // `w` with the parts upper-case matchers take from the line — or
  // nothing when it does not match; `line` gets its clines and `exact`
  // whether it is the word on the line.
  opt<str> comp_match(strv pfx, strv sfx, strv w, Cline** line, bool* exact);

  // The clines for a word added without matching (-U).
  Cline* plain(strv w);

  // Adds a match's clines to the joined list, with what compadd puts
  // around it: <qipre><ipre><pre><ppre>word<psuf><suf><isuf><qisuf>, where
  // `pline` / `sline` are the clines -p and -s matched with.
  void add(Cline* line, strv qipre, strv ipre, strv pre, strv ppre, Cline* pline,
           strv psuf, Cline* sline, strv isuf, strv qisuf);

  // The unambiguous string of the matches added, and the cursor in it
  // (0-based); `positions` gets where characters are missing or the
  // matches differ.
  str unambiguous(s::size_t* cursor = nullptr, vec<s::size_t>* positions = nullptr) const;

  bool       empty() const noexcept { return mCount == 0; }
  bool       hasmatched   = false;   // a word was added with matching
  bool       hasunmatched = false;   // or without (-U)
  int        minmlen      = 1000000; // the shortest match

private:
  struct Impl;
  uptr<Impl> mImpl;
  int        mCount = 0;
};

}  // namespace jfsh

#endif  // JFSH_EDITOR_COMPMATCH_H
