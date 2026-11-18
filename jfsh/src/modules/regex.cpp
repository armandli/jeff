#include <jfsh/modules/regex.h>

#include <cstdio>
#include <cstring>
#include <regex.h>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>

// zsh/regex (M10 polish): POSIX ERE companion to zsh/pcre.  The
// `[[ string -regex-match pattern ]]` condition is the only surface
// zsh exposes; on a match it populates $MATCH / $match the way `=~` does.

namespace jfsh {

namespace {

int cond_regex_match(const str& lhs, const str& rhs, ExecContext& ctx) {
  regex_t rx;
  int rc = ::regcomp(&rx, rhs.c_str(), REG_EXTENDED);
  if (rc != 0) {
    char msg[256];
    ::regerror(rc, &rx, msg, sizeof msg);
    s::fprintf(stderr, "jfsh: [[: -regex-match: %s\n", msg);
    ::regfree(&rx);
    return 2;
  }
  constexpr s::size_t NMATCH = 32;
  regmatch_t m[NMATCH];
  rc = ::regexec(&rx, lhs.c_str(), NMATCH, m, 0);
  if (rc == 0) {
    str whole(lhs.data() + m[0].rm_so, m[0].rm_eo - m[0].rm_so);
    (void)ctx.params.set_scalar("MATCH", whole);
    vec<str> captures;
    for (s::size_t k = 1; k < NMATCH; ++k) {
      if (m[k].rm_so < 0) break;
      captures.emplace_back(lhs.data() + m[k].rm_so,
                            static_cast<s::size_t>(m[k].rm_eo - m[k].rm_so));
    }
    (void)ctx.params.set_array("match", captures, false);
  }
  ::regfree(&rx);
  return rc == 0 ? 0 : 1;
}

}  // namespace

void register_regex_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/regex", [](ModuleFeatures& out, ExecContext&) {
    out.conds.push_back(CondDef{"-regex-match", cond_regex_match});
    out.feature_names = {"c:-regex-match"};
  });
}

}  // namespace jfsh
