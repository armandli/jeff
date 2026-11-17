// Output builtins: echo, print and printf.

#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <algorithm>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/math.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/glob/glob.h>
#include <jfsh/history/history.h>

namespace jfsh {

namespace {

void put_utf8(str& out, s::uint32_t cp) {
  if (cp < 0x80) {
    out += static_cast<char>(cp);
  } else if (cp < 0x800) {
    out += static_cast<char>(0xc0 | (cp >> 6));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  } else if (cp < 0x10000) {
    out += static_cast<char>(0xe0 | (cp >> 12));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  } else {
    out += static_cast<char>(0xf0 | (cp >> 18));
    out += static_cast<char>(0x80 | ((cp >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((cp >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (cp & 0x3f));
  }
}

int hex_value(char c) {
  if (c >= '0' and c <= '9') return c - '0';
  if (c >= 'a' and c <= 'f') return c - 'a' + 10;
  if (c >= 'A' and c <= 'F') return c - 'A' + 10;
  return -1;
}

enum class Octal {
  ZeroPrefixed,   // echo/print: \0NNN
  Bare,           // printf formats: \NNN
  Both,           // printf %b: \0NNN and \NNN
};

// Interprets backslash escapes.  `\c` sets `stop`: nothing more is output.
str unescape(strv s, Octal octal, bool& stop) {
  str out;
  for (s::size_t i = 0; i < s.size(); ++i) {
    char c = s[i];
    if (c != '\\' or i + 1 >= s.size()) {
      out += c;
      continue;
    }
    char e = s[++i];
    switch (e) {
      case 'a': out += '\a'; break;
      case 'b': out += '\b'; break;
      case 'c': stop = true; return out;
      case 'e': case 'E': out += '\033'; break;
      case 'f': out += '\f'; break;
      case 'n': out += '\n'; break;
      case 'r': out += '\r'; break;
      case 't': out += '\t'; break;
      case 'v': out += '\v'; break;
      case '\\': out += '\\'; break;
      case 'x': {
        int v = 0, n = 0;
        while (n < 2 and i + 1 < s.size() and hex_value(s[i + 1]) >= 0) {
          v = v * 16 + hex_value(s[++i]);
          ++n;
        }
        if (n == 0) { out += "\\x"; break; }
        out += static_cast<char>(v);
        break;
      }
      case 'u': case 'U': {
        int max = e == 'u' ? 4 : 8, n = 0;
        s::uint32_t v = 0;
        while (n < max and i + 1 < s.size() and hex_value(s[i + 1]) >= 0) {
          v = v * 16 + static_cast<s::uint32_t>(hex_value(s[++i]));
          ++n;
        }
        if (n == 0) { out += '\\'; out += e; break; }
        put_utf8(out, v);
        break;
      }
      default: {
        bool zero = e == '0';
        bool digit = e >= '0' and e <= '7';
        if (digit and (octal != Octal::ZeroPrefixed or zero)) {
          // \0NNN takes up to three more digits; a bare \NNN up to three.
          int v = zero and octal != Octal::Bare ? 0 : e - '0';
          int n = 0, max = zero and octal != Octal::Bare ? 3 : 2;
          while (n < max and i + 1 < s.size() and s[i + 1] >= '0' and s[i + 1] <= '7') {
            v = v * 8 + (s[++i] - '0');
            ++n;
          }
          out += static_cast<char>(v & 0xff);
          break;
        }
        out += '\\';
        out += e;
      }
    }
  }
  return out;
}

bool write_all(int fd, strv data) {
  s::fflush(stdout);
  while (not data.empty()) {
    ssize_t n = ::write(fd, data.data(), data.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    data.remove_prefix(static_cast<s::size_t>(n));
  }
  return true;
}

int emit(strv cmd, int fd, strv data) {
  if (fd == STDOUT_FILENO) {
    s::fwrite(data.data(), 1, data.size(), stdout);
    if (s::ferror(stdout)) {
      s::clearerr(stdout);
      return builtin_error(cmd, "write error: " + str(s::strerror(errno)));
    }
    return 0;
  }
  if (not write_all(fd, data)) return builtin_error(cmd, "write error: " + str(s::strerror(errno)));
  return 0;
}

// ---- printf -----------------------------------------------------------------

// `%q`: backslashes before the characters the shell would treat specially.
str backslash_quote(strv s) {
  if (s.empty()) return "''";
  str out;
  for (char c : s) {
    if (strv(" \t\n'\"\\$`|&;<>()*?[]#~={}^!").find(c) != strv::npos) out += '\\';
    out += c;
  }
  return out;
}

struct Printf {
  strv              cmd;
  const vec<str>&   args;
  s::size_t         next;          // next argument to take
  s::size_t         max_used = 0;  // for %N$ arguments
  ExecContext&      ctx;
  int               rc = 0;
  bool              stop = false;  // \c in %b

  opt<str> take(opt<s::size_t> pos) {
    s::size_t at = pos ? next_base + *pos - 1 : next++;
    if (pos) max_used = s::max(max_used, *pos);
    if (at < args.size()) return args[at];
    return s::nullopt;
  }
  s::size_t next_base = 0;

  // A numeric argument: `'c` gives the character's code, anything else is
  // an arithmetic expression.
  MathNum number(const opt<str>& a) {
    if (not a or a->empty()) return MathNum::of(s::int64_t{0});
    if ((*a)[0] == '\'' or (*a)[0] == '"') {
      if (a->size() < 2) return MathNum::of(s::int64_t{0});
      return MathNum::of(static_cast<s::int64_t>(static_cast<unsigned char>((*a)[1])));
    }
    auto v = math_eval(*a, ctx.params, math_options(ctx.opts));
    if (not v) {
      s::fprintf(stderr, "jfsh: %.*s: %s\n", static_cast<int>(cmd.size()), cmd.data(),
                 v.error().message.c_str());
      rc = 1;
      return MathNum::of(s::int64_t{0});
    }
    return *v;
  }

  // One pass over the format; returns the output.
  str run(strv fmt, bool& used_args) {
    str out;
    s::size_t start_next = next;
    for (s::size_t i = 0; i < fmt.size() and not stop; ++i) {
      char c = fmt[i];
      if (c == '\\') {
        s::size_t j = i + 1;
        // Gather the escape and pass it to unescape.
        if (j < fmt.size()) {
          s::size_t end = j + 1;
          if (fmt[j] == 'x') { while (end < fmt.size() and end < j + 3 and hex_value(fmt[end]) >= 0) ++end; }
          else if (fmt[j] == 'u') { while (end < fmt.size() and end < j + 5 and hex_value(fmt[end]) >= 0) ++end; }
          else if (fmt[j] == 'U') { while (end < fmt.size() and end < j + 9 and hex_value(fmt[end]) >= 0) ++end; }
          else if (fmt[j] >= '0' and fmt[j] <= '7') {
            while (end < fmt.size() and end < j + 3 and fmt[end] >= '0' and fmt[end] <= '7') ++end;
          }
          bool st = false;
          out += unescape(fmt.substr(i, end - i), Octal::Bare, st);
          if (st) { stop = true; break; }
          i = end - 1;
        } else {
          out += '\\';
        }
        continue;
      }
      if (c != '%') { out += c; continue; }
      if (i + 1 < fmt.size() and fmt[i + 1] == '%') { out += '%'; ++i; continue; }
      // %[N$][flags][width][.prec]conv
      s::size_t j = i + 1;
      opt<s::size_t> pos;
      {
        s::size_t k = j;
        while (k < fmt.size() and s::isdigit(static_cast<unsigned char>(fmt[k]))) ++k;
        if (k > j and k < fmt.size() and fmt[k] == '$') {
          pos = s::strtoul(str(fmt.substr(j, k - j)).c_str(), nullptr, 10);
          j = k + 1;
        }
      }
      str spec = "%";
      while (j < fmt.size() and strv("-+ #0").find(fmt[j]) != strv::npos) spec += fmt[j++];
      auto star = [&]() {
        auto v = number(take(s::nullopt));
        spec += s::to_string(v.as_int());
      };
      if (j < fmt.size() and fmt[j] == '*') { star(); ++j; }
      else while (j < fmt.size() and s::isdigit(static_cast<unsigned char>(fmt[j]))) spec += fmt[j++];
      if (j < fmt.size() and fmt[j] == '.') {
        spec += fmt[j++];
        if (j < fmt.size() and fmt[j] == '*') { star(); ++j; }
        else while (j < fmt.size() and s::isdigit(static_cast<unsigned char>(fmt[j]))) spec += fmt[j++];
      }
      // Length modifiers mean nothing here.
      while (j < fmt.size() and strv("hlL").find(fmt[j]) != strv::npos) ++j;
      if (j >= fmt.size()) {
        builtin_error(cmd, "%: invalid directive");
        rc = 1;
        return out;
      }
      char conv = fmt[j];
      i = j;
      char buf[512];
      switch (conv) {
        case 's': case 'b': case 'q': {
          auto a = take(pos);
          str v = a ? *a : str{};
          if (conv == 'b') {
            bool st = false;
            v = unescape(v, Octal::Both, st);
            if (st) stop = true;
          } else if (conv == 'q') {
            v = backslash_quote(v);
          }
          // Width and precision by hand: values may be any length.
          bool left = spec.find('-') != str::npos;
          s::size_t dot = spec.find('.');
          str width_text = spec.substr(1, dot == str::npos ? str::npos : dot - 1);
          width_text.erase(0, width_text.find_first_not_of("-+ #0"));
          long width = width_text.empty() ? 0 : s::strtol(width_text.c_str(), nullptr, 10);
          if (width < 0) { left = true; width = -width; }
          if (dot != str::npos) {
            long prec = s::strtol(spec.c_str() + dot + 1, nullptr, 10);
            if (prec >= 0 and static_cast<s::size_t>(prec) < v.size()) v.resize(static_cast<s::size_t>(prec));
          }
          s::size_t pad = static_cast<s::size_t>(width) > v.size() ? static_cast<s::size_t>(width) - v.size() : 0;
          if (left) out += v + str(pad, ' ');
          else      out += str(pad, ' ') + v;
          break;
        }
        case 'c': {
          auto a = take(pos);
          spec += 'c';
          s::snprintf(buf, sizeof buf, spec.c_str(), a and not a->empty() ? (*a)[0] : '\0');
          out += (a and not a->empty()) ? str(buf) : str{};
          break;
        }
        case 'd': case 'i': {
          spec += "lld";
          s::snprintf(buf, sizeof buf, spec.c_str(),
                      static_cast<long long>(number(take(pos)).as_int()));
          out += buf;
          break;
        }
        case 'o': case 'u': case 'x': case 'X': {
          spec += "ll";
          spec += conv;
          s::snprintf(buf, sizeof buf, spec.c_str(),
                      static_cast<unsigned long long>(number(take(pos)).as_int()));
          out += buf;
          break;
        }
        case 'e': case 'E': case 'f': case 'F': case 'g': case 'G': case 'a': case 'A': {
          spec += conv;
          s::snprintf(buf, sizeof buf, spec.c_str(), number(take(pos)).as_float());
          out += buf;
          break;
        }
        default:
          s::fprintf(stderr, "jfsh: %.*s: %%%c: invalid directive\n",
                     static_cast<int>(cmd.size()), cmd.data(), conv);
          rc = 1;
          return out;
      }
    }
    used_args = next > start_next or max_used > 0;
    return out;
  }
};

// Runs printf's format over `args[first…]`, reusing it while arguments
// remain.
int do_printf(strv cmd, strv fmt, const vec<str>& args, s::size_t first,
              ExecContext& ctx, str& out) {
  Printf p{cmd, args, first, 0, ctx};
  for (;;) {
    p.next_base = p.next;
    p.max_used = 0;
    bool used = false;
    out += p.run(fmt, used);
    if (p.stop or p.rc) break;
    if (p.max_used) p.next = s::max(p.next, p.next_base + p.max_used);
    if (not used or p.next >= args.size()) break;
  }
  return p.rc;
}

// ---- print columns ------------------------------------------------------------

str columns(const vec<str>& items, s::size_t ncols, bool across, s::size_t width_limit) {
  if (items.empty()) return {};
  s::size_t w = 0;
  for (const auto& i : items) w = s::max(w, i.size());
  w += 2;
  if (ncols == 0) ncols = s::max<s::size_t>(1, width_limit / w);
  ncols = s::min(ncols, items.size());
  s::size_t nrows = (items.size() + ncols - 1) / ncols;
  str out;
  for (s::size_t r = 0; r < nrows; ++r) {
    str line;
    for (s::size_t c = 0; c < ncols; ++c) {
      s::size_t k = across ? r * ncols + c : c * nrows + r;
      if (k >= items.size()) break;
      line += items[k];
      bool last = c + 1 == ncols or (across ? k + 1 >= items.size() : (c + 1) * nrows + r >= items.size());
      if (not last) line += str(w - items[k].size(), ' ');
    }
    out += line + '\n';
  }
  return out;
}

str expand_tabs(strv s, s::size_t tab, bool all) {
  str out;
  s::size_t col = 0;
  bool leading = true;
  for (char c : s) {
    if (c == '\t' and (all or leading)) {
      s::size_t n = tab - col % tab;
      out += str(n, ' ');
      col += n;
      continue;
    }
    if (c != '\t' and c != ' ') leading = false;
    if (c == '\n') { col = 0; leading = true; }
    else ++col;
    out += c;
  }
  return out;
}

}  // namespace

// echo [-neE] [arg …]: escapes are interpreted unless -E or BSD_ECHO
// (then only with -e).  Only words made of those letters are options.
int builtin_echo(vec<str>& args, ExecContext& ctx) {
  bool newline = true;
  bool escapes = not ctx.opts.get(OPT_BSDECHO);
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    strv a = args[i];
    if (a.size() < 2 or a[0] != '-' or a.find_first_not_of("neE", 1) != strv::npos) break;
    for (char c : a.substr(1)) {
      if (c == 'n') newline = false;
      else if (c == 'e') escapes = true;
      else escapes = false;
    }
  }
  str out;
  bool stop = false;
  for (s::size_t k = i; k < args.size() and not stop; ++k) {
    if (k > i) out += ' ';
    out += escapes ? unescape(args[k], Octal::ZeroPrefixed, stop) : args[k];
  }
  if (newline and not stop) out += '\n';
  return emit(args[0], STDOUT_FILENO, out);
}

// print [-abcDilmnNoOPrRsSz] [-C cols] [-f fmt] [-u fd] [-v var] [-xX tab]
//       [arg …]
int builtin_print(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  OptParse o;
  // -R: BSD echo style, where only -n and -e may follow.
  if (args.size() > 1 and args[1].starts_with("-R")) {
    o.flags.push_back({'R', true, {}});
    o.flags.push_back({'r', true, {}});
    s::size_t i = 1;
    for (char c : strv(args[1]).substr(2)) {
      if (c == 'n' or c == 'e') o.flags.push_back({c, true, {}});
    }
    ++i;
    while (i < args.size() and args[i].size() > 1 and args[i][0] == '-'
           and args[i].find_first_not_of("ne", 1) == str::npos) {
      for (char c : strv(args[i]).substr(1)) o.flags.push_back({c, true, {}});
      ++i;
    }
    o.next = i;
  } else if (not parse_options(args, "abcDilmnNoOPrsSz", "CfuvxXp", o)) {
    return 1;
  }
  vec<str> words(args.begin() + static_cast<s::ptrdiff_t>(o.next), args.end());
  int fd = STDOUT_FILENO;
  if (auto* u = o.arg('u')) {
    char* end = nullptr;
    long n = s::strtol(u->c_str(), &end, 10);
    if (u->empty() or *end != '\0' or n < 0) return builtin_error(cmd, "number expected after -u: " + *u);
    fd = static_cast<int>(n);
  }
  if (o.has('m') and not words.empty()) {
    auto pat = CompiledPattern::compile(words[0]);
    vec<str> kept;
    for (s::size_t k = 1; k < words.size(); ++k)
      if (pat.matches(words[k])) kept.push_back(words[k]);
    words = s::move(kept);
  }
  bool raw = o.has('r');
  bool stop = false;
  if (auto* f = o.arg('f')) {
    str fmt = *f;
    vec<str> pargs{str(cmd)};
    pargs.insert(pargs.end(), words.begin(), words.end());
    str out;
    int rc = do_printf(cmd, fmt, pargs, 1, ctx, out);
    if (auto* v = o.arg('v')) {
      if (auto r = ctx.params.set_scalar(*v, out); not r) return builtin_error(cmd, r.error().message);
      return rc;
    }
    int wrc = emit(cmd, fd, out);
    return rc ? rc : wrc;
  }
  for (auto& w : words) {
    if (not raw and not stop) w = unescape(w, Octal::ZeroPrefixed, stop);
    if (o.has('P')) w = ctx.executor->expand_prompt(w);
    if (o.has('D')) w = tilde_abbrev(ctx.params, w);
  }
  if (o.has('o') or o.has('O')) {
    bool fold = o.has('i');
    auto less = [fold](const str& a, const str& b) {
      if (not fold) return a < b;
      return s::lexicographical_compare(a.begin(), a.end(), b.begin(), b.end(),
          [](char x, char y) { return s::tolower(static_cast<unsigned char>(x))
                                    < s::tolower(static_cast<unsigned char>(y)); });
    };
    s::stable_sort(words.begin(), words.end(), less);
    if (o.has('O')) s::reverse(words.begin(), words.end());
  }
  // -s/-S: onto the history list instead of output.
  if (o.has('s') or o.has('S')) {
    str line;
    for (s::size_t k = 0; k < words.size(); ++k) line += (k ? " " : "") + words[k];
    if (ctx.history) ctx.history->add(line);
    return 0;
  }
  if (o.has('z')) return 0;   // no line editor buffer to push onto yet
  str out;
  if (o.has('c') or o.arg('C')) {
    s::size_t cols = 0;
    if (auto* c = o.arg('C')) cols = static_cast<s::size_t>(s::max(1L, s::strtol(c->c_str(), nullptr, 10)));
    s::size_t width = 80;
    if (auto p = ctx.params.lookup("COLUMNS"); p and p->get().is_set())
      width = static_cast<s::size_t>(s::max(1L, s::strtol(p->get().scalar().c_str(), nullptr, 10)));
    out = columns(words, cols, o.has('a'), width);
  } else {
    char sep = o.has('N') ? '\0' : o.has('l') ? '\n' : ' ';
    for (s::size_t k = 0; k < words.size(); ++k) {
      if (k) out += sep;
      out += words[k];
    }
    if (o.has('N')) out += '\0';
    else if (not o.has('n') and not stop) out += '\n';
  }
  if (auto* x = o.arg('x')) out = expand_tabs(out, static_cast<s::size_t>(s::max(1L, s::strtol(x->c_str(), nullptr, 10))), false);
  if (auto* x = o.arg('X')) out = expand_tabs(out, static_cast<s::size_t>(s::max(1L, s::strtol(x->c_str(), nullptr, 10))), true);
  if (auto* v = o.arg('v')) {
    if (not out.empty() and out.back() == '\n') out.pop_back();
    if (auto r = ctx.params.set_scalar(*v, out); not r) return builtin_error(cmd, r.error().message);
    return 0;
  }
  return emit(cmd, fd, out);
}

// printf [-v var] format [arg …]
int builtin_printf(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  s::size_t i = 1;
  opt<str> var;
  if (i < args.size() and args[i] == "-v") {
    if (i + 1 >= args.size()) return builtin_error(cmd, "argument expected: -v");
    var = args[i + 1];
    i += 2;
  }
  if (i < args.size() and args[i] == "--") ++i;
  if (i >= args.size()) return builtin_error(cmd, "not enough arguments");
  str out;
  int rc = do_printf(cmd, args[i], args, i + 1, ctx, out);
  if (var) {
    if (auto r = ctx.params.set_scalar(*var, out); not r) return builtin_error(cmd, r.error().message);
    return rc;
  }
  int wrc = emit(cmd, STDOUT_FILENO, out);
  return rc ? rc : wrc;
}

}  // namespace jfsh
