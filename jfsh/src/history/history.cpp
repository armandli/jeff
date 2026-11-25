// History expansion in this file is derived from zsh 5.9's Src/hist.c
// (histsubchar, getargspec, getargs, getsubsargs, subst, convamps, quote,
// quotebreak, remtpath, remlpaths, remtext, rembutext, hend's
// HIST_TMPSTORE, pushhiststack).
// zsh's copyright and licence:
//
// The Z Shell is copyright (c) 1992-2017 Paul Falstad, Richard Coleman,
// Zoltán Hidvégi, Andrew Main, Peter Stephenson, Sven Wischnowsky, and
// others.  All rights reserved.  Individual authors, whether or not
// specifically named, retain copyright in all changes; in what follows, they
// are referred to as `the Zsh Development Group'.  This is for convenience
// only and this body has no legal status.  The Z shell is distributed under
// the following licence; any provisions made in individual files take
// precedence.
//
// Permission is hereby granted, without written agreement and without
// licence or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall the Zsh Development Group be liable to any party for
// direct, indirect, special, incidental, or consequential damages arising out
// of the use of this software and its documentation, even if the Zsh
// Development Group have been advised of the possibility of such damage.
//
// The Zsh Development Group specifically disclaim any warranties, including,
// but not limited to, the implied warranties of merchantability and fitness
// for a particular purpose.  The software provided hereunder is on an "as is"
// basis, and the Zsh Development Group have no obligation to provide
// maintenance, support, updates, enhancements, or modifications.

#include <jfsh/history/history.h>

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/core/aliases.h>

namespace jfsh {

namespace {

// zsh writes its history file metafied: NUL and the bytes 0x83 (Meta)
// through 0xa2 (Marker) become Meta followed by the byte xor 32.
constexpr unsigned char Meta = 0x83, Marker = 0xa2;

str metafy(strv s) {
  str out;
  out.reserve(s.size());
  for (char ch : s) {
    auto c = static_cast<unsigned char>(ch);
    if (c == 0 or (c >= Meta and c <= Marker)) {
      out += static_cast<char>(Meta);
      out += static_cast<char>(c ^ 32);
    } else {
      out += ch;
    }
  }
  return out;
}

str unmetafy(strv s) {
  str out;
  out.reserve(s.size());
  for (s::size_t i = 0; i < s.size(); ++i) {
    if (static_cast<unsigned char>(s[i]) == Meta and i + 1 < s.size())
      out += static_cast<char>(static_cast<unsigned char>(s[++i]) ^ 32);
    else
      out += s[i];
  }
  return out;
}

ShellError hist_error(str msg) {
  return ShellError{ErrorKind::ExecError, s::move(msg)};
}

// An open, locked history file; unlocked and closed when it goes away.
struct LockedFile {
  int fd = -1;
  LockedFile(strv path, bool write, bool lock) {
    fd = ::open(str(path).c_str(), write ? O_RDWR | O_CREAT | O_CLOEXEC : O_RDONLY | O_CLOEXEC, 0600);
    if (fd < 0 or not lock) return;
    struct flock fl{};
    fl.l_type   = static_cast<short>(write ? F_WRLCK : F_RDLCK);
    fl.l_whence = SEEK_SET;
    while (::fcntl(fd, F_SETLKW, &fl) < 0 and errno == EINTR) {}
  }
  ~LockedFile() { if (fd >= 0) ::close(fd); }   // closing drops the lock
  LockedFile(const LockedFile&) = delete;
  LockedFile& operator=(const LockedFile&) = delete;

  str read_from(off_t offset) const {
    str data;
    if (::lseek(fd, offset, SEEK_SET) < 0) return data;
    char buf[8192];
    for (;;) {
      ssize_t n = ::read(fd, buf, sizeof buf);
      if (n < 0 and errno == EINTR) continue;
      if (n <= 0) break;
      data.append(buf, static_cast<s::size_t>(n));
    }
    return data;
  }
  bool write_all(strv data) const {
    while (not data.empty()) {
      ssize_t n = ::write(fd, data.data(), data.size());
      if (n < 0 and errno == EINTR) continue;
      if (n <= 0) return false;
      data.remove_prefix(static_cast<s::size_t>(n));
    }
    return true;
  }
  off_t size() const {
    struct stat st{};
    return ::fstat(fd, &st) == 0 ? st.st_size : 0;
  }
};

bool is_blank(char c) { return c == ' ' or c == '\t' or c == '\n'; }

// The first word of a command line and the rest, for HIST_NO_STORE.
bool is_history_command(strv text) {
  vec<str> w = HistoryManager::split_words(text);
  if (w.empty()) return false;
  if (w[0] == "history") return true;
  if (w[0] != "fc") return false;
  for (s::size_t i = 1; i < w.size(); ++i)
    if (w[i].starts_with('-') and w[i].find('l') != str::npos) return true;
  return false;
}

}  // namespace

vec<str> HistoryManager::split_words(strv text) {
  vec<str> words;
  str cur;
  bool in_word = false;
  auto flush = [&] {
    if (in_word) words.push_back(s::move(cur));
    cur.clear();
    in_word = false;
  };
  for (s::size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (is_blank(c)) { flush(); continue; }
    if (c == '\\' and i + 1 < text.size()) {
      cur += c;
      cur += text[++i];
      in_word = true;
      continue;
    }
    if (c == '\'' or c == '"' or c == '`') {
      auto end = text.find(c, i + 1);
      if (c == '"') {   // backslash-escaped quotes stay inside
        end = i + 1;
        while (end < text.size() and text[end] != '"') end += text[end] == '\\' ? 2 : 1;
        if (end >= text.size()) end = strv::npos;
      }
      if (end == strv::npos) end = text.size() - 1;
      cur += text.substr(i, end - i + 1);
      i = end;
      in_word = true;
      continue;
    }
    if (c == '$' and i + 1 < text.size() and text[i + 1] == '(') {
      int depth = 0;
      s::size_t j = i + 1;
      for (; j < text.size(); ++j) {
        if (text[j] == '(') ++depth;
        else if (text[j] == ')' and --depth == 0) break;
      }
      if (j >= text.size()) j = text.size() - 1;
      cur += text.substr(i, j - i + 1);
      i = j;
      in_word = true;
      continue;
    }
    if (strv(";|&<>()").find(c) != strv::npos) {
      flush();
      str op(1, c);
      // && || ;; >> << >& <& >| &> and digits glued in front stay one word
      if (i + 1 < text.size() and strv("&|;><").find(text[i + 1]) != strv::npos
          and (text[i + 1] == c or c == '>' or c == '<' or c == '&')) {
        op += text[++i];
      }
      words.push_back(s::move(op));
      continue;
    }
    cur += c;
    in_word = true;
  }
  flush();
  return words;
}

// zsh's should_ignore_line: lines kept only until the next one comes
// (HIST_TMPSTORE): HIST_IGNORE_SPACE's, and HIST_NO_STORE's `history` and
// `fc -l` lines (B40).
bool HistoryManager::should_suppress(strv text) const noexcept {
  if (mOpts.ignore_space and not text.empty() and text[0] == ' ') return true;
  return mOpts.no_store and is_history_command(text);
}

void HistoryManager::trim_to_limit() {
  s::size_t limit = static_cast<s::size_t>(s::max(mOpts.histsize, 1));
  if (mEntries.size() > limit)
    mEntries.erase(mEntries.begin(),
                   mEntries.begin() + static_cast<s::ptrdiff_t>(mEntries.size() - limit));
}

// zsh's histreduceblanks: the words close up to one blank between them
// (the first character of each gap is kept); leading blanks go, unless
// HIST_IGNORE_SPACE wants them; what follows the last word stays as it is
// unless it is only blanks.  Blanks inside a word (quotes, `$( … )`) stay.
str HistoryManager::reduce_blanks(strv line, const vec<s::pair<s::size_t, s::size_t>>& words,
                                  bool keep_leading) {
  s::size_t spacecount = 0;
  if (keep_leading)
    while (spacecount < line.size() and line[spacecount] == ' ') ++spacecount;
  str out(line.substr(0, spacecount));
  for (s::size_t i = 0; i < words.size(); ++i) {
    auto [begin, end] = words[i];
    out += line.substr(begin, end - begin);
    if (i + 1 < words.size() and words[i + 1].first > end) out += line[end];
  }
  s::size_t last = words.empty() ? 0 : words.back().second;
  strv tail = line.substr(s::min(last, line.size()));
  if (tail.find_first_not_of(" \t\n") != strv::npos) out += tail;
  return out;
}

str HistoryManager::hend_text(strv line, const opt<vec<s::pair<s::size_t, s::size_t>>>& words,
                              const HistOptions& o) {
  if (not o.reduce_blanks or not words) return str(line);
  return reduce_blanks(line, *words, o.ignore_space);
}

// zsh's hend: a line kept for now replaces the one kept before it; a
// duplicate of the last line (HIST_IGNORE_DUPS) takes its place.
s::uint32_t HistoryManager::add(str text, s::uint32_t elapsed) {
  if (text.find_first_not_of(" \t\n") == str::npos) return 0;
  bool tmp = should_suppress(text);
  if (not mEntries.empty() and mEntries.back().transient) {
    mEntries.pop_back();
    --mNextNum;
  }
  if (not tmp and (mOpts.ignore_dups or mOpts.ignore_all) and not mEntries.empty()
      and mEntries.back().text == text) {
    mEntries.back().timestamp = s::time(nullptr);
    mEntries.back().elapsed   = elapsed;
    return mEntries.back().num;
  }
  // B58: HIST_IGNORE_ALL_DUPS drops the older copy and keeps the new one.
  if (mOpts.ignore_all and not tmp)
    s::erase_if(mEntries, [&](const HistEntry& x) { return x.text == text; });
  HistEntry e;
  e.num       = mNextNum++;
  e.text      = s::move(text);
  e.timestamp = s::time(nullptr);
  e.elapsed   = elapsed;
  e.transient = tmp;
  mEntries.push_back(s::move(e));
  trim_to_limit();
  return mEntries.back().num;
}

// History expansion, as zsh's histsubchar and its helpers (Src/hist.c:
// getargspec, getargs, hconsearch, hcomsearch, getsubsargs, subst,
// convamps, quote, quotebreak, remtpath, remlpaths, remtext, rembutext),
// run over one line read.
namespace {

struct HistLine {
  strv      line;
  s::size_t pos = 0;

  bool at_end() const { return pos >= line.size(); }
  // zsh's ingetc: the next character; -1 (lexstop) at the end.
  int get() { return pos < line.size() ? static_cast<unsigned char>(line[pos++]) : -1; }
  int peek() const { return pos < line.size() ? static_cast<unsigned char>(line[pos]) : -1; }
  strv rest() const { return line.substr(s::min(pos, line.size())); }
  void unget(int c) { if (c >= 0 and pos > 0) --pos; }
};

bool hist_blank(int c) { return c == ' ' or c == '\t'; }

// zsh's remtpath: the head of a path, `count` components of it with a
// count; false (modifier failed) when it has no directory part.
bool rem_tpath(str& s, int count) {
  long e = static_cast<long>(s.size()) - 1;
  while (e >= 0 and s[static_cast<s::size_t>(e)] == '/') --e;
  if (not count)
    while (e >= 0 and s[static_cast<s::size_t>(e)] != '/') --e;
  if (e < 0) {
    s = not s.empty() and s[0] == '/' ? "/" : ".";
    return false;
  }
  if (count) {
    for (long p = 0; p < e; ++p) {
      if (s[static_cast<s::size_t>(p)] == '/') {
        if (--count <= 0) {
          if (p == 0) ++p;
          s.resize(static_cast<s::size_t>(p));
          return true;
        }
        while (p + 1 < static_cast<long>(s.size()) and s[static_cast<s::size_t>(p + 1)] == '/') ++p;
      }
    }
    return true;
  }
  while (e > 0 and s[static_cast<s::size_t>(e - 1)] == '/') --e;
  if (e == 0) {
    ++e;
    if (e < static_cast<long>(s.size()) and s[static_cast<s::size_t>(e)] == '/'
        and not (e + 1 < static_cast<long>(s.size()) and s[static_cast<s::size_t>(e + 1)] == '/'))
      ++e;
  }
  s.resize(static_cast<s::size_t>(e));
  return true;
}

// zsh's remlpaths: the tail of a path (`count` components of it).
bool rem_lpaths(str& s, int count) {
  long e = static_cast<long>(s.size()) - 1;
  if (e >= 0 and s[static_cast<s::size_t>(e)] == '/') {
    while (e >= 0 and s[static_cast<s::size_t>(e)] == '/') --e;
    s.resize(static_cast<s::size_t>(e + 1));
  }
  for (;;) {
    for (; e >= 0; --e) {
      if (s[static_cast<s::size_t>(e)] == '/') {
        if (--count > 0) {
          if (e > 0) {
            --e;
            break;
          }
          return true;
        }
        s = s.substr(static_cast<s::size_t>(e + 1));
        return true;
      }
    }
    while (e >= 0 and s[static_cast<s::size_t>(e)] == '/') --e;
    if (e <= 0) break;
  }
  return false;
}

bool rem_text(str& s) {
  for (s::size_t e = s.size(); e-- > 0 and s[e] != '/';)
    if (s[e] == '.') {
      s.resize(e);
      return true;
    }
  return false;
}

bool rem_but_text(str& s) {
  for (s::size_t e = s.size(); e-- > 0 and s[e] != '/';)
    if (s[e] == '.') {
      s = s.substr(e + 1);
      return true;
    }
  s.clear();
  return false;
}

// zsh's quote (:q): in single quotes, blanks outside quotes breaking it
// into words.
str hist_quote(strv s) {
  str out = "'";
  bool inquotes = false;
  for (s::size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\'') {
      inquotes = not inquotes;
      out += "'\\''";
    } else if (hist_blank(c) and not inquotes and not (i > 0 and s[i - 1] == '\\')) {
      out += '\'';
      out += c;
      out += '\'';
    } else {
      out += c;
    }
  }
  return out + "'";
}

// zsh's quotebreak (:x): as :q, every blank breaking it.
str hist_quote_break(strv s) {
  str out = "'";
  for (char c : s) {
    if (c == '\'') {
      out += "'\\''";
    } else if (hist_blank(c)) {
      out += '\'';
      out += c;
      out += '\'';
    } else {
      out += c;
    }
  }
  return out + "'";
}

// zsh's hdynread2: up to `stop` or the end of the line; a backslash
// takes the next character as it is.  nullopt without a delimiter (end
// of input right away).
str hist_read_to(HistLine& in, int stop) {
  str out;
  for (;;) {
    int c = in.get();
    if (c == stop or c == '\n' or c < 0) break;
    if (c == '\\') {
      c = in.get();
      if (c < 0) break;
    }
    out += static_cast<char>(c);
  }
  return out;
}

// zsh's convamps: & is what was matched, \x is x.
str conv_amps(strv out, strv in) {
  str r;
  for (s::size_t i = 0; i < out.size(); ++i) {
    if (out[i] == '\\' and i + 1 < out.size()) r += out[++i];
    else if (out[i] == '&')                    r += in;
    else                                       r += out[i];
  }
  return r;
}

// zsh's subst without HIST_SUBST_PATTERN: false when `in` isn't there.
bool hist_subst(str& s, strv in, strv out, bool global) {
  str from = in.empty() ? s : str(in);
  if (in.empty()) global = false;
  s::size_t at = s.find(from);
  if (at == str::npos) return false;
  str repl = conv_amps(out, from);
  do {
    s.replace(at, from.size(), repl);
    at = s.find(from, at + repl.size());
  } while (global and at != str::npos);
  return true;
}

}  // namespace

vec<s::pair<s::size_t, s::size_t>> HistoryManager::word_positions(strv text) {
  vec<s::pair<s::size_t, s::size_t>> spans;
  s::size_t start = 0;
  bool in_word = false;
  auto flush = [&](s::size_t end) {
    if (in_word) spans.emplace_back(start, end);
    in_word = false;
  };
  auto begin = [&](s::size_t i) {
    if (not in_word) start = i;
    in_word = true;
  };
  for (s::size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (is_blank(c)) { flush(i); continue; }
    if (c == '\\' and i + 1 < text.size()) {
      begin(i);
      ++i;
      continue;
    }
    if (c == '\'' or c == '"' or c == '`') {
      begin(i);
      auto end = text.find(c, i + 1);
      if (c == '"') {
        end = i + 1;
        while (end < text.size() and text[end] != '"') end += text[end] == '\\' ? 2 : 1;
        if (end >= text.size()) end = strv::npos;
      }
      i = end == strv::npos ? text.size() - 1 : end;
      continue;
    }
    if (c == '$' and i + 1 < text.size() and text[i + 1] == '(') {
      begin(i);
      int depth = 0;
      s::size_t j = i + 1;
      for (; j < text.size(); ++j) {
        if (text[j] == '(') ++depth;
        else if (text[j] == ')' and --depth == 0) break;
      }
      i = j >= text.size() ? text.size() - 1 : j;
      continue;
    }
    if (strv(";|&<>()").find(c) != strv::npos) {
      flush(i);
      s::size_t b = i;
      if (i + 1 < text.size() and strv("&|;><").find(text[i + 1]) != strv::npos
          and (text[i + 1] == c or c == '>' or c == '<' or c == '&'))
        ++i;
      spans.emplace_back(b, i + 1);
      continue;
    }
    begin(i);
  }
  flush(text.size());
  return spans;
}

Result<HistExpansion> HistoryManager::expand_lines(strv line, strv before, bool first,
                                                   HistQuoting quoting, bool bodies) {
  if (line.find('\n') == strv::npos) return expand_one(line, before, first, s::move(quoting));
  HistExpansion all;
  all.quoting = s::move(quoting);
  str done(before);
  for (s::size_t at = 0;;) {
    s::size_t nl = line.find('\n', at);
    strv one = line.substr(at, nl == strv::npos ? strv::npos : nl - at);
    auto r = expand_one(one, done, first and at == 0, s::move(all.quoting));
    if (not r) return r;
    all.text      += r->text;
    all.quoting    = s::move(r->quoting);
    all.changed    = all.changed or r->changed;
    all.print_only = all.print_only or r->print_only;
    done += r->text;
    if (not bodies) all.quoting.heredocs.clear();
    if (nl == strv::npos) break;
    all.text += '\n';
    done     += '\n';
    at = nl + 1;
  }
  return all;
}

Result<HistExpansion> HistoryManager::expand_one(strv line, strv before, bool first,
                                                 HistQuoting quoting) {
  HistExpansion res;
  if (not mOpts.bang or mOpts.histchars.empty()) {
    res.text    = str(line);
    res.quoting = s::move(quoting);
    return res;
  }
  int bangchar = static_cast<unsigned char>(mOpts.histchars[0]);
  int hatchar  = mOpts.histchars.size() > 1 ? static_cast<unsigned char>(mOpts.histchars[1]) : -2;
  long curhist = static_cast<long>(mNextNum);
  long defev   = curhist - 1;
  auto err = [](str msg) -> Result<HistExpansion> {
    return s::unexpected(hist_error(s::move(msg)));
  };

  // The text of event `ev` (the line so far for this one), or nullopt.
  str out;
  auto event_text = [&](long ev) -> opt<str> {
    if (ev == curhist) return str(before) + out;
    auto e = lookup(static_cast<int>(s::clamp(ev, 1L, static_cast<long>(INT_MAX))));
    if (ev <= 0 or not e) return s::nullopt;
    return e->get().text;
  };
  // zsh's getargs: words arg1 to arg2, as they are in the event.
  auto get_args = [&](const str& text, long arg1, long arg2) -> opt<str> {
    auto spans = word_positions(text);
    long nwords = static_cast<long>(spans.size());
    if (arg2 < arg1 or arg1 >= nwords or arg2 >= nwords) return s::nullopt;
    if (arg1 == 0 and arg2 == nwords - 1) return text;
    s::size_t p1 = spans[static_cast<s::size_t>(arg1)].first;
    s::size_t p2 = spans[static_cast<s::size_t>(arg2)].second;
    return text.substr(p1, p2 - p1);
  };
  auto argc_of = [&](const str& text) {
    long n = static_cast<long>(word_positions(text).size());
    return n ? n - 1 : 0;
  };

  HistLine in{line};
  HistQuoting& qs = quoting;
  bool& stophist = qs.stophist;
  int hashchar = mOpts.histchars.size() > 2 ? static_cast<unsigned char>(mOpts.histchars[2]) : '#';
  // The context the lexer is in, 0 at command level.
  auto kind = [&] { return qs.open.empty() ? '\0' : qs.open.back().kind; };
  // Where zsh's lexer has history off (STOPHIST).
  auto history_off = [&] {
    char k = kind();
    return stophist or k == 'q' or k == 'Q' or k == 'B';
  };
  auto command_level = [](char k) { return k == '\0' or k == 'c' or k == 'a'; };
  // A word read at command level ends, and incmdpos follows the token as
  // ctxtlex and the parser set it: after a reserved word that takes a
  // command, or an assignment, a command can still start; after any
  // other word it can't, and after a redirection's word it is as it was
  // before the redirection.  par_for: a for's name is no command, the
  // word after it is (`in`, or its list's `(`), and its list isn't.  In
  // [[ … ]] the third word of a test (after a binary operator) is lexed
  // with incond 2, where `(` is a glob group.
  auto end_word = [&] {
    auto& w = qs.word;
    if (not w.active) return;
    static const uset<str> keeps = {
      "if", "then", "else", "elif", "do", "while", "until", "!", "{", "time", "coproc",
      "nocorrect"};
    static const uset<str> typesets = {
      "declare", "export", "float", "integer", "local", "readonly", "typeset"};
    char forstate = s::exchange(qs.forstate, '\0');
    if (qs.inredir) {
      qs.cmdpos  = qs.redir_cmdpos;
      qs.inredir = false;
    } else if (qs.incond and w.text == "]]") {
      qs.incond = false;
      qs.cmdpos = true;
    } else if (forstate == 'n') {
      qs.cmdpos   = true;
      qs.forstate = 'l';
    } else if (forstate == 'l' and w.text == "in") {
      qs.cmdpos = false;
    } else if (forstate == 'C') {
      qs.forstate = 'i';   // par_case: its word, then `in`
    } else if (forstate == 'i' and w.text == "in") {
      qs.casepat = true;
    } else if (qs.casepat and w.text.size() > 1 and w.text.front() == '('
               and w.text.back() == ')' and w.pct == 0) {
      // A pattern in parentheses, its `)` in the word: a command follows.
      qs.casepat = false;
      qs.cmdpos  = true;
    } else if (qs.casepat and w.text == "esac") {
      qs.casepat = false;
    } else if (forstate == 'r') {
      qs.cmdpos = true;   // par_repeat: its count, then a command
    } else if (forstate == 'f') {
      // par_funcdef: names up to a `{`, then the body's commands.
      qs.cmdpos = w.text == "{";
      if (not qs.cmdpos) qs.forstate = 'f';
    } else {
      if (qs.cmdpos) {
        if (w.text == "for" or w.text == "foreach" or w.text == "select") {
          qs.infor    = w.text == "for";
          qs.forstate = 'n';
        } else if (w.text == "function" or w.text == "repeat") {
          qs.forstate = w.text[0];
        } else if (w.text == "case") {
          qs.forstate = 'C';
        } else if (w.text == "[[") {
          qs.incond = true;
          qs.condw  = -1;
        } else if (typesets.contains(w.text)) {
          qs.intypeset = true;
        }
      }
      if (qs.incond) qs.condw = w.text == "!" ? 0 : qs.condw + 1;
      qs.cmdpos = qs.cmdpos and (keeps.contains(w.text) or w.envstring);
    }
    w = {};
  };
  // A command ends (`;`, `&`, `|`, a new line; in [[ … ]] `&&` and `||`
  // end a test).
  auto separator = [&] {
    end_word();
    qs.cmdpos    = true;
    qs.intypeset = false;
    qs.inredir   = false;
    qs.forstate  = 0;
    qs.condw     = 0;
  };
  // A new line: at command level the newline ended the word (unless a
  // glob group is open in it, where gettokstr keeps it) and, but for a
  // backslash before it, separates commands.  The body of a
  // here-document is read as it is (lex.c: gethere under STOPHIST), up to
  // its end word.
  bool continued = strv(before).ends_with("\\\n");
  if (not qs.heredocs.empty()) {
    strv body = line;
    if (qs.heredocs.front().strip_tabs)
      while (not body.empty() and body.front() == '\t') body.remove_prefix(1);
    if (body == qs.heredocs.front().end) qs.heredocs.erase(qs.heredocs.begin());
    res.text    = str(line);
    res.quoting = s::move(qs);
    return res;
  }
  if (command_level(kind()) and not continued and qs.word.pct == 0) {
    if (kind() == 'a') end_word();
    else               separator();
  }
  // An assignment's name before `=` (lex.c, LX2_EQUALS): digits, or an
  // identifier with a subscript, either with a `+`.
  auto assign_name = [](strv t) {
    s::size_t i = 0;
    auto digit = [](char ch) { return s::isdigit(static_cast<unsigned char>(ch)) != 0; };
    if (not t.empty() and digit(t[0])) {
      while (i < t.size() and digit(t[i])) ++i;
    } else {
      while (i < t.size() and (s::isalnum(static_cast<unsigned char>(t[i])) or t[i] == '_')) ++i;
      if (i < t.size() and t[i] == '[') {
        for (int d = 0; i < t.size(); ++i) {
          if (t[i] == '[') ++d;
          else if (t[i] == ']' and --d == 0) {
            ++i;
            break;
          }
        }
      }
    }
    if (i < t.size() and t[i] == '+') ++i;
    return i == t.size();
  };
  // A here-document's end word, from where it starts in `out` to where it
  // ends, unquoted; its body comes on the lines after this one.
  bool hd_next = false, hd_strip = false;
  s::size_t hd_from = str::npos;
  auto heredoc_word = [&](s::size_t end) {
    strv raw = strv(out).substr(hd_from, end - hd_from);
    HistQuoting::HereDoc hd;
    hd.strip_tabs = hd_strip;
    for (s::size_t i = 0; i < raw.size(); ++i) {
      char ch = raw[i];
      if (ch == '\\' and i + 1 < raw.size()) {
        hd.end += raw[++i];
      } else if (ch == '\'' or ch == '"') {
        for (++i; i < raw.size() and raw[i] != ch; ++i) hd.end += raw[i];
      } else {
        hd.end += ch;
      }
    }
    qs.heredocs.push_back(s::move(hd));
    hd_from = str::npos;
  };
  bool isfirstch = first;
  // The lexer's transitions for `c` (lex.c: gettok, gettokstr,
  // dquote_parse for "…" and math, the backquote loop, and $(…) read as a
  // command); `c` and anything it takes with it go to `out`.  False when
  // the rest of the line is a comment, read without history.
  auto lex = [&](HistLine& src, int c) {
    char k = kind();
    auto take_next = [&] {
      int g = src.get();
      if (g >= 0) out += static_cast<char>(g);
      return g;
    };
    out += static_cast<char>(c);
    // A command read inside a word ($(…), <(…), >(…), =(…): skipcomm);
    // the word goes on after it.
    auto open_command = [&] {
      qs.open.push_back({qs.word, 0, 'c', qs.cmdpos, qs.intypeset, qs.casepat});
      qs.word      = {};
      qs.cmdpos    = true;
      qs.intypeset = false;
      qs.casepat   = false;
    };
    // `$` and what follows it: $'…', $((…)), $(…), $[…], ${…}.
    auto dollar = [&]() -> bool {
      if (c != '$') return false;
      int n = src.peek();
      bool dq_like = k == 'd' or k == 'E' or k == 'm' or k == 'M';
      if (n == '\'' and not dq_like) {
        take_next();
        qs.open.push_back({{}, 0, 'Q', false, false});
      } else if (n == '(') {
        take_next();
        if (src.peek() == '(') {
          take_next();
          qs.open.push_back({{}, 2, 'm', false, false});
        } else {
          open_command();
        }
      } else if (n == '[') {
        take_next();
        qs.open.push_back({{}, 1, 'M', false, false});
      } else if (n == '{') {
        take_next();
        qs.open.push_back({{}, 0, dq_like ? 'E' : 'e', false, false});
      } else {
        return false;
      }
      return true;
    };
    // zsh's isnumglob: <n-m>, either number left out.
    auto numglob = [&] {
      strv rest = src.rest();
      s::size_t i = 0;
      while (i < rest.size() and s::isdigit(static_cast<unsigned char>(rest[i]))) ++i;
      if (i == rest.size() or rest[i] != '-') return false;
      for (++i; i < rest.size() and s::isdigit(static_cast<unsigned char>(rest[i]));) ++i;
      return i < rest.size() and rest[i] == '>';
    };
    // A redirection operator at a token's start; a here-document's end
    // word is read with it, its body comes on the lines after this one.
    auto redirection = [&] {
      if (c == '<' and src.peek() == '<') {
        take_next();
        if (src.peek() == '<') {
          take_next();   // a here-string: its word follows
        } else {
          // A here-document: its end word is the next word, read as any
          // word is (history expanded).
          hd_strip = src.peek() == '-';
          if (hd_strip) take_next();
          hd_next = true;
        }
      } else if (c == '<') {
        if (src.peek() == '&' or src.peek() == '>') take_next();
      } else {
        if (c == '&') take_next();   // &>
        for (int i = 0; i < 3 and strv(">&|!").find(static_cast<char>(src.peek())) != strv::npos;
             ++i)
          take_next();
      }
      // ctxtlex: the redirection's word is no command.
      qs.redir_cmdpos = qs.cmdpos;
      qs.inredir      = true;
      qs.cmdpos       = false;
    };
    switch (k) {
    case 'q':
      if (c == '\'') qs.open.pop_back();

    break; case 'Q':
      if (c == '\\') take_next();
      else if (c == '\'') qs.open.pop_back();

    break; case 'b': case 'B':
      if (c == '\\') take_next();
      else if (c == '`') qs.open.pop_back();
      else if (c == '\'') qs.open.back().kind = k == 'b' ? 'B' : 'b';

    break; case 'd': case 'E':
      // dquote_parse: "…", and ${…} inside it.
      if (c == '\\') take_next();
      else if (c == '"' and k == 'd') qs.open.pop_back();
      else if (c == '"') qs.open.push_back({{}, 0, 'd', false, false});
      else if (c == '}' and k == 'E') qs.open.pop_back();
      else if (c == '`') qs.open.push_back({{}, 0, 'b', false, false});
      else (void)dollar();

    break; case 'm': case 'M':
      // Math is dquote_parse too (cmd_or_math): no comments, `'` and `"`
      // plain characters, its parentheses (or brackets) counted.
      if (c == '\\') take_next();
      else if (c == '`') qs.open.push_back({{}, 0, 'b', false, false});
      else if (dollar()) {}
      else if (c == (k == 'm' ? '(' : '[')) ++qs.open.back().depth;
      else if (c == (k == 'm' ? ')' : ']') and --qs.open.back().depth == 0) qs.open.pop_back();

    break; case 'e':
      // ${…} at command level: gettokstr inside the word, so quotes work
      // and nothing starts a comment.
      if (c == '\\') take_next();
      else if (c == '\'') qs.open.push_back({{}, 0, 'q', false, false});
      else if (c == '"') qs.open.push_back({{}, 0, 'd', false, false});
      else if (c == '`') qs.open.push_back({{}, 0, 'b', false, false});
      else if (dollar()) {}
      else if (c == '}') qs.open.pop_back();

    break; default:
      // Command level, inside $(…), or an array assignment's (…).
      for (bool again = true; again;) {
        again = false;
        auto& w = qs.word;
        if (not w.active) {
          // gettok: a token starts, and only here can a comment.
          if (hist_blank(c)) break;
          // A backslash before the line's end joins the lines before any
          // token starts.
          if (c == '\\' and src.at_end()) break;
          bool infor = s::exchange(qs.infor, false);
          if (c == hashchar and mOpts.comments) {
            // An INTERACTIVE_COMMENTS comment: read to the line's end
            // with ingetc, outside history.
            while (not src.at_end()) out += static_cast<char>(src.get());
            return false;
          }
          if (c == '&' and src.peek() == '>') {
            redirection();
            break;
          }
          if (c == ';' or c == '&' or c == '|') {
            // `;;`, `;&` and `;|` end a case's body: a pattern is next.
            int after = src.peek();
            bool body_end = c == ';' and after >= 0
                            and strv(";&|").find(static_cast<char>(after)) != strv::npos;
            if (body_end) take_next();
            separator();
            if (body_end) qs.casepat = true;
            break;
          }
          if (c == '<' or c == '>') {
            if (src.peek() != '(' and not (c == '<' and numglob())) {
              redirection();
              break;
            }
          } else if (c == '(') {
            if (src.peek() == '(' and (infor or qs.cmdpos)) {
              // ((…)) at a command's start, or for's: math; after for's a
              // command follows.
              take_next();
              qs.open.push_back({{}, 2, 'm', false, false});
              if (infor) {
                qs.forstate = 0;
                qs.cmdpos   = true;
              }
              break;
            }
            if (src.peek() == ')') {
              // INOUTPAR: a function's body follows.
              take_next();
              qs.cmdpos   = true;
              qs.forstate = 0;
              break;
            }
            if (s::exchange(qs.forstate, '\0') == 'l') {
              // A for's list in parentheses: words, then a command.
              qs.open.push_back({{}, 0, 'a', true, qs.intypeset});
              qs.cmdpos = false;
              break;
            }
            if (qs.cmdpos or (qs.incond and qs.condw != 2)) {
              // INPAR: a subshell (or a group in [[ … ]]).
              if (k != '\0') ++qs.open.back().depth;
              qs.condw = 0;
              break;
            }
            // Otherwise a word that starts with a glob group.
          } else if (c == ')') {
            if (qs.casepat) {
              // par_case: the pattern's end, and a command follows.
              qs.casepat = false;
              qs.cmdpos  = true;
            } else if (k != '\0' and qs.open.back().depth == 0) {
              // The end of $(…) or of an array's (…).
              auto ctx = s::move(qs.open.back());
              qs.open.pop_back();
              qs.cmdpos    = ctx.cmdpos;
              qs.intypeset = ctx.intypeset;
              qs.casepat   = ctx.casepat;
              qs.word      = k == 'c' ? s::move(ctx.word) : HistQuoting::Word{};
            } else {
              if (k != '\0') --qs.open.back().depth;
              qs.cmdpos = false;
              qs.condw  = 0;
            }
            break;
          } else if (c == '{' and qs.cmdpos) {
            break;   // a word of its own: INBRACE
          }
          w.active = true;
          w.intpos = 1;
          if (s::exchange(hd_next, false)) hd_from = out.size() - 1;
        }
        // gettokstr: inside a glob group blanks and `|` are the word's.
        bool brk = false;
        switch (c) {
        case ' ': case '\t': case '|':
          brk = w.pct == 0;
        break; case ';': case '&':
          brk = true;
        break; case ')':
          if (w.pct == 0) brk = true;
          else --w.pct;
        break; case '(':
          if (src.peek() == ')') brk = true;   // name(): a function
          else ++w.pct;
        break; case '<': case '>':
          if (src.peek() == '(') {
            take_next();
            open_command();
          } else if (c == '<' and numglob()) {
            for (int g; (g = take_next()) >= 0 and g != '>';) {}
          } else {
            brk = true;
          }
        break; case '=':
          if (w.intpos and src.peek() == '(') {
            take_next();
            open_command();
          } else if (not w.envstring and (qs.cmdpos or qs.intypeset) and w.bct == 0
                     and w.brct == 0 and assign_name(w.text)) {
            if (src.peek() == '(') {
              // ENVARRAY: the elements are words, no command.
              take_next();
              qs.open.push_back({{}, 0, 'a', qs.cmdpos, qs.intypeset});
              qs.word      = {};
              qs.cmdpos    = false;
              qs.intypeset = false;
              break;
            }
            w.envstring = true;
            w.intpos    = 2;
          }
        break; case '[':
          ++w.brct;
        break; case ']':
          if (w.brct > 0) --w.brct;
        break; case '{':
          ++w.bct;
        break; case '}':
          if (w.bct > 0) --w.bct;
        break; case '\\':
          take_next();
        break; case '\'':
          qs.open.push_back({{}, 0, 'q', false, false});
        break; case '"':
          qs.open.push_back({{}, 0, 'd', false, false});
        break; case '`':
          qs.open.push_back({{}, 0, 'b', false, false});
        break; case '$':
          (void)dollar();
        break; default:
          break;
        }
        if (brk) {
          if (hd_from != str::npos) heredoc_word(out.size() - 1);
          // A lone digit before a redirection is its file descriptor.
          if ((c == '<' or c == '>') and w.text.size() == 1
              and s::isdigit(static_cast<unsigned char>(w.text[0])))
            w = {};
          else
            end_word();
          again = true;
          continue;
        }
        if (qs.word.active) {
          qs.word.text += static_cast<char>(c);
          if (qs.word.intpos) --qs.word.intpos;
        }
      }
    }
    return true;
  };
  while (not in.at_end()) {
    int c = in.get();
    str sline;
    bool gbal = false, cflag = false, bflag = false;
    if (isfirstch and c == hatchar and not history_off()) {
      // A line beginning ^foo^bar.
      isfirstch = false;
      in.unget(c);
      auto ev = event_text(defev);
      if (not ev) return err("no such event: " + s::to_string(defev));
      sline = *ev;
      in.get();
      str l = hist_read_to(in, hatchar), r = hist_read_to(in, hatchar);
      if (not l.empty() or mSubstLeft) {
        if (not l.empty()) {
          mLastSubstFrom = l;
          mSubstLeft = true;
        }
        mLastSubstTo = r;
        mSubstRight = true;
      }
      if (not mSubstLeft) return s::unexpected(hist_error(""));
      int follow = in.get();
      if (follow == ':') {
        follow = in.get();
        if (follow == 'G') gbal = true;
        else {
          in.unget(follow);
          cflag = true;
        }
      } else {
        in.unget(follow);
      }
      if (not hist_subst(sline, mLastSubstFrom, mLastSubstTo, gbal))
        return err("substitution failed");
    } else {
      if (c != ' ') isfirstch = false;
      if (c != bangchar or history_off()) {
        if (not lex(in, c)) break;
        continue;
      }
      s::size_t bang_at = in.pos - 1;
      c = in.get();
      if (c == '{') {
        bflag = cflag = true;
        c = in.get();
      }
      if (c == '"') {
        // !" ends history expansion for the line, and goes itself (zsh's
        // histsubchar returns the character after it).
        stophist = true;
        if (bflag) out += '{';
        continue;
      }
      if ((not cflag and hist_blank(c)) or c == '=' or c == '(' or c < 0) {
        in.unget(c);
        out += static_cast<char>(bangchar);
        if (bflag) out += '{';
        continue;
      }
      cflag = false;
      long ev = 0;
      int evset = -1;
      str buf;
      if (c == '?') {
        for (;;) {
          c = in.get();
          if (c == '?' or c == '\n' or c < 0) break;
          buf += static_cast<char>(c);
        }
        if (c != '\n' and c >= 0) c = in.get();
        mLastSubstFrom = buf;
        mSubstLeft = true;
        mMarg = -1;
        ev = -1;
        for (auto it = mEntries.rbegin(); it != mEntries.rend(); ++it) {
          s::size_t at = it->text.find(buf);
          if (at == str::npos) continue;
          long t1 = 0;
          auto spans = word_positions(it->text);
          while (t1 < static_cast<long>(spans.size())
                 and spans[static_cast<s::size_t>(t1)].first <= at)
            ++t1;
          mMarg = t1 - 1;
          ev = it->num;
          break;
        }
        mMev = ev;
        evset = 0;
        if (ev == -1) return err("no such event: " + buf);
      } else {
        for (;;) {
          if (hist_blank(c) or c == ';' or c == ':' or c == '^' or c == '$' or c == '*'
              or c == '%' or c == '}' or c == '\'' or c == '"' or c == '`' or c < 0)
            break;
          if (not buf.empty()) {
            if (c == '-') break;
            if ((s::isdigit(static_cast<unsigned char>(buf[0])) or buf[0] == '-')
                and not s::isdigit(c))
              break;
          }
          buf += static_cast<char>(c);
          if (c == '#' or c == bangchar) {
            c = in.get();
            break;
          }
          c = in.get();
        }
        if (buf.empty() and (c == '}' or c == ';' or c == '\'' or c == '"' or c == '`')) {
          // Neither event nor word designator: no expansion.
          in.unget(c);
          out += line.substr(bang_at, in.pos - bang_at);
          continue;
        }
        long t0 = buf.empty() ? 0 : s::strtol(buf.c_str(), nullptr, 10);
        if (buf.empty()) {
          if (c != '%') {
            ev = mOpts.csh_junkie ? curhist - 1 : defev;
            evset = c == ':' ? 0 : 1;
          } else {
            ev = mMarg != -1 ? mMev : defev;
            evset = 0;
          }
        } else if (t0) {
          ev = t0 < 0 ? s::max(0L, curhist + t0) : t0;
          evset = 1;
        } else if (static_cast<unsigned char>(buf[0]) == bangchar) {
          ev = curhist - 1;
          evset = 1;
        } else if (buf[0] == '#') {
          ev = curhist;
          evset = 1;
        } else {
          ev = -1;
          for (auto it = mEntries.rbegin(); it != mEntries.rend(); ++it)
            if (strv(it->text).starts_with(buf)) {
              ev = it->num;
              break;
            }
          if (ev == -1) return err("event not found: " + buf);
          evset = 1;
        }
      }
      auto ehist = event_text(defev = ev);
      if (not ehist) return err("no such event: " + s::to_string(ev));
      long argc = argc_of(*ehist);
      if (c == ':') {
        cflag = true;
        c = in.get();
        if (c == '%' and mMarg != -1) {
          if (evset) return err("ambiguous history reference");
          ehist = event_text(defev = mMev);
          if (not ehist) return err("no such event: " + s::to_string(mMev));
          argc = argc_of(*ehist);
        }
      }
      // zsh's getargspec: -1 none, -2 an error.
      str spec_error;
      auto argspec = [&]() -> long {
        int d = in.get();
        if (d == '0') return 0;
        if (d >= 0 and s::isdigit(d)) {
          long r = 0;
          while (d >= 0 and s::isdigit(d)) {
            r = r * 10 + (d - '0');
            d = in.get();
          }
          in.unget(d);
          return r;
        }
        if (d == '^') return 1;
        if (d == '$') return argc;
        if (d == '%') {
          if (evset) {
            spec_error = "Ambiguous history reference";
            return -2;
          }
          if (mMarg == -1) {
            spec_error = "% with no previous word matched";
            return -2;
          }
          return mMarg;
        }
        in.unget(d);
        return -1;
      };
      long farg, larg;
      if (c == '*') {
        farg = 1;
        larg = argc;
        cflag = false;
      } else {
        in.unget(c);
        larg = farg = argspec();
        if (larg == -2) return err(spec_error);
        if (farg != -1) cflag = false;
        c = in.get();
        if (c == '*') {
          cflag = false;
          larg = argc;
        } else if (c == '-') {
          cflag = false;
          larg = argspec();
          if (larg == -2) return err(spec_error);
          if (larg == -1) larg = argc - 1;
        } else {
          in.unget(c);
        }
      }
      if (farg == -1) farg = 0;
      if (larg == -1) larg = argc;
      auto args = get_args(*ehist, farg, larg);
      if (not args) return err("no such word in event");
      sline = *args;
    }

    // The modifiers.
    for (;;) {
      c = cflag ? ':' : in.get();
      cflag = false;
      if (c != ':') {
        if (c != '}' or not bflag) in.unget(c);
        if (c != '}' and bflag) return err("'}' expected");
        break;
      }
      gbal = false;
      c = in.get();
      if (c == 'g') {
        gbal = true;
        c = in.get();
        if (c != 's' and c != '&') return err("'s' or '&' modifier expected after 'g'");
      }
      auto count = [&] {
        int d = in.get(), n = 0;
        while (d >= 0 and s::isdigit(d)) {
          n = n * 10 + (d - '0');
          d = in.get();
        }
        in.unget(d);
        return n;
      };
      auto lower = [](char ch) {
        return static_cast<char>(s::tolower(static_cast<unsigned char>(ch)));
      };
      auto upper = [](char ch) {
        return static_cast<char>(s::toupper(static_cast<unsigned char>(ch)));
      };
      switch (c) {
      case 'p': res.print_only = true;

      break; case 'a': case 'A':
        if (not sline.starts_with('/')) {
          char cwd[4096];
          if (::getcwd(cwd, sizeof cwd)) sline = str(cwd) + (strv(cwd) == "/" ? "" : "/") + sline;
        }
        if (c == 'A') {
          char real[PATH_MAX];
          if (::realpath(sline.c_str(), real)) sline = real;
        }

      break; case 'c': {
        // zsh's equalsubstr: the command's path, as `=cmd` gives it.
        auto path = mFindCommand ? mFindCommand(sline) : s::nullopt;
        if (not path) return err("modifier failed: c");
        sline = s::move(*path);
      }

      break; case 'h':
        if (not rem_tpath(sline, count())) return err("modifier failed: h");

      break; case 'e':
        if (not rem_but_text(sline)) return err("modifier failed: e");

      break; case 'r':
        if (not rem_text(sline)) return err("modifier failed: r");

      break; case 't':
        if (not rem_lpaths(sline, count())) return err("modifier failed: t");

      break; case 's': case '&':
        if (c == 's') {
          // zsh's getsubsargs: an empty left side is the last one.
          int del = in.get();
          str l = hist_read_to(in, del), r = hist_read_to(in, del);
          if (not l.empty() or mSubstLeft) {
            if (not l.empty()) {
              mLastSubstFrom = l;
              mSubstLeft = true;
            }
            mLastSubstTo = r;
            mSubstRight = true;
          }
          int follow = in.get();
          if (follow == ':') {
            follow = in.get();
            if (follow == 'G') gbal = true;
            else {
              in.unget(follow);
              cflag = true;
            }
          } else {
            in.unget(follow);
          }
        }
        if (not (mSubstLeft and mSubstRight)) return err("no previous substitution");
        if (not hist_subst(sline, mLastSubstFrom, mLastSubstTo, gbal))
          return err("substitution failed");

      break; case 'q': sline = hist_quote(sline);

      break; case 'Q': {
        // Quotes removed (parse_subst_string, remnulargs, untokenize).
        str q;
        char quote = 0;
        for (s::size_t i = 0; i < sline.size(); ++i) {
          char ch = sline[i];
          if (quote) {
            if (ch == quote) quote = 0;
            else if (ch == '\\' and quote == '"' and i + 1 < sline.size()) q += sline[++i];
            else q += ch;
          } else if (ch == '\'' or ch == '"') {
            quote = ch;
          } else if (ch == '\\' and i + 1 < sline.size()) {
            q += sline[++i];
          } else {
            q += ch;
          }
        }
        sline = q;
      }

      break; case 'x': sline = hist_quote_break(sline);

      break; case 'l': s::transform(sline.begin(), sline.end(), sline.begin(), lower);

      break; case 'u': s::transform(sline.begin(), sline.end(), sline.begin(), upper);

      break; case 'P': {
        if (not sline.starts_with('/')) {
          char cwd[4096];
          if (::getcwd(cwd, sizeof cwd))
            sline = str(cwd) + (strv(cwd).ends_with('/') ? "" : "/") + sline;
        }
        char real[PATH_MAX];
        if (::realpath(sline.c_str(), real)) sline = real;
      }

      break; default:
        return err(str("illegal modifier: ") + (c < 0 ? str{} : str(1, static_cast<char>(c))));
      }
    }
    // The expansion is read by the lexer as it goes on (not expanded
    // again): its quotes count, and a comment it starts takes the rest.
    HistLine sub{sline};
    bool comment = false;
    while (not sub.at_end() and not comment) comment = not lex(sub, sub.get());
    res.changed = true;
    if (comment) {
      while (not in.at_end()) out += static_cast<char>(in.get());
      break;
    }
  }
  if (hd_from != str::npos) heredoc_word(out.size());
  res.text    = s::move(out);
  res.quoting = s::move(qs);
  return res;
}

void HistoryManager::push_stack(int level) {
  mStack.push_back({s::move(mEntries), mFileOffset, mNextNum, level});
  mEntries.clear();
  mNextNum    = 1;
  mFileOffset = 0;
}

bool HistoryManager::pop_stack() {
  if (mStack.empty()) return false;
  Saved& top  = mStack.back();
  mEntries    = s::move(top.entries);
  mNextNum    = top.next_num;
  mFileOffset = top.file_offset;
  mStack.pop_back();
  return true;
}

optref<const HistEntry> HistoryManager::lookup(int num) const noexcept {
  if (num > 0) {
    for (auto const& e : mEntries)
      if (static_cast<int>(e.num) == num) return s::cref(e);
  } else {
    // -1 is the last entry; 0 is none (B6).
    int idx = static_cast<int>(mEntries.size()) + num;
    if (num < 0 and idx >= 0) return s::cref(mEntries[static_cast<s::size_t>(idx)]);
  }
  return s::nullopt;
}

optref<const HistEntry> HistoryManager::search(strv substr) const noexcept {
  for (auto it = mEntries.rbegin(); it != mEntries.rend(); ++it)
    if (it->text.find(substr) != str::npos) return s::cref(*it);
  return s::nullopt;
}

vec<HistEntry> HistoryManager::parse_file(strv data) const {
  vec<HistEntry> out;
  str pending;
  bool continued = false;
  auto finish = [&](str text) {
    HistEntry e;
    e.saved = true;
    // `: start:elapsed;command`
    if (text.starts_with(": ")) {
      s::size_t k = 2;
      s::size_t start_digits = k;
      while (k < text.size() and isdigit(static_cast<unsigned char>(text[k]))) ++k;
      if (k > start_digits and k < text.size() and text[k] == ':') {
        s::time_t ts = static_cast<s::time_t>(s::strtoll(text.c_str() + start_digits, nullptr, 10));
        s::size_t el = ++k;
        while (k < text.size() and isdigit(static_cast<unsigned char>(text[k]))) ++k;
        if (k < text.size() and text[k] == ';') {
          e.timestamp = ts;
          e.elapsed   = static_cast<s::uint32_t>(s::strtoul(text.c_str() + el, nullptr, 10));
          text.erase(0, k + 1);
        }
      }
    }
    if (text.empty()) return;
    e.text = s::move(text);
    out.push_back(s::move(e));
  };
  s::size_t at = 0;
  while (at < data.size()) {
    auto nl = data.find('\n', at);
    strv line = data.substr(at, nl == strv::npos ? strv::npos : nl - at);
    at = nl == strv::npos ? data.size() : nl + 1;
    s::size_t slashes = 0;
    while (slashes < line.size() and line[line.size() - 1 - slashes] == '\\') ++slashes;
    str text = unmetafy(line);
    if (continued) pending += '\n';
    if (slashes % 2 == 1) {   // a multi-line entry goes on
      text.pop_back();
      pending += text;
      continued = true;
      continue;
    }
    pending += text;
    continued = false;
    finish(s::move(pending));
    pending.clear();
  }
  if (continued) finish(s::move(pending));
  return out;
}

str HistoryManager::format_entry(const HistEntry& e, bool extended) const {
  str line;
  if (extended) {
    line = ": " + s::to_string(static_cast<long long>(e.timestamp ? e.timestamp : s::time(nullptr)))
         + ":" + s::to_string(e.elapsed) + ";";
  }
  for (char c : e.text) {
    if (c == '\n') line += "\\\n";
    else           line += c;
  }
  return metafy(line) + "\n";
}

Result<void> HistoryManager::load(strv path, bool skip_old) {
  LockedFile f(path, false, mOpts.fcntl_lock);
  if (f.fd < 0) return {};  // file not found is not an error on first run
  str data = f.read_from(0);
  mFileOffset = static_cast<off_t>(data.size());
  if (not skip_old) mFileLines = static_cast<s::size_t>(s::count(data.begin(), data.end(), '\n'));
  uset<str> have;
  if (skip_old)
    for (const auto& e : mEntries) have.insert(e.text);
  for (auto& e : parse_file(data)) {
    if (skip_old and not have.insert(e.text).second) continue;
    e.num = mNextNum++;
    mEntries.push_back(s::move(e));
  }
  trim_to_limit();
  return {};
}

Result<void> HistoryManager::sync(strv path) {
  LockedFile f(path, false, mOpts.fcntl_lock);
  if (f.fd < 0) return {};
  off_t size = f.size();
  if (size < mFileOffset) {   // another shell rewrote it
    mFileOffset = size;
    str all = f.read_from(0);
    mFileLines = static_cast<s::size_t>(s::count(all.begin(), all.end(), '\n'));
    return {};
  }
  if (size == mFileOffset) return {};
  str data = f.read_from(mFileOffset);
  // Only whole lines: another shell may be half way through a write.
  auto last_nl = data.rfind('\n');
  if (last_nl == str::npos) return {};
  data.resize(last_nl + 1);
  mFileOffset += static_cast<off_t>(data.size());
  mFileLines  += static_cast<s::size_t>(s::count(data.begin(), data.end(), '\n'));
  for (auto& e : parse_file(data)) {
    e.num = mNextNum++;
    mEntries.push_back(s::move(e));
  }
  trim_to_limit();
  return {};
}

Result<void> HistoryManager::save(strv path, HistSave mode, bool skip_old) {
  if (path.empty()) return {};
  if ((mode == HistSave::Options or mode == HistSave::Incremental) and mOpts.savehist <= 0)
    return {};
  LockedFile f(path, true, mOpts.fcntl_lock);
  if (f.fd < 0)
  {
    // zsh's lockhistfile: "locking failed for FILE: error".
    str why = s::strerror(errno);
    if (not why.empty()) why[0] = static_cast<char>(s::tolower(static_cast<unsigned char>(why[0])));
    return s::unexpected(
        ShellError{ErrorKind::FileNotFound, "locking failed for " + str(path) + ": " + why});
  }
  bool append = mode == HistSave::Incremental
             or (mode == HistSave::Options and (mOpts.append or mOpts.inc_append or mOpts.share));

  // The entries to keep when the file is rewritten: the newest SAVEHIST,
  // and with HIST_SAVE_NO_DUPS only the newest copy of each line.
  auto keep = [&](vec<HistEntry> list, bool limit) {
    if (mOpts.save_no_dups and mode == HistSave::Options) {
      uset<str> seen;
      vec<HistEntry> kept;
      for (auto it = list.rbegin(); it != list.rend(); ++it)
        if (seen.insert(it->text).second) kept.push_back(s::move(*it));
      s::reverse(kept.begin(), kept.end());
      list = s::move(kept);
    }
    s::size_t max = static_cast<s::size_t>(s::max(mOpts.savehist, 0));
    if (limit and list.size() > max)
      list.erase(list.begin(), list.begin() + static_cast<s::ptrdiff_t>(list.size() - max));
    str data;
    for (const auto& e : list)
      if (not e.transient) data += format_entry(e);
    return data;
  };

  if (mode == HistSave::Append or mode == HistSave::Rewrite) {
    // fc -W and fc -A: the whole list (zsh's savehistfile from the oldest
    // entry), or with -I only what wasn't read or written (HIST_OLD).
    str data;
    for (auto& e : mEntries) {
      if (e.transient or (skip_old and e.saved)) continue;
      data += format_entry(e);
      if (skip_old) e.saved = true;
    }
    bool ok = mode == HistSave::Append
            ? ::lseek(f.fd, 0, SEEK_END) >= 0 and f.write_all(data)
            : ::ftruncate(f.fd, 0) == 0 and ::lseek(f.fd, 0, SEEK_SET) >= 0 and f.write_all(data);
    if (not ok) return s::unexpected(ShellError{ErrorKind::ExecError, "cannot write histfile"});
  } else if (append) {
    // B5: only the entries that aren't in the file yet.  With
    // SHARE_HISTORY, what other shells wrote meanwhile is read first.
    if (mOpts.share and f.size() > mFileOffset) {
      str theirs = f.read_from(mFileOffset);
      auto last_nl = theirs.rfind('\n');
      if (last_nl != str::npos) {
        theirs.resize(last_nl + 1);
        vec<HistEntry> others = parse_file(theirs);
        for (auto& e : others) e.num = mNextNum++;
        // Ours stay newest.
        auto first_new = s::find_if(mEntries.begin(), mEntries.end(),
                                    [](const HistEntry& e) { return not e.saved; });
        mEntries.insert(first_new, others.begin(), others.end());
        trim_to_limit();
      }
    }
    // zsh's HFILE_FAST write (INC_APPEND_HISTORY and friends): once the
    // file has more lines than SAVEHIST and a fifth, the write is a full
    // one, and the file is cut back to SAVEHIST entries after it, written
    // as EXTENDED_HISTORY itself says (hist.c: savehistfile's linect).
    auto savehist = static_cast<s::size_t>(s::max(mOpts.savehist, 0));
    s::size_t limit = savehist + savehist / 5;
    bool rewrite = mode == HistSave::Incremental and mFileLines > limit;
    str data;
    for (auto& e : mEntries) {
      if (e.saved or e.transient) continue;
      data += format_entry(e);
      e.saved = true;
    }
    if (mode == HistSave::Incremental and data.empty()) return {};
    mFileLines += static_cast<s::size_t>(s::count(data.begin(), data.end(), '\n'));
    if (::lseek(f.fd, 0, SEEK_END) < 0 or not f.write_all(data))
      return s::unexpected(ShellError{ErrorKind::ExecError, "cannot write histfile"});
    if (rewrite) {
      vec<HistEntry> all = parse_file(f.read_from(0));
      s::size_t max = static_cast<s::size_t>(s::max(mOpts.savehist, 0));
      if (all.size() > max)
        all.erase(all.begin(), all.begin() + static_cast<s::ptrdiff_t>(all.size() - max));
      str text;
      for (const auto& e : all) text += format_entry(e, mOpts.extended_set);
      if (::ftruncate(f.fd, 0) != 0 or ::lseek(f.fd, 0, SEEK_SET) < 0 or not f.write_all(text))
        return s::unexpected(ShellError{ErrorKind::ExecError, "cannot write histfile"});
      // SHARE_HISTORY reads the file before each prompt and counts it
      // again then (readhistfile after a rewrite); INC_APPEND_HISTORY
      // goes on counting what it wrote.
      if (mOpts.share) mFileLines = all.size();
    }
    // On the way out, a file grown past SAVEHIST is cut back to it.
    if (mode == HistSave::Options) {
      vec<HistEntry> all = parse_file(f.read_from(0));
      if (all.size() > static_cast<s::size_t>(mOpts.savehist)) {
        str trimmed = keep(s::move(all), true);
        if (::ftruncate(f.fd, 0) != 0 or ::lseek(f.fd, 0, SEEK_SET) < 0
            or not f.write_all(trimmed))
          return s::unexpected(ShellError{ErrorKind::ExecError, "cannot write histfile"});
      }
    }
  } else {
    str data = keep(mEntries, mode == HistSave::Options);
    if (::ftruncate(f.fd, 0) != 0 or ::lseek(f.fd, 0, SEEK_SET) < 0 or not f.write_all(data))
      return s::unexpected(ShellError{ErrorKind::ExecError, "cannot write histfile"});
    for (auto& e : mEntries) e.saved = true;
  }
  mFileOffset = f.size();
  return {};
}

}  // namespace jfsh
