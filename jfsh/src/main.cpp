#include <cerrno>
#include <clocale>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <vector>

#include <jfsh/agent/agent.h>
#include <jfsh/agent/provider.h>
#include <jfsh/core/aliases.h>
#include <jfsh/core/jobs.h>
#include <jfsh/core/options.h>
#include <jfsh/core/param.h>
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

str param(jfsh::ParamTable& params, s::string_view name) {
  auto p = params.lookup(name);
  return (p and p->get().is_set()) ? p->get().scalar() : str{};
}

}  // namespace

int main(int argc, char* argv[]) {
  // The line editor measures characters with wcwidth().
  std::setlocale(LC_CTYPE, "");
  // Globs and ${(o)a} sort by the locale's collation, as in zsh.
  std::setlocale(LC_COLLATE, "");
  auto parsed = jfsh::parse_invocation(vec<str>(argv, argv + argc));
  if (not parsed) {
    s::fprintf(stderr, "jfsh: %s\n", parsed.error().message.c_str());
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

  // Interactive, unless told otherwise, when commands come from a terminal.
  bool interactive = inv.interactive.value_or(
      inv.shinstdin and isatty(STDIN_FILENO) and isatty(STDOUT_FILENO));

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
  opts.assign(jfsh::OPT_MONITOR,       inv.monitor.value_or(interactive));
  opts.assign(jfsh::OPT_ZLE,           inv.zle.value_or(interactive));
  opts.assign(jfsh::OPT_LOGIN,         inv.login);
  opts.assign(jfsh::OPT_RCS,           inv.rcs);
  opts.assign(jfsh::OPT_GLOBALRCS,     inv.global_rcs);
  opts.assign(jfsh::OPT_SINGLECOMMAND, false);
  for (auto [idx, on] : inv.options) opts.assign(idx, on);

  for (char** ep = environ; ep and *ep; ++ep) {
    str env(*ep);
    auto eq = env.find('=');
    if (eq == str::npos) continue;
    params.assign(env.substr(0, eq), env.substr(eq + 1),
                  jfsh::PM_SCALAR | jfsh::PM_EXPORTED);
  }

  jfsh::Signals.init(interactive);

  jfsh::Router router;
  load_router(router, inv);
  auto agent = jfsh::Agent::create(provider_config(inv));

  jfsh::ExecContext ctx{params, opts, jfsh::Jobs, jfsh::Signals, 0, -1};
  ctx.arg0       = inv.arg0;
  ctx.positional = inv.positional;
  jfsh::Executor executor(ctx);
  jfsh::init_shell_params(ctx);

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
  (void)ctx.modules.load("zsh/datetime",  &ctx);
  (void)ctx.modules.load("zsh/mathfunc",  &ctx);
  (void)ctx.modules.load("zsh/parameter", &ctx);
  (void)ctx.modules.load("zsh/zutil",     &ctx);
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

  // zsh/newuser (M11): an interactive shell with no user startup files
  // offers to write a starter ~/.jfshrc first, so the one it writes is
  // read straight away.
  if (interactive and ctx.opts.get(jfsh::OPT_RCS) and ::isatty(STDIN_FILENO)
      and jfsh::newuser_needed(ctx) and ctx.modules.load("zsh/newuser", &ctx))
    (void)executor.run_command({"zsh-newuser-install"});

  jfsh::StartupConfig startup;
  startup.zsh_compat = inv.zsh_compat_rc;
  jfsh::run_startup_files(executor, startup);
  ctx.aborting = false;

  // History, after the startup files have had their say on HISTFILE,
  // HISTSIZE and the history options.  Only an interactive shell keeps
  // one.
  jfsh::HistoryManager history(jfsh::history_options(ctx));
  bool save_history = interactive;   // to $HISTFILE as it is on the way out
  if (interactive) {
    auto hist_opts = jfsh::history_options(ctx);
    if (not hist_opts.histfile.empty()) (void)history.load(hist_opts.histfile);
    ctx.history = &history;   // fc, history, r, $HISTCMD
  }

  // Exiting, as zsh's zexit does: the history is saved, a login shell reads
  // its logout files, then the EXIT trap and the zshexit hook run.
  auto finish = [&](int rc) {
    if (ctx.exiting) rc = ctx.exit_val;
    if (save_history) {
      auto hist_opts = jfsh::history_options(ctx);
      history.set_options(hist_opts);
      (void)history.save(hist_opts.histfile);
    }
    ctx.last_exit = rc;
    jfsh::run_logout_files(executor, startup);
    if (ctx.exiting) rc = ctx.exit_val;
    rc = executor.run_exit_trap(rc);
    ctx.exiting = false;
    jfsh::run_hook(executor, "zshexit");
    s::fflush(nullptr);
    return rc;
  };

  if (ctx.exiting) return finish(ctx.exit_val);   // `exit` in a startup file

  if (inv.command) return finish(executor.exec_string(*inv.command));

  if (not inv.script.empty()) {
    s::FILE* f = s::fopen(inv.script.c_str(), "r");
    if (not f) {
      s::fprintf(stderr, "jfsh: can't open input file: %s\n", inv.script.c_str());
      return 127;
    }
    str source;
    char buf[4096];
    s::size_t n;
    while ((n = s::fread(buf, 1, sizeof buf, f)) > 0) source.append(buf, n);
    s::fclose(f);
    return finish(executor.exec_string(source, /*by_line=*/true));
  }

  if (interactive) {
    jfsh::PromptRenderer prompt_renderer(params, opts);
    prompt_renderer.set_hooks(executor.prompt_hooks());
    jfsh::Editor editor(opts, history, prompt_renderer, *ctx.zle);
    // Signals that arrive while a line is being typed: traps run, and with
    // NOTIFY finished or stopped jobs are reported at once, where the
    // prompt was; the editor draws it again below.
    editor.set_wake(jfsh::Signals.wake_fd(), [&](bool& quit) {
      bool printed = jfsh::Signals.pending_trapped() != 0;
      if (printed) editor.trash();
      executor.handle_signals();
      if (opts.monitor() and opts.get(jfsh::OPT_NOTIFY) and ctx.jobs.has_changes()) {
        if (not printed) editor.trash();
        ctx.jobs.notify_changes();
        printed = true;
      }
      s::fflush(nullptr);
      quit = ctx.exiting;
      return printed;
    });

    // CHECK_JOBS: the first attempt to exit with stopped (or, with
    // CHECK_RUNNING_JOBS, running) jobs only warns.  Exiting again right
    // away, or after `jobs`, goes ahead.
    bool warned        = false;
    bool prev_ran_jobs = false;
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
      return true;
    };

    for (;;) {
      // 1–2. Run traps, reap jobs and report the ones that changed.
      executor.handle_signals();
      if (opts.monitor()) ctx.jobs.notify_changes();
      ctx.aborting = false;
      if (ctx.exiting) {   // SIGHUP, or an EXIT-ing trap
        if (not exit_blocked()) break;
        ctx.exiting = false;
      }
      // 3–4. precmd, periodic, sched, mail.
      jfsh::before_prompt(executor, s::time(nullptr));
      ctx.aborting = false;
      if (ctx.exiting) {
        if (not exit_blocked()) break;
        ctx.exiting = false;
      }

      // The history settings as they are now; with SHARE_HISTORY, what
      // other shells added.
      auto hist_opts = jfsh::history_options(ctx);
      history.set_options(hist_opts);
      if (hist_opts.share and not hist_opts.histfile.empty()) (void)history.sync(hist_opts.histfile);

      // 5–6. The prompt, from $PS1 as it is now, and the line.
      prompt_renderer.reload();
      editor.set_prebuffer({});
      auto line_opt = editor.read_line();
      if (not line_opt) {   // EOF or hangup
        if (ctx.exiting or not exit_blocked()) break;
        continue;
      }
      if (editor.interrupted()) continue;

      str line = s::move(*line_opt);
      if (line.empty()) continue;
      // An unfinished `if`, loop, quote or here-document goes on over the
      // next lines, read after PS2.  Ctrl-C there drops the whole command.
      bool comments = opts.get(jfsh::OPT_INTERACTIVECOMMENTS);
      while (jfsh::is_incomplete(line, comments)) {
        editor.set_prebuffer(line + "\n");
        auto more = editor.read_line(/*continuation=*/true);
        if (not more) break;
        if (editor.interrupted()) {
          line.clear();
          break;
        }
        line += '\n';
        line += *more;
      }
      if (line.empty()) continue;

      // Bang history.  An expanded line is shown before it runs; with
      // HIST_VERIFY it comes back to be edited instead, and :p only shows
      // it (and keeps it in the history).
      auto expanded = history.expand_line(line);
      if (not expanded) {
        s::fprintf(stderr, "jfsh: %s\n", expanded.error().message.c_str());
        ctx.last_exit = 1;
        continue;
      }
      if (expanded->changed) {
        line = s::move(expanded->text);
        if (hist_opts.verify and not expanded->print_only) {
          ctx.zle->next_buffer = line;
          continue;
        }
        s::fprintf(stderr, "%s\n", line.c_str());
        if (expanded->print_only) {
          history.add(line);
          continue;
        }
      }
      // The line is in the history before it runs, as in zsh: `history`
      // and $HISTCMD see it, and `r` replaces it with what it runs.
      ctx.hist_current = history.add(line) != 0;
      bool inc_append = not hist_opts.histfile.empty()
          and (hist_opts.share or opts.get(jfsh::OPT_INCAPPENDHISTORY));
      if (inc_append and ctx.hist_current) (void)history.save(hist_opts.histfile, jfsh::HistSave::Append);
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
        // 7–8. preexec, then the line; $? and ZERR come from the executor.
        jfsh::before_exec(executor, line);
        ctx.ran_jobs = false;
        if (not ctx.exiting) ctx.last_exit = executor.exec_string(line);
        // Ctrl-C: the terminal echoed ^C; start the prompt on a new line.
        if (ctx.aborting) s::fputc('\n', stderr);
      }
      if (ctx.hist_current) {
        history.set_last_elapsed(static_cast<s::uint32_t>(s::time(nullptr) - started));
        // INC_APPEND_HISTORY_TIME: written once it is done, with its time.
        if (opts.get(jfsh::OPT_INCAPPENDHISTORYTIME) and not opts.get(jfsh::OPT_INCAPPENDHISTORY)
            and not hist_opts.share and not hist_opts.histfile.empty())
          (void)history.save(hist_opts.histfile, jfsh::HistSave::Append);
      }
      ctx.hist_current = false;

      if (ctx.exiting) {
        if (not exit_blocked()) break;
        ctx.exiting   = false;
        prev_ran_jobs = ctx.ran_jobs;
        continue;
      }
      warned        = false;
      prev_ran_jobs = ctx.ran_jobs;
    }

    // HUP: jobs left running get SIGHUP (stopped ones SIGCONT too).
    if (jfsh::Signals.job_control())
      if (int n = jfsh::hangup_jobs(ctx); n > 0)
        s::fprintf(stderr, "jfsh: warning: %d jobs SIGHUPed\n", n);
  } else {
    // Lines are gathered until they make a whole command, so constructs
    // spanning lines work, and so do lines longer than the buffer.
    str pending;
    char buf[4096];
    auto run = [&] {
      bool blank = pending.find_first_not_of(" \t\n") == str::npos;
      if (not blank) ctx.last_exit = executor.exec_string(pending);
      pending.clear();
      return not ctx.exiting;
    };
    bool more = true;
    while (more and s::fgets(buf, sizeof buf, stdin)) {
      pending += buf;
      if (pending.back() != '\n' or jfsh::is_incomplete(pending)) continue;
      more = run();
    }
    if (more and not pending.empty()) run();
  }

  return finish(ctx.last_exit);
}
