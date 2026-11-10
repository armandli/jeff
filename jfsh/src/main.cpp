#include <jfsh/core/types.hh>
#include <jfsh/core/param.hh>
#include <jfsh/core/options.hh>
#include <jfsh/core/jobs.hh>
#include <jfsh/core/signals.hh>
#include <jfsh/lexer/lexer.hh>
#include <jfsh/parser/parser.hh>
#include <jfsh/exec/executor.hh>
#include <jfsh/history/history.hh>
#include <jfsh/prompt/prompt.hh>
#include <jfsh/editor/editor.hh>
#include <jfsh/router/router.hh>
#include <jfsh/agent/agent.hh>
#include <jfsh/agent/provider.hh>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

static void print_usage(const char* prog) {
    std::fprintf(stderr,
        "Usage: %s [-c command] [-s] [-i] [--norouter] [--model DIR] [script [args...]]\n"
        "Options:\n"
        "  -c CMD        execute CMD and exit\n"
        "  -s            read commands from stdin\n"
        "  -i            force interactive mode\n"
        "  --norouter    disable AI routing (all commands run as shell)\n"
        "  --model DIR   router model checkpoint directory\n"
        "  --provider P  LLM provider: openai|anthropic|ollama|openrouter\n"
        "  --api-key K   LLM API key\n"
        "  --llm-model M LLM model name\n",
        prog);
}

int main(int argc, char* argv[]) {
    // ---------------------------------------------------------------------------
    // Argument parsing
    // ---------------------------------------------------------------------------
    bool        interactive  = isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    bool        norouter     = false;
    std::string eval_cmd;
    std::string script_path;
    std::string model_dir;
    std::string provider_name;
    std::string api_key;
    std::string llm_model;

    for (int i = 1; i < argc; ++i) {
        std::string_view arg(argv[i]);
        if (arg == "-c" && i + 1 < argc) {
            eval_cmd    = argv[++i];
            interactive = false;
        } else if (arg == "-i") {
            interactive = true;
        } else if (arg == "-s") {
            interactive = false;
        } else if (arg == "--norouter") {
            norouter = true;
        } else if (arg == "--model" && i + 1 < argc) {
            model_dir = argv[++i];
        } else if (arg == "--provider" && i + 1 < argc) {
            provider_name = argv[++i];
        } else if (arg == "--api-key" && i + 1 < argc) {
            api_key = argv[++i];
        } else if (arg == "--llm-model" && i + 1 < argc) {
            llm_model = argv[++i];
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            return 0;
        } else if (arg[0] != '-' && script_path.empty()) {
            script_path = std::string(arg);
            interactive = false;
        }
    }

    // ---------------------------------------------------------------------------
    // Shell state initialisation
    // ---------------------------------------------------------------------------
    jfsh::ParamTable  params;
    jfsh::OptionState opts;

    opts.assign(jfsh::OPT_STDIN,    interactive);
    opts.assign(jfsh::OPT_MONITOR,  interactive);
    opts.assign(jfsh::OPT_ALIASES,  true);

    // Seed params from environment
    for (char** ep = environ; ep && *ep; ++ep) {
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

    // ---------------------------------------------------------------------------
    // Router setup
    // ---------------------------------------------------------------------------
    jfsh::Router router;
    router.set_bypass(norouter);
    if (!model_dir.empty()) router.load_model(model_dir);

    // ---------------------------------------------------------------------------
    // Agent setup
    // ---------------------------------------------------------------------------
    jfsh::ProviderConfig provider_cfg = jfsh::ProviderConfig::from_env();
    if (!provider_name.empty()) {
        if (provider_name == "openai")      provider_cfg.kind = jfsh::ProviderKind::OpenAI;
        else if (provider_name == "anthropic") provider_cfg.kind = jfsh::ProviderKind::Anthropic;
        else if (provider_name == "openrouter") provider_cfg.kind = jfsh::ProviderKind::OpenRouter;
        else                                provider_cfg.kind = jfsh::ProviderKind::Ollama;
    }
    if (!api_key.empty())   provider_cfg.api_key = api_key;
    if (!llm_model.empty()) provider_cfg.model   = llm_model;

    auto agent = jfsh::Agent::create(provider_cfg);

    // ---------------------------------------------------------------------------
    // Executor context
    // ---------------------------------------------------------------------------
    jfsh::ExecContext ctx{params, opts, jfsh::g_jobs, jfsh::g_signals, 0, -1};
    jfsh::Executor executor(ctx);

    // ---------------------------------------------------------------------------
    // -c CMD mode
    // ---------------------------------------------------------------------------
    if (!eval_cmd.empty()) {
        return executor.exec_string(eval_cmd);
    }

    // ---------------------------------------------------------------------------
    // Script file mode
    // ---------------------------------------------------------------------------
    if (!script_path.empty()) {
        std::FILE* f = std::fopen(script_path.c_str(), "r");
        if (!f) {
            std::fprintf(stderr, "jfsh: %s: %s\n", script_path.c_str(), std::strerror(errno));
            return 1;
        }
        // Read and exec entire file
        std::string source;
        char buf[4096];
        while (std::fgets(buf, sizeof buf, f)) source += buf;
        std::fclose(f);
        return executor.exec_string(source);
    }

    // ---------------------------------------------------------------------------
    // Interactive / stdin REPL
    // ---------------------------------------------------------------------------
    jfsh::HistOptions hist_opts;
    hist_opts.histsize = 1000;
    const char* histfile = std::getenv("HISTFILE");
    if (histfile) hist_opts.histfile = histfile;

    jfsh::HistoryManager history(hist_opts);
    if (!hist_opts.histfile.empty()) history.load(hist_opts.histfile);

    jfsh::PromptRenderer prompt_renderer(params, opts);

    // Choose editor or plain getline
    if (interactive) {
        jfsh::Editor editor(opts, history, prompt_renderer);
        editor.set_completer([](std::string_view prefix) -> std::vector<std::string> {
            return {};  // completion engine wired in later
        });

        for (;;) {
            auto line_opt = editor.read_line();
            if (!line_opt) break;  // EOF

            std::string line = std::move(*line_opt);
            if (line.empty()) continue;

            // History expansion
            auto expanded = history.expand(line);
            if (!expanded) {
                std::fprintf(stderr, "jfsh: %s\n", expanded.error().message.c_str());
                continue;
            }
            line = std::move(*expanded);

            // System 1 routing
            auto decision = router.classify(line);

            if (decision.is_agent()) {
                // Forward to LLM
                auto result = agent->handle(line,
                    [](std::string_view chunk) {
                        std::fwrite(chunk.data(), 1, chunk.size(), stdout);
                        std::fflush(stdout);
                    });
                if (!result) {
                    std::fprintf(stderr, "jfsh: agent error: %s\n",
                                 result.error().message.c_str());
                    ctx.last_exit = 1;
                } else {
                    std::putchar('\n');
                    ctx.last_exit = 0;
                }
            } else {
                // Execute as shell command
                ctx.last_exit = executor.exec_string(line);
                if (opts.xtrace()) {
                    std::fprintf(stderr, "+ %s\n", line.c_str());
                }
                if (opts.errexit() && ctx.last_exit != 0) break;
            }

            history.add(line);

            jfsh::g_jobs.notify_changes();

            if (ctx.exiting) break;
        }
    } else {
        // Non-interactive: read from stdin, execute line by line
        std::string line;
        char buf[4096];
        while (std::fgets(buf, sizeof buf, stdin)) {
            line = buf;
            // Strip trailing newline
            if (!line.empty() && line.back() == '\n') line.pop_back();
            if (line.empty()) continue;
            ctx.last_exit = executor.exec_string(line);
            if (opts.errexit() && ctx.last_exit != 0) break;
            if (ctx.exiting) break;
        }
    }

    // ---------------------------------------------------------------------------
    // Cleanup
    // ---------------------------------------------------------------------------
    if (!hist_opts.histfile.empty()) history.save(hist_opts.histfile);
    jfsh::g_signals.traps().run_exit_trap();

    return ctx.exiting ? ctx.exit_val : ctx.last_exit;
}
