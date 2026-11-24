#include <jfsh/exec/subst.h>

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <pwd.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <unordered_set>

#include <jfsh/builtins/math.h>
#include <jfsh/core/aliases.h>
#include <jfsh/core/sort.h>
#include <jfsh/core/stack.h>
#include <jfsh/core/utf8.h>
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

// zsh's error for a pattern it cannot compile.
ShellError bad_pattern(strv pat) {
  return ShellError{ErrorKind::BadPattern, "bad pattern: " + glob_unescape(pat)};
}

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

s::size_t find_close(strv w, s::size_t open, bool dq = false);

// Set when find_close() gave up because the stack ran low.
thread_local bool CloseTooDeep = false;

// The error for a `${` that find_close() found unclosed.
ShellError unclosed_error() {
  bool deep = s::exchange(CloseTooDeep, false);
  return subst_error(deep ? "substitutions nested too deeply" : "bad substitution");
}

// Index of the `"` that ends the double-quoted text opened at w[q],
// skipping the substitutions inside it; npos when it is never closed.
s::size_t dquote_end(strv w, s::size_t q) {
  for (s::size_t i = q + 1; i < w.size(); ++i) {
    char ch = w[i];
    if (ch == '"') return i;
    if (ch == '\\') {
      ++i;
    } else if (ch == '`') {
      for (++i; i < w.size() and w[i] != '`'; ++i)
        if (w[i] == '\\') ++i;
      if (i >= w.size()) return strv::npos;
    } else if (ch == '$' and i + 1 < w.size() and (w[i + 1] == '(' or w[i + 1] == '{')) {
      // ${…} stays in the double quotes; $(…) starts afresh.
      auto e = find_close(w, i + 1, w[i + 1] == '{');
      if (e == strv::npos) return e;
      i = e;
    }
  }
  return strv::npos;
}

// Index of the character that closes the bracket at w[open], skipping
// quoted text and nested substitutions; npos when it is never closed.
// `dq`: a ${…} in double quotes, where, as in zsh, a `{` inside does not
// pair with a `}` ("${x:-a{b\}c}") and a `'` is an ordinary character:
// "${y:-'}'}" ends at the first `}`.
s::size_t find_close(strv w, s::size_t open, bool dq) {
  // Nesting past what the stack holds counts as never closed, and the
  // caller says why (unclosed_error()).
  if (stack_low()) { CloseTooDeep = true; return strv::npos; }
  char o = w[open];
  char c = o == '(' ? ')' : o == '{' ? '}' : ']';
  bool singles = o != '[' and not (dq and o == '{');
  int depth = 1;
  for (s::size_t i = open + 1; i < w.size(); ++i) {
    char ch = w[i];
    if (ch == '\\') { ++i; continue; }
    if (ch == '$' and i + 1 < w.size() and w[i + 1] == '\'' and singles) {
      // $'…', where `\'` doesn't end it.
      for (i += 2; i < w.size() and w[i] != '\''; ++i)
        if (w[i] == '\\') ++i;
      if (i >= w.size()) return strv::npos;
      continue;
    }
    if (ch == '\'' and singles) {
      auto e = w.find('\'', i + 1);
      if (e == strv::npos) return e;
      i = e;
    } else if (ch == '"') {
      auto e = dquote_end(w, i);
      if (e == strv::npos) return e;
      i = e;
    } else if (ch == '`') {
      for (++i; i < w.size() and w[i] != '`'; ++i)
        if (w[i] == '\\') ++i;
    } else if (ch == '$' and i + 1 < w.size()
               and (w[i + 1] == '(' or w[i + 1] == '{')
               and (w[i + 1] != o or (dq and o == '{'))) {
      auto e = find_close(w, i + 1, w[i + 1] == '{' and dq and o == '{');
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

// Decodes the body of $'…' (and the (p) flag's separators): C escapes,
// with zsh's \C-x (control) and \M-x (meta) applying to the next
// character, however it is written.
str c_escapes(strv s) {
  str out;
  bool ctl = false, meta = false;
  auto emit = [&](char ch) {
    auto c = static_cast<unsigned char>(ch);
    if (ctl)  c = c == '?' ? 0x7f : static_cast<unsigned char>(c & 0x9f);
    if (meta) c = static_cast<unsigned char>(c | 0x80);
    ctl = meta = false;
    out += static_cast<char>(c);
  };
  for (s::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' or i + 1 >= s.size()) { emit(s[i]); continue; }
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
      case 'a': emit('\a');
      break; case 'b': emit('\b');
      break; case 'e': case 'E': emit('\x1b');
      break; case 'f': emit('\f');
      break; case 'n': emit('\n');
      break; case 'r': emit('\r');
      break; case 't': emit('\t');
      break; case 'v': emit('\v');
      break; case 'x': emit(static_cast<char>(hex(2)));
      break; case 'u': append_utf8(out, hex(4));
      break; case 'U': append_utf8(out, hex(8));
      break; case 'C': case 'M': {
        // With nothing after it, zsh drops it.
        if (i + 1 < s.size() and s[i + 1] == '-') ++i;
        (e == 'C' ? ctl : meta) = true;
      }
      break; case '0': case '1': case '2': case '3':
      case '4': case '5': case '6': case '7': {
        int v = e - '0';
        for (int n = 0; n < 2 and i + 1 < s.size() and s[i + 1] >= '0' and s[i + 1] <= '7'; ++n)
          v = v * 8 + (s[++i] - '0');
        emit(static_cast<char>(v));
      }
      break; default: emit(e);
    }
  }
  return out;
}

// The length of the character at s[i] when it prints as itself, or 0 for
// a control character or (under `mb`, a byte outside a valid UTF-8
// character; without it, any byte from 0x80), which zsh's quoting writes
// as an escape.
s::size_t printable_len(strv s, s::size_t i, bool mb) {
  auto c = static_cast<unsigned char>(s[i]);
  if (c < 0x80) return c < 0x20 or c == 0x7f ? 0 : 1;
  if (not mb) return 0;
  s::size_t len = 1;
  utf8_decode(s, i, len);
  return len > 1 ? len : 0;
}

// `c` inside $'…': a named escape, else three octal digits.
str dollar_escape(unsigned char c, bool nul_short) {
  switch (c) {
    case '\n': return "\\n";
    case '\t': return "\\t";
    case '\r': return "\\r";
    case '\a': return "\\a";
    case '\b': return "\\b";
    case '\f': return "\\f";
    case '\v': return "\\v";
    default: {
      if (c == 0 and nul_short) return "\\0";
      char buf[8];
      s::snprintf(buf, sizeof buf, "\\%03o", c);
      return buf;
    }
  }
}

// (q) and :q: a backslash before each special character, and $'…' for
// each unprintable one.
str quote_backslash(strv s, bool mb) {
  constexpr strv SPECIAL = " \t\n'\"\\$`&|;<>()[]{}*?#^";
  str out;
  for (s::size_t i = 0; i < s.size();) {
    if (s::size_t n = printable_len(s, i, mb); n == 0) {
      out += "$'" + dollar_escape(static_cast<unsigned char>(s[i]), true) + "'";
      ++i;
    } else {
      char c = s[i];
      if (n == 1 and (SPECIAL.find(c) != strv::npos or (i == 0 and (c == '~' or c == '='))))
        out += '\\';
      out += s.substr(i, n);
      i += n;
    }
  }
  return out;
}

// (qq).
str quote_single(strv s) {
  str out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else           out += c;
  }
  return out + "'";
}

// (qqq).
str quote_double(strv s) {
  str out = "\"";
  for (char c : s) {
    if (strv("\"\\$`").find(c) != strv::npos) out += '\\';
    out += c;
  }
  return out + "\"";
}

// (qqqq).
str quote_dollar(strv s, bool mb) {
  str out = "$'";
  for (s::size_t i = 0; i < s.size();) {
    if (s::size_t n = printable_len(s, i, mb); n == 0) {
      out += dollar_escape(static_cast<unsigned char>(s[i]), true);
      ++i;
    } else {
      if (n == 1 and strv("'\\!").find(s[i]) != strv::npos) out += '\\';
      out += s.substr(i, n);
      i += n;
    }
  }
  return out + "'";
}

// Whether (q-) or (q+) must quote `s`.  (q+) also quotes `~` and `=`
// anywhere, (q-) only at the start.
bool needs_quotes(strv s, bool plus) {
  constexpr strv SPECIAL = " \t\n\"\\$`&|;<>()[]{}*?#^";
  for (s::size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c == '\0' or SPECIAL.find(c) != strv::npos) return true;
    if ((c == '~' or c == '=') and (plus or i == 0)) return true;
  }
  return false;
}

// (q-): quotes only where needed.  The text between single quotes goes in
// '…' when it needs it, and each single quote becomes \'.
str quote_minimal(strv s) {
  if (s.empty()) return "''";
  str out;
  for (s::size_t b = 0;;) {
    auto e = s.find('\'', b);
    strv part = s.substr(b, e == strv::npos ? strv::npos : e - b);
    if (needs_quotes(part, false))
      out += "'" + str(part) + "'";
    else
      out += part;
    if (e == strv::npos) break;
    out += "\\'";
    b = e + 1;
  }
  return out;
}

// (q+): like (q-), but text with unprintable characters becomes one
// $'…' word with \C- and \M- for them, and text with a single quote has
// every other part quoted.
str quote_plus(strv s, bool mb) {
  if (s.empty()) return "''";
  bool unprintable = false;
  for (s::size_t i = 0, n = 0; i < s.size() and not unprintable; i += n ? n : 1)
    unprintable = (n = printable_len(s, i, mb)) == 0;
  if (unprintable) {
    str out = "$'";
    for (s::size_t i = 0; i < s.size();) {
      s::size_t n = printable_len(s, i, mb);
      auto c = static_cast<unsigned char>(s[i]);
      if (n) {
        if (n == 1 and strv("'\\").find(s[i]) != strv::npos) out += '\\';
        out += s.substr(i, n);
        i += n;
        continue;
      }
      ++i;
      if (c == '\n' or c == '\t') { out += dollar_escape(c, true); continue; }
      if (c >= 0x80) { out += "\\M-"; c = static_cast<unsigned char>(c - 0x80); }
      if (c < 0x20)       { out += "\\C-"; out += static_cast<char>(c + 0x40); }
      else if (c == 0x7f) out += "\\C-?";
      else                out += static_cast<char>(c);
    }
    return out + "'";
  }
  if (s.find('\'') == strv::npos)
    return needs_quotes(s, true) ? "'" + str(s) + "'" : str(s);
  str out;
  for (s::size_t b = 0;;) {
    auto e = s.find('\'', b);
    strv part = s.substr(b, e == strv::npos ? strv::npos : e - b);
    if (not part.empty()) out += "'" + str(part) + "'";
    if (e == strv::npos) break;
    out += "\\'";
    b = e + 1;
  }
  return out;
}

// (U), (L), (C), :u and :l: `how` is 'U', 'L' or 'C' (capitalize each
// word); under `mb` a UTF-8 character is mapped as one character.
str map_case(strv s, char how, bool mb) {
  str out;
  out.reserve(s.size());
  bool start = true;
  for (s::size_t i = 0; i < s.size();) {
    s::size_t len = 1;
    char32_t c = mb ? utf8_decode(s, i, len) : static_cast<unsigned char>(s[i]);
    bool up = how == 'U' or (how == 'C' and start);
    if (len == 1 and c < 0x80) {
      auto u = static_cast<unsigned char>(s[i]);
      out += static_cast<char>(up ? s::toupper(u) : s::tolower(u));
      start = not s::isalnum(u);
    } else if (len > 1) {
      auto w = static_cast<wint_t>(c);
      utf8_append(out, static_cast<char32_t>(up ? ::towupper(w) : ::towlower(w)));
      start = not ::iswalnum(w);
    } else {
      out += s[i];
      start = true;
    }
    i += len;
  }
  return out;
}

// A piece of text split on $IFS.  `keep` is false only for the empty
// pieces that leading or trailing IFS whitespace leaves: they glue the
// neighbouring text but vanish as words of their own.
struct IfsPiece {
  str  text;
  bool keep = true;
};

// The parts of `s` between occurrences of `sep`, empty ones included.
vec<str> split_on(strv s, strv sep) {
  vec<str> out;
  for (s::size_t b = 0;;) {
    auto e = s.find(sep, b);
    out.emplace_back(s.substr(b, e == strv::npos ? strv::npos : e - b));
    if (e == strv::npos) break;
    b = e + sep.size();
  }
  return out;
}

// Text as zsh's zerr() prints it: control characters as \t, \n or ^X.
str nice_text(strv s) {
  str out;
  for (char ch : s) {
    auto c = static_cast<unsigned char>(ch);
    if (c == '\n')                  out += "\\n";
    else if (c == '\t')             out += "\\t";
    else if (c < 0x20 or c == 0x7f) { out += '^'; out += static_cast<char>(c ^ 0x40); }
    else                            out += ch;
  }
  return out;
}

// zsh's spacesplit(): a run of IFS whitespace with at most one other IFS
// character in it is one separator, so two non-whitespace separators in a
// row, or one at either end, leave an empty piece that is kept.
vec<IfsPiece> ifs_pieces(strv s, strv ifs) {
  auto is_sep  = [&](char c) { return ifs.find(c) != strv::npos; };
  auto is_ws   = [&](char c) {
    return (c == ' ' or c == '\t' or c == '\n') and is_sep(c);
  };
  auto is_hard = [&](char c) { return is_sep(c) and not is_ws(c); };
  vec<IfsPiece> out;
  s::size_t i = 0, n = s.size();
  auto skip_ws = [&] { while (i < n and is_ws(s[i])) ++i; };
  skip_ws();
  if (i < n and is_hard(s[i])) out.push_back({"", true});
  else if (i != 0)             out.push_back({"", false});
  s::size_t t = 0;
  while (i < n) {
    if (is_hard(s[i])) { ++i; skip_ws(); }
    t = i;
    while (i < n and not is_sep(s[i])) ++i;
    out.push_back({str(s.substr(t, i - t)), true});
    t = i;
    skip_ws();
  }
  if (t != i) out.push_back({"", false});
  return out;
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
  // Where in `text` each quoted string began, so splitting an operand
  // keeps a quoted empty string as a word (`${=t:+a "" b}`).  Only the
  // scanner sets it; slices and joins leave it out.
  vec<s::size_t> qmarks;

  void put(char c, bool l) { text += c; lit.push_back(l); }
  // The text of an expansion: even where it may glob, its braces and
  // commas are never brace syntax.
  void put(strv s, bool l) {
    for (char c : s) put(c, l or c == '{' or c == '}' or c == ',');
  }

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
  // o.text[b, e), without a temporary slice.
  void append(const Field& o, s::size_t b, s::size_t e) {
    text.append(o.text, b, e - b);
    lit.insert(lit.end(), o.lit.begin() + static_cast<s::ptrdiff_t>(b),
               o.lit.begin() + static_cast<s::ptrdiff_t>(e));
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
  int  array_assign = 0;       // (A) count: ${(A)x::=…} makes an array, (AA) an association
  str  z_opts;                 // (Z:opts:): c keeps comments, C strips them, n newlines as blanks
  int  quote = 0;              // q count; -1 for (q-), -2 for (q+)
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
  opt<str> escapes_g;          // (g:opts:): print's escapes
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

// The characters of a scalar.  Under MULTIBYTE a UTF-8 character counts
// as one, so ${#x}, ${x:o:l}, ${x[i]}, (s::) and padding count characters.
struct Chars {
  Chars(strv text, bool mb) : s{text} {
    if (not mb or s::none_of(s.begin(), s.end(),
                             [](char c) { return static_cast<unsigned char>(c) >= 0x80; }))
      return;
    for (s::size_t i = 0; i < s.size();) {
      at.push_back(i);
      s::size_t len = 1;
      utf8_decode(s, i, len);
      i += len;
    }
    at.push_back(s.size());
  }
  s::size_t size() const { return at.empty() ? s.size() : at.size() - 1; }
  s::size_t pos(s::size_t k) const {
    return at.empty() ? s::min(k, s.size()) : at[s::min(k, size())];
  }
  // The character that starts at byte `off`.
  s::size_t index(s::size_t off) const {
    if (at.empty()) return off;
    return static_cast<s::size_t>(s::lower_bound(at.begin(), at.end(), off) - at.begin());
  }
  // `n` characters from character `b`.
  str sub(s::size_t b, s::size_t n) const {
    s::size_t p = pos(b), q = n >= size() ? s.size() : pos(b + n);
    return str(s.substr(p, q - p));
  }

  strv           s;
  vec<s::size_t> at;   // each character's offset, then s.size(); empty when
                       // every character is one byte
};

// Pads (or truncates) `s` to `w` characters: on the left for (l), keeping
// the rightmost characters, on the right for (r), keeping the leftmost.
// `once` goes next to the word, `fill` repeats in the rest of the room.
str pad_word(const str& s, s::size_t w, const ParamFlags::Pad& p, bool left, bool mb) {
  Chars cs{s, mb};
  if (cs.size() >= w)
    return left ? cs.sub(cs.size() - w, w) : cs.sub(0, w);
  s::size_t room = w - cs.size();
  Chars co{p.once, mb};
  str once = co.size() > room
      ? (left ? co.sub(co.size() - room, room) : co.sub(0, room)) : p.once;
  s::size_t n = room - s::min(room, co.size());
  str fill = p.fill.empty() ? str(" ") : p.fill;
  s::size_t fc = Chars{fill, mb}.size();
  str rep;
  for (s::size_t have = 0; have < n; have += fc) rep += fill;
  Chars cr{rep, mb};
  rep = left ? cr.sub(cr.size() - n, n) : cr.sub(0, n);
  return left ? rep + once + s : s + once + rep;
}

// Arith is Heredoc text in which double quotes are removed, as in
// `$(( "$x" + 1 ))`; a single quote stays and the evaluator rejects it.
enum class Mode : int { Word, Scalar, Heredoc, Arith };

// zsh's special parameters as ${(t)…} shows them in a fresh shell.
struct SpecialType {
  strv name, type;
  bool readonly = false, tied = false, hide = false;
};
constexpr SpecialType SPECIAL_TYPES[] = {
  {"!", "integer", true}, {"#", "integer", true}, {"$", "integer", true},
  {"?", "integer", true}, {"ARGC", "integer", true}, {"HISTCMD", "integer", true},
  {"LINENO", "integer", true}, {"PPID", "integer", true}, {"status", "integer", true},
  {"ZSH_SUBSHELL", "integer", true}, {"TTYIDLE", "integer", true},
  {"*", "array", true}, {"@", "array", true}, {"-", "scalar", true}, {"0", "scalar"},
  {"argv", "array"}, {"pipestatus", "array"}, {"watch", "array"},
  {"SECONDS", "integer"}, {"RANDOM", "integer"}, {"EUID", "integer"},
  {"EGID", "integer"}, {"UID", "integer"}, {"GID", "integer"},
  {"HISTSIZE", "integer"}, {"SAVEHIST", "integer"}, {"OPTIND", "integer"},
  {"COLUMNS", "integer"}, {"LINES", "integer"}, {"SHLVL", "integer"},
  {"USERNAME", "scalar"}, {"HOME", "scalar"}, {"IFS", "scalar"}, {"TERM", "scalar"},
  {"PROMPT", "scalar"}, {"PS1", "scalar"}, {"PS2", "scalar"}, {"PS3", "scalar"},
  {"PS4", "scalar"}, {"HISTCHARS", "scalar"}, {"histchars", "scalar"},
  {"OPTARG", "scalar"}, {"WORDCHARS", "scalar"}, {"NULLCMD", "scalar"},
  {"READNULLCMD", "scalar"}, {"WATCH", "scalar"}, {"_", "scalar"},
  {"PATH", "scalar", false, true}, {"FPATH", "scalar", false, true},
  {"CDPATH", "scalar", false, true}, {"MANPATH", "scalar", false, true},
  {"MODULE_PATH", "scalar", false, true}, {"PSVAR", "scalar", false, true},
  {"path", "array", false, true}, {"fpath", "array", false, true},
  {"cdpath", "array", false, true}, {"manpath", "array", false, true},
  {"module_path", "array", false, true}, {"psvar", "array", false, true},
  {"ZSH_EVAL_CONTEXT", "scalar", true, true},
  {"funcstack", "array", true, false, true}, {"dirstack", "array", false, false, true},
};

Result<void> tilde_expand(Field& f, bool assign_like, ExecContext& ctx);

struct Scan {
  Expander& ex;
  Mode      mode;
  // With literal_bar: the unquoted `(` written and not yet closed.  It
  // sits in the padding after `mode`, so that a Scan, one on the stack per
  // nested substitution, does not grow.
  int       parens = 0;
  vec<Field> fields{1};
  // After ${^x}, fields[rc_from..] are the copies of the word, one per
  // element; text that follows goes onto every one of them.
  s::size_t rc_from = NONE;
  // Unquoted parameter values are pattern text, as in a subscript flag's
  // pattern: ${a[(r)$p]}.
  bool glob_values = false;
  // The operand of ${x#…} or ${x/…}: a `|` written there outside
  // parentheses is text, as zsh's lexer leaves it; one from $~var still
  // separates alternatives.
  bool literal_bar = false;
  // Scanning a word as written (not the inside of ${…}): an unquoted
  // `<(…)` / `>(…)`, or `=(…)` at its start, is a process substitution.
  bool top = false;
  // The text is the operand of a ${…} in double quotes: there `\}` is an
  // escape too, and a `"` opens a nested double-quoted region instead of
  // ending the quotes, as in zsh.
  bool brace_dq = false;
  // An operand of a word that stays one word: SH_WORD_SPLIT is off.
  bool no_shwordsplit = false;
  // An operand of ${=x:-…}: its expansions split even without SH_WORD_SPLIT.
  bool force_split = false;
  // The `=` operand of a result that will be split: no ${=…} splitting.
  bool no_eq_split = false;

  Scan(Expander& e, Mode m) : ex{e}, mode{m} {}

  ExecContext& ctx() { return ex.ctx(); }
  OptionState& opts() { return ex.ctx().opts; }

  Field& cur() { return fields.back(); }
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
    rc_from = NONE;
    fields.emplace_back();
    fields.back().quoted = quoted;
  }
  // ${^x}: the word so far is copied once per element, each copy getting
  // that element; with no elements the word is removed.
  // With `words` (an operand's own words, one per part) each copy keeps
  // its word's quoting, so a quoted empty word stays.
  void rc_splice(const vec<str>& parts, bool lit, const vec<Field>* words = nullptr) {
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
      for (s::size_t k = 0; k < parts.size(); ++k) {
        fields.push_back(b);
        if (words) {
          const Field& wd = (*words)[k];
          for (s::size_t c = 0; c < wd.text.size(); ++c) fields.back().put(wd.text[c], wd.lit[c]);
          fields.back().quoted = fields.back().quoted or wd.quoted;
        } else {
          fields.back().put(strv(parts[k]), lit);
        }
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

  // Unquoted text split on $IFS: the first piece continues the current
  // word and each further piece starts one.  An empty piece between two
  // non-whitespace separators is a word of its own.
  void splice_split(strv s, bool lit) {
    auto pieces = ifs_pieces(s, ifs());
    for (s::size_t k = 0; k < pieces.size(); ++k) {
      if (k) new_field(false);
      if (pieces[k].keep and pieces[k].text.empty()) mark_quoted();
      put(strv(pieces[k].text), lit);
    }
  }

  // -------------------------------------------------------------------------
  // The scanner proper.
  // -------------------------------------------------------------------------
  Result<void> scan(strv w, bool dq = false) {
    bool inner = false;   // in a nested "…" of a brace_dq operand
    for (s::size_t i = 0; i < w.size();) {
      char c = w[i];
      if (mode == Mode::Heredoc or mode == Mode::Arith) {
        if (mode == Mode::Arith and c == '"') { dq = not dq; ++i; continue; }
        if (c == '\\' and i + 1 < w.size()
            and (strv("$`\\\n").find(w[i + 1]) != strv::npos
                 or (dq and w[i + 1] == '"'))) {
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
          // An unclosed one (from an operand read in double quotes, where
          // zsh's lexer did not take it as a quote) is a plain character.
          auto e = w.find('\'', i + 1);
          if (e == strv::npos) { put(c, true); ++i; continue; }
          mark_quoted();
          cur().qmarks.push_back(cur().text.size());
          put(w.substr(i + 1, e - i - 1), true);
          i = e + 1;
          continue;
        }
        if (c == '"') {
          mark_quoted();
          cur().qmarks.push_back(cur().text.size());
          dq = true;
          ++i;
          continue;
        }
        if (c == '$' and i + 1 < w.size() and w[i + 1] == '\'') {
          mark_quoted();
          cur().qmarks.push_back(cur().text.size());
          s::size_t e = i + 2;
          while (e < w.size() and w[e] != '\'') e += (w[e] == '\\') ? 2u : 1u;
          put(c_escapes(w.substr(i + 2, s::min(e, w.size()) - i - 2)), true);
          i = e + 1;
          continue;
        }
      } else {
        if (c == '"' and brace_dq) { inner = not inner; mark_quoted(); ++i; continue; }
        if (c == '"') { dq = false; ++i; continue; }
        if (c == '\\' and i + 1 < w.size()
            and (strv("$`\"\\\n").find(w[i + 1]) != strv::npos
                 or (brace_dq and not inner and w[i + 1] == '}'))) {
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
      if (literal_bar and not dq) {
        if (c == '(') ++parens;
        else if (c == ')' and parens > 0) --parens;
        else if (c == '|' and parens == 0) {
          put(c, true);
          ++i;
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
      CloseTooDeep = false;
      s::size_t e = find_close(w, n);
      // Unclosed, `$(` is text; nested past the stack, an error.
      if (e == strv::npos and CloseTooDeep) return s::unexpected(unclosed_error());
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
      CloseTooDeep = false;
      s::size_t e = find_close(w, n, dq);
      if (e == strv::npos) return s::unexpected(unclosed_error());
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
    } else if (j < w.size() and s::isdigit(static_cast<unsigned char>(w[j]))) {
      // $10 is the tenth argument; under KSH_ARRAYS it is $1 then `0`.
      s::size_t s = j++;
      if (not opts().get(OPT_KSH_ARRAYS))
        while (j < w.size() and s::isdigit(static_cast<unsigned char>(w[j]))) ++j;
      expr += w.substr(s, j - s);
    } else if (j < w.size() and is_special_param(w[j])) {
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
      ex.exec().enter_child(ChildKind::Subshell);
      ::close(pfd[0]);
      if (pfd[1] != 1) {
        ::dup2(pfd[1], 1);
        ::close(pfd[1]);
      }
      // Its lines go on from the line it is on, as zsh's parse_string
      // counts on from lineno.
      ex.exec().exit_child(ex.exec().exec_string(src, false, static_cast<int>(ctx().lineno)));
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
    // A signal that came meanwhile (`$(kill -USR1 $$)`) is handled now, in
    // the expansion, as zsh does: a TRAPxxx that fails makes it fail.
    ex.exec().drop_trap_signals();   // it waited for the child
    if (ctx().signals.has_pending()) {
      // A TRAPxxx that fails ends the line, and the substitution expands to
      // nothing; it keeps its own status (zsh's cmdoutval), which an
      // assignment replaces (exec_simple).
      ex.exec().handle_signals();
      if (ctx().aborting and ctx().trap_status >= 0) out.clear();
    }
    ctx().last_exit = rc;
    ex.note_subst_status(rc);
    while (not out.empty() and out.back() == '\n') out.pop_back();
    return out;
  }

  // `dq`: the text is read as if inside double quotes.
  Result<str> expand_scalar(strv text, Mode m = Mode::Heredoc, bool dq = false) {
    // `$(( $(( … )) ))` and the like recurse through here: nesting that
    // would run the stack out fails instead.
    if (stack_low()) return s::unexpected(subst_error("substitutions nested too deeply"));
    return expand_scalar_at(text, m, dq);
  }

  Result<str> expand_scalar_at(strv text, Mode m, bool dq) {
    Scan sub{ex, m};
    sub.brace_dq = dq;
    if (auto r = sub.scan(text, dq); not r) return s::unexpected(r.error());
    str out;
    for (s::size_t k = 0; k < sub.fields.size(); ++k) {
      if (k) out += ' ';
      out += sub.fields[k].text;
    }
    return out;
  }

  // Quotes come off before the arithmetic, as in zsh: (( "5" > x )).
  // Without `expand` the text is already expanded.
  Result<MathNum> math(strv expr, bool expand = true) {
    // Text with nothing to expand (`i++`, `i < n`) is evaluated as it is.
    str expanded;
    if (expand and expr.find_first_of("\"\\$`") != strv::npos) {
      auto text = expand_scalar(expr, Mode::Arith);
      if (not text) return s::unexpected(text.error());
      expanded = s::move(*text);
      expr = expanded;
    }
    MathOptions mo = math_options(ctx());
    mo.flagged_element = [this](const str& name, const str& sub) -> Result<str> {
      return expand_scalar("${" + name + "[" + sub + "]}", Mode::Scalar);
    };
    return math_eval(expr, ctx().params, mo, &ctx().modules.math_funcs());
  }

  Result<s::int64_t> arith(strv expr) {
    auto v = math(expr);
    if (not v) return s::unexpected(v.error());
    return v->as_int();
  }

  // A subscript index: as zsh's getindex, the expanded text is evaluated
  // only up to its first top-level comma (`i=2,3; ${a[$i]}` is ${a[2]}).
  Result<s::int64_t> index(strv expr) {
    auto text = expand_scalar(expr, Mode::Arith);
    if (not text) return s::unexpected(text.error());
    int depth = 0;
    for (s::size_t k = 0; k < text->size(); ++k) {
      char c = (*text)[k];
      if (c == '(' or c == '[') ++depth;
      else if (c == ')' or c == ']') --depth;
      else if (c == ',' and depth == 0) { text->resize(k); break; }
    }
    auto v = math_eval(*text, ctx().params, math_options(ctx()), &ctx().modules.math_funcs());
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
  struct ParamResult {
    Value v;
    bool  keep_separate = false;   // "$@", "${a[@]}", "${(@)a}"
    bool  split         = false;
    bool  glob          = false;
    bool  rc_expand     = false;   // ${^x}, RC_EXPAND_PARAM
  };

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
      vec<str> out;
      for (auto* kv : sorted_entries(param.hash_ref())) {
        if (fl.keys) out.push_back(kv->first);
        if (fl.values or not fl.keys) out.push_back(kv->second);
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
    bool set = p and p->get().is_set();
    const SpecialType* sp = nullptr;
    for (const auto& e : SPECIAL_TYPES) if (e.name == name) { sp = &e; break; }
    // The positional and status parameters are always set.
    bool always = sp and (name.size() == 1 or name == "ARGC" or name == "argv"
                          or name == "status" or name == "pipestatus");
    if (not set and not always) return {};
    if (not sp) return param_type_text(p->get());
    const Param* param = set ? &p->get() : nullptr;
    str t(sp->type);
    if (param and param->flags & PM_LOCAL)                  t += "-local";
    if (sp->readonly or (param and param->is_readonly()))   t += "-readonly";
    if (sp->tied)                                           t += "-tied";
    if (param and param->is_exported())                     t += "-export";
    if (sp->hide)                                           t += "-hide-hideval";
    t += "-special";
    return t;
  }

  // The value of a nested ${…}: one that ${=x} (or SH_WORD_SPLIT) splits
  // is split here, before the outer level's subscripts and flags.
  Value nested_value(const ParamResult& r, bool dq) {
    Value v = r.v;
    if (not r.split or dq or not v.set) return v;
    str text = v.scalar;
    if (v.array) {
      str sep = ifs_first();
      text.clear();
      for (s::size_t k = 0; k < v.arr.size(); ++k) text += (k ? sep : str{}) + v.arr[k];
    }
    v.make_array(ifs_split(text, ifs()));
    return v;
  }

  // The `)` that ends the flags opened at t[open]: as zsh's flag parser,
  // the delimited arguments of (j) (s) (l) (r) are skipped whole, so a
  // quote or parenthesis in one is just text: ${(s:):)x}, "${(j:':)a}".
  static s::size_t flags_end(strv t, s::size_t open) {
    auto skip_arg = [&](s::size_t k) -> s::size_t {   // k at the opening delimiter
      if (k >= t.size()) return strv::npos;
      char o = t[k];
      char c = o == '(' ? ')' : o == '[' ? ']' : o == '{' ? '}' : o == '<' ? '>' : o;
      auto e = t.find(c, k + 1);
      return e;
    };
    for (s::size_t k = open + 1; k < t.size(); ++k) {
      char c = t[k];
      if (c == ')') return k;
      if (c == 'j' or c == 's' or c == 'Z') {
        k = skip_arg(k + 1);
        if (k == strv::npos) return k;
      } else if (c == 'l' or c == 'r') {
        // A width, then optional fill and once arguments with the same
        // delimiter.
        if (k + 1 >= t.size()) return strv::npos;
        char o = t[k + 1];
        k = skip_arg(k + 1);
        for (int more = 0; more < 2 and k != strv::npos and k + 1 < t.size() and t[k + 1] == o; ++more)
          k = skip_arg(k + 1);
        if (k == strv::npos) return k;
      }
    }
    return strv::npos;
  }

  Result<ParamFlags> parse_flags(strv f) {
    ParamFlags fl;
    for (s::size_t i = 0; i < f.size(); ++i) {
      char c = f[i];
      auto delimited = [&]() -> Result<str> {
        // zsh takes no `{` as a delimiter: it would end the ${.
        if (i + 1 < f.size() and f[i + 1] == '{')
          return s::unexpected(subst_error("error in flags"));
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
        break; case '0': fl.split = true; fl.split_sep = str(1, '\0');
        break; case 'A': ++fl.array_assign;
        break; case 'Z': {
          auto a = delimited();
          if (not a) return s::unexpected(a.error());
          if (a->find_first_not_of("cCn") != str::npos)
            return s::unexpected(subst_error("error in flags"));
          // zsh splits nothing with an empty option list.
          fl.split_words = not a->empty();
          fl.z_opts = *a;
        }
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
      auto e = dquote_end(t, i);
      return e == strv::npos ? t.size() : e + 1;
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
  // `split`: the substitution splits its result (${=x:-…}); `assign`:
  // the operand of `=`, whose value is assigned as one string.
  // `no_eq`: no ${=…} splitting inside (the `=` operand of a split result).
  Result<Value> operand(strv word, bool dq, bool split = false, bool assign = false,
                        bool no_eq = false) {
    // zsh expands the operand as words (multsub) even in a word that stays
    // one word: `y=${z:-${(s:,:)x}}` splits, and the words are joined later.
    Scan sub{ex, Mode::Word};
    sub.brace_dq = dq;
    // There SH_WORD_SPLIT does not split a plain $x; ${=x} still does.
    sub.no_shwordsplit = no_shwordsplit or mode != Mode::Word or assign;
    // ${=x:-$v}: the operand's expansions split as for SH_WORD_SPLIT.
    sub.force_split = force_split or split;
    // Only at the operand's own level: deeper operands split as usual.
    sub.no_eq_split = no_eq;
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
    sub.literal_bar = true;
    if (auto r = sub.scan(word, dq); not r) return s::unexpected(r.error());
    str pat;
    for (s::size_t k = 0; k < sub.fields.size(); ++k) {
      if (k) pat += ' ';
      pat += sub.fields[k].as_pattern();
    }
    auto cp = CompiledPattern::compile(pat, false, opts().extended_glob(), opts().multibyte());
    if (cp.bad()) return s::unexpected(bad_pattern(pat));
    return cp;
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

  // The parameter that `ref`, the value of a (P) substitution, names; an
  // invalid name is unset.  A subscript in it is evaluated as arithmetic
  // or a key, but one that would expand anything (`a[$(cmd)]`) is not
  // taken, unlike in zsh, so a value never runs a command.
  Result<Value> indirect(strv ref, const ParamFlags& fl, str& name, ParamResult& res) {
    s::size_t j = 0;
    if (j < ref.size() and is_name_start(ref[j])) {
      while (j < ref.size() and is_name_char(ref[j])) ++j;
    } else if (j < ref.size() and s::isdigit(static_cast<unsigned char>(ref[j]))) {
      while (j < ref.size() and s::isdigit(static_cast<unsigned char>(ref[j]))) ++j;
    } else if (j < ref.size() and is_special_param(ref[j])) {
      ++j;
    }
    name = str(ref.substr(0, j));
    Value v;
    if (j == 0) return v;
    for (s::size_t k = j; k < ref.size(); ++k)
      if (ref[k] == '$' or ref[k] == '`') return v;
    if (fl.type and j == ref.size()) {
      v.set = true;
      v.make_scalar(type_of(name));
      return v;
    }
    if (auto ev = element_view(name, ref, j)) {
      if (not *ev) return s::unexpected(ev->error());
      v = s::move(**ev);
    } else {
      v = get_param(name, fl);
    }
    if (name == "@") res.keep_separate = true;
    // KSH_ARRAYS: an array named without a subscript is its first element.
    if (j == ref.size() and opts().get(OPT_KSH_ARRAYS) and v.array
        and name != "@" and name != "*") {
      str first = v.arr.empty() ? str{} : v.arr[0];
      v.make_scalar(s::move(first));
    }
    while (j < ref.size()) {
      s::size_t e = ref[j] == '[' ? find_close(ref, j) : strv::npos;
      if (e == strv::npos) return Value{};
      bool all = false;
      if (auto r = apply_subscript(v, ref.substr(j + 1, e - j - 1), name, all); not r)
        return s::unexpected(r.error());
      if (all) res.keep_separate = true;
      j = e + 1;
    }
    return v;
  }

  // An external 1-based (zsh) or 0-based (KSH_ARRAYS) subscript as a
  // 1-based internal index.  Below any first slot comes back as 0 or less
  // so the callers can still clamp uniformly.
  s::int64_t norm_index(s::int64_t i, s::int64_t n) {
    if (opts().get(OPT_KSH_ARRAYS)) {
      if (i < 0) i += n;
      return i + 1;
    }
    if (i == 0 and opts().get(OPT_KSH_ZERO_SUBSCRIPT)) return 1;
    return i < 0 ? n + i + 1 : i;
  }

  // ${a[i]} or ${h[key]} on an ordinary array or association: the one
  // element, read in place rather than from a copy of the whole value.
  // Null when the subscript is not a single index or key.
  opt<Result<Value>> element_in_place(strv name, const ParamFlags& fl, strv t, s::size_t i) {
    if (i >= t.size() or t[i] != '[') return s::nullopt;
    auto p = ctx().params.lookup(name);
    if (not p or not p->get().is_set() or not (p->get().is_array() or p->get().is_hash()))
      return s::nullopt;
    bool hash = p->get().is_hash();
    s::size_t e = find_close(t, i);
    if (e == strv::npos) return s::nullopt;
    strv sub = t.substr(i + 1, e - i - 1);
    if (sub.empty() or sub == "@" or sub == "*" or sub[0] == '(') return s::nullopt;
    if (not hash) {
      int depth = 0;
      for (char c : sub) {
        if (c == '(' or c == '[') ++depth;
        else if (c == ')' or c == ']') --depth;
        else if (c == ',' and depth == 0) return s::nullopt;
      }
    }
    Value v;
    bool key_only = fl.keys and not fl.values;
    if (hash) {
      auto key = expand_scalar(sub, Mode::Scalar);
      if (not key) return Result<Value>(s::unexpected(key.error()));
      // Expanding the key may have changed the table: look again.
      auto q = ctx().params.lookup(name);
      if (not q or not q->get().is_hash()) return Result<Value>(s::move(v));
      const auto& h = q->get().hash_ref();
      if (auto it = h.find(*key); it != h.end()) {
        v.set = true;
        v.make_scalar(key_only ? *key : it->second);
      }
      return Result<Value>(s::move(v));
    }
    auto idx = index(sub);
    if (not idx) return Result<Value>(s::unexpected(idx.error()));
    auto q = ctx().params.lookup(name);
    if (not q or not q->get().is_array()) return Result<Value>(s::move(v));
    const auto& arr = q->get().array_ref();
    s::int64_t n = static_cast<s::int64_t>(arr.size());
    s::int64_t a = norm_index(*idx, n);
    if (a >= 1 and a <= n) {
      v.set = true;
      v.make_scalar(key_only ? s::to_string(*idx) : arr[static_cast<s::size_t>(a - 1)]);
    }
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
      auto q = ctx().params.lookup(name);
      static const umap<str, str> NO_HASH;
      const auto& h = q and q->get().is_hash() ? q->get().hash_ref() : NO_HASH;
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
      if (text.empty() or text[0] != '(') return index(text);
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
    bool ksh  = opts().get(OPT_KSH_ARRAYS);
    auto norm = [&](s::int64_t i, s::int64_t n) { return norm_index(i, n); };
    if (v.array) {
      s::int64_t n = static_cast<s::int64_t>(v.arr.size());
      s::int64_t a = norm(*lo, n);
      if (not hi) {
        if (a >= 1 and a <= n) v.make_scalar(v.arr[static_cast<s::size_t>(a - 1)]);
        else                   { v.make_scalar(""); v.set = false; }
        return {};
      }
      s::int64_t b = s::min(norm(*hi, n), n);
      // A start before the first element selects nothing, unlike on a
      // scalar, where it is clamped.
      if (a < 1 and (*lo < 0 or ksh)) b = 0;
      a = s::max<s::int64_t>(a, 1);
      vec<str> out;
      for (s::int64_t k = a; k <= b; ++k) out.push_back(v.arr[static_cast<s::size_t>(k - 1)]);
      v.make_array(s::move(out));
      return {};
    }
    Chars cs{v.scalar, opts().multibyte()};
    s::int64_t n = static_cast<s::int64_t>(cs.size());
    s::int64_t a = norm(*lo, n);
    s::int64_t b = hi ? norm(*hi, n) : a;
    a = s::max<s::int64_t>(a, 1);
    b = s::min(b, n);
    v.make_scalar(a > b ? str{}
                        : cs.sub(static_cast<s::size_t>(a - 1),
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
    auto pat = CompiledPattern::compile(pat_text, false, extended, opts().multibyte());
    auto test = [&](const str& s) { return exact ? s == text : pat.matches(s); };

    // Associations.
    if (auto p = ctx().params.lookup(name); not name.empty() and p and p->get().is_hash()) {
      const auto& h = p->get().hash_ref();
      vec<const str*> keys;
      for (auto& [key, _] : h) keys.push_back(&key);
      s::sort(keys.begin(), keys.end(), [](const str* a, const str* b) { return *a < *b; });
      vec<str> out;
      for (const str* key : keys) {
        const str& val = h.at(*key);
        bool hit = kind == 'k' or kind == 'K'
            ? (exact ? *key == text
                     : CompiledPattern::compile(*key, false, extended, opts().multibyte()).matches(text))
            : test(kind == 'i' or kind == 'I' ? *key : val);
        if (not hit) continue;
        if (kind == 'i' or kind == 'I') {
          out.push_back(*key);
        } else {
          if (want_keys) out.push_back(*key);
          if (want_values or not want_keys) out.push_back(val);
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
    Chars cs{v.scalar, opts().multibyte()};
    s::int64_t n = static_cast<s::int64_t>(v.array ? v.arr.size() : cs.size());
    auto hit_at = [&](s::int64_t k) {   // k is 1-based
      if (v.array) {
        const str& e = v.arr[static_cast<s::size_t>(k - 1)];
        return kind == 'k' or kind == 'K'
            ? CompiledPattern::compile(e, false, extended, opts().multibyte()).matches(text)
            : test(e);
      }
      // As zsh's getarg: each tail is a string of its own, so (#s) is
      // where it starts.
      strv rest = strv(v.scalar).substr(cs.pos(static_cast<s::size_t>(k - 1)));
      return exact ? rest.starts_with(text) : pat.match_prefix(rest, true) != strv::npos;
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
    else              v.make_scalar(cs.sub(static_cast<s::size_t>(found - 1), 1));
    return {};
  }

  // :h and :t, with an optional count: `n` leading or trailing path
  // components.  Trailing slashes are ignored.
  static str path_head(const str& s, s::size_t n) {
    s::size_t e = s.size();
    while (e > 0 and s[e - 1] == '/') --e;
    if (n == 0) {
      if (e == 0) return s.empty() ? "." : "/";
      auto sl = s.rfind('/', e - 1);
      if (sl == str::npos) return ".";
      while (sl > 0 and s[sl - 1] == '/') --sl;
      return sl == 0 ? "/" : s.substr(0, sl);
    }
    if (e == 0) return s.empty() ? s : "/";
    s::size_t i = 0, count = 0;
    if (s[0] == '/') { i = 1; count = 1; }
    while (count < n and i < s.size()) {
      while (i < s.size() and s[i] == '/') ++i;
      if (i == s.size()) break;
      while (i < s.size() and s[i] != '/') ++i;
      ++count;
    }
    if (count < n or s.find_first_not_of('/', i) == str::npos) return s;
    return s.substr(0, i);
  }
  static str path_tail(const str& s, s::size_t n) {
    s::size_t e = s.size();
    while (e > 0 and s[e - 1] == '/') --e;
    if (e == 0) return {};
    s::size_t b = e;
    for (s::size_t k = 0; k < s::max<s::size_t>(n, 1); ++k) {
      if (k) {
        while (b > 0 and s[b - 1] == '/') --b;
        if (b == 0) return s.substr(0, e);
      }
      auto sl = s.rfind('/', b - 1);
      if (sl == str::npos) return s.substr(0, e);
      b = sl + 1;
    }
    return s.substr(b, e - b);
  }

  static str modify(str s, char m, s::size_t count, bool mb) {
    switch (m) {
      case 'h': return path_head(s, count);
      case 't': return path_tail(s, count);
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
      case 'u': return map_case(s, 'U', mb);
      case 'l': return map_case(s, 'L', mb);
      case 'q': return quote_backslash(s, mb);
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
        // The left side is plain text unless HIST_SUBST_PATTERN is set; even
        // then a `|` outside parentheses is text, as in ${x/…}.
        str lit;
        int depth = 0;
        bool pattern = opts().get(OPT_HISTSUBSTPATTERN);
        for (s::size_t k = 0; k < pat_text.size(); ++k) {
          char ch = pat_text[k];
          if (pattern and ch == '\\' and k + 1 < pat_text.size()) {
            lit += ch;
            lit += pat_text[++k];
            continue;
          }
          if (ch == '(') ++depth;
          else if (ch == ')' and depth > 0) --depth;
          if (not pattern ? PATTERN_CHARS.find(ch) != strv::npos : ch == '|' and depth == 0)
            lit += '\\';
          lit += ch;
        }
        pat_text = s::move(lit);
        bool mb = opts().multibyte();
        auto cp = CompiledPattern::compile(pat_text, false,
                                           opts().get(OPT_EXTENDEDGLOB), mb);
        if (cp.bad()) return s::unexpected(bad_pattern(pat_text));
        bool g = global;
        each(v, [&](const str& s) {
          return replace(s, cp, repl_text, g, /*anchor=*/0, mb);
        });
        continue;
      }
      if (strv("htreulqQaAcP").find(m) == strv::npos)
        return s::unexpected(
            subst_error("unrecognized modifier `" + str(1, m) + "'"));
      mods.remove_prefix(2);
      // :h2, :t3: a count of path components.
      s::size_t count = 0;
      if (m == 'h' or m == 't')
        while (not mods.empty() and s::isdigit(static_cast<unsigned char>(mods[0]))) {
          count = s::min<s::size_t>(count * 10 + static_cast<s::size_t>(mods[0] - '0'), 1u << 20);
          mods.remove_prefix(1);
        }
      if (m == 'c') {
        // :c — the command's path, as `=cmd` would give.
        each(v, [&](const str& s) { return ex.exec().find_command(s).value_or(s); });
      } else {
        bool mb = opts().multibyte();
        each(v, [&](const str& s) { return modify(s, m, count, mb); });
      }
    }
    if (not mods.empty()) return s::unexpected(subst_error("bad substitution"));
    return {};
  }

  template <typename F>
  static void each(Value& v, F fn) {
    if (v.array) for (auto& e : v.arr) e = fn(e);
    else         v.scalar = fn(v.scalar);
  }

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

  // How zsh's igetmatch looks for a match: its SUB_END (at the end),
  // SUB_LONG, SUB_SUBSTR (anywhere: (S), and ${x/…}) and SUB_GLOBAL flags.
  struct MatchHow {
    bool end = false, longest = false, substr = false, global = false;
  };
  // A match of s[b, e), `b_char` characters into s.
  using HitFn = func<Result<void>(s::size_t b, s::size_t e, s::size_t b_char,
                                  const PatternMatch& pm)>;

  // zsh's igetmatch: finds the match of `pat` in `s` that ${x#pat},
  // ${x%pat} and ${x/pat/…} take (with `global` each one, left to right)
  // and calls `hit` for it.  `nth` is (I:n:).  Matches are tried at
  // character starts only.  False when there is none.  Its quirks are
  // kept: (S) finds an empty match at the end first and reports it at the
  // start (${(SB)x#(#e)} is 1), after it has looked for a shortest match
  // (#e) fails, and in (S)% and % some searches keep the (#s) and the
  // character index of another start.
  static Result<bool> find_match(const str& s, const CompiledPattern& pat, MatchHow how,
                                 s::int64_t nth, bool mb, const HitFn& hit) {
    if (not pat.approximate()) {
      PrefixSearch search;
      return scan_match(s, pat, how, nth, mb, hit, search);
    }
    // With (#aN) a backtracker that gave up anywhere leaves the answer
    // unknown: no match, the calls after it skipped, and `hit` called only
    // once the scan is done.
    struct Hit {
      s::size_t    b, e, k;
      PatternMatch pm;
    };
    vec<Hit> hits;
    PrefixSearch search;
    auto r = scan_match(s, pat, how, nth, mb,
                        [&](s::size_t b, s::size_t e, s::size_t k,
                            const PatternMatch& pm) -> Result<void> {
                          hits.push_back({b, e, k, pm});
                          return {};
                        },
                        search);
    if (search.lost) return false;
    if (not r) return r;
    for (const auto& h : hits)
      if (auto x = hit(h.b, h.e, h.k, h.pm); not x) return s::unexpected(x.error());
    return r;
  }

  // find_match()'s search; `search` is shared by its calls: (#e) off after
  // a shortest search, and whether the backtracker gave up.
  static Result<bool> scan_match(const str& s, const CompiledPattern& pat, MatchHow how,
                                 s::int64_t nth, bool mb, const HitFn& hit,
                                 PrefixSearch& search) {
    Chars cs{s, mb};
    s::size_t size = s.size(), chars = cs.size();
    s::int64_t n = s::max<s::int64_t>(nth, 1);
    PatternMatch pm;
    // Whether the last from() found a shorter match than the first one.
    bool shorter = false;
    // The length of the match from character k: the end zsh's search
    // reaches first, or the shortest, which zsh looks for with (#e) off
    // and (#s) as `notstart` says.
    auto from = [&](s::size_t k, bool notstart = false) {
      s::size_t b = cs.pos(k);
      shorter = false;
      search.notstart = false;
      // With the same flags as the search for the end zsh's reaches first,
      // the shortest one exists where that does and ends no later: without
      // groups to take from the first, it answers alone.
      auto cut_search = [&] {
        PrefixSearch cut = search;
        cut.notstart = notstart;
        cut.notend   = true;
        return cut;
      };
      if (not how.longest and search.notend and not notstart and not pat.backrefs()
          and pat.ordered_ends()) {
        PrefixSearch cut = cut_search();
        return pat.match_prefix(s, b, false, &pm, &cut);
      }
      s::size_t len = pat.match_prefix(s, b, true, &pm, &search);
      if (len == str::npos or how.longest or len == 0) return len;
      PatternMatch shortest;
      PrefixSearch cut = cut_search();
      cut.below = b + len;
      s::size_t min = pat.match_prefix(s, b, false, &shortest, &cut);
      search.notend  = true;
      search.gave_up = cut.gave_up;
      search.lost    = cut.lost;
      if (min == str::npos or min >= len) return len;
      pm = s::move(shortest);
      shorter = true;
      return min;
    };
    // The first start from byte b before `until`, or with `last` the last,
    // from which there is a match (to the end with `whole`), npos when none.
    auto first_start = [&](s::size_t b, s::size_t until, bool whole, bool last) {
      return pat.find_start(s, b, until, whole, last, search, whole ? &pm : nullptr);
    };
    auto empty_at_end = [&] {
      search.notstart = false;
      return pat.match_prefix(s, size, true, &pm, &search) == 0;
    };
    auto found = [&](s::size_t b, s::size_t e, s::size_t k) -> Result<bool> {
      if (search.lost) return false;
      if (auto r = hit(b, e, k, pm); not r) return s::unexpected(r.error());
      return true;
    };
    auto whole = [&](s::size_t b, PatternMatch* info) {
      return pat.matches(s, b, size, info, search);
    };
    if (not how.end and not how.substr) {
      s::size_t len = from(0);
      if (len == str::npos) return false;
      return found(0, len, 0);
    }
    if (not how.substr) {
      // Anchored at the end: the shortest is from the last start that
      // matches, the longest from the first.
      if (not how.longest) {
        // zsh gives its tries at the end the byte length as the index.
        if (whole(size, &pm) and --n == 0) return found(size, size, size);
        if (s::size_t b = first_start(0, size, true, true); b != str::npos)
          return found(b, size, cs.index(b));
        // zsh tries the empty match at the end again with the (#s) of the
        // last start, so on one character (#s) matches there.
        bool at_start = chars <= 1;
        if (search.lost) return false;
        if (at_start ? pat.matches(strv(s).substr(size), &pm) : whole(size, &pm))
          return found(size, size, chars);
        return false;
      }
      s::size_t b = first_start(0, size + 1, true, false);
      if (b == str::npos) return false;
      return found(b, size, cs.index(b));
    }
    if (how.end) {
      // (S)% and (S)%%: the nth start from the end that has a match, or
      // with fewer the last one.
      if (empty_at_end() and --n == 0) return found(size, size, size);
      opt<s::size_t> last, nth_last;
      s::int64_t count = 0;
      for (s::size_t until = size; not search.lost;) {
        s::size_t b = first_start(0, until, false, true);
        if (b == str::npos) break;
        s::size_t k = cs.index(b);
        if (not last) last = k;
        if (++count == n) {
          nth_last = k;
          break;
        }
        until = b;
      }
      if (last) {
        // zsh's shortest search here keeps the (#s) and the character
        // index of the last start it tried: the match, when a second pass
        // found the nth, else the end.
        s::size_t k = nth_last.value_or(*last);
        bool second = n > 1 and nth_last;
        s::size_t len = from(k, (second ? k : chars - 1) != 0);
        return found(cs.pos(k), cs.pos(k) + len, shorter and not second ? chars : k);
      }
      if (how.longest and empty_at_end() and --n == 0) return found(size, size, size);
      return false;
    }
    if (not how.longest and not how.global and empty_at_end() and --n == 0)
      return found(0, 0, 0);
    bool any = false, matched = false;
    s::size_t k = 0;
    do {
      matched = false;
      for (int missed = 0; k <= chars and not search.lost;) {
        s::size_t len = from(k);
        if (len != str::npos) {
          missed = 0;
          s::size_t b = cs.pos(k), mpos = b + len;
          if (--n == 0 or (n <= 0 and how.global)) {
            if (auto r = found(b, mpos, k); not r) return r;
            any = true;
            // After an empty match the character there is kept.
            if (len == 0 and k < chars) mpos = cs.pos(k + 1);
          }
          if (not how.global) {
            if (n > 0) {
              ++k;
              continue;
            }
            return true;
          }
          matched = true;
          if (k == chars) break;
          while (k < chars and cs.pos(k) < mpos) ++k;
          break;
        }
        if (k == chars) break;
        // After a few starts without a match (each a short run, mostly),
        // the next one with a match is found at once.
        if (++missed < 8) {
          ++k;
          continue;
        }
        s::size_t b = first_start(cs.pos(k + 1), size + 1, false, false);
        if (b == str::npos) break;
        k = cs.index(b);
      }
    } while (matched and k < chars and not search.lost);
    if (search.lost) return false;
    if (how.longest and not how.global and empty_at_end() and --n == 0) return found(0, 0, 0);
    return any;
  }

  // zsh's getmatch for ${x#pat} ${x##pat} ${x%pat} ${x%%pat}: the match,
  // anchored at the start (or, for `end`, the end) or with (S) anywhere;
  // then what the flags ask for: (M) the match, (R) the rest (the
  // default), (B) (E) (N) its start, end and length.  No match is an
  // empty one at the start.  (B) (E) (N) count characters.  `hit`, when
  // given, is called for the match.
  static str get_match(const str& s, const CompiledPattern& pat, bool end, bool longest,
                       const ParamFlags& fl, s::int64_t nth, bool multibyte,
                       const HitFn* hit = nullptr) {
    Chars cs{s, multibyte};
    s::size_t n = cs.size(), mb = 0, me = 0;
    MatchHow how;
    how.end     = end;
    how.longest = longest;
    how.substr  = fl.m_substr;
    (void)find_match(s, pat, how, nth, multibyte,
                     [&](s::size_t b, s::size_t e, s::size_t b_char,
                         const PatternMatch& pm) -> Result<void> {
                       mb = b;
                       me = e;
                       if (hit) return (*hit)(b, e, b_char, pm);
                       return {};
                     });
    // As zsh's get_match_ret, the parts asked for are separated by a
    // space even when empty.
    str out;
    bool any = false;
    auto part = [&](strv t) {
      if (any) out += ' ';
      out += t;
      any = true;
    };
    bool rest = fl.m_rest or not (fl.match_mode or fl.m_begin or fl.m_end or fl.m_len);
    if (fl.match_mode) part(strv(s).substr(mb, me - mb));
    if (rest) part(s.substr(0, mb) + s.substr(me));
    // The character a byte offset falls in.
    auto char_at = [&](s::size_t off) {
      s::size_t k = 0;
      while (k < n and cs.pos(k + 1) <= off) ++k;
      return off >= s.size() ? n : k;
    };
    if (fl.m_begin) part(s::to_string(char_at(mb) + 1));
    if (fl.m_end) part(s::to_string(char_at(me) + 1));
    if (fl.m_len) part(s::to_string(char_at(me) - char_at(mb)));
    return out;
  }

  // The replacement for a match of s[b, b + len), `b_char` characters into
  // s, whose backreferences are in the PatternMatch.
  using ReplaceFn = func<Result<str>(s::size_t, s::size_t, s::size_t, const PatternMatch&)>;

  // zsh's igetmatch for ${x/pat/r}: `anchor` is 'w' (the whole value),
  // '#' (the start), '%' (the end) or 0 (anywhere); `shortest` is (S),
  // `nth` (I:n:) skips the matches before it.
  static Result<str> substitute(const str& s, const CompiledPattern& pat, bool global,
                                char anchor, bool shortest, s::int64_t nth, bool mb,
                                const ReplaceFn& repl) {
    if (anchor == 'w') {
      PatternMatch pm;
      if (not pat.matches(s, &pm)) return s;
      return repl(0, s.size(), 0, pm);
    }
    MatchHow how;
    how.longest = not shortest;
    how.global  = global;
    how.end     = anchor == '%';
    how.substr  = anchor == 0;
    str out;
    s::size_t done = 0;
    auto r = find_match(s, pat, how, nth, mb,
                        [&](s::size_t b, s::size_t e, s::size_t b_char,
                            const PatternMatch& pm) -> Result<void> {
                          auto t = repl(b, e - b, b_char, pm);
                          if (not t) return s::unexpected(t.error());
                          out.append(s, done, b - done);
                          out += *t;
                          done = e;
                          return {};
                        });
    if (not r) return s::unexpected(r.error());
    if (not *r) return s;
    out.append(s, done);
    return out;
  }

  // The replacement text of ${x/pat/repl}, expanded.  In double quotes it
  // is read as double-quoted text (the pattern is not, so `"${x#*/}"`
  // still matches); unquoted, a leading `~` expands: ${x/b/~} gives a$HOME…
  Result<str> replacement(strv text, bool dq) {
    if (dq) return expand_scalar(text, Mode::Scalar, dq);
    Scan sub{ex, Mode::Scalar};
    if (auto r = sub.scan(text); not r) return s::unexpected(r.error());
    Field all;
    for (s::size_t k = 0; k < sub.fields.size(); ++k) {
      if (k) all.put(' ', true);
      all.append(sub.fields[k]);
    }
    if (auto r = tilde_expand(all, false, ctx()); not r) return s::unexpected(r.error());
    return s::move(all.text);
  }

  // substitute() for a pattern with (#b) or (#m): $match … are set to
  // each match before `repl_text` is expanded for it.
  Result<str> replace_each(const str& s, const CompiledPattern& pat, strv repl_text, bool dq,
                           bool global, char anchor, bool shortest, s::int64_t nth) {
    bool ksh = opts().get(OPT_KSH_ARRAYS);
    return substitute(s, pat, global, anchor, shortest, nth, opts().multibyte(),
                      [&](s::size_t b, s::size_t len, s::size_t b_char,
                          const PatternMatch& pm) -> Result<str> {
                        set_match_params(ctx().params, pat, s, b, len, pm, ksh, b_char);
                        return replacement(repl_text, dq);
                      });
  }

  // substitute() with a fixed replacement.
  static str replace(const str& s, const CompiledPattern& pat,
                     const str& repl, bool global, char anchor, bool mb,
                     bool shortest = false, s::int64_t nth = 1) {
    auto r = substitute(s, pat, global, anchor, shortest, nth, mb,
                        [&](s::size_t, s::size_t, s::size_t, const PatternMatch&) -> Result<str> {
                          return repl;
                        });
    return r ? *r : s;
  }

  // ${…}: `t` is the text between the braces.
  Result<void> param(strv t, bool dq) {
    // At the top of the `=` operand of a result that is to be split, zsh
    // (PREFORK_SINGLE) also skips (s) and (f).
    auto r = param_value(t, dq, mode != Mode::Word or no_eq_split);
    if (not r) return s::unexpected(r.error());
    auto& [v, keep_separate, split, glob, rc_expand] = *r;
    if (mode != Mode::Word) {
      // Only a pattern (`case`, `[[ = ]]`) looks at what is literal: there
      // ${~x} and GLOB_SUBST make the value's characters active.
      bool literal = dq or not (glob or glob_values);
      if (v.array) {
        str sep = ifs_first(), joined;
        for (s::size_t k = 0; k < v.arr.size(); ++k) {
          if (k) joined += sep;
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
    // ${^z:=…} under SH_WORD_SPLIT, ${^=v}: a scalar value that splits
    // into words makes their product (an operand's own words, and a value
    // with no words, stay as before).
    if (rc_expand and not v.array and split and not v.words_current()) {
      // Every piece, the empty ones that leading or trailing IFS
      // whitespace leaves included, as zsh keeps them under ${^…}.
      vec<str> parts;
      for (auto& p : ifs_pieces(v.scalar, ifs())) parts.push_back(s::move(p.text));
      if (not parts.empty()) {
        rc_splice(parts, lit);
        return {};
      }
    }
    rc_expand = rc_expand and v.array;
    if (not rc_expand and split and v.words_current()) {
      // ${=x:-word}, or SH_WORD_SPLIT: the operand's words split at their
      // unquoted IFS characters (an expansion's value was split when the
      // operand was scanned); each piece keeps its quoting and pattern
      // characters, so `a\ b` stays one word and `a*` still globs.
      str sep = ifs();
      bool first = true;
      for (auto& wd : v.words) {
        s::size_t n = wd.text.size(), k = 0;
        auto is_sep = [&](s::size_t i) {
          return not wd.lit[i] and sep.find(wd.text[i]) != str::npos;
        };
        auto emit = [&](s::size_t from, s::size_t to, bool quoted) {
          if (not first) new_field(quoted);
          if (quoted) mark_quoted();
          for (s::size_t c = from; c < to; ++c) {
            char ch = wd.text[c];
            bool l = wd.lit[c];
            put(ch, glob ? l and strv("{},").find(ch) != strv::npos : l);
          }
          first = false;
        };
        // A quoted string that began at q belongs to the piece [from, to]
        // around it; one inside a run of separators is an empty word.
        auto quoted_in = [&](s::size_t from, s::size_t to) {
          return s::any_of(wd.qmarks.begin(), wd.qmarks.end(),
                           [&](s::size_t q) { return q >= from and q <= to; });
        };
        bool any = false;
        while (k < n or (not any and k == n)) {
          s::size_t run = k;
          while (k < n and is_sep(k)) ++k;
          // Empty quoted words inside the separators [run, k).
          s::size_t last = str::npos;
          for (s::size_t q : wd.qmarks)
            if (q != last and (q > run or (q == run and run == 0))
                and (q < k or (q == k and k == n))) {
              emit(q, q, true);
              any = true;
              last = q;
            }
          if (k >= n) break;
          s::size_t e = k;
          while (e < n and not is_sep(e)) ++e;
          emit(k, e, wd.quoted and quoted_in(k, e));
          any = true;
          k = e;
        }
        if (not any and wd.quoted) {   // a quoted empty word stays
          emit(0, 0, true);
        }
      }
      return {};
    }
    if (not rc_expand and v.words_current()) {
      for (s::size_t k = 0; k < v.words.size(); ++k) {
        if (k) new_field(v.words[k].quoted);
        if (v.words[k].quoted) mark_quoted();
        // ${~x:-word} makes the word's characters pattern syntax; its
        // braces and commas stay as written.
        for (s::size_t c = 0; c < v.words[k].text.size(); ++c) {
          char ch = v.words[k].text[c];
          bool l = v.words[k].lit[c];
          put(ch, glob ? l and strv("{},").find(ch) != strv::npos : l);
        }
      }
      return {};
    }
    if (v.array and split and not rc_expand) {
      // zsh joins the elements, then splits the result.
      str sep = ifs_first(), joined;
      for (s::size_t k = 0; k < v.arr.size(); ++k) {
        if (k) joined += sep;
        joined += v.arr[k];
      }
      splice_split(joined, lit);
      return {};
    }
    if (v.array and no_eq_split) {
      // Inside the `=` operand of a result that is to be split (zsh's
      // PREFORK_SINGLE) an array is its elements joined, empty ones
      // included, with no ${^…} product.
      str sep = ifs_first(), joined;
      for (s::size_t k = 0; k < v.arr.size(); ++k) joined += (k ? sep : str{}) + v.arr[k];
      put(strv(joined), lit);
      return {};
    }
    if (v.array) {
      // Empty elements stay: one glued to text is part of a word, and an
      // empty word on its own goes away later.
      vec<str> parts = v.arr;
      if (split) {
        str sep = ifs();
        vec<str> words;
        for (auto& e : parts)
          for (auto& w : ifs_split(e, sep)) words.push_back(w);
        parts = s::move(words);
      }
      if (rc_expand) {
        rc_splice(parts, lit, not split and v.words_current() ? &v.words : nullptr);
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
  // `single`: the outermost ${…} of a word that stays one word (an
  // assignment, a here-string, a pattern), where zsh does no splitting.
  Result<ParamResult> param_value(strv t, bool dq, bool single = false) {
    // Nested ${…} past what the stack holds fails instead of crashing.
    if (stack_low()) return s::unexpected(subst_error("substitutions nested too deeply"));
    return param_value_at(t, dq, single);
  }

  Result<ParamResult> param_value_at(strv t, bool dq, bool single) {
    ParamResult res;
    s::size_t i = 0;
    ParamFlags fl;
    if (i < t.size() and t[i] == '(') {
      auto e = flags_end(t, i);
      if (e == strv::npos) return s::unexpected(subst_error("bad substitution"));
      auto f = parse_flags(t.substr(i + 1, e - i - 1));
      if (not f) return s::unexpected(f.error());
      fl = *f;
      i = e + 1;
    }
    bool length = false, plus = false;
    bool eq_split = (opts().get(OPT_SHWORDSPLIT) and not no_shwordsplit) or force_split;
    bool eq_given = false;   // ${=x}: splits even in double quotes
    bool glob = opts().get(OPT_GLOBSUBST);
    bool rc_expand = opts().get(OPT_RCEXPANDPARAM);
    // Prefix operators: ${=x} ${==x} ${~x} ${~~x} ${^x} ${^^x} ${+x} ${#x}.
    while (i + 1 < t.size()) {
      char c = t[i], n = t[i + 1];
      // ${#:-word}: the length of the word (an empty subject).
      bool subject = is_name_start(n) or is_special_param(n) or n == '$' or n == '"'
                  or s::isdigit(static_cast<unsigned char>(n)) or n == ':';
      if (c == '=' or c == '~') {
        bool twice = n == c;
        (c == '=' ? eq_split : glob) = not twice;
        if (c == '=') eq_given = not twice;
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

    if (no_eq_split) eq_split = eq_given = false;

    // The subject: a name, a special parameter, or a nested substitution.
    str name;
    Value v;
    bool nested = false;
    bool subscripted = false;   // the first subscript is applied already
    if (i + 1 < t.size() and (t[i] == '$' or t[i] == '"')) {
      nested = true;
      s::size_t e = nested_end(t, i);
      strv inner = t.substr(i, e - i);
      if (inner.size() > 2 and inner[0] == '$' and inner[1] == '{') {
        auto r = param_value(inner.substr(2, inner.size() - 3), dq);
        if (not r) return s::unexpected(r.error());
        v = nested_value(*r, dq);
        // Unquoted, the inner result's empty elements go, as when it is
        // a word list: ${#${a}}.
        if (not dq and v.array and not v.quoted_elems)
          s::erase_if(v.arr, [](const str& e) { return e.empty(); });
      } else if (inner.size() > 1 and inner[0] == '$'
                 and inner[1] != '(' and inner[1] != '[') {
        auto r = param_value(inner.substr(1), dq);
        if (not r) return s::unexpected(r.error());
        v = nested_value(*r, dq);
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
      // ${(P)${…}} and ${(tP)${…}} are resolved once the subscripts are
      // applied, by indirect() below.
    } else {
      s::size_t s = i;
      if (i < t.size() and is_name_start(t[i])) {
        while (i < t.size() and is_name_char(t[i])) ++i;
      } else if (i < t.size() and s::isdigit(static_cast<unsigned char>(t[i]))) {
        while (i < t.size() and s::isdigit(static_cast<unsigned char>(t[i]))) ++i;
      } else if (i < t.size() and is_special_param(t[i])) {
        ++i;
      } else if (i < t.size() and strv(":-+=?#%/").find(t[i]) == strv::npos) {
        return s::unexpected(subst_error("bad substitution"));
      }
      // No name at all, as in ${:-word}: an unset parameter.
      name = str(t.substr(s, i - s));
      if (fl.indirect) {
        v = get_param(name, ParamFlags{});
      } else if (fl.type) {
        v.set = true;
        v.make_scalar(type_of(name));
      } else if (auto ev = element_view(name, t, i)) {
        // A special association with per-key handlers ($mapfile, …):
        // `${h[key]}` reads that key only, not the whole table.
        if (not *ev) return s::unexpected(ev->error());
        v = s::move(**ev);
      } else if (auto el = element_in_place(name, fl, t, i)) {
        if (not *el) return s::unexpected(el->error());
        v = s::move(**el);
        subscripted = true;
      } else {
        v = get_param(name, fl);
      }
      if (name == "@") res.keep_separate = true;
    }

    // The parameter as error messages name it: `a[5]: parameter not set`.
    str label = name;
    // KSH_ARRAYS: an array named without a subscript is its first element.
    if (opts().get(OPT_KSH_ARRAYS) and not nested and v.array and not fl.indirect
        and name != "@" and name != "*" and (i >= t.size() or t[i] != '[')) {
      str first = v.arr.empty() ? str{} : v.arr[0];
      v.make_scalar(s::move(first));
    }
    // Subscripts, possibly several: ${a[2][1]}.
    s::size_t subscripts = i;
    while (i < t.size() and t[i] == '[') {
      CloseTooDeep = false;
      s::size_t e = find_close(t, i);
      if (e == strv::npos) return s::unexpected(unclosed_error());
      label += t.substr(i, e - i + 1);
      if (subscripted) {   // done in place already
        subscripted = false;
        i = e + 1;
        continue;
      }
      bool all = false;
      if (auto r = apply_subscript(v, t.substr(i + 1, e - i - 1), nested ? "" : name, all,
                                   fl.keys, fl.values);
          not r)
        return s::unexpected(r.error());
      if (all) res.keep_separate = true;
      i = e + 1;
    }
    if (fl.indirect) {
      // (P): the value so far names the parameter, perhaps with a
      // subscript (`a[2]`).
      str ref = v.array ? (v.arr.empty() ? str{} : v.arr[0]) : v.scalar;
      auto r = indirect(ref, fl, name, res);
      if (not r) return s::unexpected(r.error());
      v = s::move(*r);
      if (not ref.empty()) label = ref;
      nested = false;
    }
    if (fl.at) res.keep_separate = true;

    if (plus) {
      Value out;
      out.set = true;
      out.make_scalar(v.set ? "1" : "0");
      res.v = s::move(out);
      return res;
    }

    strv subscript_text = t.substr(subscripts, i - subscripts);
    strv op = t.substr(i);
    bool special = not name.empty() and (is_special_param(name[0])
                    or s::isdigit(static_cast<unsigned char>(name[0])));
    // NO_UNSET: special parameters are always set, except positional
    // parameters past the last.
    bool nounset_error = not v.set and not opts().get(OPT_UNSET) and not nested
                     and not label.empty();

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

    bool from_operand = false;   // the value is that of ${x:-word} and the like
    if (not op.empty()) {
      bool colon = op[0] == ':';
      strv rest = colon ? op.substr(1) : op;
      // ${x::=word} assigns unconditionally.
      bool force_assign = colon and rest.starts_with(":=");
      if (force_assign) rest.remove_prefix(1);
      char o = rest.empty() ? 0 : rest[0];
      // In double quotes (without (@)) an array is null when its joined
      // value is: ("") is, ("" "") is not unless IFS is empty.
      bool null = not v.set or (v.array ? v.arr.empty() : v.scalar.empty());
      if (v.set and v.array and dq and not res.keep_separate and not null)
        null = s::all_of(v.arr.begin(), v.arr.end(), [](const str& e) { return e.empty(); })
           and (v.arr.size() == 1 or ifs_first().empty());
      bool empty_test = colon ? null : not v.set;
      if (o == '-' or o == '=' or o == '?' or o == '+') {
        strv word = rest.substr(1);
        switch (o) {
          case '-':
            if (empty_test) {
              auto w = operand(word, dq, eq_given and eq_split);
              if (not w) return s::unexpected(w.error());
              v = *w;
              from_operand = true;
            }
          break; case '+':
            if (empty_test) { v.set = true; v.make_scalar(""); }
            else {
              auto w = operand(word, dq, eq_given and eq_split);
              if (not w) return s::unexpected(w.error());
              v = *w;
              from_operand = true;
            }
          break; case '=':
            if (empty_test or force_assign) {
              if (not nested and name.empty())
                return s::unexpected(subst_error("not an identifier: "));
              if (nested or special)
                return s::unexpected(subst_error("can't set " + str(name)));
              // When the result will be split (${=…}, or SH_WORD_SPLIT where
              // words are made), zsh assigns the operand with no ${=…},
              // SH_WORD_SPLIT, (s) or (f) splitting at its own top level
              // (multsub with PREFORK_SINGLE); one level deeper they apply.
              // With (A) its words are split instead.
              bool will_split = eq_split and (eq_given or mode == Mode::Word);
              auto w = operand(word, dq, false, true, will_split and not fl.array_assign);
              if (not w) return s::unexpected(w.error());
              if (eq_split and fl.array_assign) {
                vec<str> parts;
                for (auto& e : w->list())
                  for (auto& p : ifs_split(e, ifs())) parts.push_back(s::move(p));
                w->make_array(s::move(parts));
              }
              Result<void> r;
              if (fl.array_assign) {
                // (A): the words become an array; (AA): an association's
                // key/value pairs.  An empty operand is an empty one.
                vec<str> words = w->array ? w->arr
                               : w->scalar.empty() ? vec<str>{} : vec<str>{w->scalar};
                // (s:sep:) and (f) split the words before they are assigned.
                if (fl.split and not fl.split_sep.empty()) {
                  vec<str> parts;
                  for (auto& wd : words)
                    for (auto& part : split_on(wd, fl.split_sep))
                      if (not part.empty()) parts.push_back(s::move(part));
                  words = s::move(parts);
                }
                r = assign_array(name, words, fl.array_assign >= 2);
                if (fl.array_assign >= 2) {
                  vec<str> values;
                  for (s::size_t k = 1; k < words.size(); k += 2) values.push_back(words[k]);
                  words = s::move(values);
                }
                w->set = true;
                w->make_array(s::move(words));
              } else {
                // zsh assigns, and goes on with, the words joined as one
                // string; only that result is split by SH_WORD_SPLIT.
                if (w->array) {
                  str sep = ifs_first(), joined;
                  for (s::size_t k = 0; k < w->arr.size(); ++k) joined += (k ? sep : str{}) + w->arr[k];
                  w->make_scalar(s::move(joined));
                }
                w->words.clear();   // the result is a plain value now
                if (not subscript_text.empty()) {
                  // ${h[key]:=word}: that element.
                  auto key = expand_scalar(subscript_text.substr(1, subscript_text.size() - 2),
                                           Mode::Scalar);
                  if (not key) return s::unexpected(key.error());
                  Assignment as;
                  as.name = name;
                  as.value = w->scalar;
                  as.subscript = *key;
                  r = ex.exec().assign_expanded(as);
                } else {
                  r = ctx().params.set_scalar(name, w->scalar);
                }
              }
              if (not r) return s::unexpected(r.error());
              v = *w;
            }
          break; default:  // '?'
            if (empty_test) {
              // zsh 5.9 prints the word as written: no expansion, no
              // quote removal (`${x:?'boom'}` says `x: 'boom'`).
              str msg = word.empty() ? str("parameter not set") : nice_text(word);
              ctx().hard_error = true;
              // A shell that isn't interactive exits at once, as zsh's
              // does: `always` blocks don't run.
              if (not opts().interactive()) {
                ctx().exiting  = ctx().exit_now = true;
                ctx().exit_val = 1;
                // From a function zsh exits at once (zexit): the shell's
                // EXIT trap doesn't run.
                if (ctx().func_depth > 0) ex.exec().drop_exit_trap();
              }
              return s::unexpected(subst_error(label + ": " + msg));
            }
        }
        join_dq();
      } else if (colon and o == '#') {
        // ${x:#pat}: drop array elements matching pat (scalar→empty if
        // it matches); ${(M)x:#pat}: keep matches instead.  Introduced
        // in M13 Phase 13a.  The pattern operand is read with `dq=false`
        // so glob characters stay active even when the whole
        // substitution is in a double-quoted context.
        join_dq();
        auto pat = operand_pattern(rest.substr(1), /*dq=*/false);
        if (not pat) return s::unexpected(pat.error());
        bool refs = pat->backrefs() or pat->sets_match();
        auto keep = [&](const str& s) {
          PatternMatch pm;
          bool m = pat->matches(s, &pm);
          if (m and refs)
            set_match_params(ctx().params, *pat, s, 0, s.size(), pm, opts().get(OPT_KSH_ARRAYS), 0);
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
        if (nounset_error)
          return s::unexpected(subst_error(label + ": parameter not set"));
        join_dq();
        bool longest = rest.size() > 1 and rest[1] == o;
        auto pat = operand_pattern(rest.substr(longest ? 2 : 1), false);
        if (not pat) return s::unexpected(pat.error());
        auto nth = nth_match(fl);
        if (not nth) return s::unexpected(nth.error());
        bool mb = opts().multibyte();
        // (#b) and (#m) set $match and $MATCH to the match.
        bool refs = pat->backrefs() or pat->sets_match();
        each(v, [&](const str& s) {
          HitFn set_refs = [&](s::size_t b, s::size_t e, s::size_t b_char,
                               const PatternMatch& pm) -> Result<void> {
            set_match_params(ctx().params, *pat, s, b, e - b, pm, opts().get(OPT_KSH_ARRAYS),
                             b_char);
            return {};
          };
          return get_match(s, *pat, o == '%', longest, fl, *nth, mb, refs ? &set_refs : nullptr);
        });
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
        auto pat = operand_pattern(body.substr(0, slash), false);
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
            auto r = replace_each(s, *pat, repl_text, dq, global, anchor, fl.m_substr, *nth);
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
            auto r = replacement(repl_text, dq);
            if (not r) return s::unexpected(r.error());
            repl = s::move(*r);
          }
          bool mb = opts().multibyte();
          each(v, [&](const str& s) {
            return replace(s, *pat, repl, global, anchor, mb, fl.m_substr, *nth);
          });
        }
      } else if (colon and o == 0) {
        return s::unexpected(subst_error("unrecognized modifier"));
      } else if (colon and (s::isdigit(static_cast<unsigned char>(o))
                            or strv(" ($\"'").find(o) != strv::npos)) {
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
        // [b, e) of n items.  n + x cannot overflow for n >= 0 and x < 0,
        // and b + len is only formed when it stays below n.
        s::int64_t b = 0, e = 0;
        auto cut = [&](s::int64_t n) -> Result<void> {
          b = *off < 0 ? s::max<s::int64_t>(0, n + *off) : s::min(*off, n);
          if (not len)       e = n;
          else if (*len < 0) e = n + *len;
          else               e = *len >= n - b ? n : b + *len;
          if (e < b) {
            if (len and *len < 0)
              return s::unexpected(subst_error("substring expression: "
                  + s::to_string(e) + " < " + s::to_string(b)));
            e = b;
          }
          return {};
        };
        // $@ and $* count $0 as offset 0, as zsh (and bash) do.
        if (v.array and not nested and (name == "@" or name == "*"))
          v.arr.insert(v.arr.begin(), get_param("0", ParamFlags{}).scalar);
        if (v.array) {
          if (auto r = cut(static_cast<s::int64_t>(v.arr.size())); not r) return s::unexpected(r.error());
          v.make_array(vec<str>(v.arr.begin() + b, v.arr.begin() + e));
        } else {
          Chars cs{v.scalar, opts().multibyte()};
          if (auto r = cut(static_cast<s::int64_t>(cs.size())); not r) return s::unexpected(r.error());
          v.make_scalar(cs.sub(static_cast<s::size_t>(b), static_cast<s::size_t>(e - b)));
        }
      } else if (colon and (strv("htreulqQaAcPs&").find(o) != strv::npos or o == 'g')) {
        join_dq();
        // :h :t :r :e :u :l :q :Q :a :A :c :P modifiers, chained (`:t:r`),
        // plus :s/pat/repl/ (and :gs/pat/repl/ for global), the
        // history-style substitution modifier (M13 Phase 13a), and :& for
        // "repeat the last :s".
        if (auto r = apply_modifiers(v, op); not r) return s::unexpected(r.error());
      } else if (colon and s::isalpha(static_cast<unsigned char>(o))) {
        return s::unexpected(
            subst_error("unrecognized modifier `" + str(1, o) + "'"));
      } else {
        return s::unexpected(subst_error("bad substitution"));
      }
    } else {
      if (nounset_error)
        return s::unexpected(subst_error(label + ": parameter not set"));
      // M13 Phase 13a: don't pre-join arrays here — the (V)/(#)/(c)/(w)/
      // (D) flags below work element-wise through each(), and the final
      // dq-join at the end of this function still collapses the array
      // into one word for the scalar `"..."` context.
    }

    if (length) {
      bool mb = opts().multibyte();
      s::size_t n = v.array ? v.arr.size() : Chars{v.scalar, mb}.size();
      // (w) counts words and (c) characters, an array's elements being
      // joined with spaces first.
      if ((fl.count_w or fl.count_c) and (v.array or fl.count_w)) {
        str all = v.scalar;
        if (v.array) {
          all.clear();
          for (s::size_t k = 0; k < v.arr.size(); ++k) all += (k ? " " : "") + v.arr[k];
        }
        if (fl.count_c) {
          n = Chars{all, mb}.size();
        } else if (fl.split and not fl.split_sep.empty()) {
          n = 0;
          for (s::size_t b = 0;;) {
            auto e = all.find(fl.split_sep, b);
            if ((e == str::npos ? all.size() : e) > b) ++n;
            if (e == str::npos) break;
            b = e + fl.split_sep.size();
          }
        } else {
          n = ifs_split(all, ifs()).size();
        }
      }
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
    // Splitting (rule 11 of zsh's list).  An array is joined first unless
    // (@) keeps its elements apart.  In double quotes the pieces stay
    // separate words; there, as zsh does, empty pieces of (s)/(f) between
    // the first and the last go away unless (@) is given.
    // An operand's value in double quotes is not split by `=` (zsh splits
    // only its unquoted text; jfsh, like the base, splits none of it).
    bool dq_eq_split = dq and eq_split and eq_given and not from_operand;
    // (z) splits even where the word stays one word, as zsh's shsplit.
    if (fl.split_words or ((fl.split or dq_eq_split) and not single)) {
      if (v.array and not fl.at) {
        str sep = ifs_first(), joined;
        for (s::size_t k = 0; k < v.arr.size(); ++k) {
          if (k) joined += sep;
          joined += v.arr[k];
        }
        v.make_scalar(s::move(joined));
      }
      vec<str> out;
      for (auto& s : v.list()) {
        if (not fl.split and not fl.split_words) {
          for (auto& p : ifs_pieces(s, ifs())) out.push_back(s::move(p.text));
          continue;
        }
        if (fl.split_words) {
          z_words(s, fl.z_opts, out);
          continue;
        }
        if (fl.split_sep.empty()) {
          Chars cs{s, opts().multibyte()};
          for (s::size_t k = 0; k < cs.size(); ++k) out.push_back(cs.sub(k, 1));
          continue;
        }
        for (auto& part : split_on(s, fl.split_sep)) out.push_back(s::move(part));
      }
      if (not dq)
        s::erase_if(out, [](const str& p) { return p.empty(); });
      else if (fl.split and not fl.at and out.size() > 2)
        out.erase(s::remove_if(out.begin() + 1, out.end() - 1,
                               [](const str& p) { return p.empty(); }),
                  out.end() - 1);
      if (dq and out.empty()) out.emplace_back();
      v.make_array(s::move(out));
      eq_split = false;
      if (dq) res.keep_separate = true;
    }
    if (fl.upper or fl.lower or fl.capitalize) {
      char how = fl.upper ? 'U' : fl.lower ? 'L' : 'C';
      bool mb = opts().multibyte();
      each(v, [&](const str& s) { return map_case(s, how, mb); });
    }
    // Sorting; (i) and (n) alone sort ascending too.
    if (v.array and (fl.sort_asc or fl.sort_desc or fl.sort_nocase or fl.sort_numeric)) {
      bool mb = opts().multibyte();
      // Without (n) the strxfrm() keys compare bytewise, which is much
      // cheaper than strcoll() per comparison.
      vec<s::pair<str, s::size_t>> keys;
      keys.reserve(v.arr.size());
      for (s::size_t k = 0; k < v.arr.size(); ++k) {
        str key = fl.sort_nocase ? map_case(v.arr[k], 'L', mb) : v.arr[k];
        keys.emplace_back(fl.sort_numeric ? s::move(key) : collate_key(key), k);
      }
      s::stable_sort(keys.begin(), keys.end(), [&](const auto& a, const auto& b) {
        if (not fl.sort_numeric) return a.first < b.first;
        if (fl.sort_nocase and a.first == b.first) return false;
        return collate(a.first, b.first, true) < 0;
      });
      vec<str> sorted;
      sorted.reserve(keys.size());
      for (auto& [_, k] : keys) sorted.push_back(s::move(v.arr[k]));
      if (fl.sort_desc) s::reverse(sorted.begin(), sorted.end());
      v.arr = s::move(sorted);
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
    bool mb = opts().multibyte();
    // An empty word quotes as '' so that it survives being read back.
    if (fl.quote == -1) each(v, [](const str& s) { return quote_minimal(s); });
    else if (fl.quote == -2) each(v, [&](const str& s) { return quote_plus(s, mb); });
    else if (fl.quote == 1)
      each(v, [&](const str& s) { return s.empty() ? str("''") : quote_backslash(s, mb); });
    else if (fl.quote == 2) each(v, [](const str& s) { return quote_single(s); });
    else if (fl.quote == 3) each(v, [](const str& s) { return quote_double(s); });
    else if (fl.quote >= 4) each(v, [&](const str& s) { return quote_dollar(s, mb); });

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
        for (char ch : s) {
          auto c = static_cast<unsigned char>(ch);
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
        auto r = math_eval(s, ctx().params, math_options(ctx()),
                           &ctx().modules.math_funcs());
        if (not r) return "0";
        return format_math(*r);
      });
    }

    // (c) — replace each element with its character length (same semantic
    // as ${#x}, but per-element for arrays).
    if (fl.count_c) {
      each(v, [&](const str& s) { return s::to_string(Chars{s, mb}.size()); });
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
          Chars cs{s, mb};
          s::size_t half = cs.size() / 2;
          return pad_word(cs.sub(0, half), *wl, fl.pad_l, true, mb)
               + pad_word(cs.sub(half, cs.size()), *wr, fl.pad_r, false, mb);
        }
        return wl ? pad_word(s, *wl, fl.pad_l, true, mb) : pad_word(s, *wr, fl.pad_r, false, mb);
      });
    }

    res.v     = s::move(v);
    res.split = eq_split;
    res.glob  = glob or fl.glob_pat;
    res.rc_expand = rc_expand;
    return res;
  }

  // (z): the shell words of `s`, a newline being `;`.  An array
  // assignment in command position comes apart as zsh's lexer gives it:
  // `y=(`, its elements, `)`.
  // (z) and (Z:opts:).  With c or C a `#` that starts a word starts a
  // comment running to the end of its line, quotes in it included: c keeps
  // it as one word, C drops it.  With n a newline is a blank, not `;`.
  static void z_words(strv s, strv opts, vec<str>& out) {
    bool keep = opts.find('c') != strv::npos, drop = opts.find('C') != strv::npos;
    bool blank = opts.find('n') != strv::npos;
    if (not keep and not drop) { blank ? code_blank(s, out) : shell_words(s, out); return; }
    // Find the comments: outside quotes and substitutions, at the start
    // of a word (after an unescaped blank).
    s::size_t from = 0;
    bool sq = false, dq = false, word_start = true;
    for (s::size_t k = 0; k < s.size(); ++k) {
      char c = s[k];
      bool start = word_start;
      word_start = false;
      if (sq) { if (c == '\'') sq = false; continue; }
      if (c == '\\') { ++k; continue; }
      if (dq) { if (c == '"') dq = false; continue; }
      if (c == '\'') { sq = true; continue; }
      if (c == '"') { dq = true; continue; }
      if (c == '`') {
        for (++k; k < s.size() and s[k] != '`'; ++k)
          if (s[k] == '\\') ++k;
        continue;
      }
      if ((c == '$' or c == '<' or c == '>') and k + 1 < s.size()
          and (s[k + 1] == '(' or (c == '$' and (s[k + 1] == '{' or s[k + 1] == '[')))) {
        auto e = find_close(s, k + 1);
        k = e == strv::npos ? s.size() : e;
        continue;
      }
      if (strv(" \t\n;|&").find(c) != strv::npos) { word_start = true; continue; }
      if (c != '#' or not start) continue;
      auto e = s.find('\n', k);
      if (e == strv::npos) e = s.size();
      if (blank) code_blank(s.substr(from, k - from), out);
      else       shell_words(s.substr(from, k - from), out);
      if (keep) out.emplace_back(s.substr(k, e - k));
      from = k = e;   // the newline itself goes with the code after
      if (k >= s.size()) break;
      --k;
    }
    if (from < s.size()) {
      if (blank) code_blank(s.substr(from), out);
      else       shell_words(s.substr(from), out);
    }
  }

  // shell_words() with each newline taken as a blank.
  static void code_blank(strv s, vec<str>& out) {
    str t(s);
    bool sq = false, dq = false;
    for (s::size_t k = 0; k < t.size(); ++k) {
      char c = t[k];
      if (sq) { if (c == '\'') sq = false; continue; }
      if (c == '\\') { ++k; continue; }
      if (c == '"') { dq = not dq; continue; }
      if (dq) continue;
      if (c == '\'') { sq = true; continue; }
      if (c == '\n') t[k] = ' ';
    }
    shell_words(t, out);
  }

  static void shell_words(strv s, vec<str>& out) {
    StringInputSource in{str(s)};
    Lexer lex{in};
    bool command = true;   // the next word starts a simple command
    for (;;) {
      auto tok = lex.next_token();
      if (tok.is_eof()) break;
      if (tok.is_newline()) { out.push_back(";"); command = true; continue; }
      const str& w = tok.text;
      s::size_t k = 0;
      while (k < w.size() and is_name_char(w[k])) ++k;
      if (k > 0 and k < w.size() and w[k] == '[') {
        auto e = find_close(w, k);
        k = e == strv::npos ? 0 : e + 1;
      }
      if (k < w.size() and w[k] == '+') ++k;
      bool assign = k > 0 and is_name_start(w[0]) and k < w.size() and w[k] == '=';
      bool array = command and assign and w.compare(k, 2, "=(") == 0 and w.back() == ')';
      bool sep = w == ";" or w == "|" or w == "||" or w == "&&" or w == "&" or w == "|&"
              or w == ";;" or w == "(" or w == "{";
      command = sep or (command and assign);
      if (not array) { out.push_back(w); continue; }
      out.push_back(w.substr(0, k + 2));
      array_words(strv(w).substr(k + 2, w.size() - k - 3), out);
      out.push_back(")");
    }
  }

  // The elements of `name=(…)` for (z): words split on blanks outside
  // quotes, substitutions and parentheses.
  static void array_words(strv s, vec<str>& out) {
    str cur;
    int depth = 0;
    auto flush = [&] { if (not cur.empty()) out.push_back(s::move(cur)); cur.clear(); };
    for (s::size_t i = 0; i < s.size(); ++i) {
      char c = s[i];
      s::size_t e = strv::npos;
      if (c == '\\' and i + 1 < s.size()) e = i + 1;
      else if (c == '\'') e = s.find('\'', i + 1);
      else if (c == '"') e = dquote_end(s, i);
      else if (c == '$' and i + 1 < s.size() and strv("({[").find(s[i + 1]) != strv::npos)
        e = find_close(s, i + 1);
      if (e != strv::npos) {
        cur += s.substr(i, e - i + 1);
        i = e;
        continue;
      }
      if (depth == 0 and (c == ' ' or c == '\t' or c == '\n')) {
        flush();
        if (c == '\n') out.push_back(";");
        continue;
      }
      if (c == '(') ++depth;
      else if (c == ')' and depth > 0) --depth;
      cur += c;
    }
    flush();
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

// zsh's BRACE_CCL set: the characters of `{…}`, `x-y` adding a range.
vec<char> brace_ccl(strv t) {
  bool in[256] = {};
  int last = -1;
  for (s::size_t k = 0; k < t.size(); ++k) {
    int c = static_cast<unsigned char>(t[k]);
    int next = k + 1 < t.size() ? static_cast<unsigned char>(t[k + 1]) : -1;
    if (c == '-' and last >= 0 and next >= 0 and last <= next) {
      while (last < next) in[last++] = true;
      last = -1;
    } else {
      in[last = c] = true;
    }
  }
  vec<char> out;
  for (int c = 1; c < 256; ++c) if (in[c]) out.push_back(static_cast<char>(c));
  return out;
}

// Brace expansion follows zsh's hasbraces() and xpandbraces() (glob.c).
// An active brace or comma is one written unquoted in the script (Field::put
// keeps those of a value literal); has_braces() makes literal the ones
// that cannot take part in an expansion, as zsh untokenizes them.
struct BraceScan {
  struct CharRange { char32_t from, to; };
  struct Number { s::int64_t value; s::size_t end; };

  // Scans field.text up to `end`, the end of the range being expanded.
  BraceScan(Field& field, s::size_t end) : f{field}, n{end} {}

  bool tok(s::size_t k, char c) const { return k < n and f.text[k] == c and not f.lit[k]; }
  bool digit(s::size_t k) const {
    return k < n and s::isdigit(static_cast<unsigned char>(f.text[k]));
  }
  bool dots(s::size_t k) const { return k + 1 < n and f.text[k] == '.' and f.text[k + 1] == '.'; }
  void literal(s::size_t k) { f.lit[k] = true; }

  // zsh's bracechardots(): `{x..y}` with single characters, at f.text[p].
  opt<CharRange> char_dots(s::size_t p, bool mb) const {
    s::size_t q = p + 1;
    auto take = [&](char32_t& c) {
      if (q >= n or tok(q, '{')) return false;
      s::size_t len = 1;
      c = mb ? utf8_decode(f.text, q, len) : static_cast<unsigned char>(f.text[q]);
      q += len;
      return true;
    };
    char32_t a = 0, b = 0;
    if (not take(a) or not dots(q)) return s::nullopt;
    q += 2;
    if (not take(b) or not tok(q, '}')) return s::nullopt;
    return CharRange{a, b};
  }

  // zsh's hasbraces() from f.text[from]: whether an expansion is left.
  bool has_braces(s::size_t from, bool ccl, bool mb) {
    if (ccl) {
      int bc = 0;
      for (s::size_t k = from; k < n; ++k) {
        if (tok(k, '{')) {
          if (bc == 0 and tok(k + 1, '}')) { literal(k); literal(k + 1); ++k; }
          else ++bc;
        } else if (tok(k, '}')) {
          if (bc == 0) literal(k);
          else if (--bc == 0) return true;
        }
      }
      return false;
    }
    constexpr s::size_t NO = static_cast<s::size_t>(-1);
    s::size_t lbr = NO, mbr = NO, comma = NO, i = from;
    // zsh's loop always ends; the bound only guards against a slip here.
    for (s::size_t steps = 0; steps < 64 * (n + 1) * (n + 1) + 1024; ++steps) {
      if (i >= n) {
        if (lbr != NO) literal(lbr);
        if (mbr == NO and comma == NO) return false;
        i = comma != NO ? comma : n + 1;
        if (mbr != NO and mbr < i) i = mbr;
        lbr = mbr = comma = NO;
        continue;
      }
      s::size_t p = i++;
      if (tok(p, '{')) {
        if (lbr == NO) {
          if (char_dots(p, mb)) return true;
          lbr = p;
          // {n..m} and {n..m..s}, digits in at least one end.
          if (i < n and f.text[i] == '-') ++i;
          while (digit(i)) ++i;
          for (int part = 0; part < 2 and dots(i); ++part) {
            i += 2;
            if (i < n and f.text[i] == '-') ++i;
            while (digit(i)) ++i;
            if (tok(i, '}') and (digit(lbr + 1) or digit(i - 1))) return true;
          }
        } else {
          s::size_t k = p;
          int level = 1;
          while (++k < n and level)
            if (tok(k, '{')) ++level;
            else if (tok(k, '}')) --level;
          if (level) {   // never closed: neither is a brace
            literal(lbr);
            literal(p);
            i = comma != NO ? comma : k;
            if (mbr != NO and mbr < i) i = mbr;
            lbr = mbr = comma = NO;
          } else {
            i = k;
            if (mbr == NO) mbr = p;
          }
        }
      } else if (tok(p, '}')) {
        if (lbr == NO) {
          literal(p);
        } else if (comma != NO) {
          return true;
        } else {
          literal(lbr);
          literal(p);
          if (mbr != NO) i = mbr;
          mbr = lbr = NO;
        }
      } else if (tok(p, ',')) {
        if (lbr == NO) literal(p);
        else if (comma == NO) comma = p;
      }
    }
    return false;
  }

  // zsh's zstrtol() for a sequence: an optional sign and digits,
  // saturating; the end is where the digits stop.
  Number number(s::size_t k) const {
    bool neg = k < n and f.text[k] == '-';
    if (k < n and (f.text[k] == '-' or f.text[k] == '+')) ++k;
    // Saturating at INT64_MAX, or at INT64_MIN for a negative number.
    s::uint64_t limit = static_cast<s::uint64_t>(INT64_MAX) + (neg ? 1 : 0);
    s::uint64_t v = 0;
    for (; digit(k); ++k) {
      auto d = static_cast<s::uint64_t>(f.text[k] - '0');
      v = v > (limit - d) / 10 ? limit : v * 10 + d;
    }
    if (neg) return {static_cast<s::int64_t>(0 - v), k};
    return {static_cast<s::int64_t>(v), k};
  }

  // The words of `{n..m}`/`{n..m..s}` opened at g and closed at c, as
  // xpandbraces makes them, or nothing when the text is not one.
  opt<vec<str>> sequence(s::size_t g, s::size_t c, int dotdot) const {
    auto [rstart, d1] = number(g + 1);
    if (d1 == g + 1 or not dots(d1)) return s::nullopt;
    auto [rend, p] = number(d1 + 2);
    if (p == d1 + 2) return s::nullopt;
    s::int64_t rincr = 1;
    s::size_t wid1 = d1 - g - 1, wid2 = c - d1 - 2, wid3 = 0, d2 = s::size_t(-1);
    if (p != c) {
      wid2 = p - d1 - 2;
      d2 = p;
      if (dotdot != 2 or not dots(p)) return s::nullopt;
      auto [inc, e] = number(p + 2);
      // zsh keeps the step in an int (so {1..10..4294967298} steps by 2
      // there); a full int64 step cannot turn a huge range into billions
      // of words.
      rincr = inc;
      wid3 = e - d2 - 2;
      if (e != c or rincr == 0) return s::nullopt;
    }
    auto zero_at = [&](s::size_t k) {
      return k < n and (f.text[k] == '0' or (f.text[k] == '-' and k + 1 < n and f.text[k + 1] == '0'));
    };
    int minw = static_cast<int>(zero_at(g + 1) ? wid1 : zero_at(d1 + 2) ? wid2
                                : d2 != s::size_t(-1) and zero_at(d2 + 2) ? wid3 : 0);
    bool rev = false;
    if (rincr < 0) { rincr = rincr == INT64_MIN ? INT64_MAX : -rincr; rev = not rev; }
    using U = s::uint64_t;
    auto step = static_cast<U>(rincr);
    if (rstart > rend) {
      s::swap(rstart, rend);
      rev = not rev;
    } else if (rincr > 1) {
      rend = static_cast<s::int64_t>(static_cast<U>(rend)
                                     - (static_cast<U>(rend) - static_cast<U>(rstart)) % step);
    }
    vec<str> out;
    char buf[32];
    for (s::int64_t v = rend;;) {
      s::snprintf(buf, sizeof buf, "%0*lld", minw, static_cast<long long>(v));
      out.emplace_back(buf);
      if (static_cast<U>(v) - static_cast<U>(rstart) < step) break;
      v = static_cast<s::int64_t>(static_cast<U>(v) - step);
    }
    // Made highest first; zsh lists them lowest first unless reversed.
    if (not rev) s::reverse(out.begin(), out.end());
    return out;
  }

  Field&    f;
  s::size_t n;
};

// The words f.text[lo, hi) expands to: xpandbraces on the first
// expansion has_braces() finds, then on the rest.  The groups of a word
// are expanded left to right as one product, the first varying slowest,
// which is zsh's order; only an expansion's own alternatives recurse.
void brace_range(Field& f, s::size_t lo, s::size_t hi, bool ccl, bool mb,
                 vec<Field>& out)
{
  struct Group {
    s::size_t size() const { return alts.empty() ? words.size() : alts.size(); }

    s::size_t  open, close;
    vec<Field> alts;    // alternatives, each expanded
    vec<str>   words;   // or the words of a sequence or BRACE_CCL set
  };
  // The scan marks literal braces in `f` itself: the ranges recursed on
  // are disjoint, and each is scanned only after the text before it, as
  // zsh scans each new word.
  BraceScan bs{f, hi};
  vec<Group> groups;
  for (s::size_t pos = lo; pos < hi and bs.has_braces(pos, ccl, mb);) {
    s::size_t g = pos;
    while (not bs.tok(g, '{')) ++g;
    // Match the brace, counting the commas and `..` at its top.
    int bc = 0, comma = 0, dotdot = 0;
    s::size_t c = g;
    for (; c < hi; ++c) {
      if (bs.tok(c, '{')) {
        ++bc;
      } else if (bs.tok(c, '}')) {
        if (--bc == 0) break;
      } else if (bc == 1) {
        if (bs.tok(c, ',')) ++comma;
        else if (bs.dots(c)) { ++dotdot; ++c; }
      }
    }
    if (c >= hi) break;
    // A group that is the whole range puts its words straight into `out`,
    // so deep nesting moves words up instead of copying them per level.
    bool whole = groups.empty() and g == lo and c + 1 == hi;
    Group grp{g, c, {}, {}};
    strv inner = strv(f.text).substr(g + 1, c - g - 1);
    bool done = false;
    if (not comma and dotdot) {
      if (auto r = bs.char_dots(g, mb)) {
        for (char32_t k = r->from;;) {
          str ch;
          if (mb) utf8_append(ch, k);
          else    ch = str(1, static_cast<char>(k));
          grp.words.push_back(s::move(ch));
          if (k == r->to) break;
          if (r->from <= r->to) ++k;
          else                  --k;
        }
        done = true;
      } else if (auto seq = bs.sequence(g, c, dotdot)) {
        grp.words = s::move(*seq);
        done = true;
      }
    }
    if (not done and not comma and ccl) {
      for (char ch : brace_ccl(inner)) grp.words.emplace_back(1, ch);
      done = not grp.words.empty();
    }
    if (not done) {
      // Comma alternatives, or the text alone (`{1..3..}` gives `1..3..`).
      vec<Field>& into = whole ? out : grp.alts;
      s::size_t first = into.size();
      s::size_t st = g + 1;
      int cnt = 0;
      for (s::size_t k = g + 1; k <= c; ++k) {
        if (k < c and bs.tok(k, '{')) ++cnt;
        else if (k < c and bs.tok(k, '}')) --cnt;
        else if (k == c or (cnt == 0 and bs.tok(k, ','))) {
          // Nesting that would run the stack out stays as it is.
          if (not stack_low()) {
            brace_range(f, st, k, ccl, mb, into);
          } else {
            into.push_back(f.slice(st, k));
          }
          st = k + 1;
        }
      }
      if (whole) {
        for (s::size_t k = first; k < out.size(); ++k) out[k].quoted = true;
        return;
      }
    }
    groups.push_back(s::move(grp));
    pos = c + 1;
  }
  if (groups.empty()) {
    out.push_back(lo == 0 and hi == f.text.size() ? f : f.slice(lo, hi));
    return;
  }

  // Every combination, as an odometer whose last group turns fastest.
  vec<s::size_t> at(groups.size(), 0);
  for (;;) {
    Field w;
    s::size_t pos = lo;
    for (s::size_t k = 0; k < groups.size(); ++k) {
      const Group& g = groups[k];
      w.append(f, pos, g.open);
      if (g.alts.empty()) w.put(strv(g.words[at[k]]), true);
      else                w.append(g.alts[at[k]]);
      pos = g.close + 1;
    }
    w.append(f, pos, hi);
    w.quoted = true;   // a word of a brace expansion stays when empty
    out.push_back(s::move(w));
    s::size_t k = groups.size();
    while (k > 0 and ++at[k - 1] == groups[k - 1].size()) at[--k] = 0;
    if (k == 0) break;
  }
}

void brace_expand(const Field& f, bool ccl, bool mb, vec<Field>& out) {
  Field w = f;
  // zsh's lexer makes a `}` a brace only while more `{` than `}` came
  // before it; any other `}` is plain text.
  int open = 0;
  for (s::size_t k = 0; k < w.text.size(); ++k) {
    if (w.lit[k]) continue;
    if (w.text[k] == '{') ++open;
    else if (w.text[k] == '}' and open-- == 0) { open = 0; w.lit[k] = true; }
  }
  brace_range(w, 0, w.text.size(), ccl, mb, out);
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
  vec<str> out;
  for (auto& p : ifs_pieces(s, ifs))
    if (p.keep) out.push_back(s::move(p.text));
  return out;
}

// ---------------------------------------------------------------------------
// Expander.
// ---------------------------------------------------------------------------
namespace {

// `~`, `~+`, `~-`, `~user` at text[p] (unquoted); `end` bounds the name,
// which may be quoted or come from an expansion (`~"root"`, `~$user`), as
// in zsh.
Result<void> tilde_at(Field& f, s::size_t p, s::size_t end, ExecContext& ctx) {
  str name = f.text.substr(p + 1, end - p - 1);
  auto param = [&](const char* n) -> opt<str> {
    auto v = ctx.params.lookup(n);
    if (v and v->get().is_set()) return v->get().scalar();
    return s::nullopt;
  };
  opt<str> dir;
  if (name.empty()) {
    dir = param("HOME").value_or("");   // zsh: empty when HOME is unset
  } else if (name == "+") {
    dir = param("PWD");
  } else if (name == "-") {
    // Before any cd, or once OLDPWD is unset, zsh's own record of the
    // previous directory is the one the shell started in.
    dir = param("OLDPWD");
    if (not dir) dir = param("PWD");
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
      else brace_expand(f, opts.get(OPT_BRACECCL), opts.multibyte(), fields);
    }
    for (auto& f : fields) {
      if (f.text.empty()) {
        if (f.quoted and not f.empty_at) out.emplace_back();
        continue;
      }
      bool magic = opts.get(OPT_MAGICEQUALSUBST)
          and s::any_of(f.text.begin(), f.text.end(), [](char c) { return c == '='; });
      if (auto r = tilde_expand(f, magic, mCtx); not r) return s::unexpected(r.error());
      // `=cmd` becomes the command's path.  Only the `=` must be unquoted:
      // the name may be quoted or come from an expansion (`=$x`), as in zsh.
      if (opts.get(OPT_EQUALS) and f.text.size() > 1 and f.text[0] == '=' and f.unlit(0)
          and not (f.text[1] == '(' and f.unlit(1))) {
        str name = f.text.substr(1);
        auto path = mExec.find_command(name);
        // A name with a slash must be an executable file.
        struct stat st{};
        if (path and name.find('/') != str::npos
            and (::stat(path->c_str(), &st) != 0 or not S_ISREG(st.st_mode)
                 or ::access(path->c_str(), X_OK) != 0))
          path = s::unexpected(subst_error(name + " not found"));
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
  // Plain text (a number, a name) has nothing to expand.
  if (word.find_first_of("$`\\'\"~<>(=") == strv::npos) return str(word);
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

Result<MathNum> Expander::eval_math(strv text) {
  Scan scan{*this, Mode::Heredoc};
  return scan.math(text, false);
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
