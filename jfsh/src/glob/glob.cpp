#include <cctype>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <functional>

#include <jfsh/glob/glob.h>

namespace jfsh {

// ---------------------------------------------------------------------------
// Pattern compiler.  Patterns arrive in escaped form: a backslash makes the
// next character literal, everything else unescaped is pattern syntax.
// ---------------------------------------------------------------------------

using PatternSeq = std::vector<CompiledPattern::Node>;

struct CompiledPattern::Node {
  enum class Kind : std::uint8_t { Lit, Any, Star, Class, Group, Range, Repeat };

  Kind kind = Kind::Lit;
  char ch   = 0;                                         // Lit

  bool class_negated = false;                            // Class
  std::vector<std::pair<unsigned char, unsigned char>> ranges;
  std::vector<int (*)(int)> classes;

  std::vector<PatternSeq> alts;                          // Group

  bool      has_lo = false, has_hi = false;              // Range <lo-hi>
  long long lo = 0, hi = 0;

  std::shared_ptr<Node> sub;                             // Repeat x# / x##
  int min = 0;
};

struct CompiledPattern::Impl {
  PatternSeq              main;
  std::vector<PatternSeq> excludes;   // a~b~c: `b` and `c`
  bool                    negate = false;   // ^pat
  bool                    nocase = false;
};

namespace {

using Node = CompiledPattern::Node;
using Seq  = PatternSeq;
using Kind = Node::Kind;

int (*class_fn(std::string_view name))(int) {
  if (name == "alpha")  return [](int c) { return std::isalpha(c); };
  if (name == "digit")  return [](int c) { return std::isdigit(c); };
  if (name == "alnum")  return [](int c) { return std::isalnum(c); };
  if (name == "upper")  return [](int c) { return std::isupper(c); };
  if (name == "lower")  return [](int c) { return std::islower(c); };
  if (name == "space")  return [](int c) { return std::isspace(c); };
  if (name == "blank")  return [](int c) { return std::isblank(c); };
  if (name == "punct")  return [](int c) { return std::ispunct(c); };
  if (name == "print")  return [](int c) { return std::isprint(c); };
  if (name == "graph")  return [](int c) { return std::isgraph(c); };
  if (name == "cntrl")  return [](int c) { return std::iscntrl(c); };
  if (name == "xdigit") return [](int c) { return std::isxdigit(c); };
  return nullptr;
}

struct PatternParser {
  std::string_view p;
  bool             ext;
  std::size_t      i = 0;

  // `[…]` starting at p[i] == '['.  Returns false (and leaves i) when the
  // bracket is never closed, so the caller treats `[` as a literal.
  bool parse_class(Node& n) {
    std::size_t j = i + 1;
    n.kind = Kind::Class;
    if (j < p.size() and (p[j] == '!' or p[j] == '^')) { n.class_negated = true; ++j; }
    bool first = true;
    while (j < p.size() and (p[j] != ']' or first)) {
      first = false;
      if (p[j] == '[' and j + 1 < p.size() and p[j + 1] == ':') {
        auto close = p.find(":]", j + 2);
        if (close != std::string_view::npos) {
          if (auto fn = class_fn(p.substr(j + 2, close - j - 2))) {
            n.classes.push_back(fn);
            j = close + 2;
            continue;
          }
        }
      }
      unsigned char lo = static_cast<unsigned char>(p[j]);
      if (p[j] == '\\' and j + 1 < p.size()) lo = static_cast<unsigned char>(p[++j]);
      ++j;
      unsigned char hi = lo;
      if (j + 1 < p.size() and p[j] == '-' and p[j + 1] != ']') {
        ++j;
        hi = static_cast<unsigned char>(p[j]);
        if (p[j] == '\\' and j + 1 < p.size()) hi = static_cast<unsigned char>(p[++j]);
        ++j;
      }
      n.ranges.emplace_back(lo, hi);
    }
    if (j >= p.size()) return false;
    i = j + 1;
    return true;
  }

  // `<lo-hi>` with either bound optional.
  bool parse_range(Node& n) {
    std::size_t j = i + 1;
    auto digits = [&](bool& has, long long& v) {
      std::size_t s = j;
      while (j < p.size() and std::isdigit(static_cast<unsigned char>(p[j]))) ++j;
      has = j > s;
      if (has) v = std::strtoll(std::string(p.substr(s, j - s)).c_str(), nullptr, 10);
    };
    digits(n.has_lo, n.lo);
    if (j >= p.size() or p[j] != '-') return false;
    ++j;
    digits(n.has_hi, n.hi);
    if (j >= p.size() or p[j] != '>') return false;
    n.kind = Kind::Range;
    i = j + 1;
    return true;
  }

  bool parse_group(Node& n) {
    std::size_t save = i;
    ++i;
    n.kind = Kind::Group;
    for (;;) {
      n.alts.push_back(parse_seq(true));
      if (i >= p.size()) { i = save; n.alts.clear(); return false; }
      if (p[i] == ')') { ++i; return true; }
      ++i;  // '|'
    }
  }

  Seq parse_seq(bool in_group) {
    Seq seq;
    while (i < p.size()) {
      char c = p[i];
      if (in_group and (c == '|' or c == ')')) break;
      if (ext and not in_group and c == '~' and not seq.empty()) break;
      Node n;
      if (c == '\\' and i + 1 < p.size()) {
        n.ch = p[i + 1];
        i += 2;
      } else if (c == '*') {
        n.kind = Kind::Star;
        while (i < p.size() and p[i] == '*') ++i;
      } else if (c == '?') {
        n.kind = Kind::Any;
        ++i;
      } else if (c == '[' and parse_class(n)) {
      } else if (c == '(' and parse_group(n)) {
      } else if (c == '<' and parse_range(n)) {
      } else {
        n = Node{};
        n.ch = c;
        ++i;
      }
      if (ext and i < p.size() and p[i] == '#') {
        Node rep;
        rep.kind = Kind::Repeat;
        rep.min  = 0;
        ++i;
        if (i < p.size() and p[i] == '#') { rep.min = 1; ++i; }
        rep.sub = std::make_shared<Node>(std::move(n));
        n = std::move(rep);
      }
      seq.push_back(std::move(n));
    }
    return seq;
  }
};

unsigned char fold(unsigned char c, bool nocase) noexcept {
  return nocase ? static_cast<unsigned char>(std::tolower(c)) : c;
}

bool class_has(const Node& n, unsigned char c, bool nocase) {
  bool in = false;
  for (auto [lo, hi] : n.ranges) {
    if (c >= lo and c <= hi) { in = true; break; }
    if (nocase) {
      unsigned char l = static_cast<unsigned char>(std::tolower(c));
      unsigned char u = static_cast<unsigned char>(std::toupper(c));
      if ((l >= lo and l <= hi) or (u >= lo and u <= hi)) { in = true; break; }
    }
  }
  if (not in)
    for (auto fn : n.classes)
      if (fn(c)) { in = true; break; }
  return in != n.class_negated;
}

using Cont = std::function<bool(std::size_t)>;

struct Matcher {
  std::string_view s;
  bool             nocase;

  bool node(const Node& n, std::size_t pos, const Cont& k) const {
    switch (n.kind) {
      case Kind::Lit:
        return pos < s.size()
           and fold(static_cast<unsigned char>(s[pos]), nocase)
               == fold(static_cast<unsigned char>(n.ch), nocase)
           and k(pos + 1);
      case Kind::Any:
        return pos < s.size() and k(pos + 1);
      case Kind::Star:
        for (std::size_t e = pos; e <= s.size(); ++e)
          if (k(e)) return true;
        return false;
      case Kind::Class:
        return pos < s.size()
           and class_has(n, static_cast<unsigned char>(s[pos]), nocase)
           and k(pos + 1);
      case Kind::Group:
        for (const auto& alt : n.alts)
          if (seq(alt, 0, pos, k)) return true;
        return false;
      case Kind::Range: {
        std::size_t e = pos;
        while (e < s.size() and std::isdigit(static_cast<unsigned char>(s[e]))) ++e;
        for (std::size_t end = e; end > pos; --end) {
          long long v = std::strtoll(std::string(s.substr(pos, end - pos)).c_str(),
                                     nullptr, 10);
          if ((not n.has_lo or v >= n.lo) and (not n.has_hi or v <= n.hi) and k(end))
            return true;
        }
        return false;
      }
      case Kind::Repeat:
        return repeat(n, 0, pos, k);
    }
    return false;
  }

  bool repeat(const Node& n, int count, std::size_t pos, const Cont& k) const {
    if (count >= n.min and k(pos)) return true;
    return node(*n.sub, pos, [&](std::size_t e) {
      return e > pos and repeat(n, count + 1, e, k);
    });
  }

  bool seq(const Seq& q, std::size_t i, std::size_t pos, const Cont& k) const {
    if (i == q.size()) return k(pos);
    return node(q[i], pos, [&](std::size_t e) { return seq(q, i + 1, e, k); });
  }

  bool whole(const Seq& q) const {
    return seq(q, 0, 0, [&](std::size_t e) { return e == s.size(); });
  }
};

}  // namespace

CompiledPattern CompiledPattern::compile(std::string_view pattern,
                                         bool case_insensitive, bool extended) {
  auto impl = std::make_shared<Impl>();
  impl->nocase = case_insensitive;
  PatternParser pp{pattern, extended};
  if (extended and pattern.size() > 1 and pattern[0] == '^') {
    impl->negate = true;
    pp.i         = 1;
  }
  impl->main = pp.parse_seq(false);
  while (pp.i < pattern.size() and pattern[pp.i] == '~') {
    ++pp.i;
    impl->excludes.push_back(pp.parse_seq(false));
  }
  CompiledPattern cp;
  cp.mImpl = std::move(impl);
  return cp;
}

bool CompiledPattern::matches(std::string_view str) const {
  Matcher m{str, mImpl->nocase};
  if (m.whole(mImpl->main) == mImpl->negate) return false;
  for (const auto& ex : mImpl->excludes)
    if (m.whole(ex)) return false;
  return true;
}

std::size_t CompiledPattern::match_prefix(std::string_view str, bool longest) const {
  for (std::size_t n = 0; n <= str.size(); ++n) {
    std::size_t len = longest ? str.size() - n : n;
    if (matches(str.substr(0, len))) return len;
  }
  return std::string_view::npos;
}

bool pattern_match(std::string_view pattern, std::string_view str,
                   bool case_insensitive, bool extended) noexcept {
  try {
    return CompiledPattern::compile(pattern, case_insensitive, extended).matches(str);
  } catch (...) {
    return false;
  }
}

bool ext_pattern_match(std::string_view pattern, std::string_view str) noexcept {
  return pattern_match(pattern, str, false, true);
}

bool has_glob_chars(std::string_view s, bool extended) noexcept {
  for (std::size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\\') { ++i; continue; }
    if (c == '*' or c == '?') return true;
    // `[` and `(` are pattern characters only when closed; a lone `[` is
    // the test command.
    if (c == '[' and s.find(']', i + 2) != std::string_view::npos) return true;
    if (c == '(' and s.find(')', i + 1) != std::string_view::npos) return true;
    if (c == '<') {
      PatternParser pp{s, false, i};
      Node n;
      if (pp.parse_range(n)) return true;
    }
    if (extended and (c == '^' or c == '#' or (c == '~' and i > 0))) return true;
  }
  return false;
}

std::string glob_unescape(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\' and i + 1 < s.size()) ++i;
    out += s[i];
  }
  return out;
}

GlobPattern GlobPattern::compile(std::string_view pat, GlobFlags flags) {
  GlobPattern g;
  g.raw   = std::string(pat);
  g.flags = flags;
  return g;
}

// ---------------------------------------------------------------------------
// Filename generation.
// ---------------------------------------------------------------------------
namespace {

struct Qualifiers {
  struct Test { char type; bool negate; };
  std::vector<Test> tests;
  bool nullglob = false;
  bool dots     = false;
  char sort_key = 0;       // n L m a c (0 = default name order)
  bool sort_desc = false;
  bool select   = false;   // [n] or [n,m]
  long sel_lo = 1, sel_hi = 1;
};

// Splits zsh glob qualifiers `(…)` off the end of `raw`.  Only a final
// group with no `|` whose characters are all qualifier characters counts.
bool split_qualifiers(std::string& raw, Qualifiers& q) {
  if (raw.size() < 3 or raw.back() != ')' or raw[raw.size() - 2] == '\\')
    return true;
  std::size_t open = raw.rfind('(');
  if (open == std::string::npos or open == 0 or (open > 0 and raw[open - 1] == '\\'))
    return true;
  std::string_view body(raw.data() + open + 1, raw.size() - open - 2);
  if (body.empty() or body.find('|') != std::string_view::npos) return true;
  constexpr std::string_view kQualChars = "./@*=p%rwxRWXNDoOnLmac^-[],0123456789";
  if (body.find_first_not_of(kQualChars) != std::string_view::npos) return true;

  Qualifiers out;
  bool negate = false;
  for (std::size_t i = 0; i < body.size(); ++i) {
    char c = body[i];
    switch (c) {
      case '^': negate = not negate;
      break; case '-':                      // follow symlinks: stat() is used anyway
      break; case 'N': out.nullglob = true;
      break; case 'D': out.dots = true;
      break; case 'o': case 'O':
        if (i + 1 >= body.size()) return false;
        out.sort_key  = body[++i];
        out.sort_desc = c == 'O';
        if (std::string_view("nLmac").find(out.sort_key) == std::string_view::npos)
          return false;
      break; case '[': {
        std::size_t close = body.find(']', i);
        if (close == std::string_view::npos) return false;
        std::string spec(body.substr(i + 1, close - i - 1));
        auto comma = spec.find(',');
        out.select = true;
        out.sel_lo = std::strtol(spec.c_str(), nullptr, 10);
        out.sel_hi = comma == std::string::npos
            ? out.sel_lo : std::strtol(spec.c_str() + comma + 1, nullptr, 10);
        i = close;
      }
      break; default:
        if (std::string_view("./@*=p%rwxRWX").find(c) == std::string_view::npos)
          return false;
        out.tests.push_back({c, negate});
    }
  }
  raw.resize(open);
  q = out;
  return true;
}

bool qual_test(char type, const std::string& path) {
  struct stat st{};
  if (type == '@') return ::lstat(path.c_str(), &st) == 0 and S_ISLNK(st.st_mode);
  if (::stat(path.c_str(), &st) != 0) return false;
  switch (type) {
    case '.': return S_ISREG(st.st_mode);
    case '/': return S_ISDIR(st.st_mode);
    case '*': return S_ISREG(st.st_mode) and (st.st_mode & 0111);
    case '=': return S_ISSOCK(st.st_mode);
    case 'p': return S_ISFIFO(st.st_mode);
    case '%': return S_ISCHR(st.st_mode) or S_ISBLK(st.st_mode);
    case 'r': return st.st_mode & S_IRUSR;
    case 'w': return st.st_mode & S_IWUSR;
    case 'x': return st.st_mode & S_IXUSR;
    case 'R': return st.st_mode & S_IROTH;
    case 'W': return st.st_mode & S_IWOTH;
    case 'X': return st.st_mode & S_IXOTH;
    default:  return false;
  }
}

bool is_dir(const std::string& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 and S_ISDIR(st.st_mode);
}

std::string join_path(const std::string& dir, std::string_view name) {
  if (dir.empty()) return std::string(name);
  if (dir.back() == '/') return dir + std::string(name);
  return dir + '/' + std::string(name);
}

// Lexical order, except that with NUMERIC_GLOB_SORT runs of digits compare
// by value.
bool name_less(const std::string& a, const std::string& b, bool numeric) {
  if (not numeric) return a < b;
  std::size_t i = 0, j = 0;
  while (i < a.size() and j < b.size()) {
    bool da = std::isdigit(static_cast<unsigned char>(a[i]));
    bool db = std::isdigit(static_cast<unsigned char>(b[j]));
    if (da and db) {
      std::size_t ei = i, ej = j;
      while (ei < a.size() and std::isdigit(static_cast<unsigned char>(a[ei]))) ++ei;
      while (ej < b.size() and std::isdigit(static_cast<unsigned char>(b[ej]))) ++ej;
      unsigned long long va = std::strtoull(a.substr(i, ei - i).c_str(), nullptr, 10);
      unsigned long long vb = std::strtoull(b.substr(j, ej - j).c_str(), nullptr, 10);
      if (va != vb) return va < vb;
      i = ei; j = ej;
      continue;
    }
    if (a[i] != b[j]) return a[i] < b[j];
    ++i; ++j;
  }
  return a.size() - i < b.size() - j;
}

struct Walker {
  bool dots;
  bool nocase;
  bool extended;

  // Every directory below `dir`, depth first, `dir` itself first.  For `**`
  // symlinks to directories are not followed; for `***` they are.
  void descend(const std::string& dir, bool follow, std::vector<std::string>& out) {
    out.push_back(dir);
    DIR* d = ::opendir(dir.empty() ? "." : dir.c_str());
    if (not d) return;
    std::vector<std::string> subdirs;
    while (auto* ent = ::readdir(d)) {
      std::string_view name(ent->d_name);
      if (name == "." or name == "..") continue;
      if (name[0] == '.' and not dots) continue;
      std::string path = join_path(dir, name);
      struct stat st{};
      int rc = follow ? ::stat(path.c_str(), &st) : ::lstat(path.c_str(), &st);
      if (rc == 0 and S_ISDIR(st.st_mode)) subdirs.push_back(path);
    }
    ::closedir(d);
    std::sort(subdirs.begin(), subdirs.end());
    for (const auto& s : subdirs) descend(s, follow, out);
  }

  void match_dir(const std::string& dir, std::string_view seg, bool need_dir,
                 std::vector<std::string>& out) {
    DIR* d = ::opendir(dir.empty() ? "." : dir.c_str());
    if (not d) return;
    auto pat = CompiledPattern::compile(seg, nocase, extended);
    bool dot_ok = dots or (not seg.empty() and seg[0] == '.')
               or (seg.size() > 1 and seg[0] == '\\' and seg[1] == '.');
    while (auto* ent = ::readdir(d)) {
      std::string_view name(ent->d_name);
      if (name == "." or name == "..") continue;
      if (name[0] == '.' and not dot_ok) continue;
      if (not pat.matches(name)) continue;
      std::string path = join_path(dir, name);
      if (need_dir and not is_dir(path)) continue;
      out.push_back(std::move(path));
    }
    ::closedir(d);
  }
};

std::vector<std::string> split_segments(std::string_view raw) {
  std::vector<std::string> segs;
  std::string cur;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == '\\' and i + 1 < raw.size()) {
      cur += raw[i];
      cur += raw[++i];
    } else if (raw[i] == '/') {
      segs.push_back(std::move(cur));
      cur.clear();
    } else {
      cur += raw[i];
    }
  }
  segs.push_back(std::move(cur));
  return segs;
}

}  // namespace

Result<std::vector<std::string>> glob_expand(
    const GlobPattern& pat,
    const OptionState& opts)
{
  std::string raw = pat.raw;
  Qualifiers  q;
  bool extended = opts.extended_glob() or (pat.flags & GLOB_EXTGLOB);
  if (opts.get(OPT_BAREGLOBQUAL) and not split_qualifiers(raw, q))
    return std::unexpected(ShellError{ErrorKind::BadPattern,
        "unknown file attribute: " + pat.raw});
  bool has_quals = raw.size() != pat.raw.size();

  // Under EXTENDED_GLOB, `a~b` excludes paths matching `b`; the exclusion
  // applies to the whole path, so split it off before walking segments.
  std::vector<CompiledPattern> excludes;
  if (extended) {
    int depth = 0;
    for (std::size_t i = 1; i < raw.size(); ++i) {
      char c = raw[i];
      if (c == '\\') { ++i; continue; }
      if (c == '(' or c == '[') ++depth;
      else if ((c == ')' or c == ']') and depth > 0) --depth;
      else if (c == '~' and depth == 0) {
        for (std::size_t s = i + 1, e; s <= raw.size(); s = e + 1) {
          e = raw.find('~', s);
          if (e == std::string::npos) e = raw.size();
          excludes.push_back(CompiledPattern::compile(
              std::string_view(raw).substr(s, e - s),
              not opts.get(OPT_CASEGLOB), true));
        }
        raw.resize(i);
        break;
      }
    }
  }
  if (not has_quals and not has_glob_chars(raw, extended))
    return std::vector<std::string>{glob_unescape(raw)};

  Walker w{q.dots or (pat.flags & GLOB_DOTFILES) or opts.get(OPT_GLOBDOTS),
           not opts.get(OPT_CASEGLOB) or (pat.flags & GLOB_NOCASE),
           extended};

  auto segs = split_segments(raw);
  std::vector<std::string> cur{""};
  std::size_t first = 0;
  if (segs.size() > 1 and segs[0].empty()) {   // absolute path
    cur   = {"/"};
    first = 1;
  }
  bool trailing_slash = segs.size() > 1 and segs.back().empty();
  std::size_t last = trailing_slash ? segs.size() - 1 : segs.size();

  for (std::size_t s = first; s < last; ++s) {
    const std::string& seg = segs[s];
    bool final_seg = s + 1 == last;
    bool need_dir  = not final_seg or trailing_slash;
    std::vector<std::string> next;
    if ((seg == "**" or seg == "***") and not final_seg) {
      for (const auto& dir : cur) w.descend(dir, seg == "***", next);
    } else if (not has_glob_chars(seg, extended)) {
      std::string lit = glob_unescape(seg);
      for (const auto& dir : cur) {
        std::string path = seg.empty() ? dir : join_path(dir, lit);
        struct stat st{};
        if (::lstat(path.c_str(), &st) == 0 and (not need_dir or is_dir(path)))
          next.push_back(std::move(path));
      }
    } else {
      for (const auto& dir : cur) w.match_dir(dir, seg, need_dir, next);
    }
    cur = std::move(next);
    if (cur.empty()) break;
  }
  if (trailing_slash)
    for (auto& p : cur) if (p.empty() or p.back() != '/') p += '/';

  // Exclusions and qualifier tests.
  std::erase_if(cur, [&](const std::string& p) {
    for (const auto& ex : excludes)
      if (ex.matches(p)) return true;
    for (auto t : q.tests)
      if (qual_test(t.type, p) == t.negate) return true;
    return false;
  });

  bool numeric = opts.get(OPT_NUMERICGLOBSORT) or q.sort_key == 'n';
  if (q.sort_key == 0 or q.sort_key == 'n') {
    std::sort(cur.begin(), cur.end(), [&](const auto& a, const auto& b) {
      return name_less(a, b, numeric);
    });
    if (q.sort_desc) std::reverse(cur.begin(), cur.end());
  } else {
    auto key = [&](const std::string& p) -> long long {
      struct stat st{};
      if (::stat(p.c_str(), &st) != 0) return 0;
      switch (q.sort_key) {
        case 'L': return static_cast<long long>(st.st_size);
        case 'm': return static_cast<long long>(st.st_mtime);
        case 'a': return static_cast<long long>(st.st_atime);
        default:  return static_cast<long long>(st.st_ctime);
      }
    };
    // zsh's `om` lists the newest first; `oL` the smallest first.
    bool newest_first = q.sort_key != 'L';
    std::stable_sort(cur.begin(), cur.end(), [&](const auto& a, const auto& b) {
      long long ka = key(a), kb = key(b);
      if (ka == kb) return a < b;
      bool lt = newest_first ? ka > kb : ka < kb;
      return q.sort_desc ? not lt : lt;
    });
  }

  if (q.select and not cur.empty()) {
    long n  = static_cast<long>(cur.size());
    long lo = q.sel_lo < 0 ? n + q.sel_lo + 1 : q.sel_lo;
    long hi = q.sel_hi < 0 ? n + q.sel_hi + 1 : q.sel_hi;
    lo = std::max(lo, 1L);
    hi = std::min(hi, n);
    if (lo > hi) cur.clear();
    else cur = std::vector<std::string>(cur.begin() + (lo - 1), cur.begin() + hi);
  }

  if (opts.get(OPT_MARKDIRS))
    for (auto& p : cur)
      if (p.back() != '/' and is_dir(p)) p += '/';

  if (cur.empty()) {
    if (q.nullglob or opts.nullglob()) return std::vector<std::string>{};
    if (opts.nomatch())
      return std::unexpected(ShellError{ErrorKind::BadPattern,
          "no matches found: " + glob_unescape(pat.raw)});
    return std::vector<std::string>{glob_unescape(pat.raw)};
  }
  return cur;
}

Result<std::vector<std::string>> expand_args(
    std::vector<std::string> args,
    const OptionState&       opts)
{
  std::vector<std::string> result;
  for (auto& arg : args) {
    auto expanded = glob_expand(GlobPattern::compile(arg), opts);
    if (not expanded) return std::unexpected(expanded.error());
    for (auto& m : *expanded) result.push_back(std::move(m));
  }
  return result;
}

}  // namespace jfsh
