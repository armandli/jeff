#include <jfsh/history/history.h>

#include <ctime>

#include <algorithm>
#include <fstream>
#include <sstream>

#include <jfsh/core/aliases.h>

namespace jfsh {

bool HistoryManager::should_suppress(strv text) const noexcept {
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

s::uint32_t HistoryManager::add(str text, s::uint32_t elapsed) {
  if (mOpts.reduce_blanks) {
    str trimmed;
    bool last_ws = false;
    for (char c : text) {
      bool ws = c == ' ' or c == '\t';
      if (not ws or not last_ws) trimmed += c;
      last_ws = ws;
    }
    text = s::move(trimmed);
  }
  if (should_suppress(text)) return 0;
  HistEntry e;
  e.num       = mNextNum++;
  e.text      = s::move(text);
  e.timestamp = s::time(nullptr);
  e.elapsed   = elapsed;
  // Remove duplicates if HIST_IGNORE_ALL_DUPS
  if (mOpts.ignore_all) {
    mEntries.erase(
        s::remove_if(mEntries.begin(), mEntries.end(),
                     [&](const HistEntry& x){ return x.text == e.text; }),
        mEntries.end());
  }
  mEntries.push_back(s::move(e));
  trim_to_limit();
  return mEntries.back().num;
}

Result<str> HistoryManager::expand(strv line) {
  if (line.empty() or line[0] != '!') return str(line);
  if (line == "!!") {
    if (mEntries.empty())
      return s::unexpected(ShellError{ErrorKind::ExecError,
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
  return str(line);
}

optref<const HistEntry> HistoryManager::lookup(int num) const noexcept {
  if (num > 0) {
    for (auto const& e : mEntries)
      if (static_cast<int>(e.num) == num) return s::cref(e);
  } else {
    // -1 is the last entry; 0 is none (B6).
    int idx = static_cast<int>(mEntries.size()) + num;
    if (num < 0 and idx >= 0) return s::cref(mEntries[static_cast<s::size_t>(idx)]);
  }
  return s::nullopt;
}

optref<const HistEntry> HistoryManager::search(strv substr) const noexcept {
  for (auto it = mEntries.rbegin(); it != mEntries.rend(); ++it)
    if (it->text.find(substr) != str::npos) return s::cref(*it);
  return s::nullopt;
}

Result<void> HistoryManager::load(strv path) {
  s::ifstream f{str(path)};
  if (not f) return {};  // file not found is not an error on first run
  str line;
  while (s::getline(f, line)) {
    if (not line.empty()) {
      HistEntry e;
      e.num  = mNextNum++;
      e.text = s::move(line);
      mEntries.push_back(s::move(e));
    }
  }
  trim_to_limit();
  return {};
}

Result<void> HistoryManager::save(strv path) {
  s::ofstream f{str(path),
      mOpts.append ? s::ios::app : s::ios::trunc};
  if (not f)
    return s::unexpected(ShellError{ErrorKind::FileNotFound,
        "cannot open histfile"});
  for (auto const& e : mEntries)
    f << e.text << '\n';
  return {};
}

}  // namespace jfsh
