#pragma once
// jfsh/history/history.hh — command history management.
//
// Mirrors zsh's hist.c.  Tracks history entries, implements history
// expansion (! bang-history), and handles HISTFILE read/write with
// SHARE_HISTORY (cross-session sharing via file locking).

#include <jfsh/core/types.hh>
#include <cstdint>
#include <ctime>
#include <optional>
#include <string>
#include <vector>

namespace jfsh {

// ---------------------------------------------------------------------------
// HistEntry — one history entry
// ---------------------------------------------------------------------------
struct HistEntry {
    std::uint32_t  num       = 0;      // history number (1-based, monotonically increasing)
    std::string    text;               // command text
    std::time_t    timestamp = 0;
    std::uint32_t  elapsed   = 0;      // execution time in seconds
    bool           dup       = false;  // duplicate of immediately preceding entry
};

// ---------------------------------------------------------------------------
// HistOptions — subset of shell options relevant to history
// ---------------------------------------------------------------------------
struct HistOptions {
    int   histsize       = 1000;    // HISTSIZE
    int   savehist       = 1000;    // SAVEHIST
    bool  append         = true;    // APPEND_HISTORY
    bool  share          = false;   // SHARE_HISTORY
    bool  ignore_dups    = false;   // HIST_IGNORE_DUPS
    bool  ignore_all     = false;   // HIST_IGNORE_ALL_DUPS
    bool  ignore_space   = false;   // HIST_IGNORE_SPACE
    bool  no_store       = false;   // HIST_NO_STORE (don't store `history`)
    bool  reduce_blanks  = false;   // HIST_REDUCE_BLANKS
    bool  save_no_dups   = false;   // HIST_SAVE_NO_DUPS
    bool  find_no_dups   = false;   // HIST_FIND_NO_DUPS
    bool  verify         = false;   // HIST_VERIFY
    std::string histfile;           // HISTFILE path
};

// ---------------------------------------------------------------------------
// HistoryManager
// ---------------------------------------------------------------------------
class HistoryManager {
public:
    explicit HistoryManager(HistOptions opts);

    // Add a new entry after a command completes.
    // Returns the assigned history number, or 0 if suppressed.
    std::uint32_t add(std::string text, std::uint32_t elapsed = 0);

    // History expansion (! events).  Returns expanded string or error.
    Result<std::string> expand(std::string_view line);

    // Look up entry by absolute number or negative offset from end.
    const HistEntry* lookup(int num) const noexcept;

    // Search backwards for the most recent entry containing `substr`.
    const HistEntry* search(std::string_view substr) const noexcept;

    // All entries in chronological order (oldest first).
    const std::vector<HistEntry>& entries() const noexcept { return entries_; }

    // Load history from HISTFILE.
    Result<void> load(std::string_view path);

    // Save history to HISTFILE (called on shell exit or SHARE_HISTORY sync).
    Result<void> save(std::string_view path);

    // Merge new entries from HISTFILE into the current session (SHARE_HISTORY).
    Result<void> sync(std::string_view path);

    // Current history size (may be less than histsize after pruning).
    std::size_t size() const noexcept { return entries_.size(); }

    // The highest history number assigned so far.
    std::uint32_t last_num() const noexcept { return next_num_ - 1; }

    // Update options (e.g. after `setopt`).
    void set_options(HistOptions opts) { opts_ = std::move(opts); }

private:
    HistOptions          opts_;
    std::vector<HistEntry> entries_;
    std::uint32_t        next_num_ = 1;

    bool should_suppress(const std::string& text) const noexcept;
    void trim_to_limit();
};

} // namespace jfsh
