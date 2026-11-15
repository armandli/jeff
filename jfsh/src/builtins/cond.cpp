#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <regex.h>
#include <sys/stat.h>
#include <unistd.h>

#include <string>
#include <vector>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/cond.h>
#include <jfsh/exec/executor.h>

// The conditions of `[[ … ]]` and `test`.  The `[[ ]]` grammar is parsed by
// Parser::par_cond and evaluated by Executor::exec_cond, which call the
// primitives here; `test` / `[` is builtin_test at the end of this file.

namespace jfsh {

bool cond_is_unary(std::string_view op) noexcept {
  return op.size() == 2 and op[0] == '-'
     and std::string_view{"abcdefghknoprstuvwxzLOGSN"}.find(op[1]) != std::string_view::npos;
}

bool cond_is_numeric(std::string_view op) noexcept {
  return op == "-eq" or op == "-ne" or op == "-lt" or op == "-le" or op == "-gt" or op == "-ge";
}

bool cond_is_binary(std::string_view op) noexcept {
  return op == "=" or op == "==" or op == "!=" or op == "<" or op == ">"
      or op == "-nt" or op == "-ot" or op == "-ef" or cond_is_numeric(op);
}

int cond_compare(std::string_view op, int cmp) noexcept {
  bool r = op == "-eq" ? cmp == 0 : op == "-ne" ? cmp != 0
         : op == "-lt" ? cmp <  0 : op == "-le" ? cmp <= 0
         : op == "-gt" ? cmp >  0 : cmp >= 0;
  return r ? 0 : 1;
}

static int status(bool b) noexcept { return b ? 0 : 1; }

static void complain(std::string_view who, const std::string& msg) {
  std::fprintf(stderr, "jfsh: %.*s%s\n", static_cast<int>(who.size()), who.data(), msg.c_str());
}

int cond_unary(std::string_view op, const std::string& arg, ExecContext& ctx,
               std::string_view who) {
  char c = op.size() == 2 and op[0] == '-' ? op[1] : 0;
  switch (c) {
    case 'n': return status(not arg.empty());
    case 'z': return status(arg.empty());
    case 'o': {
      bool negated = false;
      const OptionInfo* info = OptionState::find(arg, &negated);
      if (not info) {
        complain(who, "no such option: " + arg);
        return 3;
      }
      return status(ctx.opts.get(info->index) != negated);
    }
    case 'v': {
      auto p = ctx.params.lookup(arg);
      return status(p and p->get().is_set());
    }
    case 't': {
      char* rest = nullptr;
      errno = 0;
      long fd = std::strtol(arg.c_str(), &rest, 10);
      if (arg.empty() or *rest or errno or fd < 0 or fd > 1 << 20) return 1;
      return status(::isatty(static_cast<int>(fd)));
    }
    case 'r': return status(::access(arg.c_str(), R_OK) == 0);
    case 'w': return status(::access(arg.c_str(), W_OK) == 0);
    case 'x': return status(::access(arg.c_str(), X_OK) == 0);
    case 'h': case 'L': {
      struct stat st;
      return status(::lstat(arg.c_str(), &st) == 0 and S_ISLNK(st.st_mode));
    }
    case 0:
      break;
    default: {
      if (not cond_is_unary(op)) break;
      struct stat st;
      if (::stat(arg.c_str(), &st) != 0) return 1;
      switch (c) {
        case 'a': case 'e': return 0;
        case 'b': return status(S_ISBLK(st.st_mode));
        case 'c': return status(S_ISCHR(st.st_mode));
        case 'd': return status(S_ISDIR(st.st_mode));
        case 'f': return status(S_ISREG(st.st_mode));
        case 'p': return status(S_ISFIFO(st.st_mode));
        case 'S': return status(S_ISSOCK(st.st_mode));
        case 'g': return status(st.st_mode & S_ISGID);
        case 'u': return status(st.st_mode & S_ISUID);
        case 'k': return status(st.st_mode & S_ISVTX);
        case 's': return status(st.st_size > 0);
        case 'O': return status(st.st_uid == ::geteuid());
        case 'G': return status(st.st_gid == ::getegid());
        case 'N': return status(st.st_atime <= st.st_mtime);
        default:  break;
      }
    }
  }
  complain(who, "unknown condition: " + std::string(op));
  return 2;
}

// Modification time in nanoseconds.
static long long mtime_ns(const struct stat& st) noexcept {
#ifdef __APPLE__
  return st.st_mtimespec.tv_sec * 1'000'000'000LL + st.st_mtimespec.tv_nsec;
#else
  return st.st_mtim.tv_sec * 1'000'000'000LL + st.st_mtim.tv_nsec;
#endif
}

int cond_file_binary(std::string_view op, const std::string& l, const std::string& r) {
  struct stat a, b;
  if (::stat(l.c_str(), &a) != 0 or ::stat(r.c_str(), &b) != 0) return 1;
  if (op == "-nt") return status(mtime_ns(a) > mtime_ns(b));
  if (op == "-ot") return status(mtime_ns(a) < mtime_ns(b));
  return status(a.st_dev == b.st_dev and a.st_ino == b.st_ino);
}

// Characters (UTF-8 sequences) in the first n bytes of s.
static long long char_count(const std::string& s, regoff_t n) noexcept {
  long long chars = 0;
  for (regoff_t i = 0; i < n; ++i)
    if ((static_cast<unsigned char>(s[static_cast<std::size_t>(i)]) & 0xC0) != 0x80) ++chars;
  return chars;
}

int cond_regex(const std::string& l, const std::string& re, ExecContext& ctx,
               std::string_view who) {
  regex_t rx;
  if (int e = ::regcomp(&rx, re.c_str(), REG_EXTENDED); e != 0) {
    char buf[256];
    ::regerror(e, &rx, buf, sizeof buf);
    complain(who, std::string("failed to compile regex: ") + buf);
    return 1;
  }
  std::vector<regmatch_t> m(rx.re_nsub + 1);
  int rc = ::regexec(&rx, l.c_str(), m.size(), m.data(), 0);
  ::regfree(&rx);
  if (rc != 0) return 1;
  auto set_int = [&](const char* name, long long v) {
    (void)ctx.params.set_scalar(name, {});
    ctx.params.lookup(name)->get().set(v);
  };
  auto text = [&](const regmatch_t& r) {
    return l.substr(static_cast<std::size_t>(r.rm_so), static_cast<std::size_t>(r.rm_eo - r.rm_so));
  };
  (void)ctx.params.set_scalar("MATCH", text(m[0]));
  set_int("MBEGIN", char_count(l, m[0].rm_so) + 1);
  set_int("MEND", char_count(l, m[0].rm_eo));
  std::vector<std::string> match, mbegin, mend;
  for (std::size_t i = 1; i < m.size(); ++i) {
    bool used = m[i].rm_so >= 0;
    match.push_back(used ? text(m[i]) : std::string{});
    mbegin.push_back(used ? std::to_string(char_count(l, m[i].rm_so) + 1) : "-1");
    mend.push_back(used ? std::to_string(char_count(l, m[i].rm_eo)) : "-1");
  }
  (void)ctx.params.set_array("match", std::move(match));
  (void)ctx.params.set_array("mbegin", std::move(mbegin));
  (void)ctx.params.set_array("mend", std::move(mend));
  return 0;
}

// ---------------------------------------------------------------------------
// test / [
// ---------------------------------------------------------------------------
namespace {

// Evaluates the arguments of `test` (without the command name and `]`).
// Up to four arguments are read by POSIX's rules, which decide by count
// and so let `test = = =` or `test ! = !` compare strings; anything longer,
// or that the rules don't settle, goes through the grammar
//   or := and (-o and)*    and := not (-a not)*    not := ! not | primary
//   primary := ( or ) | arg binop arg | unop arg | arg
struct Test {
  const std::vector<std::string>& a;
  ExecContext&                    ctx;
  std::string                     who;
  std::size_t                     pos = 0;
  bool                            error = false;

  int fail(const std::string& msg) {
    if (not error) complain(who, msg);
    error = true;
    return 2;
  }

  static int negate(int r) noexcept { return r < 2 ? 1 - r : r; }

  // A decimal integer, as test (unlike [[ ]]) requires.
  bool integer(const std::string& s, long long& out) {
    char* rest = nullptr;
    errno = 0;
    out = std::strtoll(s.c_str(), &rest, 10);
    while (rest and (*rest == ' ' or *rest == '\t')) ++rest;
    if (s.empty() or *rest or errno) {
      fail("integer expression expected: " + s);
      return false;
    }
    return true;
  }

  int binary(const std::string& l, std::string_view op, const std::string& r) {
    if (op == "=" or op == "==") return status(l == r);
    if (op == "!=") return status(l != r);
    if (op == "<")  return status(l < r);
    if (op == ">")  return status(l > r);
    if (op == "-a") return status(not l.empty() and not r.empty());
    if (op == "-o") return status(not l.empty() or not r.empty());
    if (cond_is_numeric(op)) {
      long long x = 0, y = 0;
      if (not integer(l, x) or not integer(r, y)) return 2;
      return cond_compare(op, x < y ? -1 : x > y ? 1 : 0);
    }
    return cond_file_binary(op, l, r);
  }

  int unary(std::string_view op, const std::string& arg) {
    int r = cond_unary(op, arg, ctx, who);
    if (r >= 2) error = true;
    return r;
  }

  // The rules for n arguments starting at index i; -1 when they don't
  // apply.
  int posix(std::size_t i, std::size_t n) {
    switch (n) {
      case 0: return 1;
      case 1: return status(not a[i].empty());
      case 2:
        if (a[i] == "!") return negate(posix(i + 1, 1));
        if (a[i].size() > 1 and a[i][0] == '-') return unary(a[i], a[i + 1]);
        return fail("condition expected: " + a[i]);
      case 3:
        if (cond_is_binary(a[i + 1]) or a[i + 1] == "-a" or a[i + 1] == "-o")
          return binary(a[i], a[i + 1], a[i + 2]);
        if (a[i] == "!") return negate(posix(i + 1, 2));
        if (a[i] == "(" and a[i + 2] == ")") return posix(i + 1, 1);
        return -1;
      case 4:
        if (a[i] == "!") {
          int r = posix(i + 1, 3);
          return r < 0 ? r : negate(r);
        }
        if (a[i] == "(" and a[i + 3] == ")") return posix(i + 1, 2);
        return -1;
      default:
        return -1;
    }
  }

  bool more() const noexcept { return pos < a.size(); }

  int primary() {
    if (not more()) return fail("argument expected");
    std::size_t left = a.size() - pos;
    if (a[pos] == "(" and left > 1) {
      ++pos;
      int r = parse_or();
      if (not more() or a[pos] != ")") return fail("')' expected");
      ++pos;
      return r;
    }
    if (left >= 3 and cond_is_binary(a[pos + 1])) {
      pos += 3;
      return binary(a[pos - 3], a[pos - 2], a[pos - 1]);
    }
    if (left >= 2 and cond_is_unary(a[pos])) {
      pos += 2;
      return unary(a[pos - 2], a[pos - 1]);
    }
    return status(not a[pos++].empty());
  }

  int parse_not() {
    if (more() and a[pos] == "!" and a.size() - pos > 1) {
      ++pos;
      return negate(parse_not());
    }
    return primary();
  }

  int parse_and() {
    int r = parse_not();
    while (more() and a[pos] == "-a") {
      ++pos;
      int s = parse_not();
      if (r == 0) r = s;
    }
    return r;
  }

  int parse_or() {
    int r = parse_and();
    while (more() and a[pos] == "-o") {
      ++pos;
      int s = parse_and();
      if (r == 1) r = s;
    }
    return r;
  }

  int run() {
    int r = posix(0, a.size());
    if (r >= 0) return error ? 2 : r;
    r = parse_or();
    if (not error and more()) fail("too many arguments");
    return error ? 2 : r;
  }
};

}  // namespace

int builtin_test(std::vector<std::string>& args, ExecContext& ctx) {
  std::vector<std::string> operands(args.begin() + 1, args.end());
  if (args[0] == "[") {
    if (operands.empty() or operands.back() != "]") {
      complain("[: ", "']' expected");
      return 2;
    }
    operands.pop_back();
  }
  return Test{operands, ctx, args[0] + ": "}.run();
}

}  // namespace jfsh
