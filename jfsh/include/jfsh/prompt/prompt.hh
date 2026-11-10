#pragma once
// jfsh/prompt/prompt.hh — prompt expansion and rendering.
//
// Mirrors zsh's prompt.c.  Expands PROMPT / PS1 / PS2 / PS3 / PS4
// (and RPROMPT / RPS1 / RPS2) percent-escape sequences, colors, and
// conditional substrings to produce the final display string.

#include <jfsh/core/types.hh>
#include <jfsh/core/param.hh>
#include <jfsh/core/options.hh>
#include <string>
#include <string_view>

namespace jfsh {

// ---------------------------------------------------------------------------
// Prompt variables (zsh has many; we model the core ones)
// ---------------------------------------------------------------------------
struct PromptVars {
    std::string PS1;    // primary prompt (also PROMPT)
    std::string PS2;    // continuation prompt (also PROMPT2)
    std::string PS3;    // select prompt
    std::string PS4;    // xtrace prefix
    std::string RPS1;   // right-side prompt (also RPROMPT)
    std::string RPS2;   // right-side continuation
    // SPROMPT for spelling correction
    std::string SPROMPT;
};

// ---------------------------------------------------------------------------
// PromptRenderer
// ---------------------------------------------------------------------------
class PromptRenderer {
public:
    PromptRenderer(const ParamTable& params, const OptionState& opts);

    // Expand a prompt string (e.g. PS1 value) and return the display string.
    // With `strip_escapes=true`, ANSI escape sequences are removed (for width
    // calculation purposes).
    std::string render(std::string_view prompt_str,
                       bool strip_escapes = false) const;

    // Rendered primary prompt (for display by ZLE).
    std::string primary()      const { return render(vars_.PS1); }
    std::string continuation() const { return render(vars_.PS2); }
    std::string right()        const { return render(vars_.RPS1); }
    std::string xtrace()       const { return render(vars_.PS4); }

    // Reload from param table (called after `setopt PROMPT_SUBST` or env change).
    void reload();

    // Visual width of a rendered prompt string (accounts for ANSI escapes).
    static int visual_width(std::string_view rendered) noexcept;

private:
    const ParamTable& params_;
    const OptionState& opts_;
    PromptVars         vars_;

    // Expand a single %-escape at position pos in src.
    // Returns (expanded_string, chars_consumed).
    std::pair<std::string, int>
    expand_percent(std::string_view src, int pos) const;

    // Perform PROMPT_SUBST (parameter/command substitution in the prompt).
    std::string do_subst(std::string_view s) const;
};

} // namespace jfsh
