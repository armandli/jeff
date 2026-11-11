#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include <jfsh/agent/agent.hh>
#include <jfsh/agent/provider.hh>
#include <jfsh/router/jeff.hh>
#include <jfsh/router/jev.hh>
#include <jfsh/core/jobs.hh>
#include <jfsh/core/options.hh>
#include <jfsh/core/param.hh>
#include <jfsh/core/signals.hh>
#include <jfsh/core/types.hh>
#include <jfsh/editor/editor.hh>
#include <jfsh/exec/executor.hh>
#include <jfsh/history/history.hh>
#include <jfsh/lexer/lexer.hh>
#include <jfsh/parser/parser.hh>
#include <jfsh/prompt/prompt.hh>
#include <jfsh/router/router.hh>

static void print_usage(const char* prog) {
  std::fprintf(stderr,
      "Usage: %s [-c command] [-s] [-i] [--norouter] [script [args...]]\n"
      "Options:\n"
      "  -c CMD              execute CMD and exit\n"
      "  -s                  read commands from stdin\n"
      "  -i                  force interactive mode\n"
      "  --norouter          disable AI routing (all commands run as shell)\n"
      "  --router R          System 1 router: jeff|jev|heuristic\n"
      "  --jeff-url URL      jeff-server base URL (default: http://127.0.0.1:8765)\n"
      "  --typesafe-key K    TypeSafe API key for JEV router\n"
      "  --jev-model M       JEV model name (default: jev-latest)\n"
      "  --provider P        LLM provider: openai|anthropic|ollama|openrouter\n"
      "  --api-key K         LLM API key\n"
      "  --llm-model M       LLM model name\n",
      prog);
}

int main(int argc, char* argv[]) {
  bool        interactive    = isatty(STDIN_FILENO) and isatty(STDOUT_FILENO);
  bool        norouter       = false;
  std::string eval_cmd;
  std::string script_path;
  std::string provider_name;
  std::string api_key;
  std::string llm_model;
  std::string router_kind;    // "jeff" | "jev" | "heuristic" | ""
  std::string jeff_url;       // overrides JeffConfig::base_url
  std::string typesafe_key;
  std::string jev_model;

  for (int i = 1; i < argc; ++i) {
    std::string_view arg(argv[i]);
    if (arg == "-c" and i + 1 < argc) {
      eval_cmd    = argv[++i];
      interactive = false;
    } else if (arg == "-i") {
      interactive = true;
    } else if (arg == "-s") {
      interactive = false;
    } else if (arg == "--norouter") {
      norouter = true;
    } else if (arg == "--provider" and i + 1 < argc) {
      provider_name = argv[++i];
    } else if (arg == "--api-key" and i + 1 < argc) {
      api_key = argv[++i];
    } else if (arg == "--llm-model" and i + 1 < argc) {
      llm_model = argv[++i];
    } else if (arg == "--router" and i + 1 < argc) {
      router_kind = argv[++i];
    } else if (arg == "--jeff-url" and i + 1 < argc) {
      jeff_url = argv[++i];
    } else if (arg == "--typesafe-key" and i + 1 < argc) {
      typesafe_key = argv[++i];
    } else if (arg == "--jev-model" and i + 1 < argc) {
      jev_model = argv[++i];
    } else if (arg == "--help" or arg == "-h") {
      print_usage(argv[0]);
      return 0;
    } else if (arg[0] != '-' and script_path.empty()) {
      script_path = std::string(arg);
      interactive = false;
    }
  }

  jfsh::ParamTable  params;
  jfsh::OptionState opts;

  opts.assign(jfsh::OPT_STDIN,   interactive);
  opts.assign(jfsh::OPT_MONITOR, interactive);
  opts.assign(jfsh::OPT_ALIASES, true);

  for (char** ep = environ; ep and *ep; ++ep) {
    std::string env(*ep);
    auto eq = env.find('=');
    if (eq == std::string::npos) continue;
    jfsh::Param& p = params.assign(
        env.substr(0, eq),
        env.substr(eq + 1),
        jfsh::PM_SCALAR | jfsh::PM_EXPORTED);
    (void)p;
  }

  jfsh::g_signals.init(interactive);

  jfsh::Router router;
  router.set_bypass(norouter);

  // Determine which System 1 backend to use.
  //   --router jeff       → local jeff-server HTTP client (default port 8765)
  //   --router jev        → TypeSafe JEV cloud API
  //   --router heuristic  → zsh -n heuristic (no service required)
  //   Auto-detect: --typesafe-key → jev; --jeff-url → jeff; else heuristic
  bool use_jev  = (router_kind == "jev") or
                  (router_kind.empty() and not typesafe_key.empty());
  bool use_jeff = (router_kind == "jeff") or
                  (router_kind.empty() and not jeff_url.empty() and not use_jev);

  if (use_jev) {
    jfsh::JevConfig jev_cfg = jfsh::JevConfig::from_env();
    if (not typesafe_key.empty()) jev_cfg.api_key = typesafe_key;
    if (not jev_model.empty())    jev_cfg.model   = jev_model;
    router.load_jev(std::move(jev_cfg));
  } else if (use_jeff) {
    jfsh::JeffConfig jeff_cfg;
    if (not jeff_url.empty()) jeff_cfg.base_url = jeff_url;
    router.load_jeff(std::move(jeff_cfg));
  }

  jfsh::ProviderConfig provider_cfg = jfsh::ProviderConfig::from_env();
  if (not provider_name.empty()) {
    if (provider_name == "openai")
      provider_cfg.kind = jfsh::ProviderKind::OpenAI;
    else if (provider_name == "anthropic")
      provider_cfg.kind = jfsh::ProviderKind::Anthropic;
    else if (provider_name == "openrouter")
      provider_cfg.kind = jfsh::ProviderKind::OpenRouter;
    else
      provider_cfg.kind = jfsh::ProviderKind::Ollama;
  }
  if (not api_key.empty())   provider_cfg.api_key = api_key;
  if (not llm_model.empty()) provider_cfg.model   = llm_model;

  auto agent = jfsh::Agent::create(provider_cfg);

  jfsh::ExecContext ctx{params, opts, jfsh::g_jobs, jfsh::g_signals, 0, -1};
  jfsh::Executor executor(ctx);

  if (not eval_cmd.empty()) {
    return executor.exec_string(eval_cmd);
  }

  if (not script_path.empty()) {
    std::FILE* f = std::fopen(script_path.c_str(), "r");
    if (not f) {
      std::fprintf(stderr, "jfsh: %s: %s\n",
                   script_path.c_str(), std::strerror(errno));
      return 1;
    }
    std::string source;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, f)) source += buf;
    std::fclose(f);
    return executor.exec_string(source);
  }

  jfsh::HistOptions hist_opts;
  hist_opts.histsize = 1000;
  const char* histfile = std::getenv("HISTFILE");
  if (histfile) hist_opts.histfile = histfile;

  jfsh::HistoryManager history(hist_opts);
  if (not hist_opts.histfile.empty()) history.load(hist_opts.histfile);

  jfsh::PromptRenderer prompt_renderer(params, opts);

  if (interactive) {
    jfsh::Editor editor(opts, history, prompt_renderer);
    editor.set_completer([](std::string_view prefix) -> std::vector<std::string> {
      return {};  // completion engine wired in later
    });

    for (;;) {
      auto line_opt = editor.read_line();
      if (not line_opt) break;  // EOF

      std::string line = std::move(*line_opt);
      if (line.empty()) continue;

      auto expanded = history.expand(line);
      if (not expanded) {
        std::fprintf(stderr, "jfsh: %s\n", expanded.error().message.c_str());
        continue;
      }
      line = std::move(*expanded);

      // System 1 routing
      auto decision = router.classify(line);

      if (decision.is_agent()) {
        auto result = agent->handle(line,
            [](std::string_view chunk) {
              std::fwrite(chunk.data(), 1, chunk.size(), stdout);
              std::fflush(stdout);
            });
        if (not result) {
          std::fprintf(stderr, "jfsh: agent error: %s\n",
                       result.error().message.c_str());
          ctx.last_exit = 1;
        } else {
          std::putchar('\n');
          ctx.last_exit = 0;
        }
      } else {
        ctx.last_exit = executor.exec_string(line);
        if (opts.xtrace()) {
          std::fprintf(stderr, "+ %s\n", line.c_str());
        }
        if (opts.errexit() and ctx.last_exit != 0) break;
      }

      history.add(line);

      jfsh::g_jobs.notify_changes();

      if (ctx.exiting) break;
    }
  } else {
    std::string line;
    char buf[4096];
    while (std::fgets(buf, sizeof buf, stdin)) {
      line = buf;
      if (not line.empty() and line.back() == '\n') line.pop_back();
      if (line.empty()) continue;
      ctx.last_exit = executor.exec_string(line);
      if (opts.errexit() and ctx.last_exit != 0) break;
      if (ctx.exiting) break;
    }
  }

  if (not hist_opts.histfile.empty()) history.save(hist_opts.histfile);
  jfsh::g_signals.traps().run_exit_trap();

  return ctx.exiting ? ctx.exit_val : ctx.last_exit;
}
