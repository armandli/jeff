// zsh_router — manually test the exp_006 zsh shell router.
//
// Sends a typed line to python/experiments/exp_006_zsh_router/infer.py, which applies
// the exp_006 decision (zsh -n cascade, P(RUN) threshold), and displays the result.
//
// Usage:
//   zsh_router [OPTIONS] TEXT...
//   zsh_router [OPTIONS] --text TEXT
//   zsh_router --help
//
// Environment:
//   JEFF_REPO_ROOT   repo root (default: the source dir this binary was built from)
//
// Default model:     <repo_root>/checkpoints/exp_006_zsh_router/<model-key>/best
// Default model-key: modernbert_base
// Available keys:    modernbert_base | modernbert_large | roberta_base

#include "router_common.hpp"

namespace fs = std::filesystem;

static void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [OPTIONS] TEXT...\n"
        << "       " << prog << " [OPTIONS] --text TEXT\n\n"
        << "Options:\n"
        << "  -m, --model PATH     checkpoint dir  (default: checkpoints/exp_006_zsh_router/<key>/best)\n"
        << "  --model-key KEY      model variant; must match the checkpoint  (default: modernbert_base)\n"
        << "                       choices: modernbert_base | modernbert_large | roberta_base\n"
        << "  --threshold P        RUN when P(RUN) >= P  (default: 0.5)\n"
        << "  --raw                model only: skip the zsh -n check\n"
        << "  -v, --verbose        print the context, options and command sent to the model\n"
        << "  -h, --help           show this message\n"
        << "  --                   everything after this is TEXT, even if it starts with '-'\n\n"
        << "Options are read only before TEXT starts; the rest of the line is TEXT.\n\n"
        << "Environment:\n"
        << "  JEFF_REPO_ROOT  repo root dir\n\n"
        << "Examples:\n"
        << "  " << prog << " ls '**/*.py'\n"
        << "  " << prog << " setopt extendedglob\n"
        << "  " << prog << " 'how do I write a zsh function'\n"
        << "  " << prog << " --model-key modernbert_large autoload -U compinit\n";
}

int main(int argc, char* argv[]) {
    router::Args a;
    a.model_key = "modernbert_base";
    if (!router::parse_args(argc, argv, /*accept_model_key=*/true, a)) {
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
        a.model_path = (fs::path(root) / "checkpoints/exp_006_zsh_router" / a.model_key / "best").string();

    fs::path infer = fs::path(root) / "python/experiments/exp_006_zsh_router/infer.py";
    if (!fs::exists(infer)) {
        std::cerr << "Error: inference script not found: " << infer << "\n"
                  << "       Set JEFF_REPO_ROOT to the repo root.\n";
        return 1;
    }
    if (!fs::exists(a.model_path)) {
        std::cerr << "Error: model checkpoint not found: " << a.model_path << "\n"
                  << "       Train first: python python/experiments/exp_006_zsh_router/train.py"
                  << " --model-key " << a.model_key << "\n";
        return 1;
    }

    std::string cmd = router::build_command(
        router::find_python(root), infer,
        {{"model", a.model_path}, {"model-key", a.model_key},
         {"threshold", a.threshold}, {"text", a.text}},
        a.raw, a.verbose);
    if (a.verbose)
        std::cerr << "=== command ===\n" << cmd << "\n\n";

    auto json = router::run_infer(cmd);
    if (!json) return 1;

    std::string model_name = router::json_str(*json, "model_name");
    router::print_result(a.text, *json, "zsh -n:     ",
        {{"Model:      ", a.model_key + (model_name.empty() ? "" : "  (" + model_name + ")")}});
    return 0;
}
