// Resource builtins: ulimit, umask and times.

// ulimit and umask are derived from zsh 5.9's bin_ulimit
// (Src/Builtins/rlimits.c) and bin_umask (Src/builtin.c).  zsh's copyright
// and licence:
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

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>

#include <algorithm>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

namespace jfsh {

namespace {

struct Limit {
  char        letter;
  int         resource;
  s::uint64_t unit;
  const char* label;   // as `ulimit -a` prints it
};

// zsh's order and wording.
const Limit LIMITS[] = {
  {'t', RLIMIT_CPU,        1,    "-t: cpu time (seconds)"},
  {'f', RLIMIT_FSIZE,      512,  "-f: file size (blocks)"},
  {'d', RLIMIT_DATA,       1024, "-d: data seg size (kbytes)"},
  {'s', RLIMIT_STACK,      1024, "-s: stack size (kbytes)"},
  {'c', RLIMIT_CORE,       512,  "-c: core file size (blocks)"},
#ifdef RLIMIT_RSS
  {'m', RLIMIT_RSS,        1024, "-m: resident set size (kbytes)"},
#endif
#ifdef RLIMIT_NPROC
  {'u', RLIMIT_NPROC,      1,    "-u: processes"},
#endif
  {'n', RLIMIT_NOFILE,     1,    "-n: file descriptors"},
#ifdef RLIMIT_MEMLOCK
  {'l', RLIMIT_MEMLOCK,    1024, "-l: locked-in-memory size (kbytes)"},
#endif
  {'v', RLIMIT_AS,         1024, "-v: address space (kbytes)"},
#ifdef RLIMIT_LOCKS
  {'x', RLIMIT_LOCKS,      1,    "-x: file locks"},
#endif
#ifdef RLIMIT_SIGPENDING
  {'i', RLIMIT_SIGPENDING, 1,    "-i: pending signals"},
#endif
#ifdef RLIMIT_MSGQUEUE
  {'q', RLIMIT_MSGQUEUE,   1,    "-q: bytes in POSIX msg queues"},
#endif
#ifdef RLIMIT_NICE
  {'e', RLIMIT_NICE,       1,    "-e: max nice"},
#endif
#ifdef RLIMIT_RTPRIO
  {'r', RLIMIT_RTPRIO,     1,    "-r: max rt priority"},
#endif
#ifdef RLIMIT_RTTIME
  {'N', RLIMIT_RTTIME,     1,    "-N 15: rt cpu time (microseconds)"},
#endif
};

// The limits this system has, by resource number as zsh's `ulimit -a`
// lists them; one that is another's alias (RLIMIT_RSS is RLIMIT_AS on
// macOS) is left out, as zsh's RLIMIT_RSS_IS_AS.
vec<const Limit*> system_limits() {
  vec<const Limit*> out;
  for (const auto& l : LIMITS) {
#if defined(RLIMIT_RSS)
    if (l.letter == 'm' and RLIMIT_RSS == RLIMIT_AS) continue;
#endif
    out.push_back(&l);
  }
  s::stable_sort(out.begin(), out.end(),
                 [](const Limit* a, const Limit* b) { return a->resource < b->resource; });
  return out;
}

const Limit* find_limit(char c) {
  for (const Limit* l : system_limits())
    if (l->letter == c) return l;
  return nullptr;
}

const Limit* find_resource(int res) {
  for (const Limit* l : system_limits())
    if (l->resource == res) return l;
  return nullptr;
}

str limit_text(rlim_t v, s::uint64_t unit) {
  if (v == RLIM_INFINITY) return "unlimited";
  return s::to_string(static_cast<s::uint64_t>(v) / unit);
}

}  // namespace

// ulimit [-HSa] [-tfdscmunlvxiqer [limit]] [-N n [limit]] …: shows or sets
// resource limits, soft ones unless -H (-HS: both).  With no resource,
// -f.  As zsh's bin_ulimit.
int builtin_ulimit(vec<str>& args, ExecContext& ctx) {
  (void)ctx;
  strv cmd = args[0];
  bool hard = false, soft = false, all = false;
  struct Request {
    const Limit* limit;
    opt<str>     value;
  };
  vec<Request> reqs;
  for (s::size_t i = 1; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "-") return builtin_error(cmd, "missing option letter");
    if (a.size() > 1 and a[0] == '-') {
      for (s::size_t k = 1; k < a.size(); ++k) {
        char c = a[k];
        if (c == 'H') { hard = true; continue; }
        if (c == 'S') { soft = true; continue; }
        if (c == 'a') {
          if (not reqs.empty()) return builtin_error(cmd, "no limits allowed with -a");
          all = true;
          continue;
        }
        const Limit* l = nullptr;
        if (c == 'N') {
          // -N takes the resource's number, attached or in the next word.
          str number = a.substr(k + 1);
          if (number.empty()) {
            if (i + 1 >= args.size()) return builtin_error(cmd, "number required after -N");
            number = args[++i];
          }
          char* end = nullptr;
          long n = s::strtol(number.c_str(), &end, 10);
          if (number.empty() or *end != '\0')
            return builtin_error(cmd, "invalid number: " + number);
          l = find_resource(static_cast<int>(n));
          if (not l) return builtin_error(cmd, "bad option: -N " + number);
          k = a.size();
        } else if (not (l = find_limit(c))) {
          s::fprintf(stderr, "jfsh: ulimit: bad option: -%c\n", c);
          return 1;
        }
        if (all) return builtin_error(cmd, "no limits allowed with -a");
        reqs.push_back({l, s::nullopt});
      }
      continue;
    }
    if (all) return builtin_error(cmd, "no arguments allowed after -a");
    if (reqs.empty() or reqs.back().value) reqs.push_back({find_limit('f'), s::nullopt});
    reqs.back().value = a;
  }
  if (all) {
    for (const Limit* l : system_limits()) {
      struct rlimit rl{};
      if (::getrlimit(l->resource, &rl) != 0) continue;
      s::printf("%-36s%s\n", l->label,
                limit_text(hard ? rl.rlim_max : rl.rlim_cur, l->unit).c_str());
    }
    return 0;
  }
  if (reqs.empty()) reqs.push_back({find_limit('f'), s::nullopt});
  // The ones to show are shown once each, after the ones set, in
  // resource order (zsh's resmask); with a label when more were named.
  bool labelled = s::count_if(reqs.begin(), reqs.end(), [](const Request& r) {
                    return not r.value;
                  }) > 1;
  vec<const Limit*> shown;
  int rc = 0;
  for (const auto& r : reqs) {
    struct rlimit rl{};
    if (::getrlimit(r.limit->resource, &rl) != 0)
      return builtin_error(cmd, s::strerror(errno));
    if (not r.value) {
      if (s::find(shown.begin(), shown.end(), r.limit) == shown.end()) shown.push_back(r.limit);
      continue;
    }
    // "unlimited", "hard" (the hard limit) or a number; zsh has no "soft".
    rlim_t v;
    if (*r.value == "unlimited") {
      v = RLIM_INFINITY;
    } else if (*r.value == "hard") {
      v = rl.rlim_max;
    } else {
      char* end = nullptr;
      errno = 0;
      unsigned long long n = s::strtoull(r.value->c_str(), &end, 10);
      if (r.value->empty() or *end != '\0' or errno)
        return builtin_error(cmd, "invalid number: " + *r.value);
      v = static_cast<rlim_t>(n * r.limit->unit);
    }
    bool set_hard = hard;
    bool set_soft = soft or not hard;
    if (set_soft and not set_hard and rl.rlim_max != RLIM_INFINITY
        and (v == RLIM_INFINITY or v > rl.rlim_max)) {
      builtin_error(cmd, "value exceeds hard limit");
      rc = 1;
      continue;
    }
    if (set_hard) rl.rlim_max = v;
    if (set_soft) rl.rlim_cur = v;
    // A hard limit below the soft one takes the soft one down with it.
    if (set_hard and not set_soft and v != RLIM_INFINITY
        and (rl.rlim_cur == RLIM_INFINITY or rl.rlim_cur > v))
      rl.rlim_cur = v;
    if (::setrlimit(r.limit->resource, &rl) != 0) {
      builtin_error(cmd, errno == EPERM ? str("can't raise hard limits") : str(s::strerror(errno)));
      rc = 1;
    }
  }
  s::sort(shown.begin(), shown.end(),
          [](const Limit* a, const Limit* b) { return a->resource < b->resource; });
  for (const Limit* l : shown) {
    struct rlimit rl{};
    if (::getrlimit(l->resource, &rl) != 0) return builtin_error(cmd, s::strerror(errno));
    str v = limit_text(hard ? rl.rlim_max : rl.rlim_cur, l->unit);
    if (labelled) s::printf("%-36s%s\n", l->label, v.c_str());
    else          s::printf("%s\n", v.c_str());
  }
  return rc;
}

// umask [-S] [mask]: an octal mask or a symbolic one; -S shows it
// symbolically.  As zsh's bin_umask.
int builtin_umask(vec<str>& args, ExecContext& ctx) {
  (void)ctx;
  strv cmd = args[0];
  OptParse o;
  if (not parse_options(args, "S", "", o)) return 1;
  if (args.size() > o.next + 1) return builtin_error(cmd, "too many arguments");
  mode_t um = ::umask(0777);
  ::umask(um);
  if (o.next >= args.size()) {
    if (o.has('S')) {
      str out;
      for (const char* who = "ugo"; *who; ++who) {
        out += *who;
        out += '=';
        for (const char* what = "rwx"; *what; ++what) {
          if (not (um & 0400)) out += *what;
          um = static_cast<mode_t>(um << 1);
        }
        out += who[1] ? ',' : '\n';
      }
      s::fputs(out.c_str(), stdout);
    } else {
      s::printf("%s%03o\n", (um & 0700) ? "0" : "", static_cast<unsigned>(um));
    }
    return 0;
  }
  const str& spec = args[o.next];
  if (not spec.empty() and s::isdigit(static_cast<unsigned char>(spec[0]))) {
    // An octal number (zstrtol: as many octal digits as there are).
    char* end = nullptr;
    unsigned long v = s::strtoul(spec.c_str(), &end, 8);
    if (*end) return builtin_error(cmd, "bad umask");
    um = static_cast<mode_t>(v);
  } else {
    // u, g, o or a (all by default), an operator, and rwx; clauses
    // separated by commas.
    s::size_t i = 0;
    for (;;) {
      mode_t who = 0;
      for (; i < spec.size() and strv("ugoa").find(spec[i]) != strv::npos; ++i)
        who |= spec[i] == 'u' ? 0700 : spec[i] == 'g' ? 0070 : spec[i] == 'o' ? 0007 : 0777;
      if (not who) who = 0777;
      char op = i < spec.size() ? spec[i] : '\0';
      if (op != '+' and op != '-' and op != '=') {
        if (op) return builtin_error(cmd, str("bad symbolic mode operator: ") + op);
        return builtin_error(cmd, "bad umask");
      }
      mode_t mask = 0;
      for (++i; i < spec.size() and spec[i] != ','; ++i) {
        char c = spec[i];
        if      (c == 'r') mask |= 0444 & who;
        else if (c == 'w') mask |= 0222 & who;
        else if (c == 'x') mask |= 0111 & who;
        else return builtin_error(cmd, str("bad symbolic mode permission: ") + c);
      }
      if      (op == '+') um &= static_cast<mode_t>(~mask);
      else if (op == '-') um |= mask;
      else                um = static_cast<mode_t>((um | who) & ~mask);
      if (i < spec.size() and spec[i] == ',') ++i;
      else break;
    }
  }
  ::umask(um);
  return 0;
}

namespace {

str minutes_seconds(const struct timeval& tv) {
  double secs = static_cast<double>(tv.tv_sec) + static_cast<double>(tv.tv_usec) / 1e6;
  long m = static_cast<long>(secs / 60);
  char buf[64];
  s::snprintf(buf, sizeof buf, "%ldm%.2fs", m, secs - static_cast<double>(m) * 60);
  return buf;
}

}  // namespace

// times: the user and system time of the shell, then of its children.
int builtin_times(vec<str>& args, ExecContext& ctx) {
  (void)ctx;
  if (args.size() > 1) return builtin_error(args[0], "too many arguments");
  struct rusage self{}, children{};
  ::getrusage(RUSAGE_SELF, &self);
  ::getrusage(RUSAGE_CHILDREN, &children);
  s::printf("%s %s\n%s %s\n", minutes_seconds(self.ru_utime).c_str(),
            minutes_seconds(self.ru_stime).c_str(), minutes_seconds(children.ru_utime).c_str(),
            minutes_seconds(children.ru_stime).c_str());
  return 0;
}

}  // namespace jfsh
