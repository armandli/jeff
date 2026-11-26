#include <jfsh/modules/curses.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include <algorithm>
#include <map>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

// curses.h without its function-like macros (`move`, `clear`, `refresh`,
// …), which would rewrite C++ names; it comes last.
#ifdef JFSH_HAVE_CURSES
#define NCURSES_NOMACROS
#include <curses.h>
#endif

// zsh/curses (M11): `zcurses`, as zsh's Src/Modules/curses.c.  The
// screen comes from newterm() on stdout/stdin rather than initscr(), so a
// terminal that can't be set up is an error status, not an exit.

namespace jfsh {

namespace {

int err(strv msg) { return mod_error("zcurses", msg); }

#ifdef JFSH_HAVE_CURSES

struct CursesState {
  SCREEN*                   screen = nullptr;
  s::map<str, WINDOW*>      windows;        // includes "stdscr"
  s::map<str, short>        pairs;          // "fg/bg" → colour pair
  short                     next_pair = 1;
};

CursesState& cs() {
  static CursesState st;
  return st;
}

struct Named { const char* name; int value; };

constexpr Named kAttrs[] = {
  {"blink", A_BLINK}, {"bold", A_BOLD}, {"dim", A_DIM}, {"reverse", A_REVERSE},
  {"standout", A_STANDOUT}, {"underline", A_UNDERLINE},
};

constexpr Named kColors[] = {
  {"default", -1}, {"black", COLOR_BLACK}, {"red", COLOR_RED}, {"green", COLOR_GREEN},
  {"yellow", COLOR_YELLOW}, {"blue", COLOR_BLUE}, {"magenta", COLOR_MAGENTA},
  {"cyan", COLOR_CYAN}, {"white", COLOR_WHITE},
};

opt<int> color_number(const str& name) {
  for (const auto& c : kColors)
    if (name == c.name) return c.value;
  char* end = nullptr;
  long n = s::strtol(name.c_str(), &end, 10);
  if (not name.empty() and not *end and n >= 0 and n < COLORS) return static_cast<int>(n);
  return s::nullopt;
}

// The colour pair for "fg/bg", allocated on first use.
opt<short> color_pair(const str& spec) {
  auto slash = spec.find('/');
  if (slash == str::npos or not ::has_colors()) return s::nullopt;
  auto fg = color_number(spec.substr(0, slash));
  auto bg = color_number(spec.substr(slash + 1));
  if (not fg or not bg) return s::nullopt;
  auto& st = cs();
  if (auto it = st.pairs.find(spec); it != st.pairs.end()) return it->second;
  if (st.next_pair >= COLOR_PAIRS) return s::nullopt;
  short n = st.next_pair++;
  ::init_pair(n, static_cast<short>(*fg), static_cast<short>(*bg));
  st.pairs[spec] = n;
  return n;
}

WINDOW* window(const str& name) {
  auto& w = cs().windows;
  auto it = w.find(name);
  return it == w.end() ? nullptr : it->second;
}

int need(const vec<str>& a, s::size_t n, strv sub) {
  if (a.size() < n) return err(str(sub) + ": not enough arguments");
  return 0;
}

int num(const str& s) { return static_cast<int>(s::strtol(s.c_str(), nullptr, 10)); }

void set_params(ExecContext& ctx) {
  vec<str> names;
  for (const auto& [n, _] : cs().windows) names.push_back(n);
  (void)ctx.params.set_array("zcurses_windows", names);
  if (cs().screen) {
    (void)ctx.params.set_scalar("ZCURSES_COLORS", s::to_string(COLORS));
    (void)ctx.params.set_scalar("ZCURSES_COLOR_PAIRS", s::to_string(COLOR_PAIRS));
  }
}

int sub_init(ExecContext& ctx) {
  auto& st = cs();
  if (st.screen) return 0;
  s::fflush(stdout);
  str term = param_or(ctx.params, "TERM");
  st.screen = ::newterm(term.empty() ? nullptr : const_cast<char*>(term.c_str()), stdout, stdin);
  if (not st.screen) return err("failed to initialise terminal " + term);
  ::set_term(st.screen);
  ::noecho();
  ::cbreak();
  ::keypad(stdscr, TRUE);
  if (::has_colors()) {
    ::start_color();
    ::use_default_colors();
  }
  st.windows["stdscr"] = stdscr;
  set_params(ctx);
  return 0;
}

int sub_end(ExecContext& ctx) {
  auto& st = cs();
  if (not st.screen) return err("not initialised");
  for (auto& [name, w] : st.windows)
    if (name != "stdscr") ::delwin(w);
  ::endwin();
  ::delscreen(st.screen);
  st = CursesState{};
  s::fflush(stdout);
  set_params(ctx);
  return 0;
}

// `attr win [+|-]attr fg/bg …`, and `bg win …` for the background.
int sub_attr(const vec<str>& a, bool background) {
  WINDOW* w = window(a[0]);
  if (not w) return err("window `" + a[0] + "' does not exist");
  int rc = 0;
  for (s::size_t k = 1; k < a.size(); ++k) {
    str spec = a[k];
    bool off = false;
    if (not spec.empty() and (spec[0] == '+' or spec[0] == '-')) {
      off = spec[0] == '-';
      spec.erase(0, 1);
    }
    if (spec.find('/') != str::npos) {
      auto pair = color_pair(spec);
      if (not pair) { rc = err("bad colour pair: " + spec); continue; }
      if (background) ::wbkgd(w, static_cast<chtype>(COLOR_PAIR(*pair)));
      else            ::wcolor_set(w, *pair, nullptr);
      continue;
    }
    auto it = s::find_if(s::begin(kAttrs), s::end(kAttrs),
                         [&](const Named& n) { return spec == n.name; });
    if (it == s::end(kAttrs)) { rc = err("attribute `" + spec + "' not known"); continue; }
    if (background) {
      chtype cur = ::getbkgd(w);
      ::wbkgdset(w, off ? (cur & ~static_cast<chtype>(it->value)) : (cur | static_cast<chtype>(it->value)));
    } else if (off) {
      ::wattroff(w, it->value);
    } else {
      ::wattron(w, it->value);
    }
  }
  return rc;
}

str key_name(int ch) {
  const char* n = ::keyname(ch);
  str s = n ? n : "";
  if (s.starts_with("KEY_")) s.erase(0, 4);
  return s;
}

int sub_input(ExecContext& ctx, const vec<str>& a) {
  WINDOW* w = window(a[0]);
  if (not w) return err("window `" + a[0] + "' does not exist");
  int ch = ::wgetch(w);
  if (ch == ERR) return 1;
  str chr, key;
  if (ch >= KEY_MIN) key = key_name(ch);
  else               chr = str(1, static_cast<char>(ch));
  if (a.size() > 1) (void)ctx.params.set_scalar(a[1], chr);
  else if (not chr.empty()) s::printf("%s\n", chr.c_str());
  if (a.size() > 2) (void)ctx.params.set_scalar(a[2], key);
  if (a.size() > 3) (void)ctx.params.set_scalar(a[3], "");
  return 0;
}

int builtin_zcurses(vec<str>& args, ExecContext& ctx) {
  if (args.size() < 2) return err("subcommand required");
  const str& sub = args[1];
  vec<str> a(args.begin() + 2, args.end());
  if (sub == "init") return sub_init(ctx);
  if (not cs().screen) return err(sub + ": not initialised");
  if (sub == "end") return sub_end(ctx);
  if (sub == "addwin") {
    if (int rc = need(a, 5, sub)) return rc;
    if (window(a[0])) return err("window name `" + a[0] + "' already exists");
    WINDOW* w;
    if (a.size() > 5) {
      WINDOW* parent = window(a[5]);
      if (not parent) return err("parent window `" + a[5] + "' does not exist");
      w = ::derwin(parent, num(a[1]), num(a[2]), num(a[3]), num(a[4]));
    } else {
      w = ::newwin(num(a[1]), num(a[2]), num(a[3]), num(a[4]));
    }
    if (not w) return err("failed to create window `" + a[0] + "'");
    cs().windows[a[0]] = w;
    set_params(ctx);
    return 0;
  }
  if (sub == "resize") {
    if (int rc = need(a, 2, sub)) return rc;
    if (a.size() > 2 and a[2].starts_with("endwin")) ::endwin();
    return ::resizeterm(num(a[0]), num(a[1])) == OK ? 0 : 1;
  }
  if (sub == "refresh") {
    if (a.empty()) a.emplace_back("stdscr");
    for (const auto& n : a) {
      WINDOW* w = window(n);
      if (not w) return err("window `" + n + "' does not exist");
      ::wnoutrefresh(w);
    }
    ::doupdate();
    s::fflush(stdout);
    return 0;
  }
  if (sub == "mouse")   return 0;   // accepted, as on terminals without a mouse
  if (int rc = need(a, 1, sub)) return rc;
  WINDOW* w = window(a[0]);
  if (not w) return err("window `" + a[0] + "' does not exist");
  if (sub == "delwin") {
    if (a[0] == "stdscr") return err("can't delete stdscr");
    ::delwin(w);
    cs().windows.erase(a[0]);
    set_params(ctx);
    return 0;
  }
  if (sub == "touch") {
    for (const auto& n : a)
      if (WINDOW* t = window(n)) ::touchwin(t);
    return 0;
  }
  if (sub == "move") {
    if (int rc = need(a, 3, sub)) return rc;
    return ::wmove(w, num(a[1]), num(a[2])) == OK ? 0 : 1;
  }
  if (sub == "clear") {
    str how = a.size() > 1 ? a[1] : "";
    int rc = how == "redraw" ? ::wclear(w) : how == "eol" ? ::wclrtoeol(w)
           : how == "bot" ? ::wclrtobot(w) : ::werase(w);
    return rc == OK ? 0 : 1;
  }
  if (sub == "position") {
    if (int rc = need(a, 2, sub)) return rc;
    vec<str> v = {
      s::to_string(getcury(w)), s::to_string(getcurx(w)),
      s::to_string(getbegy(w)), s::to_string(getbegx(w)),
      s::to_string(getmaxy(w)), s::to_string(getmaxx(w)),
    };
    (void)ctx.params.set_array(a[1], v);
    return 0;
  }
  if (sub == "char") {
    if (int rc = need(a, 2, sub)) return rc;
    return ::waddnstr(w, a[1].c_str(), static_cast<int>(a[1].size())) == OK ? 0 : 1;
  }
  if (sub == "string") {
    if (int rc = need(a, 2, sub)) return rc;
    return ::waddstr(w, a[1].c_str()) == OK ? 0 : 1;
  }
  if (sub == "border") return ::box(w, 0, 0) == OK ? 0 : 1;
  if (sub == "attr")   return sub_attr(a, false);
  if (sub == "bg")     return sub_attr(a, true);
  if (sub == "scroll") {
    if (int rc = need(a, 2, sub)) return rc;
    if (a[1] == "on")  return ::scrollok(w, TRUE) == OK ? 0 : 1;
    if (a[1] == "off") return ::scrollok(w, FALSE) == OK ? 0 : 1;
    return ::wscrl(w, num(a[1])) == OK ? 0 : 1;
  }
  if (sub == "timeout") {
    if (int rc = need(a, 2, sub)) return rc;
    ::wtimeout(w, num(a[1]));
    return 0;
  }
  if (sub == "input") return sub_input(ctx, a);
  if (sub == "querychar") {
    chtype c = ::winch(w);
    vec<str> v = {str(1, static_cast<char>(c & A_CHARTEXT))};
    short pair = static_cast<short>(PAIR_NUMBER(static_cast<int>(c)));
    str colors = "default/default";
    for (const auto& [spec, n] : cs().pairs)
      if (n == pair) colors = spec;
    v.push_back(colors);
    for (const auto& at : kAttrs)
      if (c & static_cast<chtype>(at.value)) v.emplace_back(at.name);
    if (a.size() > 1) (void)ctx.params.set_array(a[1], v);
    else for (const auto& e : v) s::printf("%s\n", e.c_str());
    return 0;
  }
  return err("unknown subcommand: " + sub);
}

#else  // no curses

int builtin_zcurses(vec<str>&, ExecContext&) {
  return err("curses is not available (built without curses)");
}

#endif

}  // namespace

void register_curses_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/curses", [](ModuleFeatures& out, ExecContext& ctx) {
    out.builtins.push_back(BuiltinDef{"zcurses", builtin_zcurses, BINF_NONE, 0});
#ifdef JFSH_HAVE_CURSES
    ExecContext* ctxp = &ctx;
    vec<str> attrs, colors;
    for (const auto& at : kAttrs)  attrs.emplace_back(at.name);
    for (const auto& c  : kColors) colors.emplace_back(c.name);
    ctx.params.assign("zcurses_attrs",  attrs,  PM_ARRAY | PM_SPECIAL | PM_READONLY);
    ctx.params.assign("zcurses_colors", colors, PM_ARRAY | PM_SPECIAL | PM_READONLY);
    set_params(ctx);
    out.on_unload = [ctxp] {
      if (cs().screen) (void)sub_end(*ctxp);
      for (const char* p : {"zcurses_attrs", "zcurses_colors", "zcurses_windows",
                            "ZCURSES_COLORS", "ZCURSES_COLOR_PAIRS"})
        drop_module_param(ctxp->params, p);
    };
#else
    (void)ctx;
#endif
  });
}

}  // namespace jfsh
