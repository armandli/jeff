#ifndef JFSH_PROMPT_PROMPT_H
#define JFSH_PROMPT_PROMPT_H
// jfsh/prompt/prompt.h — prompt expansion and rendering.
//
// Mirrors zsh's prompt.c.  Expands PROMPT / PS1 / PS2 / PS3 / PS4
// (and RPROMPT / RPS1 / RPS2) percent-escape sequences, colors, and
// conditional substrings to produce the final display string.

#include <cstdint>

#include <functional>
#include <string>
#include <string_view>

#include <jfsh/core/aliases.h>
#include <jfsh/core/options.h>
#include <jfsh/core/param.h>
#include <jfsh/core/types.h>

namespace jfsh {

// An expanded prompt marks the text that takes no room on the screen
// (escape sequences: %B, %F{…}, %{…%}) with these two bytes, as readline
// does, so the editor can tell how wide the prompt is.  Strip them before
// writing the prompt anywhere (strip_markers, or the editor's refresh).
inline constexpr char PROMPT_ZW_START = '\x01';
inline constexpr char PROMPT_ZW_END   = '\x02';

struct PromptVars {
  str PS1;
  str PS2;
  str PS3;
  str PS4;
  str RPS1;
  str RPS2;
  str SPROMPT;
};

// What the prompt needs from the rest of the shell.  Each hook is
// optional; without it the value comes from the parameters ($?, $LINENO,
// $0) or is empty.
struct PromptHooks {
  // PROMPT_SUBST: parameter, command and arithmetic expansion of the
  // prompt before the percent escapes.
  func<str(strv)> subst;
  func<int()>     job_count;     // %j
  func<str()>     script_name;   // %N: the script, sourced file or function
  func<str()>     file_name;     // %x: the script or sourced file
  func<int()>     eval_depth;    // %e
};

struct PromptRenderer {
  PromptRenderer(const ParamTable& params, const OptionState& opts)
    : mParams(params), mOpts(opts) {
    reload();
  }

  void set_hooks(PromptHooks hooks) { mHooks = s::move(hooks); }

  // Expands `prompt_str`, with the zero-width parts between
  // PROMPT_ZW_START and PROMPT_ZW_END.
  str expand(strv prompt_str) const;

  // Expands `prompt_str` for output (`print -P`, PS4): markers removed.
  // With strip_escapes the escape sequences go too, leaving the text.
  str render(strv prompt_str, bool strip_escapes = false) const;

  str primary()            const { return expand(mVars.PS1); }
  str continuation()       const { return expand(mVars.PS2); }
  str right()              const { return expand(mVars.RPS1); }
  str right_continuation() const { return expand(mVars.RPS2); }
  str select()             const { return expand(mVars.PS3); }
  str xtrace()             const { return expand(mVars.PS4); }

  // Reads PS1 … SPROMPT from the parameters again.
  void reload();

  // Removes the markers, keeping the zero-width text between them (or,
  // with drop_zero_width, dropping it).
  static str strip_markers(strv expanded, bool drop_zero_width = false);

  // Columns the last line of an expanded prompt takes: zero-width parts
  // and escape sequences don't count.
  static int visual_width(strv expanded) noexcept;

private:
  const ParamTable&  mParams;
  const OptionState& mOpts;
  PromptVars         mVars;
  PromptHooks        mHooks;

  friend struct PromptExpander;
};

// zsh's ztrftime, as %D{…} uses it (fc -t, -i, -E, -f too).
str ztrftime(strv fmt, const struct tm& tm, long nsec = 0);

// zsh/nearcolor (M11): while set, a `%F{#rrggbb}` / `%K{#rrggbb}` colour
// is handed here for the SGR sequence to use instead of a 24-bit one, so a
// terminal without truecolor gets the nearest palette colour.
inline func<str(unsigned r, unsigned g, unsigned b, bool fg)> truecolor_mapper;

}  // namespace jfsh

#endif  // JFSH_PROMPT_PROMPT_H
