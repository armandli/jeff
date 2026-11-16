// Directory builtins: cd (chdir), pushd, popd, dirs, pwd.  $PWD is kept
// logical (symlinks as walked) unless CHASE_LINKS or -P says otherwise;
// the directory stack, without $PWD, is ExecContext::dirstack ($dirstack).

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

namespace jfsh {

namespace {

str param_value(const ParamTable& params, strv name) {
  auto p = params.lookup(name);
  return (p and p->get().is_set()) ? p->get().scalar() : str{};
}

str physical_cwd() {
  char buf[PATH_MAX];
  return ::getcwd(buf, sizeof buf) ? str(buf) : str{};
}

// $PWD when it names the current directory, otherwise the real path.
str current_dir(const ParamTable& params) {
  str pwd = param_value(params, "PWD");
  struct stat a{}, b{};
  if (not pwd.empty() and pwd[0] == '/' and ::stat(pwd.c_str(), &a) == 0
      and ::stat(".", &b) == 0 and a.st_dev == b.st_dev and a.st_ino == b.st_ino)
    return pwd;
  return physical_cwd();
}

// Removes `.` and `x/..` components without looking at the file system.
str lexical_clean(strv path) {
  vec<str> parts;
  s::size_t i = 0;
  while (i <= path.size()) {
    s::size_t j = path.find('/', i);
    if (j == strv::npos) j = path.size();
    strv part = path.substr(i, j - i);
    if (part == "..") {
      if (not parts.empty()) parts.pop_back();
    } else if (not part.empty() and part != ".") {
      parts.emplace_back(part);
    }
    i = j + 1;
  }
  str out;
  for (const auto& p : parts) out += "/" + p;
  return out.empty() ? "/" : out;
}

struct CdOptions {
  bool physical = false;   // -P, or CHASE_LINKS
  bool silent   = false;   // -q
};

// Changes to `target` (relative ones to $PWD), updates $PWD/$OLDPWD and
// runs chpwd.  Prints the error and returns false on failure.
bool change_dir(strv cmd, const str& target, const CdOptions& o, ExecContext& ctx,
                bool print = false) {
  str old = current_dir(ctx.params);
  bool physical = o.physical or ctx.opts.get(OPT_CHASELINKS);
  str path = target[0] == '/' ? target : old + "/" + target;
  // Logically, `a/..` is `.` (unless CHASE_DOTS asks for the real parent).
  str logical = physical or ctx.opts.get(OPT_CHASEDOTS) ? path : lexical_clean(path);
  int rc = ::chdir(logical.c_str());
  if (rc != 0 and logical != path) rc = ::chdir(path.c_str());
  if (rc != 0) {
    int err = errno;
    const char* why = err == ENOENT  ? "no such file or directory"
                    : err == ENOTDIR ? "not a directory"
                    : err == EACCES  ? "permission denied"
                                     : s::strerror(err);
    s::fprintf(stderr, "jfsh: %.*s: %s: %s\n", static_cast<int>(cmd.size()), cmd.data(),
               why, target.c_str());
    return false;
  }
  str now = physical or ctx.opts.get(OPT_CHASEDOTS) ? physical_cwd() : lexical_clean(path);
  if (now.empty()) now = physical_cwd();
  (void)ctx.params.set_scalar("OLDPWD", old);
  (void)ctx.params.set_scalar("PWD", now);
  if (print and ctx.opts.interactive() and not o.silent and not ctx.opts.get(OPT_PUSHDSILENT))
    s::printf("%s\n", tilde_abbrev(ctx.params, now).c_str());
  // The chpwd hook, as zsh runs it after a change of directory.
  if (old != now and not o.silent) {
    if (auto fn = ctx.functions.find("chpwd"); fn != ctx.functions.end()) {
      vec<str> fargs{"chpwd"};
      ProgRef body = fn->second;
      ctx.executor->call_function(body, fargs);
    }
  }
  return true;
}

// The stack with $PWD on top.
vec<str> full_stack(const ExecContext& ctx) {
  vec<str> all{current_dir(ctx.params)};
  all.insert(all.end(), ctx.dirstack.begin(), ctx.dirstack.end());
  return all;
}

// `+N` / `-N`: an index into the full stack (PUSHD_MINUS swaps them).
opt<s::size_t> stack_index(strv arg, s::size_t size, bool minus_swapped) {
  if (arg.size() < 2 or (arg[0] != '+' and arg[0] != '-')) return s::nullopt;
  for (char c : arg.substr(1))
    if (not s::isdigit(static_cast<unsigned char>(c))) return s::nullopt;
  s::size_t n = s::strtoul(str(arg.substr(1)).c_str(), nullptr, 10);
  bool from_top = (arg[0] == '+') != minus_swapped;
  if (n >= size) return s::size_t(-1);
  return from_top ? n : size - 1 - n;
}

void print_stack(const ExecContext& ctx, bool full_paths, bool one_per_line, bool numbered) {
  auto all = full_stack(ctx);
  for (s::size_t i = 0; i < all.size(); ++i) {
    str d = full_paths ? all[i] : tilde_abbrev(ctx.params, all[i]);
    if (numbered)          s::printf("%zu\t%s\n", i, d.c_str());
    else if (one_per_line) s::printf("%s\n", d.c_str());
    else                   s::printf("%s%s", i ? " " : "", d.c_str());
  }
  if (not numbered and not one_per_line) s::printf("\n");
}

void push_dir(ExecContext& ctx, str dir) {
  if (ctx.opts.get(OPT_PUSHDIGNOREDUPS))
    s::erase(ctx.dirstack, dir);
  ctx.dirstack.insert(ctx.dirstack.begin(), s::move(dir));
}

void print_stack_if_interactive(ExecContext& ctx) {
  if (ctx.opts.interactive() and not ctx.opts.get(OPT_PUSHDSILENT))
    print_stack(ctx, false, false, false);
}

// `cd old new`: $PWD with the first `old` replaced by `new`.
opt<str> substitute_pwd(strv cmd, const ExecContext& ctx, const str& from, const str& to) {
  str pwd = current_dir(ctx.params);
  auto at = pwd.find(from);
  if (at == str::npos) {
    builtin_error(cmd, "string not in pwd: " + from);
    return s::nullopt;
  }
  return pwd.substr(0, at) + to + pwd.substr(at + from.size());
}

// Options of cd/pushd: -q -s -L -P.  Stops at a `-N`/`+N` stack index.
bool cd_options(vec<str>& args, s::size_t& i, CdOptions& o) {
  for (i = 1; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "--") { ++i; break; }
    if (a.size() < 2 or a[0] != '-' or s::isdigit(static_cast<unsigned char>(a[1]))) break;
    for (char c : strv(a).substr(1)) {
      switch (c) {
        case 'q': o.silent = true; break;
        case 's': break;
        case 'L': o.physical = false; break;
        case 'P': o.physical = true; break;
        default:
          s::fprintf(stderr, "jfsh: %s: bad option: -%c\n", args[0].c_str(), c);
          return false;
      }
    }
  }
  return true;
}

// Resolves a cd argument: CDPATH for a relative name (reports whether a
// CDPATH entry was used, which makes cd print the result), then
// CDABLE_VARS.
str resolve_with_cdpath(const str& dir, ExecContext& ctx, bool& via_cdpath) {
  via_cdpath = false;
  bool relative = dir[0] != '/' and not dir.starts_with("./") and not dir.starts_with("../")
                  and dir != "." and dir != "..";
  if (relative) {
    vec<str> cdpath;
    if (auto p = ctx.params.lookup("cdpath"); p and p->get().is_array()) {
      cdpath = p->get().array();
    } else {
      str all = param_value(ctx.params, "CDPATH");
      s::size_t start = 0;
      while (not all.empty() and start <= all.size()) {
        s::size_t end = all.find(':', start);
        if (end == str::npos) end = all.size();
        cdpath.push_back(all.substr(start, end - start));
        start = end + 1;
      }
    }
    // The current directory comes first unless CDPATH lists it.
    struct stat st{};
    if (::stat(dir.c_str(), &st) == 0 and S_ISDIR(st.st_mode)) return dir;
    for (const auto& base : cdpath) {
      if (base.empty() or base == ".") continue;
      str candidate = base + "/" + dir;
      if (::stat(candidate.c_str(), &st) == 0 and S_ISDIR(st.st_mode)) {
        via_cdpath = true;
        return candidate[0] == '/' ? candidate : current_dir(ctx.params) + "/" + candidate;
      }
    }
  }
  if (ctx.opts.get(OPT_CDABLEVARS) and is_identifier(dir)) {
    struct stat st{};
    if (::stat(dir.c_str(), &st) != 0) {
      str v = param_value(ctx.params, dir);
      if (not v.empty() and v[0] == '/') {
        via_cdpath = true;
        return v;
      }
    }
  }
  return dir;
}

// cd and chdir; with AUTO_PUSHD, pushd.
int cd_common(vec<str>& args, ExecContext& ctx, bool pushd) {
  strv cmd = args[0];
  CdOptions o;
  s::size_t i = 1;
  if (not cd_options(args, i, o)) return 1;
  vec<str> rest(args.begin() + static_cast<s::ptrdiff_t>(i), args.end());
  if (rest.size() > 2) return builtin_error(cmd, "too many arguments");
  bool print = false;
  str  target;
  bool minus_swapped = ctx.opts.get(OPT_PUSHDMINUS);
  if (rest.size() == 2) {
    auto sub = substitute_pwd(cmd, ctx, rest[0], rest[1]);
    if (not sub) return 1;
    target = *sub;
    print  = true;
  } else if (rest.empty() or rest[0].empty()) {
    if (not rest.empty()) return 0;   // `cd ""` stays put
    if (pushd and not ctx.opts.get(OPT_PUSHDTOHOME)) {
      // pushd alone swaps the top two entries.
      if (ctx.dirstack.empty()) return builtin_error(cmd, "no other directory");
      str top = ctx.dirstack.front();
      str here = current_dir(ctx.params);
      if (not change_dir(cmd, top, o, ctx)) return 1;
      ctx.dirstack.front() = here;
      print_stack_if_interactive(ctx);
      return 0;
    }
    target = param_value(ctx.params, "HOME");
    if (target.empty()) return 0;
  } else if (rest[0] == "-" and not pushd) {
    target = param_value(ctx.params, "OLDPWD");
    if (target.empty()) return builtin_error(cmd, "no previous directory");
    print = true;
  } else if (auto idx = stack_index(rest[0], ctx.dirstack.size() + 1, minus_swapped)) {
    if (*idx == s::size_t(-1)) return builtin_error(cmd, "no such entry in dir stack");
    auto all = full_stack(ctx);
    if (pushd) {
      // pushd +N rotates the stack so entry N is on top.
      s::rotate(all.begin(), all.begin() + static_cast<s::ptrdiff_t>(*idx), all.end());
      if (not change_dir(cmd, all.front(), o, ctx)) return 1;
      ctx.dirstack.assign(all.begin() + 1, all.end());
      print_stack_if_interactive(ctx);
      return 0;
    }
    target = all[*idx];
    if (*idx > 0) ctx.dirstack.erase(ctx.dirstack.begin() + static_cast<s::ptrdiff_t>(*idx - 1));
    str here = current_dir(ctx.params);
    if (not change_dir(cmd, target, o, ctx, true)) return 1;
    if (ctx.opts.get(OPT_AUTOPUSHD)) push_dir(ctx, here);
    return 0;
  } else {
    bool via = false;
    target = resolve_with_cdpath(rest[0], ctx, via);
    print = via;
  }
  str here = current_dir(ctx.params);
  if (not change_dir(cmd, target, o, ctx, print and not pushd)) return 1;
  if (pushd or ctx.opts.get(OPT_AUTOPUSHD)) {
    push_dir(ctx, here);
    if (pushd) print_stack_if_interactive(ctx);
  }
  return 0;
}

}  // namespace

int builtin_cd(vec<str>& args, ExecContext& ctx) { return cd_common(args, ctx, false); }
int builtin_pushd(vec<str>& args, ExecContext& ctx) { return cd_common(args, ctx, true); }

// popd [-q] [±N]: drops the top of the stack and changes to the next
// entry; with ±N drops that entry instead.
int builtin_popd(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  CdOptions o;
  s::size_t i = 1;
  if (not cd_options(args, i, o)) return 1;
  if (ctx.dirstack.empty()) return builtin_error(cmd, "directory stack empty");
  s::size_t idx = 0;
  if (i < args.size()) {
    auto n = stack_index(args[i], ctx.dirstack.size() + 1, ctx.opts.get(OPT_PUSHDMINUS));
    if (not n) return builtin_error(cmd, "bad argument: " + args[i]);
    if (*n == s::size_t(-1)) return builtin_error(cmd, "no such entry in dir stack");
    idx = *n;
  }
  if (idx == 0) {
    str next = ctx.dirstack.front();
    if (not change_dir(cmd, next, o, ctx)) return 1;
    ctx.dirstack.erase(ctx.dirstack.begin());
  } else {
    ctx.dirstack.erase(ctx.dirstack.begin() + static_cast<s::ptrdiff_t>(idx - 1));
  }
  if (not o.silent) print_stack_if_interactive(ctx);
  return 0;
}

// dirs [-clpv] [dir …]: prints the stack, clears it (-c), or with
// arguments replaces it.
int builtin_dirs(vec<str>& args, ExecContext& ctx) {
  OptParse o;
  if (not parse_options(args, "clpv", "", o)) return 1;
  if (o.has('c')) {
    ctx.dirstack.clear();
    return 0;
  }
  if (o.next < args.size()) {
    ctx.dirstack.assign(args.begin() + static_cast<s::ptrdiff_t>(o.next), args.end());
    return 0;
  }
  print_stack(ctx, o.has('l'), o.has('p'), o.has('v'));
  return 0;
}

// pwd [-rLP]: the logical $PWD, or with -P / -r / CHASE_LINKS the real one.
int builtin_pwd(vec<str>& args, ExecContext& ctx) {
  OptParse o;
  if (not parse_options(args, "rLP", "", o)) return 1;
  bool physical = o.has('P') or o.has('r') or ctx.opts.get(OPT_CHASELINKS);
  if (o.has('L')) physical = false;
  str dir = physical ? physical_cwd() : current_dir(ctx.params);
  if (dir.empty()) return builtin_error(args[0], s::strerror(errno));
  s::printf("%s\n", dir.c_str());
  return 0;
}

}  // namespace jfsh
