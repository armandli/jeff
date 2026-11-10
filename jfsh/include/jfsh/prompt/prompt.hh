#ifndef JFSH_PROMPT_PROMPT_HH
#define JFSH_PROMPT_PROMPT_HH
// jfsh/prompt/prompt.hh — prompt expansion and rendering.
//
// Mirrors zsh's prompt.c.  Expands PROMPT / PS1 / PS2 / PS3 / PS4
// (and RPROMPT / RPS1 / RPS2) percent-escape sequences, colors, and
// conditional substrings to produce the final display string.

#include <string>
#include <string_view>

#include <jfsh/core/options.hh>
#include <jfsh/core/param.hh>
#include <jfsh/core/types.hh>

namespace jfsh {

struct PromptVars {
  std::string PS1;
  std::string PS2;
  std::string PS3;
  std::string PS4;
  std::string RPS1;
  std::string RPS2;
  std::string SPROMPT;
};

struct PromptRenderer {
  PromptRenderer(const ParamTable& params, const OptionState& opts);

  // Returns the rendered string; with strip_escapes=true, ANSI sequences
  // are removed (for width calculation).
  std::string render(std::string_view prompt_str,
                     bool strip_escapes = false) const;

  std::string primary()      const { return render(mVars.PS1); }
  std::string continuation() const { return render(mVars.PS2); }
  std::string right()        const { return render(mVars.RPS1); }
  std::string xtrace()       const { return render(mVars.PS4); }

  void reload();

  static int visual_width(std::string_view rendered) noexcept;

protected:
  // Returns (expanded_string, chars_consumed).
  std::pair<std::string, int>
  expand_percent(std::string_view src, int pos) const;

  std::string do_subst(std::string_view s) const;

private:
  const ParamTable& mParams;
  const OptionState& mOpts;
  PromptVars         mVars;
};

}  // namespace jfsh

#endif  // JFSH_PROMPT_PROMPT_HH
