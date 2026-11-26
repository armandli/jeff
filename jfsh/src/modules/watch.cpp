#include <jfsh/modules/watch.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>
#include <utmpx.h>

#include <algorithm>
#include <tuple>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/glob/glob.h>
#include <jfsh/modules/support.h>

// zsh/watch (M11): login/logout reports and `log`, as zsh's
// Src/Modules/watch.c, from the utmpx database.  The check runs from
// ExecContext::hooks.before_prompt, next to periodic.

namespace jfsh {

namespace {

constexpr const char* kDefaultFmt = "%n has %a %l from %m.";

struct Login {
  str         user;
  str         line;
  str         host;
  s::time_t   when = 0;

  auto key() const { return s::tie(user, line, host, when); }
  bool operator<(const Login& o) const { return key() < o.key(); }
  bool operator==(const Login& o) const { return key() == o.key(); }
};

struct WatchState {
  vec<Login> last;           // sorted, from the previous check
  bool       primed = false; // the first check only records
  s::time_t  checked = 0;
};

WatchState& ws() {
  static WatchState st;
  return st;
}

vec<Login> current_logins() {
  vec<Login> v;
  ::setutxent();
  while (struct utmpx* u = ::getutxent()) {
    if (u->ut_type != USER_PROCESS) continue;
    Login l;
    l.user = str(u->ut_user, ::strnlen(u->ut_user, sizeof u->ut_user));
    l.line = str(u->ut_line, ::strnlen(u->ut_line, sizeof u->ut_line));
    l.host = str(u->ut_host, ::strnlen(u->ut_host, sizeof u->ut_host));
    l.when = u->ut_tv.tv_sec;
    v.push_back(s::move(l));
  }
  ::endutxent();
  s::sort(v.begin(), v.end());
  return v;
}

str my_name() {
  const char* n = ::getlogin();
  if (n) return n;
  const char* u = s::getenv("USER");
  return u ? u : "";
}

// Whether `l` is one of the logins $watch asks about: `all`, `notme`, or
// user@host%line with each part a pattern and any part left out.
bool watched(const Login& l, const vec<str>& watch) {
  for (const auto& w : watch) {
    if (w == "all") return true;
    if (w == "notme") {
      if (l.user != my_name()) return true;
      continue;
    }
    str user = w, host, line;
    if (auto pct = user.find('%'); pct != str::npos) {
      line = user.substr(pct + 1);
      user.resize(pct);
    }
    if (auto at = user.find('@'); at != str::npos) {
      host = user.substr(at + 1);
      user.resize(at);
    }
    auto match = [](const str& pat, const str& text) {
      return pat.empty() or CompiledPattern::compile(pat).matches(text);
    };
    if (match(user, l.user) and match(host, l.host) and match(line, l.line)) return true;
  }
  return false;
}

// $WATCHFMT for one login (`in`) or logout: %n %a %l %M %m %t/%@ %T %w
// %W %D %% and %(x.true.false) for x in a l n m M.
str format(strv fmt, const Login& l, bool in) {
  str out;
  struct tm tm{};
  s::time_t when = in ? l.when : s::time(nullptr);
  ::localtime_r(&when, &tm);
  auto strf = [&](const char* f) {
    char buf[64];
    s::strftime(buf, sizeof buf, f, &tm);
    return str(buf);
  };
  str short_host = l.host.substr(0, l.host.find('.'));
  auto value = [&](char c) -> opt<str> {
    switch (c) {
      case 'n': return l.user;
      case 'a': return str(in ? "logged on" : "logged off");
      case 'l': return l.line.starts_with("tty") ? l.line.substr(3) : l.line;
      case 'M': return l.host;
      case 'm': return short_host;
      case 't': case '@': return strf("%l:%M%p");
      case 'T': return strf("%H:%M");
      case 'w': return strf("%a %e");
      case 'W': return strf("%m/%d/%y");
      case 'D': return strf("%y-%m-%d");
      case '%': return str("%");
      default:  return s::nullopt;
    }
  };
  auto truth = [&](char c) {
    switch (c) {
      case 'a': return in;
      case 'l': return not l.line.empty();
      case 'n': return not l.user.empty();
      case 'm': case 'M': return not l.host.empty();
      default:  return false;
    }
  };
  for (s::size_t i = 0; i < fmt.size(); ++i) {
    if (fmt[i] != '%' or i + 1 >= fmt.size()) { out += fmt[i]; continue; }
    char c = fmt[++i];
    if (c == '(' and i + 2 < fmt.size()) {
      // %(x.true.false): any character after x separates the branches.
      char cond = fmt[i + 1];
      char sep  = fmt[i + 2];
      s::size_t a = i + 3;
      s::size_t b = fmt.find(sep, a);
      s::size_t e = b == strv::npos ? strv::npos : fmt.find(')', b + 1);
      if (e == strv::npos) { out += "%("; continue; }
      strv branch = truth(cond) ? fmt.substr(a, b - a) : fmt.substr(b + 1, e - b - 1);
      out += format(branch, l, in);
      i = e;
      continue;
    }
    if (auto v = value(c)) out += *v;
    else { out += '%'; out += c; }
  }
  return out;
}

vec<str> watch_list(ExecContext& ctx) {
  auto p = ctx.params.lookup("watch");
  if (not p or not p->get().is_set()) return {};
  return p->get().is_array() ? p->get().array() : vec<str>{p->get().scalar()};
}

void report(ExecContext& ctx, const vec<Login>& logins, bool in) {
  str fmt = param_or(ctx.params, "WATCHFMT", kDefaultFmt);
  auto watch = watch_list(ctx);
  for (const auto& l : logins)
    if (watched(l, watch)) s::printf("%s\n", format(fmt, l, in).c_str());
  s::fflush(stdout);
}

// Before a prompt: what changed since the last check, at most every
// $LOGCHECK seconds (default 60).
void check(ExecContext& ctx) {
  if (watch_list(ctx).empty()) return;
  auto& st = ws();
  s::time_t now = s::time(nullptr);
  long every = s::strtol(param_or(ctx.params, "LOGCHECK", "60").c_str(), nullptr, 10);
  if (st.primed and now - st.checked < every) return;
  st.checked = now;
  vec<Login> cur = current_logins();
  if (st.primed) {
    vec<Login> gone, came;
    s::set_difference(st.last.begin(), st.last.end(), cur.begin(), cur.end(), s::back_inserter(gone));
    s::set_difference(cur.begin(), cur.end(), st.last.begin(), st.last.end(), s::back_inserter(came));
    report(ctx, gone, false);
    report(ctx, came, true);
  }
  st.last = s::move(cur);
  st.primed = true;
}

int builtin_log(vec<str>&, ExecContext& ctx) {
  if (watch_list(ctx).empty()) return 1;
  report(ctx, current_logins(), true);
  return 0;
}

}  // namespace

void register_watch_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/watch", [](ModuleFeatures& out, ExecContext& ctx) {
    ExecContext* ctxp = &ctx;
    ws() = WatchState{};
    if (not ctx.params.lookup("WATCHFMT")) (void)ctx.params.set_scalar("WATCHFMT", kDefaultFmt);
    if (not ctx.params.lookup("LOGCHECK")) ctx.params.assign("LOGCHECK", s::int64_t{60}, PM_INTEGER);
    ctx.hooks.before_prompt.emplace_back("zsh/watch", [ctxp] { check(*ctxp); });
    out.builtins.push_back(BuiltinDef{"log", builtin_log, BINF_NONE, 0, 0});
    out.on_unload = [ctxp] { ctxp->hooks.remove("zsh/watch"); };
  });
}

}  // namespace jfsh
