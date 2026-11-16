// Resource builtins: ulimit, umask and times.

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/time.h>

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

const Limit* find_limit(char c) {
  for (const auto& l : LIMITS)
    if (l.letter == c) return &l;
  return nullptr;
}

str limit_text(rlim_t v, s::uint64_t unit) {
  if (v == RLIM_INFINITY) return "unlimited";
  return s::to_string(static_cast<s::uint64_t>(v) / unit);
}

}  // namespace

// ulimit [-HSa] [-tfdscmunlvxiqerN [limit]] …: shows or sets resource
// limits, soft ones unless -H (-HS: both).  With no resource, -f.
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
    if (a.size() > 1 and a[0] == '-') {
      for (s::size_t k = 1; k < a.size(); ++k) {
        char c = a[k];
        if (c == 'H') { hard = true; continue; }
        if (c == 'S') { soft = true; continue; }
        if (c == 'a') { all = true; continue; }
        const Limit* l = find_limit(c);
        if (c == 'N') {
          // -N takes the resource number; only RLIMIT_RTTIME (15) is named.
          if (i + 1 < args.size()) ++i;
        }
        if (not l) {
          s::fprintf(stderr, "jfsh: ulimit: bad option: -%c\n", c);
          return 1;
        }
        reqs.push_back({l, s::nullopt});
      }
      continue;
    }
    if (reqs.empty()) reqs.push_back({find_limit('f'), s::nullopt});
    reqs.back().value = a;
  }
  if (all) {
    for (const auto& l : LIMITS) {
      struct rlimit rl{};
      if (::getrlimit(l.resource, &rl) != 0) continue;
      s::printf("%-36s%s\n", l.label, limit_text(hard ? rl.rlim_max : rl.rlim_cur, l.unit).c_str());
    }
    return 0;
  }
  if (reqs.empty()) reqs.push_back({find_limit('f'), s::nullopt});
  bool labelled = reqs.size() > 1;
  for (const auto& r : reqs) {
    struct rlimit rl{};
    if (::getrlimit(r.limit->resource, &rl) != 0)
      return builtin_error(cmd, s::strerror(errno));
    if (not r.value) {
      str v = limit_text(hard ? rl.rlim_max : rl.rlim_cur, r.limit->unit);
      if (labelled) s::printf("%-36s%s\n", r.limit->label, v.c_str());
      else          s::printf("%s\n", v.c_str());
      continue;
    }
    rlim_t v;
    if (*r.value == "unlimited") {
      v = RLIM_INFINITY;
    } else if (*r.value == "hard") {
      v = rl.rlim_max;
    } else if (*r.value == "soft") {
      v = rl.rlim_cur;
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
        and (v == RLIM_INFINITY or v > rl.rlim_max))
      return builtin_error(cmd, "value exceeds hard limit");
    if (set_hard) rl.rlim_max = v;
    if (set_soft) rl.rlim_cur = v;
    // A hard limit below the soft one takes the soft one down with it.
    if (set_hard and not set_soft and v != RLIM_INFINITY
        and (rl.rlim_cur == RLIM_INFINITY or rl.rlim_cur > v))
      rl.rlim_cur = v;
    if (::setrlimit(r.limit->resource, &rl) != 0) {
      if (errno == EPERM) return builtin_error(cmd, "can't raise hard limits");
      return builtin_error(cmd, s::strerror(errno));
    }
  }
  return 0;
}

namespace {

str symbolic_mask(mode_t mask) {
  mode_t allowed = ~mask & 0777;
  str out;
  const char who[] = {'u', 'g', 'o'};
  for (int w = 0; w < 3; ++w) {
    int shift = 6 - 3 * w;
    if (w) out += ',';
    out += who[w];
    out += '=';
    if (allowed & (4u << shift)) out += 'r';
    if (allowed & (2u << shift)) out += 'w';
    if (allowed & (1u << shift)) out += 'x';
  }
  return out;
}

// `u=rwx,g+w,o-x`: applied to the permissions the mask leaves.
opt<mode_t> parse_symbolic(strv spec, mode_t mask) {
  mode_t allowed = ~mask & 0777;
  s::size_t i = 0;
  while (i <= spec.size()) {
    s::size_t end = spec.find(',', i);
    if (end == strv::npos) end = spec.size();
    strv clause = spec.substr(i, end - i);
    i = end + 1;
    if (clause.empty()) return s::nullopt;
    mode_t who = 0;
    s::size_t k = 0;
    for (; k < clause.size() and strv("ugoa").find(clause[k]) != strv::npos; ++k) {
      switch (clause[k]) {
        case 'u': who |= 0700; break;
        case 'g': who |= 0070; break;
        case 'o': who |= 0007; break;
        default:  who |= 0777;
      }
    }
    if (who == 0) who = 0777;
    if (k >= clause.size()) return s::nullopt;
    while (k < clause.size()) {
      char op = clause[k++];
      if (op != '+' and op != '-' and op != '=') return s::nullopt;
      mode_t perms = 0;
      for (; k < clause.size() and strv("rwx").find(clause[k]) != strv::npos; ++k)
        perms |= clause[k] == 'r' ? 0444 : clause[k] == 'w' ? 0222 : 0111;
      perms &= who;
      if (op == '+') allowed |= perms;
      else if (op == '-') allowed &= ~perms;
      else allowed = (allowed & ~who) | perms;
    }
    if (end == spec.size()) break;
  }
  return static_cast<mode_t>(~allowed & 0777);
}

}  // namespace

// umask [-S] [mask]: an octal mask or a symbolic one; -S shows it
// symbolically.
int builtin_umask(vec<str>& args, ExecContext& ctx) {
  (void)ctx;
  strv cmd = args[0];
  bool symbolic = false;
  s::size_t i = 1;
  if (i < args.size() and args[i] == "-S") { symbolic = true; ++i; }
  mode_t mask = ::umask(0);
  ::umask(mask);
  if (i >= args.size()) {
    if (symbolic) s::printf("%s\n", symbolic_mask(mask).c_str());
    else          s::printf("%03o\n", static_cast<unsigned>(mask));
    return 0;
  }
  const str& spec = args[i];
  opt<mode_t> next;
  if (not spec.empty() and s::isdigit(static_cast<unsigned char>(spec[0]))) {
    if (spec.find_first_not_of("01234567") == str::npos and spec.size() <= 4) {
      unsigned long v = s::strtoul(spec.c_str(), nullptr, 8);
      if (v <= 0777) next = static_cast<mode_t>(v);
    }
  } else {
    next = parse_symbolic(spec, mask);
  }
  if (not next) return builtin_error(cmd, "bad umask");
  ::umask(*next);
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
  (void)args;
  (void)ctx;
  struct rusage self{}, children{};
  ::getrusage(RUSAGE_SELF, &self);
  ::getrusage(RUSAGE_CHILDREN, &children);
  s::printf("%s %s\n%s %s\n", minutes_seconds(self.ru_utime).c_str(),
            minutes_seconds(self.ru_stime).c_str(), minutes_seconds(children.ru_utime).c_str(),
            minutes_seconds(children.ru_stime).c_str());
  return 0;
}

}  // namespace jfsh
