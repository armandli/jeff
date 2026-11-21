// zsh/computil: the builtins the compsys shell functions lean on —
// comparguments (_arguments), compdescribe (_describe), compvalues
// (_values), comptags and comptry (_tags, _wanted, _requested), compfiles
// (_path_files) and compgroups.  Mirrors Src/Zle/computil.c; compquote
// (also computil's in zsh) is with compadd in src/builtins/complete.cpp.
// Their state (the parsed descriptions, the tags offered at each function
// level) lives here between calls, as zsh keeps it in statics.

#include <jfsh/modules/computil.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <array>
#include <deque>
#include <memory>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/core/options.h>
#include <jfsh/core/sort.h>
#include <jfsh/core/utf8.h>
#include <jfsh/editor/compmatch.h>
#include <jfsh/editor/completion.h>
#include <jfsh/editor/zle.h>
#include <jfsh/exec/executor.h>
#include <jfsh/glob/glob.h>

namespace jfsh {

namespace {

constexpr int CM_SPACE = 2;

CompState* comp_state(ExecContext& ctx) {
  return ctx.zle and ctx.zle->completion ? ctx.zle->completion->state() : nullptr;
}

int not_in_completion(strv nam) {
  return builtin_error(nam, "can only be called from completion function");
}

// The words of a `(...)` list, or the value of an array, association or
// scalar parameter (zsh's get_user_var).
opt<vec<str>> user_var(ExecContext& ctx, const str& name) {
  if (name.starts_with('(')) {
    vec<str> out;
    str cur;
    bool notempty = false, brk = false;
    for (s::size_t i = 1; i < name.size(); ++i) {
      char c = name[i];
      if (c == '\\' and i + 1 < name.size()) {
        cur += name[++i];
        notempty = true;
      } else if (c == ',' or c == ' ' or c == '\t' or c == '\n' or c == ')') {
        if (c == ')') brk = true;
        if (notempty) {
          if (cur.starts_with('\n')) cur.erase(0, 1);
          out.push_back(cur);
        }
        cur.clear();
        notempty = false;
      } else {
        cur += c;
        notempty = true;
      }
      if (brk) break;
    }
    if (not brk or out.empty()) return s::nullopt;
    return out;
  }
  auto p = ctx.params.lookup(name);
  if (not p or not p->get().is_set()) return s::nullopt;
  const Param& prm = p->get();
  if (prm.is_hash()) {
    vec<str> out;
    for (const auto& [k, v] : prm.hash()) out.push_back(v);
    return out;
  }
  if (prm.is_array()) return prm.array();
  return vec<str>{prm.scalar()};
}

opt<vec<str>> array_param(ExecContext& ctx, const str& name) {
  auto p = ctx.params.lookup(name);
  if (not p or not p->get().is_set() or not p->get().is_array()) return s::nullopt;
  return p->get().array();
}

// zsh's sethparam: `name` becomes an association of the key/value pairs.
void set_hash(ExecContext& ctx, const str& name, vec<str> pairs) {
  auto p = ctx.params.lookup(name);
  if (not p or not p->get().is_hash()) ctx.params.assign(name, umap<str, str>{}, PM_HASH);
  (void)ctx.params.set_array(name, s::move(pairs));
}

// Backslashes off (zsh's rembslash).
str rembslash(strv s) {
  str out;
  for (s::size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\\') {
      if (++i < s.size()) out += s[i];
    } else {
      out += s[i];
    }
  }
  return out;
}

// Backslashes before colons off.
str rembslashcolon(strv s) {
  str out;
  for (s::size_t i = 0; i < s.size(); ++i)
    if (s[i] != '\\' or i + 1 >= s.size() or s[i + 1] != ':') out += s[i];
  return out;
}

void ignore_prefix(CompState& st, s::size_t n) {
  n = s::min(n, st.prefix.size());
  st.iprefix += st.prefix.substr(0, n);
  st.prefix.erase(0, n);
}

void ignore_suffix(CompState& st, s::size_t n) {
  n = s::min(n, st.suffix.size());
  st.isuffix.insert(0, st.suffix.substr(st.suffix.size() - n));
  st.suffix.resize(st.suffix.size() - n);
}

// ---- compdescribe -----------------------------------------------------------------

struct CdSet;

struct CdStr {
  CdStr* next = nullptr;    // the next one in this set
  str    text;              // the string to display
  opt<str> desc;
  str    match;             // the match to add
  int    len = 0;
  int    width = 0;
  CdStr* other = nullptr;   // the next string with the same description
  int    kind = 0;          // 0: not in a group, 1: the first, 2: another
  CdSet* set = nullptr;
  CdStr* run = nullptr;     // the next in this run
};

struct CdSet {
  vec<str> opts;            // the compadd options
  CdStr*   strs = nullptr;
  int      count = 0;
  int      desc = 0;
};

enum CdRunType : int { CRT_SIMPLE, CRT_DESC, CRT_SPEC, CRT_DUMMY, CRT_EXPL };

struct CdRun {
  CdRunType type = CRT_SIMPLE;
  CdStr*    strs = nullptr;
  int       count = 0;
};

struct CdState {
  bool showd = false;
  str  sep;
  int  slen = 0, swidth = 0, maxmlen = 0;
  s::deque<CdSet> sets;
  s::deque<CdStr> strs;
  int  pre = 0, premaxw = 0, suf = 0, maxg = 0, maxglen = 0, groups = 0, descs = 0, gprew = 0;
  int  columns = 80;
  s::deque<CdRun> runs;
};

CdState& cd_state() {
  static CdState st;
  return st;
}
bool CdParsed = false;

void cd_group(CdState& cd, int maxg) {
  cd.groups = cd.descs = cd.maxglen = cd.maxg = 0;
  for (auto& set : cd.sets)
    for (CdStr* s1 = set.strs; s1; s1 = s1->next) {
      s1->kind = 0;
      s1->other = nullptr;
    }
  for (s::size_t si = 0; si < cd.sets.size(); ++si) {
    for (CdStr* s1 = cd.sets[si].strs; s1; s1 = s1->next) {
      if (not s1->desc or s1->kind != 0) continue;
      int num = 1, width = s1->width + cd.swidth;
      if (width > cd.maxglen) cd.maxglen = width;
      CdStr** strp = &s1->other;
      bool stop = false;
      for (s::size_t sj = si; sj < cd.sets.size() and not stop; ++sj) {
        CdStr* s2 = sj == si ? s1->next : cd.sets[sj].strs;
        for (; s2; s2 = s2->next) {
          if (s2->desc and *s1->desc == *s2->desc) {
            width += CM_SPACE + s2->width;
            if (width > cd.maxmlen or num == maxg) break;
            if (width > cd.maxglen) cd.maxglen = width;
            s1->kind = 1;
            s2->kind = 2;
            ++num;
            *strp = s2;
            strp = &s2->other;
          }
        }
        if (s2) stop = true;
      }
      *strp = nullptr;
      if (num > 1) ++cd.groups;
      else         ++cd.descs;
      if (num > cd.maxg) cd.maxg = num;
    }
  }
}

void cd_calc(CdState& cd) {
  cd.pre = cd.suf = 0;
  for (auto& set : cd.sets) {
    set.count = set.desc = 0;
    for (CdStr* s1 = set.strs; s1; s1 = s1->next) {
      ++set.count;
      cd.pre     = s::max(cd.pre, static_cast<int>(s1->text.size()));
      cd.premaxw = s::max(cd.premaxw, text_width(s1->text));
      if (s1->desc) {
        ++set.desc;
        cd.suf = s::max(cd.suf, static_cast<int>(s1->desc->size()));
      }
    }
  }
}

bool cd_groups_want_sorting(const CdState& cd) {
  for (const auto& set : cd.sets)
    for (const auto& o : set.opts) {
      if (o.starts_with("-V")) return false;
      if (o.starts_with("-J")) return true;
    }
  return true;
}

// Builds the runs -g hands out; true when the groups are too wide (the
// caller tries again with smaller ones).
bool cd_prep(CdState& cd) {
  cd.runs.clear();
  if (cd.groups) {
    int preplines = cd.groups + cd.descs;
    vec<CdStr*> grps;
    vec<int> wids(static_cast<s::size_t>(cd.maxg), 0);
    for (auto& set : cd.sets) {
      for (CdStr* s1 = set.strs; s1; s1 = s1->next) {
        if (s1->kind != 1) {
          if (not s1->kind and s1->desc) {
            wids[0] = s::max(wids[0], s1->width);
            s1->other = nullptr;
            grps.push_back(s1);
          }
          continue;
        }
        CdStr* gs = s1;
        gs->kind = 2;
        CdStr* gp = s1->other;
        gs->other = nullptr;
        for (CdStr* gn; gp; gp = gn) {
          gn = gp->other;
          gp->other = nullptr;
          CdStr** gpp = &gs;
          for (; *gpp and (*gpp)->width > gp->width; gpp = &(*gpp)->other) {}
          gp->other = *gpp;
          *gpp = gp;
        }
        int i = 0;
        for (gp = gs; gp; gp = gp->other, ++i)
          wids[static_cast<s::size_t>(i)] = s::max(wids[static_cast<s::size_t>(i)], gp->width);
        grps.push_back(gs);
      }
    }
    cd.gprew = 0;
    for (int w : wids) cd.gprew += w + CM_SPACE;
    if (cd.gprew > cd.maxmlen and cd.maxglen > 1) return true;

    if (cd_groups_want_sorting(cd))
      s::stable_sort(grps.begin(), grps.end(),
                     [](CdStr* a, CdStr* b) { return zstrcmp(a->text, b->text) < 0; });
    // Those with the same description next to each other.
    for (s::size_t k = 0; k + 1 < grps.size(); ++k) {
      if (*grps[k]->desc == *grps[k + 1]->desc) continue;
      for (s::size_t j = k + 2; j < grps.size(); ++j) {
        if (*grps[k]->desc == *grps[j]->desc) {
          CdStr* tmp = grps[j];
          grps.erase(grps.begin() + static_cast<s::ptrdiff_t>(j));
          grps.insert(grps.begin() + static_cast<s::ptrdiff_t>(k) + 1, tmp);
          ++k;
        }
      }
    }
    CdRun expl{CRT_EXPL, grps[0], preplines};
    CdStr** strp2 = nullptr;
    for (auto& g : grps) {
      CdStr* s1 = g;
      g = s1->other;
      if (strp2) *strp2 = s1;
      strp2 = &s1->run;
      cd.runs.push_back({CRT_SPEC, s1, 1});
    }
    *strp2 = nullptr;
    for (int i = cd.maxg - 1; i; --i) {
      int d = 0;
      for (auto& g : grps) {
        if (CdStr* s1 = g) {
          if (d) {
            cd.runs.push_back({CRT_DUMMY, expl.strs, d});
            d = 0;
          }
          cd.runs.push_back({CRT_SPEC, s1, 1});
          s1->run = nullptr;
          g = s1->other;
        } else {
          ++d;
        }
      }
      if (d) cd.runs.push_back({CRT_DUMMY, expl.strs, d});
    }
    cd.runs.push_back(expl);
    for (auto& set : cd.sets) {
      int i = 0;
      CdStr* gs = nullptr;
      CdStr** gpp = &gs;
      for (CdStr* s1 = set.strs; s1; s1 = s1->next) {
        if (s1->kind or s1->desc) continue;
        ++i;
        *gpp = s1;
        gpp = &s1->run;
      }
      *gpp = nullptr;
      if (i) cd.runs.push_back({CRT_SIMPLE, gs, i});
    }
  } else if (cd.showd) {
    for (auto& set : cd.sets) {
      if (set.desc) {
        CdRun run{CRT_DESC, nullptr, set.desc};
        CdStr** strp = &run.strs;
        for (CdStr* s1 = set.strs; s1; s1 = s1->next)
          if (s1->desc) {
            *strp = s1;
            strp = &s1->run;
          }
        *strp = nullptr;
        cd.runs.push_back(run);
      }
      if (set.desc != set.count) {
        CdRun run{CRT_SIMPLE, nullptr, set.count - set.desc};
        CdStr** strp = &run.strs;
        for (CdStr* s1 = set.strs; s1; s1 = s1->next)
          if (not s1->desc) {
            *strp = s1;
            strp = &s1->run;
          }
        *strp = nullptr;
        cd.runs.push_back(run);
      }
    }
  } else {
    for (auto& set : cd.sets) {
      if (not set.count) continue;
      for (CdStr* s1 = set.strs; s1; s1 = s1->next) s1->run = s1->next;
      cd.runs.push_back({CRT_SIMPLE, set.strs, set.count});
    }
  }
  return false;
}

int cd_init(ExecContext& ctx, CompState& st, strv nam, const str& hide, const str& mlen,
            const str& sep, const vec<str>& opts, vec<str> args, bool disp) {
  CdState& cd = cd_state();
  cd = CdState{};
  CdParsed = false;
  cd.sep     = sep;
  cd.slen    = static_cast<int>(sep.size());
  cd.swidth  = text_width(sep);
  cd.showd   = disp;
  cd.columns = st.columns;
  cd.maxmlen = s::atoi(mlen.c_str());
  int itmp = cd.columns - cd.swidth - 4;
  if (cd.maxmlen > itmp) cd.maxmlen = itmp;
  if (cd.maxmlen < 4) cd.maxmlen = 4;
  s::size_t a = 0;
  bool grp = false;
  if (a < args.size() and args[a] == "-g") {
    ++a;
    grp = true;
  }
  while (a < args.size()) {
    CdSet& set = cd.sets.emplace_back();
    auto ap = user_var(ctx, args[a]);
    if (not ap) return builtin_error(nam, "invalid argument: " + args[a]);
    CdStr** strp = &set.strs;
    for (const auto& w : *ap) {
      CdStr& s1 = cd.strs.emplace_back();
      *strp = &s1;
      strp = &s1.next;
      s1.set = &set;
      // Up to the first colon not quoted with a backslash.
      s::size_t k = 0;
      for (; k < w.size() and w[k] != ':'; ++k)
        if (w[k] == '\\' and k + 1 < w.size()) ++k;
      if (k < w.size()) s1.desc = rembslash(strv(w).substr(k + 1));
      s1.text  = s1.match = rembslash(strv(w).substr(0, k));
      s1.len   = static_cast<int>(s1.text.size());
      s1.width = text_width(s1.text);
    }
    if (++a < args.size() and not args[a].starts_with('-')) {
      auto mp = user_var(ctx, args[a]);
      if (not mp) return builtin_error(nam, "invalid argument: " + args[a]);
      s::size_t k = 0;
      for (CdStr* s1 = set.strs; s1 and k < mp->size(); s1 = s1->next, ++k) s1->match = (*mp)[k];
      ++a;
    }
    if (not hide.empty()) {
      for (CdStr* s1 = set.strs; s1; s1 = s1->next) {
        if (hide.size() > 1 and s1->text.starts_with("--")) s1->text.erase(0, 2);
        else if (s1->text.starts_with('-') or s1->text.starts_with('+')) s1->text.erase(0, 1);
      }
    }
    s::size_t from = a;
    while (a < args.size() and args[a] != "--") ++a;
    set.opts.assign(args.begin() + static_cast<s::ptrdiff_t>(from),
                    args.begin() + static_cast<s::ptrdiff_t>(a));
    set.opts.insert(set.opts.end(), opts.begin(), opts.end());
    if (a < args.size()) ++a;
  }
  if (disp and grp) {
    int mg = cd.columns;
    do {
      cd_group(cd, mg);
      mg = cd.maxg - 1;
      cd_calc(cd);
    } while (cd_prep(cd));
  } else {
    cd_calc(cd);
    cd_prep(cd);
  }
  CdParsed = true;
  return 0;
}

// `desc` cut to fit `remw` columns, and the columns it takes.
struct Fitted {
  str text;
  int width = 0;
};

Fitted fit(const str& desc, int remw) {
  int w = text_width(desc);
  if (w <= remw) return {desc, w};
  Fitted out;
  for (s::size_t k = 0; out.width < remw and k < desc.size();) {
    s::size_t n = next_char(desc, k);
    int cw = text_width(strv(desc).substr(k, n - k));
    if (out.width + cw > remw) break;
    out.text += desc.substr(k, n - k);
    out.width += cw;
    k = n;
  }
  return out;
}

int cd_get(ExecContext& ctx, const vec<str>& params) {
  CdState& cd = cd_state();
  if (cd.runs.empty()) return 1;
  CdRun run = cd.runs.front();
  cd.runs.pop_front();
  vec<str> mats, dpys, opts;
  str csl;
  switch (run.type) {
  case CRT_SIMPLE: {
    for (CdStr* s1 = run.strs; s1; s1 = s1->run) {
      mats.push_back(s1->match);
      dpys.push_back(s1->text);
    }
    opts = run.strs->set->opts;
    if (cd.groups) {
      // No explanation for the matches without descriptions (zsh tests
      // the element it copies to, so all after a -X go too).
      s::size_t dp = 0;
      for (s::size_t mp = 0; mp < opts.size(); ++mp) {
        if (opts[dp].starts_with("-X")) {
          if (opts[dp].size() == 2 and dp + 1 < opts.size()) ++mp;
        } else {
          opts[dp++] = opts[mp];
        }
      }
      opts.resize(dp);
    }
  }

  break; case CRT_DESC: {
    for (CdStr* s1 = run.strs; s1; s1 = s1->run) {
      mats.push_back(s1->match);
      // zsh copies the string hide shortened at its old length: the
      // rest is lost.
      if (static_cast<int>(s1->text.size()) != s1->len) {
        dpys.push_back(s1->text);
        continue;
      }
      int pad = s::max(0, cd.premaxw - s1->width + CM_SPACE);
      str buf = s1->text + str(static_cast<s::size_t>(pad), ' ');
      int remw = cd.columns - cd.premaxw - cd.swidth - 3;
      while (remw < 0 and cd.columns) remw += cd.columns;
      if (cd.slen < remw) {
        buf += cd.sep;
        remw -= cd.slen;
        buf += fit(*s1->desc, remw).text;
      }
      dpys.push_back(s::move(buf));
    }
    opts = run.strs->set->opts;
    opts.insert(opts.begin(), "-l");
  }

  break; case CRT_SPEC: {
    mats.push_back(run.strs->match);
    dpys.push_back(run.strs->text);
    opts = run.strs->set->opts;
    auto it = s::find_if(opts.begin(), opts.end(),
                         [](const str& o) { return o.starts_with("-J") or o.starts_with("-V"); });
    if (it != opts.end()) *it = "-2V" + it->substr(2);
    else                  opts.insert(opts.begin(), "-2V-default-");
    csl = "packed";
  }

  break; case CRT_DUMMY: {
    opts = run.strs->set->opts;
    opts.insert(opts.begin(), "-E" + s::to_string(run.count));
    csl = "packed";
  }

  break; default: {
    for (CdStr* s1 = run.strs; s1; s1 = s1->run) {
      if (s1->run and *s1->desc == *s1->run->desc) {
        dpys.emplace_back();
        continue;
      }
      int remw = cd.columns - cd.gprew - cd.swidth - CM_SPACE;
      Fitted f = fit(*s1->desc, remw);
      remw -= f.width;
      str buf = cd.sep + f.text;
      if (remw > 0) buf += str(static_cast<s::size_t>(remw), ' ');
      dpys.push_back(s::move(buf));
    }
    opts = run.strs->set->opts;
    opts.insert(opts.begin(), "-E" + s::to_string(run.count));
    csl = "packed";
  }
  }
  (void)ctx.params.set_scalar(params[0], csl);
  (void)ctx.params.set_array(params[1], s::move(opts));
  (void)ctx.params.set_array(params[2], s::move(mats));
  (void)ctx.params.set_array(params[3], s::move(dpys));
  return 0;
}

// compdescribe -i hide mlen arrays… | -I hide mlen sep opts arrays… | -g csl opts mats dpys
int builtin_compdescribe(vec<str>& args, ExecContext& ctx) {
  strv nam = args[0];
  CompState* st = comp_state(ctx);
  if (args.size() < 4) return builtin_error(nam, "not enough arguments");
  if (not st) return not_in_completion(nam);
  vec<str> a(args.begin() + 1, args.end());
  if (a[0].size() != 2) return builtin_error(nam, "invalid argument: " + a[0]);
  switch (a[0][1]) {
  case 'i':
    if (a.size() < 3) return builtin_error(nam, "not enough arguments");
    return cd_init(ctx, *st, nam, a[1], a[2], "", {}, vec<str>(a.begin() + 3, a.end()), false);

  break; case 'I': {
    if (a.size() < 6) return builtin_error(nam, "not enough arguments");
    auto opts = array_param(ctx, a[4]);
    if (not opts) return builtin_error(nam, "unknown parameter: " + a[4]);
    return cd_init(ctx, *st, nam, a[1], a[2], a[3], *opts, vec<str>(a.begin() + 5, a.end()),
                   true);
  }

  break; case 'g':
    if (not CdParsed) return builtin_error(nam, "no parsed state");
    if (a.size() != 5)
      return builtin_error(nam, a.size() < 5 ? "not enough arguments" : "too many arguments");
    return cd_get(ctx, vec<str>(a.begin() + 1, a.end()));

  break; default:
    return builtin_error(nam, "invalid option: " + a[0]);
  }
}

// ---- compvalues -------------------------------------------------------------------

// An argument definition: its description and action.
struct CaArg {
  str descr, action, opt;
};

struct CvVal {
  str          name;
  opt<str>     descr;
  vec<str>     xorl;
  enum Type : int { NoArg, Arg, Opt } type = NoArg;
  opt<CaArg>   arg;
  bool         active = true;
};

struct CvDef {
  str           descr;
  bool          hassep = false;
  char          sep = 0;
  char          argsep = '=';
  bool          words = false;
  s::deque<CvVal> vals;

  CvVal* get(strv name) {
    for (auto& v : vals)
      if (v.name == name) return &v;
    return nullptr;
  }
  // The value named by `name` with its quoting off.
  CvVal* quote_get(strv name) { return get(comp_unquote(name, 0)); }
  void inactive(const vec<str>& names) {
    for (const auto& n : names)
      if (CvVal* v = get(n)) v->active = false;
  }
};

struct CvState {
  sptr<CvDef> d;
  opt<CaArg>  def;
  CvVal*      val = nullptr;
  vec<str>    vals;
};

CvState& cv_state() {
  static CvState st;
  return st;
}
bool CvParsed = false;

// parse_caarg for a value: the description up to a colon, then the action.
CaArg parse_caarg(strv name, strv def) {
  CaArg ret;
  ret.opt = str(name);
  s::size_t k = 0;
  for (; k < def.size() and def[k] != ':'; ++k)
    if (def[k] == '\\' and k + 1 < def.size()) ++k;
  ret.descr  = rembslashcolon(def.substr(0, k));
  ret.action = k < def.size() ? rembslashcolon(def.substr(k + 1)) : str{};
  return ret;
}

sptr<CvDef> parse_cvdef(strv nam, const vec<str>& in) {
  s::size_t a = 0;
  auto ret = s::make_shared<CvDef>();
  while (a + 1 < in.size() and in[a].size() == 2 and in[a][0] == '-'
         and (in[a][1] == 's' or in[a][1] == 'S' or in[a][1] == 'w')) {
    if (in[a][1] == 's') {
      ret->hassep = true;
      ret->sep = in[a + 1].empty() ? '\0' : in[a + 1][0];
      a += 2;
    } else if (in[a][1] == 'S') {
      ret->argsep = in[a + 1].empty() ? '\0' : in[a + 1][0];
      a += 2;
    } else {
      ret->words = true;
      ++a;
    }
  }
  if (a + 1 >= in.size()) {
    builtin_error(nam, "not enough arguments");
    return nullptr;
  }
  ret->descr = in[a++];
  for (; a < in.size(); ++a) {
    const str& arg = in[a];
    s::size_t p = 0;
    vec<str> xorl;
    bool has_xor = false, bs = false;
    if (arg.starts_with('(')) {
      has_xor = true;
      while (p < arg.size() and arg[p] != ')') {
        for (++p; p < arg.size() and (arg[p] == ' ' or arg[p] == '\t'); ++p) {}
        if (p < arg.size() and arg[p] == ')') break;
        s::size_t q = p++;
        for (; p < arg.size() and arg[p] != ')' and arg[p] != ' ' and arg[p] != '\t'; ++p) {}
        if (p >= arg.size()) break;
        xorl.push_back(arg.substr(q, p - q));
      }
      if (p >= arg.size() or arg[p] != ')') {
        builtin_error(nam, "invalid argument: " + arg);
        return nullptr;
      }
      ++p;
    }
    bool multi = p < arg.size() and arg[p] == '*';
    if (multi) ++p;
    s::size_t name_at = p;
    for (; p < arg.size() and arg[p] != ':' and arg[p] != '['; ++p)
      if (arg[p] == '\\' and p + 1 < arg.size()) {
        ++p;
        bs = true;
      }
    str name = arg.substr(name_at, p - name_at);
    if (ret->hassep and not ret->sep and name_at + (bs ? 1 : 0) + 1 < p) {
      builtin_error(nam, "no multi-letter values with empty separator allowed");
      return nullptr;
    }
    char c = p < arg.size() ? arg[p] : '\0';
    opt<str> descr;
    if (c == '[') {
      s::size_t d = ++p;
      for (; p < arg.size() and arg[p] != ']'; ++p)
        if (arg[p] == '\\' and p + 1 < arg.size()) ++p;
      if (p >= arg.size()) {
        builtin_error(nam, "invalid value definition: " + arg);
        return nullptr;
      }
      descr = arg.substr(d, p - d);
      ++p;
      c = p < arg.size() ? arg[p] : '\0';
    }
    if (c and c != ':') {
      builtin_error(nam, "invalid value definition: " + arg);
      return nullptr;
    }
    CvVal val;
    if (c == ':') {
      if (ret->hassep and not ret->sep) {
        builtin_error(nam, "no value with argument with empty separator allowed");
        return nullptr;
      }
      ++p;
      if (p < arg.size() and arg[p] == ':') {
        ++p;
        val.type = CvVal::Opt;
      } else {
        val.type = CvVal::Arg;
      }
      val.arg = parse_caarg(name, strv(arg).substr(p));
    }
    if (not multi) xorl.push_back(name);
    (void)has_xor;
    val.name  = name;
    val.descr = descr;
    val.xorl  = s::move(xorl);
    ret->vals.push_back(s::move(val));
  }
  return ret;
}

// The next value in a buffer from `s` (zsh's cv_next): its definition,
// where the one after it starts (null at the end) and its argument, if
// any.
struct CvNext {
  CvVal* val  = nullptr;
  char*  next = nullptr;
  char*  arg  = nullptr;
};

CvNext cv_next(CvDef& d, char* s) {
  CvVal* r = nullptr;
  char *next = nullptr, *arg = nullptr;
  if (not *s) return {};
  if ((d.hassep and not d.sep) or not d.argsep) {
    char ec = d.hassep and d.sep ? d.sep : d.argsep;
    char* v = s;
    do {
      char sav = *++s;
      *s = '\0';
      r = d.quote_get(v);
      *s = sav;
      if (r) break;
    } while (*s and *s != ec);
    char* os = s;
    if (d.hassep and d.sep) {
      if ((s = strchr(s, d.sep))) next = s + 1;
      else                        next = nullptr;
    } else {
      next = s;
    }
    if (d.argsep and *os == d.argsep) {
      arg = os + 1;
      next = nullptr;
    } else if (r and r->type != CvVal::NoArg) {
      arg = os;
    } else {
      arg = nullptr;
    }
    return {r, next, arg};
  }
  if (d.hassep) {
    char *ns = strchr(s, d.sep), *as = nullptr, *sap, sav = 0;
    bool skip = false;
    if (d.argsep and (as = strchr(s, d.argsep)) and (not ns or as <= ns)) {
      arg = as + 1;
      ns = strchr(as + 1, d.sep);
      skip = true;
      sap = as;
    } else {
      arg = nullptr;
      sap = ns;
    }
    if (sap) {
      sav = *sap;
      *sap = '\0';
    }
    if ((not (r = d.quote_get(s)) or r->type == CvVal::NoArg) and skip) ns = as;
    if (sap) *sap = sav;
    next = (not ns or (ns == as and r and r->type != CvVal::NoArg)) ? nullptr : ns + 1;
    return {r, next, arg};
  }
  char *as = strchr(s, d.argsep), *sap, sav = 0;
  next = nullptr;
  if (as) {
    arg = as + 1;
    sap = as;
    sav = *as;
    *sap = '\0';
  } else {
    arg = sap = nullptr;
  }
  r = d.quote_get(s);
  if (sap) *sap = sav;
  return {r, next, arg};
}

void cv_parse_word(CompState& st, const sptr<CvDef>& dp) {
  CvDef& d = *dp;
  CvState state;
  state.d = dp;
  for (auto& v : d.vals) v.active = true;
  CvVal* val = nullptr;
  char* arg = nullptr;
  char* sp = nullptr;
  bool nosfx = false;

  // A value and its argument (up to `end`, or the end) go in vals.
  auto note = [&](CvVal* v, char* a, char* end, const str& rest) {
    state.vals.push_back(v->name);
    if (a) state.vals.push_back(end ? str(a, static_cast<s::size_t>(end - 1 - a)) : str(a) + rest);
    else   state.vals.emplace_back();
  };

  if (d.words and not st.words.empty()) {
    for (s::size_t i = 1; i < st.words.size(); ++i) {
      if (static_cast<s::int64_t>(i) == st.current - 1) continue;
      vec<char> buf(st.words[i].begin(), st.words[i].end());
      buf.push_back('\0');
      for (sp = buf.data(); sp and *sp;) {
        auto nx = cv_next(d, sp);
        sp  = nx.next;
        arg = nx.arg;
        if ((val = nx.val)) {
          note(val, arg, sp, {});
          if (static_cast<s::int64_t>(i) + 1 < st.current) d.inactive(val->xorl);
        }
      }
    }
    val = nullptr;
    arg = nullptr;
  }
  vec<char> pbuf(st.prefix.begin(), st.prefix.end());
  pbuf.push_back('\0');
  char* base = pbuf.data();
  char* pign = base;
  for (sp = base; sp and *sp;) {
    auto nx = cv_next(d, sp);
    sp  = nx.next;
    arg = nx.arg;
    if ((val = nx.val)) {
      if (arg and not sp) nosfx = true;
      note(val, arg, sp, st.suffix);
      d.inactive(val->xorl);
      if (sp) pign = sp;
      else    val->active = true;
    }
  }
  state.val = val;
  if (val and arg and not sp) state.def = val->arg;

  if (not nosfx and d.hassep) {
    s::size_t ign = 0;
    opt<str> more;
    const str& suffix = st.suffix;
    if (not d.sep and (not val or val->type == CvVal::NoArg)) {
      ign = suffix.size();
      more = suffix;
    } else if (d.sep) {
      auto ns = suffix.find(d.sep);
      auto as = d.argsep ? suffix.find(d.argsep) : str::npos;
      if (as != str::npos and (ns == str::npos or as <= ns)) ign = suffix.size() - as;
      else ign = ns == str::npos ? 0 : suffix.size() - ns;
      if (ns != str::npos) more = suffix.substr(ns + 1);
    } else if (d.argsep) {
      auto as = suffix.find(d.argsep);
      if (as != str::npos) ign = suffix.size() - as;
    }
    ignore_prefix(st, static_cast<s::size_t>(pign - base));
    if (ign) ignore_suffix(st, ign);
    if (more) {
      vec<char> mbuf(more->begin(), more->end());
      mbuf.push_back('\0');
      for (char* m = mbuf.data(); m and *m;) {
        auto nx = cv_next(d, m);
        m   = nx.next;
        arg = nx.arg;
        if ((val = nx.val)) {
          note(val, arg, m, st.suffix);
          d.inactive(val->xorl);
        }
      }
    }
  } else if (arg) {
    ignore_prefix(st, static_cast<s::size_t>(arg - base));
  } else {
    ignore_prefix(st, static_cast<s::size_t>(pign - base));
  }
  cv_state() = s::move(state);
}

// compvalues -i spec… | -D descr action | -C subc | -V noarg arg opt | -s sep
//            | -S sep | -d descr | -L name descr action [name] | -v assoc
int builtin_compvalues(vec<str>& args, ExecContext& ctx) {
  strv nam = args[0];
  if (args.size() < 2) return builtin_error(nam, "not enough arguments");
  CompState* st = comp_state(ctx);
  if (not st) return not_in_completion(nam);
  const str& o = args[1];
  if (o.size() != 2 or o[0] != '-') return builtin_error(nam, "invalid argument: " + o);
  if (o[1] != 'i' and not CvParsed) return builtin_error(nam, "no parsed state");
  int min = 0, max = 0;
  switch (o[1]) {
  case 'i': min = 2; max = -1;
  break; case 'D': min = 2; max = 2;
  break; case 'C': case 's': case 'S': case 'd': case 'v': min = 1; max = 1;
  break; case 'V': min = 3; max = 3;
  break; case 'L': min = 3; max = 4;
  break; default: return builtin_error(nam, "invalid option: " + o);
  }
  int n = static_cast<int>(args.size()) - 2;
  if (n < min) return builtin_error(nam, "not enough arguments");
  if (max >= 0 and n > max) return builtin_error(nam, "too many arguments");
  CvState& cs = cv_state();
  auto set = [&](const str& name, const str& v) { (void)ctx.params.set_scalar(name, v); };
  switch (o[1]) {
  case 'i': {
    CvParsed = false;
    auto def = parse_cvdef(nam, vec<str>(args.begin() + 2, args.end()));
    if (not def) return 1;
    cv_parse_word(*st, def);
    CvParsed = true;
    return 0;
  }

  break; case 'D':
    if (not cs.def) return 1;
    set(args[2], cs.def->descr);
    set(args[3], cs.def->action);
    return 0;

  break; case 'C':
    if (not cs.def) return 1;
    set(args[2], cs.def->opt);
    return 0;

  break; case 'V': {
    vec<str> lists[3];
    for (const auto& v : cs.d->vals) {
      if (not v.active) continue;
      lists[v.type == CvVal::NoArg ? 0 : v.type == CvVal::Arg ? 1 : 2].push_back(
          v.descr ? v.name + ":" + *v.descr : v.name);
    }
    for (s::size_t k = 0; k < 3; ++k) (void)ctx.params.set_array(args[2 + k], lists[k]);
    return 0;
  }

  break; case 's':
    if (not cs.d->hassep) return 1;
    set(args[2], str(1, cs.d->sep));
    return 0;

  break; case 'S':
    set(args[2], cs.d->argsep ? str(1, cs.d->argsep) : str{});
    return 0;

  break; case 'd':
    set(args[2], cs.d->descr);
    return 0;

  break; case 'L': {
    CvVal* v = cs.d->get(args[2]);
    if (not v or not v->arg) return 1;
    set(args[3], v->arg->descr);
    set(args[4], v->arg->action);
    if (args.size() > 5) set(args[5], v->name);
    return 0;
  }

  break; default:
    set_hash(ctx, args[2], cs.vals);
    return 0;
  }
}

// ---- comparguments ----------------------------------------------------------------

// The parsed specifications of an _arguments call (zsh's cadef, caopt,
// caarg), kept in a small cache keyed by the arguments, as zsh does: the
// same _arguments call on the next Tab doesn't parse its specs again.
// Sets (`- set …`) are a chain of CaDefs through `snext`.

constexpr int CAO_NEXT = 1, CAO_DIRECT = 2, CAO_ODIRECT = 3, CAO_EQUAL = 4, CAO_OEQUAL = 5;
constexpr int CAA_NORMAL = 1, CAA_OPT = 2, CAA_REST = 3, CAA_RARGS = 4, CAA_RREST = 5;
constexpr int CDF_SEP = 1;   // -S: `--` ends the options

struct CaArgSpec {
  CaArgSpec* next = nullptr;
  str        descr;
  opt<vec<str>> xorl;
  str        action;
  int        type = CAA_NORMAL;
  opt<str>   end;       // the pattern of `*PAT:…`
  opt<str>   optname;   // the option it is an argument of
  int        num = 0;
  int        min = 0;
  bool       direct = false;
  bool       active = true;
  opt<str>   gsname;    // the set or group name, with its `-`
};

struct CaOpt {
  CaOpt*        next = nullptr;
  str           name;
  opt<str>      descr;
  opt<vec<str>> xorl;
  int           type = CAO_NEXT;
  CaArgSpec*    args = nullptr;
  bool          active = true;
  int           num = 0;
  opt<str>      gsname;
  bool          not_ = false;   // `!…`: on the line, but not completed
};

struct CaDef {
  CaArgSpec* new_arg() { return arg_store.emplace_back(s::make_unique<CaArgSpec>()).get(); }
  CaOpt*     new_opt() { return opt_store.emplace_back(s::make_unique<CaOpt>()).get(); }

  sptr<CaDef>   snext;
  CaOpt*        opts = nullptr;
  int           nopts = 0, ndopts = 0, nodopts = 0;
  CaArgSpec*    args = nullptr;
  CaArgSpec*    rest = nullptr;
  vec<str>      defs;
  s::time_t     lastt = 0;
  bool          has_single = false;
  s::array<CaOpt*, 188> single{};
  str           match;
  bool          argsactive = true;
  opt<str>      set;
  int           flags = 0;
  opt<str>      nonarg;
  // The nodes the raw pointers above point into.
  vec<uptr<CaOpt>>     opt_store;
  vec<uptr<CaArgSpec>> arg_store;
};

int single_index(char pre, char opt) {
  if (opt <= 0x20 or opt > 0x7e) return -1;
  return opt + (pre == '-' ? -0x21 : 94 - 0x21);
}

str bslashcolon(strv s) {
  str out;
  for (char c : s) {
    if (c == ':') out += '\\';
    out += c;
  }
  return out;
}

// zsh's parse_caarg: the description at def[p] up to an unquoted colon,
// then the action (with `mult`, only up to the next colon).  Returns the
// spec and where the reading stopped: the colon after it, or the end.
s::pair<CaArgSpec*, s::size_t> parse_caarg_spec(CaDef& d, bool mult, int type, int num, int nopt,
                                                const opt<str>& oname, const str& def,
                                                s::size_t p, const opt<str>& set) {
  CaArgSpec* ret = d.new_arg();
  ret->num = num;
  ret->min = num - nopt;
  ret->type = type;
  ret->optname = oname;
  ret->gsname = set;
  s::size_t b = p;
  for (; p < def.size() and def[p] != ':'; ++p)
    if (def[p] == '\\' and p + 1 < def.size()) ++p;
  ret->descr = rembslashcolon(strv(def).substr(b, p - b));
  if (p < def.size()) {
    if (mult) {
      s::size_t a = ++p;
      for (; p < def.size() and def[p] != ':'; ++p)
        if (def[p] == '\\' and p + 1 < def.size()) ++p;
      ret->action = rembslashcolon(strv(def).substr(a, p - a));
    } else {
      ret->action = rembslashcolon(strv(def).substr(p + 1));
      p = def.size();
    }
  }
  return {ret, p};
}

sptr<CaDef> alloc_cadef(const vec<str>* args, bool single, const str& match,
                        const opt<str>& nonarg, int flags) {
  auto ret = s::make_shared<CaDef>();
  ret->nonarg = nonarg;
  if (args) ret->defs = *args;
  ret->lastt = s::time(nullptr);
  ret->has_single = single;
  ret->match = match;
  ret->flags = flags;
  return ret;
}

void set_cadef_opts(CaDef& def) {
  int xnum = 0;
  for (CaArgSpec* a = def.args; a; a = a->next) {
    if (not a->direct) a->min = a->num - xnum;
    if (a->type == CAA_OPT) ++xnum;
  }
}

bool inblank(char c) { return c == ' ' or c == '\t'; }

// zsh's parse_cadef: args[0] is the auto-description, then _arguments'
// own -s -S -A -M options, then the specs.
sptr<CaDef> parse_cadef(strv nam, const vec<str>& in) {
  str match = "r:|[_-]=* r:|=*";
  opt<str> adpre, adsuf, axor, doset, nonarg;
  bool single = false, foreignset = false;
  int anum = 1, flags = 0;
  if (auto pct = in[0].find("%d"); pct != str::npos) {
    adpre = in[0].substr(0, pct);
    adsuf = in[0].substr(pct + 2);
  }
  s::size_t a = 1;
  while (a < in.size() and in[a].size() > 1 and in[a][0] == '-') {
    const str& w = in[a];
    s::size_t q = 1;
    for (; q < w.size(); ++q)
      if (w[q] == 'M' or w[q] == 'A') { q = w.size(); break; }
      else if (w[q] != 's' and w[q] != 'S') break;
    if (q < w.size()) break;
    s::size_t p = 1;
    for (; p < w.size(); ++p) {
      if (w[p] == 's') {
        single = true;
      } else if (w[p] == 'S') {
        flags |= CDF_SEP;
      } else if (w[p] == 'A' or w[p] == 'M') {
        str& dst = w[p] == 'A' ? nonarg.emplace() : match;
        if (p + 1 < w.size()) {
          dst = w.substr(p + 1);
          p = w.size() - 1;
        } else if (a + 1 < in.size()) {
          dst = in[++a];
        } else {
          if (w[p] == 'A') nonarg.reset();
          break;
        }
      }
    }
    if (p < w.size()) break;
    ++a;
  }
  if (a < in.size() and in[a] == ":") ++a;
  if (a >= in.size()) return nullptr;
  auto all = alloc_cadef(&in, single, match, nonarg, flags);
  CaDef* ret = all.get();
  CaOpt** optp = &ret->opts;
  s::size_t sargs = a;
  opt<s::size_t> pendset, curset;
  auto fail = [&](str msg) -> sptr<CaDef> {
    builtin_error(nam, msg);
    return nullptr;
  };
  for (;; ++a) {
    if (a >= in.size()) {
      if (not pendset) break;
      // Start a new set: the common specs again.
      a = sargs;
      doset.reset();
      set_cadef_opts(*ret);
      ret->snext = alloc_cadef(nullptr, single, match, nonarg, flags);
      ret = ret->snext.get();
      optp = &ret->opts;
      anum = 1;
      foreignset = false;
      curset = pendset;
      pendset.reset();
    }
    if (in[a] == "-" and a + 1 < in.size()) {
      if ((foreignset = curset and a != *curset)) {
        if (not pendset and a > *curset) pendset = a;
        ++a;
      } else {
        str p = in[++a];
        if (p.size() > 1 and p.front() == '(' and p.back() == ')') {
          p = p.substr(1, p.size() - 2);
          axor = p;
        } else {
          axor.reset();
        }
        if (p.empty()) return fail("empty set name");
        ret->set = doset = p + "-";
        curset = a;
      }
      continue;
    }
    if (in[a] == "+" and a + 1 < in.size()) {
      foreignset = false;
      str p = in[++a];
      if (p.size() > 1 and p.front() == '(' and p.back() == ')') {
        p = p.substr(1, p.size() - 2);
        axor = p;
      } else {
        axor.reset();
      }
      if (p.empty()) return fail("empty group name");
      doset = p + "-";
      continue;
    }
    if (foreignset) continue;
    const str& def = in[a];
    s::size_t p = 0;
    int xnum = 0;
    bool not_ = def[0] == '!';
    if (not_) ++p;
    opt<vec<str>> xorl;
    if (p < def.size() and def[p] == '(') {
      vec<str> list;
      while (p < def.size() and def[p] != ')') {
        for (++p; p < def.size() and inblank(def[p]); ++p) {}
        if (p < def.size() and def[p] == ')') break;
        s::size_t q = p++;
        for (; p < def.size() and def[p] != ')' and not inblank(def[p]); ++p) {}
        if (p >= def.size()) break;
        list.push_back(def.substr(q, p - q));
        ++xnum;
      }
      if (p >= def.size() or def[p] != ')') return fail("invalid argument: " + def);
      if (doset and axor) {
        ++xnum;
        list.push_back(*axor);
      }
      xorl = s::move(list);
      ++p;
    } else if (doset and axor) {
      xnum = 1;
      xorl = vec<str>{*axor};
    }
    char c0 = p < def.size() ? def[p] : '\0';
    char c1 = p + 1 < def.size() ? def[p + 1] : '\0';
    if (c0 == '-' or c0 == '+' or (c0 == '*' and (c1 == '-' or c1 == '+'))) {
      // An option.
      str spec = def.substr(p);
      int again = 0;
      opt<str> againp;
      for (;;) {
        s::size_t q = 0;
        bool multi = q < spec.size() and spec[q] == '*';
        if (multi) ++q;
        auto at = [&](s::size_t k) { return k < spec.size() ? spec[k] : '\0'; };
        str name_buf;
        s::size_t name_at;
        if (((at(q) == '-' and at(q + 1) == '+') or (at(q) == '+' and at(q + 1) == '-'))
            and at(q + 2) and at(q + 2) != ':' and at(q + 2) != '[' and at(q + 2) != '='
            and at(q + 2) != '-' and at(q + 2) != '+') {
          // `-+x` or `+-x`: the whole spec twice, as `+x` and `-x` (the
          // second without a leading `*`, as in zsh).
          againp = spec.substr(q);
          ++q;
          spec[q] = again ? '-' : '+';
          ++again;
          name_at = q;
        } else {
          name_at = q;
          if (at(q) == '-' and at(q + 1) == '-') ++q;
        }
        if (not at(q + 1)) return fail("invalid argument: " + def);
        for (++q; at(q) and at(q) != ':' and at(q) != '['
                  and ((at(q) != '-' and at(q) != '+') or (at(q + 1) != ':' and at(q + 1) != '['))
                  and (at(q) != '='
                       or (at(q + 1) != ':' and at(q + 1) != '[' and at(q + 1) != '-'));
             ++q)
          if (at(q) == '\\' and at(q + 1)) ++q;
        name_buf = spec.substr(name_at, q - name_at);
        int otype = CAO_NEXT;
        char c = at(q);
        if (c == '-') {
          otype = CAO_DIRECT;
          c = at(++q);
        } else if (c == '+') {
          otype = CAO_ODIRECT;
          c = at(++q);
        } else if (c == '=') {
          otype = CAO_OEQUAL;
          if ((c = at(++q)) == '-') {
            otype = CAO_EQUAL;
            c = at(++q);
          }
        }
        opt<str> descr;
        if (c == '[') {
          s::size_t b = ++q;
          for (; at(q) and at(q) != ']'; ++q)
            if (at(q) == '\\' and at(q + 1)) ++q;
          if (not at(q)) return fail("invalid option definition: " + def);
          descr = spec.substr(b, q - b);
          c = at(++q);
        }
        if (c and c != ':') return fail("invalid option definition: " + def);
        opt<vec<str>> oxor = xorl;
        if (not multi) {
          if (not oxor) oxor.emplace();
          if (oxor->size() > static_cast<s::size_t>(xnum))
            oxor->resize(static_cast<s::size_t>(xnum));
          oxor->push_back(rembslashcolon(name_buf));
        }
        CaArgSpec* oargs = nullptr;
        if (c == ':') {
          CaArgSpec** oargp = &oargs;
          int oanum = 1, onum = 0;
          while (c == ':') {
            bool rest = false;
            opt<str> end;
            int atype;
            if (at(++q) == ':') {
              atype = CAA_OPT;
              ++q;
            } else if (at(q) == '*') {
              if (at(++q) != ':') {
                s::size_t b = q++;
                for (; at(q) and at(q) != ':'; ++q)
                  if (at(q) == '\\' and at(q + 1)) ++q;
                end = spec.substr(b, q - b);
              }
              if (at(q) != ':') return fail("invalid option definition: " + def);
              if (at(++q) == ':') {
                if (at(++q) == ':') {
                  atype = CAA_RREST;
                  ++q;
                } else {
                  atype = CAA_RARGS;
                }
              } else {
                atype = CAA_REST;
              }
              rest = true;
            } else {
              atype = CAA_NORMAL;
            }
            auto [arg, next] = parse_caarg_spec(*all, not rest, atype, oanum++, onum, name_buf,
                                                spec, q, doset);
            *oargp = arg;
            q = next;
            if (atype == CAA_OPT) ++onum;
            if (end) (*oargp)->end = end;
            oargp = &(*oargp)->next;
            if (rest) break;
            c = at(q);
          }
        }
        CaOpt* o = all->new_opt();
        *optp = o;
        optp = &o->next;
        o->gsname = doset;
        o->name = rembslashcolon(name_buf);
        if (descr) {
          o->descr = descr;
        } else if (adpre and oargs and not oargs->next) {
          bool blank = s::all_of(oargs->descr.begin(), oargs->descr.end(),
                                 [](char ch) { return ch == ' ' or ch == '\t' or ch == '\n'; });
          if (not blank) o->descr = *adpre + oargs->descr + *adsuf;
        }
        o->xorl = oxor;
        o->type = otype;
        o->args = oargs;
        o->num = ret->nopts++;
        o->not_ = not_;
        if (otype == CAO_DIRECT or otype == CAO_EQUAL) ++ret->ndopts;
        else if (otype == CAO_ODIRECT or otype == CAO_OEQUAL) ++ret->nodopts;
        if (single and name_buf.size() == 2 and name_buf[1] != '-') {
          int sidx = single_index(name_buf[0], name_buf[1]);
          if (sidx >= 0) ret->single[static_cast<s::size_t>(sidx)] = o;
        }
        if (again != 1) break;
        spec = *againp;
      }
    } else if (c0 == '*') {
      // The rest arguments.
      if (not_) continue;
      int type = CAA_REST;
      if (c1 != ':') return fail("invalid rest argument definition: " + def);
      if (ret->rest) return fail("doubled rest argument definition: " + def);
      p += 2;
      if (p < def.size() and def[p] == ':') {
        if (p + 1 < def.size() and def[p + 1] == ':') {
          type = CAA_RREST;
          p += 2;
        } else {
          type = CAA_RARGS;
          ++p;
        }
      }
      auto [arg, next] = parse_caarg_spec(*all, false, type, -1, 0, s::nullopt, def, p, doset);
      ret->rest = arg;
      p = next;
      ret->rest->xorl = xorl;
    } else {
      // A normal argument.
      if (not_) continue;
      int type = CAA_NORMAL;
      bool direct = p < def.size() and s::isdigit(static_cast<unsigned char>(def[p]));
      if (direct) {
        int num = 0;
        while (p < def.size() and s::isdigit(static_cast<unsigned char>(def[p])))
          num = num * 10 + (def[p++] - '0');
        anum = num + 1;
      } else {
        ++anum;
      }
      if (p >= def.size() or def[p] != ':') return fail("invalid argument: " + def);
      if (++p < def.size() and def[p] == ':') {
        type = CAA_OPT;
        ++p;
      }
      auto [arg, next] = parse_caarg_spec(*all, false, type, anum - 1, 0, s::nullopt, def, p,
                                          doset);
      p = next;
      arg->xorl = xorl;
      arg->direct = direct;
      CaArgSpec *tmp = ret->args, *pre = nullptr;
      for (; tmp and tmp->num < anum - 1; pre = tmp, tmp = tmp->next) {}
      if (tmp and tmp->num == anum - 1) return fail("doubled argument definition: " + def);
      arg->next = tmp;
      if (pre) pre->next = arg;
      else     ret->args = arg;
    }
  }
  set_cadef_opts(*ret);
  return all;
}

constexpr s::size_t MAX_CACACHE = 8;

s::array<sptr<CaDef>, MAX_CACACHE>& cadef_cache() {
  static s::array<sptr<CaDef>, MAX_CACACHE> cache;
  return cache;
}

// The cached parse of `args`, or a new one in place of the least recently
// used.
sptr<CaDef> get_cadef(strv nam, const vec<str>& args) {
  auto& cache = cadef_cache();
  s::size_t min = 0;
  for (s::size_t i = 0; i < MAX_CACACHE; ++i) {
    if (cache[i] and cache[i]->defs == args) {
      cache[i]->lastt = s::time(nullptr);
      return cache[i];
    }
    if (not cache[i]) {
      min = i;
      break;
    }
    if (cache[i]->lastt < cache[min]->lastt) min = i;
  }
  auto fresh = parse_cadef(nam, args);
  if (fresh) cache[min] = fresh;
  return fresh;
}

// zsh's ca_get_opt: the option `line` is (with `full`), or begins with.
// `end` gets where the option ends in it.
CaOpt* ca_get_opt(CaDef& d, strv line, bool full, s::size_t* end) {
  for (CaOpt* p = d.opts; p; p = p->next)
    if (p->active and p->name == line) {
      if (end) *end = line.size();
      return p;
    }
  if (full) return nullptr;
  for (CaOpt* p = d.opts; p; p = p->next) {
    if (not p->active) continue;
    bool hit = (not p->args or p->type == CAO_NEXT) ? p->name == line
                                                     : line.starts_with(p->name);
    if (not hit) continue;
    s::size_t l = p->name.size();
    if ((p->type == CAO_OEQUAL or p->type == CAO_EQUAL) and l < line.size() and line[l] != '=')
      continue;
    if (end) {
      if ((p->type == CAO_OEQUAL or p->type == CAO_EQUAL) and l < line.size() and line[l] == '=')
        ++l;
      *end = l;
    }
    return p;
  }
  return nullptr;
}

// The same for clustered single-letter options; `lp` gets the options
// before the last that take their argument in the next word.
CaOpt* ca_get_sopt(CaDef& d, strv line, s::size_t* end, vec<CaOpt*>& lp) {
  char pre = line[0];
  CaOpt *p = nullptr, *pp = nullptr;
  lp.clear();
  s::size_t i = 1;
  for (; i < line.size(); ++i) {
    int sidx = single_index(pre, line[i]);
    if (sidx != -1 and (p = d.single[static_cast<s::size_t>(sidx)]) and p->active and p->args) {
      if (p->type == CAO_NEXT) {
        lp.push_back(p);
      } else {
        if (end) {
          ++i;
          if ((p->type == CAO_OEQUAL or p->type == CAO_EQUAL) and i < line.size()
              and line[i] == '=')
            ++i;
          *end = i;
        }
        pp = p;
        break;
      }
    } else if (not p or not p->active) {
      return nullptr;
    }
    pp = p->name[0] == pre ? p : nullptr;
    p = nullptr;
  }
  if (pp and end and i >= line.size()) *end = i;
  return pp;
}

// ca_get_sopt with zsh's `pe` left alone (null) unless it says.
CaOpt* sopt_at(CaDef& d, strv line, opt<s::size_t>& pe, vec<CaOpt*>& sopts) {
  s::size_t e = strv::npos;
  CaOpt* o = ca_get_sopt(d, line, &e, sopts);
  if (e != strv::npos) pe = e;
  return o;
}

bool ca_foreign_opt(const CaDef* curset, const CaDef* all, strv option) {
  for (const CaDef* d = all; d; d = d->snext.get()) {
    if (d == curset) continue;
    for (CaOpt* p = d->opts; p; p = p->next)
      if (p->name == option) return true;
  }
  return false;
}

CaArgSpec* ca_get_arg(CaDef& d, int n) {
  if (not d.argsactive) return nullptr;
  CaArgSpec* a = d.args;
  while (a and (not a->active or n < a->min or n > a->num)) {
    if (not a->active) ++n;
    a = a->next;
  }
  if (a and a->min <= n and a->num >= n and a->active) return a;
  return d.rest and d.rest->active ? d.rest : nullptr;
}

bool gs_is(const opt<str>& gsname, strv grp, s::size_t extra) {
  return gsname and gsname->size() == grp.size() + extra and gsname->starts_with(grp);
}

// zsh's ca_inactive: what `xorl` (or, with `opts`, all the options)
// excludes is no longer active, from word `cur` on.
void ca_inactive(CaDef& d, const vec<str>* xorl, int cur, bool opts, s::int64_t compcurrent) {
  if (not (xorl or opts) or cur > compcurrent) return;
  // The current word may be the start of a longer one: only single
  // letters are excluded by it (clustering).
  bool single = not opts and cur == compcurrent;
  s::size_t n = opts ? 1 : xorl->size();
  for (s::size_t k = 0; k < n; ++k) {
    str x = opts ? str("-") : (*xorl)[k];
    bool excludeall = false;
    opt<str> grp;
    s::size_t sep = 0;
    while (sep < x.size() and x[sep] != '+' and x[sep] != '-' and x[sep] != ':' and x[sep] != '*'
           and not s::isdigit(static_cast<unsigned char>(x[sep]))) {
      auto next = x.find('-', sep);
      if (next == str::npos or next + 1 >= x.size()) {
        excludeall = true;
        sep = x.size();
        break;
      }
      sep = next + 1;
    }
    if (sep > 0) {
      grp = x.substr(0, sep);
      x = x.substr(sep);
    }
    s::size_t extra = excludeall ? 1 : 0;
    auto in_grp = [&](const opt<str>& gs) { return not grp or gs_is(gs, *grp, extra); };
    if (excludeall or x == ":") {
      if (grp) {
        for (CaArgSpec* a = d.args; a; a = a->next)
          if (gs_is(a->gsname, *grp, extra)) a->active = false;
        if (d.rest and gs_is(d.rest->gsname, *grp, extra)) d.rest->active = false;
      } else {
        d.argsactive = false;
      }
    }
    if (excludeall or x == "-") {
      for (CaOpt* p = d.opts; p; p = p->next)
        if (in_grp(p->gsname) and not (single and p->name.size() > 2)) p->active = false;
    }
    if (excludeall or x == "*") {
      if (d.rest and in_grp(d.rest->gsname)) d.rest->active = false;
    }
    if (not excludeall) {
      auto starts = [&](const opt<str>& gs) { return not grp or (gs and gs->starts_with(*grp)); };
      if (not x.empty() and s::isdigit(static_cast<unsigned char>(x[0]))) {
        int num = s::atoi(x.c_str());
        CaArgSpec* a = d.args;
        while (a and a->num < num) a = a->next;
        if (a and a->num == num and starts(a->gsname)) a->active = false;
      } else if (CaOpt* o = ca_get_opt(d, x, true, nullptr);
                 o and starts(o->gsname) and not (single and o->name.size() > 2)) {
        o->active = false;
      }
      if (opts) break;
    }
  }
}

// An option's argument in the same word, past the option's name (zsh's
// ca_opt_arg, which skips quoting in the word).
str ca_opt_arg(const CaOpt& opt, strv line) {
  const str& o = opt.name;
  s::size_t oi = 0, li = 0;
  for (;;) {
    if (oi < o.size() and o[oi] == '\\') ++oi;
    if (li < line.size() and (line[li] == '\\' or line[li] == '\'' or line[li] == '"')) ++li;
    if (oi >= o.size() or li >= line.size() or o[oi] != line[li]) break;
    ++oi;
    ++li;
  }
  if (li < line.size() and (opt.type == CAO_EQUAL or opt.type == CAO_OEQUAL)) {
    if (line[li] == '\\') ++li;
    if (li < line.size() and line[li] == '=') ++li;
  }
  return str(line.substr(li));
}

// What parsing the line against one set found (zsh's castate).
struct CaState {
  sptr<CaDef> d;
  CaArgSpec* def = nullptr;
  CaArgSpec* ddef = nullptr;
  CaOpt*     curopt = nullptr;
  CaOpt*     dopt = nullptr;
  int optflag = 1, arg = 1, argbeg = 1, optbeg = 1, nargbeg = 1, restbeg = 1;
  int curpos = 0, argend = 0, inopt = 0, inarg = 1, nth = 1, singles = 0, oopt = 0;
  int actopts = 1;
  vec<str>           args;
  vec<opt<vec<str>>> oargs;
};

struct CaParse {
  vec<CaState> states;   // the first is zsh's ca_laststate, then the other sets
  bool parsed = false;
  int  doff = 0;
};

CaParse& ca_parse() {
  static CaParse p;
  return p;
}

// zsh's ca_parse_line: the words on the line against set `d`; the state
// for the cursor goes into `last`.  True when the set doesn't fit the
// options on the line.
bool ca_parse_line(const sptr<CaDef>& dp, const CaDef* all, bool multi, CompState& st,
                   CaState& last, int& doff_out, bool extended) {
  CaDef& d = *dp;
  const vec<str>& words = st.words;
  s::int64_t compcurrent = st.current;
  auto word = [&](int i) -> const str* {
    return i >= 0 and static_cast<s::size_t>(i) < words.size() ? &words[static_cast<s::size_t>(i)]
                                                                : nullptr;
  };
  for (CaOpt* p = d.opts; p; p = p->next) p->active = true;
  d.argsactive = true;
  if (d.rest) d.rest->active = true;
  for (CaArgSpec* a = d.args; a; a = a->next) a->active = true;

  CaState state;
  state.d = dp;
  state.argend = static_cast<int>(words.size()) - 1;
  int argend = state.argend;
  state.inopt = state.singles = state.oopt = 0;
  state.curpos = static_cast<int>(compcurrent);
  state.oargs.assign(static_cast<s::size_t>(d.nopts), s::nullopt);
  last = state;

  // zsh's ca_laststate shares the lists of words with the state being
  // parsed: they end up with everything the parse put in them.
  auto finish = [&] {
    last.args  = state.args;
    last.oargs = state.oargs;
    last.actopts = 0;
    for (CaOpt* p = d.opts; p; p = p->next)
      if (p->active) ++last.actopts;
  };
  if (not word(1)) {
    last.optflag = last.arg = 0;
    finish();
    return false;
  }
  opt<CompiledPattern> napat, endpat;
  if (d.nonarg) napat = CompiledPattern::compile(*d.nonarg, false, extended);
  const vec<str>* argxor = nullptr;
  vec<CaOpt*> sopts;
  int doff = 0;
  int cur = 2;
  const str* linep = word(1);
  state.curopt = nullptr;
  state.def = nullptr;
  for (; linep; linep = word(cur++)) {
    str line = *linep;
    CaArgSpec *ddef = nullptr, *adef = nullptr;
    CaOpt *dopt = nullptr, *wasopt = nullptr;
    state.singles = 0;
    bool arglast = false;
    if (argxor) {
      ca_inactive(d, argxor, cur - 1, false, compcurrent);
      argxor = nullptr;
    }
    if ((d.flags & CDF_SEP) and cur != compcurrent and state.actopts and line == "--") {
      ca_inactive(d, nullptr, cur, true, compcurrent);
      state.actopts = 0;
      continue;
    }
    bool to_cont = false;
    if (state.def) {
      state.arg = 0;
      if (state.curopt) {
        auto& l = state.oargs[static_cast<s::size_t>(state.curopt->num)];
        if (not l) l.emplace();
        l->push_back(line);
      }
      if ((state.optflag = state.def->type == CAA_OPT) and state.def->optname) ++state.oopt;
      if (state.def->type == CAA_REST or state.def->type == CAA_RARGS
          or state.def->type == CAA_RREST) {
        if (state.def->end and endpat and endpat->matches(line)) {
          state.def = nullptr;
          state.curopt = nullptr;
          state.optflag = state.arg = 1;
          state.argend = last.argend = cur - 1;
          to_cont = true;
        }
      } else if ((state.def = state.def->next)) {
        state.argbeg = cur;
        state.argend = argend;
      } else if (not sopts.empty()) {
        state.curopt = sopts.front();
        sopts.erase(sopts.begin());
        state.def = state.curopt->args;
        state.optflag = 0;
        state.argbeg = state.optbeg = state.inopt = cur;
        state.argend = argend;
        doff = 0;
        state.singles = 1;
        auto& l = state.oargs[static_cast<s::size_t>(state.curopt->num)];
        if (not l) l.emplace();
        to_cont = true;
      } else {
        state.curopt = nullptr;
        state.optflag = 1;
      }
    } else {
      state.optflag = state.arg = 1;
      state.curopt = nullptr;
    }
    if (not to_cont) {
      if (state.optflag) state.optflag = line.empty() ? 0 : line.size() > 1 ? 2 : 1;
      // Where the option ends in the word; null as zsh's pe when nothing
      // said.
      opt<s::size_t> pe;
      bool is_dash = not line.empty() and (line[0] == '-' or line[0] == '+');
      auto eq_before = [&] { return pe and *pe > 0 and line[*pe - 1] == '='; };
      auto rest_here = [&] { return pe and *pe < line.size(); };
      bool long_opt = false;
      if (state.optflag == 2 and is_dash) {
        s::size_t e = 0;
        state.curopt = ca_get_opt(d, line, false, &e);
        if (state.curopt) pe = e;
        CaOpt* o = state.curopt;
        long_opt = o and (o->type == CAO_OEQUAL ? word(cur) != nullptr or eq_before()
                          : o->type == CAO_EQUAL ? eq_before() or not rest_here() : true);
      }
      if (long_opt) {
        if ((ddef = state.def = (state.curopt->type != CAO_EQUAL or eq_before())
                                    ? state.curopt->args : nullptr))
          dopt = state.curopt;
        doff = static_cast<int>(*pe);
        state.optbeg = state.argbeg = state.inopt = cur;
        state.argend = argend;
        state.singles = d.has_single and not rest_here() and state.curopt->name.size() == 2
                        and state.curopt->name[1] != '-';
        auto& l = state.oargs[static_cast<s::size_t>(state.curopt->num)];
        if (not l) l.emplace();
        if (state.curopt->xorl) ca_inactive(d, &*state.curopt->xorl, cur, false, compcurrent);
        if (state.def and (state.curopt->type == CAO_DIRECT or state.curopt->type == CAO_EQUAL
                           or (state.curopt->type == CAO_ODIRECT and rest_here())
                           or (state.curopt->type == CAO_OEQUAL
                               and (rest_here() or eq_before())))) {
          if (state.def->type != CAA_REST and state.def->type != CAA_RARGS
              and state.def->type != CAA_RREST)
            state.def = state.def->next;
          l->push_back(ca_opt_arg(*state.curopt, line));
        }
        if (state.def) {
          state.optflag = 0;
        } else {
          if (not d.has_single or state.curopt->name.size() > 2) wasopt = state.curopt;
          state.curopt = nullptr;
        }
      } else if (state.optflag == 2 and d.has_single and is_dash
                 and ((state.curopt = sopt_at(d, line, pe, sopts))
                      or (cur != compcurrent and not sopts.empty()))) {
        // Clustered single-letter options.
        if (cur != compcurrent and not sopts.empty()) {
          state.curopt = sopts.front();
          sopts.erase(sopts.begin());
        }
        auto& l = state.oargs[static_cast<s::size_t>(state.curopt->num)];
        if (not l) l.emplace();
        state.def = state.curopt->args;
        ddef = state.curopt->type == CAO_NEXT and cur == compcurrent ? nullptr : state.def;
        dopt = state.curopt;
        doff = pe ? static_cast<int>(*pe) : 0;
        state.optbeg = state.argbeg = state.inopt = cur;
        state.argend = argend;
        state.singles = not rest_here();
        for (s::size_t k = 1; pe and k < *pe; ++k) {
          int sidx = single_index(line[0], line[k]);
          if (sidx == -1) continue;
          if (CaOpt* tmp = d.single[static_cast<s::size_t>(sidx)]) {
            auto& tl = state.oargs[static_cast<s::size_t>(tmp->num)];
            if (not tl) tl.emplace();
            if (tmp->xorl) ca_inactive(d, &*tmp->xorl, cur, false, compcurrent);
          }
        }
        if (state.def and (state.curopt->type == CAO_DIRECT or state.curopt->type == CAO_EQUAL
                           or (state.curopt->type == CAO_ODIRECT and rest_here())
                           or (state.curopt->type == CAO_OEQUAL
                               and (rest_here() or eq_before())))) {
          if (state.def->type != CAA_REST and state.def->type != CAA_RARGS
              and state.def->type != CAA_RREST)
            state.def = state.def->next;
          l->push_back(ca_opt_arg(*state.curopt, line));
        }
        if (state.def) state.optflag = 0;
        else           state.curopt = nullptr;
      } else if (multi and is_dash and cur != compcurrent and ca_foreign_opt(&d, all, line)) {
        return true;
      } else if (state.arg and cur <= compcurrent) {
        // A normal argument.  -A's pattern: an unknown option to go past.
        if (napat and cur < compcurrent and state.actopts) {
          if (napat->matches(line)) continue;
          ca_inactive(d, nullptr, cur + 1, true, compcurrent);
          state.actopts = 0;
        }
        arglast = true;
        if (state.inopt) {
          state.inopt = 0;
          state.nargbeg = cur - 1;
          state.argend = argend;
        }
        if (not d.args and not d.rest and not line.empty() and line[0] != '-' and line[0] != '+') {
          if (not multi and cur > compcurrent) break;
          return true;
        }
        if ((adef = state.def = ca_get_arg(d, state.nth))
            and (state.def->type == CAA_RREST or state.def->type == CAA_RARGS)) {
          // The rest: an earlier definition for the current word wins;
          // otherwise the remaining words are the rest's.
          if (last.def) break;
          state.optflag = cur == state.nargbeg + 1
                      and (not multi or line.empty() or line[0] == '-' or line[0] == '+');
          state.optbeg = state.nargbeg;
          state.argbeg = cur - 1;
          state.argend = argend;
          for (; linep; linep = word(cur++)) state.args.push_back(*linep);
          last = state;
          last.ddef = nullptr;
          last.dopt = nullptr;
          break;
        }
        state.args.push_back(line);
        if (adef) state.oopt = adef->num - state.nth;
        if (state.def and cur != compcurrent and state.def->xorl) argxor = &*state.def->xorl;
        if (state.def and state.def->type != CAA_NORMAL and state.def->type != CAA_OPT
            and state.inarg) {
          state.restbeg = cur;
          state.inarg = 0;
        } else if (not state.def or state.def->type == CAA_NORMAL or state.def->type == CAA_OPT) {
          state.inarg = 1;
        }
        ++state.nth;
        state.def = nullptr;
      }
      // The end pattern of `*PAT:…`.
      if (state.def and state.curopt
          and (state.def->type == CAA_RREST or state.def->type == CAA_RARGS)) {
        if (state.def->end) {
          endpat = CompiledPattern::compile(*state.def->end, false, extended);
        } else {
          auto& l = state.oargs[static_cast<s::size_t>(state.curopt->num)];
          if (cur < compcurrent) last = state;
          if (not l) l.emplace();
          for (; linep; linep = word(cur++)) l->push_back(*linep);
          last.ddef = nullptr;
          last.dopt = nullptr;
          break;
        }
      } else if (state.def and state.def->end) {
        endpat = CompiledPattern::compile(*state.def->end, false, extended);
      }
    }
    // cont: the state for the word at the cursor.
    if (cur + 1 == compcurrent) {
      last = state;
      last.ddef = nullptr;
      last.dopt = nullptr;
    } else if (cur == compcurrent and not last.def) {
      if ((last.def = ddef)) {
        last.singles = state.singles;
        if (state.curopt and state.curopt->type == CAO_NEXT) {
          last.ddef = ddef;
          last.dopt = dopt;
          last.def = nullptr;
          last.optflag = 1;
          state.curopt->active = true;
        } else {
          doff_out = doff;
          last.optflag = 0;
        }
      } else {
        last.def = adef;
        last.optflag = not arglast or not multi or line.empty() or line[0] == '-' or line[0] == '+';
        last.ddef = nullptr;
        last.dopt = nullptr;
        last.optbeg = state.nargbeg;
        last.argbeg = state.restbeg;
        last.argend = state.argend;
        last.singles = state.singles;
        last.oopt = state.oopt;
        if (wasopt) wasopt->active = true;
      }
    }
  }
  finish();
  return false;
}

// zsh's restrict_range: the words from `b` to `e` (0-based) only.
void restrict_range(CompState& st, int b, int e) {
  int wl = static_cast<int>(st.words.size()) - 1;
  if (wl and b >= 0 and e >= 0 and (b > 0 or e < wl)) {
    if (e > wl) e = wl;
    vec<str> w(st.words.begin() + b, st.words.begin() + e + 1);
    st.words = s::move(w);
    st.current -= b;
  }
}

// zsh's ca_set_data: the descriptions, actions and subcontexts of the
// arguments that may come at the cursor, from `arg` on.
void ca_set_data(CompState& st, const CaState& last, vec<str>& descr, vec<str>& act,
                 vec<str>& subc, opt<str> optname, CaArgSpec* arg, CaOpt* optdef, bool single) {
  bool restr = false;
  int miss = 0, oopt = 1, lopt = 0;
  for (;;) {
    int addopt = optname ? 0 : last.oopt;
    while (arg and (optname or arg->num < 0
                    or (arg->min <= last.nth + addopt and arg->num >= last.nth))) {
      lopt = arg->type == CAA_OPT;
      if (not optname and not lopt and oopt > 0) oopt = 0;
      bool seen = false;
      for (s::size_t k = 0; k < descr.size(); ++k)
        if (descr[k] == arg->descr and act[k] == arg->action) { seen = true; break; }
      // With an ignored prefix, no normal arguments are completed.
      if (single and not arg->optname) return;
      if (not seen) {
        descr.push_back(arg->descr);
        act.push_back(arg->action);
        if (not restr) {
          if ((restr = arg->type == CAA_RARGS))
            restrict_range(st, last.optbeg, last.argend);
          else if ((restr = arg->type == CAA_RREST))
            restrict_range(st, last.argbeg, last.argend);
        }
        str gs = arg->gsname.value_or("");
        if (arg->optname) {
          if (arg->num > 0 and arg->type < CAA_REST)
            subc.push_back(gs + "option" + *arg->optname + "-" + s::to_string(arg->num));
          else
            subc.push_back(gs + "option" + *arg->optname + "-rest");
        } else if (arg->num > 0) {
          subc.push_back(gs + "argument-" + s::to_string(arg->num));
        } else {
          subc.push_back(gs + "argument-rest");
        }
      }
      // An argument the option says must be in this word: no normal
      // arguments then.
      if (arg->type == CAA_NORMAL and optname and optdef
          and (optdef->type == CAO_NEXT or optdef->type == CAO_ODIRECT
               or optdef->type == CAO_OEQUAL))
        return;
      if (single) break;
      CaDef& d = *last.d;
      CaArgSpec* rest = d.rest and d.rest->active ? d.rest : nullptr;
      if (not optname) {
        if (arg->num >= 0 and not arg->next and miss) {
          arg = rest;
        } else {
          int onum = arg->num;
          bool rst = onum != arg->min and onum == last.nth;
          if ((arg = arg->next)) {
            if (arg->num != onum + 1) miss = 1;
          } else if (rst or (oopt > 0 and not optname)) {
            arg = rest;
            oopt = -1;
          }
        }
      } else {
        if (not lopt) break;
        arg = arg->next;
      }
    }
    if (not single and optname and (lopt or last.oopt)) {
      optname.reset();
      arg = ca_get_arg(*last.d, last.nth);
      continue;
    }
    if (not optname and oopt > 0) {
      oopt = -1;
      arg = last.d->rest and last.d->rest->active ? last.d->rest : nullptr;
      continue;
    }
    return;
  }
}

str ca_nullist(const opt<vec<str>>& l) {
  str out;
  if (not l) return out;
  for (s::size_t k = 0; k < l->size(); ++k) {
    if (k) out += '\0';
    out += (*l)[k];
  }
  return out;
}

str ca_colonlist(const opt<vec<str>>& l) {
  str out;
  if (not l) return out;
  for (s::size_t k = 0; k < l->size(); ++k) {
    if (k) out += ':';
    for (char c : (*l)[k]) {
      if (c == ':' or c == '\\') out += '\\';
      out += c;
    }
  }
  return out;
}

// comparguments -i autodescr [-sSAM…] specs… | -D descr act subc | -O next
// direct odirect equal | -L opt descr act subc | -s var | -M var | -a |
// -W line opt_args nul | -n var: _arguments' engine (zsh's
// bin_comparguments).
int builtin_comparguments(vec<str>& args, ExecContext& ctx) {
  strv nam = args[0];
  CompState* st = comp_state(ctx);
  if (not st) return not_in_completion(nam);
  if (args.size() < 2) return builtin_error(nam, "not enough arguments");
  const str& o = args[1];
  if (o.size() != 2 or o[0] != '-') return builtin_error(nam, "invalid argument: " + o);
  CaParse& cp = ca_parse();
  if (o[1] != 'i' and o[1] != 'I' and not cp.parsed) return builtin_error(nam, "no parsed state");
  int min, max;
  switch (o[1]) {
    case 'i': min = 2; max = -1;
    break; case 'D': min = 3; max = 3;
    break; case 'O': min = 4; max = 4;
    break; case 'L': min = 3; max = 4;
    break; case 's': case 'M': case 'n': min = 1; max = 1;
    break; case 'a': min = 0; max = 0;
    break; case 'W': min = 3; max = 3;
    break; default: return builtin_error(nam, "invalid option: " + o);
  }
  int n = static_cast<int>(args.size()) - 2;
  if (n < min) return builtin_error(nam, "not enough arguments");
  if (max >= 0 and n > max) return builtin_error(nam, "too many arguments");
  vec<str> a(args.begin() + 2, args.end());
  auto set_list = [&](const str& name, vec<str> v) {
    (void)ctx.params.set_array(name, s::move(v));
  };
  switch (o[1]) {
    case 'i': {
      if (st->current <= 1 or st->words.empty()) return 1;
      bool was = cp.parsed;
      cp.parsed = false;
      auto all = get_cadef(nam, a);
      if (not all) return 1;
      cp.parsed = was;
      cp.doff = 0;
      bool multi = all->snext != nullptr;
      bool extended = ctx.opts.extended_glob();
      vec<CaState> states;   // the sets that fit, last first, as zsh's list
      CaState last;
      int ret = 0;
      for (sptr<CaDef> def = all; def;) {
        CaState here;
        bool use = not ca_parse_line(def, all.get(), multi, *st, here, cp.doff, extended);
        def = def->snext;
        if (use and def) {
          states.insert(states.begin(), s::move(here));
        } else if (use) {
          last = s::move(here);
          last.d = last.d;
        } else if (not def) {
          if (not states.empty()) {
            last = s::move(states.front());
            states.erase(states.begin());
          } else {
            last = s::move(here);
            ret = 1;
          }
        }
      }
      cp.states.clear();
      cp.states.push_back(s::move(last));
      for (auto& s1 : states) cp.states.push_back(s::move(s1));
      cp.parsed = true;
      return ret;
    }

    break; case 'D': {
      vec<str> descr, act, subc;
      int ret = 1;
      ignore_prefix(*st, static_cast<s::size_t>(cp.doff));
      for (auto& ls : cp.states) {
        if (CaArgSpec* arg = ls.def) {
          ret = 0;
          ca_set_data(*st, cp.states.front(), descr, act, subc, arg->optname, arg, ls.curopt,
                      cp.doff > 0);
        }
      }
      if (not ret) {
        set_list(a[0], s::move(descr));
        set_list(a[1], s::move(act));
        set_list(a[2], s::move(subc));
      }
      return ret;
    }

    break; case 'O': {
      vec<str> lists[4];
      int ret = 1;
      for (auto& ls : cp.states) {
        CaArgSpec* def = ls.def;
        bool want = ls.actopts
                    and (ls.optflag or (cp.doff and def)
                         or (def and def->optname
                             and (def->type == CAA_OPT
                                  or (def->type >= CAA_RARGS and def->num < 0))))
                    and (not def or def->type < CAA_RARGS
                         or (def->type == CAA_RARGS ? ls.curpos == ls.argbeg + 1
                                                    : st->current == 1));
        if (not want) continue;
        ret = 0;
        for (CaOpt* p = ls.d->opts; p; p = p->next) {
          if (not p->active or p->not_) continue;
          auto& l = lists[p->type == CAO_NEXT ? 0 : p->type == CAO_DIRECT ? 1
                          : p->type == CAO_ODIRECT ? 2 : 3];
          str s1 = bslashcolon(p->name);
          if (p->descr) s1 += ":" + *p->descr;
          if (s::find(l.begin(), l.end(), s1) == l.end()) l.push_back(s1);
        }
      }
      if (not ret) {
        for (int k = 0; k < 4; ++k) set_list(a[static_cast<s::size_t>(k)], s::move(lists[k]));
        return 0;
      }
      return cp.states.front().singles ? 2 : 1;
    }

    break; case 'L': {
      vec<str> descr, act, subc;
      int ret = 1;
      for (auto& ls : cp.states) {
        CaOpt* opt = ca_get_opt(*ls.d, a[0], true, nullptr);
        if (opt and opt->args) {
          ret = 0;
          ca_set_data(*st, cp.states.front(), descr, act, subc, opt->name, opt->args, opt, true);
        }
      }
      if (not ret) {
        set_list(a[1], s::move(descr));
        set_list(a[2], s::move(act));
        if (a.size() > 3) set_list(a[3], s::move(subc));
      }
      return ret;
    }

    break; case 's':
      for (auto& ls : cp.states) {
        if (ls.d->has_single and ls.singles and ls.actopts) {
          str v;
          if (ls.ddef and ls.dopt)
            v = ls.dopt->type == CAO_DIRECT ? "direct"
              : ls.dopt->type == CAO_OEQUAL or ls.dopt->type == CAO_EQUAL ? "equal" : "next";
          (void)ctx.params.set_scalar(a[0], v);
          return 0;
        }
      }
      return 1;

    break; case 'M':
      (void)ctx.params.set_scalar(a[0], cp.states.front().d->match);
      return 0;

    break; case 'a':
      for (auto& ls : cp.states)
        if (ls.d->args or ls.d->rest) return 0;
      return 1;

    break; case 'W': {
      bool nul = not a[2].empty() and a[2][0] != '0';
      vec<str> line, pairs;
      for (auto& ls : cp.states) line.insert(line.end(), ls.args.begin(), ls.args.end());
      set_list(a[0], s::move(line));
      for (auto& ls : cp.states) {
        s::size_t k = 0;
        for (CaOpt* p = ls.d->opts; p; p = p->next, ++k) {
          if (k >= ls.oargs.size() or not ls.oargs[k]) continue;
          pairs.push_back(p->gsname.value_or("") + p->name);
          pairs.push_back(nul ? ca_nullist(ls.oargs[k]) : ca_colonlist(ls.oargs[k]));
        }
      }
      set_hash(ctx, a[1], s::move(pairs));
      return 0;
    }

    break; case 'n':
      ctx.params.assign(a[0], static_cast<s::int64_t>(cp.states.front().optbeg
                                                       + (ctx.opts.get(OPT_KSH_ARRAYS) ? 0 : 1)),
                        PM_INTEGER);
      return 0;

    break; default:
      return 1;
  }
  return 1;
}

// ---- comptags / comptry -------------------------------------------------------------

struct CtSet {
  vec<str>  tags;
  opt<str>  tag;      // the last tag -A was asked for
  s::size_t ptr = 0;  // where -A goes on in tags
};

struct CTags {
  vec<str>        all;
  str             context;
  bool            init = true;
  s::deque<CtSet> sets;
};

constexpr int MAX_TAGS = 256;

s::array<opt<CTags>, MAX_TAGS>& tag_levels() {
  static s::array<opt<CTags>, MAX_TAGS> levels;
  return levels;
}
int LastTagLevel = 0;

// Whether `a` has `s`; with `colon`, comparing only up to a colon in
// either.
bool arrcontains(const vec<str>& a, strv s, bool colon) {
  for (const auto& e : a) {
    if (colon) {
      s::size_t k = 0;
      while (k < s.size() and k < e.size() and s[k] != ':' and e[k] != ':' and s[k] == e[k]) ++k;
      if ((k == s.size() or s[k] == ':') and (k == e.size() or e[k] == ':')) return true;
    } else if (e == s) {
      return true;
    }
  }
  return false;
}

// comptags -i context tags… | -C param | -T | -N | -R tag | -S param | -A tag param [param]
int builtin_comptags(vec<str>& args, ExecContext& ctx) {
  strv nam = args[0];
  if (args.size() < 2) return builtin_error(nam, "not enough arguments");
  if (not comp_state(ctx)) return not_in_completion(nam);
  const str& o = args[1];
  if (o.size() < 2 or o[0] != '-' or (o.size() > 2 and (o[2] != '-' or o.size() > 3)))
    return builtin_error(nam, "invalid argument: " + o);
  int level = ctx.func_depth - (o.size() > 2 ? 1 : 0);
  if (level >= MAX_TAGS) return builtin_error(nam, "nesting level too deep");
  if (level < 0) level = 0;
  auto& levels = tag_levels();
  auto& here = levels[static_cast<s::size_t>(level)];
  if (o[1] != 'i' and o[1] != 'I' and not here) return builtin_error(nam, "no tags registered");
  int min = 0, max = 0;
  switch (o[1]) {
  case 'i': min = 2; max = -1;
  break; case 'C': case 'R': case 'S': min = 1; max = 1;
  break; case 'T': case 'N': min = 0; max = 0;
  break; case 'A': min = 2; max = 3;
  break; default: return builtin_error(nam, "invalid option: " + o);
  }
  int n = static_cast<int>(args.size()) - 2;
  if (n < min) return builtin_error(nam, "not enough arguments");
  if (max >= 0 and n > max) return builtin_error(nam, "too many arguments");
  switch (o[1]) {
  case 'i':
    here = CTags{vec<str>(args.begin() + 3, args.end()), args[2], true, {}};
    LastTagLevel = level;
    return 0;

  break; case 'C':
    (void)ctx.params.set_scalar(args[2], here->context);
    return 0;

  break; case 'T':
    return here->sets.empty() ? 1 : 0;

  break; case 'N':
    if (here->init)               here->init = false;
    else if (not here->sets.empty()) here->sets.pop_front();
    return here->sets.empty() ? 1 : 0;

  break; case 'R':
    return not here->sets.empty() and arrcontains(here->sets.front().tags, args[2], true) ? 0 : 1;

  break; case 'A': {
    if (here->sets.empty()) return 1;
    CtSet& s1 = here->sets.front();
    const str& tag = args[2];
    if (not s1.tag or *s1.tag != tag) {
      s1.tag = tag;
      s1.ptr = 0;
    }
    s::size_t q = s1.ptr;
    opt<s::size_t> v;   // where the value starts in tags[q]
    for (; q < s1.tags.size(); ++q) {
      const str& t = s1.tags[q];
      if (t.starts_with(tag)) {
        if (t.size() == tag.size()) {
          v = 0;
          break;
        }
        if (t[tag.size()] == ':') {
          v = tag.size() + 1;
          break;
        }
      }
    }
    if (not v) {
      s1.tag.reset();
      return 1;
    }
    s1.ptr = q + 1;
    const str& t = s1.tags[q];
    str value = t.substr(*v);
    (void)ctx.params.set_scalar(args[3], value.starts_with('-') ? tag + value : value);
    if (args.size() > 4) {
      auto colon = t.find(':', *v);
      (void)ctx.params.set_scalar(args[4], t.substr(0, colon));
    }
    return 0;
  }

  break; default:
    if (here->sets.empty()) return 1;
    (void)ctx.params.set_array(args[2], here->sets.front().tags);
    return 0;
  }
}

// {a,b} alternatives not quoted with a backslash, expanded.
vec<str> expand_braces(const str& w) {
  int depth = 0;
  s::size_t open = str::npos;
  for (s::size_t k = 0; k < w.size(); ++k) {
    char c = w[k];
    if (c == '\\' and k + 1 < w.size()) {
      ++k;
    } else if (c == '{') {
      if (depth++ == 0) open = k;
    } else if (c == '}' and depth > 0 and --depth == 0) {
      // The alternatives between open and k.
      vec<str> alts;
      s::size_t from = open + 1;
      int d = 0;
      for (s::size_t j = open + 1; j < k; ++j) {
        if (w[j] == '\\' and j + 1 < k) ++j;
        else if (w[j] == '{') ++d;
        else if (w[j] == '}') --d;
        else if (w[j] == ',' and d == 0) {
          alts.push_back(w.substr(from, j - from));
          from = j + 1;
        }
      }
      alts.push_back(w.substr(from, k - from));
      if (alts.size() < 2) continue;
      vec<str> out;
      for (const auto& alt : alts)
        for (auto& e : expand_braces(w.substr(0, open) + alt + w.substr(k + 1)))
          out.push_back(s::move(e));
      return out;
    }
  }
  return {w};
}

bool has_wilds(strv w) {
  for (s::size_t k = 0; k < w.size(); ++k) {
    if (w[k] == '\\') {
      ++k;
      continue;
    }
    if (strchr("*?[]<>()|#^~", w[k])) return true;
  }
  return false;
}

// comptry [-s] tags… | -m specs…
int builtin_comptry(vec<str>& args, ExecContext& ctx) {
  strv nam = args[0];
  if (not comp_state(ctx)) return not_in_completion(nam);
  auto& levels = tag_levels();
  auto& here = levels[static_cast<s::size_t>(LastTagLevel)];
  if (not LastTagLevel or not here) return builtin_error(nam, "no tags registered");
  if (args.size() < 2) return 0;
  bool ext = ctx.opts.get(OPT_EXTENDEDGLOB);
  if (args[1] == "-m") {
    // As in zsh, what each argument finds adds to the list, and the list
    // so far becomes a set after each.
    vec<str> list;
    for (s::size_t a = 2; a < args.size(); ++a) {
      const str& spec = args[a];
      for (s::size_t k = 0; k < spec.size();) {
        while (k < spec.size() and (spec[k] == ' ' or spec[k] == '\t' or spec[k] == '\n')) ++k;
        // One word: backslashes off but before brace characters; the
        // first colon starts what follows each tag found.
        str q;
        opt<s::size_t> c;
        for (; k < spec.size() and spec[k] != ' ' and spec[k] != '\t' and spec[k] != '\n'; ++k) {
          if (not c and spec[k] == ':') c = q.size();
          if (spec[k] == '\\' and k + 1 < spec.size()) ++k;
          q += spec[k];
        }
        if (k < spec.size()) ++k;
        if (q.empty()) continue;
        str pat = c ? q.substr(0, *c) : q;
        str rest = c ? q.substr(*c + 1) : str{};
        vec<str> pats = expand_braces(pat);
        bool braces = pats.size() > 1 or pats[0] != pat;
        if (has_wilds(pat) or braces) {
          for (const auto& one : pats) {
            auto prog = CompiledPattern::compile(one, false, ext);
            for (const auto& t : here->all) {
              if (arrcontains(list, t, true)) continue;
              if (prog.matches(t)) list.push_back(c ? t + ":" + rest : t);
            }
          }
        } else if (arrcontains(here->all, pat, false)) {
          bool used = s::any_of(here->sets.begin(), here->sets.end(),
                                [&](const CtSet& s1) { return arrcontains(s1.tags, pat, false); });
          if (not used) list.push_back(c ? pat + ":" + rest : pat);
        }
      }
      if (not list.empty()) here->sets.push_back(CtSet{list, s::nullopt, 0});
    }
    return 0;
  }
  s::size_t a = 1;
  bool sep = args[a] == "-s";
  if (sep) ++a;
  vec<str> tags;
  for (; a < args.size(); ++a) {
    if (not arrcontains(here->all, args[a], true)) continue;
    bool used = s::any_of(here->sets.begin(), here->sets.end(),
                          [&](const CtSet& s1) { return arrcontains(s1.tags, args[a], false); });
    if (not used) tags.push_back(args[a]);
  }
  if (tags.empty()) return 0;
  if (sep) {
    for (auto& t : tags) here->sets.push_back(CtSet{{t}, s::nullopt, 0});
  } else {
    here->sets.push_back(CtSet{s::move(tags), s::nullopt, 0});
  }
  return 0;
}

// ---- compfiles ------------------------------------------------------------------

bool true_word(strv w) { return w == "true" or w == "yes" or w == "on" or w == "1"; }
bool pattern_char(char c) { return strchr("#^*()|[]<>?~\\", c) != nullptr; }

str quote_pattern(strv w) {
  str out;
  for (char c : w) {
    if (pattern_char(c)) out += '\\';
    out += c;
  }
  return out;
}

opt<vec<str>> cfp_test_exact(ExecContext& ctx, const CompState& st, const vec<str>& names,
                             const opt<vec<str>>& accept, const str& skipped) {
  if (st.prefix.empty() and st.suffix.empty()) return s::nullopt;
  if (not accept or accept->empty()) return s::nullopt;
  const vec<str>& acc = *accept;
  if (acc.size() == 1 and (acc[0] == "false" or acc[0] == "no" or acc[0] == "off" or acc[0] == "0"))
    return s::nullopt;
  opt<vec<CompiledPattern>> alist;
  if (acc.size() > 1 or not true_word(acc[0])) {
    alist.emplace();
    for (const auto& p : acc) {
      if (p == "*") {
        alist.reset();
        break;
      }
      alist->push_back(CompiledPattern::compile(p, false, ctx.opts.get(OPT_EXTENDEDGLOB)));
    }
  }
  str suf = skipped + rembslash(st.prefix + st.suffix);
  vec<str> ret;
  for (const auto& n : names) {
    str buf = n + suf;
    struct stat sb{};
    if (::stat(buf.c_str(), &sb) != 0) continue;
    if (alist and s::none_of(alist->begin(), alist->end(),
                             [&](const CompiledPattern& p) { return p.matches(buf); }))
      continue;
    ret.push_back(buf);
  }
  if (ret.empty()) return s::nullopt;
  return ret;
}

// A pattern matching `add` or, at each place, what the matcher there
// lets the line's character stand for.
str cfp_matcher_range(const vec<const Matcher*>& ms, const str& add) {
  str out;
  for (s::size_t k = 0; k < add.size(); ++k) {
    char ch = add[k];
    int addc = static_cast<unsigned char>(ch);
    const Matcher* m = k < ms.size() ? ms[k] : nullptr;
    if (not m) {
      out += ch;
    } else if (m->flags & Matcher::Right) {
      out += '*';
      out += ch;
    } else if (m->line->tp == CPattern::Equiv and m->word->tp == CPattern::Equiv) {
      out += '[';
      out += ch;
      int ind = 0, mt = 0;
      if (cclass_match(m->line->items, addc, &ind, &mt)) {
        int eq = cpattern_equivalence(m->word, ind + 1, mt, addc);
        if (eq >= 0) out += static_cast<char>(eq);
      }
      out += ']';
    } else {
      int mt = 0;
      switch (m->word->tp) {
      case CPattern::NClass:
        out += "[^" + cclass_string(m->word->items) + "]";

      break; case CPattern::CClass: case CPattern::Equiv: case CPattern::Char: {
        out += '[';
        bool addadd = not cpattern_match1(m->word, addc, &mt);
        if (addadd and ch == ']') out += ch;
        if (m->word->tp == CPattern::Char) out += static_cast<char>(m->word->chr);
        else                               out += cclass_string(m->word->items);
        if (addadd and ch != ']') out += ch;
        out += ']';
      }

      break; default:
        out += '?';
      }
    }
  }
  return out;
}

opt<str> cfp_matcher_pats(const str& matcher, str add) {
  str err;
  auto spec = MatcherSpec::parse(matcher, err);
  if (not spec or not spec->first()) return add;
  vec<const Matcher*> ms(add.size(), nullptr);
  for (const Matcher* m = spec->first(); m and not add.empty(); m = m->next) {
    const CPattern* stopp = nullptr;
    int stopl = 0;
    if (not (m->flags & (Matcher::Left | Matcher::Right))) {
      if (m->llen == 1 and m->wlen == 1) {
        for (s::size_t k = 0; k < add.size(); ++k) {
          if (cpattern_match(m->line, add.c_str() + k)) {
            if (ms[k]) {
              add.resize(k);
              break;
            }
            ms[k] = m;
          }
        }
      } else {
        stopp = m->line;
        stopl = m->llen;
      }
    } else if (m->flags & Matcher::Right) {
      if (m->wlen < 0 and not m->llen and m->ralen == 1) {
        for (s::size_t k = 0; k < add.size(); ++k) {
          if (cpattern_match(m->right, add.c_str() + k)) {
            if (ms[k] or (k == 0 and add[0] == '.')) {
              add.resize(k);
              break;
            }
            ms[k] = m;
          }
        }
      } else if (m->llen) {
        stopp = m->line;
        stopl = m->llen;
      } else {
        stopp = m->right;
        stopl = m->ralen;
      }
    } else {
      if (not m->lalen) return str{};
      stopp = m->left;
      stopl = m->lalen;
    }
    if (stopp) {
      for (s::size_t k = 0; static_cast<int>(add.size() - k) >= stopl and k < add.size(); ++k) {
        if (cpattern_match(stopp, add.c_str() + k)) {
          add.resize(k);
          break;
        }
      }
    }
  }
  if (not add.empty()) return cfp_matcher_range(ms, add);
  return add;
}

// The prefix on the line, quoted for a pattern and cut where a pattern
// in `pats` doesn't stand for it literally, goes in front of the `*`
// patterns ("optimise" them: zsh's cfp_opt_pats).
void cfp_opt_pats(const CompState& st, vec<str>& pats, const str& matcher) {
  const str& pre = st.prefix;
  if (pre.empty()) return;
  if (auto pm = st.keys.find("pattern_match"); pm != st.keys.end() and not pm->second.empty())
    if (has_wilds(rembslash(pre + st.suffix))) return;
  str add;
  for (s::size_t i = 0; i < pre.size(); ++i) {
    char c = pre[i];
    char nx = i + 1 < pre.size() ? pre[i + 1] : '\0';
    if (c != '\\' or not nx or strchr("*?<>()[]|#^~=", nx)) {
      if ((i == 0 or pre[i - 1] != '\\') and strchr("*?<>()[]|#^~=", c)) add += '\\';
      add += c;
    }
  }
  auto cut = [&](auto stop) {
    s::size_t k = 0;
    while (k < add.size() and not stop(add[k])) ++k;
    add.resize(k);
  };
  for (s::size_t pi = 0; not add.empty() and pi < pats.size(); ++pi) {
    str q = pats[pi];
    if (q.empty()) continue;
    if (q.size() > 1 and q.back() == ')') {
      s::size_t t = q.size() - 2;
      for (; t > 0; --t)
        if (strchr(")|~(", q[t])) break;
      if (t != 0 and q[t] == '(') q.resize(t);
    }
    for (s::size_t k = 0; k < q.size() and not add.empty(); ++k) {
      char c = q[k];
      if (c == '\\' and k + 1 < q.size()) {
        char x = q[++k];
        cut([&](char a) { return a == x; });
      } else if (c == '<') {
        cut([](char a) { return s::isdigit(static_cast<unsigned char>(a)) != 0; });
      } else if (c == '[') {
        s::size_t x = ++k;
        if (x < q.size() and (q[x] == '!' or q[x] == '^')) ++x;
        for (; x < q.size(); ++x) {
          if (x + 2 < q.size() and q[x + 1] == '-') {
            char c1 = q[x], c2 = q[x + 2];
            // zsh compares the class character, not add's (sic).
            char cx = q[x];
            cut([&](char) { return not (cx < c1 or cx > c2); });
          } else {
            char cx = q[x];
            cut([&](char a) { return a == cx; });
          }
        }
      } else if (not strchr("?*()|~#", c)) {
        cut([&](char a) { return a == c; });
      }
    }
  }
  if (add.empty()) return;
  if (not matcher.empty()) {
    auto m = cfp_matcher_pats(matcher, add);
    if (not m) return;
    add = *m;
  }
  for (auto& p : pats)
    if (p.starts_with('*')) p = add + p;
}

vec<str> cfp_bld_pats(ExecContext& ctx, const CompState& st, const vec<str>& names,
                      const str& skipped, const vec<str>& pats) {
  vec<str> ret;
  bool dot = not ctx.opts.get(OPT_GLOBDOTS) and st.prefix.starts_with('.');
  for (const auto& o : names)
    for (const auto& p : pats) {
      ret.push_back(o + skipped + p);
      if (dot and not p.starts_with('.')) ret.push_back(o + skipped + "." + p);
    }
  return ret;
}

vec<str> cfp_add_sdirs(ExecContext& ctx, const CompState& st, vec<str> final,
                       const vec<str>& orig, const str& skipped, const str& sdirs,
                       const opt<vec<str>>& fake) {
  int add = 0;
  if (not sdirs.empty() and (ctx.opts.get(OPT_GLOBDOTS) or st.prefix.starts_with('.'))) {
    if (true_word(sdirs))   add = 2;
    else if (sdirs == "..") add = 1;
  }
  if (add)
    for (const auto& m : orig) {
      final.push_back(m + skipped + "..");
      if (add == 2) final.push_back(m + skipped + ".");
    }
  if (not fake) return final;
  for (const auto& entry : *fake) {
    // dir:files… — the files are added after the names that are dir.
    str f;
    s::size_t p = 0;
    for (; p < entry.size(); ++p) {
      if (entry[p] == ':') break;
      if (entry[p] == '\\' and p + 1 < entry.size() and entry[p + 1] == ':') ++p;
      f += entry[p];
    }
    if (p >= entry.size() or p + 1 >= entry.size()) continue;
    strv files = strv(entry).substr(p + 1);
    auto prog = CompiledPattern::compile(f, false, ctx.opts.get(OPT_EXTENDEDGLOB));
    for (const auto& m : orig) {
      struct stat st1{}, st2{};
      bool same = prog.matches(m)
               or (::stat(f.c_str(), &st1) == 0 and ::stat(m.empty() ? "." : m.c_str(), &st2) == 0
                   and st1.st_dev == st2.st_dev and st1.st_ino == st2.st_ino);
      if (not same) continue;
      for (s::size_t k = 0; k < files.size();) {
        while (k < files.size() and (files[k] == ' ' or files[k] == '\t')) ++k;
        if (k >= files.size()) break;
        str name;
        for (; k < files.size() and files[k] != ' ' and files[k] != '\t'; ++k) {
          if (files[k] == '\\' and k + 1 < files.size()) ++k;
          name += files[k];
        }
        final.push_back(m + skipped + name);
      }
    }
  }
  return final;
}

// compfiles -p|-p-|-P names accept skipped matcher sdirs fake [pats…]
//         | -i names ignored style path | -r names prefix
int builtin_compfiles(vec<str>& args, ExecContext& ctx) {
  strv nam = args[0];
  if (args.size() < 2) return builtin_error(nam, "not enough arguments");
  CompState* st = comp_state(ctx);
  if (not st) return not_in_completion(nam);
  const str& o = args[1];
  if (not o.starts_with('-')) return builtin_error(nam, "missing option: " + o);
  auto arg = [&](s::size_t k) -> const str* {
    return k + 1 < args.size() ? &args[k + 1] : nullptr;
  };
  char c = o.size() > 1 ? o[1] : '\0';
  if (c == 'p' or c == 'P') {
    if (o.size() > 2 and (o[2] != '-' or o.size() > 3))
      return builtin_error(nam, "invalid option: " + o);
    for (s::size_t k = 1; k <= 6; ++k)
      if (not arg(k)) return builtin_error(nam, "too few arguments");
    if (c == 'p' and not arg(7)) return builtin_error(nam, "too few arguments");
    auto tmp = array_param(ctx, args[2]);
    if (not tmp) {
      builtin_error(nam, "unknown parameter: " + args[2]);
      return 0;
    }
    vec<str> names;
    for (const auto& n : *tmp) names.push_back(quote_pattern(n));
    auto accept = array_param(ctx, args[3]);
    const str& skipped = args[4];
    const str& matcher = args[5];
    const str& sdirs = args[6];
    auto fake = array_param(ctx, args[7]);
    vec<str> result;
    if (auto exact = cfp_test_exact(ctx, *st, names, accept, skipped)) {
      result = cfp_add_sdirs(ctx, *st, s::move(*exact), names, skipped, sdirs, fake);
    } else {
      vec<str> pats = c == 'P' ? vec<str>{"*(-/)"} : vec<str>(args.begin() + 8, args.end());
      if (o.size() == 2) cfp_opt_pats(*st, pats, matcher);
      result = cfp_add_sdirs(ctx, *st, cfp_bld_pats(ctx, *st, names, skipped, pats), names,
                             skipped, sdirs, fake);
    }
    (void)ctx.params.set_array(args[2], s::move(result));
    return 0;
  }
  if (c == 'i') {
    if (o.size() > 2) return builtin_error(nam, "invalid option: " + o);
    for (s::size_t k = 1; k <= 4; ++k)
      if (not arg(k)) return builtin_error(nam, "too few arguments");
    if (arg(5)) return builtin_error(nam, "too many arguments");
    vec<str> ign = array_param(ctx, args[3]).value_or(vec<str>{});
    auto tmp = array_param(ctx, args[2]);
    if (not tmp) {
      builtin_error(nam, "unknown parameter: " + args[2]);
      return 0;
    }
    // Directories that are the current one (pwd) or one of the path's
    // parents (parent) are ignored.
    const str& style = args[4];
    const str& path = args[5];
    bool tpar = style.find("parent") != str::npos;
    bool tpwd = style.find("pwd") != str::npos;
    struct stat est{};
    if (tpwd and ::lstat(".", &est) != 0) tpwd = false;
    if (tpar or tpwd) {
      for (const auto& n : *tmp) {
        struct stat nst{};
        if (::stat(n.c_str(), &nst) != 0 or not S_ISDIR(nst.st_mode)) continue;
        if (tpwd and nst.st_dev == est.st_dev and nst.st_ino == est.st_ino) {
          ign.push_back(comp_quote(n, 0));
          continue;
        }
        if (not tpar or not n.starts_with(path)) continue;
        str cpath = n;
        bool found = false;
        for (;;) {
          auto e = cpath.rfind('/');
          if (e == str::npos or e <= path.size()) break;
          cpath.resize(e);
          struct stat sb{};
          if (::stat(cpath.c_str(), &sb) == 0 and sb.st_dev == nst.st_dev
              and sb.st_ino == nst.st_ino) {
            found = true;
            break;
          }
        }
        if (not found) {
          auto e = cpath.rfind('/');
          struct stat sb{};
          found = e != str::npos and e > path.size() and ::stat(cpath.c_str(), &sb) == 0
              and sb.st_dev == nst.st_dev and sb.st_ino == nst.st_ino;
        }
        if (found) ign.push_back(comp_quote(n, 0));
      }
    }
    (void)ctx.params.set_array(args[3], s::move(ign));
    return 0;
  }
  if (c == 'r') {
    if (not arg(1) or not arg(2)) return builtin_error(nam, "too few arguments");
    if (arg(3)) return builtin_error(nam, "too many arguments");
    auto tmp = array_param(ctx, args[2]);
    if (not tmp) {
      builtin_error(nam, "unknown parameter: " + args[2]);
      return 0;
    }
    // Only the names in the directory the prefix names, if any; 1 when
    // the names differ (in their first component).
    const vec<str>& names = *tmp;
    const str& pre = args[3];
    auto slash = pre.find('/');
    if (slash != str::npos) {
      str dir = pre.substr(0, slash + 1);
      if (s::any_of(names.begin(), names.end(), [&](const str& n) { return n.starts_with(dir); })) {
        vec<str> kept;
        for (const auto& n : names)
          if (n.starts_with(dir)) kept.push_back(n);
        (void)ctx.params.set_array(args[2], s::move(kept));
        return 0;
      }
      if (names.empty()) return 0;
      str first = names[0];
      if (auto q = first.find('/'); q != str::npos) first.resize(q);
      first += '/';
      for (s::size_t k = 1; k < names.size(); ++k)
        if (not names[k].starts_with(first)) return 1;
      return 0;
    }
    for (s::size_t k = 1; k < names.size(); ++k)
      if (names[k] != names[0]) return 1;
    return 0;
  }
  return builtin_error(nam, "invalid option: " + o);
}

// compgroups names…: the groups each name has (sorted or not, with or
// without duplicates), made now so they are listed in this order.
int builtin_compgroups(vec<str>& args, ExecContext& ctx) {
  if (args.size() < 2) return builtin_error(args[0], "not enough arguments");
  CompState* st = comp_state(ctx);
  if (not st) return not_in_completion(args[0]);
  for (s::size_t k = 1; k < args.size(); ++k) {
    // zsh's order: -2V, -1J, -V, -J (the first two again in between).
    for (auto [sorted, dups] : {s::pair{false, 2}, s::pair{true, 1}, s::pair{false, 0},
                                 s::pair{true, 0}}) {
      bool have = s::any_of(st->groups.begin(), st->groups.end(), [&](const CompGroup& g) {
        return g.name == args[k] and g.sorted == sorted and g.dups == dups and not g.by_match;
      });
      if (not have) st->groups.push_back(CompGroup{args[k], sorted, dups, false, {}, {}});
    }
  }
  return 0;
}

}  // namespace

void register_computil_module(ModuleManager& mm) {
  mm.register_builtin_module("zsh/computil", [](ModuleFeatures& out, ExecContext&) {
    out.builtins.push_back(BuiltinDef{"comparguments", builtin_comparguments, BINF_NONE});
    out.builtins.push_back(BuiltinDef{"compdescribe", builtin_compdescribe, BINF_NONE});
    out.builtins.push_back(BuiltinDef{"compfiles",    builtin_compfiles,    BINF_NONE});
    out.builtins.push_back(BuiltinDef{"compgroups",   builtin_compgroups,   BINF_NONE});
    out.builtins.push_back(BuiltinDef{"comptags",     builtin_comptags,     BINF_NONE});
    out.builtins.push_back(BuiltinDef{"comptry",      builtin_comptry,      BINF_NONE});
    out.builtins.push_back(BuiltinDef{"compvalues",   builtin_compvalues,   BINF_NONE});
  });
}

}  // namespace jfsh
