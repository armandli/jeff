// bin_sched and its time parsing in this file are derived from zsh 5.9's
// Src/Builtins/sched.c, and the number reading from zstrtol in Src/utils.c.
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

// sched: run a command at a given time, before the next prompt (zsh's
// zsh/sched module).
//
//   sched                       list the events
//   sched -N                    remove event N
//   sched [-o] [+]hh:mm[:ss] cmd …
//   sched [-o] [+]seconds cmd …
//
// An absolute hh:mm (am/pm after it adds 12 hours for p) is the next time
// the clock shows it; plain seconds are since the epoch.  A `+` time counts
// from now.  The interactive loop runs the events that are due before it
// prints a prompt (run_sched).

#include <cctype>
#include <cstdio>
#include <ctime>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

namespace jfsh {

namespace {

// What zsh's zstrtol reads in base 10: blanks, a sign, then digits; `rest`
// is what follows them.
struct Number {
  long long value;
  strv      rest;
};

Number number(strv p) {
  while (not p.empty() and (p[0] == ' ' or p[0] == '\t')) p.remove_prefix(1);
  bool neg = not p.empty() and p[0] == '-';
  if (not p.empty() and (p[0] == '-' or p[0] == '+')) p.remove_prefix(1);
  unsigned long long n = 0;
  while (not p.empty() and p[0] >= '0' and p[0] <= '9') {
    n = n * 10 + static_cast<unsigned long long>(p[0] - '0');
    p.remove_prefix(1);
  }
  return {static_cast<long long>(neg ? 0 - n : n), p};
}

// now + h hours + m minutes + sec seconds, wrapping as zsh's C arithmetic
// does rather than overflowing.
s::time_t offset(s::time_t now, long long h, long long m, long long sec) {
  using u = unsigned long long;
  return static_cast<s::time_t>(static_cast<u>(now) + static_cast<u>(h) * 3600
                                + static_cast<u>(m) * 60 + static_cast<u>(sec));
}

// When `spec` (bin_sched's time argument) names, from `now`; nothing for a
// bad time specifier.
opt<s::time_t> when_of(strv spec, s::time_t now) {
  bool relative = spec.starts_with('+');
  auto [h, p] = number(relative ? spec.substr(1) : spec);
  long long m = 0, sec = 0;
  bool clock = p.starts_with(':');
  if (clock) {
    // [+]hh:mm[:ss]
    auto mins = number(p.substr(1));
    m = mins.value;
    p = mins.rest;
    if (p.starts_with(':')) {
      auto secs = number(p.substr(1));
      sec = secs.value;
      p = secs.rest;
    }
  }
  if (relative) {
    // +secs, or +hh:mm[:ss] from now.
    if (not p.empty()) return s::nullopt;
    return clock ? offset(now, h, m, sec) : offset(now, 0, 0, h);
  }
  if (not clock) {
    if (not p.empty()) return s::nullopt;
    return static_cast<s::time_t>(h);   // seconds since the epoch
  }
  // hh:mm[:ss], with am/pm after: the next time the clock shows it.
  char ampm = p.empty() ? '\0' : p[0];
  if (ampm and ampm != 'a' and ampm != 'A' and ampm != 'p' and ampm != 'P') return s::nullopt;
  struct tm tm{};
  ::localtime_r(&now, &tm);
  s::time_t t = now - (tm.tm_sec + tm.tm_min * 60 + tm.tm_hour * 3600);
  if (ampm == 'p' or ampm == 'P') h += 12;
  t = offset(t, h, m, sec);
  if (t < now) t += 24 * 3600;
  return t;
}

}  // namespace

// As zsh's bin_sched.
int builtin_sched(vec<str>& args, ExecContext& ctx) {
  auto& events = ctx.loop.sched;
  bool trash = false;
  s::size_t i = 1;
  // Options: -N removes event N at once, -o clears the line first, `--`
  // ends them.
  for (; i < args.size() and args[i].starts_with('-'); ++i) {
    strv arg = strv(args[i]).substr(1);
    if (not arg.empty() and s::isdigit(static_cast<unsigned char>(arg[0]))) {
      long long n = number(arg).value;
      if (n == 0) return builtin_error("sched", "usage for delete: sched -<item#>.");
      if (n > static_cast<long long>(events.size()))
        return builtin_error("sched", "not that many entries");
      events.erase(events.begin() + static_cast<s::ptrdiff_t>(n - 1));
      return 0;
    }
    if (arg.starts_with('-')) {
      ++i;
      break;
    }
    if (arg == "o") {
      trash = true;
      continue;
    }
    if (arg.empty()) return builtin_error(args[0], "option expected");
    return builtin_error(args[0], str("bad option: -") + arg[0]);
  }

  // No arguments: the list, as "%3d %a %b %e %k:%M:%S [-o ][-- ]cmd".
  if (i >= args.size()) {
    int n = 0;
    for (const auto& e : events) {
      struct tm tm{};
      ::localtime_r(&e.when, &tm);
      // %k (a blank-padded hour) is a GNU extension; it is spelled out.
      char day[32];
      s::strftime(day, sizeof day, "%a %b %e", &tm);
      s::printf("%3d %s %2d:%02d:%02d %s%s%s\n", ++n, day, tm.tm_hour, tm.tm_min, tm.tm_sec,
                e.trash_zle ? "-o " : "", e.cmd.starts_with('-') ? "-- " : "", e.cmd.c_str());
    }
    return 0;
  }
  if (i + 1 >= args.size()) return builtin_error("sched", "not enough arguments");

  auto when = when_of(args[i++], s::time(nullptr));
  if (not when) return builtin_error("sched", "bad time specifier");
  str cmd;
  for (; i < args.size(); ++i) cmd += (cmd.empty() ? "" : " ") + args[i];
  // In time order: first if earlier than the first, else before the first
  // later one after it that isn't earlier (zsh's list walk).
  s::size_t at = 0;
  if (not events.empty() and *when >= events.front().when) {
    while (at + 1 < events.size() and events[at + 1].when < *when) ++at;
    ++at;
  }
  events.insert(events.begin() + static_cast<s::ptrdiff_t>(at),
                SchedEvent{*when, s::move(cmd), trash});
  return 0;
}

}  // namespace jfsh
