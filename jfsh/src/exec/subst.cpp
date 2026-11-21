#include <jfsh/exec/subst.h>

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pwd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <unordered_set>

#include <jfsh/builtins/math.h>
#include <jfsh/core/aliases.h>
#include <jfsh/core/sort.h>
#include <jfsh/exec/executor.h>
#include <jfsh/glob/glob.h>
#include <jfsh/lexer/input.h>
#include <jfsh/lexer/lexer.h>

// TODO(subst/param): still missing from ${…}: named directories for (D)
//   (M17), the (g), (S), (B)/(E)/(N) match-position flags, and subscript
//   flags on the left of an assignment (`a[(i)x]=y`).

namespace jfsh {

namespace {

constexpr strv DEFAULT_IFS = strv(" \t\n\0", 4);
constexpr strv PATTERN_CHARS = "*?[]()|<>^#~\\";

ShellError subst_error(str msg) {
  return ShellError{ErrorKind::BadSubstitution, s::move(msg)};
}

bool is_name_start(char c) noexcept {
  return s::isalpha(static_cast<unsigned char>(c)) or c == '_';
}
bool is_name_char(char c) noexcept {
  return s::isalnum(static_cast<unsigned char>(c)) or c == '_';
}
bool is_special_param(char c) noexcept {
  return strv("?$!#-@*0").find(c) != strv::npos;
}

// Index of the character that closes the bracket at w[open], skipping
// quoted text and nested substitutions; npos when it is never closed.
// `dq`: a ${…} in double quotes, where, as in zsh, a `{` inside does not
// pair with a `}` ("${x:-a{b\}c}") and a `'` is an ordinary character.
s::size_t find_close(strv w, s::size_t open, bool dq = false) {
  char o = w[open];
  char c = o == '(' ? ')' : o == '{' ? '}' : ']';
  int depth = 1;
  for (s::size_t i = open + 1; i < w.size(); ++i) {
    char ch = w[i];
    if (ch == '\\') { ++i; continue; }
    if (ch == '$' and i + 1 < w.size() and w[i + 1] == '\'' and o != '[') {
      // $'…', where `\'` doesn't end it.
      for (i += 2; i < w.size() and w[i] != '\''; ++i)
        if (w[i] == '\\') ++i;
      if (i >= w.size()) return strv::npos;
      continue;
    }
    if (ch == '\'' and o != '[' and not dq) {
      auto e = w.find('\'', i + 1);
      if (e == strv::npos) return e;
      i = e;
    } else if (ch == '"') {
      for (++i; i < w.size() and w[i] != '"'; ++i)
        if (w[i] == '\\') ++i;
    } else if (ch == '`') {
      for (++i; i < w.size() and w[i] != '`'; ++i)
        if (w[i] == '\\') ++i;
    } else if (ch == '$' and i + 1 < w.size()
               and (w[i + 1] == '(' or w[i + 1] == '{') and (w[i + 1] != o or dq)) {
      auto e = find_close(w, i + 1, dq);
      if (e == strv::npos) return e;
      i = e;
    } else if (ch == o and not (dq and o == '{')) {
      ++depth;
    } else if (ch == c and --depth == 0) {
      return i;
    }
  }
  return strv::npos;
}

void append_utf8(str& out, s::uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xC0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xE0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  } else {
    out += static_cast<char>(0xF0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
    out += static_cast<char>(0x80 | (cp & 0x3F));
  }
}

// Decodes the body of $'…' (and the (p) flag's separators): C escapes.
str c_escapes(strv s) {
  str out;
  for (s::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' or i + 1 >= s.size()) { out += s[i]; continue; }
    char e = s[++i];
    auto hex = [&](s::size_t max) {
      s::uint32_t v = 0;
      s::size_t n = 0;
      while (n < max and i + 1 < s.size()
             and s::isxdigit(static_cast<unsigned char>(s[i + 1]))) {
        char h = s[++i];
        v = v * 16 + (s::isdigit(static_cast<unsigned char>(h))
                      ? static_cast<s::uint32_t>(h - '0')
                      : static_cast<s::uint32_t>(s::tolower(h) - 'a' + 10));
        ++n;
      }
      return v;
    };
    switch (e) {
      case 'a': out += '\a';
      break; case 'b': out += '\b';
      break; case 'e': case 'E': out += '\x1b';
      break; case 'f': out += '\f';
      break; case 'n': out += '\n';
      break; case 'r': out += '\r';
      break; case 't': out += '\t';
      break; case 'v': out += '\v';
      break; case 'x': out += static_cast<char>(hex(2));
      break; case 'u': append_utf8(out, hex(4));
      break; case 'U': append_utf8(out, hex(8));
      break; case 'c':
        if (i + 1 < s.size()) out += static_cast<char>(s[++i] & 0x1f);
      break; case '0': case '1': case '2': case '3':
      case '4': case '5': case '6': case '7': {
        int v = e - '0';
        for (int n = 0; n < 2 and i + 1 < s.size() and s[i + 1] >= '0' and s[i + 1] <= '7'; ++n)
          v = v * 8 + (s[++i] - '0');
        out += static_cast<char>(v);
      }
      break; default: out += e;
    }
  }
  return out;
}

str quote_backslash(strv s) {
  constexpr strv SPECIAL = " \t\n'\"\\$`&|;<>()[]{}*?#~=%!^";
  str out;
  for (char c : s) {
    if (SPECIAL.find(c) != strv::npos) out += '\\';
    out += c;
  }
  return out;
}

str quote_single(strv s) {
  str out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else           out += c;
  }
  return out + "'";
}

// (qqq): in double quotes, a backslash before " $ ` and \.
str quote_double(strv s) {
  str out = "\"";
  for (char c : s) {
    if (strv("\"$`\\").find(c) != strv::npos) out += '\\';
    out += c;
  }
  return out + "\"";
}

// (qqqq): in $'…', with C escapes for what needs them.
str quote_dollar(strv s) {
  str out = "$'";
  for (char c : s) {
    auto u = static_cast<unsigned char>(c);
    if (c == '\n') out += "\\n";
    else if (c == '\t') out += "\\t";
    else if (c == '\r') out += "\\r";
    else if (c == '\033') out += "\\e";
    else if (c == '\'' or c == '\\') { out += '\\'; out += c; }
    else if (u < 0x20 or u == 0x7f) {
      char buf[8];
      s::snprintf(buf, sizeof buf, "\\%03o", u);
      out += buf;
    } else out += c;
  }
  return out + "'";
}

// zsh's (q-): quote only when needed, with single quotes.
str quote_minimal(strv s) {
  constexpr strv SAFE =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_@%+=:,./-";
  if (not s.empty() and s.find_first_not_of(SAFE) == strv::npos)
    return str(s);
  return quote_single(s);
}

// (q+): as (q-), but text with control characters goes in $'…'.
str quote_minimal_plus(strv s) {
  bool control = s::any_of(s.begin(), s.end(), [](char c) {
    auto u = static_cast<unsigned char>(c);
    return u < 0x20 or u == 0x7f;
  });
  if (not control) return quote_minimal(s);
  str out = "$'";
  for (char c : s) {
    auto u = static_cast<unsigned char>(c);
    if (c == '\n') out += "\\n";
    else if (c == '\t') out += "\\t";
    else if (c == '\'' or c == '\\') { out += '\\'; out += c; }
    else if (u < 0x20 or u == 0x7f) {
      char buf[8];
      s::snprintf(buf, sizeof buf, "\\%03o", u);
      out += buf;
    } else out += c;
  }
  return out + "'";
}

// ---------------------------------------------------------------------------
// Fields: words under construction.  `lit[i]` marks text[i] as literal,
// i.e. quoted or produced by an expansion, so not pattern/brace/tilde syntax.
// ---------------------------------------------------------------------------
struct Field {
  str       text;
  vec<bool> lit;
  bool quoted   = false;   // had quotes, so it survives being empty
  bool empty_at = false;   // an empty "$@" landed here
  bool dropped  = false;   // ${^x} of an empty array: the word goes away

  void put(char c, bool l) { text += c; lit.push_back(l); }
  void put(strv s, bool l) { for (char c : s) put(c, l); }

  Field slice(s::size_t b, s::size_t e) const {
    Field f;
    f.text   = text.substr(b, e - b);
    f.lit    = vec<bool>(lit.begin() + static_cast<s::ptrdiff_t>(b),
                         lit.begin() + static_cast<s::ptrdiff_t>(e));
    f.quoted = quoted;
    return f;
  }
  void append(const Field& o) {
    text += o.text;
    lit.insert(lit.end(), o.lit.begin(), o.lit.end());
  }
  bool unlit(s::size_t i) const { return i < text.size() and not lit[i]; }

  // The text as a pattern: literal pattern characters get a backslash.
  str as_pattern() const {
    str out;
    for (s::size_t i = 0; i < text.size(); ++i) {
      if (lit[i] and PATTERN_CHARS.find(text[i]) != strv::npos) out += '\\';
      out += text[i];
    }
    return out;
  }
  bool has_unlit_glob() const {
    for (s::size_t i = 0; i < text.size(); ++i)
      if (not lit[i] and strv("*?[(<^#~|").find(text[i]) != strv::npos)
        return true;
    return false;
  }
};

// A parameter's value while ${…} is being worked on.
struct Value {
  bool     set   = false;
  bool     array = false;
  // The elements came from a quoted string (${~"${(@)a}"}): empty ones
  // stay even unquoted.
  bool     quoted_elems = false;
  vec<str> arr;
  str              scalar;
  // The words of a ${x:-word} operand with their quoting kept, so that
  // unquoted pattern characters in `word` still glob.  Only used while
  // the value's text is still exactly these words.
  vec<Field> words;

  vec<str> list() const {
    if (array) return arr;
    return {scalar};
  }
  bool words_current() const {
    if (words.empty()) return false;
    auto l = list();
    if (l.size() != words.size()) return false;
    for (s::size_t k = 0; k < l.size(); ++k)
      if (l[k] != words[k].text) return false;
    return true;
  }
  void make_scalar(str s) { array = false; scalar = s::move(s); arr.clear(); }
  void make_array(vec<str> a) { array = true; arr = s::move(a); scalar.clear(); }
};

// The (flags) of ${(flags)name}.
struct ParamFlags {
  bool upper = false, lower = false, capitalize = false;
  bool at = false, keys = false, values = false;
  bool sort_asc = false, sort_desc = false, sort_nocase = false, sort_numeric = false;
  bool unique = false, indirect = false, type = false, split_words = false;
  bool unquote = false, escapes = false;
  int  quote = 0;              // q count; -1 for (q-)
  bool join = false, split = false;
  str join_sep, split_sep;
  // M13 Phase 13a additions:
  bool visible = false;        // (V): escape unprintables; empty elems as ''
  bool arith   = false;        // (#): arithmetic-eval each element
  bool count_c = false;        // (c): length in characters (after flags)
  bool count_w = false;        // (w): count words (after split)
  bool dirabbr = false;        // (D): abbreviate $HOME as ~ in result
  bool match_mode = false;     // (M): invert `:#pat` filter to keep matches
  // M13 Phase 13b additions:
  bool prompt  = false;        // (%): prompt-escape expansion
  bool reeval  = false;        // (e): parameter/command/arith re-expansion
  struct Pad {
    bool on = false;
    str  width;                // arithmetic expression
    str  fill = " ";           // repeated to fill
    str  once;                 // put once next to the word
  };
  Pad pad_l, pad_r;            // (l:n::fill::once:), (r:n::fill::once:)
  bool glob_pat  = false;      // (~): the value is a pattern
  bool quote_pat = false;      // (b): backslash before pattern characters
  // For # % / and :#: (S) substrings, (B) (E) (N) the match's start, end
  // and length, (R) the rest; (I:n:) the nth match.
  bool m_substr = false, m_begin = false, m_end = false, m_len = false, m_rest = false;
  str  nth;
  int  assign = 0;             // (A) an array, (AA) an association
  opt<str> escapes_g;          // (g:opts:): print's escapes
  str  z_opts;                 // (Z:opts:): (z) with c, C, n
};

constexpr s::size_t NONE = static_cast<s::size_t>(-1);

// Reads the delimited argument that starts at f[i + 1] (`:arg:`, `<arg>`,
// `(arg)`…), leaving `i` on the closing delimiter.
opt<str> read_delimited(strv f, s::size_t& i) {
  if (i + 1 >= f.size()) return s::nullopt;
  char open = f[++i];
  char close = open == '(' ? ')' : open == '[' ? ']' : open == '{' ? '}'
             : open == '<' ? '>' : open;
  auto e = f.find(close, i + 1);
  if (e == strv::npos) return s::nullopt;
  str arg(f.substr(i + 1, e - i - 1));
  i = e;
  return arg;
}

// Pads (or truncates) `s` to `w` columns: on the left for (l), keeping
// the rightmost characters, on the right for (r), keeping the leftmost.
// `once` goes next to the word, `fill` repeats in the rest of the room.
str pad_word(const str& s, s::size_t w, const ParamFlags::Pad& p, bool left) {
  if (s.size() >= w)
    return left ? s.substr(s.size() - w) : s.substr(0, w);
  s::size_t room = w - s.size();
  str once = p.once;
  if (once.size() > room)
    once = left ? once.substr(once.size() - room) : once.substr(0, room);
  s::size_t n = room - once.size();
  str fill = p.fill.empty() ? str(" ") : p.fill, rep;
  while (rep.size() < n) rep += fill;
  rep = left ? rep.substr(rep.size() - n) : rep.substr(0, n);
  return left ? rep + once + s : s + once + rep;
}

enum class Mode : int { Word, Scalar, Heredoc };

struct Scan {
  Expander& ex;
  Mode      mode;
  vec<Field> fields{1};
  bool pending_break = false;
  // After ${^x}, fields[rc_from..] are the copies of the word, one per
  // element; text that follows goes onto every one of them.
  s::size_t rc_from = NONE;
  // Unquoted parameter values are pattern text, as in a subscript flag's
  // pattern: ${a[(r)$p]}.
  bool glob_values = false;
  // Scanning a word as written (not the inside of ${…}): an unquoted
  // `<(…)` / `>(…)`, or `=(…)` at its start, is a process substitution.
  bool top = false;
  // Scanning the operand of a ${…}: in double quotes `\}` is a `}`.
  bool in_brace = false;

  Scan(Expander& e, Mode m) : ex{e}, mode{m} {}

  ExecContext& ctx() { return ex.ctx(); }
  OptionState& opts() { return ex.ctx().opts; }

  Field& cur() {
    if (pending_break) {
      pending_break = false;
      rc_from = NONE;
      fields.emplace_back();
    }
    return fields.back();
  }
  template <typename T>
  void put(T x, bool lit) {
    Field& f = cur();
    if (rc_from == NONE) { f.put(x, lit); return; }
    for (s::size_t k = rc_from; k < fields.size(); ++k) fields[k].put(x, lit);
  }
  void mark_quoted() {
    cur().quoted = true;
    if (rc_from != NONE)
      for (s::size_t k = rc_from; k < fields.size(); ++k) fields[k].quoted = true;
  }
  void new_field(bool quoted) {
    pending_break = false;
    rc_from = NONE;
    fields.emplace_back();
    fields.back().quoted = quoted;
  }
  // ${^x}: the word so far is copied once per element, each copy getting
  // that element; with no elements the word is removed.
  void rc_splice(const vec<str>& parts, bool lit) {
    cur();
    s::size_t from = rc_from == NONE ? fields.size() - 1 : rc_from;
    vec<Field> base(s::make_move_iterator(fields.begin() + static_cast<s::ptrdiff_t>(from)),
                    s::make_move_iterator(fields.end()));
    fields.resize(from);
    rc_from = from;
    if (parts.empty()) {
      fields.emplace_back();
      fields.back().dropped = true;
      return;
    }
    for (auto& b : base)
      for (auto& p : parts) {
        fields.push_back(b);
        fields.back().put(strv(p), lit);
      }
  }
  // First part continues the current word; each further part starts one.
  void splice(const vec<str>& parts, bool lit, bool quoted) {
    for (s::size_t k = 0; k < parts.size(); ++k) {
      if (k) new_field(quoted);
      put(parts[k], lit);
    }
  }

  str ifs() {
    auto p = ctx().params.lookup("IFS");
    if (p and p->get().is_set()) return p->get().scalar();
    return str(DEFAULT_IFS);
  }
  str ifs_first() {
    str s = ifs();
    return s.empty() ? str{} : str(1, s[0]);
  }

  // Unquoted text split on $IFS joins the current word only if it doesn't
  // start with a separator, and ends it if it ends with one.
  void splice_split(strv s, bool lit) {
    str sep = ifs();
    if (s.empty()) return;
    bool lead  = sep.find(s.front()) != str::npos;
    bool trail = sep.find(s.back())  != str::npos;
    auto parts = ifs_split(s, sep);
    if (lead and not cur().text.empty()) new_field(false);
    splice(parts, lit, false);
    if (trail) pending_break = true;
  }

  // -------------------------------------------------------------------------
  // The scanner proper.
  // -------------------------------------------------------------------------
  Result<void> scan(strv w, bool dq = false) {
    for (s::size_t i = 0; i < w.size();) {
      char c = w[i];
      if (mode == Mode::Heredoc) {
        if (c == '\\' and i + 1 < w.size()
            and strv("$`\\\n").find(w[i + 1]) != strv::npos) {
          if (w[i + 1] != '\n') put(w[i + 1], true);
          i += 2;
          continue;
        }
        if (c == '$' or c == '`') {
          if (auto r = substitution(w, i, true); not r) return r;
          continue;
        }
        put(c, true);
        ++i;
        continue;
      }
      if (not dq) {
        if (c == '\\') {
          if (i + 1 < w.size()) {
            if (w[i + 1] != '\n') put(w[i + 1], true);
            i += 2;
          } else {
            put('\\', true);
            ++i;
          }
          continue;
        }
        if (c == '\'') {
          mark_quoted();
          auto e = w.find('\'', i + 1);
          if (e == strv::npos) e = w.size();
          put(w.substr(i + 1, e - i - 1), true);
          i = e + 1;
          continue;
        }
        if (c == '"') {
          mark_quoted();
          dq = true;
          ++i;
          continue;
        }
        if (c == '$' and i + 1 < w.size() and w[i + 1] == '\'') {
          mark_quoted();
          s::size_t e = i + 2;
          while (e < w.size() and w[e] != '\'') e += (w[e] == '\\') ? 2u : 1u;
          put(c_escapes(w.substr(i + 2, s::min(e, w.size()) - i - 2)), true);
          i = e + 1;
          continue;
        }
      } else {
        if (c == '"') { dq = false; ++i; continue; }
        if (c == '\\' and i + 1 < w.size()
            and (strv("$`\"\\\n").find(w[i + 1]) != strv::npos
                 or (in_brace and w[i + 1] == '}'))) {
          if (w[i + 1] != '\n') put(w[i + 1], true);
          i += 2;
          continue;
        }
      }
      if (c == '$' or c == '`') {
        if (auto r = substitution(w, i, dq); not r) return r;
        continue;
      }
      if (top and not dq and i + 1 < w.size() and w[i + 1] == '('
          and (c == '<' or c == '>' or (c == '=' and i == 0))
          and ex.process_substitution()) {
        // The command runs to the matching `)`, past quotes and nested
        // substitutions; without one, this is text.
        if (s::size_t j = find_close(w, i + 1); j != strv::npos) {
          put(strv(ex.exec().procsubst(c, w.substr(i + 2, j - i - 2))), true);
          i = j + 1;
          continue;
        }
      }
      put(c, dq);
      ++i;
    }
    return {};
  }

  // `$…` or `` `…` `` at w[i]; advances i past it.
  Result<void> substitution(strv w, s::size_t& i, bool dq) {
    if (w[i] == '`') {
      str src;
      s::size_t j = i + 1;
      for (; j < w.size() and w[j] != '`'; ++j) {
        if (w[j] == '\\' and j + 1 < w.size()
            and (w[j + 1] == '`' or w[j + 1] == '\\' or w[j + 1] == '$'
                 or (dq and w[j + 1] == '"'))) ++j;
        src += w[j];
      }
      i = j + 1;
      return command_output(src, dq);
    }
    s::size_t n = i + 1;
    if (n >= w.size()) { put('$', dq or mode != Mode::Word); ++i; return {}; }
    char c = w[n];
    if (c == '(') {
      s::size_t e = find_close(w, n);
      if (e == strv::npos) { put('$', true); ++i; return {}; }
      i = e + 1;
      // $((expr)) — but $( (subshell) ) and $((a) | b) are commands.
      if (n + 1 < e and w[n + 1] == '(' and w[e - 1] == ')'
          and find_close(w, n + 1) == e - 1)
        return arithmetic(w.substr(n + 2, e - n - 3), dq);
      return command_output(w.substr(n + 1, e - n - 1), dq);
    }
    if (c == '[') {
      s::size_t e = find_close(w, n);
      if (e == strv::npos) { put('$', true); ++i; return {}; }
      i = e + 1;
      return arithmetic(w.substr(n + 1, e - n - 1), dq);
    }
    if (c == '{') {
      s::size_t e = find_close(w, n, dq);
      if (e == strv::npos)
        return s::unexpected(subst_error("bad substitution"));
      i = e + 1;
      return param(w.substr(n + 1, e - n - 1), dq);
    }
    // Unbraced: $name, $name[sub], $1, $?, $#name, $+name, $=name, $~name,
    // $^name.
    s::size_t j = n;
    str expr;
    while (j < w.size() and (w[j] == '=' or w[j] == '~' or w[j] == '^')) ++j;
    if (j < w.size() and (w[j] == '#' or w[j] == '+') and j + 1 < w.size()
        and is_name_start(w[j + 1])) ++j;
    expr = str(w.substr(n, j - n));
    if (j < w.size() and is_name_start(w[j])) {
      s::size_t s = j;
      while (j < w.size() and is_name_char(w[j])) ++j;
      expr += w.substr(s, j - s);
      if (j < w.size() and w[j] == '[') {
        s::size_t e = find_close(w, j);
        if (e != strv::npos) {
          expr += w.substr(j, e - j + 1);
          j = e + 1;
        }
      }
    } else if (j < w.size() and (s::isdigit(static_cast<unsigned char>(w[j]))
                                 or is_special_param(w[j]))) {
      expr += w[j++];
    } else {
      put('$', dq or mode != Mode::Word);
      ++i;
      return {};
    }
    // csh-style modifiers on an unbraced parameter: $x:h, $0:t:r, $x:s/a/b/.
    for (;;) {
      s::size_t k = j;
      if (k + 1 >= w.size() or w[k] != ':') break;
      char m = w[k + 1];
      if (strv("htreulqQaAcP").find(m) != strv::npos) {
        j = k + 2;
      } else if (m == 's' or (m == 'g' and k + 2 < w.size() and w[k + 2] == 's')) {
        s::size_t d = k + (m == 'g' ? 3 : 2);
        if (d >= w.size()) break;
        char delim = w[d];
        s::size_t mid = w.find(delim, d + 1);
        if (mid == strv::npos) break;
        s::size_t end = w.find(delim, mid + 1);
        j = end == strv::npos ? w.size() : end + 1;
      } else {
        break;
      }
      expr += w.substr(k, j - k);
    }
    i = j;
    return param(expr, dq);
  }

  Result<void> command_output(strv src, bool dq) {
    auto out = run_command(src);
    if (not out) return s::unexpected(out.error());
    if (dq or mode != Mode::Word) {
      put(*out, true);
      return {};
    }
    splice_split(*out, not opts().get(OPT_GLOBSUBST));
    return {};
  }

  // Runs `src` in a subshell and returns its output without trailing
  // newlines; $? becomes its status.
  Result<str> run_command(strv src) {
    int pfd[2];
    if (::pipe(pfd) != 0)
      return s::unexpected(ShellError::from_errno(ErrorKind::ExecError, "pipe"));
    s::fflush(nullptr);
    pid_t pid = ::fork();
    if (pid < 0) {
      ::close(pfd[0]);
      ::close(pfd[1]);
      return s::unexpected(ShellError::from_errno(ErrorKind::ExecError, "fork"));
    }
    if (pid == 0) {
      ctx().signals.child_reset(ChildKind::Other);
      ctx().jobs.clear();
      ::close(pfd[0]);
      if (pfd[1] != 1) {
        ::dup2(pfd[1], 1);
        ::close(pfd[1]);
      }
      int rc = ex.exec().exec_string(src);
      s::fflush(nullptr);
      _exit(rc);
    }
    ::close(pfd[1]);
    str out;
    char buf[4096];
    for (;;) {
      ssize_t r = ::read(pfd[0], buf, sizeof buf);
      if (r < 0 and errno == EINTR) continue;
      if (r <= 0) break;
      out.append(buf, static_cast<s::size_t>(r));
    }
    ::close(pfd[0]);
    int status = 0;
    while (::waitpid(pid, &status, 0) < 0 and errno == EINTR) {}
    int rc = WIFEXITED(status) ? WEXITSTATUS(status)
           : WIFSIGNALED(status) ? 128 + WTERMSIG(status) : 1;
    ctx().last_exit = rc;
    ex.note_subst_status(rc);
    while (not out.empty() and out.back() == '\n') out.pop_back();
    return out;
  }

  Result<str> expand_scalar(strv text, Mode m = Mode::Heredoc) {
    Scan sub{ex, m};
    if (auto r = sub.scan(text); not r) return s::unexpected(r.error());
    str out;
    for (s::size_t k = 0; k < sub.fields.size(); ++k) {
      if (k) out += ' ';
      out += sub.fields[k].text;
    }
    return out;
  }

  // Quotes come off before the arithmetic, as in zsh: (( "5" > x )).
  Result<MathNum> math(strv expr) {
    auto text = expand_scalar(expr, Mode::Scalar);
    if (not text) return s::unexpected(text.error());
    MathOptions mo = math_options(opts());
    mo.flagged_element = [this](const str& name, const str& sub) -> opt<str> {
      auto r = expand_scalar("${" + name + "[" + sub + "]}", Mode::Scalar);
      if (not r) return s::nullopt;
      return *r;
    };
    return math_eval(*text, ctx().params, s::move(mo), &ctx().modules.math_funcs());
  }

  Result<s::int64_t> arith(strv expr) {
    auto v = math(expr);
    if (not v) return s::unexpected(v.error());
    return v->as_int();
  }

  Result<void> arithmetic(strv expr, bool dq) {
    auto v = math(expr);
    if (not v) return s::unexpected(v.error());
    put(format_math(*v), true);
    (void)dq;
    return {};
  }

  // -------------------------------------------------------------------------
  // Parameters.
  // -------------------------------------------------------------------------
  str option_letters() { return opts().letters(); }

  Value get_param(strv name, const ParamFlags& fl) {
    Value v;
    auto scalar = [&](str s) { v.set = true; v.make_scalar(s::move(s)); };
    auto& c = ctx();
    if (name == "?") { scalar(s::to_string(c.last_exit)); return v; }
    if (name == "$") { scalar(s::to_string(c.shell_pid)); return v; }
    if (name == "!") { scalar(s::to_string(c.last_bg_pid < 0 ? 0 : c.last_bg_pid)); return v; }
    if (name == "#" or name == "ARGC") { scalar(s::to_string(c.positional.size())); return v; }
    if (name == "-") { scalar(option_letters()); return v; }
    if (name == "0") { scalar(c.arg0); return v; }
    if (name == "@" or name == "*" or name == "argv") {
      v.set = true;
      v.make_array(c.positional);
      return v;
    }
    if (not name.empty() and s::isdigit(static_cast<unsigned char>(name[0]))) {
      s::size_t n = s::strtoul(str(name).c_str(), nullptr, 10);
      if (n >= 1 and n <= c.positional.size()) scalar(c.positional[n - 1]);
      return v;
    }
    if (name == "dirstack") {
      v.set = true;
      v.make_array(c.dirstack);
      return v;
    }

    auto p = c.params.lookup(name);
    if (not p or not p->get().is_set()) return v;
    const Param& param = p->get();
    v.set = true;
    if (param.is_hash()) {
      auto h = param.hash();
      vec<str> keys;
      for (auto& [k, _] : h) keys.push_back(k);
      s::sort(keys.begin(), keys.end());
      vec<str> out;
      for (auto& k : keys) {
        if (fl.keys) out.push_back(k);
        if (fl.values or not fl.keys) out.push_back(h[k]);
      }
      v.make_array(s::move(out));
    } else if (param.is_array()) {
      v.make_array(param.array());
    } else {
      v.make_scalar(param.scalar());
    }
    return v;
  }

  str type_of(strv name) {
    auto p = ctx().params.lookup(name);
    if (not p or not p->get().is_set()) {
      if (name == "@" or name == "*" or name == "argv") return "array-special";
      if (name == "?" or name == "$" or name == "#" or name == "!") return "integer-special";
      return {};
    }
    return param_type_text(p->get());
  }

  Result<ParamFlags> parse_flags(strv f) {
    ParamFlags fl;
    for (s::size_t i = 0; i < f.size(); ++i) {
      char c = f[i];
      auto delimited = [&]() -> Result<str> {
        auto a = read_delimited(f, i);
        if (not a) return s::unexpected(subst_error("error in flags"));
        return *a;
      };
      // (l:n::fill::once:): the optional arguments repeat the delimiter.
      auto padding = [&](ParamFlags::Pad& p) -> Result<void> {
        auto w = delimited();
        if (not w) return s::unexpected(w.error());
        p.on = true;
        p.width = *w;
        char open = f[i - w->size() - 1];
        for (str* arg : {&p.fill, &p.once}) {
          if (i + 1 >= f.size() or f[i + 1] != open) break;
          auto a = delimited();
          if (not a) return s::unexpected(a.error());
          *arg = *a;
        }
        return {};
      };
      switch (c) {
        case 'U': fl.upper = true;
        break; case 'L': fl.lower = true;
        break; case 'C': fl.capitalize = true;
        break; case '@': fl.at = true;
        break; case 'k': fl.keys = true;
        break; case 'v': fl.values = true;
        break; case 'o': fl.sort_asc = true;
        break; case 'O': fl.sort_desc = true;
        break; case 'i': fl.sort_nocase = true;
        break; case 'n': fl.sort_numeric = true;
        break; case '-': fl.sort_numeric = true;
        break; case 'u': fl.unique = true;
        break; case 'P': fl.indirect = true;
        break; case 't': fl.type = true;
        break; case 'z': fl.split_words = true;
        break; case 'Q': fl.unquote = true;
        break; case 'p': fl.escapes = true;
        break; case 'q':
          if (i + 1 < f.size() and f[i + 1] == '-') { fl.quote = -1; ++i; }
          else if (i + 1 < f.size() and f[i + 1] == '+') { fl.quote = -2; ++i; }
          else if (fl.quote >= 0) ++fl.quote;
        break; case '~': fl.glob_pat = true;
        break; case 'b': fl.quote_pat = true;
        break; case 'S': fl.m_substr = true;
        break; case 'B': fl.m_begin = true;
        break; case 'E': fl.m_end = true;
        break; case 'N': fl.m_len = true;
        break; case 'R': fl.m_rest = true;
        break; case 'A': ++fl.assign;
        break; case 'm': case 'X':   // widths are counted in bytes; errors reported anyway
        break; case 'I': {
          auto a = delimited();
          if (not a) return s::unexpected(a.error());
          fl.nth = *a;
        }
        break; case 'g': {
          auto a = delimited();
          if (not a) return s::unexpected(a.error());
          fl.escapes_g = *a;
        }
        break; case 'Z': {
          auto a = delimited();
          if (not a) return s::unexpected(a.error());
          fl.split_words = true;
          fl.z_opts = *a;
        }
        break; case '0': fl.split = true; fl.split_sep = str(1, '\0');
        break; case 'M': fl.match_mode = true;
        break; case 'V': fl.visible = true;
        break; case '#': fl.arith = true;
        break; case 'c': fl.count_c = true;
        break; case 'w': fl.count_w = true;
        break; case 'D': fl.dirabbr = true;
        break; case '%': fl.prompt = true;
        break; case 'e': fl.reeval = true;
        break; case 'l':
          if (auto r = padding(fl.pad_l); not r) return s::unexpected(r.error());
        break; case 'r':
          if (auto r = padding(fl.pad_r); not r) return s::unexpected(r.error());
        break; case 'f': fl.split = true; fl.split_sep = "\n";
        break; case 'F': fl.join = true; fl.join_sep = "\n";
        break; case 'j': {
          auto a = delimited();
          if (not a) return s::unexpected(a.error());
          fl.join = true;
          fl.join_sep = *a;
        }
        break; case 's': {
          auto a = delimited();
          if (not a) return s::unexpected(a.error());
          fl.split = true;
          fl.split_sep = *a;
        }
        break; default:
          return s::unexpected(subst_error(str("error in flags: ") + c));
      }
    }
    if (fl.escapes) {
      fl.join_sep  = c_escapes(fl.join_sep);
      fl.split_sep = c_escapes(fl.split_sep);
      for (auto* p : {&fl.pad_l, &fl.pad_r}) {
        p->fill = c_escapes(p->fill);
        p->once = c_escapes(p->once);
      }
    }
    return fl;
  }

  // Index of the end of the nested substitution that starts at t[i].
  static s::size_t nested_end(strv t, s::size_t i) {
    if (t[i] == '"') {
      // Substitutions inside it are passed over whole: "$(cmd "a")".
      for (++i; i < t.size() and t[i] != '"'; ++i) {
        if (t[i] == '\\') {
          ++i;
        } else if (t[i] == '$' and i + 1 < t.size() and (t[i + 1] == '(' or t[i + 1] == '{')) {
          auto e = find_close(t, i + 1);
          if (e != strv::npos) i = e;
        } else if (t[i] == '`') {
          auto e = t.find('`', i + 1);
          if (e != strv::npos) i = e;
        }
      }
      return s::min(i + 1, t.size());
    }
    // t[i] == '$'
    if (i + 1 < t.size() and (t[i + 1] == '{' or t[i + 1] == '(')) {
      auto e = find_close(t, i + 1);
      return e == strv::npos ? t.size() : e + 1;
    }
    s::size_t j = i + 1;
    if (j < t.size() and (is_special_param(t[j]) or s::isdigit(static_cast<unsigned char>(t[j]))))
      return j + 1;
    while (j < t.size() and is_name_char(t[j])) ++j;
    return j;
  }

  // Expands an operand of ${x:-word} etc.  Several words make an array.
  Result<Value> operand(strv word, bool dq) {
    Scan sub{ex, mode == Mode::Word ? Mode::Word : Mode::Scalar};
    sub.in_brace = true;
    if (auto r = sub.scan(word, dq); not r) return s::unexpected(r.error());
    Value v;
    v.set = true;
    vec<str> out;
    vec<Field> words;
    for (auto& f : sub.fields) {
      if (f.dropped or (f.text.empty() and not f.quoted)) continue;
      out.push_back(f.text);
      words.push_back(s::move(f));
    }
    if (out.size() <= 1) v.make_scalar(out.empty() ? "" : out[0]);
    else                 v.make_array(s::move(out));
    if (mode == Mode::Word and not dq) v.words = s::move(words);
    return v;
  }

  // The operand of ${x#pat} ${x/pat/…} ${x:#pat}: its pattern characters
  // work in double quotes too, as in zsh, so `dq` is passed as false.
  Result<CompiledPattern> operand_pattern(strv word, bool dq) {
    Scan sub{ex, Mode::Scalar};
    sub.in_brace = true;
    if (auto r = sub.scan(word, dq); not r) return s::unexpected(r.error());
    str pat;
    for (s::size_t k = 0; k < sub.fields.size(); ++k) {
      if (k) pat += ' ';
      pat += sub.fields[k].as_pattern();
    }
    return CompiledPattern::compile(pat, false, opts().extended_glob());
  }

  // `name[key]` where `name` has a get_elem handler and `key` is not @
  // or *: the element's value, with `i` moved past the subscript.  Null
  // when the handlers don't apply.
  opt<Result<Value>> element_view(strv name, strv t, s::size_t& i) {
    if (i >= t.size() or t[i] != '[') return s::nullopt;
    auto p = ctx().params.lookup_raw(name);
    if (not p or not p->get().handlers or not p->get().handlers->get_elem)
      return s::nullopt;
    s::size_t e = find_close(t, i);
    if (e == strv::npos) return s::nullopt;
    strv sub = t.substr(i + 1, e - i - 1);
    if (sub == "@" or sub == "*") return s::nullopt;
    auto key = expand_scalar(sub, Mode::Scalar);
    if (not key) return Result<Value>(s::unexpected(key.error()));
    auto got = p->get().handlers->get_elem(*key);
    Value v;
    v.make_scalar(got.value_or(""));
    v.set = got.has_value();
    i = e + 1;
    return Result<Value>(s::move(v));
  }

  // `keys`: (k), so an association's subscript gives the key; `values`
  // (v) with it, the key and the value.
  Result<void> apply_subscript(Value& v, strv sub, strv name,
                               bool& all, bool keys = false, bool values = false) {
    if (sub == "@" or sub == "*") {
      all = sub == "@";
      if (not v.array and v.set) v.make_array({v.scalar});
      return {};
    }
    // A top-level comma makes a range, whose bounds may be searches.
    auto top_comma = [&] {
      int depth = 0;
      for (s::size_t k = 0; k < sub.size(); ++k) {
        if (sub[k] == '\\') { ++k; continue; }
        if (sub[k] == '(' or sub[k] == '[') ++depth;
        else if ((sub[k] == ')' or sub[k] == ']') and depth > 0) --depth;
        else if (sub[k] == ',' and depth == 0) return true;
      }
      return false;
    };
    if (not sub.empty() and sub[0] == '(' and not top_comma()) {
      auto e = find_close(sub, 0);
      if (e == strv::npos) return s::unexpected(subst_error("bad subscript"));
      return subscript_flags(v, sub.substr(1, e - 1), sub.substr(e + 1), name, keys, values);
    }
    // Associative arrays are indexed by key.
    if (auto p = ctx().params.lookup(name); p and p->get().is_hash()) {
      auto key = expand_scalar(sub, Mode::Scalar);
      if (not key) return s::unexpected(key.error());
      auto h = p->get().hash();
      auto it = h.find(*key);
      if (it == h.end()) { v.set = false; v.make_scalar(""); }
      else               v.make_scalar(keys ? it->first : it->second);
      return {};
    }
    s::size_t comma = strv::npos;
    int depth = 0;
    for (s::size_t k = 0; k < sub.size(); ++k) {
      if (sub[k] == '(' or sub[k] == '[') ++depth;
      else if (sub[k] == ')' or sub[k] == ']') --depth;
      else if (sub[k] == ',' and depth == 0) { comma = k; break; }
    }
    // A bound may be a search: a[1,(i)--] ends at the first `--`.
    auto bound = [&](strv text) -> Result<s::int64_t> {
      if (text.empty() or text[0] != '(') return arith(text);
      auto e = find_close(text, 0);
      if (e == strv::npos) return s::unexpected(subst_error("bad subscript"));
      str flags(text.substr(1, e - 1));
      for (char& c : flags) c = c == 'r' ? 'i' : c == 'R' ? 'I' : c;
      Value copy = v;
      if (auto r = subscript_flags(copy, flags, text.substr(e + 1), name); not r)
        return s::unexpected(r.error());
      return static_cast<s::int64_t>(s::strtoll(copy.scalar.c_str(), nullptr, 10));
    };
    auto lo = bound(sub.substr(0, comma));
    if (not lo) return s::unexpected(lo.error());
    opt<s::int64_t> hi;
    if (comma != strv::npos) {
      auto h = bound(sub.substr(comma + 1));
      if (not h) return s::unexpected(h.error());
      hi = *h;
    }
    // Normalise an external 1-based (zsh) or 0-based (KSH_ARRAYS)
    // subscript into a 1-based internal index.  Below any first slot
    // comes back as 0 so the callers can still clamp uniformly.
    bool ksh  = opts().get(OPT_KSH_ARRAYS);
    bool kz   = opts().get(OPT_KSH_ZERO_SUBSCRIPT);
    auto norm = [&](s::int64_t i, s::int64_t n) -> s::int64_t {
      if (ksh) {
        if (i < 0) i += n;
        return i + 1;   // convert ksh 0-based to internal 1-based
      }
      if (i == 0 and kz) return 1;
      return i < 0 ? n + i + 1 : i;
    };
    if (v.array) {
      s::int64_t n = static_cast<s::int64_t>(v.arr.size());
      s::int64_t a = norm(*lo, n);
      if (not hi) {
        if (a >= 1 and a <= n) {
          v.make_scalar(v.arr[static_cast<s::size_t>(a - 1)]);
        } else {
          v.make_scalar("");
          v.set = false;
        }
        return {};
      }
      s::int64_t b = s::min(norm(*hi, n), n);
      a = s::max<s::int64_t>(a, 1);
      vec<str> out;
      for (s::int64_t k = a; k <= b; ++k) out.push_back(v.arr[static_cast<s::size_t>(k - 1)]);
      v.make_array(s::move(out));
      return {};
    }
    s::int64_t n = static_cast<s::int64_t>(v.scalar.size());
    s::int64_t a = norm(*lo, n);
    s::int64_t b = hi ? norm(*hi, n) : a;
    a = s::max<s::int64_t>(a, 1);
    b = s::min(b, n);
    v.make_scalar(a > b ? str{}
                        : v.scalar.substr(static_cast<s::size_t>(a - 1),
                                       static_cast<s::size_t>(b - a + 1)));
    return {};
  }

  // ${a[(flags)expr]}: (r)/(R) the first/last element matching the
  // pattern `expr`, (i)/(I) its index; (e) compares exactly; (n:N:) takes
  // the Nth match, (b:N:) starts the search at N.  On an association (r)
  // and (R) search the values, (i) and (I) the keys, and (k)/(K) use the
  // keys as patterns against `expr`.  (w) and (s:sep:) index the words of
  // a scalar.
  // `keys`: the (k) flag, under which an association's (r) and (R) give
  // the keys of the values they find.
  Result<void> subscript_flags(Value& v, strv flags, strv expr, strv name, bool want_keys = false,
                               bool want_values = false) {
    char kind = 0;
    bool exact = false, words = false;
    str sep, nth_expr, begin_expr;
    for (s::size_t k = 0; k < flags.size(); ++k) {
      char c = flags[k];
      if (strv("rRiIkK").find(c) != strv::npos) { kind = c; continue; }
      if (c == 'e') { exact = true; continue; }
      if (c == 'w') { words = true; continue; }
      if (c == 'n' or c == 'b' or c == 's') {
        auto a = read_delimited(flags, k);
        if (not a) return s::unexpected(subst_error("bad subscript flags"));
        (c == 'n' ? nth_expr : c == 'b' ? begin_expr : sep) = *a;
        if (c == 's') words = true;
        continue;
      }
      return s::unexpected(subst_error(str("unknown subscript flag: ") + c));
    }
    if (words) {
      // The words of the scalar, then an ordinary numeric subscript.
      str s = v.array ? str{} : v.scalar;
      if (v.array)
        for (s::size_t k = 0; k < v.arr.size(); ++k) s += (k ? " " : "") + v.arr[k];
      vec<str> ws;
      if (sep.empty()) {
        ws = ifs_split(s, ifs());
      } else {
        for (s::size_t b = 0;;) {
          auto e = s.find(sep, b);
          ws.push_back(s.substr(b, e == str::npos ? str::npos : e - b));
          if (e == str::npos) break;
          b = e + sep.size();
        }
      }
      v.make_array(s::move(ws));
      if (not kind) {
        bool all = false;
        return apply_subscript(v, expr, "", all);
      }
    }
    if (not kind) return s::unexpected(subst_error("bad subscript flags"));

    s::int64_t nth = 1;
    if (not nth_expr.empty()) {
      auto n = arith(nth_expr);
      if (not n) return s::unexpected(n.error());
      nth = *n;
    }
    opt<s::int64_t> begin;
    if (not begin_expr.empty()) {
      auto b = arith(begin_expr);
      if (not b) return s::unexpected(b.error());
      begin = *b;
    }
    // The subscript text: parameter values in it are pattern characters
    // unless quoted, unlike in ${x#$p}.
    Scan sub{ex, Mode::Scalar};
    sub.glob_values = true;
    if (auto r = sub.scan(expr); not r) return s::unexpected(r.error());
    str text, pat_text;
    for (s::size_t k = 0; k < sub.fields.size(); ++k) {
      if (k) { text += ' '; pat_text += ' '; }
      text += sub.fields[k].text;
      pat_text += sub.fields[k].as_pattern();
    }
    bool extended = opts().extended_glob();
    auto pat = CompiledPattern::compile(pat_text, false, extended);
    auto test = [&](const str& s) { return exact ? s == text : pat.matches(s); };

    // Associations.
    if (auto p = ctx().params.lookup(name); not name.empty() and p and p->get().is_hash()) {
      auto h = p->get().hash();
      vec<str> keys;
      for (auto& [key, _] : h) keys.push_back(key);
      s::sort(keys.begin(), keys.end());
      vec<str> out;
      for (auto& key : keys) {
        bool hit = kind == 'k' or kind == 'K'
            ? (exact ? key == text
                     : CompiledPattern::compile(key, false, extended).matches(text))
            : test(kind == 'i' or kind == 'I' ? key : h[key]);
        if (not hit) continue;
        if (kind == 'i' or kind == 'I') {
          out.push_back(key);
        } else {
          if (want_keys) out.push_back(key);
          if (want_values or not want_keys) out.push_back(h[key]);
        }
      }
      if (kind == 'R' or kind == 'I' or kind == 'K') {
        v.set = true;
        v.make_array(s::move(out));
      } else {
        v.set = not out.empty();
        v.make_scalar(out.empty() ? str{} : out.front());
      }
      return {};
    }

    // Arrays, and scalars by character position.
    bool ksh = opts().get(OPT_KSH_ARRAYS);
    s::int64_t n = static_cast<s::int64_t>(v.array ? v.arr.size() : v.scalar.size());
    auto hit_at = [&](s::int64_t k) {   // k is 1-based
      if (v.array) {
        const str& e = v.arr[static_cast<s::size_t>(k - 1)];
        return kind == 'k' or kind == 'K'
            ? CompiledPattern::compile(e, false, extended).matches(text)
            : test(e);
      }
      // As zsh's getarg: each tail is a string of its own, so (#s) is
      // where it starts.
      strv rest = strv(v.scalar).substr(static_cast<s::size_t>(k - 1));
      return exact ? rest.starts_with(text) : pat.match_prefix(rest, false) != strv::npos;
    };
    bool backward = kind == 'R' or kind == 'I' or kind == 'K';
    s::int64_t from = backward ? n : 1;
    if (begin) {
      from = *begin + (ksh ? 1 : 0);
      if (from < 0) from += n + 1;
      from = s::clamp<s::int64_t>(from, backward ? 0 : 1, backward ? n : n + 1);
    }
    s::int64_t found = 0;
    s::int64_t left = s::max<s::int64_t>(nth, 1);
    for (s::int64_t k = from; backward ? k >= 1 : k <= n; k += backward ? -1 : 1)
      if (hit_at(k) and --left == 0) { found = k; break; }

    if (kind == 'i' or kind == 'I') {
      s::int64_t idx = found ? found : (kind == 'i' ? n + 1 : 0);
      if (ksh and idx >= 1) --idx;
      v.set = true;
      v.make_scalar(s::to_string(idx));
      return {};
    }
    v.set = found != 0;
    if (not found)    v.make_scalar("");
    else if (v.array) v.make_scalar(v.arr[static_cast<s::size_t>(found - 1)]);
    else              v.make_scalar(v.scalar.substr(static_cast<s::size_t>(found - 1), 1));
    return {};
  }

  static str modify(str s, char m) {
    switch (m) {
      case 'h': {
        auto slash = s.rfind('/');
        if (slash == str::npos) return ".";
        return slash == 0 ? "/" : s.substr(0, slash);
      }
      case 't': {
        auto slash = s.rfind('/');
        return slash == str::npos ? s : s.substr(slash + 1);
      }
      case 'r': {
        auto dot = s.rfind('.');
        auto slash = s.rfind('/');
        if (dot == str::npos or (slash != str::npos and dot < slash)) return s;
        return s.substr(0, dot);
      }
      case 'e': {
        auto dot = s.rfind('.');
        auto slash = s.rfind('/');
        if (dot == str::npos or (slash != str::npos and dot < slash)) return {};
        return s.substr(dot + 1);
      }
      case 'u':
        for (auto& ch : s) ch = static_cast<char>(s::toupper(static_cast<unsigned char>(ch)));
        return s;
      case 'l':
        for (auto& ch : s) ch = static_cast<char>(s::tolower(static_cast<unsigned char>(ch)));
        return s;
      case 'q': return quote_backslash(s);
      case 'Q': return remove_quotes(s);
      case 'a': case 'A': case 'P': {
        if (s.empty() or s[0] != '/') {
          char buf[PATH_MAX];
          if (::getcwd(buf, sizeof buf)) s = str(buf) + "/" + s;
        }
        if (m == 'A' or m == 'P') {
          char buf[PATH_MAX];
          if (::realpath(s.c_str(), buf)) return buf;
        }
        // Lexically remove `.` and `..` components.
        vec<str> parts;
        s::size_t k = 0;
        while (k <= s.size()) {
          auto e = s.find('/', k);
          if (e == str::npos) e = s.size();
          str part = s.substr(k, e - k);
          if (part == "..") { if (not parts.empty()) parts.pop_back(); }
          else if (not part.empty() and part != ".") parts.push_back(part);
          k = e + 1;
        }
        str out;
        for (auto& p : parts) out += "/" + p;
        return out.empty() ? "/" : out;
      }
      default: return s;
    }
  }

  // :h :t :r :e :u :l :q :Q :a :A :c :P, chained (`:t:r`), :s/pat/repl/
  // (:gs for every match) and :& (the last :s again), applied to `v`;
  // `mods` starts at the first colon.
  Result<void> apply_modifiers(Value& v, strv mods) {
    opt<str> last_pat, last_repl;
    while (mods.size() >= 2 and mods[0] == ':') {
      char m = mods[1];
      bool global = false;
      if (m == 'g' and mods.size() >= 3 and (mods[2] == 's' or mods[2] == '&')) {
        global = true;
        mods.remove_prefix(1);
        m = mods[1];
      }
      if (m == 's' or m == '&') {
        str pat_text, repl_text;
        if (m == 's') {
          if (mods.size() < 4) return s::unexpected(subst_error("bad substitution"));
          char delim = mods[2];
          s::size_t mid = mods.find(delim, 3);
          if (mid == strv::npos)
            return s::unexpected(subst_error("bad substitution"));
          s::size_t end = mods.find(delim, mid + 1);
          if (end == strv::npos) end = mods.size();
          pat_text  = str(mods.substr(3, mid - 3));
          repl_text = str(mods.substr(mid + 1, end - mid - 1));
          last_pat  = pat_text;
          last_repl = repl_text;
          mods.remove_prefix(end < mods.size() ? end + 1 : mods.size());
        } else {
          // With no :s before it there is nothing to repeat, as in zsh.
          if (not last_pat) {
            mods.remove_prefix(2);
            continue;
          }
          pat_text  = *last_pat;
          repl_text = *last_repl;
          mods.remove_prefix(2);
        }
        // The left side is plain text unless HIST_SUBST_PATTERN is set.
        if (not opts().get(OPT_HISTSUBSTPATTERN)) {
          str lit;
          for (char ch : pat_text) {
            if (PATTERN_CHARS.find(ch) != strv::npos) lit += '\\';
            lit += ch;
          }
          pat_text = s::move(lit);
        }
        auto cp = CompiledPattern::compile(pat_text, false,
                                           opts().get(OPT_EXTENDEDGLOB));
        bool g = global;
        each(v, [&](const str& s) {
          return replace(s, cp, repl_text, g, /*anchor=*/0);
        });
        continue;
      }
      if (strv("htreulqQaAcP").find(m) == strv::npos)
        return s::unexpected(
            subst_error("unrecognized modifier `" + str(1, m) + "'"));
      if (m == 'c') {
        // :c — the command's path, as `=cmd` would give.
        each(v, [&](const str& s) { return ex.exec().find_command(s).value_or(s); });
      } else {
        each(v, [&](const str& s) { return modify(s, m); });
      }
      mods.remove_prefix(2);
    }
    if (not mods.empty()) return s::unexpected(subst_error("bad substitution"));
    return {};
  }

  template <typename F>
  static void each(Value& v, F fn) {
    if (v.array) for (auto& e : v.arr) e = fn(e);
    else         v.scalar = fn(v.scalar);
  }

  // ${name/pat/repl}, ${name//pat/repl}, ${name:/pat/repl}.
  // ${(A)name=…}: `name` becomes an array, or (hash) an association of
  // the words taken as key/value pairs.
  Result<void> assign_array(const str& name, vec<str> words, bool hash) {
    auto& params = ctx().params;
    if (not hash) {
      auto p = params.lookup(name);
      if (p and p->get().is_hash()) (void)params.unset(name);
      return params.set_array(name, s::move(words));
    }
    if (words.size() % 2)
      return s::unexpected(subst_error("bad set of key/value pairs for associative array"));
    umap<str, str> h;
    for (s::size_t k = 0; k + 1 < words.size(); k += 2) h[words[k]] = words[k + 1];
    auto p = params.lookup(name);
    if (p and p->get().is_readonly())
      return s::unexpected(subst_error("read-only variable: " + name));
    if (not p) {
      (void)params.set_array(name, {});
      p = params.lookup(name);
    }
    p->get().set(s::move(h));
    return {};
  }

  // (I:expr:): which match, 1 without.
  Result<s::int64_t> nth_match(const ParamFlags& fl) {
    if (fl.nth.empty()) return 1;
    return arith(fl.nth);
  }

  // zsh's getmatch for ${x#pat} ${x##pat} ${x%pat} ${x%%pat}: the match,
  // anchored at the start (or, for `end`, the end) or with (S) anywhere,
  // the first (last for `end`) or (I:n:) nth; then what the flags ask
  // for: (M) the match, (R) the rest (the default), (B) (E) (N) its
  // start, end and length.  No match is an empty one at the start.
  static str get_match(const str& s, const CompiledPattern& pat, bool end, bool longest,
                       const ParamFlags& fl, s::int64_t nth) {
    s::size_t n = s.size(), mb = 0, me = 0;
    if (not fl.m_substr) {
      if (not end) {
        s::size_t len = pat.match_prefix(s, longest);
        if (len != str::npos) me = len;
      } else {
        for (s::size_t k = 0; k <= n; ++k) {
          s::size_t b = longest ? k : n - k;
          if (pat.matches(s, b, s.size())) {
            mb = b;
            me = n;
            break;
          }
        }
      }
    } else {
      for (s::size_t k = 0; k <= n; ++k) {
        s::size_t b = end ? n - k : k;
        s::size_t len = pat.match_prefix(s, b, longest);
        if (len == str::npos or --nth > 0) continue;
        mb = b;
        me = b + len;
        break;
      }
    }
    str out;
    bool rest = fl.m_rest or not (fl.match_mode or fl.m_begin or fl.m_end or fl.m_len);
    if (fl.match_mode) out += s.substr(mb, me - mb);
    if (rest) out += s.substr(0, mb) + s.substr(me);
    auto num = [&](s::size_t v) {
      if (not out.empty()) out += ' ';
      out += s::to_string(v);
    };
    if (fl.m_begin) num(mb + 1);
    if (fl.m_end) num(me + 1);
    if (fl.m_len) num(me - mb);
    return out;
  }

  // The replacement for a match of s[b, b + len), whose backreferences
  // are in the PatternMatch.
  using ReplaceFn = func<Result<str>(s::size_t, s::size_t, const PatternMatch&)>;

  // zsh's igetmatch for ${x/pat/r}: `anchor` is 'w' (the whole value),
  // '#' (the start), '%' (the end) or 0 (anywhere); `shortest` is (S),
  // `nth` (I:n:) skips the matches before it.
  static Result<str> substitute(const str& s, const CompiledPattern& pat, bool global,
                                char anchor, bool shortest, s::int64_t nth,
                                const ReplaceFn& repl) {
    s::size_t n = s.size();
    PatternMatch pm;
    if (anchor == 'w') {
      if (not pat.matches(s, &pm)) return s;
      return repl(0, n, pm);
    }
    if (anchor == '#') {
      s::size_t len = pat.match_prefix(s, not shortest, &pm);
      if (len == str::npos) return s;
      auto r = repl(0, len, pm);
      if (not r) return r;
      return *r + s.substr(len);
    }
    if (anchor == '%') {
      for (s::size_t k = 0; k <= n; ++k) {
        s::size_t b = shortest ? n - k : k;
        if (pat.matches(s, b, n, &pm)) {
          auto r = repl(b, n - b, pm);
          if (not r) return r;
          return s.substr(0, b) + *r;
        }
      }
      return s;
    }
    str out;
    // `last_hit`: the step that got to `b` was a match (one, or an empty
    // one and the character after it).  As in zsh, the empty match at the
    // end is tried unless that step reached it, and, without `global`,
    // only when nothing matched: ${x//(b|(#e))/Z} on `abcd` is `aZcdZ`,
    // ${x//a#/q} on `aaab` is `qqb`.
    bool done = false, any = false, last_hit = false;
    for (s::size_t b = 0; b < n;) {
      s::size_t len = done ? str::npos : pat.match_prefix(s, b, not shortest, &pm);
      if (len == str::npos) {
        out += s[b];
        ++b;
        last_hit = false;
        continue;
      }
      any = last_hit = true;
      if (nth > 1) {
        --nth;
        out += s.substr(b, s::max<s::size_t>(len, 1));
        b += s::max<s::size_t>(len, 1);
        continue;
      }
      auto r = repl(b, len, pm);
      if (not r) return r;
      out += *r;
      if (not global) done = true;
      if (len == 0) {
        out += s[b];
        ++b;
      } else {
        b += len;
      }
    }
    if (not last_hit and (global or not any) and nth <= 1
        and pat.match_prefix(s, n, false, &pm) == 0) {
      auto r = repl(n, 0, pm);
      if (not r) return r;
      out += *r;
    }
    return out;
  }

  // substitute() for a pattern with (#b) or (#m): $match … are set to
  // each match before `repl_text` is expanded for it.
  Result<str> replace_each(const str& s, const CompiledPattern& pat, strv repl_text,
                           bool global, char anchor, bool shortest, s::int64_t nth) {
    bool ksh = opts().get(OPT_KSH_ARRAYS);
    return substitute(s, pat, global, anchor, shortest, nth,
                      [&](s::size_t b, s::size_t len, const PatternMatch& pm) -> Result<str> {
                        set_match_params(ctx().params, pat, s, b, len, pm, ksh);
                        return expand_scalar(repl_text, Mode::Scalar);
                      });
  }

  // substitute() with a fixed replacement.
  static str replace(const str& s, const CompiledPattern& pat,
                     const str& repl, bool global, char anchor,
                     bool shortest = false, s::int64_t nth = 1) {
    auto r = substitute(s, pat, global, anchor, shortest, nth,
                        [&](s::size_t, s::size_t, const PatternMatch&) -> Result<str> {
                          return repl;
                        });
    return r ? *r : s;
  }

  // ${…}: `t` is the text between the braces.
  Result<void> param(strv t, bool dq) {
    auto r = param_value(t, dq);
    if (not r) return s::unexpected(r.error());
    auto& [v, keep_separate, split, glob, rc_expand] = *r;
    if (mode != Mode::Word) {
      // Only a pattern (`case`, `[[ = ]]`) looks at what is literal: there
      // ${~x} and GLOB_SUBST make the value's characters active.
      bool literal = dq or not (glob or glob_values);
      if (v.array) {
        str joined;
        for (s::size_t k = 0; k < v.arr.size(); ++k) {
          if (k) joined += ' ';
          joined += v.arr[k];
        }
        put(joined, literal);
      } else {
        put(v.scalar, literal);
      }
      return {};
    }
    if (dq) {
      if (v.array and keep_separate and rc_expand) {
        rc_splice(v.arr, true);
        mark_quoted();
      } else if (v.array and keep_separate) {
        if (v.arr.empty()) cur().empty_at = true;
        splice(v.arr, true, true);
      } else if (v.array) {
        str sep = ifs_first(), joined;
        for (s::size_t k = 0; k < v.arr.size(); ++k) {
          if (k) joined += sep;
          joined += v.arr[k];
        }
        put(joined, true);
      } else {
        put(v.scalar, true);
      }
      return {};
    }
    bool lit = not glob;
    // ${x:-word}: the operand's own words, so `*` in `word` still globs.
    rc_expand = rc_expand and v.array;
    if (not rc_expand and v.words_current()) {
      for (s::size_t k = 0; k < v.words.size(); ++k) {
        if (k) new_field(v.words[k].quoted);
        if (v.words[k].quoted) mark_quoted();
        if (glob) put(strv(v.words[k].text), false);
        else for (s::size_t c = 0; c < v.words[k].text.size(); ++c)
          put(v.words[k].text[c], static_cast<bool>(v.words[k].lit[c]));
      }
      return {};
    }
    if (v.array) {
      vec<str> parts;
      for (auto& e : v.list()) if (rc_expand or v.quoted_elems or not e.empty()) parts.push_back(e);
      if (split) {
        str sep = ifs();
        vec<str> words;
        for (auto& e : parts)
          for (auto& w : ifs_split(e, sep)) words.push_back(w);
        parts = s::move(words);
      }
      if (rc_expand) {
        rc_splice(parts, lit);
      } else if (v.quoted_elems) {
        mark_quoted();
        splice(parts, lit, true);
      } else {
        splice(parts, lit, false);
      }
      return {};
    }
    if (split) splice_split(v.scalar, lit);
    else       put(v.scalar, lit);
    return {};
  }

  struct ParamResult {
    Value v;
    bool  keep_separate = false;   // "$@", "${a[@]}", "${(@)a}"
    bool  split         = false;
    bool  glob          = false;
    bool  rc_expand     = false;   // ${^x}, RC_EXPAND_PARAM
  };

  Result<ParamResult> param_value(strv t, bool dq) {
    ParamResult res;
    s::size_t i = 0;
    ParamFlags fl;
    if (i < t.size() and t[i] == '(') {
      auto e = find_close(t, i);
      if (e == strv::npos) return s::unexpected(subst_error("bad substitution"));
      auto f = parse_flags(t.substr(i + 1, e - i - 1));
      if (not f) return s::unexpected(f.error());
      fl = *f;
      i = e + 1;
    }
    bool length = false, plus = false;
    bool eq_split = opts().get(OPT_SHWORDSPLIT);
    bool glob = opts().get(OPT_GLOBSUBST);
    bool rc_expand = opts().get(OPT_RCEXPANDPARAM);
    // Prefix operators: ${=x} ${==x} ${~x} ${~~x} ${^x} ${^^x} ${+x} ${#x}.
    while (i + 1 < t.size()) {
      char c = t[i], n = t[i + 1];
      // ${#:-word}: the length of the word (an empty subject).
      bool subject = is_name_start(n) or is_special_param(n) or n == '$' or n == '"'
                  or s::isdigit(static_cast<unsigned char>(n))
                  or (c == '#' and n == ':' and i + 2 < t.size() and t[i + 2] == '-');
      if (c == '=' or c == '~') {
        bool twice = n == c;
        (c == '=' ? eq_split : glob) = not twice;
        i += twice ? 2 : 1;
      } else if (c == '^') {
        rc_expand = n != '^';
        i += n == '^' ? 2 : 1;
      } else if (c == '+' and subject) {
        plus = true;
        ++i;
      } else if (c == '#' and subject) {
        length = true;
        ++i;
      } else {
        break;
      }
    }

    // The subject: a name, a special parameter, or a nested substitution.
    str name;
    Value v;
    bool nested = false;
    if (i + 1 < t.size() and (t[i] == '$' or t[i] == '"')) {
      nested = true;
      s::size_t e = nested_end(t, i);
      strv inner = t.substr(i, e - i);
      if (inner.size() > 2 and inner[0] == '$' and inner[1] == '{') {
        auto r = param_value(inner.substr(2, inner.size() - 3), dq);
        if (not r) return s::unexpected(r.error());
        v = r->v;
        res.keep_separate = r->keep_separate;
        // Unquoted, the inner result's empty elements go, as when it is
        // a word list: ${#${a}}.
        if (not dq and v.array and not v.quoted_elems)
          s::erase_if(v.arr, [](const str& e) { return e.empty(); });
      } else if (inner.size() > 1 and inner[0] == '$'
                 and inner[1] != '(' and inner[1] != '[') {
        auto r = param_value(inner.substr(1), dq);
        if (not r) return s::unexpected(r.error());
        v = r->v;
        res.keep_separate = r->keep_separate;
      } else if (inner[0] == '"') {
        // ${~"${(@s.:.)x}"}: the words of the quoted string, kept apart
        // where (@) keeps them.
        Scan sub{ex, Mode::Word};
        if (auto r = sub.scan(inner); not r) return s::unexpected(r.error());
        vec<str> words;
        for (auto& f : sub.fields)
          if (not f.dropped and (f.quoted or not f.text.empty())) words.push_back(f.text);
        v.set = true;
        v.quoted_elems = true;
        if (words.size() == 1) v.make_scalar(words[0]);
        else                   v.make_array(s::move(words));
      } else {
        auto s = expand_scalar(inner, Mode::Scalar);
        if (not s) return s::unexpected(s.error());
        v.set = true;
        // ${(o)$(cmd)}: unquoted, the output is split into words.
        if (not dq and inner.starts_with("$(")) v.make_array(ifs_split(*s, ifs()));
        else                                    v.make_scalar(*s);
      }
      i = e;
      // ${(P)${…}}: the value names the parameter; ${(tP)…} its type.
      if (fl.indirect) {
        name = v.array ? (v.arr.empty() ? str{} : v.arr[0]) : v.scalar;
        if (fl.type) {
          v = Value{};
          v.set = true;
          v.make_scalar(type_of(name));
        } else {
          v = get_param(name, fl);
        }
      }
    } else {
      s::size_t s = i;
      if (i < t.size() and is_name_start(t[i])) {
        while (i < t.size() and is_name_char(t[i])) ++i;
      } else if (i < t.size() and s::isdigit(static_cast<unsigned char>(t[i]))) {
        while (i < t.size() and s::isdigit(static_cast<unsigned char>(t[i]))) ++i;
      } else if (i < t.size() and is_special_param(t[i])) {
        ++i;
      } else if (i < t.size() and t[i] == ':') {
        // ${:-word}, ${(z):-word}: no parameter, so the word.
        nested = true;
      } else {
        return s::unexpected(subst_error("bad substitution"));
      }
      name = str(t.substr(s, i - s));
      if (fl.indirect) {
        // ${(P)a[2]}: the subscript picks the name, which is then looked up.
        Value ref = get_param(name, fl);
        if (i < t.size() and t[i] == '[') {
          s::size_t e = find_close(t, i);
          if (e == strv::npos) return s::unexpected(subst_error("bad substitution"));
          bool all = false;
          if (auto r = apply_subscript(ref, t.substr(i + 1, e - i - 1), name, all, fl.keys); not r)
            return s::unexpected(r.error());
          i = e + 1;
        }
        name = ref.array ? (ref.arr.empty() ? "" : ref.arr[0]) : ref.scalar;
      }
      if (fl.type) {
        v.set = true;
        v.make_scalar(type_of(name));
      } else if (auto ev = element_view(name, t, i)) {
        // A special association with per-key handlers ($mapfile, …):
        // `${h[key]}` reads that key only, not the whole table.
        if (not *ev) return s::unexpected(ev->error());
        v = s::move(**ev);
      } else {
        v = get_param(name, fl);
      }
      if (name == "@") res.keep_separate = true;
    }

    // Subscripts, possibly several: ${a[2][1]}.
    s::size_t subscripts = i;
    while (i < t.size() and t[i] == '[') {
      s::size_t e = find_close(t, i);
      if (e == strv::npos) return s::unexpected(subst_error("bad substitution"));
      bool all = false;
      if (auto r = apply_subscript(v, t.substr(i + 1, e - i - 1), nested ? "" : name, all,
                                   fl.keys, fl.values);
          not r)
        return s::unexpected(r.error());
      if (all) res.keep_separate = true;
      i = e + 1;
    }
    if (fl.at) res.keep_separate = true;

    if (plus) {
      Value out;
      out.set = true;
      out.make_scalar(v.set ? "1" : "0");
      res.v = s::move(out);
      return res;
    }

    strv subscripted = t.substr(subscripts, i - subscripts);
    strv op = t.substr(i);
    bool special = not name.empty() and (is_special_param(name[0])
                    or s::isdigit(static_cast<unsigned char>(name[0])));

    // Double-quoted joining happens before the operators.
    auto join_dq = [&] {
      if (dq and v.array and not res.keep_separate and not length) {
        str sep = fl.join ? fl.join_sep : ifs_first(), joined;
        for (s::size_t k = 0; k < v.arr.size(); ++k) {
          if (k) joined += sep;
          joined += v.arr[k];
        }
        v.make_scalar(joined);
      }
    };

    if (not op.empty()) {
      bool colon = op[0] == ':';
      strv rest = colon ? op.substr(1) : op;
      char o = rest.empty() ? 0 : rest[0];
      bool null = not v.set or (v.array ? v.arr.empty() : v.scalar.empty());
      bool empty_test = colon ? null : not v.set;
      // ${x::=word} assigns unconditionally.
      bool force_assign = colon and rest.starts_with(":=");
      if (force_assign) {
        rest.remove_prefix(1);
        o = '=';
      }
      if (o == '-' or o == '=' or o == '?' or o == '+') {
        strv word = rest.substr(1);
        switch (o) {
          case '-':
            if (empty_test) {
              auto w = operand(word, dq);
              if (not w) return s::unexpected(w.error());
              v = *w;
            }
          break; case '+':
            if (empty_test) { v.set = true; v.make_scalar(""); }
            else {
              auto w = operand(word, dq);
              if (not w) return s::unexpected(w.error());
              v = *w;
            }
          break; case '=':
            if (empty_test or force_assign) {
              if (nested or special)
                return s::unexpected(subst_error("can't set " + str(name)));
              auto w = operand(word, dq);
              if (not w) return s::unexpected(w.error());
              Result<void> r;
              if (fl.assign) {
                // (A) assigns an array, (AA) an association.
                vec<str> words = w->array ? w->arr
                               : w->scalar.empty() ? vec<str>{} : vec<str>{w->scalar};
                r = assign_array(name, s::move(words), fl.assign == 2);
                if (r) {
                  w->set = true;
                  w->make_array(get_param(name, ParamFlags{}).list());
                }
              } else if (not subscripted.empty()) {
                // ${h[key]:=word}: that element.
                auto key = expand_scalar(subscripted.substr(1, subscripted.size() - 2),
                                         Mode::Scalar);
                if (not key) return s::unexpected(key.error());
                Assignment as;
                as.name = name;
                as.value = w->array ? (w->arr.empty() ? str{} : w->arr[0]) : w->scalar;
                as.subscript = *key;
                r = ex.exec().assign_expanded(as);
              } else {
                r = w->array ? ctx().params.set_array(name, w->arr)
                             : ctx().params.set_scalar(name, w->scalar);
              }
              if (not r) return s::unexpected(r.error());
              v = *w;
            }
          break; default:  // '?'
            if (empty_test) {
              str msg = word.empty()
                  ? (v.set ? "parameter null" : "parameter not set")
                  : str(word);
              if (not word.empty()) {
                auto w = expand_scalar(word, Mode::Scalar);
                if (w) msg = *w;
              }
              return s::unexpected(subst_error(name + ": " + msg));
            }
        }
        join_dq();
      } else if (colon and o == '#') {
        // ${x:#pat}: drop array elements matching pat (scalar→empty if
        // it matches); ${(M)x:#pat}: keep matches instead.  Introduced
        // in M13 Phase 13a.  The pattern operand is read with `dq=false`
        // so glob characters stay active even when the whole
        // substitution is in a double-quoted context.
        auto pat = operand_pattern(rest.substr(1), /*dq=*/false);
        if (not pat) return s::unexpected(pat.error());
        auto keep = [&](const str& s) {
          bool m = pat->matches(s);
          return fl.match_mode ? m : not m;
        };
        if (v.array) {
          vec<str> out;
          for (auto& e : v.arr) if (keep(e)) out.push_back(e);
          v.make_array(s::move(out));
        } else {
          if (not keep(v.scalar)) v.make_scalar("");
        }
      } else if (colon and (o == '|' or o == '*')) {
        // ${a:|b}: the elements of a not in array b; ${a:*b}: those in it.
        uset<str> other;
        if (auto p = ctx().params.lookup(rest.substr(1)); p and p->get().is_set())
          for (auto& e : p->get().is_array() ? p->get().array() : vec<str>{p->get().scalar()})
            other.insert(e);
        vec<str> out;
        for (auto& e : v.list())
          if (other.contains(e) == (o == '*')) out.push_back(e);
        if (v.array) v.make_array(s::move(out));
        else         v.make_scalar(out.empty() ? str{} : out[0]);
      } else if (not colon and (o == '#' or o == '%')) {
        if (not v.set and not opts().get(OPT_UNSET) and not special)
          return s::unexpected(subst_error(name + ": parameter not set"));
        join_dq();
        bool longest = rest.size() > 1 and rest[1] == o;
        auto pat = operand_pattern(rest.substr(longest ? 2 : 1), /*dq=*/false);
        if (not pat) return s::unexpected(pat.error());
        auto nth = nth_match(fl);
        if (not nth) return s::unexpected(nth.error());
        each(v, [&](const str& s) { return get_match(s, *pat, o == '%', longest, fl, *nth); });
      } else if (o == '/') {
        join_dq();
        strv body = rest.substr(1);
        bool global = false;
        char anchor = colon ? 'w' : 0;
        if (not colon and not body.empty() and body[0] == '/') {
          global = true;
          body.remove_prefix(1);
        }
        // `#` or `%` anchors, after `//` too (${y//#/Z} is Zabc, as zsh).
        if (not body.empty() and (body[0] == '#' or body[0] == '%')) {
          anchor = body[0];
          body.remove_prefix(1);
        }
        s::size_t slash = strv::npos;
        for (s::size_t k = 0; k < body.size(); ++k) {
          if (body[k] == '\\') { ++k; continue; }
          if (body[k] == '/') { slash = k; break; }
        }
        auto pat = operand_pattern(body.substr(0, slash), /*dq=*/false);
        if (not pat) return s::unexpected(pat.error());
        auto nth = nth_match(fl);
        if (not nth) return s::unexpected(nth.error());
        strv repl_text = slash != strv::npos ? body.substr(slash + 1) : strv{};
        if (pat->backrefs() or pat->sets_match()) {
          // (#b) and (#m): the replacement is expanded for each match, with
          // $match and $MATCH set to it.
          opt<ShellError> err;
          each(v, [&](const str& s) {
            if (err) return s;
            auto r = replace_each(s, *pat, repl_text, global, anchor, fl.m_substr, *nth);
            if (not r) {
              err = r.error();
              return s;
            }
            return *r;
          });
          if (err) return s::unexpected(*err);
        } else {
          str repl;
          if (slash != strv::npos) {
            auto r = expand_scalar(repl_text, Mode::Scalar);
            if (not r) return s::unexpected(r.error());
            repl = *r;
          }
          each(v, [&](const str& s) {
            return replace(s, *pat, repl, global, anchor, fl.m_substr, *nth);
          });
        }
      } else if (colon and (o == 0 or s::isdigit(static_cast<unsigned char>(o))
                            or o == ' ' or o == '(' or o == '$')) {
        // ${x:offset} / ${x:offset:length}, 0-based.
        join_dq();
        s::size_t c2 = find_top_colon(rest);
        auto off = arith(rest.substr(0, c2));
        if (not off) return s::unexpected(off.error());
        opt<s::int64_t> len;
        if (c2 != strv::npos) {
          auto l = arith(rest.substr(c2 + 1));
          if (not l) return s::unexpected(l.error());
          len = *l;
        }
        auto cut = [&](s::int64_t n, s::int64_t& b, s::int64_t& e) {
          b = *off < 0 ? s::max<s::int64_t>(0, n + *off) : s::min(*off, n);
          e = not len ? n : (*len < 0 ? s::max(b, n + *len) : s::min(n, b + *len));
        };
        // $@ and $* count $0 as offset 0, as zsh (and bash) do.
        if (v.array and not nested and (name == "@" or name == "*"))
          v.arr.insert(v.arr.begin(), get_param("0", ParamFlags{}).scalar);
        if (v.array) {
          s::int64_t b, e;
          cut(static_cast<s::int64_t>(v.arr.size()), b, e);
          v.make_array(vec<str>(v.arr.begin() + b, v.arr.begin() + e));
        } else {
          s::int64_t b, e;
          cut(static_cast<s::int64_t>(v.scalar.size()), b, e);
          v.make_scalar(v.scalar.substr(static_cast<s::size_t>(b), static_cast<s::size_t>(e - b)));
        }
      } else if (colon and (strv("htreulqQaAcPs&").find(o) != strv::npos or o == 'g')) {
        join_dq();
        // :h :t :r :e :u :l :q :Q :a :A :c :P modifiers, chained (`:t:r`),
        // plus :s/pat/repl/ (and :gs/pat/repl/ for global), the
        // history-style substitution modifier (M13 Phase 13a), and :& for
        // "repeat the last :s".
        if (auto r = apply_modifiers(v, op); not r) return s::unexpected(r.error());
      } else {
        return s::unexpected(subst_error("bad substitution"));
      }
    } else {
      if (not v.set and not opts().get(OPT_UNSET) and not special and not nested)
        return s::unexpected(subst_error(name + str(subscripted)
                                         + ": parameter not set"));
      // M13 Phase 13a: don't pre-join arrays here — the (V)/(#)/(c)/(w)/
      // (D) flags below work element-wise through each(), and the final
      // dq-join at the end of this function still collapses the array
      // into one word for the scalar `"..."` context.
    }

    if (length) {
      s::size_t n = v.array ? v.arr.size() : v.scalar.size();
      v.set = true;
      v.make_scalar(s::to_string(n));
      res.v = s::move(v);
      return res;
    }

    if (fl.join and v.array) {
      str joined;
      for (s::size_t k = 0; k < v.arr.size(); ++k) {
        if (k) joined += fl.join_sep;
        joined += v.arr[k];
      }
      v.make_scalar(joined);
    }
    if (fl.split or fl.split_words) {
      // A split stays words of their own in double quotes, as in zsh.
      if (dq) res.keep_separate = true;
      vec<str> out;
      for (auto& s : v.list()) {
        if (fl.split_words) {
          s::size_t before = out.size();
          // In double quotes nothing to split is one empty word.
          struct EmptyWord {
            ~EmptyWord() { if (dq and out.size() == before) out.emplace_back(); }
            vec<str>& out;
            s::size_t before;
            bool dq;
          } empty_word{out, before, dq};
          // (Z:C:) drops comments, (Z:n:) takes newlines as blanks.
          StringInputSource in{s};
          Lexer lex{in};
          if (fl.z_opts.find('C') != str::npos) lex.push_state(LEX_SKIPCOMMENTS);
          bool newline_blank = fl.z_opts.find('n') != str::npos;
          for (;;) {
            auto tok = lex.next_token();
            if (tok.is_eof()) break;
            if (tok.is_newline()) {
              if (not newline_blank) out.push_back(";");
            } else {
              out.push_back(tok.text);
            }
          }
          continue;
        }
        if (fl.split_sep.empty()) {
          for (char ch : s) out.emplace_back(1, ch);
          continue;
        }
        s::size_t b = 0;
        vec<str> parts;
        for (;;) {
          auto e = s.find(fl.split_sep, b);
          parts.push_back(s.substr(b, e == str::npos ? str::npos : e - b));
          if (e == str::npos) break;
          b = e + fl.split_sep.size();
        }
        for (s::size_t k = 0; k < parts.size(); ++k) {
          // In double quotes with (@) empty fields stay: "${(@s.:.)x}".
          // Without it, as zsh's sepsplit, a run of separators is one, but
          // an empty field at either end stays ("${(s:,:)x}" on `,a` is
          // `` and `a`; on nothing, one empty word).
          bool end = k == 0 or k + 1 == parts.size();
          if (not parts[k].empty() or (dq and (fl.at or end))) out.push_back(s::move(parts[k]));
        }
      }
      v.make_array(s::move(out));
      eq_split = false;
    }
    if (fl.upper or fl.lower or fl.capitalize) {
      each(v, [&](str s) {
        bool start = true;
        for (auto& ch : s) {
          unsigned char u = static_cast<unsigned char>(ch);
          if (fl.upper) ch = static_cast<char>(s::toupper(u));
          else if (fl.lower) ch = static_cast<char>(s::tolower(u));
          else {
            ch = static_cast<char>(start ? s::toupper(u) : s::tolower(u));
            start = not s::isalnum(u);
          }
        }
        return s;
      });
    }
    if (v.array and (fl.sort_asc or fl.sort_desc or fl.sort_numeric or fl.sort_nocase)) {
      auto key_less = [&](const str& a, const str& b) {
        if (fl.sort_nocase) {
          str x = a, y = b;
          for (char& c : x) c = static_cast<char>(s::tolower(static_cast<unsigned char>(c)));
          for (char& c : y) c = static_cast<char>(s::tolower(static_cast<unsigned char>(c)));
          return zstrcmp(x, y, fl.sort_numeric) < 0;
        }
        return zstrcmp(a, b, fl.sort_numeric) < 0;
      };
      s::stable_sort(v.arr.begin(), v.arr.end(), key_less);
      if (fl.sort_desc) s::reverse(v.arr.begin(), v.arr.end());
    }
    if (v.array and fl.unique) {
      uset<str> seen;
      vec<str> out;
      for (auto& e : v.arr) if (seen.insert(e).second) out.push_back(e);
      v.arr = s::move(out);
    }
    if (fl.unquote) each(v, [](const str& s) { return remove_quotes(s); });
    if (fl.escapes_g) each(v, [](const str& s) { return c_escapes(s); });
    if (fl.quote_pat) {
      each(v, [](const str& s) {
        str out;
        for (char c : s) {
          if (PATTERN_CHARS.find(c) != strv::npos) out += '\\';
          out += c;
        }
        return out;
      });
    }
    if (fl.quote == -2) each(v, [](const str& s) { return quote_minimal_plus(s); });
    if (fl.quote == -1) each(v, [](const str& s) { return quote_minimal(s); });
    else if (fl.quote == 1)
      each(v, [](const str& s) { return s.empty() ? str("''") : quote_backslash(s); });
    else if (fl.quote == 2) each(v, [](const str& s) { return quote_single(s); });
    else if (fl.quote == 3) each(v, [](const str& s) { return quote_double(s); });
    else if (fl.quote >= 4) each(v, [](const str& s) { return quote_dollar(s); });

    // (D) — abbreviate home-relative paths as `~` / `~user`.  Only the
    // shell's own $HOME is consulted; the named-dir table is M17.
    if (fl.dirabbr) {
      str home;
      if (auto p = ctx().params.lookup("HOME"); p and p->get().is_set())
        home = p->get().scalar();
      if (not home.empty()) {
        each(v, [&](str s) -> str {
          if (s.size() >= home.size() and s.compare(0, home.size(), home) == 0
              and (s.size() == home.size() or s[home.size()] == '/'))
            return "~" + s.substr(home.size());
          return s;
        });
      }
    }

    // (V) — make non-printables visible: C-escapes, and an empty string
    // becomes `''` so it survives splitting.
    if (fl.visible) {
      each(v, [](str s) -> str {
        if (s.empty()) return "''";
        str out;
        for (unsigned char c : s) {
          if      (c == '\t') out += "\\t";
          else if (c == '\n') out += "\\n";
          else if (c == '\r') out += "\\r";
          else if (c == '\\') out += "\\\\";
          else if (c < 0x20 or c == 0x7f) {
            char buf[8]; s::snprintf(buf, sizeof buf, "\\x%02x", c);
            out += buf;
          } else out += static_cast<char>(c);
        }
        return out;
      });
    }

    // (#) — arithmetic-evaluate each element.  Errors fall through to 0
    // as zsh does when a non-numeric word appears in arithmetic context.
    if (fl.arith) {
      each(v, [&](const str& s) -> str {
        auto r = math_eval(s, ctx().params, math_options(opts()),
                           &ctx().modules.math_funcs());
        if (not r) return "0";
        return format_math(*r);
      });
    }

    // (c) — replace each element with its character length (same semantic
    // as ${#x}, but per-element for arrays).
    if (fl.count_c) {
      each(v, [](const str& s) { return s::to_string(s.size()); });
    }

    // (w) — count words in each element (after `ifs`-style split).  For a
    // scalar this ends up with a single count; for an array, per-element.
    if (fl.count_w) {
      str sep = ifs();
      each(v, [&](const str& s) {
        return s::to_string(ifs_split(s, sep).size());
      });
    }

    // (%) — prompt escapes, as `print -P` expands them.
    if (fl.prompt)
      each(v, [&](const str& s) { return ex.exec().expand_prompt(s); });

    // (e) — parameter, command and arithmetic expansion of the value, with
    // quotes left as they are.
    if (fl.reeval) {
      opt<ShellError> err;
      each(v, [&](const str& s) {
        auto r = err ? Result<str>(s) : expand_scalar(s, Mode::Heredoc);
        if (not r) { err = r.error(); return s; }
        return *r;
      });
      if (err) return s::unexpected(*err);
    }

    // In double quotes a split result is joined again unless (@) is given.
    if (dq and v.array and not res.keep_separate) {
      str sep = ifs_first(), joined;
      for (s::size_t k = 0; k < v.arr.size(); ++k) {
        if (k) joined += sep;
        joined += v.arr[k];
      }
      v.make_scalar(joined);
    }

    // (l:n:) and (r:n:) — padding, after double-quoted joining.  With both
    // the word is split in the middle and each half padded on its side.
    if (fl.pad_l.on or fl.pad_r.on) {
      auto width = [&](const ParamFlags::Pad& p) -> Result<s::size_t> {
        auto n = arith(p.width);
        if (not n) return s::unexpected(n.error());
        return static_cast<s::size_t>(s::max<s::int64_t>(*n, 0));
      };
      opt<s::size_t> wl, wr;
      if (fl.pad_l.on) {
        auto w = width(fl.pad_l);
        if (not w) return s::unexpected(w.error());
        wl = *w;
      }
      if (fl.pad_r.on) {
        auto w = width(fl.pad_r);
        if (not w) return s::unexpected(w.error());
        wr = *w;
      }
      each(v, [&](const str& s) {
        if (wl and wr) {
          s::size_t half = s.size() / 2;
          return pad_word(s.substr(0, half), *wl, fl.pad_l, true)
               + pad_word(s.substr(half), *wr, fl.pad_r, false);
        }
        return wl ? pad_word(s, *wl, fl.pad_l, true) : pad_word(s, *wr, fl.pad_r, false);
      });
    }

    res.v     = s::move(v);
    res.split = eq_split;
    res.glob  = glob or fl.glob_pat;
    res.rc_expand = rc_expand;
    return res;
  }

  static s::size_t find_top_colon(strv s) {
    int depth = 0;
    for (s::size_t k = 0; k < s.size(); ++k) {
      if (s[k] == '(') ++depth;
      else if (s[k] == ')') --depth;
      else if (s[k] == ':' and depth == 0) return k;
    }
    return strv::npos;
  }
};

// ---------------------------------------------------------------------------
// Brace expansion.
// ---------------------------------------------------------------------------

// Parses `a..b` or `a..b..step` (numbers or single characters) and returns
// the sequence, or nothing when `s` is not a sequence.
opt<vec<str>> brace_sequence(strv s) {
  auto dots = s.find("..");
  if (dots == strv::npos) return s::nullopt;
  strv a = s.substr(0, dots), rest = s.substr(dots + 2);
  strv b = rest, step_s;
  if (auto d2 = rest.find(".."); d2 != strv::npos) {
    b = rest.substr(0, d2);
    step_s = rest.substr(d2 + 2);
  }
  auto is_int = [](strv x) {
    if (x.empty()) return false;
    s::size_t k = (x[0] == '-' or x[0] == '+') ? 1 : 0;
    if (k == x.size()) return false;
    for (; k < x.size(); ++k)
      if (not s::isdigit(static_cast<unsigned char>(x[k]))) return false;
    return true;
  };
  s::int64_t step = 1;
  if (not step_s.empty()) {
    if (not is_int(step_s)) return s::nullopt;
    s::int64_t v = s::strtoll(str(step_s).c_str(), nullptr, 10);
    step = v == INT64_MIN ? INT64_MAX : s::abs(v);
    if (step == 0) step = 1;
  }
  vec<str> out;
  if (is_int(a) and is_int(b)) {
    s::int64_t x = s::strtoll(str(a).c_str(), nullptr, 10);
    s::int64_t y = s::strtoll(str(b).c_str(), nullptr, 10);
    auto padded = [](strv v) {
      strv d = (v[0] == '-' or v[0] == '+') ? v.substr(1) : v;
      return d.size() > 1 and d[0] == '0';
    };
    s::size_t width = (padded(a) or padded(b)) ? s::max(a.size(), b.size()) : 0;
    // Step towards y without overflowing past it.
    using U = s::uint64_t;
    for (s::int64_t k = x;;) {
      out.push_back(s::to_string(k));
      U left = x <= y ? static_cast<U>(y) - static_cast<U>(k)
                      : static_cast<U>(k) - static_cast<U>(y);
      if (left < static_cast<U>(step)) break;
      k = x <= y ? k + step : k - step;
    }
    if (width)
      for (auto& e : out) {
        bool neg = e[0] == '-';
        str digits = neg ? e.substr(1) : e;
        s::size_t want = width - (neg ? 1 : 0);
        if (digits.size() < want) digits.insert(0, want - digits.size(), '0');
        e = (neg ? "-" : "") + digits;
      }
    return out;
  }
  if (a.size() == 1 and b.size() == 1) {
    int x = static_cast<unsigned char>(a[0]), y = static_cast<unsigned char>(b[0]);
    int by = static_cast<int>(step);
    if (x <= y)
      for (int k = x; k <= y; k += by) out.emplace_back(1, static_cast<char>(k));
    else
      for (int k = x; k >= y; k -= by) out.emplace_back(1, static_cast<char>(k));
    return out;
  }
  return s::nullopt;
}

void brace_expand(const Field& f, bool ccl, vec<Field>& out) {
  for (s::size_t b = 0; b < f.text.size(); ++b) {
    if (f.text[b] != '{' or not f.unlit(b)) continue;
    // Find the matching unquoted `}` and the top-level commas.
    int depth = 0;
    s::size_t e = str::npos;
    vec<s::size_t> commas;
    for (s::size_t k = b + 1; k < f.text.size(); ++k) {
      if (not f.unlit(k)) continue;
      char c = f.text[k];
      if (c == '{') ++depth;
      else if (c == '}') {
        if (depth == 0) { e = k; break; }
        --depth;
      } else if (c == ',' and depth == 0) {
        commas.push_back(k);
      }
    }
    if (e == str::npos) return out.push_back(f);
    Field prefix = f.slice(0, b), suffix = f.slice(e + 1, f.text.size());
    vec<Field> alts;
    if (not commas.empty()) {
      s::size_t s = b + 1;
      for (s::size_t c : commas) { alts.push_back(f.slice(s, c)); s = c + 1; }
      alts.push_back(f.slice(s, e));
    } else {
      Field inner = f.slice(b + 1, e);
      // A range expands whatever made its text, as in zsh: {1..$#a},
      // {"1"..3}; only the braces must be unquoted.
      if (auto seq = brace_sequence(inner.text)) {
        for (auto& s : *seq) {
          Field a;
          a.put(s, false);
          alts.push_back(s::move(a));
        }
      }
      if (alts.empty() and ccl and not inner.text.empty()) {
        vec<char> chars(inner.text.begin(), inner.text.end());
        s::sort(chars.begin(), chars.end());
        chars.erase(s::unique(chars.begin(), chars.end()), chars.end());
        for (char c : chars) {
          Field a;
          a.put(c, true);
          alts.push_back(s::move(a));
        }
      }
      if (alts.empty()) continue;   // `{a}`, `{}`: not an expansion
    }
    for (auto& a : alts) {
      Field w = prefix;
      w.append(a);
      w.append(suffix);
      brace_expand(w, ccl, out);
    }
    return;
  }
  out.push_back(f);
}

}  // namespace

// ---------------------------------------------------------------------------
// Public helpers.
// ---------------------------------------------------------------------------

str remove_quotes(strv w) {
  constexpr strv DQUOTE_ESCAPES = "$`\"\\\n";
  enum class Quote : int { None, Single, Double } q = Quote::None;
  str out;
  out.reserve(w.size());
  for (s::size_t i = 0; i < w.size(); ++i) {
    char c = w[i];
    bool escape = c == '\\' and i + 1 < w.size()
        and (q == Quote::None
             or (q == Quote::Double
                 and DQUOTE_ESCAPES.find(w[i + 1]) != strv::npos));
    if (q == Quote::Single) {
      if (c == '\'') q = Quote::None;
      else           out += c;
    } else if (escape) {
      if (w[i + 1] != '\n') out += w[i + 1];
      ++i;
    } else if (q == Quote::Double) {
      if (c == '"') q = Quote::None;
      else          out += c;
    } else if (c == '$' and i + 1 < w.size() and w[i + 1] == '\'') {
      s::size_t e = i + 2;
      while (e < w.size() and w[e] != '\'') e += (w[e] == '\\') ? 2u : 1u;
      out += c_escapes(w.substr(i + 2, s::min(e, w.size()) - i - 2));
      i = e;
    } else if (c == '\'') {
      q = Quote::Single;
    } else if (c == '"') {
      q = Quote::Double;
    } else {
      out += c;
    }
  }
  return out;
}

vec<str> ifs_split(strv s, strv ifs) {
  auto is_sep = [&](char c) { return ifs.find(c) != strv::npos; };
  auto is_ws  = [&](char c) { return is_sep(c) and (c == ' ' or c == '\t' or c == '\n'); };
  auto skip_ws = [&](s::size_t& i) { while (i < s.size() and is_ws(s[i])) ++i; };
  vec<str> out;
  str cur;
  bool open = false;   // a word has started and not been pushed yet
  s::size_t i = 0;
  skip_ws(i);
  while (i < s.size()) {
    if (not is_sep(s[i])) {
      cur += s[i++];
      open = true;
      continue;
    }
    // One separator: a run of IFS whitespace with at most one other IFS
    // character in it.
    out.push_back(s::move(cur));
    cur.clear();
    open = false;
    bool hard = not is_ws(s[i]);
    ++i;
    skip_ws(i);
    if (not hard and i < s.size() and is_sep(s[i])) {
      ++i;
      skip_ws(i);
    }
  }
  if (open) out.push_back(s::move(cur));
  return out;
}

// ---------------------------------------------------------------------------
// Expander.
// ---------------------------------------------------------------------------
namespace {

// `~`, `~+`, `~-`, `~user` at text[p] (unquoted); `end` bounds the name.
Result<void> tilde_at(Field& f, s::size_t p, s::size_t end, ExecContext& ctx) {
  for (s::size_t k = p; k < end; ++k)
    if (f.lit[k]) return {};
  str name = f.text.substr(p + 1, end - p - 1);
  auto param = [&](const char* n) -> opt<str> {
    auto v = ctx.params.lookup(n);
    if (v and v->get().is_set()) return v->get().scalar();
    return s::nullopt;
  };
  opt<str> dir;
  if (name.empty()) {
    dir = param("HOME");
    if (not dir) if (auto* pw = ::getpwuid(::getuid())) dir = pw->pw_dir;
  } else if (name == "+") {
    dir = param("PWD");
  } else if (name == "-") {
    dir = param("OLDPWD");
  } else if (auto digits = strv(name).substr(name[0] == '+' or name[0] == '-' ? 1 : 0);
             not digits.empty() and s::all_of(digits.begin(), digits.end(), [](char d) {
               return s::isdigit(static_cast<unsigned char>(d)); })) {
    // ~N, ~+N, ~-N: entries of the directory stack, $PWD being entry 0.
    vec<str> stack;
    if (auto pwd = param("PWD")) stack.push_back(*pwd);
    stack.insert(stack.end(), ctx.dirstack.begin(), ctx.dirstack.end());
    s::size_t n = s::strtoul(str(digits).c_str(), nullptr, 10);
    if (n >= stack.size())
      return s::unexpected(subst_error("not enough directory stack entries."));
    dir = name[0] == '-' ? stack[stack.size() - 1 - n] : stack[n];
  } else {
    for (char c : name)
      if (not is_name_char(c) and c != '-' and c != '.') return {};
    if (auto* pw = ::getpwnam(name.c_str())) dir = pw->pw_dir;
    else return s::unexpected(subst_error("no such user or named directory: " + name));
  }
  if (not dir) return {};
  Field out = f.slice(0, p);
  out.quoted = f.quoted;
  out.put(*dir, true);
  out.append(f.slice(end, f.text.size()));
  f = s::move(out);
  return {};
}

// Tilde expansion at the start of the word and, for assignments (or
// MAGIC_EQUAL_SUBST arguments), after `=` and each `:`.
Result<void> tilde_expand(Field& f, bool assign_like, ExecContext& ctx) {
  auto bound = [&](s::size_t p) {
    s::size_t e = p + 1;
    while (e < f.text.size() and f.text[e] != '/' and not (assign_like and f.text[e] == ':'))
      ++e;
    return e;
  };
  if (f.unlit(0) and f.text[0] == '~')
    if (auto r = tilde_at(f, 0, bound(0), ctx); not r) return r;
  if (not assign_like) return {};
  s::size_t eq = str::npos;
  for (s::size_t k = 0; k < f.text.size(); ++k)
    if (f.unlit(k) and f.text[k] == '=') { eq = k; break; }
  for (s::size_t k = 0; k + 1 < f.text.size(); ++k) {
    bool after = (eq != str::npos and k == eq) or (f.unlit(k) and f.text[k] == ':');
    if (after and f.unlit(k + 1) and f.text[k + 1] == '~')
      if (auto r = tilde_at(f, k + 1, bound(k + 1), ctx); not r) return r;
  }
  return {};
}

}  // namespace

Result<vec<str>> Expander::words(const vec<str>& words) {
  vec<str> out;
  auto& opts = mCtx.opts;
  for (const auto& w : words) {
    Scan scan{*this, Mode::Word};
    scan.top = true;
    if (auto r = scan.scan(w); not r) return s::unexpected(r.error());

    vec<Field> fields;
    for (auto& f : scan.fields) {
      if (f.dropped) continue;
      if (opts.get(OPT_IGNOREBRACES)) fields.push_back(s::move(f));
      else brace_expand(f, opts.get(OPT_BRACECCL), fields);
    }
    for (auto& f : fields) {
      if (f.text.empty()) {
        if (f.quoted and not f.empty_at) out.emplace_back();
        continue;
      }
      bool magic = opts.get(OPT_MAGICEQUALSUBST)
          and s::any_of(f.text.begin(), f.text.end(), [](char c) { return c == '='; });
      if (auto r = tilde_expand(f, magic, mCtx); not r) return s::unexpected(r.error());
      // `=cmd` becomes the command's path.
      if (opts.get(OPT_EQUALS) and f.text.size() > 1 and f.text[0] == '=' and f.unlit(0)
          and s::none_of(f.lit.begin(), f.lit.end(), [](bool l) { return l; })
          and f.text[1] != '(') {
        str name = f.text.substr(1);
        auto path = mExec.find_command(name);
        if (not path) return s::unexpected(subst_error(name + " not found"));
        out.push_back(*path);
        continue;
      }
      if (opts.get(OPT_GLOB) and f.has_unlit_glob()) {
        auto matches = glob_expand(GlobPattern::compile(f.as_pattern()), opts, &glob_hooks());
        if (not matches) return s::unexpected(matches.error());
        for (auto& m : *matches) out.push_back(s::move(m));
        continue;
      }
      out.push_back(s::move(f.text));
    }
  }
  return out;
}

const GlobHooks& Expander::glob_hooks() {
  if (mGlobHooks.eval) return mGlobHooks;
  // e:code: and +func run with $REPLY set to the file; $reply, or a new
  // $REPLY, names what to use instead.  $? is left as it was.
  mGlobHooks.eval = [this](strv code, bool, const str& file) -> opt<vec<str>> {
    auto& params = mCtx.params;
    (void)params.unset("reply");
    (void)params.set_scalar("REPLY", file);
    int  last     = mCtx.last_exit;
    bool aborting = mCtx.aborting;
    int  rc       = mExec.exec_string(str(code));
    opt<vec<str>> out;
    if (rc == 0 and not (mCtx.aborting and not aborting)) {
      out.emplace();
      auto reply = params.lookup("reply");
      if (reply and reply->get().is_set() and (reply->get().is_array() or reply->get().is_hash())) {
        if (reply->get().is_array()) {
          *out = reply->get().array();
        } else {
          for (auto& [k, v] : reply->get().hash()) out->push_back(v);
        }
      } else if (auto r = params.lookup("REPLY"); r and r->get().scalar() != file) {
        out->push_back(r->get().scalar());
      }
    }
    mCtx.aborting  = aborting;
    mCtx.last_exit = last;
    (void)params.unset("reply");
    return out;
  };
  mGlobHooks.modify = [this](strv mods, const str& file) -> Result<str> {
    Scan scan{*this, Mode::Scalar};
    Value v;
    v.set = true;
    v.make_scalar(file);
    if (auto r = scan.apply_modifiers(v, mods); not r) return s::unexpected(r.error());
    return v.scalar;
  };
  return mGlobHooks;
}

Result<str> Expander::scalar(strv word, bool assignment) {
  Scan scan{*this, Mode::Scalar};
  scan.top = true;
  if (auto r = scan.scan(word); not r) return s::unexpected(r.error());
  Field all;
  for (s::size_t k = 0; k < scan.fields.size(); ++k) {
    if (k) all.put(' ', true);
    all.append(scan.fields[k]);
  }
  if (auto r = tilde_expand(all, assignment, mCtx); not r) return s::unexpected(r.error());
  return all.text;
}

Result<vec<str>> Expander::array_elements(strv src) {
  StringInputSource in{str(src)};
  Lexer lex{in};
  // Elements are arguments: `(` starts a glob group word, a=( (a|b)1 ).
  lex.push_state(static_cast<LexFlags>(LEX_SKIPCOMMENTS | LEX_ARGPAREN));
  vec<str> raw;
  for (;;) {
    auto tok = lex.next_token();
    if (tok.is_eof()) break;
    if (tok.is_newline()) continue;
    // `(<(cmd))`: a process substitution as an element.
    bool procsubst = tok.type == TokType::TK_REDIR and tok.text.size() > 2
                 and tok.text[1] == '(' and tok.text.back() == ')';
    if (tok.type != TokType::TK_WORD and tok.type < TokType::TK_IF and not procsubst)
      return s::unexpected(ShellError{ErrorKind::ParseError,
          "parse error near `" + tok.text + "'"});
    raw.push_back(s::move(tok.text));
  }
  return words(raw);
}

Result<str> Expander::heredoc(strv body) {
  Scan scan{*this, Mode::Heredoc};
  if (auto r = scan.scan(body); not r) return s::unexpected(r.error());
  str out;
  for (s::size_t k = 0; k < scan.fields.size(); ++k) {
    if (k) out += ' ';
    out += scan.fields[k].text;
  }
  return out;
}

Result<s::int64_t> Expander::arith(strv expr) {
  Scan scan{*this, Mode::Heredoc};
  return scan.arith(expr);
}

Result<MathNum> Expander::math(strv expr) {
  Scan scan{*this, Mode::Heredoc};
  return scan.math(expr);
}

Result<str> Expander::pattern(strv word) {
  Scan scan{*this, Mode::Scalar};
  if (auto r = scan.scan(word); not r) return s::unexpected(r.error());
  str out;
  for (s::size_t k = 0; k < scan.fields.size(); ++k) {
    if (k) out += ' ';
    out += scan.fields[k].as_pattern();
  }
  return out;
}

}  // namespace jfsh
