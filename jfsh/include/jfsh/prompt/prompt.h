#ifndef JFSH_PROMPT_PROMPT_H
#define JFSH_PROMPT_PROMPT_H
// jfsh/prompt/prompt.h — prompt expansion and rendering.
//
// Mirrors zsh's prompt.c.  Expands PROMPT / PS1 / PS2 / PS3 / PS4
// (and RPROMPT / RPS1 / RPS2) percent-escape sequences, colors, and
// conditional substrings to produce the final display string.

#include <string>
#include <string_view>

#include <jfsh/core/aliases.h>
#include <jfsh/core/options.h>
#include <jfsh/core/param.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct PromptVars {
  str PS1;
  str PS2;
  str PS3;
  str PS4;
  str RPS1;
  str RPS2;
  str SPROMPT;
};

struct PromptRenderer {
  PromptRenderer(const ParamTable& params, const OptionState& opts)
    : mParams(params), mOpts(opts) {
    reload();
  }

  // Returns the rendered string; with strip_escapes=true, ANSI sequences
  // are removed (for width calculation).
  str render(strv prompt_str,
             bool strip_escapes = false) const;

  str primary()      const { return render(mVars.PS1); }
  str continuation() const { return render(mVars.PS2); }
  str right()        const { return render(mVars.RPS1); }
  str xtrace()       const { return render(mVars.PS4); }

  void reload();

  static int visual_width(strv rendered) noexcept;

protected:
  // Returns (expanded_string, chars_consumed).
  s::pair<str, int>
  expand_percent(strv src, int pos) const;

  str do_subst(strv s) const;

private:
  const ParamTable& mParams;
  [[maybe_unused]] const OptionState& mOpts;   // not used yet
  PromptVars         mVars;
};

}  // namespace jfsh

#endif  // JFSH_PROMPT_PROMPT_H
