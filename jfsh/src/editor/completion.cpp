#include <jfsh/editor/completion.hh>
#include <dirent.h>
#include <algorithm>
#include <cstring>

namespace jfsh {

CompletionEngine::CompletionEngine(const ParamTable& params)
    : params_(params) {
    default_fn_ = complete_files;
}

CompContext CompletionEngine::build_context(std::string_view line,
                                            std::size_t cursor) const {
    CompContext ctx;
    ctx.line   = std::string(line);
    ctx.cursor = cursor;
    // Extract current word
    std::size_t word_start = cursor;
    while (word_start > 0 && line[word_start - 1] != ' ' && line[word_start - 1] != '\t')
        --word_start;
    ctx.word = std::string(line.substr(word_start, cursor - word_start));
    return ctx;
}

std::vector<CompCandidate> CompletionEngine::complete(
    std::string_view line, std::size_t cursor) {
    auto ctx = build_context(line, cursor);
    // Find command name (first word)
    std::string_view sv(line);
    auto space = sv.find(' ');
    ctx.command = space != std::string_view::npos ? std::string(sv.substr(0, space)) : ctx.word;

    // Check compdef table
    auto it = compdef_table_.find(ctx.command);
    if (it != compdef_table_.end()) return it->second(ctx);

    // Default
    if (default_fn_) return default_fn_(ctx);
    return {};
}

void CompletionEngine::compdef(std::string command, CompletionFn fn) {
    compdef_table_[std::move(command)] = std::move(fn);
}

std::vector<CompCandidate> CompletionEngine::complete_files(const CompContext& ctx) {
    std::vector<CompCandidate> cands;
    std::string prefix = ctx.word;
    std::string dir_part = ".";
    std::string file_part = prefix;
    auto slash = prefix.rfind('/');
    if (slash != std::string::npos) {
        dir_part  = prefix.substr(0, slash + 1);
        file_part = prefix.substr(slash + 1);
    }
    DIR* d = opendir(dir_part.c_str());
    if (!d) return cands;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
        std::string_view name(ent->d_name);
        if (name == "." || name == "..") continue;
        if (!file_part.empty() && name.substr(0, file_part.size()) != file_part) continue;
        CompCandidate c;
        c.insert  = dir_part == "." ? std::string(name) : dir_part + std::string(name);
        c.display = c.insert;
        cands.push_back(std::move(c));
    }
    closedir(d);
    std::sort(cands.begin(), cands.end(), [](const auto& a, const auto& b){
        return a.insert < b.insert;
    });
    return cands;
}

std::vector<CompCandidate> CompletionEngine::complete_commands(const CompContext& ctx) {
    // Search PATH for matching executables
    std::vector<CompCandidate> cands;
    const char* path_env = getenv("PATH");
    if (!path_env) return cands;
    std::string path(path_env);
    std::size_t start = 0;
    while (start < path.size()) {
        auto end = path.find(':', start);
        std::string dir = path.substr(start, end == std::string::npos ? std::string::npos : end - start);
        start = (end == std::string::npos) ? path.size() : end + 1;
        DIR* d = opendir(dir.c_str());
        if (!d) continue;
        struct dirent* ent;
        while ((ent = readdir(d)) != nullptr) {
            std::string_view name(ent->d_name);
            if (!ctx.word.empty() && name.substr(0, ctx.word.size()) != ctx.word) continue;
            CompCandidate c;
            c.insert = c.display = std::string(name);
            cands.push_back(std::move(c));
        }
        closedir(d);
    }
    return cands;
}

std::vector<CompCandidate> CompletionEngine::complete_variables(
    const CompContext& ctx, const ParamTable& params) {
    std::vector<CompCandidate> cands;
    std::string prefix = ctx.word;
    if (!prefix.empty() && prefix[0] == '$') prefix = prefix.substr(1);
    params.for_each([&](const Param& p) {
        if (!prefix.empty() && p.name.substr(0, prefix.size()) != prefix) return;
        CompCandidate c;
        c.insert = "$" + p.name;
        c.display = c.insert;
        cands.push_back(std::move(c));
    });
    return cands;
}

std::vector<CompCandidate> CompletionEngine::complete_options(
    const CompContext&, std::string_view /*optstring*/) {
    return {};
}

} // namespace jfsh
