// Shared plumbing for the bash_router / zsh_router manual-test CLIs.
//
// The prompt context, option strings, parse check, safety guard and threshold all live
// in the Python infer.py scripts (they import jeff.data.*), so the CLIs never hold a
// second copy that could drift from what the model was trained on.

#pragma once

#include <array>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "json.h"

namespace router {

namespace fs = std::filesystem;

// POSIX single-quote escaping: safe for every byte except NUL.
inline std::string shell_quote(const std::string& s) {
    std::string out = "'";
    for (char c : s) {
        if (c == '\'') out += "'\"'\"'";
        else out += c;
    }
    return out + "'";
}

struct ProcResult {
    std::string out;
    int exit_code;
};

// Runs cmd through /bin/sh, captures stdout (stderr passes through to the terminal).
inline ProcResult popen_read(const std::string& cmd) {
    FILE* pipe = popen(cmd.c_str(), "r");
    if (!pipe)
        throw std::runtime_error("popen failed");
    std::string result;
    std::array<char, 512> buf{};
    while (fgets(buf.data(), static_cast<int>(buf.size()), pipe))
        result += buf.data();
    int status = pclose(pipe);
    int code = (status != -1 && WIFEXITED(status)) ? WEXITSTATUS(status) : 1;
    return {result, code};
}

// The last non-empty line: anything a library prints to stdout earlier is ignored.
inline std::string last_line(const std::string& s) {
    std::string line, last;
    for (char c : s) {
        if (c == '\n') {
            if (!line.empty()) last = line;
            line.clear();
        } else if (c != '\r') {
            line += c;
        }
    }
    return line.empty() ? last : line;
}

// Field readers for the one-line JSON object that infer.py prints with json.dumps.
inline std::string json_str(const jeff::json::Value& json, std::string_view key) {
    return json.get_string(key);
}

inline double json_num(const jeff::json::Value& json, std::string_view key, double dflt = 0.0) {
    return json.get_double(key, dflt);
}

// Returns 1 for true, 0 for false, -1 for null or missing.
inline int json_bool(const jeff::json::Value& json, std::string_view key) {
    auto b = json.get_bool(key);
    return b ? static_cast<int>(*b) : -1;
}

inline std::string repo_root() {
    if (const char* env = std::getenv("JEFF_REPO_ROOT"); env && *env) return env;
#ifdef JEFF_SOURCE_DIR
    return JEFF_SOURCE_DIR;
#else
    return fs::current_path().string();
#endif
}

inline std::string find_python(const std::string& root) {
    fs::path venv = fs::path(root) / "python/.venv/bin/python";
    return fs::exists(venv) ? venv.string() : "python3";
}

// Common command-line options. Unknown options are rejected only before the text
// starts: after the first word of TEXT (or after "--") every argument is text, so
// `bash_router ls -la ~/Downloads` works without quoting.
struct Args {
    std::string model_path;
    std::string model_key;     // zsh_router only
    std::string threshold;     // passed through verbatim; infer.py validates it
    std::string text;
    bool raw = false;
    bool verbose = false;
    bool help = false;
};

// Returns false (after printing a message) on a usage error.
inline bool parse_args(int argc, char* argv[], bool accept_model_key, Args& a) {
    bool text_started = false;
    auto append = [&](const std::string& s) {
        if (!a.text.empty()) a.text += ' ';
        a.text += s;
        text_started = true;
    };
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (text_started) {
            append(arg);
            continue;
        }
        auto value = [&](std::string& dst) {
            if (i + 1 >= argc) {
                std::cerr << "Error: " << arg << " needs a value.\n\n";
                return false;
            }
            dst = argv[++i];
            return true;
        };
        if (arg == "--") {
            text_started = true;
        } else if (arg == "--model" || arg == "-m") {
            if (!value(a.model_path)) return false;
        } else if (accept_model_key && arg == "--model-key") {
            if (!value(a.model_key)) return false;
        } else if (arg == "--threshold") {
            if (!value(a.threshold)) return false;
        } else if (arg == "--text" || arg == "-t") {
            std::string t;
            if (!value(t)) return false;
            append(t);
        } else if (arg == "--raw") {
            a.raw = true;
        } else if (arg == "--verbose" || arg == "-v") {
            a.verbose = true;
        } else if (arg == "--help" || arg == "-h") {
            a.help = true;
        } else if (!arg.empty() && arg[0] == '-') {
            std::cerr << "Error: unknown option " << arg
                      << " (put \"--\" before text that starts with '-')\n\n";
            return false;
        } else {
            append(arg);
        }
    }
    return true;
}

// Builds the infer.py command line. --opt=VALUE keeps argparse from reading a text
// such as "-la" or "--help" as an option.
inline std::string build_command(const std::string& python, const fs::path& infer,
                                 const std::vector<std::pair<std::string, std::string>>& kv,
                                 bool raw, bool verbose) {
    std::string cmd = shell_quote(python) + " " + shell_quote(infer.string());
    for (const auto& [k, v] : kv)
        if (!v.empty()) cmd += " " + shell_quote("--" + k + "=" + v);
    if (raw) cmd += " --raw";
    if (verbose) cmd += " --verbose";
    return cmd;
}

// Runs infer.py and returns its parsed JSON line, or nullopt after printing an error.
inline std::optional<jeff::json::Value> run_infer(const std::string& cmd) {
    ProcResult r;
    try {
        r = popen_read(cmd);
    } catch (const std::exception& e) {
        std::cerr << "Error: " << e.what() << "\n";
        return std::nullopt;
    }
    std::string line = last_line(r.out);
    auto parsed = jeff::json::parse(line);
    if (!parsed || !parsed.value.is_object()) {
        std::cerr << "Inference failed (exit " << r.exit_code << ")"
                  << (r.out.empty() ? std::string{} : ":\n" + r.out) << "\n";
        return std::nullopt;
    }
    const jeff::json::Value& json = parsed.value;
    if (std::string err = json_str(json, "error"); !err.empty() || r.exit_code != 0) {
        std::cerr << "Inference failed (exit " << r.exit_code << "): "
                  << (err.empty() ? line : err) << "\n";
        return std::nullopt;
    }
    if (std::string d = json_str(json, "decision"); d != "RUN" && d != "AGENT") {
        std::cerr << "Inference returned no decision: " << line << "\n";
        return std::nullopt;
    }
    return std::move(parsed.value);
}

// Prints the verdict. `extra` lines are appended as label/value pairs.
inline void print_result(const std::string& text, const jeff::json::Value& json,
                         const std::string& shell_check_label,
                         const std::vector<std::pair<std::string, std::string>>& extra) {
    std::string decision = json_str(json, "decision");
    std::string reason   = json_str(json, "reason");
    double p_run     = json_num(json, "p_run");
    double p_agent   = json_num(json, "p_agent");
    double threshold = json_num(json, "threshold", 0.5);
    int parses       = json_bool(json, "parses");

    const bool tty    = isatty(fileno(stdout));
    const char* col   = !tty ? "" : (decision == "RUN") ? "\033[32m" : "\033[33m";
    const char* reset = tty ? "\033[0m" : "";

    std::cout << "\n"
              << "  Text:       " << text << "\n"
              << "  Decision:   " << col << decision << reset;
    if (!reason.empty()) std::cout << "  (" << reason << ")";
    std::cout << "\n" << std::fixed
              << "  P(RUN):     " << std::setprecision(4) << p_run
              << "   threshold " << std::setprecision(4) << threshold << "\n"
              << "  P(AGENT):   " << std::setprecision(4) << p_agent << "\n"
              << "  " << shell_check_label
              << (parses == 1 ? "ok" : parses == 0 ? "FAILED" : "skipped") << "\n";
    for (const auto& [label, value] : extra)
        std::cout << "  " << label << value << "\n";
    std::cout << "\n";
}

}  // namespace router
