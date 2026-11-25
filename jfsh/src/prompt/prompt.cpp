// The prompt expander in this file follows zsh 5.9's Src/prompt.c
// (putpromptchar, prompttrunc, parsecolorchar, tsetcap's TSC_DIRTY) and
// ports ztrftime from Src/utils.c.
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

#include <jfsh/prompt/prompt.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <pwd.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/core/utf8.h>

namespace jfsh {

namespace {

str param_str(const ParamTable& params, strv name) {
  auto p = params.lookup(name);
  return (p and p->get().is_set()) ? p->get().scalar() : str{};
}

long param_long(const ParamTable& params, strv name) {
  return s::strtol(param_str(params, name).c_str(), nullptr, 10);
}

// $PWD with $HOME replaced by `~` (only at a component boundary, B36).
str tilde_path(const ParamTable& params, const str& dir) {
  str home = param_str(params, "HOME");
  while (home.size() > 1 and home.back() == '/') home.pop_back();
  if (home.empty() or home == "/") return dir;
  if (dir == home) return "~";
  if (dir.starts_with(home) and dir.size() > home.size() and dir[home.size()] == '/')
    return "~" + dir.substr(home.size());
  return dir;
}

str current_dir(const ParamTable& params) {
  str pwd = param_str(params, "PWD");
  if (not pwd.empty()) return pwd;
  char buf[4096];
  return ::getcwd(buf, sizeof buf) ? str(buf) : str(".");
}

// The last `n` components of `path` (n > 0) or the first -n (n < 0); the
// whole path when it has no more than that.  A leading `~` counts as a
// component, a leading `/` doesn't.
str path_components(const str& path, int n) {
  if (n == 0) return path;
  bool absolute = path.starts_with('/');
  vec<strv> comps;
  strv rest(path);
  while (not rest.empty()) {
    auto slash = rest.find('/');
    strv c = rest.substr(0, slash);
    if (not c.empty()) comps.push_back(c);
    if (slash == strv::npos) break;
    rest.remove_prefix(slash + 1);
  }
  s::size_t want = static_cast<s::size_t>(n < 0 ? -n : n);
  if (want >= comps.size()) return path;
  str out;
  if (n > 0) {
    for (s::size_t i = comps.size() - want; i < comps.size(); ++i) {
      if (not out.empty()) out += '/';
      out += comps[i];
    }
  } else {
    if (absolute) out = "/";
    for (s::size_t i = 0; i < want; ++i) {
      if (i > 0) out += '/';
      out += comps[i];
    }
  }
  return out;
}

// zsh's ztrftime: strftime with zsh's own %. (fractions of a second, %3.
// for milliseconds), %N, %f, %K, %L and the padding of %e %k %l, the rest
// left to strftime.
str format_time(strv fmt, const struct tm& tm, long nsec) {
  str out;
  for (s::size_t i = 0; i < fmt.size();) {
    if (fmt[i] != '%') {
      out += fmt[i++];
      continue;
    }
    s::size_t start = i++;
    bool strip = false;
    int digs = 3;
    if (i < fmt.size() and fmt[i] == '-') {
      strip = true;
      ++i;
    }
    if (i < fmt.size() and s::isdigit(static_cast<unsigned char>(fmt[i]))) {
      s::size_t dend = i + 1;
      while (dend < fmt.size() and s::isdigit(static_cast<unsigned char>(fmt[dend]))) ++dend;
      if (dend < fmt.size() and fmt[dend] == '.') {
        digs = s::atoi(str(fmt.substr(i, dend - i)).c_str());
        i = dend;
      }
    }
    // GNU padding flags and the like: strftime's.
    bool plain = i - start == 1 or (i - start == 2 and strip) or (i < fmt.size() and fmt[i] == '.');
    if (not plain) {
      while (i < fmt.size() and strv("OE^#_-0123456789").find(fmt[i]) != strv::npos) ++i;
      if (i < fmt.size()) {
        ++i;
        char buf[256];
        s::size_t n = s::strftime(buf, sizeof buf, str(fmt.substr(start, i - start)).c_str(), &tm);
        out.append(buf, n);
        continue;
      }
    }
    if (i >= fmt.size()) {
      out += '%';
      break;
    }
    char c = fmt[i++];
    auto two = [&](int v, bool pad_space, bool zero) {
      if (v > 9)       out += static_cast<char>('0' + v / 10);
      else if (not strip) {
        if (zero)           out += '0';
        else if (pad_space) out += ' ';
      }
      out += static_cast<char>('0' + v % 10);
    };
    switch (c) {
      case '.': {
        long f = nsec;
        if (digs < 0 or digs > 9) digs = 9;
        if (digs < 9) {
          long max = 100000000;
          for (int t = 8 - digs; t; --t) {
            max /= 10;
            f /= 10;
          }
          max -= 1;
          f = (f + 5) / 10;
          if (f > max) f = max;
        }
        char buf[16];
        s::snprintf(buf, sizeof buf, "%0*ld", digs, f);
        out += buf;
        break;
      }
      case 'f': strip = true; [[fallthrough]];
      case 'e': two(tm.tm_mday, true, false); break;
      case 'K': strip = true; [[fallthrough]];
      case 'H': case 'k': two(tm.tm_hour, true, c == 'H'); break;
      case 'L': strip = true; [[fallthrough]];
      case 'l': {
        int h = tm.tm_hour % 12;
        if (h == 0) h = 12;
        if (h > 9)          out += '1';
        else if (not strip) out += ' ';
        out += static_cast<char>('0' + h % 10);
        break;
      }
      case 'd': two(tm.tm_mday, false, true); break;
      case 'm': two(tm.tm_mon + 1, false, true); break;
      case 'M': two(tm.tm_min, false, true); break;
      case 'S': two(tm.tm_sec, false, true); break;
      case 'y': {
        int y = tm.tm_year;
        if (y > 9 or not strip) out += static_cast<char>('0' + (y / 10) % 10);
        out += static_cast<char>('0' + y % 10);
        break;
      }
      case 'N': {
        char buf[16];
        s::snprintf(buf, sizeof buf, "%09ld", nsec);
        out += buf;
        break;
      }
      case 'E': case 'O': case '^': case '#': case '_': case '-':
      case '0': case '1': case '2': case '3': case '4':
      case '5': case '6': case '7': case '8': case '9':
        // A flag after a plain start (`%E`, `%-O`): zsh's goto morefmt, the
        // flags and the next character go to strftime.
        // At the end of the format, a lone `%` (its case '\0').
        while (i < fmt.size() and strv("OE^#_-0123456789").find(fmt[i]) != strv::npos) ++i;
        if (i >= fmt.size()) {
          out += '%';
          break;
        }
        ++i;
        [[fallthrough]];
      default: {
        char buf[256];
        s::size_t n = s::strftime(buf, sizeof buf, str(fmt.substr(start, i - start)).c_str(), &tm);
        out.append(buf, n);
      }
    }
  }
  return out;
}

struct Now {
  struct tm tm{};
  long      nsec = 0;
};

Now local_now() {
  struct timespec ts{};
  ::clock_gettime(CLOCK_REALTIME, &ts);
  Now now;
  s::time_t t = ts.tv_sec;
  ::localtime_r(&t, &now.tm);
  now.nsec = ts.tv_nsec;
  return now;
}

str tty_name(bool short_form) {
  const char* t = ::ttyname(STDIN_FILENO);
  if (not t) return "()";
  strv name(t);
  if (short_form and name.starts_with("/dev/tty")) name.remove_prefix(8);
  else if (name.starts_with("/dev/"))              name.remove_prefix(5);
  return str(name);
}

// The SGR sequence for colour `spec` (a name, 0-255 or #rrggbb), as
// foreground or background.
str color_seq(strv spec, bool fg) {
  static constexpr const char* names[] = {
    "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};
  int base = fg ? 30 : 40;
  str def = fg ? "\x1b[39m" : "\x1b[49m";
  for (int i = 0; i < 8; ++i)
    if (spec == names[i]) return "\x1b[" + s::to_string(base + i) + "m";
  if (not spec.empty() and spec[0] == '#') {
    strv hex = spec.substr(1);
    if (hex.find_first_not_of("0123456789abcdefABCDEF") != strv::npos) return def;
    unsigned r, g, b;
    if (hex.size() == 6) {
      r = static_cast<unsigned>(s::stoul(str(hex.substr(0, 2)), nullptr, 16));
      g = static_cast<unsigned>(s::stoul(str(hex.substr(2, 2)), nullptr, 16));
      b = static_cast<unsigned>(s::stoul(str(hex.substr(4, 2)), nullptr, 16));
    } else if (hex.size() == 3) {
      r = static_cast<unsigned>(s::stoul(str(hex.substr(0, 1)), nullptr, 16)) * 17;
      g = static_cast<unsigned>(s::stoul(str(hex.substr(1, 1)), nullptr, 16)) * 17;
      b = static_cast<unsigned>(s::stoul(str(hex.substr(2, 1)), nullptr, 16)) * 17;
    } else {
      return def;
    }
    if (truecolor_mapper) return truecolor_mapper(r, g, b, fg);
    char buf[48];
    s::snprintf(buf, sizeof buf, "\x1b[%d;2;%u;%u;%um", fg ? 38 : 48, r, g, b);
    return buf;
  }
  if (not spec.empty() and spec.find_first_not_of("0123456789") == strv::npos) {
    int n = s::atoi(str(spec).c_str());
    if (n > 255) return def;
    if (n < 8) return "\x1b[" + s::to_string(base + n) + "m";
    return "\x1b[" + s::to_string(fg ? 38 : 48) + ";5;" + s::to_string(n) + "m";
  }
  return def;
}

// zsh's match_colour for a name in %F{…}: nullopt when it names no colour
// (the escape then acts as %f / %k).
opt<str> color_spec(strv name, bool fg) {
  static constexpr strv names[] = {
    "black", "red", "green", "yellow", "blue", "magenta", "cyan", "white"};
  for (strv n : names)
    if (name == n) return color_seq(name, fg);
  if (not name.empty() and name[0] == '#') return color_seq(name, fg);
  if (not name.empty() and name.find_first_not_of("0123456789") == strv::npos
      and s::atoi(str(name).c_str()) < 256)
    return color_seq(name, fg);
  return s::nullopt;
}

}  // namespace

str ztrftime(strv fmt, const struct tm& tm, long nsec) { return format_time(fmt, tm, nsec); }

// One expansion of a prompt string.
struct PromptExpander {
  const PromptRenderer& r;
  strv              src;
  s::size_t         pos = 0;
  bool              percent;
  bool              bang;

  struct Attrs {
    bool bold = false, underline = false, standout = false;
    str  fg, bg;
  } attrs;
  int  line_width = 0;     // columns on the current line, for %(l…)
  bool in_literal = false; // inside %{ … %}

  PromptExpander(const PromptRenderer& renderer, strv text)
    : r(renderer), src(text),
      percent(renderer.mOpts.get(OPT_PROMPTPERCENT)),
      bang(renderer.mOpts.get(OPT_PROMPTBANG)) {}

  str run() {
    str out;
    seq(out, '\0', false);
    if (in_literal) out += PROMPT_ZW_END;
    return out;
  }

  void text(str& out, strv t) {
    for (char c : t) {
      if (c == '\n')                                        line_width = 0;
      else if ((static_cast<unsigned char>(c) & 0xc0) != 0x80) ++line_width;
    }
    out += t;
  }

  void zero_width(str& out, strv seq_text) {
    if (in_literal) { out += seq_text; return; }
    out += PROMPT_ZW_START;
    out += seq_text;
    out += PROMPT_ZW_END;
  }

  str colors() const {
    str s;
    if (not attrs.fg.empty()) s += attrs.fg;
    if (not attrs.bg.empty()) s += attrs.bg;
    return s;
  }

  // zsh's tsetcap with TSC_DIRTY: after `cap`, the attributes still on
  // are sent again.
  str dirty(char cap) const {
    str s;
    if (attrs.bold and cap != 'B') s += "\x1b[1m";
    if (attrs.standout)            s += "\x1b[7m";
    if (attrs.underline)           s += "\x1b[4m";
    s += attrs.fg;
    s += attrs.bg;
    return s;
  }

  int columns() const { return static_cast<int>(param_long(r.mParams, "COLUMNS")); }

  // The width of what is on the prompt's current line so far.
  int current_column(const str& out) const {
    s::size_t nl = out.rfind('\n');
    return PromptRenderer::visual_width(strv(out).substr(nl == str::npos ? 0 : nl + 1));
  }

  // Renders until the unquoted character `stop` (left for the caller) or
  // the end; with stop_at_trunc, also until the next truncation, which is
  // left in place (zsh's putpromptchar and its truncwidth check).
  void seq(str& out, char stop, bool stop_at_trunc) {
    while (pos < src.size()) {
      char c = src[pos];
      if (stop and c == stop) return;
      if (bang and c == '!') {
        ++pos;
        if (pos < src.size() and src[pos] == '!') {
          ++pos;
          text(out, "!");
        } else {
          text(out, param_str(r.mParams, "HISTCMD").empty()
                        ? "0" : param_str(r.mParams, "HISTCMD"));
        }
        continue;
      }
      if (not percent or c != '%') {
        text(out, strv(&src[pos], 1));
        ++pos;
        continue;
      }
      s::size_t start = pos++;
      bool minus = false, has_num = false;
      int  num = 0;
      if (pos < src.size() and src[pos] == '-') {
        minus = true;
        ++pos;
      }
      if (pos < src.size() and src[pos] >= '0' and src[pos] <= '9') {
        has_num = true;
        num = number();
        if (minus) num = -num;
      } else if (minus) {
        has_num = true;
        num = -1;
      }
      // A `%` at the end, with or without a number, is nothing.
      if (pos >= src.size()) {
        pos = src.size();
        return;
      }
      char e = src[pos++];
      if ((e == '<' or e == '>' or e == '[') and stop_at_trunc) {
        pos = start;
        return;
      }
      if (e == '(') {
        conditional(out, num, minus);
        continue;
      }
      escape(out, e, num, has_num, minus, stop);
    }
  }

  // zsh's zstrtol on the digits at `pos`, into putpromptchar's int: past
  // 64 bits the rest is dropped with a warning, the value wrapping as C's
  // conversions do.
  int number() {
    s::size_t from = pos;
    s::uint64_t calc = 0;
    opt<s::size_t> trunc;
    for (; pos < src.size() and src[pos] >= '0' and src[pos] <= '9'; ++pos) {
      if (trunc) continue;
      s::uint64_t next = calc * 10 + static_cast<s::uint64_t>(src[pos] - '0');
      if (next < calc) {
        trunc = pos;
        continue;
      }
      calc = next;
    }
    if (not trunc and static_cast<s::int64_t>(calc) < 0) {
      trunc = pos - 1;
      calc /= 10;
    }
    if (trunc) {
      s::fprintf(stderr, "jfsh: number truncated after %d digits: %.*s\n",
                 static_cast<int>(*trunc - from), static_cast<int>(src.size() - from),
                 src.data() + from);
    }
    return static_cast<int>(static_cast<s::int64_t>(calc));
  }

  // zsh's parsecolorchar: %F{name} (the name prompt-expanded first), else
  // the number before the F; nullopt for one that isn't a colour.
  opt<str> colour(bool fg, int num) {
    if (pos < src.size() and src[pos] == '{') {
      auto end = src.find('}', pos + 1);
      if (end == strv::npos) {
        pos = src.size();
        return color_seq("0", fg);
      }
      strv inner = src.substr(pos + 1, end - pos - 1);
      pos = end + 1;
      PromptExpander sub(r, inner);
      sub.percent = true;
      sub.bang    = false;
      str name = PromptRenderer::strip_markers(sub.run(), true);
      return color_spec(name, fg);
    }
    return color_seq(s::to_string(num), fg);
  }

  void escape(str& out, char e, int num, bool has_num, bool minus, char stop) {
    const ParamTable& params = r.mParams;
    switch (e) {
      case '%': text(out, "%"); break;
      case ')': text(out, ")"); break;
      case 'd': case '/':
        text(out, path_components(current_dir(params), num));
        break;
      case '~':
        text(out, path_components(tilde_path(params, current_dir(params)), num));
        break;
      case 'c': case '.':
        text(out, path_components(tilde_path(params, current_dir(params)), num ? num : 1));
        break;
      case 'C':
        text(out, path_components(current_dir(params), num ? num : 1));
        break;
      case 'n': {
        str user = param_str(params, "USERNAME");
        if (user.empty()) {
          struct passwd* pw = ::getpwuid(::geteuid());
          user = pw ? pw->pw_name : "";
        }
        text(out, user);
        break;
      }
      case 'M':
        text(out, param_str(params, "HOST"));
        break;
      case 'm': {
        // The first `num` parts of $HOST, or the last -num.
        auto hp = params.lookup("HOST");
        if (not hp or not hp->get().is_set()) break;
        str h = hp->get().scalar();
        int arg = num ? num : 1;
        if (arg < 0) {
          s::size_t i = h.size();
          for (; i > 0; --i)
            if (h[i - 1] == '.' and not ++arg) break;
          text(out, strv(h).substr(i));
        } else {
          s::size_t i = 0;
          for (; i < h.size(); ++i)
            if (h[i] == '.' and not --arg) break;
          text(out, strv(h).substr(0, i));
        }
        break;
      }
      case 'y': text(out, tty_name(false)); break;
      case 'l': text(out, tty_name(true)); break;
      case '#': text(out, ::geteuid() == 0 ? "#" : "%"); break;
      case '?': {
        str st = param_str(params, "status");
        text(out, st.empty() ? "0" : st);
        break;
      }
      case 'h': case '!': {
        str h = param_str(params, "HISTCMD");
        text(out, h.empty() ? "0" : h);
        break;
      }
      case 'i': case 'I': {
        str l = param_str(params, "LINENO");
        text(out, l.empty() ? "0" : l);
        break;
      }
      case 'j': text(out, s::to_string(r.mHooks.job_count ? r.mHooks.job_count() : 0)); break;
      case 'L': text(out, s::to_string(param_long(params, "SHLVL"))); break;
      case 'e': text(out, s::to_string(r.mHooks.eval_depth ? r.mHooks.eval_depth() : 0)); break;
      case 'N': case 'x': {
        // %x: the file the code came from; else, as %N, the script or
        // function name.
        str name = e == 'x' and r.mHooks.file_name ? r.mHooks.file_name()
                 : r.mHooks.script_name             ? r.mHooks.script_name()
                                                    : str("jfsh");
        text(out, path_components(name, num));
        break;
      }
      case '_': case '^': break;   // the parser's state: jfsh doesn't track it
      case 'D':
        if (pos < src.size() and src[pos] == '{') {
          str fmt;
          s::size_t i = pos + 1;
          for (; i < src.size() and src[i] != '}'; ++i) {
            if (src[i] == '\\' and i + 1 < src.size()) ++i;
            fmt += src[i];
          }
          pos = i < src.size() ? i + 1 : src.size();
          if (not fmt.empty()) text(out, time_text(fmt));
        } else {
          text(out, time_text("%y-%m-%d"));
        }
        break;
      case 'T': text(out, time_text("%K:%M")); break;
      case '*': text(out, time_text("%K:%M:%S")); break;
      case 't': case '@': text(out, time_text("%l:%M%p")); break;
      case 'w': text(out, time_text("%a %f")); break;
      case 'W': text(out, time_text("%m/%d/%y")); break;
      case 'v': {
        vec<str> v = psvar();
        long arg = num;
        if (not arg) arg = 1;
        else if (arg < 0) arg += static_cast<long>(v.size()) + 1;
        if (arg > 0 and arg <= static_cast<long>(v.size()))
          text(out, v[static_cast<s::size_t>(arg - 1)]);
        break;
      }
      case 'E': zero_width(out, "\x1b[K"); break;
      case 'B':
        attrs.bold = true;
        zero_width(out, "\x1b[1m" + dirty('B'));
        break;
      case 'b':
        attrs.bold = false;
        zero_width(out, "\x1b[0m" + dirty('b'));
        break;
      case 'U': attrs.underline = true;  zero_width(out, "\x1b[4m");  break;
      case 'u':
        attrs.underline = false;
        zero_width(out, "\x1b[24m" + dirty('u'));
        break;
      case 'S': attrs.standout = true;   zero_width(out, "\x1b[7m");  break;
      case 's':
        attrs.standout = false;
        zero_width(out, "\x1b[27m" + dirty('s'));
        break;
      case 'F': case 'K': {
        bool fg = e == 'F';
        opt<str> seq_text = colour(fg, num);
        if (seq_text) {
          (fg ? attrs.fg : attrs.bg) = *seq_text;
          zero_width(out, *seq_text);
          break;
        }
        // Not a colour: %f / %k.
        (fg ? attrs.fg : attrs.bg).clear();
        zero_width(out, fg ? "\x1b[39m" : "\x1b[49m");
        break;
      }
      case 'f': attrs.fg.clear(); zero_width(out, "\x1b[39m"); break;
      case 'k': attrs.bg.clear(); zero_width(out, "\x1b[49m"); break;
      case '{':
        if (not in_literal) out += PROMPT_ZW_START;
        in_literal = true;
        break;
      case '}':
        if (in_literal) out += PROMPT_ZW_END;
        in_literal = false;
        break;
      case 'G': break;
      case '[': {
        int arg = num;
        if (pos < src.size() and src[pos] >= '0' and src[pos] <= '9') {
          arg = number();
        }
        truncation(out, ']', arg, stop);
        break;
      }
      case '<': case '>': {
        int arg = num;
        // %-N<…<: N columns short of the right margin.
        if (minus) {
          arg = columns() - current_column(out) + arg;
          if (arg <= 0) arg = 1;
        }
        --pos;   // prompttrunc starts on the < or >
        truncation(out, e, arg, stop);
        break;
      }
      default: break;   // unknown escapes expand to nothing, as in zsh
    }
    (void)has_num;
  }

  str time_text(strv fmt) const {
    auto now = local_now();
    return format_time(fmt, now.tm, now.nsec);
  }

  vec<str> psvar() const {
    auto p = r.mParams.lookup("psvar");
    if (not p or not p->get().is_set()) return {};
    if (p->get().is_array()) return p->get().array();
    return {p->get().scalar()};
  }

  // zsh's %(x.true.false) tests: 1 true, 0 false, -1 not a test (neither
  // branch is shown).
  int test(char t, int arg, bool minus, const str& out) {
    const ParamTable& params = r.mParams;
    auto tm = local_now().tm;
    switch (t) {
      case 'c': case '.': case '~': case '/': case 'C': {
        str d = current_dir(params);
        if (t == 'c' or t == '.' or t == '~') {
          str home = param_str(params, "HOME");
          while (home.size() > 1 and home.back() == '/') home.pop_back();
          if (not home.empty() and home != "/" and (d == home or d.starts_with(home + "/"))) {
            --arg;
            d = d.substr(home.size());
          }
        }
        // `/' gives 0, `/any' gives 1, etc.
        s::size_t i = 0;
        if (i < d.size() and d[i++] == '/' and i < d.size()) --arg;
        for (; i < d.size(); ++i)
          if (d[i] == '/') --arg;
        return arg <= 0;
      }
      case 't': return arg == tm.tm_min;
      case 'T': return arg == tm.tm_hour;
      case 'd': return arg == tm.tm_mday;
      case 'D': return arg == tm.tm_mon;
      case 'w': return arg == tm.tm_wday;
      case '?': return s::atoi(param_str(params, "status").c_str()) == arg;
      case '#': return static_cast<int>(::geteuid()) == arg;
      case 'g': return static_cast<int>(::getegid()) == arg;
      case 'j': return (r.mHooks.job_count ? r.mHooks.job_count() : 0) >= arg;
      case 'l': {
        int t0 = current_column(out);
        if (minus) t0 = columns() - t0;
        return t0 >= arg;
      }
      case 'e': return (r.mHooks.eval_depth ? r.mHooks.eval_depth() : 0) >= arg;
      case 'L': return param_long(params, "SHLVL") >= arg;
      case 'S': return param_long(params, "SECONDS") >= arg;
      case 'v': return static_cast<int>(psvar().size()) >= arg;
      case 'V': {
        vec<str> v = psvar();
        int i = arg ? arg : 1;
        int size = static_cast<int>(v.size());
        return not v.empty() and size >= arg and i <= size
           and not v[static_cast<s::size_t>(i - 1)].empty();
      }
      case '_': return 0 >= arg;
      case '!': return ::geteuid() == 0;
      default:  return -1;
    }
  }

  // %N(x.true.false): either branch may hold further escapes; the one not
  // taken leaves no trace, not even its attribute changes.
  void conditional(str& out, int arg, bool minus) {
    if (pos < src.size() and src[pos] >= '0' and src[pos] <= '9') {
      arg = 0;
      arg = number();
    } else if (arg < 0) {
      arg = -arg;
    }
    if (pos + 1 >= src.size()) {
      pos = src.size();
      return;
    }
    int t = test(src[pos++], arg, minus, out);
    char delim = src[pos++];

    Attrs saved = attrs;
    int   width = line_width;
    str yes, no;
    seq(yes, delim, false);
    // A branch the string ends in ends the whole expansion, shown as far
    // as it got if it was the one to show.
    if (pos >= src.size()) {
      if (t == 1) out += yes;
      else        attrs = saved, line_width = width;
      return;
    }
    ++pos;
    Attrs yes_attrs = attrs;
    int   yes_width = line_width;
    attrs      = saved;
    line_width = width;
    seq(no, ')', false);
    if (pos < src.size()) ++pos;
    if (t == 1) {
      out += yes;
      attrs      = yes_attrs;
      line_width = yes_width;
    } else if (t == 0) {
      out += no;
    } else {
      attrs      = saved;
      line_width = width;
    }
  }

  // zsh's prompttrunc: %N<str<, %N>str> and %N[xstr] cut what follows, up
  // to the next truncation or the end of the enclosing group, to N
  // columns, `str` marking where.  `pos` is on the < or > (or after the
  // [ and its number).
  void truncation(str& out, char truncchar, int arg, char stop) {
    char ch = pos < src.size() ? src[pos] : '\0';
    bool left = ch == '<';
    if (ch != ']') ++pos;
    str repl;
    while (pos < src.size() and src[pos] != truncchar) {
      if (src[pos] == '\\' and pos + 1 < src.size()) ++pos;
      repl += src[pos++];
    }
    if (pos >= src.size()) return;
    ++pos;
    if (arg <= 0) return;   // %<< ends a truncation
    if (repl.empty() and truncchar == ']') repl = "<";
    int width = line_width;
    str part;
    seq(part, stop, true);
    str cut = truncate(part, arg, repl, left);
    out += cut;
    line_width = width + PromptRenderer::visual_width(cut);
  }

  static str truncate(strv text, int limit, strv repl, bool cut_left) {
    struct Piece { strv bytes; int width; };
    vec<Piece> pieces;
    int total = 0;
    for (s::size_t i = 0; i < text.size();) {
      if (text[i] == PROMPT_ZW_START) {
        auto end = text.find(PROMPT_ZW_END, i);
        end = end == strv::npos ? text.size() : end + 1;
        pieces.push_back({text.substr(i, end - i), -1});
        i = end;
        continue;
      }
      s::size_t len;
      char32_t c = utf8_decode(text, i, len);
      int w = c == '\n' ? 0 : char_width(c);
      pieces.push_back({text.substr(i, len), w});
      total += w;
      i += len;
    }
    if (total <= limit) return str(text);
    int twidth = text_width(repl);
    if (twidth >= limit) return str(repl);
    int maxwidth = limit - twidth;
    str out;
    if (cut_left) {
      // The truncation string, then what is left once the width is no
      // more than maxwidth; zero-width parts on the way are kept.
      out = repl;
      int remw = total;
      s::size_t i = 0;
      for (; i < pieces.size() and remw > maxwidth; ++i) {
        if (pieces[i].width < 0) out += pieces[i].bytes;
        else                     remw -= pieces[i].width;
      }
      for (; i < pieces.size(); ++i) out += pieces[i].bytes;
      return out;
    }
    // At the right: characters until the width is reached (the last may
    // go past it), the truncation string, then the zero-width parts left.
    s::size_t i = 0;
    for (; i < pieces.size() and maxwidth > 0; ++i) {
      out += pieces[i].bytes;
      if (pieces[i].width > 0) maxwidth -= pieces[i].width;
    }
    out += repl;
    for (; i < pieces.size(); ++i)
      if (pieces[i].width < 0) out += pieces[i].bytes;
    return out;
  }
};

void PromptRenderer::reload() {
  auto get = [&](const char* name, str def) -> str {
    auto p = mParams.lookup(name);
    return (p and p->get().is_set()) ? p->get().scalar() : def;
  };
  // The shell starts them (install_special_params, as zsh's setupvals).
  // Each is the string its names share (PS1 with PROMPT and prompt, RPS1
  // with RPROMPT, …), whichever name is unset: zsh's zle reads the string.
  auto shared = [&](const char* name) -> str {
    auto p = mParams.lookup(name);
    return p ? p->get().scalar() : str{};
  };
  mVars.PS1    = shared("PS1");
  mVars.PS2    = shared("PS2");
  mVars.PS3    = shared("PS3");
  mVars.PS4    = shared("PS4");
  mVars.RPS1   = shared("RPS1");
  mVars.RPS2   = shared("RPS2");
  mVars.SPROMPT= get("SPROMPT",  "");
}

str PromptRenderer::expand(strv prompt_str) const {
  str source;
  if (mOpts.get(OPT_PROMPTSUBST) and mHooks.subst) {
    source = mHooks.subst(prompt_str);
    prompt_str = source;
  }
  PromptExpander x(*this, prompt_str);
  return x.run();
}

str PromptRenderer::render(strv prompt_str, bool strip_escapes) const {
  return strip_markers(expand(prompt_str), strip_escapes);
}

str PromptRenderer::strip_markers(strv expanded, bool drop_zero_width) {
  str out;
  out.reserve(expanded.size());
  bool zw = false;
  for (char c : expanded) {
    if (c == PROMPT_ZW_START) { zw = true; continue; }
    if (c == PROMPT_ZW_END)   { zw = false; continue; }
    if (zw and drop_zero_width) continue;
    out += c;
  }
  return out;
}

int PromptRenderer::visual_width(strv expanded) noexcept {
  int width = 0;
  for (s::size_t i = 0; i < expanded.size();) {
    char c = expanded[i];
    if (c == PROMPT_ZW_START) {
      auto end = expanded.find(PROMPT_ZW_END, i);
      i = end == strv::npos ? expanded.size() : end + 1;
      continue;
    }
    if (c == '\x1b') {   // an escape sequence outside %{…%}
      ++i;
      if (i < expanded.size() and expanded[i] == '[') {
        ++i;
        while (i < expanded.size() and not (expanded[i] >= 0x40 and expanded[i] <= 0x7e)) ++i;
      }
      if (i < expanded.size()) ++i;
      continue;
    }
    if (c == '\n' or c == '\r') { width = 0; ++i; continue; }
    s::size_t len;
    width += char_width(utf8_decode(expanded, i, len));
    i += len;
  }
  return width;
}

}  // namespace jfsh
