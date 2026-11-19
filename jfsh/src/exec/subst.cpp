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
#include <jfsh/exec/executor.h>
#include <jfsh/glob/glob.h>
#include <jfsh/lexer/input.h>
#include <jfsh/lexer/lexer.h>

// TODO(subst/param): still missing from ${…}: the subscript flags
//   ${a[(r)pat]} ${a[(i)pat]}, padding (l:n:) (r:n:), (%) prompt expansion,
//   (e), (D), (V), (c)/(w) counting, ${^x} / RC_EXPAND_PARAM, ${x:#pat}
//   array filtering, :s/x/y/ and :& modifiers, and KSH_ARRAYS indexing.
// TODO(subst/errexit): a failed ${x?} or ${x:?} makes a non-interactive
//   zsh exit; jfsh only fails the command.

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
s::size_t find_close(strv w, s::size_t open) {
  char o = w[open];
  char c = o == '(' ? ')' : o == '{' ? '}' : ']';
  int depth = 1;
  for (s::size_t i = open + 1; i < w.size(); ++i) {
    char ch = w[i];
    if (ch == '\\') { ++i; continue; }
    if (ch == '\'' and o != '[') {
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
               and (w[i + 1] == '(' or w[i + 1] == '{') and w[i + 1] != o) {
      auto e = find_close(w, i + 1);
      if (e == strv::npos) return e;
      i = e;
    } else if (ch == o) {
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

// zsh's (q-): quote only when needed, with single quotes.
str quote_minimal(strv s) {
  constexpr strv SAFE =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_@%+=:,./-";
  if (not s.empty() and s.find_first_not_of(SAFE) == strv::npos)
    return str(s);
  return quote_single(s);
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
      if (not lit[i] and strv("*?[(<^#~").find(text[i]) != strv::npos)
        return true;
    return false;
  }
};

// A parameter's value while ${…} is being worked on.
struct Value {
  bool     set   = false;
  bool     array = false;
  vec<str> arr;
  str              scalar;

  vec<str> list() const {
    if (array) return arr;
    return {scalar};
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
};

enum class Mode : int { Word, Scalar, Heredoc };

struct Scan {
  Expander& ex;
  Mode      mode;
  vec<Field> fields{1};
  bool pending_break = false;

  Scan(Expander& e, Mode m) : ex{e}, mode{m} {}

  ExecContext& ctx() { return ex.ctx(); }
  OptionState& opts() { return ex.ctx().opts; }

  Field& cur() {
    if (pending_break) {
      pending_break = false;
      fields.emplace_back();
    }
    return fields.back();
  }
  void put(char c, bool lit) { cur().put(c, lit); }
  void put(strv s, bool lit) { cur().put(s, lit); }
  void new_field(bool quoted) {
    pending_break = false;
    fields.emplace_back();
    fields.back().quoted = quoted;
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
          cur().quoted = true;
          auto e = w.find('\'', i + 1);
          if (e == strv::npos) e = w.size();
          put(w.substr(i + 1, e - i - 1), true);
          i = e + 1;
          continue;
        }
        if (c == '"') {
          cur().quoted = true;
          dq = true;
          ++i;
          continue;
        }
        if (c == '$' and i + 1 < w.size() and w[i + 1] == '\'') {
          cur().quoted = true;
          s::size_t e = i + 2;
          while (e < w.size() and w[e] != '\'') e += (w[e] == '\\') ? 2u : 1u;
          put(c_escapes(w.substr(i + 2, s::min(e, w.size()) - i - 2)), true);
          i = e + 1;
          continue;
        }
      } else {
        if (c == '"') { dq = false; ++i; continue; }
        if (c == '\\' and i + 1 < w.size()
            and strv("$`\"\\\n").find(w[i + 1]) != strv::npos) {
          if (w[i + 1] != '\n') put(w[i + 1], true);
          i += 2;
          continue;
        }
      }
      if (c == '$' or c == '`') {
        if (auto r = substitution(w, i, dq); not r) return r;
        continue;
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
      s::size_t e = find_close(w, n);
      if (e == strv::npos)
        return s::unexpected(subst_error("bad substitution"));
      i = e + 1;
      return param(w.substr(n + 1, e - n - 1), dq);
    }
    // Unbraced: $name, $name[sub], $1, $?, $#name, $=name, $~name.
    s::size_t j = n;
    str expr;
    while (j < w.size() and (w[j] == '=' or w[j] == '~')) ++j;
    if (j < w.size() and w[j] == '#' and j + 1 < w.size() and is_name_start(w[j + 1])) ++j;
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

  Result<MathNum> math(strv expr) {
    auto text = expand_scalar(expr);
    if (not text) return s::unexpected(text.error());
    return math_eval(*text, ctx().params, math_options(opts()),
                     &ctx().modules.math_funcs());
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
    const Param& param = p->get();
    str t = param.is_hash() ? "association" : param.is_array() ? "array"
          : param.is_integer() ? "integer" : param.is_float() ? "float" : "scalar";
    if (param.flags & PM_LOCAL)    t += "-local";
    if (param.is_readonly())       t += "-readonly";
    if (param.is_exported())       t += "-export";
    if (param.flags & PM_SPECIAL)  t += "-special";
    return t;
  }

  Result<ParamFlags> parse_flags(strv f) {
    ParamFlags fl;
    for (s::size_t i = 0; i < f.size(); ++i) {
      char c = f[i];
      auto delimited = [&]() -> Result<str> {
        if (i + 1 >= f.size()) return s::unexpected(subst_error("error in flags"));
        char open = f[++i];
        char close = open == '(' ? ')' : open == '[' ? ']' : open == '{' ? '}'
                   : open == '<' ? '>' : open;
        auto e = f.find(close, i + 1);
        if (e == strv::npos) return s::unexpected(subst_error("error in flags"));
        str arg(f.substr(i + 1, e - i - 1));
        i = e;
        return arg;
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
        break; case 'u': fl.unique = true;
        break; case 'P': fl.indirect = true;
        break; case 't': fl.type = true;
        break; case 'z': fl.split_words = true;
        break; case 'Q': fl.unquote = true;
        break; case 'p': fl.escapes = true;
        break; case 'q':
          if (i + 1 < f.size() and f[i + 1] == '-') { fl.quote = -1; ++i; }
          else if (fl.quote >= 0) ++fl.quote;
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
    }
    return fl;
  }

  // Index of the end of the nested substitution that starts at t[i].
  static s::size_t nested_end(strv t, s::size_t i) {
    if (t[i] == '"') {
      for (++i; i < t.size() and t[i] != '"'; ++i)
        if (t[i] == '\\') ++i;
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
    if (auto r = sub.scan(word, dq); not r) return s::unexpected(r.error());
    Value v;
    v.set = true;
    vec<str> out;
    for (auto& f : sub.fields)
      if (not f.text.empty() or f.quoted) out.push_back(f.text);
    if (out.size() <= 1) v.make_scalar(out.empty() ? "" : out[0]);
    else                 v.make_array(s::move(out));
    return v;
  }

  Result<CompiledPattern> operand_pattern(strv word, bool dq) {
    Scan sub{ex, Mode::Scalar};
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

  Result<void> apply_subscript(Value& v, strv sub, strv name,
                               bool& all) {
    if (sub == "@" or sub == "*") {
      all = sub == "@";
      if (not v.array and v.set) v.make_array({v.scalar});
      return {};
    }
    // Associative arrays are indexed by key.
    if (auto p = ctx().params.lookup(name); p and p->get().is_hash()) {
      auto key = expand_scalar(sub, Mode::Scalar);
      if (not key) return s::unexpected(key.error());
      auto h = p->get().hash();
      auto it = h.find(*key);
      if (it == h.end()) { v.set = false; v.make_scalar(""); }
      else               v.make_scalar(it->second);
      return {};
    }
    s::size_t comma = strv::npos;
    int depth = 0;
    for (s::size_t k = 0; k < sub.size(); ++k) {
      if (sub[k] == '(' or sub[k] == '[') ++depth;
      else if (sub[k] == ')' or sub[k] == ']') --depth;
      else if (sub[k] == ',' and depth == 0) { comma = k; break; }
    }
    auto lo = arith(sub.substr(0, comma));
    if (not lo) return s::unexpected(lo.error());
    opt<s::int64_t> hi;
    if (comma != strv::npos) {
      auto h = arith(sub.substr(comma + 1));
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
        if (a >= 1 and a <= n) v.make_scalar(v.arr[static_cast<s::size_t>(a - 1)]);
        else                   v.make_scalar("");
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
      case 'a': case 'A': {
        if (s.empty() or s[0] != '/') {
          char buf[PATH_MAX];
          if (::getcwd(buf, sizeof buf)) s = str(buf) + "/" + s;
        }
        if (m == 'A') {
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

  template <typename F>
  static void each(Value& v, F fn) {
    if (v.array) for (auto& e : v.arr) e = fn(e);
    else         v.scalar = fn(v.scalar);
  }

  // ${name/pat/repl}, ${name//pat/repl}, ${name:/pat/repl}.
  static str replace(const str& s, const CompiledPattern& pat,
                     const str& repl, bool global, char anchor) {
    s::size_t n = s.size();
    if (anchor == 'w') return pat.matches(s) ? repl : s;
    if (anchor == '#') {
      s::size_t len = pat.match_prefix(s, true);
      return len == str::npos ? s : repl + s.substr(len);
    }
    if (anchor == '%') {
      for (s::size_t b = 0; b <= n; ++b)
        if (pat.matches(strv(s).substr(b))) return s.substr(0, b) + repl;
      return s;
    }
    str out;
    s::size_t b = 0;
    bool done = false;
    while (b <= n) {
      s::size_t len = done ? str::npos
                           : pat.match_prefix(strv(s).substr(b), true);
      if (len == str::npos) {
        if (b < n) out += s[b];
        ++b;
        continue;
      }
      out += repl;
      if (not global) done = true;
      if (len == 0) {
        if (b < n) out += s[b];
        ++b;
      } else {
        b += len;
      }
    }
    return out;
  }

  // ${…}: `t` is the text between the braces.
  Result<void> param(strv t, bool dq) {
    auto r = param_value(t, dq);
    if (not r) return s::unexpected(r.error());
    auto& [v, keep_separate, split, glob] = *r;
    if (mode != Mode::Word) {
      // Only a pattern (`case`, `[[ = ]]`) looks at what is literal: there
      // ${~x} and GLOB_SUBST make the value's characters active.
      bool literal = dq or not glob;
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
      if (v.array and keep_separate) {
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
    if (v.array) {
      vec<str> parts;
      for (auto& e : v.arr) if (not e.empty()) parts.push_back(e);
      if (split) {
        str sep = ifs();
        vec<str> words;
        for (auto& e : parts)
          for (auto& w : ifs_split(e, sep)) words.push_back(w);
        parts = s::move(words);
      }
      splice(parts, lit, false);
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
    // Prefix operators: ${=x} ${==x} ${~x} ${~~x} ${^x} ${+x} ${#x}.
    while (i + 1 < t.size()) {
      char c = t[i], n = t[i + 1];
      bool subject = is_name_start(n) or is_special_param(n) or n == '$' or n == '"'
                  or s::isdigit(static_cast<unsigned char>(n));
      if (c == '=' or c == '~') {
        bool twice = n == c;
        (c == '=' ? eq_split : glob) = not twice;
        i += twice ? 2 : 1;
      } else if (c == '^') {
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
      } else if (inner.size() > 1 and inner[0] == '$'
                 and inner[1] != '(' and inner[1] != '[') {
        auto r = param_value(inner.substr(1), dq);
        if (not r) return s::unexpected(r.error());
        v = r->v;
        res.keep_separate = r->keep_separate;
      } else {
        auto s = expand_scalar(inner, Mode::Scalar);
        if (not s) return s::unexpected(s.error());
        v.set = true;
        v.make_scalar(*s);
      }
      i = e;
    } else {
      s::size_t s = i;
      if (i < t.size() and is_name_start(t[i])) {
        while (i < t.size() and is_name_char(t[i])) ++i;
      } else if (i < t.size() and s::isdigit(static_cast<unsigned char>(t[i]))) {
        while (i < t.size() and s::isdigit(static_cast<unsigned char>(t[i]))) ++i;
      } else if (i < t.size() and is_special_param(t[i])) {
        ++i;
      } else {
        return s::unexpected(subst_error("bad substitution"));
      }
      name = str(t.substr(s, i - s));
      if (fl.indirect) {
        Value ref = get_param(name, fl);
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
    while (i < t.size() and t[i] == '[') {
      s::size_t e = find_close(t, i);
      if (e == strv::npos) return s::unexpected(subst_error("bad substitution"));
      bool all = false;
      if (auto r = apply_subscript(v, t.substr(i + 1, e - i - 1), nested ? "" : name, all); not r)
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
      if (o == '-' or o == '=' or o == '?' or o == '+') {
        strv word = rest.substr(1);
        bool force_assign = false;
        if (o == '=' and colon and not word.empty() and word[0] == ':') {
          // ${x::=word} assigns unconditionally.
          force_assign = true;
          word.remove_prefix(1);
        }
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
              Result<void> r = w->array ? ctx().params.set_array(name, w->arr)
                                        : ctx().params.set_scalar(name, w->scalar);
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
      } else if (not colon and (o == '#' or o == '%')) {
        if (not v.set and not opts().get(OPT_UNSET) and not special)
          return s::unexpected(subst_error(name + ": parameter not set"));
        join_dq();
        bool longest = rest.size() > 1 and rest[1] == o;
        auto pat = operand_pattern(rest.substr(longest ? 2 : 1), dq);
        if (not pat) return s::unexpected(pat.error());
        each(v, [&](const str& s) {
          if (o == '#') {
            s::size_t len = pat->match_prefix(s, longest);
            return len == str::npos ? s : s.substr(len);
          }
          // Suffix: the shortest suffix starts as far right as possible.
          s::size_t n = s.size();
          for (s::size_t k = 0; k <= n; ++k) {
            s::size_t b = longest ? k : n - k;
            if (pat->matches(strv(s).substr(b))) return s.substr(0, b);
          }
          return s;
        });
      } else if (o == '/') {
        join_dq();
        strv body = rest.substr(1);
        bool global = false;
        char anchor = colon ? 'w' : 0;
        if (not colon and not body.empty() and body[0] == '/') {
          global = true;
          body.remove_prefix(1);
        } else if (not body.empty() and (body[0] == '#' or body[0] == '%')) {
          anchor = body[0];
          body.remove_prefix(1);
        }
        s::size_t slash = strv::npos;
        for (s::size_t k = 0; k < body.size(); ++k) {
          if (body[k] == '\\') { ++k; continue; }
          if (body[k] == '/') { slash = k; break; }
        }
        auto pat = operand_pattern(body.substr(0, slash), dq);
        if (not pat) return s::unexpected(pat.error());
        str repl;
        if (slash != strv::npos) {
          auto r = expand_scalar(body.substr(slash + 1), Mode::Scalar);
          if (not r) return s::unexpected(r.error());
          repl = *r;
        }
        each(v, [&](const str& s) { return replace(s, *pat, repl, global, anchor); });
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
        if (v.array) {
          s::int64_t b, e;
          cut(static_cast<s::int64_t>(v.arr.size()), b, e);
          v.make_array(vec<str>(v.arr.begin() + b, v.arr.begin() + e));
        } else {
          s::int64_t b, e;
          cut(static_cast<s::int64_t>(v.scalar.size()), b, e);
          v.make_scalar(v.scalar.substr(static_cast<s::size_t>(b), static_cast<s::size_t>(e - b)));
        }
      } else if (colon and strv("htreulqQaA").find(o) != strv::npos) {
        join_dq();
        // :h :t … modifiers, possibly chained (`:t:r`).
        strv mods = op;
        while (mods.size() >= 2 and mods[0] == ':') {
          char m = mods[1];
          if (strv("htreulqQaA").find(m) == strv::npos)
            return s::unexpected(
                subst_error("unrecognized modifier `" + str(1, m) + "'"));
          each(v, [&](const str& s) { return modify(s, m); });
          mods.remove_prefix(2);
        }
        if (not mods.empty()) return s::unexpected(subst_error("bad substitution"));
      } else {
        return s::unexpected(subst_error("bad substitution"));
      }
    } else {
      if (not v.set and not opts().get(OPT_UNSET) and not special and not nested)
        return s::unexpected(subst_error(name + ": parameter not set"));
      join_dq();
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
      vec<str> out;
      for (auto& s : v.list()) {
        if (fl.split_words) {
          StringInputSource in{s};
          Lexer lex{in};
          for (;;) {
            auto tok = lex.next_token();
            if (tok.is_eof()) break;
            if (tok.is_newline()) out.push_back(";");
            else out.push_back(tok.text);
          }
          continue;
        }
        if (fl.split_sep.empty()) {
          for (char ch : s) out.emplace_back(1, ch);
          continue;
        }
        s::size_t b = 0;
        for (;;) {
          auto e = s.find(fl.split_sep, b);
          str part = s.substr(b, e == str::npos ? str::npos : e - b);
          if (not part.empty()) out.push_back(part);
          if (e == str::npos) break;
          b = e + fl.split_sep.size();
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
    if (v.array and (fl.sort_asc or fl.sort_desc)) {
      auto key_less = [&](const str& a, const str& b) {
        if (fl.sort_numeric) {
          s::int64_t x = s::strtoll(a.c_str(), nullptr, 10);
          s::int64_t y = s::strtoll(b.c_str(), nullptr, 10);
          if (x != y) return x < y;
        }
        if (fl.sort_nocase) {
          return s::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
              [](char p, char q) {
                return s::tolower(static_cast<unsigned char>(p))
                     < s::tolower(static_cast<unsigned char>(q));
              });
        }
        return a < b;
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
    if (fl.quote == -1) each(v, [](const str& s) { return quote_minimal(s); });
    else if (fl.quote == 1) each(v, [](const str& s) { return quote_backslash(s); });
    else if (fl.quote >= 2) each(v, [](const str& s) { return quote_single(s); });

    // In double quotes a split result is joined again unless (@) is given.
    if (dq and v.array and not res.keep_separate) {
      str sep = ifs_first(), joined;
      for (s::size_t k = 0; k < v.arr.size(); ++k) {
        if (k) joined += sep;
        joined += v.arr[k];
      }
      v.make_scalar(joined);
    }

    res.v     = s::move(v);
    res.split = eq_split;
    res.glob  = glob;
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
      bool all_unlit = s::none_of(inner.lit.begin(), inner.lit.end(), [](bool l) { return l; });
      if (all_unlit) {
        if (auto seq = brace_sequence(inner.text)) {
          for (auto& s : *seq) {
            Field a;
            a.put(s, false);
            alts.push_back(s::move(a));
          }
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
    // Process substitution is handled by the executor.
    if (w.size() >= 3 and w.back() == ')'
        and (w.starts_with("<(") or w.starts_with(">(") or w.starts_with("=("))) {
      out.push_back(w);
      continue;
    }
    Scan scan{*this, Mode::Word};
    if (auto r = scan.scan(w); not r) return s::unexpected(r.error());

    vec<Field> fields;
    for (auto& f : scan.fields) {
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
        auto matches = glob_expand(GlobPattern::compile(f.as_pattern()), opts);
        if (not matches) return s::unexpected(matches.error());
        for (auto& m : *matches) out.push_back(s::move(m));
        continue;
      }
      out.push_back(s::move(f.text));
    }
  }
  return out;
}

Result<str> Expander::scalar(strv word, bool assignment) {
  Scan scan{*this, Mode::Scalar};
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
  lex.push_state(LEX_SKIPCOMMENTS);
  vec<str> raw;
  for (;;) {
    auto tok = lex.next_token();
    if (tok.is_eof()) break;
    if (tok.is_newline()) continue;
    if (tok.type != TokType::TK_WORD and tok.type < TokType::TK_IF)
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
