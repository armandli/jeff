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

// strftime with zsh's additions: %f, %K and %L are %e, %k and %l without
// the leading space.
str format_time(strv fmt, const struct tm& tm) {
  str f;
  for (s::size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%' or i + 1 >= fmt.size()) { f += fmt[i]; continue; }
    char c = fmt[++i];
    char num[8];
    switch (c) {
      case 'f': s::snprintf(num, sizeof num, "%d", tm.tm_mday); f += num; break;
      case 'K': s::snprintf(num, sizeof num, "%d", tm.tm_hour); f += num; break;
      case 'L': s::snprintf(num, sizeof num, "%d", tm.tm_hour % 12 ? tm.tm_hour % 12 : 12);
                f += num; break;
      default:  f += '%'; f += c;
    }
  }
  if (f.empty()) return {};
  char buf[256];
  s::size_t n = s::strftime(buf, sizeof buf, f.c_str(), &tm);
  return str(buf, n);
}

struct tm local_now() {
  s::time_t t = s::time(nullptr);
  struct tm tm{};
  ::localtime_r(&t, &tm);
  return tm;
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

}  // namespace

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

  // Renders until the unquoted character `stop` (left for the caller) or
  // the end; with stop_at_trunc, also until the next truncation, which is
  // left in place.
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
      if (not percent or c != '%' or pos + 1 >= src.size()) {
        text(out, strv(&src[pos], 1));
        ++pos;
        continue;
      }
      s::size_t start = pos++;
      bool has_num = false, neg = false;
      int  num = 0;
      if (src[pos] == '-') { neg = true; ++pos; }
      while (pos < src.size() and src[pos] >= '0' and src[pos] <= '9') {
        has_num = true;
        num = num * 10 + (src[pos++] - '0');
      }
      if (neg) { has_num = true; num = -num; }
      if (pos >= src.size()) break;
      char e = src[pos++];
      if ((e == '<' or e == '>' or e == '[') and stop_at_trunc) {
        pos = start;
        return;
      }
      escape(out, e, num, has_num, stop);
    }
  }

  void escape(str& out, char e, int num, bool has_num, char stop) {
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
        text(out, path_components(tilde_path(params, current_dir(params)), has_num ? num : 1));
        break;
      case 'C':
        text(out, path_components(current_dir(params), has_num ? num : 1));
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
      case 'm': case 'M': {
        char host[256] = {};
        ::gethostname(host, sizeof host - 1);
        str h = host;
        if (e == 'm') {
          int n = has_num ? num : 1;
          vec<str> parts;
          s::size_t at = 0;
          for (;;) {
            auto dot = h.find('.', at);
            parts.push_back(h.substr(at, dot == str::npos ? str::npos : dot - at));
            if (dot == str::npos) break;
            at = dot + 1;
          }
          s::size_t want = static_cast<s::size_t>(n < 0 ? -n : n);
          if (n != 0 and want < parts.size()) {
            str o;
            s::size_t from = n > 0 ? 0 : parts.size() - want;
            for (s::size_t i = from; i < from + want; ++i) o += (i > from ? "." : "") + parts[i];
            h = o;
          }
        }
        text(out, h);
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
      case 'N': case 'x':
        text(out, r.mHooks.script_name ? r.mHooks.script_name() : str("jfsh"));
        break;
      case '_': case '^': break;   // the parser's state: jfsh doesn't track it
      case 'D':
        if (pos < src.size() and src[pos] == '{') {
          auto end = src.find('}', pos + 1);
          strv fmt = src.substr(pos + 1, end == strv::npos ? strv::npos : end - pos - 1);
          pos = end == strv::npos ? src.size() : end + 1;
          text(out, format_time(fmt, local_now()));
        } else {
          text(out, format_time("%y-%m-%d", local_now()));
        }
        break;
      case 'T': text(out, format_time("%K:%M", local_now())); break;
      case '*': text(out, format_time("%K:%M:%S", local_now())); break;
      case 't': case '@': text(out, format_time("%L:%M%p", local_now())); break;
      case 'w': text(out, format_time("%a %f", local_now())); break;
      case 'W': text(out, format_time("%m/%d/%y", local_now())); break;
      case 'v': {
        auto p = params.lookup("psvar");
        vec<str> v = p ? p->get().array() : vec<str>{};
        int n = has_num ? num : 1;
        long idx = n > 0 ? n - 1 : static_cast<long>(v.size()) + n;
        if (idx >= 0 and idx < static_cast<long>(v.size()))
          text(out, v[static_cast<s::size_t>(idx)]);
        break;
      }
      case 'E': zero_width(out, "\x1b[K"); break;
      case 'B':
        attrs.bold = true;
        zero_width(out, "\x1b[1m" + colors());
        break;
      case 'b':
        if (attrs.bold) {
          attrs.bold = false;
          str s = "\x1b[0m";
          if (attrs.standout)  s += "\x1b[7m";
          if (attrs.underline) s += "\x1b[4m";
          zero_width(out, s + colors());
        }
        break;
      case 'U': attrs.underline = true;  zero_width(out, "\x1b[4m");  break;
      case 'u': attrs.underline = false; zero_width(out, "\x1b[24m"); break;
      case 'S': attrs.standout = true;   zero_width(out, "\x1b[7m");  break;
      case 's': attrs.standout = false;  zero_width(out, "\x1b[27m"); break;
      case 'F': case 'K': {
        bool fg = e == 'F';
        str spec;
        if (pos < src.size() and src[pos] == '{') {
          auto end = src.find('}', pos + 1);
          spec = str(src.substr(pos + 1, end == strv::npos ? strv::npos : end - pos - 1));
          pos = end == strv::npos ? src.size() : end + 1;
        } else if (has_num) {
          spec = s::to_string(num);
        }
        str seq_text = color_seq(spec, fg);
        (fg ? attrs.fg : attrs.bg) = seq_text;
        zero_width(out, seq_text);
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
      case '(': conditional(out, num, has_num); break;
      case '<': case '>': case '[': truncation(out, e, num, has_num, stop); break;
      default: break;   // unknown escapes expand to nothing, as in zsh
    }
  }

  bool test(char t, int n) {
    const ParamTable& params = r.mParams;
    auto tm = local_now();
    auto dir_depth = [&](bool tilde) {
      str d = current_dir(params);
      if (tilde) d = tilde_path(params, d);
      int count = 0;
      bool in = false;
      for (char c : d) {
        if (c != '/' and not in) ++count;
        in = c != '/';
      }
      return count;
    };
    switch (t) {
      case '!': return ::geteuid() == 0;
      case '#': return static_cast<int>(::geteuid()) == n;
      case '?': {
        str st = param_str(params, "status");
        return s::atoi(st.c_str()) == n;
      }
      case '_': return n <= 0;
      case '/': case 'C': return dir_depth(false) >= n;
      case '~': case 'c': case '.': return dir_depth(true) >= n;
      case 'D': return tm.tm_mon == n;
      case 'd': return tm.tm_mday == n;
      case 'e': return (r.mHooks.eval_depth ? r.mHooks.eval_depth() : 0) >= n;
      case 'g': return static_cast<int>(::getgid()) == n;
      case 'j': return (r.mHooks.job_count ? r.mHooks.job_count() : 0) >= n;
      case 'L': return param_long(params, "SHLVL") >= n;
      case 'l': return line_width >= n;
      case 'S': return param_long(params, "SECONDS") >= n;
      case 'T': return tm.tm_hour == n;
      case 't': return tm.tm_min == n;
      case 'u': return static_cast<int>(::getuid()) == n;
      case 'w': return tm.tm_wday == n;
      case 'v': case 'V': {
        auto p = params.lookup("psvar");
        vec<str> v = p ? p->get().array() : vec<str>{};
        if (t == 'v') return static_cast<int>(v.size()) >= n;
        return n >= 1 and n <= static_cast<int>(v.size())
           and not v[static_cast<s::size_t>(n - 1)].empty();
      }
      default: return false;
    }
  }

  // %N(x.true.false): either branch may hold further escapes; the one not
  // taken leaves no trace, not even its attribute changes.
  void conditional(str& out, int num, bool has_num) {
    if (not has_num) {
      while (pos < src.size() and src[pos] >= '0' and src[pos] <= '9')
        num = num * 10 + (src[pos++] - '0');
    }
    if (pos + 1 >= src.size()) { pos = src.size(); return; }
    char t     = src[pos++];
    char delim = src[pos++];
    static constexpr strv known = "!#?_/C~c.DdegjLlSTtuwvV";
    bool valid = known.find(t) != strv::npos;
    bool cond  = valid and test(t, num);

    Attrs saved = attrs;
    int   width = line_width;
    str yes, no;
    seq(yes, delim, false);
    if (pos < src.size()) ++pos;
    Attrs yes_attrs = attrs;
    int   yes_width = line_width;
    attrs      = saved;
    line_width = width;
    seq(no, ')', false);
    if (pos < src.size()) ++pos;
    if (not valid) {
      attrs      = saved;
      line_width = width;
    } else if (cond) {
      out += yes;
      attrs      = yes_attrs;
      line_width = yes_width;
    } else {
      out += no;
    }
  }

  // %N<str< and %N>str>: what follows, up to the next truncation or the
  // end of the enclosing group, is cut to N columns, `str` marking where.
  void truncation(str& out, char dir, int num, bool has_num, char stop) {
    char close = dir;
    if (dir == '[') {
      close = ']';
      dir = '>';
      if (pos < src.size() and (src[pos] == '<' or src[pos] == '>')) dir = src[pos++];
    }
    str repl;
    while (pos < src.size() and src[pos] != close) {
      if (src[pos] == '\\' and pos + 1 < src.size()) ++pos;
      repl += src[pos++];
    }
    if (pos < src.size()) ++pos;
    if (not has_num or num == 0) return;   // %<< ends a truncation
    int limit = num < 0 ? -num : num;
    int width = line_width;
    str part;
    seq(part, stop, true);
    str cut = truncate(part, limit, repl, dir == '<');
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
    int avail = s::max(0, limit - text_width(repl));
    str kept, dropped;
    int used = 0;
    if (cut_left) {
      s::size_t i = pieces.size();
      while (i > 0) {
        const Piece& p = pieces[i - 1];
        if (p.width >= 0 and used + p.width > avail) break;
        if (p.width > 0) used += p.width;
        --i;
      }
      for (s::size_t k = 0; k < i; ++k)
        if (pieces[k].width < 0) dropped += pieces[k].bytes;
      for (s::size_t k = i; k < pieces.size(); ++k) kept += pieces[k].bytes;
      return dropped + str(repl) + kept;
    }
    s::size_t i = 0;
    while (i < pieces.size()) {
      const Piece& p = pieces[i];
      if (p.width >= 0 and used + p.width > avail) break;
      if (p.width > 0) used += p.width;
      kept += p.bytes;
      ++i;
    }
    for (s::size_t k = i; k < pieces.size(); ++k)
      if (pieces[k].width < 0) dropped += pieces[k].bytes;
    return kept + str(repl) + dropped;
  }
};

void PromptRenderer::reload() {
  auto get = [&](const char* name, str def) -> str {
    auto p = mParams.lookup(name);
    return (p and p->get().is_set()) ? p->get().scalar() : def;
  };
  mVars.PS1    = get("PROMPT",   get("PS1",   "%n@%m %1~ %# "));
  mVars.PS2    = get("PROMPT2",  get("PS2",   "%_> "));
  mVars.PS3    = get("PROMPT3",  get("PS3",   "?# "));
  mVars.PS4    = get("PROMPT4",  get("PS4",   "+%N:%i> "));
  mVars.RPS1   = get("RPROMPT",  get("RPS1",  ""));
  mVars.RPS2   = get("RPROMPT2", get("RPS2",  ""));
  mVars.SPROMPT= get("SPROMPT",  "jfsh: correct '%R' to '%r' [nyae]? ");
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
