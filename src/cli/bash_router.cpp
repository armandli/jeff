// bash_router — manually test the exp_005 bash shell router.
//
// Wraps a typed string into the exp_005 decision context, delegates inference
// to python/experiments/exp_005_shell_router/infer.py, and displays the result.
//
// Usage:
//   bash_router [--model PATH] [--verbose] TEXT...
//   bash_router [--model PATH] [--verbose] --text TEXT
//   bash_router --help
//
// Environment:
//   JEFF_REPO_ROOT   repo root (default: current working directory)
//
// Default model: <repo_root>/checkpoints/exp_005_shell_router/best

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <stdexcept>
#include <string>

namespace fs = std::filesystem;

// ---- context mirrors shell_router.py::make_context() --------------------

static std::string make_context(const std::string& text) {
    return "A user typed the following line at an interactive bash prompt. "
           "Decide whether it is a valid bash command that could run as written "
           "on a typical Linux system, ignoring whether specific files or installed "
           "tools exist. Natural language, code in other languages, broken syntax, "
           "misspelled commands or subcommands, nonexistent flags and missing "
           "required arguments are not valid.\n\nTyped line: " + text;
}

static constexpr std::array<const char*, 2> kOptions = {
    "RUN: it is a valid bash command, so execute it in the shell",
    "AGENT: it is not a valid bash command, so send it to the AI agent as a prompt",
};

// ---- helpers ----------------------------------------------------------------

// POSIX single-quote escaping: safe for all printable characters.
static std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\"'\"'";
        else out += c;
    }
    return out + "'";
}

static std::string popen_read(const std::string& cmd) {
    auto closer = [](FILE* f) { if (f) pclose(f); };
    std::unique_ptr<FILE, decltype(closer)> pipe(popen(cmd.c_str(), "r"), closer);
    if (!pipe)
        throw std::runtime_error("popen failed");
    std::string result;
    std::array<char, 512> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe.get()))
        result += buf.data();
    return result;
}

// Extract a string field from flat single-line JSON: "key": "VALUE"
static std::string json_str(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\": \"";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return {};
    pos += needle.size();
    auto end = json.find('"', pos);
    return (end == std::string::npos) ? std::string{} : json.substr(pos, end - pos);
}

// Extract a numeric field from flat single-line JSON: "key": NUMBER
static double json_num(const std::string& json, const std::string& key) {
    std::string needle = "\"" + key + "\": ";
    auto pos = json.find(needle);
    if (pos == std::string::npos) return 0.0;
    pos += needle.size();
    try { return std::stod(json.substr(pos)); } catch (...) { return 0.0; }
}

static std::string repo_root() {
    if (const char* env = std::getenv("JEFF_REPO_ROOT")) return env;
    return fs::current_path().string();
}

static std::string find_python(const std::string& root) {
    fs::path venv = fs::path(root) / "python/.venv/bin/python";
    return fs::exists(venv) ? venv.string() : "python3";
}

// ---- usage ------------------------------------------------------------------

static void usage(const char* prog) {
    std::cerr
        << "Usage: " << prog << " [--model PATH] [--verbose] TEXT...\n"
        << "       " << prog << " [--model PATH] [--verbose] --text TEXT\n\n"
        << "Options:\n"
        << "  --model PATH   checkpoint dir  (default: checkpoints/exp_005_shell_router/best)\n"
        << "  --verbose      print the full context and options sent to the model\n"
        << "  --help         show this message\n\n"
        << "Environment:\n"
        << "  JEFF_REPO_ROOT  repo root dir  (default: current working directory)\n\n"
        << "Examples:\n"
        << "  " << prog << " 'ls -la ~/Downloads'\n"
        << "  " << prog << " 'how do I undo my last commit'\n"
        << "  " << prog << " --model /path/to/checkpoint 'git status'\n";
}

// ---- main -------------------------------------------------------------------

int main(int argc, char* argv[]) {
    std::string model_path;
    std::string text;
    bool verbose = false;

    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if ((arg == "--model" || arg == "-m") && i + 1 < argc) {
            model_path = argv[++i];
        } else if ((arg == "--text" || arg == "-t") && i + 1 < argc) {
            text = argv[++i];
        } else if (arg == "--verbose" || arg == "-v") {
            verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            usage(argv[0]);
            return 0;
        } else if (!arg.empty() && arg[0] != '-') {
            if (!text.empty()) text += ' ';
            text += arg;
        } else {
            std::cerr << "Unknown argument: " << arg << "\n";
            usage(argv[0]);
            return 1;
        }
    }

    if (text.empty()) {
        std::cerr << "Error: no text provided.\n\n";
        usage(argv[0]);
        return 1;
    }

    std::string root = repo_root();
    if (model_path.empty())
        model_path = root + "/checkpoints/exp_005_shell_router/best";

    fs::path infer = fs::path(root) / "python/experiments/exp_005_shell_router/infer.py";
    if (!fs::exists(infer)) {
        std::cerr << "Error: inference script not found: " << infer << "\n"
                  << "       Set JEFF_REPO_ROOT or run from the repo root.\n";
        return 1;
    }
    if (!fs::exists(model_path)) {
        std::cerr << "Error: model checkpoint not found: " << model_path << "\n"
                  << "       Train first: python python/experiments/exp_005_shell_router/train.py\n";
        return 1;
    }

    if (verbose) {
        std::cerr << "\n=== context ===\n" << make_context(text) << "\n\n"
                  << "=== options ===\n";
        for (size_t i = 0; i < kOptions.size(); ++i)
            std::cerr << "  [" << i << "] " << kOptions[i] << "\n";
        std::cerr << "\n";
    }

    std::string python = find_python(root);
    std::string cmd = python + " " + shell_quote(infer.string())
        + " --model " + shell_quote(model_path)
        + " --text "  + shell_quote(text);

    if (verbose)
        std::cerr << "=== command ===\n" << cmd << "\n\n";

    std::string out;
    try {
        out = popen_read(cmd);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return 1;
    }
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r' || out.back() == ' '))
        out.pop_back();

    if (out.empty() || out.front() != '{') {
        std::cerr << "Inference failed:\n" << out << "\n";
        return 1;
    }

    std::string decision = json_str(out, "decision");
    double p_run   = json_num(out, "p_run");
    double p_agent = json_num(out, "p_agent");
    double conf    = (decision == "RUN") ? p_run : p_agent;

    const char* col   = (decision == "RUN") ? "\033[32m" : "\033[33m";
    const char* reset = "\033[0m";

    std::cout << "\n"
              << "  Text:     " << text << "\n"
              << "  Decision: " << col << decision << reset
              << "  (" << std::fixed << std::setprecision(1) << (conf * 100.0) << "%)\n"
              << "  P(RUN):   " << std::setprecision(4) << p_run   << "\n"
              << "  P(AGENT): " << std::setprecision(4) << p_agent << "\n\n";
    return 0;
}
