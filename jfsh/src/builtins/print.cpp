// Output builtins: echo, print and printf.

// The escape handling (unescape) is derived from zsh 5.9's getkeystring
// (Src/utils.c) and %q (backslash_quote) from its quotestring and
// addunprintable.  zsh's copyright and licence:
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

#include <cctype>
#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/math.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/core/utf8.h>
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

// How a builtin reads backslash escapes: zsh's getkeystring flags.
enum EscFlags : unsigned {
  ESC_OCTAL = 1,   // GETKEY_OCTAL_ESC: \NNN (and \0NN) without a leading 0
  ESC_C     = 2,   // GETKEY_BACKSLASH_C: \c ends the output
  ESC_EMACS = 4,   // GETKEY_EMACS: \E, \M-, \C-; other escapes drop the `\`
};
constexpr unsigned ESC_ECHO       = ESC_C;
constexpr unsigned ESC_PRINT      = ESC_OCTAL | ESC_C | ESC_EMACS;
constexpr unsigned ESC_PRINTF_FMT = ESC_OCTAL | ESC_C;
constexpr unsigned ESC_PRINTF_ARG = ESC_C;

// Interprets backslash escapes as zsh's getkeystring does for `how`.
// `\c` sets `stop`: nothing more is output.
str unescape(strv s, unsigned how, bool& stop) {
  str  out;
  bool emacs = how & ESC_EMACS;
  int  meta = 0;
  bool control = false;
  for (s::size_t i = 0; i < s.size(); ++i) {
    s::size_t before = out.size();
    if (s[i] != '\\' or i + 1 >= s.size()) {
      out += s[i];
    } else {
      char e = s[++i];
      switch (e) {
        case 'a': out += '\a'; break;
        case 'n': out += '\n'; break;
        case 'b': out += '\b'; break;
        case 't': out += '\t'; break;
        case 'v': out += '\v'; break;
        case 'f': out += '\f'; break;
        case 'r': out += '\r'; break;
        case 'e': out += '\033'; break;
        case 'E':
          if (emacs) out += '\033';
          else { out += '\\'; --i; continue; }
          break;
        case 'M': case 'C':
          if (not emacs) { out += '\\'; --i; continue; }
          if (i + 1 < s.size() and s[i + 1] == '-') ++i;
          if (e == 'M') meta = 1 + control;   // the order of \C- and \M- counts
          else          control = true;
          continue;
        case 'c':
          if (how & ESC_C) {
            stop = true;
            return out;
          }
          out += '\\';
          out += e;
          break;
        case 'u': case 'U': {
          s::uint32_t v = 0;
          for (int n = e == 'u' ? 4 : 8; n > 0 and i + 1 < s.size() and hex_value(s[i + 1]) >= 0;
               --n)
            v = v * 16 + static_cast<s::uint32_t>(hex_value(s[++i]));
          put_utf8(out, v);
          break;
        }
        default:
          if ((e >= '0' and e <= '7') or e == 'x') {
            // Without ESC_OCTAL only \0NNN is octal; \NNN stays as it is.
            s::size_t at = i;
            if (not (how & ESC_OCTAL)) {
              if (e == '0') ++at;
              else if (e != 'x') { out += '\\'; --i; continue; }
            }
            // zstrtol on at most three characters (`xHH` or `NNN`).
            bool hex = at < s.size() and s[at] == 'x';
            s::size_t k = at + (hex ? 1 : 0), lim = s::min(s.size(), at + 3);
            int v = 0;
            for (; k < lim; ++k) {
              int d = hex ? hex_value(s[k]) : (s[k] >= '0' and s[k] <= '7') ? s[k] - '0' : -1;
              if (d < 0) break;
              v = v * (hex ? 16 : 8) + d;
            }
            out += static_cast<char>(v & 0xff);
            i = k - 1;
          } else {
            if (not emacs and e != '\\') out += '\\';
            out += e;
          }
      }
    }
    if (out.size() == before) continue;
    char& c = out.back();
    if (meta == 2) {
      c = static_cast<char>(c | 0x80);
      meta = 0;
    }
    if (control) {
      c = c == '?' ? '\x7f' : static_cast<char>(c & 0x9f);
      control = false;
    }
    if (meta) {
      c = static_cast<char>(c | 0x80);
      meta = 0;
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

// Written straight to the fd, after what stdio holds, so that a failure
// is reported with this write's own errno (not an error a failed flush
// left on stdout), and nothing stays buffered to come out later.
int emit(strv cmd, int fd, strv data) {
  s::clearerr(stdout);
  if (write_all(fd, data)) return 0;
  // strerror() lowercased, as zsh's %e prints it.
  str text = s::strerror(errno);
  if (text.size() > 1 and not s::isupper(static_cast<unsigned char>(text[1])))
    text[0] = static_cast<char>(s::tolower(static_cast<unsigned char>(text[0])));
  return builtin_error(cmd, "write error: " + text);
}

// ---- printf -----------------------------------------------------------------

// zsh's addunprintable: the bytes of a character that can't be shown,
// as $'…' escapes.
void add_unprintable(str& out, strv bytes, strv rest) {
  for (s::size_t k = 0; k < bytes.size(); ++k) {
    auto c = static_cast<unsigned char>(bytes[k]);
    char next = k + 1 < bytes.size() ? bytes[k + 1] : rest.empty() ? '\0' : rest[0];
    switch (c) {
      case '\0':
        out += "\\0";
        if (next >= '0' and next <= '7') out += "00";
        break;
      case '\a': out += "\\a"; break;
      case '\b': out += "\\b"; break;
      case '\f': out += "\\f"; break;
      case '\n': out += "\\n"; break;
      case '\r': out += "\\r"; break;
      case '\t': out += "\\t"; break;
      case '\v': out += "\\v"; break;
      default: {
        char buf[8];
        s::snprintf(buf, sizeof buf, "\\%03o", c);
        out += buf;
      }
    }
  }
}

// `%q`: zsh's quotestring with QT_BACKSLASH_SHOWNULL.  A backslash before
// each special character (`=` and `~` only first, or after `=`/`:` under
// MAGIC_EQUAL_SUBST, `~` under EXTENDED_GLOB), a newline as $'\n', and an
// unprintable character as $'\NNN'.
str backslash_quote(strv s, const OptionState& opts) {
  if (s.empty()) return "''";
  str out;
  for (s::size_t i = 0; i < s.size();) {
    char c = s[i];
    bool special = strv("#$^*()=|{}[]`<>?~;&\n\t \\'\"").find(c) != strv::npos
               and ((c != '=' and c != '~') or i == 0
                    or (opts.get(OPT_MAGICEQUALSUBST) and (s[i - 1] == '=' or s[i - 1] == ':'))
                    or (c == '~' and opts.get(OPT_EXTENDEDGLOB)));
    if (special and c == '\n') {
      out += "$'\\n'";
      ++i;
      continue;
    }
    s::size_t len;
    char32_t cc = utf8_decode(s, i, len);
    bool printable = not (len == 1 and cc >= 0x80) and printable_char(cc);
    if (printable) {
      if (special) out += '\\';
      out += s.substr(i, len);
    } else {
      out += "$'";
      add_unprintable(out, s.substr(i, len), s.substr(i + len));
      out += '\'';
    }
    i += len;
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
    auto v = math_eval(*a, ctx.params, math_options(ctx),
                       &ctx.modules.math_funcs());
    if (not v) {
      // zsh's matheval reports it as a shell error, not printf's.
      s::fprintf(stderr, "jfsh: %s\n", v.error().message.c_str());
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
          out += unescape(fmt.substr(i, end - i), ESC_PRINTF_FMT, st);
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
        if (k > j and fmt[j] != '0' and k < fmt.size() and fmt[k] == '$') {
          pos = s::strtoul(str(fmt.substr(j, k - j)).c_str(), nullptr, 10);
          j = k + 1;
          if (*pos == 0 or *pos > args.size() - next_base) {
            builtin_error(cmd, s::to_string(*pos) + ": argument specifier out of range");
            rc = 1;
            return out;
          }
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
      // One length modifier means nothing here (a second is invalid).
      if (j < fmt.size() and strv("hlL").find(fmt[j]) != strv::npos) ++j;
      // The directive as zsh's message shows it: up to the bad character.
      s::size_t start = i;
      auto invalid = [&] {
        builtin_error(cmd, str(fmt.substr(start, j < fmt.size() ? j - start + 1 : str::npos))
                               + ": invalid directive");
        rc = 1;
        return out;
      };
      if (j >= fmt.size()) return invalid();
      char conv = fmt[j];
      i = j;
      char buf[512];
      switch (conv) {
        case 's': case 'b': case 'q': {
          auto a = take(pos);
          str v = a ? *a : str{};
          if (conv == 'b') {
            bool st = false;
            v = unescape(v, ESC_PRINTF_ARG, st);
            if (st) stop = true;
          } else if (conv == 'q') {
            v = backslash_quote(v, ctx.opts);
          }
          // Width and precision by hand, in characters, as zsh counts
          // them (mbrlen): values may be any length.
          bool left = spec.find('-') != str::npos;
          s::size_t dot = spec.find('.');
          str width_text = spec.substr(1, dot == str::npos ? str::npos : dot - 1);
          width_text.erase(0, width_text.find_first_not_of("-+ #0"));
          long width = width_text.empty() ? 0 : s::strtol(width_text.c_str(), nullptr, 10);
          if (width < 0) { left = true; width = -width; }
          long prec = dot == str::npos ? -1 : s::strtol(spec.c_str() + dot + 1, nullptr, 10);
          s::size_t chars = 0, at = 0;
          while (at < v.size() and static_cast<long>(chars) != prec) {
            s::size_t len;
            utf8_decode(v, at, len);
            at += len;
            ++chars;
          }
          v.resize(at);
          s::size_t pad = static_cast<s::size_t>(width) > chars
                        ? static_cast<s::size_t>(width) - chars : 0;
          if (left) out += v + str(pad, ' ');
          else      out += str(pad, ' ') + v;
          break;
        }
        case 'c': {
          // The first byte, or NUL without one, as zsh's print_val.
          auto a = take(pos);
          spec += 'c';
          int n = s::snprintf(buf, sizeof buf, spec.c_str(), a and not a->empty() ? (*a)[0] : '\0');
          out.append(buf, static_cast<s::size_t>(s::clamp(n, 0, static_cast<int>(sizeof buf) - 1)));
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
        case 'e': case 'E': case 'f': case 'g': case 'G': {
          spec += conv;
          s::snprintf(buf, sizeof buf, spec.c_str(), number(take(pos)).as_float());
          out += buf;
          break;
        }
        default:
          return invalid();
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
  // As many columns as fit (-c), at most one per item; -C N keeps N.
  if (ncols == 0) ncols = s::clamp<s::size_t>(width_limit / w, 1, items.size());
  s::size_t nrows = (items.size() + ncols - 1) / ncols;
  str out;
  for (s::size_t r = 0; r < nrows; ++r) {
    str line;
    for (s::size_t c = 0; c < ncols; ++c) {
      s::size_t k = across ? r * ncols + c : c * nrows + r;
      if (k >= items.size()) break;
      line += items[k];
      // Across, every column but the last is padded, as in zsh.
      bool last = c + 1 == ncols or (not across and (c + 1) * nrows + r >= items.size());
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
  bool e = false, E = false;
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    strv a = args[i];
    // `-` alone ends the options and is dropped, as zsh's execbuiltin.
    if (a == "-") { ++i; break; }
    if (a.size() < 2 or a[0] != '-' or a.find_first_not_of("neE", 1) != strv::npos) break;
    for (char c : a.substr(1)) {
      if (c == 'n') newline = false;
      else if (c == 'e') e = true;
      else E = true;
    }
  }
  // As bin_print: -e wins over -E and BSD_ECHO.
  bool escapes = e or not (E or ctx.opts.get(OPT_BSDECHO));
  str out;
  bool stop = false;
  for (s::size_t k = i; k < args.size() and not stop; ++k) {
    if (k > i) out += ' ';
    out += escapes ? unescape(args[k], ESC_ECHO, stop) : args[k];
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
  } else if (not parse_options(args, "abcDilmnNoOpPrsSz", "CfuvxX", o)) {
    return 1;
  }
  vec<str> words(args.begin() + static_cast<s::ptrdiff_t>(o.next), args.end());
  // There are no coprocesses yet.
  if (o.has('p')) return builtin_error(cmd, "-p: no coprocess");
  int fd = STDOUT_FILENO;
  if (auto* u = o.arg('u')) {
    char* end = nullptr;
    long n = s::strtol(u->c_str(), &end, 10);
    if (u->empty() or *end != '\0' or n < 0) return builtin_error(cmd, "number expected after -u: " + *u);
    fd = static_cast<int>(n);
    if (::fcntl(fd, F_GETFD) < 0) return builtin_error(cmd, "bad file number: " + *u);
  }
  s::size_t cols = 0;
  if (auto* c = o.arg('C')) {
    char* end = nullptr;
    long n = s::strtol(c->c_str(), &end, 10);
    if (c->empty() or *end != '\0') return builtin_error(cmd, "number expected after -C: " + *c);
    if (n <= 0) return builtin_error(cmd, "invalid number of columns: " + *c);
    cols = static_cast<s::size_t>(n);
  }
  // A -v name that isn't one ends the command line, print's status 0, as
  // zsh's setsparam.
  auto assign = [&](const str& name, str value) {
    (void)ctx.executor->assign_var(name, s::move(value));
    return 0;
  };
  if (o.has('m') and not words.empty()) {
    auto pat = CompiledPattern::compile(words[0]);
    vec<str> kept;
    for (s::size_t k = 1; k < words.size(); ++k)
      if (pat.matches(words[k])) kept.push_back(words[k]);
    words = s::move(kept);
  }
  // -e wins over -r and -R, as in bin_print.
  bool raw = o.has('r') and not o.has('e');
  bool stop = false;
  if (auto* f = o.arg('f')) {
    str fmt = *f;
    vec<str> pargs{str(cmd)};
    pargs.insert(pargs.end(), words.begin(), words.end());
    str out;
    int rc = do_printf(cmd, fmt, pargs, 1, ctx, out);
    if (auto* v = o.arg('v')) {
      assign(*v, s::move(out));
      return rc;
    }
    int wrc = emit(cmd, fd, out);
    return rc ? rc : wrc;
  }
  for (s::size_t k = 0; k < words.size(); ++k) {
    str& w = words[k];
    if (not raw) w = unescape(w, ESC_PRINT, stop);
    if (o.has('P')) w = ctx.executor->expand_prompt(w);
    if (o.has('D')) w = tilde_abbrev(ctx.params, w);
    // `\c` ends the output, the words after it too.
    if (stop) {
      words.resize(k + 1);
      break;
    }
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
    // The terminal's width; without one (no $COLUMNS), one column.
    s::size_t width = 0;
    if (auto p = ctx.params.lookup("COLUMNS"); p and p->get().is_set())
      width = static_cast<s::size_t>(s::max(0L, s::strtol(p->get().scalar().c_str(), nullptr, 10)));
    out = columns(words, cols, o.has('a'), width);
  } else {
    char sep = o.has('N') ? '\0' : o.has('l') ? '\n' : ' ';
    for (s::size_t k = 0; k < words.size(); ++k) {
      if (k) out += sep;
      out += words[k];
    }
    // As bin_print: no terminator after \c, -n, or -v without -l.
    if (not (o.has('n') or stop or (o.arg('v') and not o.has('l'))))
      out += o.has('N') ? '\0' : '\n';
  }
  for (char x : {'x', 'X'})
    if (auto* t = o.arg(x)) {
      char* end = nullptr;
      long n = s::strtol(t->c_str(), &end, 10);
      if (t->empty() or *end != '\0' or n <= 0)
        return builtin_error(cmd, "positive integer expected after -x: " + *t);
      out = expand_tabs(out, static_cast<s::size_t>(n), x == 'X');
    }
  if (auto* v = o.arg('v')) return assign(*v, s::move(out));
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
    (void)ctx.executor->assign_var(*var, s::move(out));
    return rc;
  }
  int wrc = emit(cmd, STDOUT_FILENO, out);
  return rc ? rc : wrc;
}

}  // namespace jfsh
