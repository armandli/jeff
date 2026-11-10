#include <jfsh/history/history.hh>
#include <algorithm>
#include <ctime>
#include <fstream>
#include <sstream>

namespace jfsh {

HistoryManager::HistoryManager(HistOptions opts) : opts_(std::move(opts)) {}

bool HistoryManager::should_suppress(const std::string& text) const noexcept {
    if (text.empty()) return true;
    if (opts_.ignore_space && text[0] == ' ') return true;
    if (opts_.no_store) return false;  // "history" suppressed by no_store flag
    if (opts_.ignore_dups && !entries_.empty() && entries_.back().text == text)
        return true;
    if (opts_.ignore_all) {
        for (auto const& e : entries_)
            if (e.text == text) return true;
    }
    return false;
}

void HistoryManager::trim_to_limit() {
    while (static_cast<int>(entries_.size()) > opts_.histsize && !entries_.empty())
        entries_.erase(entries_.begin());
}

std::uint32_t HistoryManager::add(std::string text, std::uint32_t elapsed) {
    if (opts_.reduce_blanks) {
        std::string trimmed;
        bool last_ws = false;
        for (char c : text) {
            bool ws = c == ' ' || c == '\t';
            if (!ws || !last_ws) trimmed += c;
            last_ws = ws;
        }
        text = std::move(trimmed);
    }
    if (should_suppress(text)) return 0;
    HistEntry e;
    e.num       = next_num_++;
    e.text      = std::move(text);
    e.timestamp = std::time(nullptr);
    e.elapsed   = elapsed;
    // Remove duplicates if HIST_IGNORE_ALL_DUPS
    if (opts_.ignore_all) {
        entries_.erase(
            std::remove_if(entries_.begin(), entries_.end(),
                           [&](const HistEntry& x){ return x.text == e.text; }),
            entries_.end());
    }
    entries_.push_back(std::move(e));
    trim_to_limit();
    return entries_.back().num;
}

Result<std::string> HistoryManager::expand(std::string_view line) {
    // Minimal ! expansion: !! = last command, !N = entry N, !-N = Nth from end
    if (line.empty() || line[0] != '!') return std::string(line);
    if (line == "!!") {
        if (entries_.empty()) return std::unexpected(ShellError{ErrorKind::ExecError, "no previous command"});
        return entries_.back().text;
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

const HistEntry* HistoryManager::lookup(int num) const noexcept {
    if (num > 0) {
        for (auto const& e : entries_)
            if (static_cast<int>(e.num) == num) return &e;
    } else {
        int idx = static_cast<int>(entries_.size()) + num;
        if (idx >= 0) return &entries_[static_cast<std::size_t>(idx)];
    }
    return nullptr;
}

const HistEntry* HistoryManager::search(std::string_view substr) const noexcept {
    for (auto it = entries_.rbegin(); it != entries_.rend(); ++it)
        if (it->text.find(substr) != std::string::npos) return &*it;
    return nullptr;
}

Result<void> HistoryManager::load(std::string_view path) {
    std::ifstream f{std::string(path)};
    if (!f) return {};  // file not found is not an error on first run
    std::string line;
    while (std::getline(f, line)) {
        if (!line.empty()) {
            HistEntry e;
            e.num  = next_num_++;
            e.text = std::move(line);
            entries_.push_back(std::move(e));
        }
    }
    trim_to_limit();
    return {};
}

Result<void> HistoryManager::save(std::string_view path) {
    std::ofstream f{std::string(path),
        opts_.append ? std::ios::app : std::ios::trunc};
    if (!f) return std::unexpected(ShellError{ErrorKind::FileNotFound, "cannot open histfile"});
    for (auto const& e : entries_)
        f << e.text << '\n';
    return {};
}

Result<void> HistoryManager::sync(std::string_view path) {
    return load(path);
}

} // namespace jfsh
