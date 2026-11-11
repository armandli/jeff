#include <cstring>
#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>

#include <jfsh/glob/glob.hh>

namespace jfsh {

GlobPattern GlobPattern::compile(std::string_view pat, GlobFlags flags) {
  GlobPattern g;
  g.raw   = std::string(pat);
  g.flags = flags;
  return g;
}

bool has_glob_chars(std::string_view s) noexcept {
  bool escaped = false;
  for (char c : s) {
    if (escaped) { escaped = false; continue; }
    if (c == '\\') { escaped = true; continue; }
    if (c == '*' or c == '?' or c == '[') return true;
  }
  return false;
}

// fnmatch-style: * any sequence, ? one char, [...] ranges
bool pattern_match(std::string_view pattern, std::string_view str,
                   bool case_insensitive, bool /*extended*/) noexcept {
  // Minimal recursive implementation
  if (pattern.empty()) return str.empty();
  if (pattern[0] == '*') {
    for (std::size_t i = 0; i <= str.size(); ++i)
      if (pattern_match(pattern.substr(1), str.substr(i), case_insensitive, false))
        return true;
    return false;
  }
  if (str.empty()) return false;
  char p = pattern[0], s = str[0];
  if (case_insensitive) {
    p = static_cast<char>(std::tolower(static_cast<unsigned char>(p)));
    s = static_cast<char>(std::tolower(static_cast<unsigned char>(s)));
  }
  if (p == '?' or p == s)
    return pattern_match(pattern.substr(1), str.substr(1), case_insensitive, false);
  return false;
}

bool ext_pattern_match(std::string_view pattern, std::string_view str) noexcept {
  return pattern_match(pattern, str, false, true);
}

Result<std::vector<std::string>> glob_expand(
    const GlobPattern& pat,
    const OptionState& opts)
{
  if (not has_glob_chars(pat.raw)) return std::vector<std::string>{pat.raw};

  std::string dir_part  = ".";
  std::string file_part = pat.raw;
  auto slash = pat.raw.rfind('/');
  if (slash != std::string::npos) {
    dir_part  = pat.raw.substr(0, slash);
    file_part = pat.raw.substr(slash + 1);
  }

  std::vector<std::string> matches;
  DIR* d = opendir(dir_part.c_str());
  if (not d) {
    if (opts.nomatch())
      return std::unexpected(ShellError{ErrorKind::BadPattern,
          "no matches for: " + pat.raw});
    return std::vector<std::string>{pat.raw};
  }
  struct dirent* ent;
  bool dot_ok = (pat.flags & GLOB_DOTFILES) or opts.get(OPT_GLOBDOTS);
  while ((ent = readdir(d)) != nullptr) {
    std::string_view name(ent->d_name);
    if (name == "." or name == "..") continue;
    if (not dot_ok
        and not file_part.empty()
        and file_part[0] != '.'
        and name[0] == '.')
      continue;
    if (pattern_match(file_part, name, not opts.get(OPT_CASEGLOB), false)) {
      if (dir_part == ".") matches.emplace_back(name);
      else                 matches.push_back(dir_part + "/" + std::string(name));
    }
  }
  closedir(d);

  if (matches.empty()) {
    if (opts.nullglob()) return std::vector<std::string>{};
    if (opts.nomatch())
      return std::unexpected(ShellError{ErrorKind::BadPattern,
          "no matches for: " + pat.raw});
    return std::vector<std::string>{pat.raw};
  }
  std::sort(matches.begin(), matches.end());
  return matches;
}

Result<std::vector<std::string>> expand_args(
    std::vector<std::string> args,
    const OptionState&       opts)
{
  std::vector<std::string> result;
  for (auto& arg : args) {
    if (not has_glob_chars(arg)) {
      result.push_back(std::move(arg));
      continue;
    }
    auto pat = GlobPattern::compile(arg);
    auto expanded = glob_expand(pat, opts);
    if (not expanded) return std::unexpected(expanded.error());
    for (auto& m : *expanded) result.push_back(std::move(m));
  }
  return result;
}

}  // namespace jfsh
