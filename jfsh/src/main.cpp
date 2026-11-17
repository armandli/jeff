#include <cerrno>
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
#include <jfsh/exec/executor.h>
#include <jfsh/exec/invocation.h>
#include <jfsh/exec/loop.h>
#include <jfsh/history/history.h>
#include <jfsh/lexer/lexer.h>
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
  if (interactive) executor.update_window_size();

  jfsh::StartupConfig startup;
  startup.zsh_compat = inv.zsh_compat_rc;
  jfsh::run_startup_files(executor, startup);
  ctx.aborting = false;

  // History, after the startup files have had their say on HISTFILE and
  // HISTSIZE.  Only an interactive shell keeps one.
  jfsh::HistOptions hist_opts;
  hist_opts.histsize = 1000;
  jfsh::HistoryManager history(hist_opts);
  bool save_history = false;
  if (interactive) {
    if (str n = param(params, "HISTSIZE"); not n.empty())
      hist_opts.histsize = static_cast<int>(s::clamp(s::strtol(n.c_str(), nullptr, 10), 1L, 1L << 30));
    hist_opts.histfile = param(params, "HISTFILE");
    history.set_options(hist_opts);
    if (not hist_opts.histfile.empty()) {
      (void)history.load(hist_opts.histfile);
      str keep = param(params, "SAVEHIST");
      save_history = keep.empty() or s::strtol(keep.c_str(), nullptr, 10) > 0;
    }
    ctx.history = &history;   // fc, history, r, $HISTCMD
  }

  // Exiting, as zsh's zexit does: the history is saved, a login shell reads
  // its logout files, then the EXIT trap and the zshexit hook run.
  auto finish = [&](int rc) {
    if (ctx.exiting) rc = ctx.exit_val;
    if (save_history) (void)history.save(hist_opts.histfile);
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
    jfsh::Editor editor(opts, history, prompt_renderer);
    editor.set_completer([](s::string_view) -> vec<str> {
      return {};  // completion engine wired in later
    });
    // Signals that arrive while a line is being typed: traps run, and with
    // NOTIFY finished or stopped jobs are reported at once.
    editor.set_wake(jfsh::Signals.wake_fd(), [&](bool& quit) {
      bool printed = jfsh::Signals.pending_trapped() != 0;
      if (printed) s::fputc('\n', stderr);
      executor.handle_signals();
      if (opts.monitor() and opts.get(jfsh::OPT_NOTIFY) and ctx.jobs.has_changes()) {
        if (not printed) s::fputc('\n', stderr);
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

      // 5–6. The prompt, from $PS1 as it is now, and the line.
      prompt_renderer.reload();
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

      auto expanded = history.expand(line);
      if (not expanded) {
        s::fprintf(stderr, "jfsh: %s\n", expanded.error().message.c_str());
        continue;
      }
      line = s::move(*expanded);
      // The line is in the history before it runs, as in zsh: `history`
      // and $HISTCMD see it, and `r` replaces it with what it runs.
      ctx.hist_current = history.add(line) != 0;

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
