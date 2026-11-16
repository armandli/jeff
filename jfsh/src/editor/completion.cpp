#include <jfsh/editor/completion.h>

#include <cstring>
#include <dirent.h>

#include <algorithm>

#include <jfsh/core/aliases.h>

namespace jfsh {

CompContext CompletionEngine::build_context(strv line,
                                            s::size_t cursor) const {
  CompContext ctx;
  ctx.line   = str(line);
  ctx.cursor = cursor;
  s::size_t word_start = cursor;
  while (word_start > 0
         and line[word_start - 1] != ' '
         and line[word_start - 1] != '\t')
    --word_start;
  ctx.word = str(line.substr(word_start, cursor - word_start));
  return ctx;
}

vec<CompCandidate> CompletionEngine::complete(
    strv line, s::size_t cursor) {
  auto ctx = build_context(line, cursor);
  strv sv(line);
  auto space = sv.find(' ');
  ctx.command = space != strv::npos
      ? str(sv.substr(0, space))
      : ctx.word;

  auto it = mCompdefTable.find(ctx.command);
  if (it != mCompdefTable.end()) return it->second(ctx);

  if (mDefaultFn) return mDefaultFn(ctx);
  return {};
}

vec<CompCandidate> CompletionEngine::complete_files(
    const CompContext& ctx) {
  vec<CompCandidate> cands;
  str prefix    = ctx.word;
  str dir_part  = ".";
  str file_part = prefix;
  auto slash = prefix.rfind('/');
  if (slash != str::npos) {
    dir_part  = prefix.substr(0, slash + 1);
    file_part = prefix.substr(slash + 1);
  }
  DIR* d = opendir(dir_part.c_str());
  if (not d) return cands;
  struct dirent* ent;
  while ((ent = readdir(d)) != nullptr) {
    strv name(ent->d_name);
    if (name == "." or name == "..") continue;
    if (not file_part.empty()
        and name.substr(0, file_part.size()) != file_part)
      continue;
    CompCandidate c;
    c.insert  = dir_part == "."
        ? str(name)
        : dir_part + str(name);
    c.display = c.insert;
    cands.push_back(s::move(c));
  }
  closedir(d);
  s::sort(cands.begin(), cands.end(), [](const auto& a, const auto& b) {
    return a.insert < b.insert;
  });
  return cands;
}

vec<CompCandidate> CompletionEngine::complete_commands(
    const CompContext& ctx) {
  vec<CompCandidate> cands;
  const char* path_env = getenv("PATH");
  if (not path_env) return cands;
  str path(path_env);
  s::size_t start = 0;
  while (start < path.size()) {
    auto end = path.find(':', start);
    str dir = path.substr(
        start, end == str::npos ? str::npos : end - start);
    start = (end == str::npos) ? path.size() : end + 1;
    DIR* d = opendir(dir.c_str());
    if (not d) continue;
    struct dirent* ent;
    while ((ent = readdir(d)) != nullptr) {
      strv name(ent->d_name);
      if (not ctx.word.empty()
          and name.substr(0, ctx.word.size()) != ctx.word)
        continue;
      CompCandidate c;
      c.insert = c.display = str(name);
      cands.push_back(s::move(c));
    }
    closedir(d);
  }
  return cands;
}

vec<CompCandidate> CompletionEngine::complete_variables(
    const CompContext& ctx, const ParamTable& params) {
  vec<CompCandidate> cands;
  str prefix = ctx.word;
  if (not prefix.empty() and prefix[0] == '$') prefix = prefix.substr(1);
  params.for_each([&](const Param& p) {
    if (not prefix.empty() and p.name.substr(0, prefix.size()) != prefix)
      return;
    CompCandidate c;
    c.insert = "$" + p.name;
    c.display = c.insert;
    cands.push_back(s::move(c));
  });
  return cands;
}


}  // namespace jfsh
