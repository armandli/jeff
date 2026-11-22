// Filename generation: glob_expand() walks multi-segment paths, **/ and
// ***/, and applies zsh's glob qualifiers; and zsh's sort order.

#include <jfsh/glob/glob.h>

#include <cctype>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <dirent.h>
#include <grp.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <utility>

#include <jfsh/core/aliases.h>
#include <jfsh/core/sort.h>

#include <glob/pattern_syntax.h>

namespace jfsh {

str glob_unescape(strv s) {
  str out;
  out.reserve(s.size());
  for (s::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' and i + 1 < s.size()) ++i;
    out += s[i];
  }
  return out;
}

GlobPattern GlobPattern::compile(strv pat, GlobFlags flags) {
  GlobPattern g;
  g.raw   = str(pat);
  g.flags = flags;
  return g;
}

int collate(const str& a, const str& b, bool numeric) {
  if (int c = zstrcmp(a, b, numeric)) return c;
  return a.compare(b);
}

str collate_key(const str& s) {
  str key(s.size() * 4 + 16, '\0');
  s::size_t n = ::strxfrm(key.data(), s.c_str(), key.size());
  if (n >= key.size()) {
    key.resize(n + 1);
    ::strxfrm(key.data(), s.c_str(), key.size());
  }
  key.resize(n);
  return key;
}

void collate_sort(vec<str>& v, bool numeric) {
  if (numeric) {
    s::sort(v.begin(), v.end(), [](const str& a, const str& b) { return collate(a, b, true) < 0; });
    return;
  }
  // strxfrm() once per string is much cheaper than strcoll() per
  // comparison in a UTF-8 locale.
  vec<s::pair<str, str>> keyed;
  keyed.reserve(v.size());
  for (auto& e : v) keyed.emplace_back(collate_key(e), s::move(e));
  s::sort(keyed.begin(), keyed.end());
  for (s::size_t k = 0; k < v.size(); ++k) v[k] = s::move(keyed[k].second);
}

// ---------------------------------------------------------------------------
// Filename generation.
// ---------------------------------------------------------------------------
namespace {

// One test of a glob qualifier list (zsh's struct qual).
struct QualTest {
  char        kind   = 0;
  bool        negate = false;
  bool        follow = false;   // `-`: test the target of a symlink
  int         range  = 0;       // `-` less than, `+` more than, 0 equal
  s::int64_t  value  = 0;
  s::int64_t  unit   = 1;       // L: bytes per unit; a m c: seconds
  s::uint32_t yes = 0, no = 0;  // f: mode bits that must be set, clear
  str         code;             // e: code, +: function name
};

struct SortKey {
  char key;
  bool desc;
};

struct Qualifiers {
  vec<vec<QualTest>> alternatives;   // a file passes when one set does
  bool nullglob = false;
  opt<bool> dots, numeric, markdirs, listtypes;
  vec<SortKey> sorts;
  bool select = false;
  s::int64_t sel_lo = 1, sel_hi = 1;
  s::int64_t first_n = -1;           // Y: stop after so many
  str modifiers;                     // `:t:r`
  vec<str> prepend;                  // P: words put before each match
};

using QualResult = Result<Qualifiers>;

ShellError qual_error(str msg) { return ShellError{ErrorKind::BadPattern, s::move(msg)}; }

char end_char(char c) {
  switch (c) {
    case '(': return ')';
    case '[': return ']';
    case '{': return '}';
    case '<': return '>';
    default:  return c;
  }
}

// Reads the arguments inside a qualifier list: `i` is where the next one
// starts, and each read moves it past what it took.
struct QualReader {
  opt<str>        delimited();
  opt<s::int64_t> number();
  int             range_of();
  Result<s::pair<s::uint32_t, s::uint32_t>> mode_spec();
  Result<s::int64_t> owner(char kind);

  strv      body;
  s::size_t i = 0;
};

// A delimited argument at body[i] (`:text:`, `[text]`); i ends after it.
opt<str> QualReader::delimited() {
  if (i >= body.size()) return s::nullopt;
  char end = end_char(body[i]);
  s::size_t e = body.find(end, i + 1);
  if (e == strv::npos) return s::nullopt;
  str out(body.substr(i + 1, e - i - 1));
  i = e + 1;
  return out;
}

opt<s::int64_t> QualReader::number() {
  s::size_t b = i;
  while (i < body.size() and s::isdigit(static_cast<unsigned char>(body[i]))) ++i;
  if (i == b) return s::nullopt;
  return s::strtoll(str(body.substr(b, i - b)).c_str(), nullptr, 10);
}

int QualReader::range_of() {
  if (i < body.size() and (body[i] == '-' or body[i] == '+')) return body[i++] == '-' ? -1 : 1;
  return 0;
}

// f's mode specification (zsh's qgetmodespec): `NNN`, `=NNN`, `-NNN`,
// `+NNN` (`?` any digit), or delimited chmod-style `[ugoa]*[=+-][rwxst]*`
// clauses, comma-separated.  The modes that must be set and unset.
Result<s::pair<s::uint32_t, s::uint32_t>> QualReader::mode_spec() {
  s::uint32_t yes = 0, no = 0;
  char end = 0;
  char c = i < body.size() ? body[i] : 0;
  if (not (c == '=' or c == '+' or c == '-' or c == '?' or (c >= '0' and c <= '7'))) {
    end = end_char(c);
    ++i;
  }
  auto bad = [] { return s::unexpected(qual_error("invalid mode specification")); };
  do {
    s::uint32_t mask = 0;
    while (end and i < body.size() and strv("ugoa").find(body[i]) != strv::npos) {
      switch (body[i++]) {
        case 'o': mask |= 01007; break;
        case 'g': mask |= 02070; break;
        case 'u': mask |= 04700; break;
        default:  mask |= 07777;
      }
    }
    c = i < body.size() ? body[i] : 0;
    char how = c == '+' or c == '-' ? c : '=';
    if (c == '+' or c == '-' or c == '=') ++i;
    s::uint32_t val = 0;
    if (mask) {
      for (;;) {
        if (i >= body.size()) return bad();
        c = body[i++];
        if (c == ',' or c == end) break;
        switch (c) {
          case 'x': val |= 00111; break;
          case 'w': val |= 00222; break;
          case 'r': val |= 00444; break;
          case 's': val |= 06000; break;
          case 't': val |= 01000; break;
          default:
            if (c < '0' or c > '7') return bad();
            s::uint32_t d = static_cast<s::uint32_t>(c - '0');
            val |= d | (d << 3) | (d << 6);
        }
      }
      if (how == '=' or how == '+') {
        yes |= val & mask;
        val = ~val;
      }
      if (how == '=' or how == '-') no |= val & mask;
    } else {
      c = i < body.size() ? body[i] : 0;
      if ((end and c == end) or c == ',' or not c) return bad();
      s::uint32_t any = 07777;
      while (i < body.size() and (body[i] == '?' or (body[i] >= '0' and body[i] <= '7'))) {
        if (body[i] == '?') {
          any = (any << 3) | 7;
          val <<= 3;
        } else {
          any <<= 3;
          val = (val << 3) | static_cast<s::uint32_t>(body[i] - '0');
        }
        ++i;
      }
      c = i < body.size() ? body[i] : 0;
      if (end and c != end and c != ',') return bad();
      if (end) ++i;
      if (how == '=') {
        yes = (yes & ~any) | val;
        no  = (no & ~any) | (~val & ~any);
      } else if (how == '+') {
        yes |= val;
      } else {
        no |= val;
      }
    }
  } while (end and c != end);
  return s::pair{yes & 07777u, no & 07777u};
}

// u and g: a number, or a delimited user or group name.
Result<s::int64_t> QualReader::owner(char kind) {
  if (auto n = number()) return *n;
  if (i >= body.size())
    return s::unexpected(qual_error(str("missing delimiter for '") + kind + "' glob qualifier"));
  auto name = delimited();
  if (not name) return s::unexpected(qual_error("missing end of name"));
  if (kind == 'u') {
    if (struct passwd* pw = ::getpwnam(name->c_str())) return pw->pw_uid;
    return s::unexpected(qual_error("unknown username '" + *name + "'"));
  }
  if (struct group* gr = ::getgrnam(name->c_str())) return gr->gr_gid;
  return s::unexpected(qual_error("unknown group"));
}

// Parses the text between the parentheses of a qualifier list.
QualResult parse_qualifiers(strv body) {
  Qualifiers q;
  q.alternatives.emplace_back();
  bool negate = false, follow = false;
  QualReader in{body};
  s::size_t& i = in.i;
  while (i < body.size()) {
    char c = body[i++];
    QualTest t;
    t.kind   = c;
    t.negate = negate;
    t.follow = follow;
    auto add = [&] { q.alternatives.back().push_back(t); };
    switch (c) {
      case ',':
        negate = follow = false;
        if (not q.alternatives.back().empty()) q.alternatives.emplace_back();
      break; case ':':
        q.modifiers = ":" + str(body.substr(i));
        i = body.size();
      break; case '^': negate = not negate;
      break; case '-': follow = not follow;
      break; case 'N': q.nullglob = not negate;
      break; case 'D': q.dots = not negate;
      break; case 'n': q.numeric = not negate;
      break; case 'M': q.markdirs = not negate;
      break; case 'T': q.listtypes = not negate;
      break; case '/': case '.': case '@': case '=': case 'p': case '*': case 'r': case 'w':
      case 'x': case 'A': case 'I': case 'E': case 'R': case 'W': case 'X': case 's':
      case 'S': case 't': case 'U': case 'G': case 'F':
        add();
      break; case '%':
        if (i < body.size() and (body[i] == 'b' or body[i] == 'c')) t.value = body[i++];
        add();
      break; case 'f': {
        auto modes = in.mode_spec();
        if (not modes) return s::unexpected(modes.error());
        t.yes = modes->first;
        t.no  = modes->second;
        add();
      }
      break; case 'e': case '+': {
        if (c == 'e') {
          auto code = in.delimited();
          if (not code) return s::unexpected(qual_error("missing end of string"));
          t.code = *code;
        } else {
          s::size_t b = i;
          while (i < body.size() and (s::isalnum(static_cast<unsigned char>(body[i]))
                                      or body[i] == '_'))
            ++i;
          if (i == b) return s::unexpected(qual_error("missing identifier after `+'"));
          t.code = str(body.substr(b, i - b));
        }
        add();
      }
      break; case 'u': case 'g': {
        auto id = in.owner(c);
        if (not id) return s::unexpected(id.error());
        t.value = *id;
        add();
      }
      break; case 'd': case 'l': {
        if (c == 'l') t.range = in.range_of();
        auto n = in.number();
        if (not n) return s::unexpected(qual_error("number expected"));
        t.value = *n;
        add();
      }
      break; case 'L': case 'a': case 'm': case 'c': {
        if (i < body.size()) {
          char u = body[i];
          s::int64_t unit = 0;
          if (c == 'L') {
            switch (u) {
              case 'p': case 'P': unit = 512; break;
              case 'k': case 'K': unit = 1024; break;
              case 'm': case 'M': unit = 1024 * 1024; break;
              case 'g': case 'G': unit = 1024 * 1024 * 1024; break;
              case 't': case 'T': unit = 1024LL * 1024 * 1024 * 1024; break;
              default: ;
            }
          } else {
            switch (u) {
              case 'M': unit = 2592000; break;
              case 'w': unit = 604800; break;
              case 'h': unit = 3600; break;
              case 'm': unit = 60; break;
              case 's': unit = 1; break;
              case 'd': unit = 86400; break;
              default: ;
            }
          }
          if (unit) ++i;
          t.unit = unit ? unit : (c == 'L' ? 1 : 86400);
        }
        t.range = in.range_of();
        auto n = in.number();
        if (not n) return s::unexpected(qual_error("number expected"));
        t.value = *n;
        add();
      }
      break; case 'o': case 'O': {
        if (i >= body.size() or strv("nLlamcdN").find(body[i]) == strv::npos)
          return s::unexpected(qual_error("unknown sort specifier"));
        q.sorts.push_back({body[i++], c == 'O'});
      }
      break; case 'Y': {
        auto n = in.number();
        if (not n) return s::unexpected(qual_error("number expected after Y"));
        q.first_n = *n;
      }
      break; case 'P': {
        auto w = in.delimited();
        if (not w) return s::unexpected(qual_error("missing end of string"));
        q.prepend.push_back(*w);
      }
      break; case '[': {
        s::size_t close = body.find(']', i);
        if (close == strv::npos) return s::unexpected(qual_error("missing end of subscript"));
        str spec(body.substr(i, close - i));
        auto comma = spec.find(',');
        q.select = true;
        q.sel_lo = s::strtoll(spec.c_str(), nullptr, 10);
        q.sel_hi = comma == str::npos ? q.sel_lo
                                      : s::strtoll(spec.c_str() + comma + 1, nullptr, 10);
        i = close + 1;
      }
      break; default:
        return s::unexpected(qual_error(str("unknown file attribute: ") + c));
    }
  }
  if (q.alternatives.back().empty() and q.alternatives.size() > 1) q.alternatives.pop_back();
  return q;
}

// The final unescaped `(` … `)` group of `raw`, or npos.  With `q_only`,
// only a `(#q…)` group.  A `|` in it, or with `tilde` (EXTENDED_GLOB) a
// `~`, makes it a pattern group instead.
s::size_t final_group(strv raw, bool q_only, bool tilde = false) {
  if (raw.size() < 3 or raw.back() != ')') return strv::npos;
  // An escaped `)` at the end isn't one.
  s::size_t bs = 0;
  for (s::size_t k = raw.size() - 1; k > 0 and raw[k - 1] == '\\'; --k) ++bs;
  if (bs % 2) return strv::npos;
  for (s::size_t k = raw.size() - 1; k-- > 0;) {
    char c = raw[k];
    s::size_t n = 0;
    for (s::size_t j = k; j > 0 and raw[j - 1] == '\\'; --j) ++n;
    if (n % 2) continue;
    if (c == ')') return strv::npos;
    if (c != '(') {
      if ((c == '|' or (c == '~' and tilde)) and not q_only) return strv::npos;
      continue;
    }
    bool hash_q = k + 2 < raw.size() and raw[k + 1] == '#' and raw[k + 2] == 'q';
    if (q_only and not hash_q) return strv::npos;
    return k;
  }
  return strv::npos;
}

// A pattern with its qualifier lists split off the end.
struct SplitPattern {
  str             pattern;
  opt<Qualifiers> quals;
};

// Splits the qualifier lists off the end of `raw`: `(#q…)` groups under
// EXTENDED_GLOB, and with BARE_GLOB_QUAL one final group with no `|`.
Result<SplitPattern> split_qualifiers(str raw, const OptionState& opts, bool extended) {
  str all;
  bool found = false;
  for (;;) {
    s::size_t open = extended ? final_group(raw, true) : strv::npos;
    if (open != strv::npos) {
      all.insert(0, raw.substr(open + 3, raw.size() - open - 4));
    } else if (not found and opts.get(OPT_BAREGLOBQUAL)
               and (open = final_group(raw, false, extended)) != strv::npos
               and not (raw[open + 1] == '#')) {
      // A word that is all one group is qualifiers too, on nothing, as in
      // zsh: `(a1)` finds no files.
      all.insert(0, raw.substr(open + 1, raw.size() - open - 2));
      raw.resize(open);
      found = true;
      break;
    } else {
      break;
    }
    raw.resize(open);
    found = true;
  }
  if (not found) return SplitPattern{s::move(raw), s::nullopt};
  auto q = parse_qualifiers(glob_unescape(all));
  if (not q) return s::unexpected(q.error());
  return SplitPattern{s::move(raw), s::move(*q)};
}

bool nonempty_dir(const str& path) {
  DIR* d = ::opendir(path.c_str());
  if (not d) return false;
  bool any = false;
  while (auto* e = ::readdir(d)) {
    strv n(e->d_name);
    if (n != "." and n != "..") { any = true; break; }
  }
  ::closedir(d);
  return any;
}

// The test against `st`; e and + are run by the caller.
bool qual_test(const QualTest& t, const struct stat& st, const str& path) {
  auto ranged = [&](s::int64_t v) {
    return t.range < 0 ? v < t.value : t.range > 0 ? v > t.value : v == t.value;
  };
  mode_t m = st.st_mode;
  switch (t.kind) {
    case '/': return S_ISDIR(m);
    case '.': return S_ISREG(m);
    case '@': return S_ISLNK(m);
    case '=': return S_ISSOCK(m);
    case 'p': return S_ISFIFO(m);
    case '%':
      return t.value == 'b' ? S_ISBLK(m) : t.value == 'c' ? S_ISCHR(m)
                            : S_ISBLK(m) or S_ISCHR(m);
    case '*': return S_ISREG(m) and (m & 0111);
    case 'r': return m & S_IRUSR;
    case 'w': return m & S_IWUSR;
    case 'x': return m & S_IXUSR;
    case 'A': return m & S_IRGRP;
    case 'I': return m & S_IWGRP;
    case 'E': return m & S_IXGRP;
    case 'R': return m & S_IROTH;
    case 'W': return m & S_IWOTH;
    case 'X': return m & S_IXOTH;
    case 's': return m & S_ISUID;
    case 'S': return m & S_ISGID;
    case 't': return m & S_ISVTX;
    case 'f': return (m & t.yes) == t.yes and not (m & t.no);
    case 'U': return st.st_uid == ::geteuid();
    case 'G': return st.st_gid == ::getegid();
    case 'u': return static_cast<s::int64_t>(st.st_uid) == t.value;
    case 'g': return static_cast<s::int64_t>(st.st_gid) == t.value;
    case 'd': return static_cast<s::int64_t>(st.st_dev) == t.value;
    case 'l': return ranged(static_cast<s::int64_t>(st.st_nlink));
    case 'F': return S_ISDIR(m) and nonempty_dir(path);
    case 'L': {
      s::int64_t size = static_cast<s::int64_t>(st.st_size);
      return ranged((size + t.unit - 1) / t.unit);
    }
    case 'a': case 'm': case 'c': {
      s::time_t when = t.kind == 'a' ? st.st_atime : t.kind == 'm' ? st.st_mtime : st.st_ctime;
      return ranged((s::time(nullptr) - when) / t.unit);
    }
    default: return false;
  }
}

// zsh's file_type() mark for LIST_TYPES / the T qualifier.
char type_mark(const str& path) {
  struct stat st{};
  if (::lstat(path.c_str(), &st) != 0) return 0;
  mode_t m = st.st_mode;
  if (S_ISBLK(m)) return '#';
  if (S_ISCHR(m)) return '%';
  if (S_ISDIR(m)) return '/';
  if (S_ISFIFO(m)) return '|';
  if (S_ISLNK(m)) return '@';
  if (S_ISSOCK(m)) return '=';
  if (S_ISREG(m) and (m & 0111)) return '*';
  // Anything else, a plain file too, gets a blank, as zsh's file_type.
  return ' ';
}

bool is_dir(const str& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 and S_ISDIR(st.st_mode);
}

str join_path(const str& dir, strv name) {
  if (dir.empty()) return str(name);
  if (dir.back() == '/') return dir + str(name);
  return dir + '/' + str(name);
}

// The locale's collation, except that with NUMERIC_GLOB_SORT runs of
// digits compare by value.
bool name_less(const str& a, const str& b, bool numeric) {
  return collate(a, b, numeric) < 0;
}

struct Walker {
  bool dots;
  bool nocase;
  bool extended;
  bool multibyte;

  // Every directory below `dir`, depth first, `dir` itself first.  For `**`
  // symlinks to directories are not followed; for `***` they are.
  void descend(const str& dir, bool follow, vec<str>& out) {
    out.push_back(dir);
    DIR* d = ::opendir(dir.empty() ? "." : dir.c_str());
    if (not d) return;
    vec<str> subdirs;
    while (auto* ent = ::readdir(d)) {
      strv name(ent->d_name);
      if (name == "." or name == "..") continue;
      if (name[0] == '.' and not dots) continue;
      str path = join_path(dir, name);
      struct stat st{};
      int rc = follow ? ::stat(path.c_str(), &st) : ::lstat(path.c_str(), &st);
      if (rc == 0 and S_ISDIR(st.st_mode)) subdirs.push_back(path);
    }
    ::closedir(d);
    collate_sort(subdirs, false);
    for (const auto& s : subdirs) descend(s, follow, out);
  }

  CompiledPattern compile(strv seg) const {
    return CompiledPattern::compile(seg, nocase, extended, multibyte);
  }

  // The names in `dir` that `pat`, compiled from `seg`, matches.
  void match_dir(const str& dir, strv seg, const CompiledPattern& pat, bool need_dir,
                 vec<str>& out) {
    DIR* d = ::opendir(dir.empty() ? "." : dir.c_str());
    if (not d) return;
    bool dot_ok = dots or (not seg.empty() and seg[0] == '.')
               or (seg.size() > 1 and seg[0] == '\\' and seg[1] == '.');
    while (auto* ent = ::readdir(d)) {
      strv name(ent->d_name);
      if (name == "." or name == "..") continue;
      if (name[0] == '.' and not dot_ok) continue;
      if (not pat.matches(name)) continue;
      str path = join_path(dir, name);
      if (need_dir and not is_dir(path)) continue;
      out.push_back(s::move(path));
    }
    ::closedir(d);
  }
};

vec<str> split_segments(strv raw) {
  vec<str> segs;
  str cur;
  for (s::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\\' and i + 1 < raw.size()) {
      cur += raw[i];
      cur += raw[++i];
    } else if (raw[i] == '/') {
      segs.push_back(s::move(cur));
      cur.clear();
    } else {
      cur += raw[i];
    }
  }
  segs.push_back(s::move(cur));
  return segs;
}

}  // namespace

Result<vec<str>> glob_expand(
    const GlobPattern& pat,
    const OptionState& opts,
    const GlobHooks*   hooks)
{
  str        raw = pat.raw;
  bool extended = opts.extended_glob() or (pat.flags & GLOB_EXTGLOB);
  // An unclosed `(` or `[` is a bad pattern, as in zsh (`echo a[b`,
  // `echo (b`); a `[` alone is the word `[`.  Under NO_BAD_PATTERN the
  // word stays as it is.
  if (raw != "[") {
    int parens = 0;
    bool bad = false;
    for (s::size_t i = 0; i < raw.size() and not bad; ++i) {
      char c = raw[i];
      if (c == '\\') { ++i; continue; }
      if (c == '[') {
        s::size_t end = pattern::bracket_end(raw, i);
        if (end == strv::npos) bad = true;
        else i = end - 1;
      } else if (c == '(') {
        ++parens;
      } else if (c == ')' and parens > 0) {
        --parens;
      }
    }
    if (bad or parens > 0) {
      if (not opts.get(OPT_BADPATTERN)) return vec<str>{glob_unescape(pat.raw)};
      return s::unexpected(qual_error("bad pattern: " + glob_unescape(pat.raw)));
    }
  }
  auto split = split_qualifiers(s::move(raw), opts, extended);
  if (not split) return s::unexpected(split.error());
  raw = s::move(split->pattern);
  bool has_quals = split->quals.has_value();
  Qualifiers q = has_quals ? s::move(*split->quals) : Qualifiers{};
  // A word with no wildcard is itself, its `~`s and all.
  if (not has_quals and not has_glob_chars(raw, extended))
    return vec<str>{glob_unescape(raw)};

  // Under EXTENDED_GLOB, `a~b` excludes paths matching `b`; the exclusion
  // applies to the whole path, so split it off before walking segments.
  vec<CompiledPattern> excludes;
  if (extended) {
    int depth = 0;
    for (s::size_t i = 0; i < raw.size(); ++i) {
      char c = raw[i];
      if (c == '\\') { ++i; continue; }
      if (c == '(' or c == '[') ++depth;
      else if ((c == ')' or c == ']') and depth > 0) --depth;
      else if (c == '~' and depth == 0 and i > 0 and i + 1 < raw.size()) {
        for (s::size_t s = i + 1, e; s <= raw.size(); s = e + 1) {
          e = raw.find('~', s);
          if (e == str::npos) e = raw.size();
          excludes.push_back(CompiledPattern::compile(
              strv(raw).substr(s, e - s),
              not opts.get(OPT_CASEGLOB), true, opts.multibyte()));
        }
        raw.resize(i);
        break;
      }
    }
  }
  if (not has_quals and excludes.empty() and not has_glob_chars(raw, extended))
    return vec<str>{glob_unescape(raw)};

  Walker w{q.dots.value_or((pat.flags & GLOB_DOTFILES) or opts.get(OPT_GLOBDOTS)),
           not opts.get(OPT_CASEGLOB) or (pat.flags & GLOB_NOCASE),
           extended, opts.multibyte()};

  auto segs = split_segments(raw);
  // Each segment with pattern characters, compiled once.  A part zsh
  // cannot compile (`(#X)`, `*#`) makes the word a bad pattern, as an
  // unclosed `[` does.
  vec<opt<CompiledPattern>> seg_pats;
  bool bad = s::any_of(excludes.begin(), excludes.end(),
                       [](const CompiledPattern& ex) { return ex.bad(); });
  for (const auto& seg : segs) {
    seg_pats.emplace_back();
    if (not has_glob_chars(seg, extended)) continue;
    seg_pats.back() = w.compile(seg);
    bad = bad or seg_pats.back()->bad();
  }
  if (bad) {
    if (not opts.get(OPT_BADPATTERN)) return vec<str>{glob_unescape(pat.raw)};
    return s::unexpected(qual_error("bad pattern: " + glob_unescape(pat.raw)));
  }
  vec<str> cur{""};
  s::size_t first = 0;
  if (segs.size() > 1 and segs[0].empty()) {   // absolute path
    cur   = {"/"};
    first = 1;
  }
  bool trailing_slash = segs.size() > 1 and segs.back().empty();
  s::size_t last = trailing_slash ? segs.size() - 1 : segs.size();

  for (s::size_t s = first; s < last; ++s) {
    const str& seg = segs[s];
    bool final_seg = s + 1 == last;
    bool need_dir  = not final_seg or trailing_slash;
    vec<str> next;
    if ((seg == "**" or seg == "***") and (not final_seg or trailing_slash)) {
      for (const auto& dir : cur) w.descend(dir, seg == "***", next);
      // A final `**/` lists the directories below, not the current one.
      if (final_seg) s::erase_if(next, [](const str& d) { return d.empty(); });
    } else if (not seg_pats[s]) {
      str lit = glob_unescape(seg);
      for (const auto& dir : cur) {
        str path = seg.empty() ? dir : join_path(dir, lit);
        struct stat st{};
        if (::lstat(path.c_str(), &st) == 0 and (not need_dir or is_dir(path)))
          next.push_back(s::move(path));
      }
    } else {
      for (const auto& dir : cur) w.match_dir(dir, seg, *seg_pats[s], need_dir, next);
    }
    cur = s::move(next);
    if (cur.empty()) break;
  }
  if (trailing_slash)
    for (auto& p : cur) if (p.empty() or p.back() != '/') p += '/';

  // Exclusions and qualifier tests: by default of the file itself, a
  // symlink not followed.
  s::erase_if(cur, [&](const str& p) {
    for (const auto& ex : excludes)
      if (ex.matches(p)) return true;
    return false;
  });
  // Each match keeps the file it came from, for sorting once modifiers or
  // e's $reply have changed its name.
  struct Match { str name, file; };
  vec<Match> kept;
  for (auto& p : cur) {
    struct stat st{}, target{};
    bool have = false, have_target = false;
    vec<str> instead;
    auto passes = [&](const vec<QualTest>& tests) {
      for (const auto& t : tests) {
        bool ok;
        if (t.kind == 'e' or t.kind == '+') {
          auto r = hooks and hooks->eval ? hooks->eval(t.code, t.kind == '+', p)
                                         : opt<vec<str>>{};
          ok = r.has_value();
          if (ok and not r->empty()) instead = s::move(*r);
        } else {
          if (not have) {
            if (::lstat(p.c_str(), &st) != 0) return false;
            have = true;
          }
          const struct stat* sp = &st;
          if (t.follow) {
            if (not have_target) {
              if (::stat(p.c_str(), &target) != 0) target = st;
              have_target = true;
            }
            sp = &target;
          }
          ok = qual_test(t, *sp, p);
        }
        if (ok == t.negate) return false;
      }
      return true;
    };
    bool any = q.alternatives.empty();
    for (const auto& alt : q.alternatives)
      if (passes(alt)) { any = true; break; }
    if (not any) continue;
    if (instead.empty()) instead.push_back(p);
    for (auto& name : instead) {
      if (q.listtypes.value_or(false)) {
        if (char m = type_mark(p)) name += m;
      } else if (q.markdirs.value_or(opts.get(OPT_MARKDIRS))) {
        if (name.back() != '/' and is_dir(p)) name += '/';
      }
      if (not q.modifiers.empty() and hooks and hooks->modify) {
        auto m = hooks->modify(q.modifiers, name);
        if (not m) return s::unexpected(m.error());
        name = s::move(*m);
      }
      kept.push_back({s::move(name), p});
    }
    if (q.first_n >= 0 and kept.size() >= static_cast<s::size_t>(q.first_n)) break;
  }

  bool numeric = q.numeric.value_or(opts.get(OPT_NUMERICGLOBSORT));
  // With Y and no o, the first matches found, unsorted.
  if (q.sorts.empty() and q.first_n < 0) q.sorts.push_back({'n', false});
  if (not q.sorts.empty() and q.sorts.front().key != 'N') {
    struct Times { s::int64_t sec, nsec; };
    auto value = [](const str& p, char key) -> Times {
      struct stat st{};
      if (::stat(p.c_str(), &st) != 0 and ::lstat(p.c_str(), &st) != 0) return {0, 0};
#ifdef __APPLE__
      auto ts = key == 'a' ? st.st_atimespec : key == 'm' ? st.st_mtimespec : st.st_ctimespec;
#else
      auto ts = key == 'a' ? st.st_atim : key == 'm' ? st.st_mtim : st.st_ctim;
#endif
      switch (key) {
        case 'L': return {static_cast<s::int64_t>(st.st_size), 0};
        case 'l': return {static_cast<s::int64_t>(st.st_nlink), 0};
        default:  return {static_cast<s::int64_t>(ts.tv_sec), static_cast<s::int64_t>(ts.tv_nsec)};
      }
    };
    // zsh's `om` lists the newest first, `oL` the smallest first, `od`
    // the deeper paths first; ties go by name.
    auto cmp = [&](const Match& a, const Match& b) {
      for (auto [key, desc] : q.sorts) {
        int r = 0;
        if (key == 'n') {
          r = name_less(a.name, b.name, numeric) ? -1 : name_less(b.name, a.name, numeric) ? 1 : 0;
        } else if (key == 'd') {
          auto da = s::count(a.file.begin(), a.file.end(), '/');
          auto db = s::count(b.file.begin(), b.file.end(), '/');
          r = da > db ? -1 : da < db ? 1 : 0;
        } else if (key != 'N') {
          Times va = value(a.file, key), vb = value(b.file, key);
          bool newest = key == 'a' or key == 'm' or key == 'c';
          bool same = va.sec == vb.sec and va.nsec == vb.nsec;
          bool less = va.sec < vb.sec or (va.sec == vb.sec and va.nsec < vb.nsec);
          r = same ? 0 : less != newest ? -1 : 1;
        }
        if (r) return desc ? r > 0 : r < 0;
      }
      return name_less(a.name, b.name, numeric);
    };
    s::stable_sort(kept.begin(), kept.end(), cmp);
  } else if (not q.sorts.empty() and q.sorts.front().desc) {
    s::reverse(kept.begin(), kept.end());
  }
  cur.clear();
  for (auto& m : kept) cur.push_back(s::move(m.name));

  if (q.select and not cur.empty()) {
    auto       n  = static_cast<s::int64_t>(cur.size());
    s::int64_t lo = q.sel_lo < 0 ? n + q.sel_lo + 1 : q.sel_lo;
    s::int64_t hi = q.sel_hi < 0 ? n + q.sel_hi + 1 : q.sel_hi;
    lo = s::max<s::int64_t>(lo, 1);
    hi = s::min(hi, n);
    if (lo > hi) cur.clear();
    else cur = vec<str>(cur.begin() + (lo - 1), cur.begin() + hi);
  }

  if (not q.prepend.empty()) {
    vec<str> out;
    for (auto& p : cur) {
      out.insert(out.end(), q.prepend.begin(), q.prepend.end());
      out.push_back(s::move(p));
    }
    cur = s::move(out);
  }

  if (cur.empty()) {
    if (q.nullglob or opts.nullglob()) return vec<str>{};
    if (opts.nomatch())
      return s::unexpected(ShellError{ErrorKind::BadPattern,
          "no matches found: " + glob_unescape(pat.raw)});
    return vec<str>{glob_unescape(pat.raw)};
  }
  return cur;
}

Result<vec<str>> expand_args(
    vec<str>           args,
    const OptionState& opts)
{
  vec<str> result;
  for (auto& arg : args) {
    auto expanded = glob_expand(GlobPattern::compile(arg), opts);
    if (not expanded) return s::unexpected(expanded.error());
    for (auto& m : *expanded) result.push_back(s::move(m));
  }
  return result;
}

}  // namespace jfsh

