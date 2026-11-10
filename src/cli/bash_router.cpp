// bash_router — manually test the exp_005 bash shell router.
//
// Sends a typed line to python/experiments/exp_005_shell_router/infer.py, which applies
// the exp_005 decision (bash -n cascade, critical-command guard, P(RUN) threshold), and
// displays the result.
//
// Usage:
//   bash_router [OPTIONS] TEXT...
//   bash_router [OPTIONS] --text TEXT
//   bash_router --help
//
// Environment:
//   JEFF_REPO_ROOT   repo root (default: the source dir this binary was built from)
//
// Default model: <repo_root>/checkpoints/exp_005_shell_router/best

#include "router_common.hpp"

namespace fs = std::filesystem;

static void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [OPTIONS] TEXT...\n"
        << "       " << prog << " [OPTIONS] --text TEXT\n\n"
        << "Options:\n"
        << "  -m, --model PATH     checkpoint dir  (default: checkpoints/exp_005_shell_router/best)\n"
        << "  --threshold P        RUN when P(RUN) >= P  (default: 0.5)\n"
        << "  --raw                model only: skip the bash -n check and the critical-command guard\n"
        << "  -v, --verbose        print the context, options and command sent to the model\n"
        << "  -h, --help           show this message\n"
        << "  --                   everything after this is TEXT, even if it starts with '-'\n\n"
        << "Options are read only before TEXT starts; the rest of the line is TEXT.\n\n"
        << "Environment:\n"
        << "  JEFF_REPO_ROOT  repo root dir\n\n"
        << "Examples:\n"
        << "  " << prog << " ls -la ~/Downloads\n"
        << "  " << prog << " 'how do I undo my last commit'\n"
        << "  " << prog << " --model checkpoints/exp_005_shell_router_v2/best 'git status'\n"
        << "  " << prog << " -- -la\n";
}

int main(int argc, char* argv[]) {
    router::Args a;
    if (!router::parse_args(argc, argv, /*accept_model_key=*/false, a)) {
        usage(argv[0]);
        return 2;
    }
    if (a.help) {
        usage(argv[0]);
        return 0;
    }
    if (a.text.empty()) {
        std::cerr << "Error: no text provided.\n\n";
        usage(argv[0]);
        return 2;
    }

    std::string root = router::repo_root();
    if (a.model_path.empty())
        a.model_path = (fs::path(root) / "checkpoints/exp_005_shell_router/best").string();

    fs::path infer = fs::path(root) / "python/experiments/exp_005_shell_router/infer.py";
    if (!fs::exists(infer)) {
        std::cerr << "Error: inference script not found: " << infer << "\n"
                  << "       Set JEFF_REPO_ROOT to the repo root.\n";
        return 1;
    }
    if (!fs::exists(a.model_path)) {
        std::cerr << "Error: model checkpoint not found: " << a.model_path << "\n"
                  << "       Train first: python python/experiments/exp_005_shell_router/train.py\n";
        return 1;
    }

    std::string cmd = router::build_command(
        router::find_python(root), infer,
        {{"model", a.model_path}, {"threshold", a.threshold}, {"text", a.text}},
        a.raw, a.verbose);
    if (a.verbose)
        std::cerr << "=== command ===\n" << cmd << "\n\n";

    std::string json = router::run_infer(cmd);
    if (json.empty()) return 1;

    int critical = router::json_bool(json, "critical");
    router::print_result(a.text, json, "bash -n:    ",
        {{"Critical:   ", critical == 1 ? "YES (guard forces AGENT)" : critical == 0 ? "no" : "skipped"}});
    return 0;
}
