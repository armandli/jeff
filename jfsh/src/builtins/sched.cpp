// sched: run a command at a given time, before the next prompt (zsh's
// zsh/sched module).
//
//   sched                       list the events
//   sched -N                    remove event N
//   sched [-o] [+]hh:mm[:ss] cmd …
//   sched [-o] [+]seconds cmd …
//
// An absolute hh:mm is the next time the clock shows it; plain seconds are
// since the epoch.  A `+` time counts from now.  The interactive loop runs
// the events that are due before it prints a prompt (run_sched).

#include <cstdio>
#include <cstdlib>
#include <ctime>

#include <algorithm>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

namespace jfsh {

namespace {

bool all_digits(strv s) {
  return not s.empty() and s.find_first_not_of("0123456789") == strv::npos;
}

// Seconds in `hh:mm[:ss]` (at most two colons), or nothing.
opt<long> clock_seconds(strv s) {
  long parts[3] = {0, 0, 0};
  int  n = 0;
  while (true) {
    auto colon = s.find(':');
    strv field = s.substr(0, colon);
    if (not all_digits(field) or n == 3) return s::nullopt;
    parts[n++] = s::strtol(str(field).c_str(), nullptr, 10);
    if (colon == strv::npos) break;
    s = s.substr(colon + 1);
  }
  if (n < 2) return s::nullopt;
  return parts[0] * 3600 + parts[1] * 60 + parts[2];
}

// When `spec` names, from `now`.
opt<s::time_t> when_of(strv spec, s::time_t now) {
  bool relative = not spec.empty() and spec[0] == '+';
  if (relative) spec.remove_prefix(1);
  if (all_digits(spec)) {
    long n = s::strtol(str(spec).c_str(), nullptr, 10);
    return relative ? now + n : static_cast<s::time_t>(n);
  }
  auto secs = clock_seconds(spec);
  if (not secs) return s::nullopt;
  if (relative) return now + *secs;
  struct tm tm{};
  ::localtime_r(&now, &tm);
  tm.tm_hour = tm.tm_min = tm.tm_sec = 0;
  tm.tm_isdst = -1;
  s::time_t at = s::mktime(&tm) + *secs;
  if (at < now) at += 24 * 60 * 60;   // already past today: tomorrow
  return at;
}

}  // namespace

int builtin_sched(vec<str>& args, ExecContext& ctx) {
  auto& events = ctx.loop.sched;
  s::time_t now = s::time(nullptr);
  if (args.size() == 1) {
    int n = 0;
    for (const auto& e : events) {
      struct tm tm{};
      ::localtime_r(&e.when, &tm);
      // zsh's "%a %b %e %k:%M:%S"; %k (a blank-padded hour) is a GNU extension.
      char day[32], when[64];
      s::strftime(day, sizeof day, "%a %b %e", &tm);
      s::snprintf(when, sizeof when, "%s %2d:%02d:%02d", day, tm.tm_hour, tm.tm_min, tm.tm_sec);
      s::printf("%3d %s %s%s%s\n", ++n, when, e.trash_zle ? "-o " : "",
                not e.cmd.empty() and e.cmd[0] == '-' ? "-- " : "", e.cmd.c_str());
    }
    return 0;
  }
  s::size_t i = 1;
  if (args[i].size() > 1 and args[i][0] == '-' and all_digits(strv(args[i]).substr(1))) {
    long n = s::strtol(args[i].c_str() + 1, nullptr, 10);
    if (n < 1 or static_cast<s::size_t>(n) > events.size())
      return builtin_error(args[0], "not that many entries");
    events.erase(events.begin() + (n - 1));
    return 0;
  }
  bool trash = false;
  for (; i < args.size() and args[i].size() > 1 and args[i][0] == '-'; ++i) {
    if (args[i] == "--") { ++i; break; }
    if (args[i] == "-o") { trash = true; continue; }
    return builtin_error(args[0], "bad option: " + args[i]);
  }
  if (i >= args.size()) return builtin_error(args[0], "not enough arguments");
  auto when = when_of(args[i], now);
  if (not when) return builtin_error(args[0], "bad time specifier");
  if (++i < args.size() and args[i] == "--") ++i;
  if (i >= args.size()) return builtin_error(args[0], "not enough arguments");
  str cmd;
  for (; i < args.size(); ++i) cmd += (cmd.empty() ? "" : " ") + args[i];
  SchedEvent ev{*when, s::move(cmd), trash};
  auto at = s::upper_bound(events.begin(), events.end(), ev.when,
                           [](s::time_t t, const SchedEvent& e) { return t < e.when; });
  events.insert(at, s::move(ev));
  return 0;
}

}  // namespace jfsh
