// Completion matching control: a port of zsh's Src/Zle/compmatch.c (match
// specifications, matching a word with them and the cline lists the
// unambiguous string is built from), parse_cmatcher from complete.c,
// add_match_data's cline assembly from compcore.c and cut_cline/cline_str
// from compresult.c.  The code follows zsh's closely, pointers into
// NUL-terminated buffers included, so that its results are zsh's; the
// buffers and clines live in the run's arena until the completion ends.
// Brace expansion in the word ({a,b}) is not handled.

#include <jfsh/editor/compmatch.h>

#include <cctype>
#include <cstring>
#include <cwctype>

#include <algorithm>
#include <deque>

namespace jfsh {

namespace {

// zsh's PP_* character class numbers.
enum : int {
  PP_ALPHA = 1, PP_ALNUM, PP_ASCII, PP_BLANK, PP_CNTRL, PP_DIGIT, PP_GRAPH, PP_LOWER,
  PP_PRINT, PP_PUNCT, PP_SPACE, PP_UPPER, PP_XDIGIT, PP_IDENT, PP_IFS, PP_IFSSPACE,
  PP_WORD, PP_INCOMPLETE, PP_INVALID, PP_UNKWN, PP_RANGE,
};

enum : int {
  CLF_MISS = 1, CLF_DIFF = 2, CLF_SUF = 4, CLF_MID = 8, CLF_NEW = 16, CLF_LINE = 32,
  CLF_JOIN = 64, CLF_MATCHED = 128, CLF_SKIP = 256,
};

constexpr int CHR_INVALID = -1;

int range_type(strv name) {
  static constexpr strv NAMES[] = {
    "alpha", "alnum", "ascii", "blank", "cntrl", "digit", "graph", "lower", "print", "punct",
    "space", "upper", "xdigit", "IDENT", "IFS", "IFSSPACE", "WORD", "INCOMPLETE", "INVALID",
  };
  for (int i = 0; i < static_cast<int>(s::size(NAMES)); ++i)
    if (NAMES[i] == name) return i + 1;
  return PP_UNKWN;
}

bool in_class(int kind, int ch) {
  auto c = static_cast<wint_t>(ch);
  switch (kind) {
  case PP_ALPHA:    return s::iswalpha(c);
  break; case PP_ALNUM:  return s::iswalnum(c);
  break; case PP_ASCII:  return (ch & ~0x7f) == 0;
  break; case PP_BLANK:  return ch == ' ' or ch == '\t';
  break; case PP_CNTRL:  return s::iswcntrl(c);
  break; case PP_DIGIT:  return s::iswdigit(c);
  break; case PP_GRAPH:  return s::iswgraph(c);
  break; case PP_LOWER:  return s::iswlower(c);
  break; case PP_PRINT:  return s::iswprint(c);
  break; case PP_PUNCT:  return s::iswpunct(c);
  break; case PP_SPACE:  return s::iswspace(c);
  break; case PP_UPPER:  return s::iswupper(c);
  break; case PP_XDIGIT: return s::iswxdigit(c);
  break; case PP_IDENT:  return s::iswalnum(c) or ch == '_';
  break; case PP_IFS:    return ch == ' ' or ch == '\t' or ch == '\n' or ch == 0;
  break; case PP_IFSSPACE: return ch == ' ' or ch == '\t' or ch == '\n';
  break; case PP_WORD:   return s::iswalnum(c) or ch == '_';
  break; default:        return false;
  }
}

// zsh's patmatchrange: whether `ch` is in the class; `ind` gets its index
// in it (ranges counting each character) and `mtp` the kind of the item
// that matched.
bool match_range(const vec<CPattern::Item>& items, int ch, int* ind, int* mtp) {
  if (ind) *ind = 0;
  for (const auto& it : items) {
    if (it.kind == 0) {
      if (it.lo == ch) {
        if (mtp) *mtp = 0;
        return true;
      }
    } else {
      if (mtp) *mtp = it.kind;
      if (it.kind == PP_RANGE) {
        if (it.lo <= ch and ch <= it.hi) {
          if (ind) *ind += ch - it.lo;
          return true;
        }
        if (ind and it.lo < it.hi) *ind += it.hi - it.lo;
      } else if (in_class(it.kind, ch)) {
        return true;
      }
    }
    if (ind) ++*ind;
  }
  return false;
}

// zsh's patmatchindex: the item at index `ind`: a character in `chr`, or
// a class in `mtp`.
bool match_index(const vec<CPattern::Item>& items, int ind, int* chr, int* mtp) {
  *chr = CHR_INVALID;
  *mtp = 0;
  for (const auto& it : items) {
    if (it.kind == 0) {
      if (ind == 0) {
        *chr = it.lo;
        return true;
      }
    } else if (it.kind == PP_RANGE) {
      int rdiff = it.hi - it.lo;
      if (rdiff >= ind) {
        *chr = it.lo + ind;
        return true;
      }
      ind -= rdiff;
    } else if (ind == 0) {
      *mtp = it.kind;
      return true;
    }
    if (ind-- == 0) break;
  }
  return false;
}

int lower(int c) { return c >= 0 and c < 256 ? s::tolower(c) : c; }
int upper(int c) { return c >= 0 and c < 256 ? s::toupper(c) : c; }

int pattern_match1(const CPattern* p, int c, int* mtp) {
  *mtp = 0;
  switch (p->tp) {
  case CPattern::CClass:
    return match_range(p->items, c, nullptr, nullptr);

  break; case CPattern::NClass:
    return not match_range(p->items, c, nullptr, nullptr);

  break; case CPattern::Equiv: {
    int ind = 0;
    return match_range(p->items, c, &ind, mtp) ? ind + 1 : 0;
  }

  break; case CPattern::Any:
    return 1;

  break; default:
    return p->chr == c;
  }
}

int pattern_match_equivalence(const CPattern* lp, int wind, int wmtp, int wchr) {
  int lchr = 0, lmtp = 0;
  if (not match_index(lp->items, wind - 1, &lchr, &lmtp)) return CHR_INVALID;
  if (lchr != CHR_INVALID) return lchr;
  if (wmtp == PP_UPPER and lmtp == PP_LOWER) return lower(wchr);
  if (wmtp == PP_LOWER and lmtp == PP_UPPER) return upper(wchr);
  if (wmtp == lmtp) return wchr;
  return CHR_INVALID;
}

// Whether the pattern `p` matches the start of `s` (`wp` and `ws`: the
// word's pattern and string, compared position by position for {...}).
bool pattern_match(const CPattern* p, const char* s, const CPattern* wp, const char* ws) {
  int mt = 0, wmt = 0;
  while (p and wp and *s and *ws) {
    int wc = static_cast<unsigned char>(*ws);
    int wind = pattern_match1(wp, wc, &wmt);
    if (not wind) return false;
    int c = static_cast<unsigned char>(*s);
    if (p->tp != CPattern::Any or wp->tp != CPattern::Any) {
      int ind = pattern_match1(p, c, &mt);
      if (not ind or ind != wind) return false;
      if (mt != wmt) {
        if ((mt == PP_LOWER or mt == PP_UPPER) and (wmt == PP_LOWER or wmt == PP_UPPER)) {
          if (lower(c) != lower(wc)) return false;
        } else {
          return false;
        }
      }
    }
    ++s;
    ++ws;
    p = p->next;
    wp = wp->next;
  }
  while (p and *s) {
    if (not pattern_match1(p, static_cast<unsigned char>(*s), &mt)) return false;
    p = p->next;
    ++s;
  }
  while (wp and ws and *ws) {
    if (not pattern_match1(wp, static_cast<unsigned char>(*ws), &wmt)) return false;
    wp = wp->next;
    ++ws;
  }
  return true;
}

bool pattern_match_restrict(const CPattern* p, const CPattern* wp, const int* wsc, int wsclen,
                            const CPattern* prestrict, int* new_line) {
  int c = 0, mt = 0, wmt = 0;
  while (p and wp and wsclen and prestrict) {
    int wind = pattern_match1(wp, *wsc, &wmt);
    if (not wind) return false;
    if (prestrict->tp == CPattern::Char) {
      c = prestrict->chr;
    } else {
      if (p->tp == CPattern::Char) {
        c = p->chr;
      } else if (p->tp == CPattern::Equiv) {
        if ((c = pattern_match_equivalence(p, wind, wmt, *wsc)) == CHR_INVALID) return false;
      } else {
        c = *wsc;
      }
      if (not pattern_match1(prestrict, c, &mt)) return false;
    }
    if (p->tp != CPattern::Any or wp->tp != CPattern::Any) {
      int ind = pattern_match1(p, c, &mt);
      if (not ind or ind != wind) return false;
      if (mt != wmt) {
        if ((mt == PP_LOWER or mt == PP_UPPER) and (wmt == PP_LOWER or wmt == PP_UPPER)) {
          if (lower(c) != lower(*wsc)) return false;
        } else {
          return false;
        }
      }
    }
    *new_line++ = c;
    prestrict = prestrict->next;
    ++wsc;
    --wsclen;
    p = p->next;
    wp = wp->next;
  }
  while (p and prestrict) {
    if (prestrict->tp == CPattern::Char) {
      c = prestrict->chr;
    } else {
      if (p->tp != CPattern::Char) return false;
      c = p->chr;
      if (not pattern_match1(prestrict, c, &mt)) return false;
    }
    if (not pattern_match1(p, c, &mt)) return false;
    p = p->next;
    *new_line++ = c;
    prestrict = prestrict->next;
  }
  if (prestrict) return false;
  while (wp and wsclen) {
    if (not pattern_match1(wp, *wsc, &wmt)) return false;
    wp = wp->next;
    ++wsc;
    --wsclen;
  }
  return true;
}

bool blank(char c) { return c == ' ' or c == '\t' or c == '\n'; }

}  // namespace

bool cpattern_match(const CPattern* p, const char* s) {
  return pattern_match(p, s, nullptr, nullptr);
}
int  cpattern_match1(const CPattern* p, int c, int* mtp) { return pattern_match1(p, c, mtp); }
int  cpattern_equivalence(const CPattern* lp, int wind, int wmtp, int wchr) {
  return pattern_match_equivalence(lp, wind, wmtp, wchr);
}
bool cclass_match(const vec<CPattern::Item>& items, int ch, int* ind, int* mtp) {
  return match_range(items, ch, ind, mtp);
}

str cclass_string(const vec<CPattern::Item>& items) {
  static constexpr strv NAMES[] = {
    "alpha", "alnum", "ascii", "blank", "cntrl", "digit", "graph", "lower", "print", "punct",
    "space", "upper", "xdigit", "IDENT", "IFS", "IFSSPACE", "WORD", "INCOMPLETE", "INVALID",
  };
  str out;
  for (const auto& it : items) {
    if (it.kind == 0) {
      out += static_cast<char>(it.lo);
    } else if (it.kind == PP_RANGE) {
      out += static_cast<char>(it.lo);
      out += '-';
      out += static_cast<char>(it.hi);
    } else if (it.kind <= static_cast<int>(s::size(NAMES))) {
      out += "[:" + str(NAMES[it.kind - 1]) + ":]";
    }
  }
  return out;
}

// ---- Parsing ---------------------------------------------------------------------

sptr<MatcherSpec> MatcherSpec::parse(strv spec, str& err) {
  auto ret = s::make_shared<MatcherSpec>();
  str buf(spec);
  const char* s = buf.c_str();

  // A class: [...] (negated with ! or ^) or {...}.  Returns where it
  // ends (at the closing character), or the string's end.
  auto parse_class = [&](CPattern& p, const char* iptr) {
    char endchar;
    if (*iptr++ == '[') {
      endchar = ']';
      if ((*iptr == '!' or *iptr == '^') and iptr[1] != ']') {
        p.tp = CPattern::NClass;
        ++iptr;
      } else {
        p.tp = CPattern::CClass;
      }
    } else {
      endchar = '}';
      p.tp = CPattern::Equiv;
    }
    for (const char* optr = iptr; optr == iptr or *optr != endchar; ++optr)
      if (not *optr) return optr;
    bool first = true;
    while (first or *iptr != endchar) {
      const char* nptr = nullptr;
      if (*iptr == '[' and iptr[1] == ':' and (nptr = strchr(iptr + 2, ':')) and nptr[1] == ']') {
        int ch = range_type(strv(iptr + 2, static_cast<s::size_t>(nptr - iptr - 2)));
        iptr = nptr + 2;
        if (ch != PP_UNKWN) p.items.push_back({ch, 0, 0});
      } else {
        const char* ptr1 = iptr++;
        if (*iptr == '-' and iptr[1] and iptr[1] != endchar) {
          ++iptr;
          p.items.push_back({PP_RANGE, static_cast<unsigned char>(*ptr1),
                             static_cast<unsigned char>(*iptr)});
          ++iptr;
        } else {
          p.items.push_back({0, static_cast<unsigned char>(*ptr1), 0});
        }
      }
      first = false;
    }
    return iptr;
  };

  // A pattern up to `e` (a blank when 0); its length in `len`.
  auto parse_pattern = [&](int& len, char e, bool& bad) -> const CPattern* {
    const CPattern* first = nullptr;
    CPattern* last = nullptr;
    len = 0;
    bad = false;
    while (*s and (e ? *s != e : not blank(*s))) {
      CPattern n;
      if (*s == '[' or *s == '{') {
        s = parse_class(n, s);
        if (not *s) {
          bad = true;
          err = "unterminated character class";
          return nullptr;
        }
        ++s;
      } else if (*s == '?') {
        n.tp = CPattern::Any;
        ++s;
      } else if (*s == '*' or *s == '(' or *s == ')' or *s == '=') {
        bad = true;
        err = str("invalid pattern character `") + *s + "'";
        return nullptr;
      } else {
        if (*s == '\\' and s[1]) ++s;
        n.tp  = CPattern::Char;
        n.chr = static_cast<unsigned char>(*s++);
      }
      CPattern& stored = ret->mPatterns.emplace_back(s::move(n));
      if (last) last->next = &stored;
      else      first = &stored;
      last = &stored;
      ++len;
    }
    return first;
  };

  Matcher* prev = nullptr;
  while (*s) {
    while (*s and blank(*s)) ++s;
    if (not *s) break;
    int fl = 0, fl2 = 0;
    switch (*s) {
    case 'b': fl2 = Matcher::Inter; [[fallthrough]];
    case 'l': fl = Matcher::Left;
    break; case 'e': fl2 = Matcher::Inter; [[fallthrough]];
    case 'r': fl = Matcher::Right;
    break; case 'm': fl = 0;
    break; case 'B': fl2 = Matcher::Inter; [[fallthrough]];
    case 'L': fl = Matcher::Left | Matcher::Line;
    break; case 'E': fl2 = Matcher::Inter; [[fallthrough]];
    case 'R': fl = Matcher::Right | Matcher::Line;
    break; case 'M': fl = Matcher::Line;
    break; case 'x': break;
    break; default:
      err = str("unknown match specification character `") + *s + "'";
      return nullptr;
    }
    if (s[1] != ':') {
      err = "missing `:'";
      return nullptr;
    }
    if (*s == 'x') {
      if (s[2] and not blank(s[2])) {
        err = "unexpected pattern following x: specification";
        return nullptr;
      }
      return ret;
    }
    s += 2;
    if (not *s) {
      err = "missing patterns";
      return nullptr;
    }
    const CPattern *left = nullptr, *right = nullptr, *line = nullptr, *word = nullptr;
    int lal = 0, ral = 0, ll = 0, wl = 0;
    bool both = false, bad = false;
    if ((fl & Matcher::Left) and not fl2) {
      left = parse_pattern(lal, '|', bad);
      if (bad) return nullptr;
      if ((both = (*s and s[1] == '|'))) ++s;
      if (not *s or not *++s) {
        err = both ? "missing right anchor" : "missing line pattern";
        return nullptr;
      }
    }
    line = parse_pattern(ll, ((fl & Matcher::Right) and not fl2) ? '|' : '=', bad);
    if (bad) return nullptr;
    if (both) {
      right = line;
      ral = ll;
      line = nullptr;
      ll = 0;
    }
    if ((fl & Matcher::Right) and not fl2 and (not *s or not *++s)) {
      err = "missing right anchor";
      return nullptr;
    } else if (not (fl & Matcher::Right) or fl2) {
      if (not *s) {
        err = "missing word pattern";
        return nullptr;
      }
      ++s;
    }
    if ((fl & Matcher::Right) and not fl2) {
      if (*s == '|') {
        left = line;
        lal = ll;
        line = nullptr;
        ll = 0;
        ++s;
      }
      right = parse_pattern(ral, '=', bad);
      if (bad) return nullptr;
      if (not *s) {
        err = "missing word pattern";
        return nullptr;
      }
      ++s;
    }
    if (*s == '*') {
      if (not (fl & (Matcher::Left | Matcher::Right))) {
        err = "need anchor for `*'";
        return nullptr;
      }
      word = nullptr;
      if (*++s == '*') {
        ++s;
        wl = -2;
      } else {
        wl = -1;
      }
    } else {
      word = parse_pattern(wl, 0, bad);
      if (bad) return nullptr;
      if (not word and not line) {
        err = "need non-empty word or line pattern";
        return nullptr;
      }
    }
    Matcher& m = ret->mMatchers.emplace_back();
    m.flags = fl | fl2;
    m.line  = line;
    m.llen  = ll;
    m.word  = word;
    m.wlen  = wl;
    m.left  = left;
    m.lalen = lal;
    m.right = right;
    m.ralen = ral;
    if (prev) prev->next = &m;
    prev = &m;
  }
  return ret;
}

// ---- Matching ----------------------------------------------------------------------

struct Cline {
  Cline* next   = nullptr;
  int    flags  = 0;
  char*  line   = nullptr;
  int    llen   = 0;
  char*  word   = nullptr;
  int    wlen   = 0;
  char*  orig   = nullptr;
  int    olen   = 0;
  int    slen   = 0;
  Cline* prefix = nullptr;
  Cline* suffix = nullptr;
  int    min    = 0;
  int    max    = 0;
};

struct MatchRun::Impl {
  s::deque<Cline> clines;
  s::deque<str>   strs;
  vec<sptr<MatcherSpec>> specs;            // kept alive for bmatchers
  vec<vec<const Matcher*>> saved_stacks;   // mstack before each compadd
  vec<const Matcher*> mstack;              // innermost first
  vec<const Matcher*> bmatchers;
  Cline sentinel;                          // zsh's (Cline) 1

  str    matchbuf;
  Cline *matchparts = nullptr, *matchlastpart = nullptr;
  Cline *matchsubs  = nullptr, *matchlastsub  = nullptr;

  Cline* joined = nullptr;   // ainfo->line

  struct Cache {
    int            count = -1;
    str            text;
    s::size_t      cursor = 0;
    vec<s::size_t> positions;
  } cache;

  char* keep(strv v) {
    str& stored = strs.emplace_back(v);
    return stored.data();
  }
  // The line's prefix and suffix, kept once for all the words of a
  // compadd call (match_str puts back what it changes in them).
  struct Kept {
    str   text;
    char* buf = nullptr;
  } kept_pfx, kept_sfx;
  char* keep_line(Kept& k, strv v) {
    if (not k.buf or k.text != v) {
      k.text = str(v);
      k.buf  = keep(v);
    }
    return k.buf;
  }
  Cline* get_cline(char* l, int ll, char* w, int wl, char* o, int ol, int fl) {
    Cline& r = clines.emplace_back();
    r.line = l;
    r.llen = ll;
    r.word = w;
    r.wlen = wl;
    r.orig = o;
    r.olen = ol;
    r.flags = fl;
    return &r;
  }

  Cline* cp_cline(Cline* l, bool deep);
  static int  cline_sublen(Cline* l);
  static void cline_setlens(Cline* l, bool both);
  static void cline_matched(Cline* p);
  static Cline* revert_cline(Cline* p);

  void start_match();
  void add_match_str(const Matcher* m, char* l, char* w, int wl, bool sfx);
  void add_match_part(const Matcher* m, char* l, char* w, int wl, char* o, int ol, char* s,
                      int sl, int osl, bool sfx);
  void add_match_sub(const Matcher* m, char* l, int ll, char* w, int wl);
  int  match_str(char* l, char* w, int* rwlp, bool sfx, int test, bool part);
  int  match_parts(char* l, char* w, int n, bool part);
  Cline* bld_parts(char* sp, int len, int plen, Cline** lp, Cline** lprem);
  int  bld_line(const Matcher* mp, int* line, const char* mword, const char* word, int wlen,
                bool sfx);
  opt<str> join_strs(int la, char* sa, int lb, char* sb);
  int  cmp_anchors(Cline* o, Cline* n, bool join);

  struct Cmdata {
    Cline *cl = nullptr, *pcl = nullptr;
    char  *sp = nullptr, *astr = nullptr;
    int    len = 0, alen = 0, olen = 0, line = 0;
  };
  static bool check_cmdata(Cmdata* md, bool sfx);
  static Cline* undo_cmdata(Cmdata* md, bool sfx);
  Cline* join_sub(Cmdata* md, char* sp, int len, int* mlen, bool sfx, bool join);
  static int sub_match(Cmdata* md, char* sp, int len, bool sfx);
  void join_psfx(Cline* ot, Cline* nt, Cline** orest, Cline** nrest, bool sfx);
  void join_mid(Cline* o, Cline* n);
  int  sub_join(Cline* a, Cline* b, Cline* e, bool anew);
  Cline* join_clines(Cline* o, Cline* n);

  Cline* cut_cline(Cline* l, bool hasmatched_, int minmlen_);
  str    cline_str(Cline* l, bool hasmatched_, int minmlen_, s::size_t* csp,
                   vec<s::size_t>* posl);
};

Cline* MatchRun::Impl::cp_cline(Cline* l, bool deep) {
  Cline* r = nullptr;
  Cline** p = &r;
  while (l) {
    Cline* t = &clines.emplace_back(*l);
    if (deep) {
      if (t->prefix) t->prefix = cp_cline(t->prefix, false);
      if (t->suffix) t->suffix = cp_cline(t->suffix, false);
    }
    *p = t;
    p = &t->next;
    l = l->next;
  }
  *p = nullptr;
  return r;
}

int MatchRun::Impl::cline_sublen(Cline* l) {
  int len = (l->flags & CLF_LINE) ? l->llen : l->wlen;
  if (l->olen and not ((l->flags & CLF_SUF) ? l->suffix : l->prefix)) {
    len += l->olen;
  } else {
    for (Cline* p = l->prefix; p; p = p->next) len += (p->flags & CLF_LINE) ? p->llen : p->wlen;
    for (Cline* p = l->suffix; p; p = p->next) len += (p->flags & CLF_LINE) ? p->llen : p->wlen;
  }
  return len;
}

void MatchRun::Impl::cline_setlens(Cline* l, bool both) {
  for (; l; l = l->next) {
    l->min = cline_sublen(l);
    if (both) l->max = l->min;
  }
}

void MatchRun::Impl::cline_matched(Cline* p) {
  for (; p; p = p->next) {
    p->flags |= CLF_MATCHED;
    cline_matched(p->prefix);
    cline_matched(p->suffix);
  }
}

Cline* MatchRun::Impl::revert_cline(Cline* p) {
  Cline* r = nullptr;
  while (p) {
    Cline* n = p->next;
    p->next = r;
    r = p;
    p = n;
  }
  return r;
}

void MatchRun::Impl::start_match() {
  matchbuf.clear();
  matchparts = matchlastpart = matchsubs = matchlastsub = nullptr;
}

void MatchRun::Impl::add_match_str(const Matcher* m, char* l, char* w, int wl, bool sfx) {
  if (m and (m->flags & Matcher::Line)) {
    wl = m->llen;
    w  = l;
  }
  if (wl <= 0) return;
  if (sfx) matchbuf.insert(0, w, static_cast<s::size_t>(wl));
  else     matchbuf.append(w, static_cast<s::size_t>(wl));
}

void MatchRun::Impl::add_match_part(const Matcher* m, char* l, char* w, int wl, char* o, int ol,
                                    char* s, int sl, int osl, bool sfx) {
  if (l and not strncmp(l, w, static_cast<s::size_t>(wl))) l = nullptr;
  Cline *lp = nullptr, *lprem = nullptr;
  Cline* p = bld_parts(s, sl, osl, &lp, &lprem);
  if (lprem and m and (m->flags & Matcher::Left)) {
    lprem->flags |= CLF_SUF;
    lprem->suffix = lprem->prefix;
    lprem->prefix = nullptr;
  }
  if (sfx) p = revert_cline(lp = p);
  if (matchsubs) {
    if (sfx) {
      if (Cline* q = lp->prefix) {
        while (q->next) q = q->next;
        q->next = matchsubs;
      } else {
        lp->prefix = matchsubs;
      }
      matchlastsub->next = nullptr;
    } else {
      matchlastsub->next = p->prefix;
      p->prefix = matchsubs;
    }
    matchsubs = matchlastsub = nullptr;
  }
  if (lp->llen or lp->wlen) {
    lp->next = get_cline(l, wl, w, wl, o, ol, CLF_NEW);
    lp = lp->next;
  } else {
    lp->line = l;
    lp->llen = wl;
    lp->word = w;
    lp->wlen = wl;
    lp->orig = o;
    lp->olen = ol;
  }
  if (o or ol) lp->flags &= ~CLF_NEW;
  if (matchlastpart) matchlastpart->next = p;
  else               matchparts = p;
  matchlastpart = lp;
}

void MatchRun::Impl::add_match_sub(const Matcher* m, char* l, int ll, char* w, int wl) {
  int flags = 0;
  if (m and (m->flags & Matcher::Line)) {
    w = nullptr;
    wl = 0;
    flags = CLF_LINE;
  }
  if (not wl and not ll) return;
  Cline *lp = nullptr, *n = nullptr;
  Cline* p = n = bld_parts(w, wl, ll, &lp, nullptr);
  if (p and n != lp) {
    for (; p->next != lp; p = p->next) {}
    if (matchsubs) {
      matchlastsub->next = n->prefix;
      n->prefix = matchsubs;
    }
    matchsubs = matchlastsub = lp;
    if (matchlastpart) matchlastpart->next = n;
    else               matchparts = n;
    p->next = nullptr;
    matchlastpart = p;
  } else {
    n = get_cline(l, ll, w, wl, nullptr, 0, flags | ((m and m->wlen == -2) ? CLF_SKIP : 0));
    if (matchlastsub) matchlastsub->next = n;
    else              matchsubs = n;
    matchlastsub = n;
  }
}

int MatchRun::Impl::match_str(char* l, char* w, int* rwlp, bool sfx, int test, bool part) {
  int ll = static_cast<int>(strlen(l)), lw = static_cast<int>(strlen(w));
  int il = 0, iw = 0, exact = 0, wexact = 0, he = 0, bslash = 0;
  const Matcher* lm = nullptr;
  const int ind = sfx ? -1 : 0;
  const int add = sfx ? -1 : 1;
  const int original_ll = ll, original_lw = lw;

  if (not test) start_match();
  if (sfx) {
    l += ll;
    w += lw;
  }
  char* ow = w;

  while (ll) {
    bslash = 0;
    if (not sfx and lw and (not part or test)
        and (l[ind] == w[ind] or (bslash = (lw > 1 and w[ind] == '\\' and w[ind + 1] == l[0])))) {
      l += add;
      w += bslash ? add + add : add;
      ++il;
      iw += 1 + bslash;
      --ll;
      lw -= 1 + bslash;
      ++exact;
      wexact += 1 + bslash;
      lm = nullptr;
      he = 0;
      continue;
    }
  retry:
    const Matcher* mp = nullptr;
    for (s::size_t si = 0; not mp and si < mstack.size(); ++si) {
      for (mp = mstack[si]; mp; mp = mp->next) {
        if ((lm and lm == mp)
            or ((original_ll == ll or original_lw == lw)
                and (test == 1 or (test and not mp->left and not mp->right)) and mp->wlen < 0))
          continue;

        if (mp->wlen < 0) {
          // A `*' pattern.
          int t = 0, both, loff, aoff, llen, alen, moff, ct, ict, aol;
          char* tp;
          char savl = 0;
          const CPattern *ap, *aop;

          llen = mp->llen;
          if (mp->flags & Matcher::Left) {
            alen = mp->lalen;
            aol  = mp->ralen;
          } else {
            alen = mp->ralen;
            aol  = mp->lalen;
          }
          if (ll < llen + alen or lw < alen + aol) continue;

          if (mp->flags & Matcher::Left) {
            ap = mp->left;
            moff = alen;
            aop = mp->right;
            if (sfx) {
              both = 0;
              loff = -llen;
              aoff = -(llen + alen);
            } else {
              both = 1;
              loff = alen;
              aoff = 0;
            }
          } else {
            ap = mp->right;
            moff = 0;
            aop = mp->left;
            if (sfx) {
              both = 1;
              loff = -(llen + alen);
              aoff = -alen;
            } else {
              both = 0;
              loff = 0;
              aoff = llen;
            }
          }
          if (not pattern_match(mp->line, l + loff, nullptr, nullptr)) continue;
          if (ap) {
            if (not pattern_match(ap, l + aoff, nullptr, nullptr)
                or (both
                    and (not pattern_match(ap, w + aoff, nullptr, nullptr)
                         or (aol and aol <= aoff + iw
                             and not pattern_match(aop, w + aoff - aol, nullptr, nullptr))
                         or not match_parts(l + aoff, w + aoff, alen, part))))
              continue;
          } else if (not both
                     or ((mp->flags & Matcher::Inter)
                             ? ((mp->flags & Matcher::Line) ? iw : il)
                             : (il or iw))) {
            continue;
          }

          if (sfx and (savl = l[-(llen + alen)])) l[-(llen + alen)] = '\0';
          for (t = 0, tp = w, ct = 0, ict = lw - alen + 1; ict; tp += add, ++ct, --ict) {
            if ((both
                 and (not ap or not test or not pattern_match(ap, tp + aoff, nullptr, nullptr)
                      or (aol and aol <= aoff + ct + iw
                          and not pattern_match(aop, tp + aoff - aol, nullptr, nullptr))))
                or (not both and pattern_match(ap, tp - moff, nullptr, nullptr)
                    and (not aol
                         or (aol <= iw + ct - moff
                             and pattern_match(aop, tp - moff - aol, nullptr, nullptr)))
                    and (mp->wlen == -1 or match_parts(l + aoff, tp - moff, alen, part)))) {
              if (not both and mp->wlen == -1 and not match_parts(l + aoff, tp - moff, alen, part))
                break;
              if (sfx) {
                char savw;
                if ((savw = tp[-alen])) tp[-alen] = '\0';
                t = match_str(l - ll, w - lw, nullptr, sfx, 2, part);
                if (savw) tp[-alen] = savw;
              } else {
                t = match_str(l + llen + moff, tp + moff, nullptr, sfx, 1, part);
              }
              if (t or (mp->wlen == -1 and not both)) break;
            }
          }
          ict = ct;
          if (sfx and savl) l[-(llen + alen)] = savl;
          if (not t) continue;

          if (not test and (not he or (llen + alen))) {
            char *op, *lp, *map, *wap, *wmp;
            int ol;
            if (sfx) {
              op = w;
              ol = static_cast<int>(ow - w);
              lp = l - (llen + alen);
              map = tp - alen;
              if (mp->flags & Matcher::Left) {
                wap = tp - alen;
                wmp = tp;
              } else {
                wap = w - alen;
                wmp = tp - alen;
              }
            } else {
              op = ow;
              ol = static_cast<int>(w - ow);
              lp = l;
              map = ow;
              if (mp->flags & Matcher::Left) {
                wap = w;
                wmp = w + alen;
              } else {
                wap = tp;
                wmp = ow;
              }
            }
            if (mp->flags & Matcher::Line) {
              add_match_str(nullptr, nullptr, op, ol, sfx);
              add_match_str(nullptr, nullptr, lp, llen + alen, sfx);
              add_match_sub(nullptr, nullptr, ol, op, ol);
              add_match_sub(nullptr, nullptr, llen + alen, lp, llen + alen);
            } else if (sfx) {
              add_match_str(nullptr, nullptr, map, ct + ol + alen, sfx);
              add_match_part(mp, l + aoff, wap, alen, l + loff, llen, op, ol, ol, sfx);
              add_match_sub(nullptr, nullptr, 0, wmp, ct);
            } else {
              add_match_str(nullptr, nullptr, map, ct + ol + alen, sfx);
              if (both) {
                add_match_sub(nullptr, nullptr, ol, op, ol);
                ol = -1;
              } else {
                ct += ol;
              }
              add_match_part(mp, l + aoff, wap, alen, l + loff, llen, wmp, ct, ol, sfx);
            }
          }
          llen += alen;
          alen += ict;
          if (sfx) {
            l -= llen;
            w -= alen;
          } else {
            l += llen;
            w += alen;
          }
          ll -= llen;
          il += llen;
          lw -= alen;
          iw += alen;
          exact = 0;
          ow = w;
          if (not llen and not alen) {
            lm = mp;
            if (he) mp = nullptr;
            else    he = 1;
          } else {
            lm = nullptr;
            he = 0;
          }
          break;
        } else if (ll >= mp->llen and lw >= mp->wlen) {
          // A pattern without `*'.
          int t = 1;
          char *tl, *tw;
          int tll, tlw, til, tiw;

          if (not (mp->flags & (Matcher::Left | Matcher::Right)) and mp->llen == mp->wlen
              and not (sfx ? strncmp(l - mp->llen, w - mp->wlen, static_cast<s::size_t>(mp->llen))
                           : strncmp(l, w, static_cast<s::size_t>(mp->llen))))
            continue;

          if (sfx) {
            tl = l - mp->llen;
            tw = w - mp->wlen;
            til = ll - mp->llen;
            tiw = lw - mp->wlen;
            tll = il + mp->llen;
            tlw = iw + mp->wlen;
          } else {
            tl = l;
            tw = w;
            til = il;
            tiw = iw;
            tll = ll;
            tlw = lw;
          }
          if (mp->flags & Matcher::Left) {
            if (til < mp->lalen or tiw < mp->lalen + mp->ralen) continue;
            else if (mp->left)
              t = pattern_match(mp->left, tl - mp->lalen, nullptr, nullptr)
                  and pattern_match(mp->left, tw - mp->lalen, nullptr, nullptr)
                  and (not mp->ralen
                       or pattern_match(mp->right, tw - mp->lalen - mp->ralen, nullptr, nullptr));
            else
              t = not sfx and not ((mp->flags & Matcher::Inter)
                                       ? ((mp->flags & Matcher::Line) ? iw : il)
                                       : (il or iw));
          }
          if (mp->flags & Matcher::Right) {
            if (tll < mp->llen + mp->ralen or tlw < mp->wlen + mp->ralen + mp->lalen) continue;
            else if (mp->right)
              t = pattern_match(mp->right, tl + mp->llen, nullptr, nullptr)
                  and pattern_match(mp->right, tw + mp->wlen, nullptr, nullptr)
                  and (not mp->lalen
                       or pattern_match(mp->left, tw + mp->wlen - mp->ralen - mp->lalen, nullptr,
                                        nullptr));
            else
              t = sfx and not ((mp->flags & Matcher::Inter)
                                   ? ((mp->flags & Matcher::Line) ? iw : il)
                                   : (il or iw));
          }
          if (not t or not pattern_match(mp->line, tl, mp->word, tw)) continue;

          if (not test) {
            if (sfx) {
              if (ow >= w) add_match_str(nullptr, nullptr, w, static_cast<int>(ow - w), sfx);
            } else {
              if (w >= ow) add_match_str(nullptr, nullptr, ow, static_cast<int>(w - ow), sfx);
            }
            add_match_str(mp, tl, tw, mp->wlen, sfx);
            if (sfx) {
              if (ow >= w) add_match_sub(nullptr, nullptr, 0, w, static_cast<int>(ow - w));
            } else {
              if (w >= ow) add_match_sub(nullptr, nullptr, 0, ow, static_cast<int>(w - ow));
            }
            add_match_sub(mp, tl, mp->llen, tw, mp->wlen);
          }
          if (sfx) {
            l = tl;
            w = tw;
          } else {
            l += mp->llen;
            w += mp->wlen;
          }
          il += mp->llen;
          iw += mp->wlen;
          ll -= mp->llen;
          lw -= mp->wlen;
          exact = 0;
          ow = w;
          lm = nullptr;
          he = 0;
          break;
        }
      }
    }
    if (mp) continue;

    bslash = 0;
    if ((not test or sfx) and lw
        and (l[ind] == w[ind] or (bslash = (lw > 1 and w[ind] == '\\' and w[ind + 1] == l[0])))) {
      l += add;
      w += bslash ? add + add : add;
      ++il;
      iw += 1 + bslash;
      --ll;
      lw -= 1 + bslash;
      lm = nullptr;
      he = 0;
    } else {
      if (not lw) break;
      if (exact and not part) {
        il -= exact;
        iw -= wexact;
        ll += exact;
        lw += wexact;
        l -= add * exact;
        w -= add * wexact;
        exact = wexact = 0;
        goto retry;
      }
      if (test) return 0;
      matchparts = matchsubs = nullptr;
      return -1;
    }
  }
  if (test) return part or not ll;
  if (not part and ll) {
    matchparts = matchsubs = nullptr;
    return -1;
  }
  if (rwlp) *rwlp = iw - static_cast<int>(sfx ? ow - w : w - ow);

  if (sfx and matchparts) {
    Cline *t, *tn;
    if (matchparts->prefix or matchparts->suffix) {
      t = get_cline(nullptr, 0, nullptr, 0, nullptr, 0, 0);
      t->next = matchparts;
      if (matchparts->prefix) t->prefix = &sentinel;
      else                    t->suffix = &sentinel;
      matchparts = t;
    }
    for (t = matchparts; (tn = t->next); t = tn) {
      Cline* sub = tn->prefix ? tn->prefix : tn->suffix;
      if (t->suffix) t->suffix = sub;
      else           t->prefix = sub;
    }
    t->prefix = t->suffix = nullptr;
  }
  return part ? il : iw;
}

int MatchRun::Impl::match_parts(char* l, char* w, int n, bool part) {
  char lsav = l[n], wsav = w[n];
  if (lsav) l[n] = '\0';
  if (wsav) w[n] = '\0';
  int ret = match_str(l, w, nullptr, false, 1, part);
  if (lsav) l[n] = lsav;
  if (wsav) w[n] = wsav;
  return ret;
}

Cline* MatchRun::Impl::bld_parts(char* sp, int len, int plen, Cline** lp, Cline** lprem) {
  Cline* ret = nullptr;
  Cline** q = &ret;
  Cline* n = nullptr;
  int op = plen;
  char *p = sp, *os = sp;

  while (len > 0) {
    bool t = false;
    for (const Matcher* mp : bmatchers) {
      if (t) break;
      if (mp and mp->flags == Matcher::Right and mp->wlen < 0 and mp->ralen and not mp->llen
          and len >= mp->ralen and (sp - os) >= mp->lalen
          and pattern_match(mp->right, sp, nullptr, nullptr)
          and (not mp->lalen
               or ((sp - os) >= mp->lalen
                   and pattern_match(mp->left, sp - mp->lalen, nullptr, nullptr)))) {
        int olen = static_cast<int>(sp - p), llen;
        *q = n = get_cline(nullptr, mp->ralen, sp, mp->ralen, nullptr, 0,
                           plen <= 0 ? CLF_NEW : 0);
        if (p != sp) {
          llen = op < 0 ? 0 : op;
          if (llen > olen) llen = olen;
          n->prefix = get_cline(nullptr, llen, p, olen, nullptr, 0, 0);
        }
        q = &n->next;
        sp += mp->ralen;
        len -= mp->ralen;
        plen -= mp->ralen;
        op -= olen;
        p = sp;
        t = true;
      }
    }
    if (not t) {
      ++sp;
      --len;
      --plen;
    }
  }
  if (p != sp) {
    int olen = static_cast<int>(sp - p), llen = op < 0 ? 0 : op;
    *q = n = get_cline(nullptr, 0, nullptr, 0, nullptr, 0, plen <= 0 ? CLF_NEW : 0);
    if (llen > olen) llen = olen;
    n->prefix = get_cline(nullptr, llen, p, olen, nullptr, 0, 0);
    if (lprem) *lprem = n;
  } else if (not ret) {
    *q = n = get_cline(nullptr, 0, nullptr, 0, nullptr, 0, plen <= 0 ? CLF_NEW : 0);
    if (lprem) *lprem = n;
  } else if (lprem) {
    *lprem = nullptr;
  }
  if (n) n->next = nullptr;
  if (lp) *lp = n;
  return ret;
}

int MatchRun::Impl::bld_line(const Matcher* mp, int* line, const char* mword, const char* word,
                             int wlen, bool sfx) {
  const CPattern* lpat = mp->line;
  const CPattern* wpat = mp->word;
  vec<int> wordchars(static_cast<s::size_t>(s::max(wlen, 0)));
  for (int k = 0; k < wlen; ++k) wordchars[static_cast<s::size_t>(k)] =
      static_cast<unsigned char>(word[k]);
  vec<CPattern> genpat(static_cast<s::size_t>(mp->llen));

  s::size_t g = 0;
  while (lpat) {
    int wchr = 0, wind = 0, wmtp = 0;
    if (lpat->tp == CPattern::Equiv and wpat and *mword) {
      wchr = static_cast<unsigned char>(*mword);
      wind = pattern_match1(wpat, wchr, &wmtp);
      wpat = wpat->next;
      ++mword;
    }
    CPattern& cur = genpat[g];
    if (wind) {
      int lchr = pattern_match_equivalence(lpat, wind, wmtp, wchr);
      if (lchr == CHR_INVALID) return 0;
      cur.tp  = CPattern::Char;
      cur.chr = lchr;
    } else {
      cur.tp    = lpat->tp;
      cur.chr   = lpat->chr;
      cur.items = lpat->items;
    }
    lpat = lpat->next;
    cur.next = lpat ? &genpat[g + 1] : nullptr;
    ++g;
  }

  int llen = mp->llen, rl = 0;
  int* wordcp;
  CPattern* curgenpat;
  if (sfx) {
    wordcp = wordchars.data() + wlen;
    line += llen;
    curgenpat = genpat.data() + llen;
  } else {
    wordcp = wordchars.data();
    curgenpat = genpat.data();
  }
  while (llen and wlen) {
    int wmtp = 0;
    int* wp;
    CPattern* tmpgenpat;
    if (sfx) {
      wp = wordcp - 1;
      tmpgenpat = curgenpat - 1;
    } else {
      tmpgenpat = curgenpat;
      wp = wordcp;
    }
    if (pattern_match1(tmpgenpat, *wp, &wmtp)) {
      int lchr = tmpgenpat->tp == CPattern::Char ? tmpgenpat->chr : *wp;
      if (sfx) *--line = lchr;
      else     *line++ = lchr;
      --llen;
      --wlen;
      ++rl;
      if (sfx) {
        wordcp = wp;
        curgenpat = tmpgenpat;
      } else {
        if (llen) ++curgenpat;
        ++wordcp;
      }
    } else {
      bool found = false;
      for (const Matcher* bm : bmatchers) {
        if (bm and not bm->flags and bm->wlen <= wlen and bm->llen <= llen) {
          int* lp = line;
          wp = wordcp;
          tmpgenpat = curgenpat;
          if (sfx) {
            lp -= bm->llen;
            wp -= bm->wlen;
            tmpgenpat -= bm->llen;
          }
          if (pattern_match_restrict(bm->line, bm->word, wp,
                                     wlen - static_cast<int>(wp - wordchars.data()), tmpgenpat,
                                     lp)) {
            if (sfx) {
              line = lp;
              wordcp = wp;
              curgenpat = tmpgenpat;
            } else {
              line += bm->llen;
              wordcp += bm->wlen;
              curgenpat += bm->llen;
            }
            llen -= bm->llen;
            wlen -= bm->wlen;
            rl += bm->wlen;
            found = true;
            break;
          }
        }
      }
      if (not found) return 0;
    }
  }
  return llen ? 0 : rl;
}

opt<str> MatchRun::Impl::join_strs(int la, char* sa, int lb, char* sb) {
  str rs;
  while (la and lb) {
    if (*sa != *sb) {
      bool t = false;
      for (const Matcher* mp : bmatchers) {
        if (t) break;
        if (mp and not mp->flags and mp->wlen > 0 and mp->llen > 0 and mp->wlen <= la
            and mp->wlen <= lb) {
          bool ta = pattern_match(mp->word, sa, nullptr, nullptr);
          if (ta or pattern_match(mp->word, sb, nullptr, nullptr)) {
            vec<int> line(static_cast<s::size_t>(mp->llen));
            char** ap = ta ? &sa : &sb;
            int*   alp = ta ? &la : &lb;
            char** bp = ta ? &sb : &sa;
            int*   blp = ta ? &lb : &la;
            if (int bl = bld_line(mp, line.data(), *ap, *bp, *blp, false)) {
              for (int c : line) rs += static_cast<char>(c);
              *ap += mp->wlen;
              *alp -= mp->wlen;
              *bp += bl;
              *blp -= bl;
              t = true;
            }
          }
        }
      }
      if (not t) break;
    } else {
      rs += *sa;
      ++sa;
      ++sb;
      --la;
      --lb;
    }
  }
  if (la or lb) return s::nullopt;
  return rs;
}

int MatchRun::Impl::cmp_anchors(Cline* o, Cline* n, bool join) {
  bool line = false;
  if ((not (o->flags & CLF_LINE) and o->wlen == n->wlen
       and (not o->word or not strncmp(o->word, n->word, static_cast<s::size_t>(o->wlen))))
      or (line = ((not o->line and not n->line and not o->wlen and not n->wlen)
                  or (o->llen == n->llen and o->line and n->line
                      and not strncmp(o->line, n->line, static_cast<s::size_t>(o->llen)))))) {
    if (line) {
      o->flags |= CLF_LINE;
      o->word = nullptr;
      o->wlen = 0;
    }
    return 1;
  }
  if (join and not (o->flags & CLF_JOIN) and o->word and n->word) {
    if (auto j = join_strs(o->wlen, o->word, n->wlen, n->word)) {
      o->flags |= CLF_JOIN;
      o->wlen = static_cast<int>(j->size());
      o->word = keep(*j);
      return 2;
    }
  }
  return 0;
}

bool MatchRun::Impl::check_cmdata(Cmdata* md, bool sfx) {
  if (not md->len) {
    if (not md->cl) return true;
    if (md->cl->flags & CLF_LINE) {
      md->line = 1;
      md->len = md->cl->llen;
      md->sp = md->cl->line;
    } else {
      md->line = 0;
      md->len = md->olen = md->cl->wlen;
      if ((md->sp = md->cl->word) and sfx) md->sp += md->len;
      md->alen = md->cl->llen;
      if ((md->astr = md->cl->line) and sfx) md->astr += md->alen;
    }
    md->pcl = md->cl;
    md->cl = md->cl->next;
  }
  return false;
}

Cline* MatchRun::Impl::undo_cmdata(Cmdata* md, bool sfx) {
  Cline* r = md->pcl;
  if (md->line) {
    r->word = nullptr;
    r->wlen = 0;
    r->flags |= CLF_LINE;
    r->llen = md->len;
    r->line = md->sp - (sfx ? md->len : 0);
  } else if (md->len != md->olen) {
    r->wlen = md->len;
    r->word = md->sp - (sfx ? md->len : 0);
  }
  return r;
}

Cline* MatchRun::Impl::join_sub(Cmdata* md, char* sp, int len, int* mlen, bool sfx, bool join) {
  if (check_cmdata(md, sfx)) return nullptr;
  char *ow = sp, *nw = md->sp;
  int ol = len, nl = md->len;
  if (sfx) {
    ow += ol;
    nw += nl;
  }
  for (const Matcher* mp : bmatchers) {
    if (not (mp and not mp->flags and mp->wlen > 0 and mp->llen > 0)) continue;
    if (mp->llen <= ol and mp->wlen <= nl
        and pattern_match(mp->line, ow - (sfx ? mp->llen : 0), mp->word,
                          nw - (sfx ? mp->wlen : 0))) {
      if (sfx) md->sp -= mp->wlen;
      else     md->sp += mp->wlen;
      md->len -= mp->wlen;
      *mlen = mp->llen;
      return get_cline(nullptr, 0, ow - (sfx ? mp->llen : 0), mp->llen, nullptr, 0, 0);
    }
    bool t = false;
    if (join and mp->wlen <= ol and mp->wlen <= nl
        and ((t = pattern_match(mp->word, ow - (sfx ? mp->wlen : 0), nullptr, nullptr))
             or pattern_match(mp->word, nw - (sfx ? mp->wlen : 0), nullptr, nullptr))) {
      vec<int> line(static_cast<s::size_t>(mp->llen));
      char* mw = t ? ow - (sfx ? mp->wlen : 0) : nw - (sfx ? mp->wlen : 0);
      if (int bl = bld_line(mp, line.data(), mw, t ? nw : ow, t ? nl : ol, sfx)) {
        if (t) {
          ol = mp->wlen;
          nl = bl;
        } else {
          ol = bl;
          nl = mp->wlen;
        }
        if (sfx) md->sp -= nl;
        else     md->sp += nl;
        md->len -= nl;
        *mlen = ol;
        str text;
        for (int c : line) text += static_cast<char>(c);
        return get_cline(nullptr, 0, keep(text), mp->llen, nullptr, 0, CLF_JOIN);
      }
    }
  }
  return nullptr;
}

int MatchRun::Impl::sub_match(Cmdata* md, char* sp, int len, bool sfx) {
  int ret = 0, ind, add;
  if (sfx) {
    sp += len;
    ind = add = -1;
  } else {
    ind = 0;
    add = 1;
  }
  while (len) {
    if (check_cmdata(md, sfx)) return ret;
    int l = 0;
    for (char *p = sp, *q = md->sp; l < len and l < md->len and p[ind] == q[ind];
         ++l, p += add, q += add) {}
    if (l) {
      md->len -= l;
      len -= l;
      if (sfx) {
        md->sp -= l;
        sp -= l;
      } else {
        md->sp += l;
        sp += l;
      }
      ret += l;
    } else if (md->line or md->len != md->olen or not md->astr) {
      return ret;
    } else {
      md->line = 1;
      md->len = md->alen;
      md->sp = md->astr;
    }
  }
  return ret;
}

void MatchRun::Impl::join_psfx(Cline* ot, Cline* nt, Cline** orest, Cline** nrest, bool sfx) {
  Cline *p = nullptr, *o, *n;
  Cmdata md, omd;
  char** sstr = nullptr;
  int len, join = 0, line = 0, *slen = nullptr;

  if (sfx) {
    o = ot->suffix;
    n = nt->suffix;
  } else {
    o = ot->prefix;
    n = nt->prefix;
  }
  if (not o) {
    if (orest) *orest = nullptr;
    if (nrest) *nrest = n;
    if (n and n->wlen) ot->flags |= CLF_MISS;
    return;
  }
  if (not n) {
    if (sfx) ot->suffix = nullptr;
    else     ot->prefix = nullptr;
    if (orest) *orest = o;
    if (nrest) *nrest = nullptr;
    return;
  }
  md.cl = n;
  md.len = 0;

  while (o) {
    join = 0;
    omd = md;
    if (o->flags & CLF_LINE) {
      if ((len = sub_match(&md, o->line, o->llen, sfx)) != o->llen) {
        join = 1;
        line = 1;
        slen = &o->llen;
        sstr = &o->line;
      }
    } else if ((len = sub_match(&md, o->word, o->wlen, sfx)) != o->wlen) {
      if (o->line) {
        md = omd;
        o->flags |= CLF_LINE | CLF_DIFF;
        continue;
      }
      o->llen = o->llen - ot->slen;
      join = 1;
      line = 0;
      slen = &o->wlen;
      sstr = &o->word;
    }
    if (join) {
      int jlen = 0;
      if (Cline* joinl = join_sub(&md, *sstr + len, *slen - len, &jlen, sfx,
                                  not (o->flags & CLF_JOIN))) {
        joinl->flags |= CLF_DIFF;
        if (len + jlen != *slen) {
          Cline* rest = get_cline(nullptr, 0, *sstr + (sfx ? 0 : len + jlen), *slen - len - jlen,
                                  nullptr, 0, 0);
          rest->next = o->next;
          joinl->next = rest;
        } else {
          joinl->next = o->next;
        }
        if (len) {
          if (sfx) *sstr += *slen - len;
          *slen = len;
          o->next = joinl;
        } else {
          o->next = nullptr;
          if (p)        p->next = joinl;
          else if (sfx) ot->suffix = joinl;
          else          ot->prefix = joinl;
        }
        o = joinl;
        join = 0;
      }
    }
    if (join) {
      if (len) {
        if (orest) {
          Cline* r;
          if (line) r = get_cline(o->line + len, *slen - len, nullptr, 0, nullptr, 0, o->flags);
          else      r = get_cline(nullptr, 0, o->word + len, *slen - len, nullptr, 0, o->flags);
          r->next = o->next;
          *orest = r;
          *slen = len;
          o->next = nullptr;
        } else {
          if (sfx) *sstr += *slen - len;
          *slen = len;
          o->next = nullptr;
        }
      } else {
        if (p)        p->next = nullptr;
        else if (sfx) ot->suffix = nullptr;
        else          ot->prefix = nullptr;
        if (orest) *orest = o;
      }
      if (not orest or not nrest) ot->flags |= CLF_MISS;
      if (nrest) *nrest = undo_cmdata(&md, sfx);
      return;
    }
    p = o;
    o = o->next;
  }
  if (md.len or md.cl) ot->flags |= CLF_MISS;
  if (orest) *orest = nullptr;
  if (nrest) *nrest = undo_cmdata(&md, sfx);
}

void MatchRun::Impl::join_mid(Cline* o, Cline* n) {
  if (o->flags & CLF_JOIN) {
    Cline* nr = nullptr;
    join_psfx(o, n, nullptr, &nr, false);
    n->suffix = revert_cline(nr);
    join_psfx(o, n, nullptr, nullptr, true);
  } else {
    Cline *orr = nullptr, *nr = nullptr;
    o->flags |= CLF_JOIN;
    join_psfx(o, n, &orr, &nr, false);
    if (orr) orr->llen = o->slen > orr->wlen ? orr->wlen : o->slen;
    o->suffix = revert_cline(orr);
    n->suffix = revert_cline(nr);
    join_psfx(o, n, nullptr, nullptr, true);
  }
  n->suffix = nullptr;
}

int MatchRun::Impl::sub_join(Cline* a, Cline* b, Cline* e, bool anew) {
  if (e->suffix or not a->prefix) return 0;
  Cline *op = e->prefix, *n = nullptr, *t, *ca;
  Cline** p = &n;
  int min = 0, max = 0;
  for (; b != e; b = b->next) {
    if ((*p = t = b->prefix)) {
      while (t->next) t = t->next;
      p = &t->next;
    }
    b->suffix = b->prefix = nullptr;
    b->flags &= ~CLF_SUF;
    min += b->min;
    max += b->max;
    *p = b;
    p = &b->next;
  }
  *p = e->prefix;
  ca = a->prefix;
  while (n) {
    e->prefix = cp_cline(n, true);
    a->prefix = cp_cline(ca, true);
    if (anew) {
      int f = e->flags;
      join_psfx(e, a, nullptr, nullptr, false);
      e->flags = f;
      if (e->prefix) return max - min;
    } else {
      int f = e->flags;
      join_psfx(a, e, nullptr, nullptr, false);
      e->flags = f;
      if (a->prefix) return max - min;
    }
    min -= n->min;
    if (n == op) break;
    n = n->next;
  }
  return max - min;
}

Cline* MatchRun::Impl::join_clines(Cline* o, Cline* n) {
  cline_setlens(n, true);
  if (not o) return n;

  Cline *oo = o, *po = nullptr, *pn = nullptr, *x;
  int diff;
  while (o and n) {
    if ((o->flags & CLF_NEW) and not (n->flags & CLF_NEW)) {
      Cline *t, *tn;
      for (t = o; (tn = t->next) and ((tn->flags & CLF_NEW) or not cmp_anchors(tn, n, false));
           t = tn) {}
      if (tn) {
        diff = sub_join(n, o, tn, true);
        if (po) po->next = tn;
        else    oo = tn;
        t->next = nullptr;
        x = o;
        o = tn;
        if (po and po->prefix and cmp_anchors(x, po, false)) {
          po->flags |= CLF_MISS;
          po->max += diff;
        } else {
          o->flags |= CLF_MISS;
          o->max += diff;
        }
        continue;
      }
    }
    if (not (o->flags & CLF_NEW) and (n->flags & CLF_NEW)) {
      Cline *t, *tn;
      for (t = n; (tn = t->next) and ((tn->flags & CLF_NEW) or not cmp_anchors(o, tn, false));
           t = tn) {}
      if (tn) {
        int of = o->flags & CLF_MISS;
        diff = sub_join(o, n, tn, false);
        o->flags = (o->flags & ~CLF_MISS) | of;
        if (po and po->prefix and cmp_anchors(n, pn, false)) {
          po->flags |= CLF_MISS;
          po->max += diff;
        } else {
          o->flags |= CLF_MISS;
          o->max += diff;
        }
        n = tn;
        continue;
      }
    }
    if ((o->flags & (CLF_SUF | CLF_MID)) != (n->flags & (CLF_SUF | CLF_MID))) {
      Cline *t, *tn;
      int ok = o->flags & (CLF_SUF | CLF_MID), nk = n->flags & (CLF_SUF | CLF_MID);
      for (t = n; (tn = t->next) and (tn->flags & (CLF_SUF | CLF_MID)) != ok; t = tn) {}
      if (tn and cmp_anchors(o, tn, true)) {
        sub_join(o, n, tn, false);
        n = tn;
        continue;
      }
      for (t = o; (tn = t->next) and (tn->flags & (CLF_SUF | CLF_MID)) != nk; t = tn) {}
      if (tn and cmp_anchors(tn, n, true)) {
        sub_join(n, o, tn, true);
        if (po) po->next = tn;
        else    oo = tn;
        t->next = nullptr;
        o = tn;
        continue;
      }
      if (o->flags & CLF_MID) {
        o->flags = (o->flags & ~CLF_MID) | (n->flags & CLF_SUF);
        if (n->flags & CLF_SUF) o->prefix = nullptr;
        else                    o->suffix = nullptr;
      }
      break;
    }
    if (not (o->flags & CLF_MID) and not cmp_anchors(o, n, true)) {
      Cline *t, *tn, *tt, *to = nullptr;
      for (t = n; (tn = t->next); t = tn) {
        if (not (tn->flags & CLF_NEW) and (tn->flags & CLF_SKIP)) {
          for (tt = o; (to = tt->next); tt = to)
            if (not (to->flags & CLF_NEW) and (to->flags & CLF_SKIP) and cmp_anchors(tn, to, true))
              break;
          if (to) break;
        }
      }
      if (tn) {
        if (po) po->next = to;
        else    oo = to;
        o = to;
        diff = sub_join(o, n, tn, false);
        o->flags |= CLF_MISS;
        o->max += diff;
        n = tn;
        po = o;
        o = o->next;
        pn = n;
        n = n->next;
        continue;
      } else {
        for (t = o; (to = t->next); t = to)
          if ((to->flags & CLF_SKIP) and cmp_anchors(n, to, true)) break;
        if (to) {
          diff = sub_join(n, o, to, true);
          if (po) po->next = to;
          else    oo = to;
          x = o;
          o = to;
          if (po and po->prefix and cmp_anchors(x, po, false)) {
            po->flags |= CLF_MISS;
            po->max += diff;
          } else {
            o->flags |= CLF_MISS;
            o->max += diff;
          }
          continue;
        } else {
          for (tt = nullptr, t = n; (tn = t->next); t = tn) {
            if (tn->flags & CLF_SKIP)
              for (tt = o; (to = tt->next); tt = to)
                if ((to->flags & CLF_SKIP) and cmp_anchors(tn, to, true)) break;
            if (to) break;
          }
          if (to) {
            diff = sub_join(n, o, to, true);
            if (po) po->next = to;
            else    oo = to;
            x = o;
            o = to;
            if (po and po->prefix and cmp_anchors(x, po, false)) {
              po->flags |= CLF_MISS;
              po->max += diff;
            } else {
              o->flags |= CLF_MISS;
              o->max += diff;
            }
            continue;
          } else {
            for (tn = n; tn; tn = tn->next)
              if ((tn->flags & CLF_NEW) == (o->flags & CLF_NEW) and cmp_anchors(tn, o, true))
                break;
            if (tn) {
              int of = o->flags & CLF_MISS;
              if ((diff = sub_join(o, n, tn, false))) {
                o->flags = (o->flags & ~CLF_MISS) | of;
                if (po and po->prefix) {
                  po->flags |= CLF_MISS;
                  po->max += diff;
                } else {
                  o->flags |= CLF_MISS;
                  o->max += diff;
                }
              }
              n = tn;
              po = o;
              o = o->next;
              pn = n;
              n = n->next;
              continue;
            }
            if (o->flags & CLF_SUF) break;
            o->word = o->line = o->orig = nullptr;
            o->wlen = 0;
            o->next = nullptr;
            o->flags |= CLF_MISS;
          }
        }
      }
    }
    if (not o->orig and not o->olen) {
      o->orig = n->orig;
      o->olen = n->olen;
    }
    if (n->min < o->min) o->min = n->min;
    if (n->max > o->max) o->max = n->max;
    if (o->flags & CLF_MID) join_mid(o, n);
    else                    join_psfx(o, n, nullptr, nullptr, o->flags & CLF_SUF);
    po = o;
    o = o->next;
    pn = n;
    n = n->next;
  }
  if (o) {
    if (po) po->next = nullptr;
    else    oo = nullptr;
  }
  return oo;
}

// compresult.c's cut_cline: drops what isn't worth inserting.
Cline* MatchRun::Impl::cut_cline(Cline* l, bool hasmatched_, int minmlen_) {
  Cline *q, *p, *e = nullptr, *maxp = nullptr;
  int sum = 0, max = 0, tmp, ls = 0, miss = 0;
  if (not hasmatched_) {
    cline_setlens(l, false);
    return l;
  }
  e = l = cp_cline(l, false);
  for (q = nullptr, p = l; p; p = p->next) {
    if (p->orig or p->olen or not (p->flags & CLF_NEW)) e = p->next;
    if (not p->suffix and (p->wlen or p->llen or p->prefix)) q = p;
  }
  if (not e and q and not q->orig and not q->olen and (q->flags & CLF_MISS)
      and (not (q->flags & CLF_MATCHED) or (not q->prefix and not q->suffix))
      and (q->word ? q->wlen : q->llen) < 3) {
    q->word = q->line = nullptr;
    q->wlen = q->llen = 0;
  }
  while (e and not (e->flags & CLF_MISS)) e = e->next;
  if (e) {
    for (p = e->next; p and not (p->flags & CLF_MISS); p = p->next) {}
    if (p) {
      bool cut = true;
      for (p = e; p; p = p->next) {
        if (not (p->flags & CLF_MISS)) {
          sum += p->max;
        } else {
          tmp = cline_sublen(p);
          if (tmp > 2 and tmp > ((p->max + p->min) >> 1))
            sum += tmp - (p->max - tmp);
          else if (tmp < p->min)
            sum -= (((p->max + p->min) >> 1) - tmp) << (tmp < 2);
        }
        if (sum > max) {
          max = sum;
          maxp = p;
        }
      }
      if (max) {
        e = maxp;
      } else {
        int len = 0;
        cline_setlens(l, false);
        ls = 1;
        for (p = e; p; p = p->next) len += p->min;
        if (len > ((minmlen_ << 1) / 3)) cut = false;
      }
      if (cut) {
        e->line = e->word = nullptr;
        e->llen = e->wlen = e->olen = 0;
        e->next = nullptr;
      }
    }
  }
  e = nullptr;
  tmp = 0;
  for (p = l; p; p = p->next) {
    if (p->flags & (CLF_MISS | CLF_DIFF)) miss = 1;
    for (q = p->prefix; q; q = q->next)
      if (q->flags & CLF_JOIN) {
        e = p;
        tmp = 0;
        break;
      }
    for (q = p->suffix; q; q = q->next)
      if (q->flags & CLF_JOIN) {
        e = p;
        tmp = 1;
        break;
      }
  }
  if (e and (not miss or cline_sublen(e) == e->min)) {
    for (p = tmp ? e->suffix : e->prefix; p and p->next and not (p->next->flags & CLF_JOIN);
         p = p->next) {}
    if (p) p->next = nullptr;
  }
  if (not ls) cline_setlens(l, false);
  return l;
}

// compresult.c's cline_str, building the string instead of inserting it.
str MatchRun::Impl::cline_str(Cline* l, bool hasmatched_, int minmlen_, s::size_t* csp,
                              vec<s::size_t>* posl) {
  str buf;
  int cs = 0;
  auto ins = [&](const char* text, int len, bool move) {
    if (not text or len <= 0) return;
    buf.insert(static_cast<s::size_t>(cs), text, static_cast<s::size_t>(len));
    if (move) cs += len;
  };
  int opos = -1;
  auto note = [&](int npos) {
    if (posl and npos != opos) {
      opos = npos;
      posl->push_back(static_cast<s::size_t>(npos));
    }
  };
  l = cut_cline(l, hasmatched_, minmlen_);
  int pmm = 0, pma = 0, smm = 0, sma = 0, dm = 0;
  int pm = -1, pmax = -1, sm = -1, smax = -1, d = -1, mid = -1;
  for (; l; l = l->next) {
    if (l->olen and not (l->flags & CLF_SUF) and not l->prefix) {
      ins(l->orig, l->olen, true);
    } else {
      for (Cline* sp = l->prefix; sp; sp = sp->next) {
        if (sp->flags & CLF_LINE) ins(sp->line, sp->llen, true);
        else                      ins(sp->word, sp->wlen, true);
        if ((sp->flags & CLF_DIFF) and (not dm or (sp->flags & CLF_MATCHED))) {
          d = cs;
          dm = sp->flags & CLF_MATCHED;
          note(cs);
        }
      }
    }
    if ((l->flags & CLF_MISS) and not (l->flags & CLF_SUF)) {
      note(cs);
      if (((pmax <= (l->max - l->min) or (pma and l->max != l->min))
           and (not pmm or (l->flags & CLF_MATCHED)))
          or ((l->flags & CLF_MATCHED) and not pmm)) {
        pm = cs;
        pmax = l->max - l->min;
        pmm = l->flags & CLF_MATCHED;
        pma = (l->prefix or l->suffix) and l->min == cline_sublen(l);
      }
    }
    if (l->flags & CLF_LINE) ins(l->line, l->llen, true);
    else                     ins(l->word, l->wlen, true);
    if (l->flags & CLF_MISS) {
      if (l->flags & CLF_MID) {
        mid = cs;
      } else if (l->flags & CLF_SUF) {
        note(cs);
        if (((smax <= (l->min - l->max) or (sma and l->max != l->min))
             and (not smm or (l->flags & CLF_MATCHED)))
            or ((l->flags & CLF_MATCHED) and not smm)) {
          sm = cs;
          smax = l->min - l->max;
          smm = l->flags & CLF_MATCHED;
          sma = (l->prefix or l->suffix) and l->min == cline_sublen(l);
        }
      }
    }
    if (l->olen and (l->flags & CLF_SUF) and not l->suffix) {
      ins(l->orig, l->olen, true);
    } else {
      int j = -1, i = 0;
      Cline* js = nullptr;
      for (Cline* sp = l->suffix; sp; sp = sp->next) {
        if (j < 0 and (sp->flags & CLF_DIFF)) {
          j = i;
          js = sp;
        }
        if (sp->flags & CLF_LINE) {
          ins(sp->line, sp->llen, false);
          i += sp->llen;
        } else {
          ins(sp->word, sp->wlen, false);
          i += sp->wlen;
        }
      }
      cs += i;
      if (j >= 0 and (not dm or (js->flags & CLF_MATCHED))) {
        d = cs - j;
        dm = js->flags & CLF_MATCHED;
        note(cs - j);
      }
    }
  }
  note(cs);
  int ncs = mid >= 0 ? mid : pm >= 0 ? pm : sm >= 0 ? sm : d >= 0 ? d : cs;
  if (csp) *csp = static_cast<s::size_t>(s::max(ncs, 0));
  return buf;
}

// ---- MatchRun --------------------------------------------------------------------

MatchRun::MatchRun() : mImpl(s::make_unique<Impl>()) {}
MatchRun::~MatchRun() = default;

namespace {

bool patterns_same(const CPattern* a, const CPattern* b) {
  for (; a; a = a->next, b = b->next) {
    if (not b or a->tp != b->tp) return false;
    if (a->tp == CPattern::Char and a->chr != b->chr) return false;
    if (a->tp != CPattern::Char and a->tp != CPattern::Any) {
      if (a->items.size() != b->items.size()) return false;
      for (s::size_t k = 0; k < a->items.size(); ++k)
        if (a->items[k].kind != b->items[k].kind or a->items[k].lo != b->items[k].lo
            or a->items[k].hi != b->items[k].hi)
          return false;
    }
  }
  return not b;
}

bool matchers_same(const Matcher* a, const Matcher* b) {
  return a == b
      or (a->flags == b->flags and a->llen == b->llen and a->wlen == b->wlen
          and (not a->llen or patterns_same(a->line, b->line))
          and (a->wlen <= 0 or patterns_same(a->word, b->word))
          and (not (a->flags & (Matcher::Left | Matcher::Right))
               or (a->lalen == b->lalen and a->ralen == b->ralen
                   and (not a->lalen or patterns_same(a->left, b->left))
                   and (not a->ralen or patterns_same(a->right, b->right)))));
}

}  // namespace

void MatchRun::begin_add(sptr<MatcherSpec> spec, s::int64_t nmatches) {
  Impl& m = *mImpl;
  m.saved_stacks.push_back(m.mstack);
  if (spec and spec->first()) {
    m.mstack.insert(m.mstack.begin(), spec->first());
    // add_bmatchers: the new ones go first.
    vec<const Matcher*> added;
    for (const Matcher* mp = spec->first(); mp; mp = mp->next)
      if ((not mp->flags and mp->wlen > 0 and mp->llen > 0)
          or (mp->flags == Matcher::Right and mp->wlen < 0 and not mp->llen))
        added.push_back(mp);
    m.bmatchers.insert(m.bmatchers.begin(), added.begin(), added.end());
    m.specs.push_back(s::move(spec));
  }
  if (nmatches and (not m.mstack.empty() or not m.bmatchers.empty())) {
    // update_bmatchers: zsh drops the ones before the last that is not in
    // the matcher stack.
    s::size_t from = 0;
    for (s::size_t k = 0; k < m.bmatchers.size(); ++k) {
      bool found = false;
      for (const Matcher* head : m.mstack)
        for (const Matcher* mp = head; mp and not found; mp = mp->next)
          found = matchers_same(mp, m.bmatchers[k]);
      if (not found) from = k + 1;
    }
    m.bmatchers.erase(m.bmatchers.begin(), m.bmatchers.begin() + static_cast<s::ptrdiff_t>(from));
  }
}

void MatchRun::end_add() {
  Impl& m = *mImpl;
  if (m.saved_stacks.empty()) return;
  m.mstack = s::move(m.saved_stacks.back());
  m.saved_stacks.pop_back();
}

int MatchRun::match_part(strv line, strv w, bool sfx, Cline** parts) {
  Impl& m = *mImpl;
  char* l = m.keep(line);
  char* wp = m.keep(w);
  int ml = m.match_str(l, wp, nullptr, sfx, 0, true);
  if (ml < 0) return -1;
  if (m.matchsubs) {
    Cline* tmp = m.get_cline(nullptr, 0, nullptr, 0, nullptr, 0, sfx ? CLF_SUF : 0);
    if (sfx) tmp->suffix = m.matchsubs;
    else     tmp->prefix = m.matchsubs;
    if (m.matchlastpart) m.matchlastpart->next = tmp;
    else                 m.matchparts = tmp;
  }
  *parts = sfx ? Impl::revert_cline(m.matchparts) : m.matchparts;
  return ml;
}

opt<str> MatchRun::comp_match(strv pfx, strv sfx, strv word, Cline** clp, bool* exact) {
  Impl& m = *mImpl;
  char* l = m.keep_line(m.kept_pfx, pfx);
  char* w = m.keep(word);
  int wl = static_cast<int>(word.size());
  int rpl = 0;
  int mpl = m.match_str(l, w, &rpl, false, 0, false);
  if (mpl < 0) return s::nullopt;
  Cline* pli;
  if (not sfx.empty()) {
    str wpfx = m.matchbuf;
    if (m.matchsubs) {
      Cline* tmp = m.get_cline(nullptr, 0, nullptr, 0, nullptr, 0, 0);
      tmp->prefix = m.matchsubs;
      if (m.matchlastpart) m.matchlastpart->next = tmp;
      else                 m.matchparts = tmp;
    }
    pli = m.matchparts;
    Cline* plil = m.matchlastpart;
    char* ls = m.keep_line(m.kept_sfx, sfx);
    int rsl = 0;
    int msl = m.match_str(ls, w + mpl, &rsl, true, 0, false);
    if (msl < 0) return s::nullopt;
    if (m.matchsubs) {
      Cline* tmp = m.get_cline(nullptr, 0, nullptr, 0, nullptr, 0, CLF_SUF);
      tmp->suffix = m.matchsubs;
      if (m.matchlastpart) m.matchlastpart->next = tmp;
      else                 m.matchparts = tmp;
    }
    m.add_match_str(nullptr, nullptr, w + rpl, wl - rpl - rsl, true);
    m.matchbuf.insert(0, wpfx);
    Cline* mlil = nullptr;
    Cline* mli = m.bld_parts(w + rpl, wl - rpl - rsl, (mpl - rpl) + (msl - rsl), &mlil, nullptr);
    mlil->flags |= CLF_MID;
    mlil->slen = msl - rsl;
    mlil->next = Impl::revert_cline(m.matchparts);
    if (plil) plil->next = mli;
    else      pli = mli;
  } else {
    m.add_match_str(nullptr, nullptr, w + rpl, wl - rpl, false);
    m.add_match_part(nullptr, nullptr, nullptr, 0, nullptr, 0, w + rpl, wl - rpl, mpl - rpl,
                     false);
    pli = m.matchparts;
  }
  *clp = pli;
  if (not sfx.empty()) {
    *exact = word.starts_with(pfx) and word.substr(pfx.size()) == sfx;
  } else {
    *exact = word == pfx;
  }
  return m.matchbuf;
}

Cline* MatchRun::plain(strv w) {
  char* p = mImpl->keep(w);
  int len = static_cast<int>(w.size());
  return mImpl->bld_parts(p, len, -1, nullptr, nullptr);
}

void MatchRun::add(Cline* line, strv qipre, strv ipre, strv pre, strv ppre_v, Cline* pline,
                   strv psuf_v, Cline* sline, strv isuf, strv qisuf) {
  Impl& m = *mImpl;
  auto len = [](strv v) { return static_cast<int>(v.size()); };
  Impl::cline_matched(line);
  if (pline) Impl::cline_matched(pline);
  if (sline) Impl::cline_matched(sline);

  int palen = 0, salen = 0, qipl = 0, ipl = 0, pl = 0, ppl = 0, qisl = 0, isl = 0, psl = 0;
  if (not sline and not psuf_v.empty()) salen = (psl = len(psuf_v));
  if (not isuf.empty()) salen += (isl = len(isuf));
  if (not qisuf.empty()) salen += (qisl = len(qisuf));

  if (salen) {
    Cline *pp = nullptr, *p, *sp, *sl = nullptr;
    for (p = line; p->next; pp = p, p = p->next) {}
    if (psl) {
      Cline* s = m.bld_parts(m.keep(psuf_v), psl, psl, &sl, nullptr);
      if (sline) {
        sline = m.cp_cline(sline, true);
        for (sp = sline; sp->next; sp = sp->next) {}
        sp->next = s;
        s = sline;
        sline = nullptr;
      }
      if (not (p->flags & (CLF_SUF | CLF_MID)) and not p->llen and not p->wlen and not p->olen) {
        if (p->prefix) {
          Cline* q;
          for (q = p->prefix; q->next; q = q->next) {}
          q->next = s->prefix;
          s->prefix = p->prefix;
          p->prefix = nullptr;
        }
        s->flags |= (p->flags & CLF_MATCHED) | CLF_MID;
        if (pp) pp->next = s;
        else    line = s;
      } else {
        p->next = s;
      }
    }
    if (isl) {
      Cline* tsl = nullptr;
      Cline* s = m.bld_parts(m.keep(isuf), isl, isl, &tsl, nullptr);
      if (sl) {
        sl->next = s;
      } else if (sline) {
        sline = m.cp_cline(sline, true);
        for (sp = sline; sp->next; sp = sp->next) {}
        sp->next = s;
        p->next = sline;
        sline = nullptr;
      } else {
        p->next = s;
      }
      sl = tsl;
    }
    if (qisl) {
      Cline* qsl = m.bld_parts(m.keep(qisuf), qisl, qisl, nullptr, nullptr);
      qsl->flags |= CLF_SUF;
      qsl->suffix = qsl->prefix;
      qsl->prefix = nullptr;
      if (sl) {
        sl->next = qsl;
      } else if (sline) {
        sline = m.cp_cline(sline, true);
        for (sp = sline; sp->next; sp = sp->next) {}
        sp->next = qsl;
        p->next = sline;
      } else {
        p->next = qsl;
      }
    }
  } else if (sline) {
    Cline* p;
    for (p = line; p->next; p = p->next) {}
    p->next = m.cp_cline(sline, true);
  }

  if (not qipre.empty()) palen = (qipl = len(qipre));
  if (not ipre.empty()) palen += (ipl = len(ipre));
  if (not pre.empty()) palen += (pl = len(pre));
  if (not pline and not ppre_v.empty()) palen += (ppl = len(ppre_v));

  if (pl) {
    if (ppl or pline) {
      Cline *lp, *p;
      if (pline) {
        for (p = m.cp_cline(pline, true), lp = p; lp->next; lp = lp->next) {}
      } else {
        p = m.bld_parts(m.keep(ppre_v), ppl, ppl, &lp, nullptr);
      }
      if (lp->prefix and not (line->flags & (CLF_SUF | CLF_MID)) and not lp->llen and not lp->wlen
          and not lp->olen) {
        Cline* lpp;
        for (lpp = lp->prefix; lpp->next; lpp = lpp->next) {}
        lpp->next = line->prefix;
        line->prefix = lp->prefix;
        lp->prefix = nullptr;
        if (p != lp) {
          Cline* q;
          for (q = p; q->next != lp; q = q->next) {}
          q->next = line;
          line = p;
        }
      } else {
        lp->next = line;
        line = p;
      }
    }
    auto prepend = [&](strv v, int n) {
      Cline* lp = nullptr;
      Cline* p = m.bld_parts(m.keep(v), n, n, &lp, nullptr);
      lp->next = line;
      line = p;
    };
    prepend(pre, pl);
    if (ipl) prepend(ipre, ipl);
    if (qipl) prepend(qipre, qipl);
  } else if (palen or pline) {
    Cline *p, *lp;
    if (palen) {
      str apre = str(qipre) + str(ipre) + str(pre) + (pline ? str{} : str(ppre_v));
      p = m.bld_parts(m.keep(apre), palen, palen, &lp, nullptr);
      if (pline)
        for (lp->next = m.cp_cline(pline, true); lp->next; lp = lp->next) {}
    } else {
      for (p = lp = m.cp_cline(pline, true); lp->next; lp = lp->next) {}
    }
    if (lp->prefix and not (line->flags & CLF_SUF) and not lp->llen and not lp->wlen
        and not lp->olen) {
      Cline* lpp;
      for (lpp = lp->prefix; lpp->next; lpp = lpp->next) {}
      lpp->next = line->prefix;
      line->prefix = lp->prefix;
      lp->prefix = nullptr;
      if (p != lp) {
        Cline* q;
        for (q = p; q->next != lp; q = q->next) {}
        q->next = line;
        line = p;
      }
    } else {
      lp->next = line;
      line = p;
    }
  }
  m.joined = m.join_clines(m.joined, line);
  ++mCount;
}

str MatchRun::unambiguous(s::size_t* cursor, vec<s::size_t>* positions) const {
  Impl& m = *mImpl;
  if (mCount == 0) {
    if (cursor) *cursor = 0;
    if (positions) positions->clear();
    return {};
  }
  if (m.cache.count != mCount) {
    m.cache.positions.clear();
    m.cache.text = m.cline_str(m.joined, hasmatched, minmlen, &m.cache.cursor, &m.cache.positions);
    m.cache.count = mCount;
  }
  if (cursor) *cursor = m.cache.cursor;
  if (positions) *positions = m.cache.positions;
  return m.cache.text;
}

}  // namespace jfsh
