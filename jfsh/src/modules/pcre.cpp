// zsh/pcre: a port of zsh 5.9's Src/Modules/pcre.c to PCRE2 (zsh 5.9 uses
// the older PCRE library, so the library's own error messages differ).
// zsh's copyright and licence:
//
// Copyright (c) 2001, 2002, 2003, 2004, 2007 Clint Adams
// All rights reserved.
//
// Permission is hereby granted, without written agreement and without
// license or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall Clint Adams or the Zsh Development Group be liable
// to any party for direct, indirect, special, incidental, or consequential
// damages arising out of the use of this software and its documentation,
// even if Andrew Main and the Zsh Development Group have been advised of
// the possibility of such damage.
//
// Clint Adams and the Zsh Development Group specifically disclaim any
// warranties, including, but not limited to, the implied warranties of
// merchantability and fitness for a particular purpose.  The software
// provided hereunder is on an "as is" basis, and Andrew Main and the
// Zsh Development Group have no obligation to provide maintenance,
// support, updates, enhancements, or modifications.

#include <jfsh/modules/pcre.h>

#include <langinfo.h>

#include <cstdio>
#include <cstring>
#include <cwchar>

#ifdef JFSH_HAVE_PCRE2
# define PCRE2_CODE_UNIT_WIDTH 8
# include <pcre2.h>
#endif

#include <jfsh/builtins/math.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/modules/support.h>

namespace jfsh {

namespace {

#ifdef JFSH_HAVE_PCRE2

// The pattern pcre_compile made, which pcre_match uses.
struct PcreState {
  ~PcreState() {
    if (code) pcre2_code_free(code);
  }
  pcre2_code* code = nullptr;
};

bool utf8_enabled(const ExecContext& ctx) {
  return ctx.opts.get(OPT_MULTIBYTE) and s::strcmp(::nl_langinfo(CODESET), "UTF-8") == 0;
}

str pcre_message(int errcode) {
  PCRE2_UCHAR msg[256];
  pcre2_get_error_message(errcode, msg, sizeof msg);
  return reinterpret_cast<const char*>(msg);
}

// pcre_compile [-aimxs] pattern
int bin_pcre_compile(vec<str>& args, ExecContext& ctx, PcreState& st) {
  ZOps ops;
  int first = zsh_options(args, "aimxs", 1, 1, ops);
  if (first < 0) return 1;
  strv nam = args[0];
  s::uint32_t opts = 0;
  if (ops.isset('a')) opts |= PCRE2_ANCHORED;
  if (ops.isset('i')) opts |= PCRE2_CASELESS;
  if (ops.isset('m')) opts |= PCRE2_MULTILINE;
  if (ops.isset('x')) opts |= PCRE2_EXTENDED;
  if (ops.isset('s')) opts |= PCRE2_DOTALL;
  if (utf8_enabled(ctx)) opts |= PCRE2_UTF;
  if (st.code) pcre2_code_free(st.code);
  st.code = nullptr;
  str target = args[static_cast<s::size_t>(first)];
  if (target.find('\0') != str::npos) {
    mod_error(nam, "embedded NULs in PCRE pattern terminate pattern");
    target.resize(target.find('\0'));
  }
  int errcode = 0;
  PCRE2_SIZE erroffset = 0;
  st.code = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(target.c_str()), target.size(), opts,
                          &errcode, &erroffset, nullptr);
  if (not st.code) return mod_error(nam, "error in regex: " + pcre_message(errcode));
  return 0;
}

// pcre_study: PCRE2 compiles on demand, so there is only the check.
int bin_pcre_study(vec<str>& args, ExecContext&, PcreState& st) {
  ZOps ops;
  if (zsh_options(args, nullptr, 0, 0, ops) < 0) return 1;
  if (not st.code) return mod_error(args[0], "no pattern has been compiled for study");
  return 0;
}

// The characters in the first `len` bytes of `p`.
s::int64_t chars(const char* p, s::size_t len, bool multibyte) {
  if (not multibyte) return static_cast<s::int64_t>(len);
  s::int64_t n = 0;
  s::mbstate_t mb{};
  while (len) {
    s::size_t c = s::mbrlen(p, len, &mb);
    if (c == 0 or c > len) c = 1;
    p += c;
    len -= c;
    ++n;
  }
  return n;
}

// zsh's zpcre_get_substrings.
void get_substrings(ExecContext& ctx, const str& arg, const PCRE2_SIZE* ovec, int captured,
                    const char* matchvar, const char* substravar, bool want_offset_pair,
                    bool matchedinarr, bool want_begin_end) {
  bool mb = ctx.opts.multibyte();
  auto piece = [&](int i) {
    PCRE2_SIZE b = ovec[2 * i], e = ovec[2 * i + 1];
    return b == PCRE2_UNSET ? str{} : arg.substr(b, e - b);
  };
  int nelem = captured - 1;
  if (want_offset_pair)
    (void)ctx.executor->assign_var("ZPCRE_OP", s::to_string(ovec[0]) + " " + s::to_string(ovec[1]));
  if (matchvar) (void)ctx.executor->assign_var(matchvar, piece(0));
  if (substravar and (not want_begin_end or nelem)) {
    vec<str> matches;
    for (int i = matchedinarr ? 0 : 1; i < captured; ++i) matches.push_back(piece(i));
    (void)ctx.params.set_array(substravar, s::move(matches));
  }
  if (want_begin_end) {
    int one = ctx.opts.get(OPT_KSH_ARRAYS) ? 0 : 1;
    s::int64_t offs = chars(arg.c_str(), ovec[0], mb);
    (void)set_iparam(ctx, "MBEGIN", offs + one);
    offs += chars(arg.c_str() + ovec[0], ovec[1] - ovec[0], mb);
    (void)set_iparam(ctx, "MEND", offs + one - 1);
    if (nelem) {
      vec<str> mbegin, mend;
      for (int i = 1; i <= nelem; ++i) {
        PCRE2_SIZE b = ovec[2 * i], e = ovec[2 * i + 1];
        if (b == PCRE2_UNSET) b = e = 0;
        s::int64_t o = chars(arg.c_str(), b, mb);
        mbegin.push_back(s::to_string(o + one));
        o += chars(arg.c_str() + b, e - b, mb);
        mend.push_back(s::to_string(o + one - 1));
      }
      (void)ctx.params.set_array("mbegin", s::move(mbegin));
      (void)ctx.params.set_array("mend", s::move(mend));
    }
  }
}

// pcre_match [-b] [-v var] [-a arr] [-n offset] string
int bin_pcre_match(vec<str>& args, ExecContext& ctx, PcreState& st) {
  ZOps ops;
  int first = zsh_options(args, "a:v:n:b", 1, 1, ops);
  if (first < 0) return 1;
  strv nam = args[0];
  if (not st.code) return mod_error(nam, "no pattern has been compiled");
  str matched_portion = "MATCH", receptacle = "match";
  if (ops.hasarg('a')) receptacle = ops.arg('a');
  if (ops.hasarg('v')) matched_portion = ops.arg('v');
  s::size_t offset_start = 0;
  if (ops.hasarg('n')) {
    const str& n = ops.arg('n');
    auto r = zstrtol(n, 0, 10);
    if (r.end != n.size() or r.value < 0) return mod_error(nam, "integer expected: " + n);
    offset_start = static_cast<s::size_t>(r.value);
  }
  const str& subject = args[static_cast<s::size_t>(first)];
  pcre2_match_data* md = pcre2_match_data_create_from_pattern(st.code, nullptr);
  int ret = offset_start > 0 and offset_start >= subject.size()
              ? PCRE2_ERROR_NOMATCH
              : pcre2_match(st.code, reinterpret_cast<PCRE2_SPTR>(subject.data()), subject.size(),
                            offset_start, 0, md, nullptr);
  int return_value = 1;
  if (ret == 0) {
    return_value = 0;
  } else if (ret > 0) {
    get_substrings(ctx, subject, pcre2_get_ovector_pointer(md), ret, matched_portion.c_str(),
                   receptacle.c_str(), ops.isset('b'), false, false);
    return_value = 0;
  } else if (ret != PCRE2_ERROR_NOMATCH) {
    mod_error(nam, "error in pcre_exec [" + s::to_string(ret) + "]");
  }
  pcre2_match_data_free(md);
  return return_value;
}

// [[ lhs -pcre-match re ]], and =~ under RE_MATCH_PCRE.
int cond_pcre_match(const str& lhs, const str& rhs, ExecContext& ctx) {
  s::uint32_t opts = 0;
  if (utf8_enabled(ctx)) opts |= PCRE2_UTF;
  if (ctx.opts.get(OPT_REMATCHPCRE) and not ctx.opts.get(OPT_CASEMATCH)) opts |= PCRE2_CASELESS;
  bool bash = ctx.opts.get(OPT_BASHREMATCH);
  str re = rhs;
  if (re.find('\0') != str::npos) {
    s::fprintf(stderr, "jfsh: embedded NULs in PCRE pattern terminate pattern\n");
    re.resize(re.find('\0'));
  }
  int errcode = 0;
  PCRE2_SIZE erroffset = 0;
  pcre2_code* pat = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(re.c_str()), re.size(), opts,
                                  &errcode, &erroffset, nullptr);
  if (not pat) {
    s::fprintf(stderr, "jfsh: failed to compile regexp /%s/: %s\n", rhs.c_str(),
               pcre_message(errcode).c_str());
    return 1;
  }
  pcre2_match_data* md = pcre2_match_data_create_from_pattern(pat, nullptr);
  int r = pcre2_match(pat, reinterpret_cast<PCRE2_SPTR>(lhs.data()), lhs.size(), 0, 0, md, nullptr);
  int ret = 1;
  if (r == 0) {
    s::fprintf(stderr, "jfsh: reportable zsh problem: pcre_exec() returned 0\n");
    ret = 0;
  } else if (r > 0) {
    get_substrings(ctx, lhs, pcre2_get_ovector_pointer(md), r, bash ? nullptr : "MATCH",
                   bash ? "BASH_REMATCH" : "match", false, bash, not bash);
    ret = 0;
  } else if (r != PCRE2_ERROR_NOMATCH) {
    s::fprintf(stderr, "jfsh: pcre_exec() error [%d]\n", r);
  }
  pcre2_match_data_free(md);
  pcre2_code_free(pat);
  return ret;
}

#else   // JFSH_HAVE_PCRE2

struct PcreState {};

int bin_notavail(vec<str>& args, ExecContext&, PcreState&) {
  return mod_error(args[0], "not available on this system");
}
#define bin_pcre_compile bin_notavail
#define bin_pcre_study   bin_notavail
#define bin_pcre_match   bin_notavail

#endif  // JFSH_HAVE_PCRE2

}  // namespace

void register_pcre_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/pcre", [](ModuleFeatures& out, ExecContext&) {
    auto st = s::make_shared<PcreState>();
    auto bind = [st](int (*fn)(vec<str>&, ExecContext&, PcreState&)) -> BuiltinFn {
      return [st, fn](vec<str>& a, ExecContext& c) { return fn(a, c, *st); };
    };
    out.builtins = {{"pcre_compile", bind(bin_pcre_compile), BINF_NONE},
                    {"pcre_match", bind(bin_pcre_match), BINF_NONE},
                    {"pcre_study", bind(bin_pcre_study), BINF_NONE}};
#ifdef JFSH_HAVE_PCRE2
    CondDef c;
    c.name = "pcre-match";
    c.fn   = cond_pcre_match;
    out.conds.push_back(s::move(c));
#endif
  });
}

}  // namespace jfsh
