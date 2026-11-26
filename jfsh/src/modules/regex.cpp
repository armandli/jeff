// zsh/regex: a port of zsh 5.9's Src/Modules/regex.c.  zsh's copyright and
// licence:
//
// Copyright (c) 2007,2012 Phil Pennock
// All Rights Reserved.
//
// Permission is hereby granted, without written agreement and without
// license or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall Phil Pennock or the Zsh Development Group be liable
// to any party for direct, indirect, special, incidental, or consequential
// damages arising out of the use of this software and its documentation,
// even if Phil Pennock and the Zsh Development Group have been advised of
// the possibility of such damage.
//
// Phil Pennock and the Zsh Development Group specifically disclaim any
// warranties, including, but not limited to, the implied warranties of
// merchantability and fitness for a particular purpose.  The software
// provided hereunder is on an "as is" basis, and Phil Pennock and the
// Zsh Development Group have no obligation to provide maintenance,
// support, updates, enhancements, or modifications.

#include <jfsh/modules/regex.h>

#include <cstdio>
#include <cwchar>
#include <regex.h>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

namespace jfsh {

namespace {

void regerrwarn(int r, const regex_t* re, strv msg) {
  char buf[512];
  ::regerror(r, re, buf, sizeof buf);
  s::fprintf(stderr, "jfsh: %.*s: %s\n", static_cast<int>(msg.size()), msg.data(), buf);
}

// The characters in the first `len` bytes of `p` (zsh's MB_CHARLEN loop).
s::int64_t chars(const char* p, s::size_t len, bool multibyte) {
  if (not multibyte) return static_cast<s::int64_t>(len);
  s::int64_t n = 0;
  s::mbstate_t st{};
  while (len) {
    s::size_t c = s::mbrlen(p, len, &st);
    if (c == 0 or c > len) c = 1;   // a bad byte counts as one
    p += c;
    len -= c;
    ++n;
  }
  return n;
}

// [[ lhs -regex-match re ]] and =~: a POSIX extended regex, case blind
// without CASE_MATCH.  The match goes to $MATCH, $MBEGIN, $MEND, $match,
// $mbegin and $mend, or with BASH_REMATCH to $BASH_REMATCH.
int zcond_regex_match(const str& lhstr, const str& rhre, ExecContext& ctx) {
  bool mb = ctx.opts.multibyte();
  int rcflags = REG_EXTENDED;
  if (not ctx.opts.get(OPT_CASEMATCH)) rcflags |= REG_ICASE;
  regex_t re;
  if (int r = ::regcomp(&re, rhre.c_str(), rcflags)) {
    regerrwarn(r, &re, "failed to compile regex");
    return 1;
  }
  vec<regmatch_t> matches(re.re_nsub + 1);
  int r = ::regexec(&re, lhstr.c_str(), matches.size(), matches.data(), 0);
  int ret = 1;
  if (r == 0) {
    ret = 0;
    bool bash = ctx.opts.get(OPT_BASHREMATCH);
    s::size_t start = bash ? 0 : 1;
    auto text = [&](const regmatch_t& m) {
      return m.rm_so < 0 ? str{} : lhstr.substr(static_cast<s::size_t>(m.rm_so),
                                                static_cast<s::size_t>(m.rm_eo - m.rm_so));
    };
    vec<str> arr;
    for (s::size_t n = start; n <= re.re_nsub; ++n) arr.push_back(text(matches[n]));
    if (bash) {
      (void)ctx.params.set_array("BASH_REMATCH", s::move(arr));
    } else {
      int one = ctx.opts.get(OPT_KSH_ARRAYS) ? 0 : 1;
      const regmatch_t& m = matches[0];
      (void)ctx.params.set_scalar("MATCH", text(m));
      s::int64_t offs = chars(lhstr.c_str(), static_cast<s::size_t>(m.rm_so), mb);
      (void)set_iparam(ctx, "MBEGIN", offs + one);
      offs += chars(lhstr.c_str() + m.rm_so, static_cast<s::size_t>(m.rm_eo - m.rm_so), mb);
      (void)set_iparam(ctx, "MEND", offs + one - 1);
      if (re.re_nsub) {
        vec<str> mbegin, mend;
        for (s::size_t n = 1; n <= re.re_nsub; ++n) {
          const regmatch_t& g = matches[n];
          if (g.rm_so < 0 or g.rm_eo < 0) {
            mbegin.emplace_back("-1");
            mend.emplace_back("-1");
            continue;
          }
          s::int64_t o = chars(lhstr.c_str(), static_cast<s::size_t>(g.rm_so), mb);
          mbegin.push_back(s::to_string(o + one));
          o += chars(lhstr.c_str() + g.rm_so, static_cast<s::size_t>(g.rm_eo - g.rm_so), mb);
          mend.push_back(s::to_string(o + one - 1));
        }
        (void)ctx.params.set_array("match", s::move(arr));
        (void)ctx.params.set_array("mbegin", s::move(mbegin));
        (void)ctx.params.set_array("mend", s::move(mend));
      }
    }
  } else if (r != REG_NOMATCH) {
    regerrwarn(r, &re, "regex matching error");
  }
  ::regfree(&re);
  return ret;
}

}  // namespace

void register_regex_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/regex", [](ModuleFeatures& out, ExecContext&) {
    CondDef c;
    c.name = "regex-match";
    c.fn   = zcond_regex_match;
    out.conds.push_back(s::move(c));
  });
}

}  // namespace jfsh
