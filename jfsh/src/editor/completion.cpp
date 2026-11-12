#include <cstring>
#include <dirent.h>

#include <algorithm>

#include <jfsh/editor/completion.h>

namespace jfsh {

CompletionEngine::CompletionEngine(const ParamTable& params)
  : mParams(params) {
  mDefaultFn = complete_files;
}

CompContext CompletionEngine::build_context(std::string_view line,
                                            std::size_t cursor) const {
  CompContext ctx;
  ctx.line   = std::string(line);
  ctx.cursor = cursor;
  std::size_t word_start = cursor;
  while (word_start > 0
         and line[word_start - 1] != ' '
         and line[word_start - 1] != '\t')
    --word_start;
  ctx.word = std::string(line.substr(word_start, cursor - word_start));
  return ctx;
}

std::vector<CompCandidate> CompletionEngine::complete(
    std::string_view line, std::size_t cursor) {
  auto ctx = build_context(line, cursor);
  std::string_view sv(line);
  auto space = sv.find(' ');
  ctx.command = space != std::string_view::npos
      ? std::string(sv.substr(0, space))
      : ctx.word;

  auto it = mCompdefTable.find(ctx.command);
  if (it != mCompdefTable.end()) return it->second(ctx);

  if (mDefaultFn) return mDefaultFn(ctx);
  return {};
}

void CompletionEngine::compdef(std::string command, CompletionFn fn) {
  mCompdefTable[std::move(command)] = std::move(fn);
}

std::vector<CompCandidate> CompletionEngine::complete_files(
    const CompContext& ctx) {
  std::vector<CompCandidate> cands;
  std::string prefix    = ctx.word;
  std::string dir_part  = ".";
  std::string file_part = prefix;
  auto slash = prefix.rfind('/');
  if (slash != std::string::npos) {
    dir_part  = prefix.substr(0, slash + 1);
    file_part = prefix.substr(slash + 1);
  }
  DIR* d = opendir(dir_part.c_str());
  if (not d) return cands;
  struct dirent* ent;
  while ((ent = readdir(d)) != nullptr) {
    std::string_view name(ent->d_name);
    if (name == "." or name == "..") continue;
    if (not file_part.empty()
        and name.substr(0, file_part.size()) != file_part)
      continue;
    CompCandidate c;
    c.insert  = dir_part == "."
        ? std::string(name)
        : dir_part + std::string(name);
    c.display = c.insert;
    cands.push_back(std::move(c));
  }
  closedir(d);
  std::sort(cands.begin(), cands.end(), [](const auto& a, const auto& b) {
    return a.insert < b.insert;
  });
  return cands;
}

std::vector<CompCandidate> CompletionEngine::complete_commands(
    const CompContext& ctx) {
  std::vector<CompCandidate> cands;
  const char* path_env = getenv("PATH");
  if (not path_env) return cands;
  std::string path(path_env);
  std::size_t start = 0;
  while (start < path.size()) {
    auto end = path.find(':', start);
    std::string dir = path.substr(
        start, end == std::string::npos ? std::string::npos : end - start);
    start = (end == std::string::npos) ? path.size() : end + 1;
    DIR* d = opendir(dir.c_str());
    if (not d) continue;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
      std::string_view name(ent->d_name);
      if (not ctx.word.empty()
          and name.substr(0, ctx.word.size()) != ctx.word)
        continue;
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
  if (not prefix.empty() and prefix[0] == '$') prefix = prefix.substr(1);
  params.for_each([&](const Param& p) {
    if (not prefix.empty() and p.name.substr(0, prefix.size()) != prefix)
      return;
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

}  // namespace jfsh
