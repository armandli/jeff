#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <pwd.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <unordered_set>

#include <jfsh/builtins/math.h>
#include <jfsh/exec/executor.h>
#include <jfsh/exec/subst.h>
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

constexpr std::string_view DEFAULT_IFS = std::string_view(" \t\n\0", 4);
constexpr std::string_view PATTERN_CHARS = "*?[]()|<>^#~\\";

ShellError subst_error(std::string msg) {
  return ShellError{ErrorKind::BadSubstitution, std::move(msg)};
}

bool is_name_start(char c) noexcept {
  return std::isalpha(static_cast<unsigned char>(c)) or c == '_';
}
bool is_name_char(char c) noexcept {
  return std::isalnum(static_cast<unsigned char>(c)) or c == '_';
}
bool is_special_param(char c) noexcept {
  return std::string_view("?$!#-@*0").find(c) != std::string_view::npos;
}

// Index of the character that closes the bracket at w[open], skipping
// quoted text and nested substitutions; npos when it is never closed.
std::size_t find_close(std::string_view w, std::size_t open) {
  char o = w[open];
  char c = o == '(' ? ')' : o == '{' ? '}' : ']';
  int depth = 1;
  for (std::size_t i = open + 1; i < w.size(); ++i) {
    char ch = w[i];
    if (ch == '\\') { ++i; continue; }
    if (ch == '\'' and o != '[') {
      auto e = w.find('\'', i + 1);
      if (e == std::string_view::npos) return e;
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
      if (e == std::string_view::npos) return e;
      i = e;
    } else if (ch == o) {
      ++depth;
    } else if (ch == c and --depth == 0) {
      return i;
    }
  }
  return std::string_view::npos;
}

void append_utf8(std::string& out, unsigned long cp) {
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
std::string c_escapes(std::string_view s) {
  std::string out;
  for (std::size_t i = 0; i < s.size(); ++i) {
    if (s[i] != '\\' or i + 1 >= s.size()) { out += s[i]; continue; }
    char e = s[++i];
    auto hex = [&](std::size_t max) {
      unsigned long v = 0;
      std::size_t n = 0;
      while (n < max and i + 1 < s.size()
             and std::isxdigit(static_cast<unsigned char>(s[i + 1]))) {
        char h = s[++i];
        v = v * 16 + (std::isdigit(static_cast<unsigned char>(h))
                      ? static_cast<unsigned long>(h - '0')
                      : static_cast<unsigned long>(std::tolower(h) - 'a' + 10));
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

std::string quote_backslash(std::string_view s) {
  constexpr std::string_view SPECIAL = " \t\n'\"\\$`&|;<>()[]{}*?#~=%!^";
  std::string out;
  for (char c : s) {
    if (SPECIAL.find(c) != std::string_view::npos) out += '\\';
    out += c;
  }
  return out;
}

std::string quote_single(std::string_view s) {
  std::string out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else           out += c;
  }
  return out + "'";
}

// zsh's (q-): quote only when needed, with single quotes.
std::string quote_minimal(std::string_view s) {
  constexpr std::string_view SAFE =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_@%+=:,./-";
  if (not s.empty() and s.find_first_not_of(SAFE) == std::string_view::npos)
    return std::string(s);
  return quote_single(s);
}

// ---------------------------------------------------------------------------
// Fields: words under construction.  `lit[i]` marks text[i] as literal,
// i.e. quoted or produced by an expansion, so not pattern/brace/tilde syntax.
// ---------------------------------------------------------------------------
struct Field {
  std::string       text;
  std::vector<bool> lit;
  bool quoted   = false;   // had quotes, so it survives being empty
  bool empty_at = false;   // an empty "$@" landed here

  void put(char c, bool l) { text += c; lit.push_back(l); }
  void put(std::string_view s, bool l) { for (char c : s) put(c, l); }

  Field slice(std::size_t b, std::size_t e) const {
    Field f;
    f.text   = text.substr(b, e - b);
    f.lit    = std::vector<bool>(lit.begin() + static_cast<std::ptrdiff_t>(b),
                                 lit.begin() + static_cast<std::ptrdiff_t>(e));
    f.quoted = quoted;
    return f;
  }
  void append(const Field& o) {
    text += o.text;
    lit.insert(lit.end(), o.lit.begin(), o.lit.end());
  }
  bool unlit(std::size_t i) const { return i < text.size() and not lit[i]; }

  // The text as a pattern: literal pattern characters get a backslash.
  std::string as_pattern() const {
    std::string out;
    for (std::size_t i = 0; i < text.size(); ++i) {
      if (lit[i] and PATTERN_CHARS.find(text[i]) != std::string_view::npos) out += '\\';
      out += text[i];
    }
    return out;
  }
  bool has_unlit_glob() const {
    for (std::size_t i = 0; i < text.size(); ++i)
      if (not lit[i] and std::string_view("*?[(<^#~").find(text[i]) != std::string_view::npos)
        return true;
    return false;
  }
};

// A parameter's value while ${…} is being worked on.
struct Value {
  bool                     set   = false;
  bool                     array = false;
  std::vector<std::string> arr;
  std::string              str;

  std::vector<std::string> list() const {
    if (array) return arr;
    return {str};
  }
  void make_scalar(std::string s) { array = false; str = std::move(s); arr.clear(); }
  void make_array(std::vector<std::string> a) { array = true; arr = std::move(a); str.clear(); }
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
  std::string join_sep, split_sep;
};

enum class Mode { Word, Scalar, Heredoc };

struct Scan {
  Expander& ex;
  Mode      mode;
  std::vector<Field> fields{1};
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
  void put(std::string_view s, bool lit) { cur().put(s, lit); }
  void new_field(bool quoted) {
    pending_break = false;
    fields.emplace_back();
    fields.back().quoted = quoted;
  }
  // First part continues the current word; each further part starts one.
  void splice(const std::vector<std::string>& parts, bool lit, bool quoted) {
    for (std::size_t k = 0; k < parts.size(); ++k) {
      if (k) new_field(quoted);
      put(parts[k], lit);
    }
  }

  std::string ifs() {
    auto p = ctx().params.lookup("IFS");
    if (p and p->get().is_set()) return p->get().scalar();
    return std::string(DEFAULT_IFS);
  }
  std::string ifs_first() {
    std::string s = ifs();
    return s.empty() ? std::string{} : std::string(1, s[0]);
  }

  // Unquoted text split on $IFS joins the current word only if it doesn't
  // start with a separator, and ends it if it ends with one.
  void splice_split(std::string_view s, bool lit) {
    std::string sep = ifs();
    if (s.empty()) return;
    bool lead  = sep.find(s.front()) != std::string::npos;
    bool trail = sep.find(s.back())  != std::string::npos;
    auto parts = ifs_split(s, sep);
    if (lead and not cur().text.empty()) new_field(false);
    splice(parts, lit, false);
    if (trail) pending_break = true;
  }

  // -------------------------------------------------------------------------
  // The scanner proper.
  // -------------------------------------------------------------------------
  Result<void> scan(std::string_view w, bool dq = false) {
    for (std::size_t i = 0; i < w.size();) {
      char c = w[i];
      if (mode == Mode::Heredoc) {
        if (c == '\\' and i + 1 < w.size()
            and std::string_view("$`\\\n").find(w[i + 1]) != std::string_view::npos) {
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
          if (e == std::string_view::npos) e = w.size();
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
          std::size_t e = i + 2;
          while (e < w.size() and w[e] != '\'') e += (w[e] == '\\') ? 2u : 1u;
          put(c_escapes(w.substr(i + 2, std::min(e, w.size()) - i - 2)), true);
          i = e + 1;
          continue;
        }
      } else {
        if (c == '"') { dq = false; ++i; continue; }
        if (c == '\\' and i + 1 < w.size()
            and std::string_view("$`\"\\\n").find(w[i + 1]) != std::string_view::npos) {
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
  Result<void> substitution(std::string_view w, std::size_t& i, bool dq) {
    if (w[i] == '`') {
      std::string src;
      std::size_t j = i + 1;
      for (; j < w.size() and w[j] != '`'; ++j) {
        if (w[j] == '\\' and j + 1 < w.size()
            and (w[j + 1] == '`' or w[j + 1] == '\\' or w[j + 1] == '$'
                 or (dq and w[j + 1] == '"'))) ++j;
        src += w[j];
      }
      i = j + 1;
      return command_output(src, dq);
    }
    std::size_t n = i + 1;
    if (n >= w.size()) { put('$', dq or mode != Mode::Word); ++i; return {}; }
    char c = w[n];
    if (c == '(') {
      std::size_t e = find_close(w, n);
      if (e == std::string_view::npos) { put('$', true); ++i; return {}; }
      i = e + 1;
      // $((expr)) — but $( (subshell) ) and $((a) | b) are commands.
      if (n + 1 < e and w[n + 1] == '(' and w[e - 1] == ')'
          and find_close(w, n + 1) == e - 1)
        return arithmetic(w.substr(n + 2, e - n - 3), dq);
      return command_output(w.substr(n + 1, e - n - 1), dq);
    }
    if (c == '[') {
      std::size_t e = find_close(w, n);
      if (e == std::string_view::npos) { put('$', true); ++i; return {}; }
      i = e + 1;
      return arithmetic(w.substr(n + 1, e - n - 1), dq);
    }
    if (c == '{') {
      std::size_t e = find_close(w, n);
      if (e == std::string_view::npos)
        return std::unexpected(subst_error("bad substitution"));
      i = e + 1;
      return param(w.substr(n + 1, e - n - 1), dq);
    }
    // Unbraced: $name, $name[sub], $1, $?, $#name, $=name, $~name.
    std::size_t j = n;
    std::string expr;
    while (j < w.size() and (w[j] == '=' or w[j] == '~')) ++j;
    if (j < w.size() and w[j] == '#' and j + 1 < w.size() and is_name_start(w[j + 1])) ++j;
    expr = std::string(w.substr(n, j - n));
    if (j < w.size() and is_name_start(w[j])) {
      std::size_t s = j;
      while (j < w.size() and is_name_char(w[j])) ++j;
      expr += w.substr(s, j - s);
      if (j < w.size() and w[j] == '[') {
        std::size_t e = find_close(w, j);
        if (e != std::string_view::npos) {
          expr += w.substr(j, e - j + 1);
          j = e + 1;
        }
      }
    } else if (j < w.size() and (std::isdigit(static_cast<unsigned char>(w[j]))
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

  Result<void> command_output(std::string_view src, bool dq) {
    auto out = run_command(src);
    if (not out) return std::unexpected(out.error());
    if (dq or mode != Mode::Word) {
      put(*out, true);
      return {};
    }
    splice_split(*out, not opts().get(OPT_GLOBSUBST));
    return {};
  }

  // Runs `src` in a subshell and returns its output without trailing
  // newlines; $? becomes its status.
  Result<std::string> run_command(std::string_view src) {
    int pfd[2];
    if (::pipe(pfd) != 0)
      return std::unexpected(ShellError::from_errno(ErrorKind::ExecError, "pipe"));
    std::fflush(nullptr);
    pid_t pid = ::fork();
    if (pid < 0) {
      ::close(pfd[0]);
      ::close(pfd[1]);
      return std::unexpected(ShellError::from_errno(ErrorKind::ExecError, "fork"));
    }
    if (pid == 0) {
      ::close(pfd[0]);
      if (pfd[1] != 1) {
        ::dup2(pfd[1], 1);
        ::close(pfd[1]);
      }
      int rc = ex.exec().exec_string(src);
      std::fflush(nullptr);
      _exit(rc);
    }
    ::close(pfd[1]);
    std::string out;
    char buf[4096];
    for (;;) {
      ssize_t r = ::read(pfd[0], buf, sizeof buf);
      if (r < 0 and errno == EINTR) continue;
      if (r <= 0) break;
      out.append(buf, static_cast<std::size_t>(r));
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

  Result<std::string> expand_scalar(std::string_view text, Mode m = Mode::Heredoc) {
    Scan sub{ex, m};
    if (auto r = sub.scan(text); not r) return std::unexpected(r.error());
    std::string out;
    for (std::size_t k = 0; k < sub.fields.size(); ++k) {
      if (k) out += ' ';
      out += sub.fields[k].text;
    }
    return out;
  }

  Result<long long> arith(std::string_view expr) {
    auto text = expand_scalar(expr);
    if (not text) return std::unexpected(text.error());
    return arith_eval(*text, ctx().params);
  }

  Result<void> arithmetic(std::string_view expr, bool dq) {
    auto v = arith(expr);
    if (not v) return std::unexpected(v.error());
    put(std::to_string(*v), true);
    (void)dq;
    return {};
  }

  // -------------------------------------------------------------------------
  // Parameters.
  // -------------------------------------------------------------------------
  std::string option_letters() {
    std::string s;
    auto& o = opts();
    if (o.get(OPT_ERREXIT))     s += 'e';
    if (o.get(OPT_HASHCMDS))    s += 'h';
    if (o.interactive())        s += 'i';
    if (o.get(OPT_MONITOR))     s += 'm';
    if (not o.get(OPT_UNSET))   s += 'u';
    if (o.get(OPT_VERBOSE))     s += 'v';
    if (o.get(OPT_XTRACE))      s += 'x';
    return s;
  }

  Value get_param(std::string_view name, const ParamFlags& fl) {
    Value v;
    auto scalar = [&](std::string s) { v.set = true; v.make_scalar(std::move(s)); };
    auto& c = ctx();
    if (name == "?") { scalar(std::to_string(c.last_exit)); return v; }
    if (name == "$") { scalar(std::to_string(c.shell_pid)); return v; }
    if (name == "!") { scalar(std::to_string(c.last_bg_pid < 0 ? 0 : c.last_bg_pid)); return v; }
    if (name == "#" or name == "ARGC") { scalar(std::to_string(c.positional.size())); return v; }
    if (name == "-") { scalar(option_letters()); return v; }
    if (name == "0") { scalar(c.arg0); return v; }
    if (name == "@" or name == "*" or name == "argv") {
      v.set = true;
      v.make_array(c.positional);
      return v;
    }
    if (not name.empty() and std::isdigit(static_cast<unsigned char>(name[0]))) {
      std::size_t n = std::strtoul(std::string(name).c_str(), nullptr, 10);
      if (n >= 1 and n <= c.positional.size()) scalar(c.positional[n - 1]);
      return v;
    }
    if (name == "PPID")   { scalar(std::to_string(::getppid())); return v; }
    if (name == "RANDOM") { scalar(std::to_string(std::rand() % 32768)); return v; }

    auto p = c.params.lookup(name);
    if (not p or not p->get().is_set()) return v;
    const Param& param = p->get();
    v.set = true;
    if (param.is_hash()) {
      auto h = param.hash();
      std::vector<std::string> keys;
      for (auto& [k, _] : h) keys.push_back(k);
      std::sort(keys.begin(), keys.end());
      std::vector<std::string> out;
      for (auto& k : keys) {
        if (fl.keys) out.push_back(k);
        if (fl.values or not fl.keys) out.push_back(h[k]);
      }
      v.make_array(std::move(out));
    } else if (param.is_array()) {
      v.make_array(param.array());
    } else if (param.is_integer()) {
      v.make_scalar(std::to_string(param.integer()));
    } else if (param.is_float()) {
      v.make_scalar(std::to_string(param.floatv()));
    } else {
      v.make_scalar(param.scalar());
    }
    return v;
  }

  std::string type_of(std::string_view name) {
    auto p = ctx().params.lookup(name);
    if (not p or not p->get().is_set()) {
      if (name == "@" or name == "*" or name == "argv") return "array-special";
      if (name == "?" or name == "$" or name == "#" or name == "!") return "integer-special";
      return {};
    }
    const Param& param = p->get();
    std::string t = param.is_hash() ? "association" : param.is_array() ? "array"
                  : param.is_integer() ? "integer" : param.is_float() ? "float" : "scalar";
    if (param.flags & PM_LOCAL)    t += "-local";
    if (param.is_readonly())       t += "-readonly";
    if (param.is_exported())       t += "-export";
    if (param.flags & PM_SPECIAL)  t += "-special";
    return t;
  }

  Result<ParamFlags> parse_flags(std::string_view f) {
    ParamFlags fl;
    for (std::size_t i = 0; i < f.size(); ++i) {
      char c = f[i];
      auto delimited = [&]() -> Result<std::string> {
        if (i + 1 >= f.size()) return std::unexpected(subst_error("error in flags"));
        char open = f[++i];
        char close = open == '(' ? ')' : open == '[' ? ']' : open == '{' ? '}'
                   : open == '<' ? '>' : open;
        auto e = f.find(close, i + 1);
        if (e == std::string_view::npos) return std::unexpected(subst_error("error in flags"));
        std::string arg(f.substr(i + 1, e - i - 1));
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
          if (not a) return std::unexpected(a.error());
          fl.join = true;
          fl.join_sep = *a;
        }
        break; case 's': {
          auto a = delimited();
          if (not a) return std::unexpected(a.error());
          fl.split = true;
          fl.split_sep = *a;
        }
        break; default:
          return std::unexpected(subst_error(std::string("error in flags: ") + c));
      }
    }
    if (fl.escapes) {
      fl.join_sep  = c_escapes(fl.join_sep);
      fl.split_sep = c_escapes(fl.split_sep);
    }
    return fl;
  }

  // Index of the end of the nested substitution that starts at t[i].
  static std::size_t nested_end(std::string_view t, std::size_t i) {
    if (t[i] == '"') {
      for (++i; i < t.size() and t[i] != '"'; ++i)
        if (t[i] == '\\') ++i;
      return std::min(i + 1, t.size());
    }
    // t[i] == '$'
    if (i + 1 < t.size() and (t[i + 1] == '{' or t[i + 1] == '(')) {
      auto e = find_close(t, i + 1);
      return e == std::string_view::npos ? t.size() : e + 1;
    }
    std::size_t j = i + 1;
    if (j < t.size() and (is_special_param(t[j]) or std::isdigit(static_cast<unsigned char>(t[j]))))
      return j + 1;
    while (j < t.size() and is_name_char(t[j])) ++j;
    return j;
  }

  // Expands an operand of ${x:-word} etc.  Several words make an array.
  Result<Value> operand(std::string_view word, bool dq) {
    Scan sub{ex, mode == Mode::Word ? Mode::Word : Mode::Scalar};
    if (auto r = sub.scan(word, dq); not r) return std::unexpected(r.error());
    Value v;
    v.set = true;
    std::vector<std::string> out;
    for (auto& f : sub.fields)
      if (not f.text.empty() or f.quoted) out.push_back(f.text);
    if (out.size() <= 1) v.make_scalar(out.empty() ? "" : out[0]);
    else                 v.make_array(std::move(out));
    return v;
  }

  Result<CompiledPattern> operand_pattern(std::string_view word, bool dq) {
    Scan sub{ex, Mode::Scalar};
    if (auto r = sub.scan(word, dq); not r) return std::unexpected(r.error());
    std::string pat;
    for (std::size_t k = 0; k < sub.fields.size(); ++k) {
      if (k) pat += ' ';
      pat += sub.fields[k].as_pattern();
    }
    return CompiledPattern::compile(pat, false, opts().extended_glob());
  }

  Result<void> apply_subscript(Value& v, std::string_view sub, std::string_view name,
                               bool& all) {
    if (sub == "@" or sub == "*") {
      all = sub == "@";
      if (not v.array and v.set) v.make_array({v.str});
      return {};
    }
    // Associative arrays are indexed by key.
    if (auto p = ctx().params.lookup(name); p and p->get().is_hash()) {
      auto key = expand_scalar(sub, Mode::Scalar);
      if (not key) return std::unexpected(key.error());
      auto h = p->get().hash();
      auto it = h.find(*key);
      if (it == h.end()) { v.set = false; v.make_scalar(""); }
      else               v.make_scalar(it->second);
      return {};
    }
    std::size_t comma = std::string_view::npos;
    int depth = 0;
    for (std::size_t k = 0; k < sub.size(); ++k) {
      if (sub[k] == '(' or sub[k] == '[') ++depth;
      else if (sub[k] == ')' or sub[k] == ']') --depth;
      else if (sub[k] == ',' and depth == 0) { comma = k; break; }
    }
    auto lo = arith(sub.substr(0, comma));
    if (not lo) return std::unexpected(lo.error());
    std::optional<long long> hi;
    if (comma != std::string_view::npos) {
      auto h = arith(sub.substr(comma + 1));
      if (not h) return std::unexpected(h.error());
      hi = *h;
    }
    auto norm = [](long long i, long long n) { return i < 0 ? n + i + 1 : i; };
    if (v.array) {
      long long n = static_cast<long long>(v.arr.size());
      long long a = norm(*lo, n);
      if (not hi) {
        if (a >= 1 and a <= n) v.make_scalar(v.arr[static_cast<std::size_t>(a - 1)]);
        else                   v.make_scalar("");
        return {};
      }
      long long b = std::min(norm(*hi, n), n);
      a = std::max(a, 1LL);
      std::vector<std::string> out;
      for (long long k = a; k <= b; ++k) out.push_back(v.arr[static_cast<std::size_t>(k - 1)]);
      v.make_array(std::move(out));
      return {};
    }
    long long n = static_cast<long long>(v.str.size());
    long long a = norm(*lo, n);
    long long b = hi ? norm(*hi, n) : a;
    a = std::max(a, 1LL);
    b = std::min(b, n);
    v.make_scalar(a > b ? std::string{}
                        : v.str.substr(static_cast<std::size_t>(a - 1),
                                       static_cast<std::size_t>(b - a + 1)));
    return {};
  }

  static std::string modify(std::string s, char m) {
    switch (m) {
      case 'h': {
        auto slash = s.rfind('/');
        if (slash == std::string::npos) return ".";
        return slash == 0 ? "/" : s.substr(0, slash);
      }
      case 't': {
        auto slash = s.rfind('/');
        return slash == std::string::npos ? s : s.substr(slash + 1);
      }
      case 'r': {
        auto dot = s.rfind('.');
        auto slash = s.rfind('/');
        if (dot == std::string::npos or (slash != std::string::npos and dot < slash)) return s;
        return s.substr(0, dot);
      }
      case 'e': {
        auto dot = s.rfind('.');
        auto slash = s.rfind('/');
        if (dot == std::string::npos or (slash != std::string::npos and dot < slash)) return {};
        return s.substr(dot + 1);
      }
      case 'u':
        for (auto& ch : s) ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
        return s;
      case 'l':
        for (auto& ch : s) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
        return s;
      case 'q': return quote_backslash(s);
      case 'Q': return remove_quotes(s);
      case 'a': case 'A': {
        if (s.empty() or s[0] != '/') {
          char buf[PATH_MAX];
          if (::getcwd(buf, sizeof buf)) s = std::string(buf) + "/" + s;
        }
        if (m == 'A') {
          char buf[PATH_MAX];
          if (::realpath(s.c_str(), buf)) return buf;
        }
        // Lexically remove `.` and `..` components.
        std::vector<std::string> parts;
        std::size_t k = 0;
        while (k <= s.size()) {
          auto e = s.find('/', k);
          if (e == std::string::npos) e = s.size();
          std::string part = s.substr(k, e - k);
          if (part == "..") { if (not parts.empty()) parts.pop_back(); }
          else if (not part.empty() and part != ".") parts.push_back(part);
          k = e + 1;
        }
        std::string out;
        for (auto& p : parts) out += "/" + p;
        return out.empty() ? "/" : out;
      }
      default: return s;
    }
  }

  template <typename F>
  static void each(Value& v, F fn) {
    if (v.array) for (auto& e : v.arr) e = fn(e);
    else         v.str = fn(v.str);
  }

  // ${name/pat/repl}, ${name//pat/repl}, ${name:/pat/repl}.
  static std::string replace(const std::string& s, const CompiledPattern& pat,
                             const std::string& repl, bool global, char anchor) {
    std::size_t n = s.size();
    if (anchor == 'w') return pat.matches(s) ? repl : s;
    if (anchor == '#') {
      std::size_t len = pat.match_prefix(s, true);
      return len == std::string::npos ? s : repl + s.substr(len);
    }
    if (anchor == '%') {
      for (std::size_t b = 0; b <= n; ++b)
        if (pat.matches(std::string_view(s).substr(b))) return s.substr(0, b) + repl;
      return s;
    }
    std::string out;
    std::size_t b = 0;
    bool done = false;
    while (b <= n) {
      std::size_t len = done ? std::string::npos
                             : pat.match_prefix(std::string_view(s).substr(b), true);
      if (len == std::string::npos) {
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
  Result<void> param(std::string_view t, bool dq) {
    auto r = param_value(t, dq);
    if (not r) return std::unexpected(r.error());
    auto& [v, keep_separate, split, glob] = *r;
    if (mode != Mode::Word) {
      if (v.array) {
        std::string joined;
        for (std::size_t k = 0; k < v.arr.size(); ++k) {
          if (k) joined += ' ';
          joined += v.arr[k];
        }
        put(joined, true);
      } else {
        put(v.str, true);
      }
      return {};
    }
    if (dq) {
      if (v.array and keep_separate) {
        if (v.arr.empty()) cur().empty_at = true;
        splice(v.arr, true, true);
      } else if (v.array) {
        std::string sep = ifs_first(), joined;
        for (std::size_t k = 0; k < v.arr.size(); ++k) {
          if (k) joined += sep;
          joined += v.arr[k];
        }
        put(joined, true);
      } else {
        put(v.str, true);
      }
      return {};
    }
    bool lit = not glob;
    if (v.array) {
      std::vector<std::string> parts;
      for (auto& e : v.arr) if (not e.empty()) parts.push_back(e);
      if (split) {
        std::string sep = ifs();
        std::vector<std::string> words;
        for (auto& e : parts)
          for (auto& w : ifs_split(e, sep)) words.push_back(w);
        parts = std::move(words);
      }
      splice(parts, lit, false);
      return {};
    }
    if (split) splice_split(v.str, lit);
    else       put(v.str, lit);
    return {};
  }

  struct ParamResult {
    Value v;
    bool  keep_separate = false;   // "$@", "${a[@]}", "${(@)a}"
    bool  split         = false;
    bool  glob          = false;
  };

  Result<ParamResult> param_value(std::string_view t, bool dq) {
    ParamResult res;
    std::size_t i = 0;
    ParamFlags fl;
    if (i < t.size() and t[i] == '(') {
      auto e = find_close(t, i);
      if (e == std::string_view::npos) return std::unexpected(subst_error("bad substitution"));
      auto f = parse_flags(t.substr(i + 1, e - i - 1));
      if (not f) return std::unexpected(f.error());
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
                  or std::isdigit(static_cast<unsigned char>(n));
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
    std::string name;
    Value v;
    bool nested = false;
    if (i + 1 < t.size() and (t[i] == '$' or t[i] == '"')) {
      nested = true;
      std::size_t e = nested_end(t, i);
      std::string_view inner = t.substr(i, e - i);
      if (inner.size() > 2 and inner[0] == '$' and inner[1] == '{') {
        auto r = param_value(inner.substr(2, inner.size() - 3), dq);
        if (not r) return std::unexpected(r.error());
        v = r->v;
        res.keep_separate = r->keep_separate;
      } else if (inner.size() > 1 and inner[0] == '$'
                 and inner[1] != '(' and inner[1] != '[') {
        auto r = param_value(inner.substr(1), dq);
        if (not r) return std::unexpected(r.error());
        v = r->v;
        res.keep_separate = r->keep_separate;
      } else {
        auto s = expand_scalar(inner, Mode::Scalar);
        if (not s) return std::unexpected(s.error());
        v.set = true;
        v.make_scalar(*s);
      }
      i = e;
    } else {
      std::size_t s = i;
      if (i < t.size() and is_name_start(t[i])) {
        while (i < t.size() and is_name_char(t[i])) ++i;
      } else if (i < t.size() and std::isdigit(static_cast<unsigned char>(t[i]))) {
        while (i < t.size() and std::isdigit(static_cast<unsigned char>(t[i]))) ++i;
      } else if (i < t.size() and is_special_param(t[i])) {
        ++i;
      } else {
        return std::unexpected(subst_error("bad substitution"));
      }
      name = std::string(t.substr(s, i - s));
      if (fl.indirect) {
        Value ref = get_param(name, fl);
        name = ref.array ? (ref.arr.empty() ? "" : ref.arr[0]) : ref.str;
      }
      if (fl.type) {
        v.set = true;
        v.make_scalar(type_of(name));
      } else {
        v = get_param(name, fl);
      }
      if (name == "@") res.keep_separate = true;
    }

    // Subscripts, possibly several: ${a[2][1]}.
    while (i < t.size() and t[i] == '[') {
      std::size_t e = find_close(t, i);
      if (e == std::string_view::npos) return std::unexpected(subst_error("bad substitution"));
      bool all = false;
      if (auto r = apply_subscript(v, t.substr(i + 1, e - i - 1), nested ? "" : name, all); not r)
        return std::unexpected(r.error());
      if (all) res.keep_separate = true;
      i = e + 1;
    }
    if (fl.at) res.keep_separate = true;

    if (plus) {
      Value out;
      out.set = true;
      out.make_scalar(v.set ? "1" : "0");
      res.v = std::move(out);
      return res;
    }

    std::string_view op = t.substr(i);
    bool special = not name.empty() and (is_special_param(name[0])
                    or std::isdigit(static_cast<unsigned char>(name[0])));

    // Double-quoted joining happens before the operators.
    auto join_dq = [&] {
      if (dq and v.array and not res.keep_separate and not length) {
        std::string sep = fl.join ? fl.join_sep : ifs_first(), joined;
        for (std::size_t k = 0; k < v.arr.size(); ++k) {
          if (k) joined += sep;
          joined += v.arr[k];
        }
        v.make_scalar(joined);
      }
    };

    if (not op.empty()) {
      bool colon = op[0] == ':';
      std::string_view rest = colon ? op.substr(1) : op;
      char o = rest.empty() ? 0 : rest[0];
      bool null = not v.set or (v.array ? v.arr.empty() : v.str.empty());
      bool empty_test = colon ? null : not v.set;
      if (o == '-' or o == '=' or o == '?' or o == '+') {
        std::string_view word = rest.substr(1);
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
              if (not w) return std::unexpected(w.error());
              v = *w;
            }
          break; case '+':
            if (empty_test) { v.set = true; v.make_scalar(""); }
            else {
              auto w = operand(word, dq);
              if (not w) return std::unexpected(w.error());
              v = *w;
            }
          break; case '=':
            if (empty_test or force_assign) {
              if (nested or special)
                return std::unexpected(subst_error("can't set " + std::string(name)));
              auto w = operand(word, dq);
              if (not w) return std::unexpected(w.error());
              Result<void> r = w->array ? ctx().params.set_array(name, w->arr)
                                        : ctx().params.set_scalar(name, w->str);
              if (not r) return std::unexpected(r.error());
              v = *w;
            }
          break; default:  // '?'
            if (empty_test) {
              std::string msg = word.empty()
                  ? (v.set ? "parameter null" : "parameter not set")
                  : std::string(word);
              if (not word.empty()) {
                auto w = expand_scalar(word, Mode::Scalar);
                if (w) msg = *w;
              }
              return std::unexpected(subst_error(name + ": " + msg));
            }
        }
        join_dq();
      } else if (not colon and (o == '#' or o == '%')) {
        if (not v.set and not opts().get(OPT_UNSET) and not special)
          return std::unexpected(subst_error(name + ": parameter not set"));
        join_dq();
        bool longest = rest.size() > 1 and rest[1] == o;
        auto pat = operand_pattern(rest.substr(longest ? 2 : 1), dq);
        if (not pat) return std::unexpected(pat.error());
        each(v, [&](const std::string& s) {
          if (o == '#') {
            std::size_t len = pat->match_prefix(s, longest);
            return len == std::string::npos ? s : s.substr(len);
          }
          // Suffix: the shortest suffix starts as far right as possible.
          std::size_t n = s.size();
          for (std::size_t k = 0; k <= n; ++k) {
            std::size_t b = longest ? k : n - k;
            if (pat->matches(std::string_view(s).substr(b))) return s.substr(0, b);
          }
          return s;
        });
      } else if (o == '/') {
        join_dq();
        std::string_view body = rest.substr(1);
        bool global = false;
        char anchor = colon ? 'w' : 0;
        if (not colon and not body.empty() and body[0] == '/') { global = true; body.remove_prefix(1); }
        else if (not body.empty() and (body[0] == '#' or body[0] == '%')) {
          anchor = body[0];
          body.remove_prefix(1);
        }
        std::size_t slash = std::string_view::npos;
        for (std::size_t k = 0; k < body.size(); ++k) {
          if (body[k] == '\\') { ++k; continue; }
          if (body[k] == '/') { slash = k; break; }
        }
        auto pat = operand_pattern(body.substr(0, slash), dq);
        if (not pat) return std::unexpected(pat.error());
        std::string repl;
        if (slash != std::string_view::npos) {
          auto r = expand_scalar(body.substr(slash + 1), Mode::Scalar);
          if (not r) return std::unexpected(r.error());
          repl = *r;
        }
        each(v, [&](const std::string& s) { return replace(s, *pat, repl, global, anchor); });
      } else if (colon and (o == 0 or std::isdigit(static_cast<unsigned char>(o))
                            or o == ' ' or o == '(' or o == '$')) {
        // ${x:offset} / ${x:offset:length}, 0-based.
        join_dq();
        std::size_t c2 = find_top_colon(rest);
        auto off = arith(rest.substr(0, c2));
        if (not off) return std::unexpected(off.error());
        std::optional<long long> len;
        if (c2 != std::string_view::npos) {
          auto l = arith(rest.substr(c2 + 1));
          if (not l) return std::unexpected(l.error());
          len = *l;
        }
        auto cut = [&](long long n, long long& b, long long& e) {
          b = *off < 0 ? std::max(0LL, n + *off) : std::min(*off, n);
          e = not len ? n : (*len < 0 ? std::max(b, n + *len) : std::min(n, b + *len));
        };
        if (v.array) {
          long long b, e;
          cut(static_cast<long long>(v.arr.size()), b, e);
          v.make_array(std::vector<std::string>(v.arr.begin() + b, v.arr.begin() + e));
        } else {
          long long b, e;
          cut(static_cast<long long>(v.str.size()), b, e);
          v.make_scalar(v.str.substr(static_cast<std::size_t>(b), static_cast<std::size_t>(e - b)));
        }
      } else if (colon and std::string_view("htreulqQaA").find(o) != std::string_view::npos) {
        join_dq();
        // :h :t … modifiers, possibly chained (`:t:r`).
        std::string_view mods = op;
        while (mods.size() >= 2 and mods[0] == ':') {
          char m = mods[1];
          if (std::string_view("htreulqQaA").find(m) == std::string_view::npos)
            return std::unexpected(subst_error("unrecognized modifier `" + std::string(1, m) + "'"));
          each(v, [&](const std::string& s) { return modify(s, m); });
          mods.remove_prefix(2);
        }
        if (not mods.empty()) return std::unexpected(subst_error("bad substitution"));
      } else {
        return std::unexpected(subst_error("bad substitution"));
      }
    } else {
      if (not v.set and not opts().get(OPT_UNSET) and not special and not nested)
        return std::unexpected(subst_error(name + ": parameter not set"));
      join_dq();
    }

    if (length) {
      std::size_t n = v.array ? v.arr.size() : v.str.size();
      v.set = true;
      v.make_scalar(std::to_string(n));
      res.v = std::move(v);
      return res;
    }

    if (fl.join and v.array) {
      std::string joined;
      for (std::size_t k = 0; k < v.arr.size(); ++k) {
        if (k) joined += fl.join_sep;
        joined += v.arr[k];
      }
      v.make_scalar(joined);
    }
    if (fl.split or fl.split_words) {
      std::vector<std::string> out;
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
        std::size_t b = 0;
        for (;;) {
          auto e = s.find(fl.split_sep, b);
          std::string part = s.substr(b, e == std::string::npos ? std::string::npos : e - b);
          if (not part.empty()) out.push_back(part);
          if (e == std::string::npos) break;
          b = e + fl.split_sep.size();
        }
      }
      v.make_array(std::move(out));
      eq_split = false;
    }
    if (fl.upper or fl.lower or fl.capitalize) {
      each(v, [&](std::string s) {
        bool start = true;
        for (auto& ch : s) {
          unsigned char u = static_cast<unsigned char>(ch);
          if (fl.upper) ch = static_cast<char>(std::toupper(u));
          else if (fl.lower) ch = static_cast<char>(std::tolower(u));
          else {
            ch = static_cast<char>(start ? std::toupper(u) : std::tolower(u));
            start = not std::isalnum(u);
          }
        }
        return s;
      });
    }
    if (v.array and (fl.sort_asc or fl.sort_desc)) {
      auto key_less = [&](const std::string& a, const std::string& b) {
        if (fl.sort_numeric) {
          long long x = std::strtoll(a.c_str(), nullptr, 10);
          long long y = std::strtoll(b.c_str(), nullptr, 10);
          if (x != y) return x < y;
        }
        if (fl.sort_nocase) {
          return std::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
              [](char p, char q) {
                return std::tolower(static_cast<unsigned char>(p))
                     < std::tolower(static_cast<unsigned char>(q));
              });
        }
        return a < b;
      };
      std::stable_sort(v.arr.begin(), v.arr.end(), key_less);
      if (fl.sort_desc) std::reverse(v.arr.begin(), v.arr.end());
    }
    if (v.array and fl.unique) {
      std::unordered_set<std::string> seen;
      std::vector<std::string> out;
      for (auto& e : v.arr) if (seen.insert(e).second) out.push_back(e);
      v.arr = std::move(out);
    }
    if (fl.unquote) each(v, [](const std::string& s) { return remove_quotes(s); });
    if (fl.quote == -1) each(v, [](const std::string& s) { return quote_minimal(s); });
    else if (fl.quote == 1) each(v, [](const std::string& s) { return quote_backslash(s); });
    else if (fl.quote >= 2) each(v, [](const std::string& s) { return quote_single(s); });

    // In double quotes a split result is joined again unless (@) is given.
    if (dq and v.array and not res.keep_separate) {
      std::string sep = ifs_first(), joined;
      for (std::size_t k = 0; k < v.arr.size(); ++k) {
        if (k) joined += sep;
        joined += v.arr[k];
      }
      v.make_scalar(joined);
    }

    res.v     = std::move(v);
    res.split = eq_split;
    res.glob  = glob;
    return res;
  }

  static std::size_t find_top_colon(std::string_view s) {
    int depth = 0;
    for (std::size_t k = 0; k < s.size(); ++k) {
      if (s[k] == '(') ++depth;
      else if (s[k] == ')') --depth;
      else if (s[k] == ':' and depth == 0) return k;
    }
    return std::string_view::npos;
  }
};

// ---------------------------------------------------------------------------
// Brace expansion.
// ---------------------------------------------------------------------------

// Parses `a..b` or `a..b..step` (numbers or single characters) and returns
// the sequence, or nothing when `s` is not a sequence.
std::optional<std::vector<std::string>> brace_sequence(std::string_view s) {
  auto dots = s.find("..");
  if (dots == std::string_view::npos) return std::nullopt;
  std::string_view a = s.substr(0, dots), rest = s.substr(dots + 2);
  std::string_view b = rest, step_s;
  if (auto d2 = rest.find(".."); d2 != std::string_view::npos) {
    b = rest.substr(0, d2);
    step_s = rest.substr(d2 + 2);
  }
  auto is_int = [](std::string_view x) {
    if (x.empty()) return false;
    std::size_t k = (x[0] == '-' or x[0] == '+') ? 1 : 0;
    if (k == x.size()) return false;
    for (; k < x.size(); ++k)
      if (not std::isdigit(static_cast<unsigned char>(x[k]))) return false;
    return true;
  };
  long long step = 1;
  if (not step_s.empty()) {
    if (not is_int(step_s)) return std::nullopt;
    long long v = std::strtoll(std::string(step_s).c_str(), nullptr, 10);
    step = v == LLONG_MIN ? LLONG_MAX : std::llabs(v);
    if (step == 0) step = 1;
  }
  std::vector<std::string> out;
  if (is_int(a) and is_int(b)) {
    long long x = std::strtoll(std::string(a).c_str(), nullptr, 10);
    long long y = std::strtoll(std::string(b).c_str(), nullptr, 10);
    auto padded = [](std::string_view v) {
      std::string_view d = (v[0] == '-' or v[0] == '+') ? v.substr(1) : v;
      return d.size() > 1 and d[0] == '0';
    };
    std::size_t width = (padded(a) or padded(b)) ? std::max(a.size(), b.size()) : 0;
    // Step towards y without overflowing past it.
    using U = unsigned long long;
    for (long long k = x;;) {
      out.push_back(std::to_string(k));
      U left = x <= y ? static_cast<U>(y) - static_cast<U>(k) : static_cast<U>(k) - static_cast<U>(y);
      if (left < static_cast<U>(step)) break;
      k = x <= y ? k + step : k - step;
    }
    if (width)
      for (auto& e : out) {
        bool neg = e[0] == '-';
        std::string digits = neg ? e.substr(1) : e;
        std::size_t want = width - (neg ? 1 : 0);
        if (digits.size() < want) digits.insert(0, want - digits.size(), '0');
        e = (neg ? "-" : "") + digits;
      }
    return out;
  }
  if (a.size() == 1 and b.size() == 1) {
    int x = static_cast<unsigned char>(a[0]), y = static_cast<unsigned char>(b[0]);
    if (x <= y) for (int k = x; k <= y; k += static_cast<int>(step)) out.emplace_back(1, static_cast<char>(k));
    else        for (int k = x; k >= y; k -= static_cast<int>(step)) out.emplace_back(1, static_cast<char>(k));
    return out;
  }
  return std::nullopt;
}

void brace_expand(const Field& f, bool ccl, std::vector<Field>& out) {
  for (std::size_t b = 0; b < f.text.size(); ++b) {
    if (f.text[b] != '{' or not f.unlit(b)) continue;
    // Find the matching unquoted `}` and the top-level commas.
    int depth = 0;
    std::size_t e = std::string::npos;
    std::vector<std::size_t> commas;
    for (std::size_t k = b + 1; k < f.text.size(); ++k) {
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
    if (e == std::string::npos) return out.push_back(f);
    Field prefix = f.slice(0, b), suffix = f.slice(e + 1, f.text.size());
    std::vector<Field> alts;
    if (not commas.empty()) {
      std::size_t s = b + 1;
      for (std::size_t c : commas) { alts.push_back(f.slice(s, c)); s = c + 1; }
      alts.push_back(f.slice(s, e));
    } else {
      Field inner = f.slice(b + 1, e);
      bool all_unlit = std::none_of(inner.lit.begin(), inner.lit.end(), [](bool l) { return l; });
      if (all_unlit) {
        if (auto seq = brace_sequence(inner.text)) {
          for (auto& s : *seq) {
            Field a;
            a.put(s, false);
            alts.push_back(std::move(a));
          }
        }
      }
      if (alts.empty() and ccl and not inner.text.empty()) {
        std::vector<char> chars(inner.text.begin(), inner.text.end());
        std::sort(chars.begin(), chars.end());
        chars.erase(std::unique(chars.begin(), chars.end()), chars.end());
        for (char c : chars) {
          Field a;
          a.put(c, true);
          alts.push_back(std::move(a));
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

std::string remove_quotes(std::string_view w) {
  constexpr std::string_view DQUOTE_ESCAPES = "$`\"\\\n";
  enum class Quote { None, Single, Double } q = Quote::None;
  std::string out;
  out.reserve(w.size());
  for (std::size_t i = 0; i < w.size(); ++i) {
    char c = w[i];
    bool escape = c == '\\' and i + 1 < w.size()
        and (q == Quote::None
             or (q == Quote::Double
                 and DQUOTE_ESCAPES.find(w[i + 1]) != std::string_view::npos));
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
      std::size_t e = i + 2;
      while (e < w.size() and w[e] != '\'') e += (w[e] == '\\') ? 2u : 1u;
      out += c_escapes(w.substr(i + 2, std::min(e, w.size()) - i - 2));
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

std::vector<std::string> ifs_split(std::string_view s, std::string_view ifs) {
  auto is_sep = [&](char c) { return ifs.find(c) != std::string_view::npos; };
  auto is_ws  = [&](char c) { return is_sep(c) and (c == ' ' or c == '\t' or c == '\n'); };
  auto skip_ws = [&](std::size_t& i) { while (i < s.size() and is_ws(s[i])) ++i; };
  std::vector<std::string> out;
  std::string cur;
  bool open = false;   // a word has started and not been pushed yet
  std::size_t i = 0;
  skip_ws(i);
  while (i < s.size()) {
    if (not is_sep(s[i])) {
      cur += s[i++];
      open = true;
      continue;
    }
    // One separator: a run of IFS whitespace with at most one other IFS
    // character in it.
    out.push_back(std::move(cur));
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
  if (open) out.push_back(std::move(cur));
  return out;
}

// ---------------------------------------------------------------------------
// Expander.
// ---------------------------------------------------------------------------
namespace {

// `~`, `~+`, `~-`, `~user` at text[p] (unquoted); `end` bounds the name.
Result<void> tilde_at(Field& f, std::size_t p, std::size_t end, ExecContext& ctx) {
  for (std::size_t k = p; k < end; ++k)
    if (f.lit[k]) return {};
  std::string name = f.text.substr(p + 1, end - p - 1);
  auto param = [&](const char* n) -> std::optional<std::string> {
    auto v = ctx.params.lookup(n);
    if (v and v->get().is_set()) return v->get().scalar();
    return std::nullopt;
  };
  std::optional<std::string> dir;
  if (name.empty()) {
    dir = param("HOME");
    if (not dir) if (auto* pw = ::getpwuid(::getuid())) dir = pw->pw_dir;
  } else if (name == "+") {
    dir = param("PWD");
  } else if (name == "-") {
    dir = param("OLDPWD");
  } else {
    for (char c : name)
      if (not is_name_char(c) and c != '-' and c != '.') return {};
    if (auto* pw = ::getpwnam(name.c_str())) dir = pw->pw_dir;
    else return std::unexpected(subst_error("no such user or named directory: " + name));
  }
  if (not dir) return {};
  Field out = f.slice(0, p);
  out.quoted = f.quoted;
  out.put(*dir, true);
  out.append(f.slice(end, f.text.size()));
  f = std::move(out);
  return {};
}

// Tilde expansion at the start of the word and, for assignments (or
// MAGIC_EQUAL_SUBST arguments), after `=` and each `:`.
Result<void> tilde_expand(Field& f, bool assign_like, ExecContext& ctx) {
  auto bound = [&](std::size_t p) {
    std::size_t e = p + 1;
    while (e < f.text.size() and f.text[e] != '/' and not (assign_like and f.text[e] == ':'))
      ++e;
    return e;
  };
  if (f.unlit(0) and f.text[0] == '~')
    if (auto r = tilde_at(f, 0, bound(0), ctx); not r) return r;
  if (not assign_like) return {};
  std::size_t eq = std::string::npos;
  for (std::size_t k = 0; k < f.text.size(); ++k)
    if (f.unlit(k) and f.text[k] == '=') { eq = k; break; }
  for (std::size_t k = 0; k + 1 < f.text.size(); ++k) {
    bool after = (eq != std::string::npos and k == eq) or (f.unlit(k) and f.text[k] == ':');
    if (after and f.unlit(k + 1) and f.text[k + 1] == '~')
      if (auto r = tilde_at(f, k + 1, bound(k + 1), ctx); not r) return r;
  }
  return {};
}

}  // namespace

Result<std::vector<std::string>> Expander::words(const std::vector<std::string>& words) {
  std::vector<std::string> out;
  auto& opts = mCtx.opts;
  for (const auto& w : words) {
    // Process substitution is handled by the executor.
    if (w.size() >= 3 and w.back() == ')'
        and (w.starts_with("<(") or w.starts_with(">(") or w.starts_with("=("))) {
      out.push_back(w);
      continue;
    }
    Scan scan{*this, Mode::Word};
    if (auto r = scan.scan(w); not r) return std::unexpected(r.error());

    std::vector<Field> fields;
    for (auto& f : scan.fields) {
      if (opts.get(OPT_IGNOREBRACES)) fields.push_back(std::move(f));
      else brace_expand(f, opts.get(OPT_BRACECCL), fields);
    }
    for (auto& f : fields) {
      if (f.text.empty()) {
        if (f.quoted and not f.empty_at) out.emplace_back();
        continue;
      }
      bool magic = opts.get(OPT_MAGICEQUALSUBST)
          and std::any_of(f.text.begin(), f.text.end(), [](char c) { return c == '='; });
      if (auto r = tilde_expand(f, magic, mCtx); not r) return std::unexpected(r.error());
      // `=cmd` becomes the command's path.
      if (opts.get(OPT_EQUALS) and f.text.size() > 1 and f.text[0] == '=' and f.unlit(0)
          and std::none_of(f.lit.begin(), f.lit.end(), [](bool l) { return l; })
          and f.text[1] != '(') {
        std::string name = f.text.substr(1);
        auto path = mExec.find_command(name);
        if (not path) return std::unexpected(subst_error(name + " not found"));
        out.push_back(*path);
        continue;
      }
      if (f.has_unlit_glob()) {
        auto matches = glob_expand(GlobPattern::compile(f.as_pattern()), opts);
        if (not matches) return std::unexpected(matches.error());
        for (auto& m : *matches) out.push_back(std::move(m));
        continue;
      }
      out.push_back(std::move(f.text));
    }
  }
  return out;
}

Result<std::string> Expander::scalar(std::string_view word, bool assignment) {
  Scan scan{*this, Mode::Scalar};
  if (auto r = scan.scan(word); not r) return std::unexpected(r.error());
  Field all;
  for (std::size_t k = 0; k < scan.fields.size(); ++k) {
    if (k) all.put(' ', true);
    all.append(scan.fields[k]);
  }
  if (auto r = tilde_expand(all, assignment, mCtx); not r) return std::unexpected(r.error());
  return all.text;
}

Result<std::vector<std::string>> Expander::array_elements(std::string_view src) {
  StringInputSource in{std::string(src)};
  Lexer lex{in};
  lex.push_state(LEX_SKIPCOMMENTS);
  std::vector<std::string> raw;
  for (;;) {
    auto tok = lex.next_token();
    if (tok.is_eof()) break;
    if (tok.is_newline()) continue;
    if (tok.type != TokType::TK_WORD and tok.type < TokType::TK_IF)
      return std::unexpected(ShellError{ErrorKind::ParseError,
          "parse error near `" + tok.text + "'"});
    raw.push_back(std::move(tok.text));
  }
  return words(raw);
}

Result<std::string> Expander::heredoc(std::string_view body) {
  Scan scan{*this, Mode::Heredoc};
  if (auto r = scan.scan(body); not r) return std::unexpected(r.error());
  std::string out;
  for (std::size_t k = 0; k < scan.fields.size(); ++k) {
    if (k) out += ' ';
    out += scan.fields[k].text;
  }
  return out;
}

Result<std::string> Expander::pattern(std::string_view word) {
  Scan scan{*this, Mode::Scalar};
  if (auto r = scan.scan(word); not r) return std::unexpected(r.error());
  std::string out;
  for (std::size_t k = 0; k < scan.fields.size(); ++k) {
    if (k) out += ' ';
    out += scan.fields[k].as_pattern();
  }
  return out;
}

}  // namespace jfsh
