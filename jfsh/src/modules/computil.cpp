// zsh/computil: the builtins the compsys shell functions lean on —
// compdescribe (_describe), compvalues (_values), comptags and comptry
// (_tags, _wanted, _requested), compfiles (_path_files) and compgroups.
// Mirrors Src/Zle/computil.c; compquote (also computil's in zsh) is with
// compadd in src/builtins/complete.cpp, and comparguments (_arguments) is
// for later.  Their state (the parsed descriptions, the tags offered at
// each function level) lives here between calls, as zsh keeps it in
// statics.

#include <jfsh/modules/computil.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>
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
                     [](CdStr* a, CdStr* b) { return a->text < b->text; });
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
    out.builtins.push_back(BuiltinDef{"compdescribe", builtin_compdescribe, BINF_NONE});
    out.builtins.push_back(BuiltinDef{"compfiles",    builtin_compfiles,    BINF_NONE});
    out.builtins.push_back(BuiltinDef{"compgroups",   builtin_compgroups,   BINF_NONE});
    out.builtins.push_back(BuiltinDef{"comptags",     builtin_comptags,     BINF_NONE});
    out.builtins.push_back(BuiltinDef{"comptry",      builtin_comptry,      BINF_NONE});
    out.builtins.push_back(BuiltinDef{"compvalues",   builtin_compvalues,   BINF_NONE});
  });
}

}  // namespace jfsh
