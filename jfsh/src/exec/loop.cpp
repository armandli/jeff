// What the shell does around the commands it reads: startup and logout
// files, and the hooks run before each prompt and each command line.

#include <jfsh/exec/loop.h>

#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/subst.h>
#include <jfsh/parser/parser.h>

namespace jfsh {

namespace {

str param(ExecContext& ctx, strv name) {
  auto p = ctx.params.lookup(name);
  return (p and p->get().is_set()) ? p->get().scalar() : str{};
}

// The directory of the user's startup files: $ZDOTDIR when it is set (even
// to nothing), else $HOME, else the home directory in the password file
// (zsh's sourcehome and its `home`).
opt<str> zdotdir(ExecContext& ctx) {
  for (strv name : {strv("ZDOTDIR"), strv("HOME")})
    if (auto p = ctx.params.lookup(name); p and p->get().is_set()) return p->get().scalar();
  if (struct passwd* pw = ::getpwuid(::getuid()); pw and pw->pw_dir) return str(pw->pw_dir);
  return s::nullopt;
}

// Sources `path` when it can be read, as zsh's source() does: a missing
// startup file is no error.  Returns false once the shell is exiting.
bool source_if_exists(Executor& ex, const str& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0 or S_ISDIR(st.st_mode)) return true;
  ex.source_file(path, {}, path, /*startup=*/true);
  return not ex.context().exiting;
}

struct Startup {
  Executor&            ex;
  const StartupConfig& cfg;

  ExecContext& ctx() { return ex.context(); }
  bool rcs()         { return ctx().opts.get(OPT_RCS); }

  bool system(strv name) {
    if (not rcs() or not ctx().opts.get(OPT_GLOBALRCS)) return true;
    return source_if_exists(ex, cfg.sysconfdir + "/" + str(name));
  }
  bool user(strv name, strv zsh_name) {
    if (not rcs()) return true;
    if (auto dir = zdotdir(ctx()); dir and not source_if_exists(ex, *dir + "/" + str(name)))
      return false;
    if (cfg.zsh_compat and rcs())
      if (auto dir = zdotdir(ctx())) return source_if_exists(ex, *dir + "/" + str(zsh_name));
    return true;
  }
};

// The functions a hook calls: `name` itself, then those in ${name}_functions.
vec<str> hook_functions(ExecContext& ctx, strv name) {
  vec<str> out;
  out.emplace_back(name);
  if (auto p = ctx.params.lookup(str(name) + "_functions"); p and p->get().is_set())
    for (auto& f : p->get().is_array() ? p->get().array() : vec<str>{p->get().scalar()})
      if (not f.empty()) out.push_back(s::move(f));
  return out;
}

// Keeps $? and $pipestatus across a hook.
struct KeepStatus {
  ExecContext& ctx;
  int          rc;
  vec<int>     pipestatus;
  explicit KeepStatus(ExecContext& c) : ctx(c), rc(c.last_exit), pipestatus(c.pipestatus) {}
  ~KeepStatus() {
    ctx.last_exit  = rc;
    ctx.pipestatus = s::move(pipestatus);
  }
};

// zsh's ispwd: `path` is absolute, names the current directory, and has no
// `.` or `..` element.
bool is_pwd(const str& path) {
  if (path.empty() or path[0] != '/') return false;
  struct stat here{}, there{};
  if (::stat(".", &here) != 0 or ::stat(path.c_str(), &there) != 0) return false;
  if (here.st_dev != there.st_dev or here.st_ino != there.st_ino) return false;
  for (s::size_t i = 0; i < path.size();) {
    s::size_t end = path.find('/', i);
    if (end == str::npos) end = path.size();
    strv elem = strv(path).substr(i, end - i);
    if (elem == "." or elem == "..") return false;
    i = end + 1;
  }
  return true;
}

constexpr s::pair<strv, strv> TIED_SPECIALS[] = {
  {"PATH", "path"}, {"FPATH", "fpath"}, {"CDPATH", "cdpath"}, {"MANPATH", "manpath"},
  {"MAILPATH", "mailpath"}, {"FIGNORE", "fignore"}, {"PSVAR", "psvar"},
};

s::int64_t int_param(ExecContext& ctx, strv name, s::int64_t def) {
  auto p = ctx.params.lookup(name);
  if (not p or not p->get().is_set()) return def;
  const Param& q = p->get();
  if (q.is_integer()) return q.integer();
  str v = q.scalar();
  if (v.empty()) return def;
  return static_cast<s::int64_t>(s::strtoll(v.c_str(), nullptr, 10));
}

// zsh's checkmailpath: each `file` or `file?message` of `entries`.  New
// mail is a file written since it was last read and since the last check;
// a directory's files are checked in turn.  The message is expanded with $_
// set to the file.
void check_mail_files(Executor& ex, const vec<str>& entries) {
  ExecContext& ctx = ex.context();
  for (const auto& entry : entries) {
    auto q = entry.find('?');
    str file = entry.substr(0, q);
    if (file.empty()) {
      s::fprintf(stderr, "jfsh: empty MAILPATH component: %s\n", entry.c_str());
      continue;
    }
    struct stat st{};
    if (::stat(file.c_str(), &st) != 0) {
      if (errno != ENOENT) s::fprintf(stderr, "jfsh: %s: %s\n", s::strerror(errno), file.c_str());
      continue;
    }
    if (S_ISDIR(st.st_mode)) {
      vec<str> inside;
      if (DIR* dir = ::opendir(file.c_str())) {
        while (struct dirent* d = ::readdir(dir)) {
          strv name = d->d_name;
          if (name == "." or name == "..") continue;
          inside.push_back(file + "/" + str(name) + (q != str::npos ? entry.substr(q) : str{}));
        }
        ::closedir(dir);
      }
      check_mail_files(ex, inside);
      continue;
    }
    if (st.st_size != 0 and st.st_atime <= st.st_mtime and st.st_mtime >= ctx.loop.last_mailcheck) {
      if (q == str::npos) {
        s::fputs("You have new mail.\n", stderr);
      } else {
        str saved = s::exchange(ctx.underscore, file);
        Expander expander{ex, ctx};
        expander.no_process_substitution();
        if (auto m = expander.scalar(entry.substr(q + 1))) s::fprintf(stderr, "%s\n", m->c_str());
        ctx.underscore = s::move(saved);
      }
    }
    if (ctx.opts.get(OPT_MAILWARNING) and st.st_atime > st.st_mtime
        and st.st_atime > ctx.loop.last_mailcheck and st.st_size != 0)
      s::fprintf(stderr, "The mail in %s has been read.\n", file.c_str());
  }
}

}  // namespace

void init_shell_params(ExecContext& ctx) {
  auto& params = ctx.params;
  auto exported = [&](str name, str value) {
    params.assign(s::move(name), s::move(value), PM_SCALAR | PM_EXPORTED);
  };
  // $SHLVL is an integer, shown in base 10 as zsh's special is.
  str lvl = param(ctx, "SHLVL");
  Param& shlvl = params.assign("SHLVL",
      s::int64_t{(lvl.empty() ? 0 : s::strtoll(lvl.c_str(), nullptr, 10)) + 1},
      PM_INTEGER | PM_EXPORTED);
  shlvl.base = 10;

  // $PWD keeps the path the user took (through symlinks): the home
  // directory, else the environment's $PWD, when it names this directory
  // (zsh's ispwd); else the real path.  $OLDPWD starts the same, whatever
  // the environment says.  (zsh takes the home directory from the password
  // file; $HOME, which is that as a rule, saves a slow getpwuid here.)
  str pwd;
  str home = param(ctx, "HOME");
  if (home.empty())
    if (struct passwd* pw = ::getpwuid(::getuid()); pw and pw->pw_dir) home = pw->pw_dir;
  if (is_pwd(home)) {
    pwd = s::move(home);
  } else if (str env = param(ctx, "PWD"); is_pwd(env)) {
    pwd = s::move(env);
  } else {
    char buf[PATH_MAX];
    pwd = ::getcwd(buf, sizeof buf) ? str(buf) : str(".");
  }
  exported("PWD", pwd);
  exported("OLDPWD", pwd);
  // Mail is new when written after the shell started.
  ctx.loop.last_mailcheck = s::time(nullptr);
  if (auto m = params.lookup("MAILCHECK"); not m or not m->get().is_set())
    params.assign("MAILCHECK", s::int64_t{60}, PM_INTEGER);

  // zsh's tied specials; an environment value goes into the array.
  for (auto [scalar, array] : TIED_SPECIALS) {
    auto p = params.lookup(scalar);
    bool set = p and p->get().is_set();
    str text = set ? p->get().scalar() : str{};
    s::uint32_t flags = set ? p->get().flags & PM_EXPORTED : 0;
    params.assign(str(array), vec<str>{}, PM_ARRAY | PM_SPECIAL);
    Param& sp = params.assign(str(scalar), str{}, PM_SCALAR | PM_SPECIAL | flags);
    params.tie(sp, str(array), ':');
    if (set) sp.handlers->set_scalar(text);
  }
  if (param(ctx, "FPATH").empty()) {
    vec<str> fpath;
    if (*JFSH_SITE_FUNCTIONS_DIR) fpath.emplace_back(JFSH_SITE_FUNCTIONS_DIR);
    struct stat st{};
    if (*JFSH_FUNCTIONS_DIR and ::stat(JFSH_FUNCTIONS_DIR, &st) == 0)
      fpath.emplace_back(JFSH_FUNCTIONS_DIR);
    else if (*JFSH_SOURCE_FUNCTIONS_DIR)
      fpath.emplace_back(JFSH_SOURCE_FUNCTIONS_DIR);
    (void)params.set_array("fpath", s::move(fpath));
  }
}

void run_startup_files(Executor& ex, const StartupConfig& cfg) {
  Startup st{ex, cfg};
  auto& ctx = ex.context();
  auto& o   = ctx.opts;
  ctx.in_startup = true;
  [&] {
    // jfshenv is read even with NO_RCS, as zsh reads /etc/zshenv.
    if (not source_if_exists(ex, cfg.sysconfdir + "/jfshenv")) return;
    if (not st.user(".jfshenv", ".zshenv")) return;
    if (o.login()) {
      if (not st.system("jfsh_profile")) return;
      if (not st.user(".jfsh_profile", ".zprofile")) return;
    }
    if (o.interactive()) {
      if (not st.system("jfshrc")) return;
      if (not st.user(".jfshrc", ".zshrc")) return;
    }
    if (o.login()) {
      if (not st.system("jfsh_login")) return;
      st.user(".jfsh_login", ".zlogin");
    }
  }();
  ctx.in_startup = false;
}

void run_logout_files(Executor& ex, const StartupConfig& cfg) {
  ExecContext& ctx = ex.context();
  // zsh's zexit: only an interactive login shell with RCS reads them.
  if (not ctx.opts.login() or not ctx.opts.interactive() or not ctx.opts.get(OPT_RCS)) return;
  Startup st{ex, cfg};
  // `exit` in a logout file ends that file only; its status is the shell's.
  ctx.exiting = false;
  st.user(".jfsh_logout", ".zlogout");
  ctx.exiting = false;
  st.system("jfsh_logout");
  ctx.exiting = false;
}

bool run_hook(Executor& ex, strv name, const vec<str>& args) {
  ExecContext& ctx = ex.context();
  KeepStatus keep{ctx};
  bool ran = false;
  for (const auto& fn : hook_functions(ctx, name)) {
    auto it = ctx.functions.find(fn);
    if (it == ctx.functions.end() and ctx.autoloads.count(fn) and ex.load_autoload(fn))
      it = ctx.functions.find(fn);
    if (it == ctx.functions.end()) continue;
    ProgRef body = it->second;   // the hook may redefine itself
    vec<str> argv{fn};
    argv.insert(argv.end(), args.begin(), args.end());
    ex.call_function(body, argv);
    ran = true;
    if (ctx.exiting) break;
  }
  return ran;
}

void before_prompt(Executor& ex, s::time_t now) {
  // zsh's preprompt: precmd, periodic, the mail check, then the sched
  // events and the modules' hooks; an error stops it.
  ExecContext& ctx = ex.context();
  auto stop = [&] { return ctx.exiting or ctx.aborting; };
  run_hook(ex, "precmd");
  if (stop()) return;
  // periodic: when $PERIOD seconds have passed since it last ran.
  if (s::int64_t period = int_param(ctx, "PERIOD", 0); period != 0
      and now - ctx.loop.last_periodic > period) {
    if (run_hook(ex, "periodic")) ctx.loop.last_periodic = now;
    if (stop()) return;
  }
  if (s::int64_t every = int_param(ctx, "MAILCHECK", 0); every != 0
      and now - ctx.loop.last_mailcheck > every)
    check_mail(ex, now);
  run_sched(ex, now);
  if (ctx.exiting) return;
  for (const auto& [_, hook] : ctx.hooks.before_prompt) hook();
}

void before_exec(Executor& ex, const str& line, const CompiledProgram& prog) {
  run_hook(ex, "preexec", {line, job_text(prog), program_text(prog)});
}

void run_sched(Executor& ex, s::time_t now) {
  ExecContext& ctx = ex.context();
  KeepStatus keep{ctx};
  auto& events = ctx.loop.sched;
  while (not events.empty() and events.front().when <= now and not ctx.exiting) {
    str cmd = s::move(events.front().cmd);
    events.erase(events.begin());
    if (ctx.opts.get(OPT_VERBOSE)) s::fprintf(stderr, "%s\n", cmd.c_str());
    ex.exec_string(cmd);
    ctx.aborting = false;
    ctx.end_trap_abort();
  }
}

void check_mail(Executor& ex, s::time_t now) {
  ExecContext& ctx = ex.context();
  // $mailpath when its first entry isn't empty, else $MAIL.
  vec<str> entries;
  if (auto p = ctx.params.lookup("mailpath"); p and p->get().is_set() and p->get().is_array())
    entries = p->get().array();
  if (entries.empty() or entries.front().empty()) {
    entries.clear();
    if (str mail = param(ctx, "MAIL"); not mail.empty()) entries.push_back(s::move(mail));
  }
  check_mail_files(ex, entries);
  ctx.loop.last_mailcheck = now;
  s::fflush(stderr);
}

HistOptions history_options(const ExecContext& ctx) {
  auto param = [&](strv name) {
    auto p = ctx.params.lookup(name);
    return (p and p->get().is_set()) ? p->get().scalar() : str{};
  };
  const OptionState& o = ctx.opts;
  HistOptions h;
  // zsh's defaults when they are unset: 30 and 0.
  str size = param("HISTSIZE");
  h.histsize = size.empty() ? 30
             : static_cast<int>(s::clamp(s::strtol(size.c_str(), nullptr, 10), 1L, 1L << 30));
  str save = param("SAVEHIST");
  h.savehist = save.empty() ? 0
             : static_cast<int>(s::clamp(s::strtol(save.c_str(), nullptr, 10), 0L, 1L << 30));
  h.histfile      = param("HISTFILE");
  h.append        = o.get(OPT_APPENDHISTORY);
  h.inc_append    = o.get(OPT_INCAPPENDHISTORY) or o.get(OPT_INCAPPENDHISTORYTIME);
  h.share         = o.get(OPT_SHAREHISTORY);
  h.extended      = o.get(OPT_EXTENDEDHISTORY) or h.share;
  h.extended_set  = o.get(OPT_EXTENDEDHISTORY);
  h.ignore_dups   = o.get(OPT_HISTIGNOREDUPS);
  h.ignore_all    = o.get(OPT_HISTIGNOREALLDUPS);
  h.ignore_space  = o.get(OPT_HISTIGNORESPACE);
  h.no_store      = o.get(OPT_HISTNOSTORE);
  h.no_functions  = o.get(OPT_HISTNOFUNCTIONS);
  h.reduce_blanks = o.get(OPT_HISTREDUCEBLANKS);
  h.save_no_dups  = o.get(OPT_HISTSAVENODUPS);
  h.find_no_dups  = o.get(OPT_HISTFINDNODUPS);
  h.verify        = o.get(OPT_HISTVERIFY);
  h.bang          = o.get(OPT_BANGHIST);
  h.csh_junkie    = o.get(OPT_CSHJUNKIEHISTORY);
  h.comments      = o.get(OPT_INTERACTIVECOMMENTS);
  h.fcntl_lock    = true;
  // zsh's histcharssetfn: set (even empty) they are the characters, at
  // most 3, a missing one none; unset, the defaults.
  if (auto p = ctx.params.lookup("histchars")) h.histchars = p->get().scalar().substr(0, 3);
  return h;
}

}  // namespace jfsh
