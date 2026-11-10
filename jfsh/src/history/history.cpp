#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>

#include <jfsh/history/history.hh>

namespace jfsh {

HistoryManager::HistoryManager(HistOptions opts) : mOpts(std::move(opts)) {}

bool HistoryManager::should_suppress(std::string_view text) const noexcept {
  if (text.empty()) return true;
  if (mOpts.ignore_space and text[0] == ' ') return true;
  if (mOpts.no_store) return false;  // "history" suppressed by no_store flag
  if (mOpts.ignore_dups
      and not mEntries.empty()
      and mEntries.back().text == text)
    return true;
  if (mOpts.ignore_all) {
    for (auto const& e : mEntries)
      if (e.text == text) return true;
  }
  return false;
}

void HistoryManager::trim_to_limit() {
  while (static_cast<int>(mEntries.size()) > mOpts.histsize
         and not mEntries.empty())
    mEntries.erase(mEntries.begin());
}

std::uint32_t HistoryManager::add(std::string text, std::uint32_t elapsed) {
  if (mOpts.reduce_blanks) {
    std::string trimmed;
    bool last_ws = false;
    for (char c : text) {
      bool ws = c == ' ' or c == '\t';
      if (not ws or not last_ws) trimmed += c;
      last_ws = ws;
    }
    text = std::move(trimmed);
  }
  if (should_suppress(text)) return 0;
  HistEntry e;
  e.num       = mNextNum++;
  e.text      = std::move(text);
  e.timestamp = std::time(nullptr);
  e.elapsed   = elapsed;
  // Remove duplicates if HIST_IGNORE_ALL_DUPS
  if (mOpts.ignore_all) {
    mEntries.erase(
        std::remove_if(mEntries.begin(), mEntries.end(),
                       [&](const HistEntry& x){ return x.text == e.text; }),
        mEntries.end());
  }
  mEntries.push_back(std::move(e));
  trim_to_limit();
  return mEntries.back().num;
}

Result<std::string> HistoryManager::expand(std::string_view line) {
  if (line.empty() or line[0] != '!') return std::string(line);
  if (line == "!!") {
    if (mEntries.empty())
      return std::unexpected(ShellError{ErrorKind::ExecError,
          "no previous command"});
    return mEntries.back().text;
  }
  // TODO(hist/expand): Implement full zsh bang-history expansion:
  //   !N      → entry number N
  //   !-N     → Nth entry from end
  //   !str    → most recent entry starting with str
  //   !?str   → most recent entry containing str
  //   !#      → current command line so far
  //   Word designators: !N:0 (command), !N:1 (first arg), !N:^ !N:$ !N:* !N:2-4
  //   Modifiers: :h (head/dirname), :t (tail/basename), :r (remove ext),
  //              :e (extension), :q (quote), :x (quote each word),
  //              :s/old/new/ (substitute), :gs/old/new/ (global substitute),
  //              :& (repeat last s), :u (uppercase), :l (lowercase), :p (print, no exec)
  //   HIST_SUBST_PATTERN option: use pattern matching instead of literal for :s.
  return std::string(line);
}

std::optional<std::reference_wrapper<const HistEntry>>
HistoryManager::lookup(int num) const noexcept {
  if (num > 0) {
    for (auto const& e : mEntries)
      if (static_cast<int>(e.num) == num) return std::cref(e);
  } else {
    int idx = static_cast<int>(mEntries.size()) + num;
    if (idx >= 0) return std::cref(mEntries[static_cast<std::size_t>(idx)]);
  }
  return std::nullopt;
}

std::optional<std::reference_wrapper<const HistEntry>>
HistoryManager::search(std::string_view substr) const noexcept {
  for (auto it = mEntries.rbegin(); it != mEntries.rend(); ++it)
    if (it->text.find(substr) != std::string::npos) return std::cref(*it);
  return std::nullopt;
}

Result<void> HistoryManager::load(std::string_view path) {
  std::ifstream f{std::string(path)};
  if (not f) return {};  // file not found is not an error on first run
  std::string line;
  while (std::getline(f, line)) {
    if (not line.empty()) {
      HistEntry e;
      e.num  = mNextNum++;
      e.text = std::move(line);
      mEntries.push_back(std::move(e));
    }
  }
  trim_to_limit();
  return {};
}

Result<void> HistoryManager::save(std::string_view path) {
  std::ofstream f{std::string(path),
      mOpts.append ? std::ios::app : std::ios::trunc};
  if (not f)
    return std::unexpected(ShellError{ErrorKind::FileNotFound,
        "cannot open histfile"});
  for (auto const& e : mEntries)
    f << e.text << '\n';
  return {};
}

Result<void> HistoryManager::sync(std::string_view path) {
  return load(path);
}

}  // namespace jfsh
