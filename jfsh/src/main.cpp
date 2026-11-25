#include <cerrno>
#include <csignal>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include <jfsh/agent/agent.h>
#include <jfsh/agent/provider.h>
#include <jfsh/builtins/builtins.h>
#include <jfsh/core/aliases.h>
#include <jfsh/core/jobs.h>
#include <jfsh/core/options.h>
#include <jfsh/core/param.h>
#include <jfsh/core/read_file.h>
#include <jfsh/core/signals.h>
#include <jfsh/core/types.h>
#include <jfsh/editor/editor.h>
#include <jfsh/editor/zle.h>
#include <jfsh/exec/executor.h>
#include <jfsh/exec/invocation.h>
#include <jfsh/exec/loop.h>
#include <jfsh/history/history.h>
#include <jfsh/lexer/lexer.h>
#include <jfsh/modules/attr.h>
#include <jfsh/modules/cap.h>
#include <jfsh/modules/clone.h>
#include <jfsh/modules/compctl.h>
#include <jfsh/modules/computil.h>
#include <jfsh/modules/curses.h>
#include <jfsh/modules/datetime.h>
#include <jfsh/modules/db_gdbm.h>
#include <jfsh/modules/deltochar.h>
#include <jfsh/modules/example.h>
#include <jfsh/modules/files.h>
#include <jfsh/modules/langinfo.h>
#include <jfsh/modules/mapfile.h>
#include <jfsh/modules/mathfunc.h>
#include <jfsh/modules/nearcolor.h>
#include <jfsh/modules/net_socket.h>
#include <jfsh/modules/net_tcp.h>
#include <jfsh/modules/newuser.h>
#include <jfsh/modules/parameter.h>
#include <jfsh/modules/pcre.h>
#include <jfsh/modules/private.h>
#include <jfsh/modules/regex.h>
#include <jfsh/modules/stat.h>
#include <jfsh/modules/system.h>
#include <jfsh/modules/termcap.h>
#include <jfsh/modules/terminfo.h>
#include <jfsh/modules/watch.h>
#include <jfsh/modules/zftp.h>
#include <jfsh/modules/zleparameter.h>
#include <jfsh/modules/zprof.h>
#include <jfsh/modules/zpty.h>
#include <jfsh/modules/zselect.h>
#include <jfsh/modules/zutil.h>
#include <jfsh/parser/parser.h>
#include <jfsh/prompt/prompt.h>
#include <jfsh/router/jeff.h>
#include <jfsh/router/jev.h>
#include <jfsh/router/router.h>

#ifndef JFSH_VERSION
#define JFSH_VERSION "0"
#endif

extern char** environ;

namespace {

namespace s = std;
using jfsh::opt;
using jfsh::str;
using jfsh::vec;

// The System 1 router the options ask for:
//   --router jeff       → local jeff-server HTTP client (default port 8765)
//   --router jev        → TypeSafe JEV cloud API
//   --router heuristic  → zsh -n heuristic (no service required)
//   Auto-detect: --typesafe-key → jev; --jeff-url → jeff; else heuristic
void load_router(jfsh::Router& router, const jfsh::Invocation& inv) {
  router.set_bypass(inv.norouter);
  bool use_jev  = inv.router == "jev" or (inv.router.empty() and not inv.typesafe_key.empty());
  bool use_jeff = inv.router == "jeff"
               or (inv.router.empty() and not inv.jeff_url.empty() and not use_jev);
  if (use_jev) {
    jfsh::JevConfig cfg = jfsh::JevConfig::from_env();
    if (not inv.typesafe_key.empty()) cfg.api_key = inv.typesafe_key;
    if (not inv.jev_model.empty())    cfg.model   = inv.jev_model;
    router.load_jev(s::move(cfg));
  } else if (use_jeff) {
    jfsh::JeffConfig cfg;
    if (not inv.jeff_url.empty()) cfg.base_url = inv.jeff_url;
    router.load_jeff(s::move(cfg));
  }
}

jfsh::ProviderConfig provider_config(const jfsh::Invocation& inv) {
  jfsh::ProviderConfig cfg = jfsh::ProviderConfig::from_env();
  if (inv.provider == "openai")          cfg.kind = jfsh::ProviderKind::OpenAI;
  else if (inv.provider == "anthropic")  cfg.kind = jfsh::ProviderKind::Anthropic;
  else if (inv.provider == "openrouter") cfg.kind = jfsh::ProviderKind::OpenRouter;
  else if (inv.provider == "ollama")     cfg.kind = jfsh::ProviderKind::Ollama;
  if (not inv.api_key.empty())   cfg.api_key = inv.api_key;
  if (not inv.llm_model.empty()) cfg.model   = inv.llm_model;
  return cfg;
}

// Whether `path` names something that is not a directory.
bool is_file(const str& path) {
  struct stat st{};
  return ::stat(path.c_str(), &st) == 0 and not S_ISDIR(st.st_mode);
}

// zsh's init_io: the shell's own terminal (SHTTY), read and written, on a
// close-on-exec fd above 9: the one standard input names, else standard
// input or output themselves, else /dev/tty.  -1 when there is none.
int open_shell_tty() {
  auto rdwr = [](int fd) { return (::fcntl(fd, F_GETFL, 0) & O_ACCMODE) == O_RDWR; };
  auto high = [](int fd) {
    if (fd < 0) return fd;
    int moved = ::fcntl(fd, F_DUPFD_CLOEXEC, 10);
    ::close(fd);
    return moved;
  };
  if (::isatty(STDIN_FILENO)) {
    if (const char* name = ::ttyname(STDIN_FILENO))
      if (int fd = high(::open(name, O_RDWR | O_NOCTTY | O_CLOEXEC)); fd >= 0) return fd;
    if (rdwr(STDIN_FILENO)) return ::fcntl(STDIN_FILENO, F_DUPFD_CLOEXEC, 10);
  }
  if (::isatty(STDOUT_FILENO) and rdwr(STDOUT_FILENO))
    return ::fcntl(STDOUT_FILENO, F_DUPFD_CLOEXEC, 10);
  return high(::open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC));
}

str param(jfsh::ParamTable& params, s::string_view name) {
  auto p = params.lookup(name);
  return (p and p->get().is_set()) ? p->get().scalar() : str{};
}

}  // namespace

int main(int argc, char* argv[]) {
  // The line editor measures characters with wcwidth(); sorting follows
  // the locale's collation, as zsh's does.
  std::setlocale(LC_CTYPE, "");
  // Globs and ${(o)a} sort by the locale's collation, as in zsh.
  std::setlocale(LC_COLLATE, "");
  auto parsed = jfsh::parse_invocation(vec<str>(argv, argv + argc));
  if (not parsed) {
    s::fprintf(stderr, "%s\n", parsed.error().message.c_str());
    return 1;
  }
  jfsh::Invocation& inv = *parsed;
  if (inv.help) {
    s::fputs(jfsh::invocation_usage(argv[0]).c_str(), stdout);
    return 0;
  }
  if (inv.version) {
    s::printf("jfsh %s\n", JFSH_VERSION);
    return 0;
  }

  // Interactive, unless told otherwise, when standard input is a terminal
  // and there is neither a command, nor a script, nor -t (zsh's parseargs).
  bool interactive = inv.interactive.value_or(
      isatty(STDIN_FILENO) and not inv.command and not inv.script and not inv.single);

  jfsh::ParamTable  params;
  jfsh::OptionState opts;
  // M12: when argv[0] is `sh`, `ksh` or `csh`, switch to that emulation
  // before applying any invocation-specific options or environment vars,
  // so SH_WORD_SPLIT, BSD_ECHO, SH_OPTION_LETTERS, etc. defaults are in
  // place before the rc files see them.
  if (inv.emulation != jfsh::OptionState::Emulation::ZSH)
    opts.apply_emulation(inv.emulation, /*full=*/true);
  opts.assign(jfsh::OPT_INTERACTIVE,   interactive);
  opts.assign(jfsh::OPT_SHINSTDIN,     inv.shinstdin);
  // MONITOR needs a terminal: zsh's init_io turns it off when it finds none.
  // An interactive shell draws its line editor there, so `jfsh > file`
  // still prompts on the terminal.
  int shell_tty = open_shell_tty();
  opts.assign(jfsh::OPT_MONITOR,       inv.monitor.value_or(interactive) and shell_tty >= 0);
  if (not interactive and shell_tty >= 0) {
    ::close(shell_tty);
    shell_tty = -1;
  }
  opts.assign(jfsh::OPT_ZLE,           inv.zle.value_or(interactive));
  opts.assign(jfsh::OPT_LOGIN,         inv.login);
  opts.assign(jfsh::OPT_RCS,           inv.rcs);
  opts.assign(jfsh::OPT_GLOBALRCS,     inv.global_rcs);
  opts.assign(jfsh::OPT_SINGLECOMMAND, inv.single);
  // HASH_DIRS defaults to INTERACTIVE, as zsh's init sets it.
  opts.assign(jfsh::OPT_HASHDIRS,      interactive);
  for (auto [idx, on] : inv.options) opts.assign(idx, on);

  for (char** ep = environ; ep and *ep; ++ep) {
    str env(*ep);
    auto eq = env.find('=');
    if (eq == str::npos) continue;
    params.assign(env.substr(0, eq), env.substr(eq + 1),
                  jfsh::PM_SCALAR | jfsh::PM_EXPORTED);
  }

  jfsh::Signals.init(interactive, shell_tty);
  // However main returns, the terminal goes back to the process group that
  // started the shell, as zsh's release_pgrp does in zexit.
  struct ReleaseTerminal {
    ~ReleaseTerminal() { jfsh::Signals.release_terminal(); }
  } release_terminal;

  jfsh::Router router;
  load_router(router, inv);
  auto agent = jfsh::Agent::create(provider_config(inv));

  jfsh::ExecContext ctx{params, opts, jfsh::Jobs, jfsh::Signals, 0, -1};
  ctx.arg0       = inv.arg0;
  ctx.positional = inv.positional;
  jfsh::Executor executor(ctx);
  jfsh::init_shell_params(ctx);
  // As zsh's setupvals.
  if (inv.command)            params.assign("ZSH_EXECUTION_STRING", *inv.command);
  if (inv.script) params.assign("ZSH_SCRIPT", *inv.script);

  // M10: register the shipped modules.  Register makes `zmodload NAME`
  // able to find them; the four modules jfsh ships today are auto-loaded
  // so $EPOCHSECONDS/strftime, sqrt()/sin(), $functions/$commands/… and
  // zstyle/zparseopts/is-at-least/zformat are available without a
  // `zmodload` in the user's rc files, as zsh does with a typical
  // distribution zshrc.
  // $module_path default from the CMake cache (M10 polish).  A user can
  // override with `module_path=(…)` in their rc, as with zsh.
  if (sizeof JFSH_MODULE_DIR > 1) ctx.modules.add_search_path(JFSH_MODULE_DIR);

  jfsh::register_datetime_module    (ctx.modules);
  jfsh::register_mathfunc_module    (ctx.modules);
  jfsh::register_parameter_module   (ctx.modules);
  jfsh::register_zutil_module       (ctx.modules);
  jfsh::register_computil_module    (ctx.modules);
  jfsh::register_system_module      (ctx.modules);
  jfsh::register_files_module       (ctx.modules);
  jfsh::register_stat_module        (ctx.modules);
  jfsh::register_pcre_module        (ctx.modules);
  jfsh::register_regex_module       (ctx.modules);
  jfsh::register_example_module     (ctx.modules);
  // M11 Phase 11a modules — registered, not auto-loaded.
  jfsh::register_langinfo_module    (ctx.modules);
  jfsh::register_zselect_module     (ctx.modules);
  jfsh::register_net_socket_module  (ctx.modules);
  jfsh::register_zleparameter_module(ctx.modules);
  // M11 Phase 11b modules — registered, not auto-loaded.
  jfsh::register_net_tcp_module     (ctx.modules);
  jfsh::register_zftp_module        (ctx.modules);
  jfsh::register_termcap_module     (ctx.modules);
  jfsh::register_terminfo_module    (ctx.modules);
  jfsh::register_curses_module      (ctx.modules);
  jfsh::register_nearcolor_module   (ctx.modules);
  jfsh::register_mapfile_module     (ctx.modules);
  jfsh::register_db_gdbm_module     (ctx.modules);
  jfsh::register_private_module     (ctx.modules);
  jfsh::register_zpty_module        (ctx.modules);
  jfsh::register_zprof_module       (ctx.modules);
  jfsh::register_attr_module        (ctx.modules);
  jfsh::register_cap_module         (ctx.modules);
  jfsh::register_deltochar_module   (ctx.modules);
  jfsh::register_compctl_module     (ctx.modules);
  jfsh::register_clone_module       (ctx.modules);
  jfsh::register_watch_module       (ctx.modules);
  jfsh::register_newuser_module     (ctx.modules);
  // zsh's autoloaded builtins: the first use loads the module.
  for (auto [mod, builtin] : {s::pair{"zsh/termcap", "echotc"}, s::pair{"zsh/terminfo", "echoti"},
                              s::pair{"zsh/compctl", "compctl"},
                              s::pair{"zsh/param/private", "private"}})
    ctx.modules.mark_autoload(mod, builtin);
  (void)ctx.modules.load("zsh/datetime",  &ctx);
  (void)ctx.modules.load("zsh/mathfunc",  &ctx);
  (void)ctx.modules.load("zsh/parameter", &ctx);
  (void)ctx.modules.load("zsh/zutil",     &ctx);
  (void)ctx.modules.load("zsh/computil",  &ctx);
  (void)ctx.modules.load("zsh/system",    &ctx);
  // zsh/files, zsh/stat, zsh/pcre, zsh/regex and zsh/example are
  // registered but NOT auto-loaded:
  //   - zsh/files replaces externals (rm, mv, …) which users often prefer
  //     to keep as GNU coreutils.
  //   - zsh/stat shadows the common `stat` external.
  //   - zsh/pcre needs libpcre2 at build time; opt in with `zmodload zsh/pcre`.
  //   - zsh/regex supplies `[[ -regex-match ]]`; opt in with `zmodload zsh/regex`.
  //   - zsh/example is a reference for third-party module authors.

  if (interactive) executor.update_window_size();

  // zsh's rule: vi editing when $VISUAL or $EDITOR mentions vi; the
  // startup files can say otherwise (bindkey -e, setopt emacs).
  {
    str visual = param(params, "VISUAL"), editor = param(params, "EDITOR");
    ctx.zle->select_main((visual.empty() ? editor : visual).find("vi") != str::npos);
  }
  ctx.zle->tty_out = shell_tty;

  // zsh/newuser (M11): an interactive shell with no user startup files
  // offers to write a starter ~/.jfshrc first, so the one it writes is
  // read straight away.
  if (interactive and ctx.opts.get(jfsh::OPT_RCS) and ::isatty(STDIN_FILENO)
      and jfsh::newuser_needed(ctx) and ctx.modules.load("zsh/newuser", &ctx))
    (void)executor.exec_string("zsh-newuser-install");

  jfsh::StartupConfig startup;
  startup.zsh_compat = inv.zsh_compat_rc;
  jfsh::run_startup_files(executor, startup);
  ctx.aborting = false;
  ctx.end_trap_abort();

  // History, after the startup files have had their say on HISTFILE,
  // HISTSIZE and the history options.  Every shell has one in memory
  // (print -s, fc -p work in a script); only an interactive one with RCS
  // reads and saves the file (zsh's init_misc and zexit).  A shell that
  // exits in a startup file saves nothing.
  jfsh::HistoryManager history(jfsh::history_options(ctx));
  bool save_history = interactive and not ctx.exiting;
  if (interactive) {
    auto hist_opts = jfsh::history_options(ctx);
    if (not hist_opts.histfile.empty() and ctx.opts.get(jfsh::OPT_RCS))
      (void)history.load(hist_opts.histfile);
  }
  ctx.history = &history;   // fc, history, r, $HISTCMD
  // The :c modifier: a name with a slash when it can run, else the
  // command's path (zsh's findcmd).
  history.set_command_lookup([&executor](const str& name) -> opt<str> {
    if (name.find('/') != str::npos) {
      struct stat st{};
      bool runs = ::stat(name.c_str(), &st) == 0 and S_ISREG(st.st_mode)
                  and ::access(name.c_str(), X_OK) == 0;
      return runs ? opt<str>(name) : s::nullopt;
    }
    auto r = executor.find_command(name);
    return r ? opt<str>(*r) : s::nullopt;
  });
  // `exec` in this shell saves the history first, as zsh's execute does.
  // Nothing is written without history to write (zsh's savehistfile
  // returns without a hist_ring).
  auto write_history = [&] {
    if (not save_history or not ctx.opts.get(jfsh::OPT_RCS)) return;
    // Histories pushed with fc -p are saved and popped first (zexit's
    // saveandpophiststack).
    while (history.stack_depth() > 0)
      if (not jfsh::hist_stack_pop(ctx)) break;
    if (history.size() == 0) return;
    auto hist_opts = jfsh::history_options(ctx);
    history.set_options(hist_opts);
    // Written line by line already, the history gets only what is left;
    // with INC_APPEND_HISTORY_TIME alone the line that is exiting never
    // finished, and is left out.
    if (hist_opts.inc_append or hist_opts.share) {
      if (ctx.hist_line and not hist_opts.share and not opts.get(jfsh::OPT_INCAPPENDHISTORY))
        history.mark_written(ctx.hist_line);
      (void)history.save(hist_opts.histfile, jfsh::HistSave::Incremental);
      return;
    }
    (void)history.save(hist_opts.histfile);
  };
  ctx.loop.save_history = write_history;

  // Exiting, as zsh's zexit does: the history is saved, a login shell reads
  // its logout files, then the EXIT trap and the zshexit hook run (neither
  // the logout files nor the hook after a trap's `exit` at the prompt).  $? in
  // the logout files is still the last command's; an `exit` there sets the
  // shell's status.
  auto finish = [&](int rc, bool ran_off = false) {
    if (ctx.exiting) rc = ctx.exit_val;
    write_history();
    save_history  = false;
    ctx.exit_val  = rc;
    if (not ctx.loop.exit_in_editor) jfsh::run_logout_files(executor, startup);
    rc = ctx.exit_val;
    ctx.last_exit = rc;
    int status = rc;
    rc = executor.run_exit_trap(rc, ran_off);
    ctx.exiting = false;
    // zshexit sees the exit status, not the EXIT trap's (dotrap puts
    // lastval back); after -t ended a script, zsh leaves without it
    // (realexit).
    if (not ctx.loop.single_done and not ctx.loop.exit_in_editor) {
      ctx.last_exit = status;
      jfsh::run_hook(executor, "zshexit");
    }
    s::fflush(nullptr);
    return rc;
  };

  if (ctx.exiting) return finish(ctx.exit_val);   // `exit` in a startup file
  ctx.lineno = 1;   // as zsh's setupshin, once there is input to number

  if (inv.command) {
    // %N and the like name the -c string as the shell (zsh's scriptname).
    ctx.source_name = ctx.script_name = "jfsh";
    if (opts.get(jfsh::OPT_VERBOSE)) s::fprintf(stderr, "%s\n", inv.command->c_str());
    int rc = executor.exec_string(*inv.command);
    return finish(rc, not ctx.exiting);
  }

  if (inv.script) {
    const str& script = *inv.script;
    // zsh's setupshin: the script as named, unless that is no file; then,
    // with PATH_SCRIPT and no `/` in the name, the first one in $path.
    str file = script;
    if (not is_file(file)) {
      file.clear();
      if (opts.get(jfsh::OPT_PATHSCRIPT) and script.find('/') == str::npos)
        if (auto p = params.lookup("path"); p and p->get().is_array())
          for (const auto& dir : p->get().array())
            if (str f = dir + "/" + script; is_file(f)) {
              file = s::move(f);
              break;
            }
    }
    opt<str> source = file.empty() ? s::nullopt : jfsh::read_file(file);
    if (not source) {
      s::fprintf(stderr, "%s: can't open input file: %s\n", inv.arg0.c_str(), script.c_str());
      return 127;
    }
    // $0 is the script from now on; the startup files saw the shell's.
    ctx.arg0 = script;
    return finish(executor.exec_string(*source, /*by_line=*/true));
  }

  if (interactive) {
    jfsh::PromptRenderer prompt_renderer(params, opts);
    prompt_renderer.set_hooks(executor.prompt_hooks());
    jfsh::Editor editor(opts, history, prompt_renderer, *ctx.zle);
    // Signals that arrive while a line is being typed: traps run, and with
    // NOTIFY finished or stopped jobs are reported at once, where the
    // prompt was; the editor draws it again below.
    ctx.zle->on_wake = [&](jfsh::Editor& ed, bool& quit) {
      // Traps run from zsh's signal handler, their output where the cursor
      // is; job notices come below the line (trashzle).
      bool printed = jfsh::Signals.pending_trapped() != 0;
      if (printed) ed.forget_display();
      auto& scope = ctx.zle->hooks.trap_scope;
      if (printed and scope) scope(ed, true);
      executor.handle_signals();
      if (printed and scope) scope(ed, false);
      // A trap run at the prompt that ended in an error, an interrupt or a
      // `return` doesn't reach the line being typed (zsh resets errflag
      // and retflag there).
      bool returned = executor.end_prompt_trap();
      if ((ctx.aborting or returned) and not ctx.exiting) {
        // (The trap's output ends a line of its own, as zsh's.)
        s::fputc('\n', stderr);
        printed = true;
        ctx.aborting = ctx.abort_line = ctx.interrupt = ctx.intr_echoed = false;
        ctx.end_trap_abort();
      }
      if (opts.monitor() and opts.get(jfsh::OPT_NOTIFY) and ctx.jobs.has_changes()) {
        if (not printed) ed.trash();
        ctx.jobs.notify_changes();
        printed = true;
      }
      s::fflush(nullptr);
      quit = ctx.exiting;
      if (quit) ctx.loop.exit_in_editor = true;
      return printed;
    };
    editor.set_wake(jfsh::Signals.wake_fd(),
                    [&](bool& quit) { return ctx.zle->on_wake(editor, quit); });

    // The interrupt key in the line editor (the prompt, PS2, isearch, vared)
    // acts as zsh's SIGINT handler there:
    // an ignored INT does nothing, a trapped one runs its trap (the signal
    // goes through the shell's own handler and wakes the editor), and only
    // an untrapped one ends the line.
    // A trap that fails (a TRAPINT returning non-zero) ends the line as an
    // interrupt would, with its status in $? (zsh's dotrap leaves errflag).
    ctx.zle->on_interrupt = [&](jfsh::Editor& ed) {
      auto action = jfsh::Signals.traps().get(SIGINT).action;
      if (action == jfsh::TrapAction::Ignore) return false;
      if (action != jfsh::TrapAction::Command and action != jfsh::TrapAction::Function)
        return true;
      // zsh runs the trap from its signal handler: its output goes where
      // the cursor is, the line staying; it is drawn again after.
      ed.forget_display();
      ed.suspend();
      ::kill(::getpid(), SIGINT);
      auto& scope = ctx.zle->hooks.trap_scope;
      if (scope) scope(ed, true);
      executor.handle_signals();
      if (scope) scope(ed, false);
      bool returned = executor.end_prompt_trap();
      s::fflush(nullptr);
      // An `exit` in the trap leaves at once (zsh's zexit from the
      // handler): the read ends and the loop goes no further.
      // (Under vared a command is running, and zsh's exit is the usual one.)
      if (ctx.exiting) {
        if (&ed == &editor) {
          ctx.loop.exit_in_editor = true;
          ed.quit();
        }
        return true;
      }
      ed.resume();
      if (not (ctx.aborting or returned)) return false;
      ctx.zle->trap_break = ctx.last_exit;
      ctx.aborting = ctx.abort_line = ctx.interrupt = ctx.intr_echoed = false;
      ctx.end_trap_abort();
      return true;
    };

    // CHECK_JOBS: the first attempt to exit with stopped (or, with
    // CHECK_RUNNING_JOBS, running) jobs only warns.  Exiting again right
    // away, or after `jobs`, goes ahead.
    bool warned        = false;
    bool prev_ran_jobs = false;
    int  line_no       = 1;   // the next line's number, for $LINENO
    int  eofs_ignored  = 0;   // Ctrl-Ds under IGNORE_EOF since the last command
    auto exit_blocked = [&] {
      if (ctx.hangup or warned or prev_ran_jobs) return false;
      if (not opts.get(jfsh::OPT_CHECKJOBS) or not jfsh::Signals.job_control()) return false;
      if (ctx.jobs.stopped_count() > 0) {
        s::fprintf(stderr, "jfsh: you have suspended jobs.\n");
      } else if (opts.get(jfsh::OPT_CHECKRUNNINGJOBS) and ctx.jobs.running_count() > 0) {
        s::fprintf(stderr, "jfsh: you have running jobs.\n");
      } else {
        return false;
      }
      warned = true;
      // The `exit` that didn't leave has status 0, as in zsh.
      ctx.last_exit = 0;
      return true;
    };

    for (;;) {
      // $LINENO is the next line's between commands, as zsh's lexer has it.
      if (not (ctx.exiting and ctx.exit_now)) ctx.lineno = static_cast<s::uint32_t>(line_no);
      // The terminal as the shell keeps it: what the last command left set
      // (`stty intr ^X`), with echo and canonical input back.
      jfsh::Signals.restore_shell_modes();
      // 1–2. Run traps, reap jobs and report the ones that changed.
      executor.handle_signals();
      if (opts.monitor()) ctx.jobs.notify_changes();
      ctx.aborting = false;
      ctx.end_trap_abort();
      (void)executor.end_prompt_trap();
      if (ctx.exiting) {   // SIGHUP, or an EXIT-ing trap
        if (not exit_blocked()) break;
        ctx.exiting = false;
      }
      // 3–4. precmd, periodic, sched, mail.
      jfsh::before_prompt(executor, s::time(nullptr));
      ctx.aborting = false;
      ctx.end_trap_abort();
      if (ctx.exiting) {
        if (not exit_blocked()) break;
        ctx.exiting = false;
      }

      // The history settings as they are now; with SHARE_HISTORY, what
      // other shells added.
      auto hist_opts = jfsh::history_options(ctx);
      history.set_options(hist_opts);
      if (hist_opts.share and not hist_opts.histfile.empty())
        (void)history.sync(hist_opts.histfile);

      // 5–6. The prompt, from $PS1 as it is now, and the line.
      prompt_renderer.reload();
      editor.set_prebuffer({});
      auto line_opt = editor.read_line();
      if (not line_opt) {   // EOF or hangup
        // IGNORE_EOF: the tenth Ctrl-D in a row exits (zsh's noexitct).
        if (not ctx.exiting and editor.eof_ignored() and ++eofs_ignored < 10) continue;
        if (ctx.exiting or not exit_blocked()) break;
        continue;
      }
      // The line was dropped: by the interrupt key, $? is 130 (zsh's SIGINT
      // handler sets lastval); by send-break, the parser's error leaves 1
      // unless $? already says something went wrong (zsh's loop).
      auto interrupted = [&] {
        if (ctx.exiting)              return;
        if (auto& tb = ctx.zle->trap_break; tb) ctx.last_exit = *s::exchange(tb, s::nullopt);
        else if (not editor.sent_break()) ctx.last_exit = 128 + SIGINT;
        else if (ctx.last_exit == 0)  ctx.last_exit = 1;
      };
      if (editor.interrupted()) {
        interrupted();
        continue;
      }

      str line = s::move(*line_opt);
      // $LINENO counts the lines read, as zsh's lexer does.
      int first_line = line_no++;
      if (line.empty()) continue;
      // An unfinished `if`, loop, quote or here-document goes on over the
      // next lines, read after PS2.  Ctrl-C there drops the whole command.
      bool comments = opts.get(jfsh::OPT_INTERACTIVECOMMENTS);
      // A line as it goes into the history: HIST_REDUCE_BLANKS closes up
      // the blanks between its words (zsh's histreduceblanks in hend).
      auto history_text = [&](const str& text) {
        const jfsh::AliasTable* aliases = opts.get(jfsh::OPT_ALIASES) ? &ctx.aliases : nullptr;
        return jfsh::HistoryManager::hend_text(
            text, hist_opts.reduce_blanks ? jfsh::word_spans(text, comments, aliases) : s::nullopt,
            hist_opts);
      };
      // Bang history, on each line as it is read (zsh's histsubchar, as
      // the lexer reads): a here-document's body is left alone, quotes
      // go on from line to line, and any line can be ^old^new.
      jfsh::HistExpansion expanded;
      jfsh::HistQuoting hist_quote;
      bool hist_failed = false;
      auto expand = [&](const str& text, s::string_view before) {
        auto r = history.expand_line(text, before, true, hist_quote);
        if (not r) {
          if (not r.error().message.empty())
            s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
          ctx.last_exit = 1;
          hist_failed = true;
          return text;
        }
        hist_quote          = s::move(r->quoting);
        expanded.changed    = expanded.changed or r->changed;
        expanded.print_only = expanded.print_only or r->print_only;
        return r->text;
      };
      str raw = line;
      line = expand(line, {});
      while (not hist_failed and jfsh::is_incomplete(line, comments)) {
        editor.set_prebuffer(line + "\n");
        auto more = editor.read_line(/*continuation=*/true);
        if (not more) {
          // $TMOUT ran out, or a hangup: the shell exits, the command
          // unread (zsh exits from the signal handler).
          if (ctx.exiting) line.clear();
          break;
        }
        if (editor.interrupted()) {
          interrupted();
          line.clear();
          break;
        }
        raw += '\n';
        raw += *more;
        str next = expand(*more, line + "\n");
        line += '\n';
        line += next;
        ++line_no;
      }
      if (ctx.exiting and line.empty()) break;
      if (hist_failed) continue;
      if (line.empty()) continue;
      // VERBOSE: the line as it was read, before history expansion.
      if (opts.get(jfsh::OPT_VERBOSE)) {
        s::fprintf(stderr, "%s\n", raw.c_str());
        s::fflush(stderr);
      }

      // An expanded line is shown before it runs; with HIST_VERIFY it
      // comes back to be edited instead, and :p only shows it (and keeps
      // it in the history).
      expanded.text = line;
      if (expanded.changed) {
        if (hist_opts.verify and not expanded.print_only) {
          ctx.zle->next_buffer = line;
          continue;
        }
        s::fprintf(stderr, "%s\n", line.c_str());
        if (expanded.print_only) {
          history.add(history_text(line));
          continue;
        }
      }
      // The line is in the history before it runs, as in zsh: `history`
      // and $HISTCMD see it.  Written at once (INC_APPEND_HISTORY,
      // SHARE_HISTORY) only with $SAVEHIST, as zsh's savehistfile.
      str stored = history_text(line);
      ctx.hist_line = history.add(stored);
      bool inc_append = not hist_opts.histfile.empty() and hist_opts.savehist > 0
          and (hist_opts.share or opts.get(jfsh::OPT_INCAPPENDHISTORY));
      if (inc_append and ctx.hist_line)
        (void)history.save(hist_opts.histfile, jfsh::HistSave::Incremental);
      s::time_t started = s::time(nullptr);

      // System 1 routing
      auto decision = router.classify(line);

      if (decision.is_agent()) {
        auto result = agent->handle(line,
            [](s::string_view chunk) {
              s::fwrite(chunk.data(), 1, chunk.size(), stdout);
              s::fflush(stdout);
            });
        if (not result) {
          s::fprintf(stderr, "jfsh: agent error: %s\n", result.error().message.c_str());
          ctx.last_exit = 1;
        } else {
          s::putchar('\n');
          ctx.last_exit = 0;
        }
      } else {
        // 7–8. The line is parsed, preexec sees it, then it runs; $? and
        // ZERR come from the executor.
        ctx.ran_jobs = false;
        if (auto prog = executor.parse_string(line, first_line); not prog) {
          executor.report_parse_error(prog.error());
          ctx.last_exit = 1;
        } else {
          // $1 is the line as read and history-expanded, as the history
          // has it (HIST_REDUCE_BLANKS): zsh keeps it in the ring while it
          // runs even when the history then drops it.
          jfsh::before_exec(executor, stored, **prog);
          // An `exit` leaves $? as it was, for the logout files.
          if (not ctx.exiting)
            if (int rc = executor.exec_parsed(**prog); not ctx.exiting) ctx.last_exit = rc;
        }
        // Ctrl-C: the terminal echoed ^C; start the prompt on a new line.
        if (s::exchange(ctx.intr_echoed, false)) s::fputc('\n', stderr);
      }
      // A line that ends the shell never finishes (zsh exits from it): the
      // exit leaves it as it is.
      if (ctx.hist_line and not ctx.exiting) {
        history.set_last_elapsed(static_cast<s::uint32_t>(s::time(nullptr) - started));
        // INC_APPEND_HISTORY_TIME: written once it is done, with its time.
        if (opts.get(jfsh::OPT_INCAPPENDHISTORYTIME) and not opts.get(jfsh::OPT_INCAPPENDHISTORY)
            and not hist_opts.share and not hist_opts.histfile.empty() and hist_opts.savehist > 0)
          (void)history.save(hist_opts.histfile, jfsh::HistSave::Incremental);
      }
      if (not ctx.exiting) ctx.hist_line = 0;

      if (ctx.exiting) {
        if (not exit_blocked()) break;
        ctx.exiting   = false;
        prev_ran_jobs = ctx.ran_jobs;
        continue;
      }
      warned        = false;
      prev_ran_jobs = ctx.ran_jobs;
      eofs_ignored  = 0;
      // -t: after one line the shell runs its EXIT trap and leaves at once
      // (zsh's loop: dotrap, realexit), without the rest of zexit.
      if (opts.get(jfsh::OPT_SINGLECOMMAND)) {
        int rc = executor.run_exit_trap(ctx.last_exit);
        s::fflush(nullptr);
        return rc;
      }
    }

    // HUP: jobs left running get SIGHUP (stopped ones too, with SIGCONT,
    // after a hangup).
    if (jfsh::Signals.job_control())
      if (int n = jfsh::hangup_jobs(ctx); n > 0)
        s::fprintf(stderr, "jfsh: warning: %d jobs SIGHUPed\n", n);
  } else {
    // Lines are gathered until they make a whole command, so constructs
    // spanning lines work.  As zsh's shingetline, fd 0 is read without
    // buffering, so what the script runs (`read`, `cat`, `exec <f`) gets
    // the lines after the current one: a file a block at a time, the rest
    // given back with lseek, a pipe a byte at a time.
    str pending;
    int  line_no = 1;   // the first line of `pending`, for $LINENO
    bool seekable = ::lseek(STDIN_FILENO, 0, SEEK_CUR) >= 0;
    auto get_line = [&](str& line) {
      line.clear();
      char b[4096];
      for (;;) {
        ssize_t n = ::read(STDIN_FILENO, b, seekable ? sizeof b : 1);
        if (n < 0 and errno == EINTR) continue;
        if (n <= 0) return not line.empty();
        auto len = static_cast<s::size_t>(n);
        if (const void* nl = s::memchr(b, '\n', len)) {
          s::size_t k = static_cast<s::size_t>(static_cast<const char*>(nl) - b) + 1;
          line.append(b, k);
          if (k < len) ::lseek(STDIN_FILENO, -static_cast<off_t>(len - k), SEEK_CUR);
          return true;
        }
        line.append(b, len);
      }
    };
    auto run = [&] {
      bool blank = pending.find_first_not_of(" \t\n") == str::npos;
      // Read as a script is (by_line), with its rules: an alias defined on
      // one line applies on the next, and a fatal error that leaves
      // status 0 exits with 1.
      (void)executor.take_top_return();   // not one a startup file left
      if (not blank) ctx.last_exit = executor.exec_string(pending, true, line_no);
      line_no += static_cast<int>(s::count(pending.begin(), pending.end(), '\n'));
      pending.clear();
      // Between commands $LINENO is where reading has got to, blank lines
      // included (an EXIT trap at the end sees the line after the last).
      if (not (ctx.exiting and ctx.exit_now)) ctx.lineno = static_cast<s::uint32_t>(line_no);
      // A `return` at the top ends the script, as it ends a file.
      return not ctx.exiting and not executor.take_top_return();
    };
    bool more = true;
    str line;
    while (more and get_line(line)) {
      pending += line;
      if (pending.back() != '\n' or jfsh::is_incomplete(pending)) continue;
      more = run();
    }
    if (more and not pending.empty()) run();
  }

  return finish(ctx.last_exit);
}
