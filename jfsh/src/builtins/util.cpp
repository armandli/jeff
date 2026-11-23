// shell_quote is derived from zsh 5.9's quotedzputs, mb_niceformat and
// nicechar_sel (Src/utils.c).  zsh's copyright and licence:
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

#include <jfsh/builtins/util.h>

#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cwctype>

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/core/utf8.h>

namespace jfsh {

int builtin_error(strv cmd, strv msg) {
  s::fprintf(stderr, "jfsh: %.*s: %.*s\n", static_cast<int>(cmd.size()), cmd.data(),
             static_cast<int>(msg.size()), msg.data());
  return 1;
}

bool is_identifier(strv s) noexcept {
  if (s.empty() or s::isdigit(static_cast<unsigned char>(s[0]))) return false;
  for (char c : s)
    if (not s::isalnum(static_cast<unsigned char>(c)) and c != '_') return false;
  return true;
}

bool printable_char(char32_t c) noexcept {
  if (c < 0x80) return c >= 0x20 and c != 0x7f;
  // iswprint, when the locale has multibyte characters; else at least
  // the C1 controls aren't.
  if (MB_CUR_MAX > 1) return ::iswprint(static_cast<wint_t>(c));
  return c >= 0xa0;
}

// zsh's nicechar_sel(c, quotable) for a byte that isn't valid UTF-8 (or
// any control character): \M- for the high bit, \C- for controls, \n and
// \t as such.
static void nice_byte(str& out, unsigned char c) {
  if (c & 0x80) {
    out += "\\M-";
    c &= 0x7f;
  }
  if (c == 0x7f) out += "\\C-?";
  else if (c == '\n') out += "\\n";
  else if (c == '\t') out += "\\t";
  else if (c < 0x20) {
    out += "\\C-";
    out += static_cast<char>(c + 0x40);
  } else {
    out += static_cast<char>(c);
  }
}

str shell_quote(strv s) {
  if (s.empty()) return "''";
  // As zsh's quotedzputs: a string with an unprintable character is
  // written as $'…' (is_mb_niceformat / mb_niceformat).
  bool nice = false;
  bool special = false;
  for (s::size_t i = 0; i < s.size() and not nice;) {
    s::size_t len;
    char32_t c = utf8_decode(s, i, len);
    bool invalid = len == 1 and c >= 0x80;
    if (invalid or not printable_char(c)) nice = true;
    else if (c < 0x80
             and strv(" '\"\\$`|&;<>()*?[]#~={}^").find(static_cast<char>(c)) != strv::npos)
      special = true;
    i += len;
  }
  if (nice) {
    str out = "$'";
    for (s::size_t i = 0; i < s.size();) {
      s::size_t len;
      char32_t c = utf8_decode(s, i, len);
      if (len == 1 and c >= 0x80) {
        nice_byte(out, static_cast<unsigned char>(s[i]));
      } else if (c == '\'') {
        out += "\\'";
      } else if (c == '\\') {
        out += "\\\\";
      } else if (c < 0x80 and not printable_char(c)) {
        nice_byte(out, static_cast<unsigned char>(c));
      } else if (not printable_char(c)) {
        char buf[16];
        s::snprintf(buf, sizeof buf, c >= 0x10000 ? "\\U%.8x" : "\\u%.4x",
                    static_cast<unsigned>(c));
        out += buf;
      } else {
        out += s.substr(i, len);
      }
      i += len;
    }
    return out + "'";
  }
  if (not special) return str(s);
  // As zsh's quotedzputs: quotes open only around what needs them, so a
  // quote at either end is `\'` alone (`'a'` is `\''a'\'`).
  str  out;
  bool open = false;
  for (char c : s) {
    if (c == '\'') {
      if (open) out += '\'';
      open = false;
      out += "\\'";
    } else {
      if (not open) out += '\'';
      open = true;
      out += c;
    }
  }
  if (open) out += '\'';
  return out;
}

bool OptParse::has(char c) const noexcept {
  return s::any_of(flags.begin(), flags.end(),
                   [c](const Flag& f) { return f.c == c and f.on; });
}

bool OptParse::has_plus(char c) const noexcept {
  return s::any_of(flags.begin(), flags.end(),
                   [c](const Flag& f) { return f.c == c and not f.on; });
}

const str* OptParse::arg(char c) const noexcept {
  const str* found = nullptr;
  for (const auto& f : flags)
    if (f.c == c) found = &f.arg;
  return found;
}

bool parse_options(const vec<str>& args, strv letters, strv with_arg,
                   OptParse& out, bool plus, bool numbers) {
  strv cmd = args[0];
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "--" or a == "-") { ++i; break; }
    if (a.size() < 2 or not (a[0] == '-' or (plus and a[0] == '+'))) break;
    if (numbers and s::isdigit(static_cast<unsigned char>(a[1]))) break;
    bool on = a[0] == '-';
    for (s::size_t k = 1; k < a.size(); ++k) {
      char c = a[k];
      if (letters.find(c) == strv::npos and with_arg.find(c) == strv::npos) {
        s::fprintf(stderr, "jfsh: %.*s: bad option: %c%c\n",
                   static_cast<int>(cmd.size()), cmd.data(), a[0], c);
        return false;
      }
      OptParse::Flag f{c, on, {}};
      if (with_arg.find(c) != strv::npos) {
        if (k + 1 < a.size()) {
          f.arg = a.substr(k + 1);
        } else if (i + 1 < args.size()) {
          f.arg = args[++i];
        } else {
          s::fprintf(stderr, "jfsh: %.*s: argument expected: %c%c\n",
                     static_cast<int>(cmd.size()), cmd.data(), a[0], c);
          return false;
        }
        out.flags.push_back(s::move(f));
        break;
      }
      out.flags.push_back(s::move(f));
    }
  }
  out.next = i;
  return true;
}

str param_value_text(const Param& p) {
  if (p.is_array()) {
    // zsh's printparamvalue: `( a b )`, and `(  )` when empty.
    str out = "(";
    for (const auto& e : p.array()) out += ' ' + shell_quote(e);
    return out + (p.array().empty() ? "  )" : " )");
  }
  if (p.is_hash()) {
    auto h = p.hash();
    vec<str> keys;
    for (const auto& [k, v] : h) keys.push_back(k);
    s::sort(keys.begin(), keys.end());
    str out = "(";
    for (const auto& k : keys) out += " [" + shell_quote(k) + "]=" + shell_quote(h[k]);
    return out + " )";
  }
  if (p.is_integer()) return s::to_string(p.integer());
  if (p.is_float()) return p.scalar();
  return shell_quote(p.scalar());
}

vec<const Param*> sorted_params(const ParamTable& params,
                                const func<bool(const Param&)>& pred) {
  vec<const Param*> out;
  params.for_each([&](const Param& p) {
    if (pred(p)) out.push_back(&p);
  });
  s::sort(out.begin(), out.end(),
          [](const Param* a, const Param* b) { return a->name < b->name; });
  return out;
}

str home_dir(const ParamTable& params) {
  auto p = params.lookup("HOME");
  return (p and p->get().is_set()) ? p->get().scalar() : str{};
}

str tilde_abbrev(const ParamTable& params, strv dir) {
  str home = home_dir(params);
  while (home.size() > 1 and home.back() == '/') home.pop_back();
  if (home.empty() or home == "/") return str(dir);
  if (dir == home) return "~";
  if (dir.starts_with(home) and dir.size() > home.size() and dir[home.size()] == '/')
    return "~" + str(dir.substr(home.size()));
  return str(dir);
}

}  // namespace jfsh
