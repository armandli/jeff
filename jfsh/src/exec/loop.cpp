// What the shell does around the commands it reads: startup and logout
// files, and the hooks run before each prompt and each command line.

#include <jfsh/exec/loop.h>

#include <cstdio>
#include <cstdlib>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/subst.h>

namespace jfsh {

namespace {

str param(ExecContext& ctx, strv name) {
  auto p = ctx.params.lookup(name);
  return (p and p->get().is_set()) ? p->get().scalar() : str{};
}

// $ZDOTDIR, else $HOME, else the home directory in the password file.
str zdotdir(ExecContext& ctx) {
  if (str z = param(ctx, "ZDOTDIR"); not z.empty()) return z;
  if (str h = param(ctx, "HOME"); not h.empty()) return h;
  if (struct passwd* pw = ::getpwuid(::getuid()); pw and pw->pw_dir) return pw->pw_dir;
  return "/";
}

// Sources `path` when it exists, as zsh does: a missing startup file is no
// error.  Returns false once the shell is exiting.
bool source_if_exists(Executor& ex, const str& path) {
  struct stat st{};
  if (::stat(path.c_str(), &st) != 0 or S_ISDIR(st.st_mode)) return true;
  ex.source_file(path, {}, path);
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
    if (not source_if_exists(ex, zdotdir(ctx()) + "/" + str(name))) return false;
    if (cfg.zsh_compat and rcs())
      return source_if_exists(ex, zdotdir(ctx()) + "/" + str(zsh_name));
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

}  // namespace

void init_shell_params(ExecContext& ctx) {
  auto& params = ctx.params;
  auto exported = [&](str name, str value) {
    params.assign(s::move(name), s::move(value), PM_SCALAR | PM_EXPORTED);
  };
  str lvl = param(ctx, "SHLVL");
  exported("SHLVL", s::to_string((lvl.empty() ? 0 : s::strtol(lvl.c_str(), nullptr, 10)) + 1));

  // $PWD from the environment keeps the path the user took (through
  // symlinks) when it still names this directory.
  str pwd = param(ctx, "PWD");
  struct stat here{}, there{};
  bool same = not pwd.empty() and pwd[0] == '/' and ::stat(".", &here) == 0
          and ::stat(pwd.c_str(), &there) == 0
          and here.st_dev == there.st_dev and here.st_ino == there.st_ino;
  if (not same) {
    char buf[4096];
    pwd = ::getcwd(buf, sizeof buf) ? str(buf) : str(".");
  }
  exported("PWD", pwd);
  if (param(ctx, "OLDPWD").empty()) exported("OLDPWD", pwd);

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
  auto& o = ex.context().opts;
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
}

void run_logout_files(Executor& ex, const StartupConfig& cfg) {
  if (not ex.context().opts.login()) return;
  Startup st{ex, cfg};
  // `exit` in a logout file doesn't stop the other one.
  ex.context().exiting = false;
  st.user(".jfsh_logout", ".zlogout");
  ex.context().exiting = false;
  st.system("jfsh_logout");
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
  ExecContext& ctx = ex.context();
  run_hook(ex, "precmd");
  if (ctx.exiting) return;
  // periodic: when $PERIOD seconds have passed since it last ran.
  if (s::int64_t period = int_param(ctx, "PERIOD", 0); period > 0
      and now > ctx.loop.last_periodic + period) {
    if (run_hook(ex, "periodic")) ctx.loop.last_periodic = now;
    if (ctx.exiting) return;
  }
  run_sched(ex, now);
  if (ctx.exiting) return;
  for (const auto& [_, hook] : ctx.hooks.before_prompt) hook();
  if (s::int64_t every = int_param(ctx, "MAILCHECK", 60); every > 0
      and now - ctx.loop.last_mailcheck > every) {
    check_mail(ex, now);
  }
}

void before_exec(Executor& ex, const str& line) {
  run_hook(ex, "preexec", {line, line, line});
}

void run_sched(Executor& ex, s::time_t now) {
  ExecContext& ctx = ex.context();
  KeepStatus keep{ctx};
  auto& events = ctx.loop.sched;
  while (not events.empty() and events.front().when <= now and not ctx.exiting) {
    str cmd = s::move(events.front().cmd);
    events.erase(events.begin());
    ex.exec_string(cmd);
    ctx.aborting = false;
  }
}

void check_mail(Executor& ex, s::time_t now) {
  ExecContext& ctx = ex.context();
  vec<str> entries;
  if (auto p = ctx.params.lookup("mailpath"); p and p->get().is_set() and p->get().is_array()) {
    entries = p->get().array();
  } else if (str mp = param(ctx, "MAILPATH"); not mp.empty()) {
    s::size_t start = 0;
    for (;;) {
      s::size_t colon = mp.find(':', start);
      entries.push_back(mp.substr(start, colon - start));
      if (colon == str::npos) break;
      start = colon + 1;
    }
  } else if (str mail = param(ctx, "MAIL"); not mail.empty()) {
    entries.push_back(mail);
  }
  s::time_t since = ctx.loop.last_mailcheck;
  ctx.loop.last_mailcheck = now;
  for (const auto& entry : entries) {
    if (entry.empty()) continue;
    auto q = entry.find('?');
    str file = entry.substr(0, q);
    struct stat st{};
    if (::stat(file.c_str(), &st) != 0 or S_ISDIR(st.st_mode)) continue;
    // New mail: something was written after it was last read, and after
    // the last check.
    if (st.st_size == 0 or st.st_atime > st.st_mtime or st.st_mtime < since) continue;
    str msg = "You have new mail.";
    if (q != str::npos) {
      // The message is expanded with $_ set to the file.
      ctx.params.assign("_", file, PM_SCALAR);
      Expander expander{ex, ctx};
      expander.no_process_substitution();
      auto m = expander.scalar(entry.substr(q + 1));
      msg = m ? *m : entry.substr(q + 1);
    }
    s::fprintf(stderr, "%s\n", msg.c_str());
  }
  s::fflush(stderr);
}

HistOptions history_options(const ExecContext& ctx) {
  auto param = [&](strv name) {
    auto p = ctx.params.lookup(name);
    return (p and p->get().is_set()) ? p->get().scalar() : str{};
  };
  const OptionState& o = ctx.opts;
  HistOptions h;
  str size = param("HISTSIZE");
  h.histsize = size.empty() ? 1000
             : static_cast<int>(s::clamp(s::strtol(size.c_str(), nullptr, 10), 1L, 1L << 30));
  str save = param("SAVEHIST");
  h.savehist = save.empty() ? h.histsize
             : static_cast<int>(s::clamp(s::strtol(save.c_str(), nullptr, 10), 0L, 1L << 30));
  h.histfile      = param("HISTFILE");
  h.append        = o.get(OPT_APPENDHISTORY);
  h.inc_append    = o.get(OPT_INCAPPENDHISTORY) or o.get(OPT_INCAPPENDHISTORYTIME);
  h.share         = o.get(OPT_SHAREHISTORY);
  h.extended      = o.get(OPT_EXTENDEDHISTORY) or h.share;
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
  h.fcntl_lock    = true;
  str chars = param("histchars");
  if (chars.empty()) chars = param("HISTCHARS");
  if (not chars.empty()) h.histchars = chars;
  return h;
}

}  // namespace jfsh
