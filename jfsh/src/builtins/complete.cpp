// zsh/complete: the builtins completion functions are written with —
// compadd, compset and compquote — and the special parameters they work
// on ($words, $CURRENT, $PREFIX, $SUFFIX, $IPREFIX, $ISUFFIX, $QIPREFIX,
// $QISUFFIX and $compstate), bound to the completion engine's CompState
// while a completion function runs.  Mirrors Src/Zle/complete.c and the
// compadd part of compcore.c.

#include <cstdlib>
#include <cstring>
#include <sys/stat.h>

#include <algorithm>
#include <memory>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/core/options.h>
#include <jfsh/core/utf8.h>
#include <jfsh/editor/completion.h>
#include <jfsh/editor/zle.h>
#include <jfsh/exec/executor.h>
#include <jfsh/exec/subst.h>
#include <jfsh/glob/glob.h>

namespace jfsh {

namespace {

constexpr strv HOOK_NAME = "zsh/complete";

CompState* state_of(ExecContext& ctx) {
  return ctx.zle and ctx.zle->completion ? ctx.zle->completion->state() : nullptr;
}

// Adds special parameter `name` in the current scope, read and written
// through `h`.
void special(ParamTable& params, str name, s::uint32_t flags, ParamValue initial,
             uptr<ParamHandlers> h) {
  Param& p = params.assign(s::move(name), s::move(initial), flags | PM_SPECIAL | PM_LOCAL);
  p.handlers = s::move(h);
}

void scalar(ParamTable& params, str name, str& field, bool readonly = false) {
  auto h = s::make_unique<ParamHandlers>();
  h->get_scalar = [&field] { return field; };
  if (not readonly) h->set_scalar = [&field](str v) { field = s::move(v); };
  if (not readonly) h->unset_fn   = [&field] { field.clear(); };
  special(params, s::move(name), PM_SCALAR | (readonly ? PM_READONLY : 0), field, s::move(h));
}

// The values a function's return puts back while $compstate[restore] is
// "auto".
struct Saved {
  vec<str>   words;
  s::int64_t current = 0;
  str        prefix, suffix, iprefix, isuffix;
  str        restore;
};

// The words named by compadd -d / -F: an array parameter, or a list of
// words in parentheses (taken as they are, not expanded).
vec<str> array_arg(ExecContext& ctx, const str& arg) {
  if (arg.starts_with('(') and arg.ends_with(')')) {
    vec<str> words;
    strv list = strv(arg).substr(1, arg.size() - 2);
    for (s::size_t at = 0; at < list.size();) {
      at = list.find_first_not_of(" \t\n", at);
      if (at == strv::npos) break;
      auto end = list.find_first_of(" \t\n", at);
      words.emplace_back(list.substr(at, end == strv::npos ? strv::npos : end - at));
      at = end == strv::npos ? list.size() : end;
    }
    return words;
  }
  auto p = ctx.params.lookup(arg);
  if (not p or not p->get().is_set()) return {};
  return p->get().is_array() ? p->get().array() : vec<str>{p->get().scalar()};
}

// compadd -o's argument: match, nosort, numeric and reverse, each
// shortened to two letters or more, separated by commas; nosort goes
// alone.
bool is_order(strv arg) {
  constexpr strv ORDERS[] = {"match", "nosort", "numeric", "reverse"};
  if (arg.empty()) return false;
  bool nosort = false, several = arg.find(',') != strv::npos;
  for (s::size_t at = 0; at <= arg.size();) {
    auto comma = arg.find(',', at);
    strv item = arg.substr(at, comma == strv::npos ? strv::npos : comma - at);
    bool ok = item.size() >= 2 and s::any_of(s::begin(ORDERS), s::end(ORDERS),
                                             [&](strv w) { return w.starts_with(item); });
    if (not ok) return false;
    if (item.starts_with("no")) nosort = true;
    if (comma == strv::npos) break;
    at = comma + 1;
  }
  return not (nosort and several);
}

// `w` with one level of quoting (`quote`: 0 for backslashes, ' or ")
// taken off; at[i] is where the text's i-th byte came from in `w`, and
// at.back() is w.size().
struct Unquoted {
  str            text;
  vec<s::size_t> at;
};

Unquoted unquote_mapped(strv w, char quote) {
  Unquoted u;
  for (s::size_t i = 0; i < w.size(); ++i) {
    u.at.push_back(i);
    if (quote != '\'' and w[i] == '\\' and i + 1 < w.size()
        and (quote != '"' or strchr("\"\\$`", w[i + 1])))
      ++i;
    u.text += w[i];
  }
  u.at.push_back(w.size());
  return u;
}

// The byte offsets of `s`'s characters, and its end.
vec<s::size_t> char_starts(strv s) {
  vec<s::size_t> at;
  for (s::size_t i = 0; i < s.size(); i = next_char(s, i)) at.push_back(i);
  at.push_back(s.size());
  return at;
}

}  // namespace

void bind_completion_params(ExecContext& ctx, CompState& st) {
  ParamTable& p = ctx.params;
  {
    auto h = s::make_unique<ParamHandlers>();
    h->get_array = [&st] { return st.words; };
    h->set_array = [&st](vec<str> v) { st.words = s::move(v); };
    h->unset_fn  = [&st] { st.words.clear(); };
    special(p, "words", PM_ARRAY, st.words, s::move(h));
  }
  {
    auto h = s::make_unique<ParamHandlers>();
    h->get_scalar = [&st] { return s::to_string(st.current); };
    h->set_scalar = [&st](str v) { st.current = s::strtoll(v.c_str(), nullptr, 10); };
    h->unset_fn   = [] {};
    special(p, "CURRENT", PM_INTEGER, st.current, s::move(h));
  }
  scalar(p, "PREFIX", st.prefix);
  scalar(p, "SUFFIX", st.suffix);
  scalar(p, "IPREFIX", st.iprefix);
  scalar(p, "ISUFFIX", st.isuffix);
  scalar(p, "QIPREFIX", st.qiprefix, true);
  scalar(p, "QISUFFIX", st.qisuffix, true);
  {
    auto h = s::make_unique<ParamHandlers>();
    h->get_hash      = [&st] { return st.view(); };
    h->set_hash      = [](umap<str, str>) {};
    h->get_elem      = [&st](const str& key) { return st.get(key); };
    h->set_elem      = [&st](const str& key, str v) { st.keys[key] = s::move(v); };
    h->unset_elem    = [](const str&) {};
    h->readonly_elem = [](const str& key) { return CompState::readonly_key(key); };
    h->unset_fn      = [] {};
    special(p, "compstate", PM_HASH, st.view(), s::move(h));
  }

  // Every function called from here on puts the parameters back when it
  // returns, unless it changed $compstate[restore] from "auto".
  auto saved = s::make_shared<vec<Saved>>();
  ctx.hooks.function.emplace_back(str(HOOK_NAME), [&st, saved](const str&, bool entering) {
    if (entering) {
      saved->push_back(Saved{st.words, st.current, st.prefix, st.suffix, st.iprefix, st.isuffix,
                             st.keys["restore"]});
      st.keys["restore"] = "auto";
      return;
    }
    if (saved->empty()) return;
    Saved sv = s::move(saved->back());
    saved->pop_back();
    if (st.keys["restore"] == "auto") {
      st.words   = s::move(sv.words);
      st.current = sv.current;
      st.prefix  = s::move(sv.prefix);
      st.suffix  = s::move(sv.suffix);
      st.iprefix = s::move(sv.iprefix);
      st.isuffix = s::move(sv.isuffix);
    }
    st.keys["restore"] = s::move(sv.restore);
  });
}

void unbind_completion_params(ExecContext& ctx) {
  auto& hooks = ctx.hooks.function;
  for (auto it = hooks.rbegin(); it != hooks.rend(); ++it) {
    if (it->first == HOOK_NAME) {
      hooks.erase(s::next(it).base());
      return;
    }
  }
}

// compadd [-akqQfenUl12C] [-F array] [-P prefix] [-S suffix] [-p hidden-prefix]
//         [-s hidden-suffix] [-i ignored-prefix] [-I ignored-suffix] [-W file-prefix]
//         [-d array] [-J group] [-X explanation] [-x message] [-V group] [-o [order]]
//         [-r remove-chars] [-R remove-func] [-D array] [-O array] [-A array] [-E number]
//         [-M match-spec] [--] [words…]
int builtin_compadd(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  CompState* st = state_of(ctx);
  if (not st) return builtin_error(cmd, "can only be called from completion function");

  // Every option counts once (the first wins), but -M and -D, which add up.
  umap<char, str> opt;
  vec<str> delete_arrays;
  str  match_spec;   // the -M arguments, joined with spaces
  bool has_match = false;
  char group_opt = 0;   // -J or -V, whichever came first
  constexpr strv FLAGS    = "akqQfenUl12C";
  constexpr strv WITH_ARG = "PSpsiIWdJVXxrRDOAEFM";
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "-" or a == "--") {
      ++i;
      break;
    }
    if (a.size() < 2 or a[0] != '-') break;
    for (s::size_t j = 1; j < a.size(); ++j) {
      char c = a[j];
      if (c == 'o') {
        // An optional order, attached or as the next word.
        str order = a.substr(j + 1);
        if (order.empty() and i + 1 < args.size() and is_order(args[i + 1])) order = args[++i];
        opt.try_emplace('o', order);
        break;
      }
      if (FLAGS.find(c) != strv::npos) {
        opt.try_emplace(c, str{});
        continue;
      }
      if (WITH_ARG.find(c) == strv::npos)
        return builtin_error(cmd, str("bad option: -") + c);
      str value = a.substr(j + 1);
      if (value.empty()) {
        if (i + 1 >= args.size()) {
          strv what = strchr("JV", c) ? "group name" : strchr("EM", c) ? "number" : "string";
          if (c == 'M') what = "matching specification";
          return builtin_error(cmd, str(what) + " expected after -" + c);
        }
        value = args[++i];
      }
      if ((c == 'J' or c == 'V') and not group_opt) group_opt = c;
      if (c == 'D') {
        delete_arrays.push_back(value);
      } else if (c == 'M') {
        match_spec += (has_match ? " " : "") + value;
        has_match = true;
      } else {
        opt.try_emplace(c, value);
      }
      break;
    }
  }
  vec<str> words(args.begin() + static_cast<s::ptrdiff_t>(i), args.end());
  auto has = [&](char c) { return opt.contains(c); };
  auto get = [&](char c) { return has(c) ? opt.at(c) : str{}; };

  // -a / -k: the words name arrays (or associations), maybe subscripted.
  if (has('a') or has('k')) {
    Expander ex(*ctx.executor, ctx);
    vec<str> expanded;
    for (const auto& n : words) {
      auto got = ex.words({"\"${(@" + str(has('k') ? "k" : "") + ")" + n + "}\""});
      if (not got) continue;
      for (auto& w : *got) expanded.push_back(s::move(w));
    }
    words = s::move(expanded);
  }

  CompAddSpec spec;
  if (has_match) {
    str err;
    spec.matcher = MatcherSpec::parse(match_spec, err);
    if (not spec.matcher) return builtin_error(cmd, err);
  }
  bool raw = has('Q');
  spec.ipre        = get('i');
  spec.apre        = get('P');
  spec.hpre        = raw ? get('p') : st->quoted(get('p'));
  spec.hsuf        = raw ? get('s') : st->quoted(get('s'), false);
  spec.asuf        = get('S');
  spec.isuf        = get('I');
  spec.has_suffix  = has('S');
  spec.file_prefix = get('W');
  spec.raw         = raw;
  spec.no_match    = has('U');
  spec.files       = has('f');
  spec.hidden      = has('n');
  spec.lines       = has('l');
  spec.all         = has('C');
  spec.empties     = has('E') ? s::max(0, s::atoi(get('E').c_str())) : 0;
  spec.dups        = has('2') ? 2 : has('1') ? 1 : 0;
  if (group_opt) {
    spec.group  = get(group_opt);
    spec.sorted = group_opt == 'J';
  }
  // -o sorts only a named group.
  if (has('o') and not spec.group.empty()) {
    str order = get('o');
    spec.by_match = order.empty() or order.find("ma") != str::npos;
    if (order.find("no") != str::npos) spec.sorted = false;
  }
  if (has('X') and not has('x')) spec.explanation = get('X');
  if (has('x')) spec.message     = get('x');
  if (has('d')) spec.display     = array_arg(ctx, get('d'));
  if (has('F')) spec.ignore      = array_arg(ctx, get('F'));
  // -q: the -S suffix goes when a blank, a widget that inserts nothing,
  // or (a one-character suffix) the same character comes next.
  if (has('q') and has('S')) {
    spec.remove_chars = " \\t\\n\\-";
    if (spec.asuf.size() == 1) spec.remove_chars += "\\" + spec.asuf;
  }
  if (has('r')) spec.remove_chars = get('r');
  if (has('R')) spec.remove_func  = get('R');

  // -O, -A, -D: the matching words go to arrays instead of the matches.
  if (has('O') or has('A') or not delete_arrays.empty()) {
    vec<bool> matched;
    vec<str> internal;
    st->add(spec, words, &matched, &internal);
    if (has('O')) {
      vec<str> out;
      for (s::size_t k = 0; k < words.size(); ++k)
        if (matched[k]) out.push_back(words[k]);
      (void)ctx.params.set_array(get('O'), s::move(out));
    }
    if (has('A')) (void)ctx.params.set_array(get('A'), s::move(internal));
    for (const auto& name : delete_arrays) {
      auto p = ctx.params.lookup(name);
      if (not p) continue;
      vec<str> arr = p->get().array(), kept;
      for (s::size_t k = 0; k < arr.size(); ++k)
        if (k >= matched.size() or matched[k]) kept.push_back(arr[k]);
      (void)ctx.params.set_array(name, s::move(kept));
    }
    return 1;
  }

  s::int64_t before = st->nmatches();
  int matched = 0;
  if (has('e') and ctx.opts.get(OPT_AUTOPARAMSLASH)) {
    // Parameter names: one whose value is a directory gets a slash.
    for (s::size_t k = 0; k < words.size(); ++k) {
      CompAddSpec one = spec;
      one.display = k < spec.display.size() ? vec<str>{spec.display[k]} : vec<str>{};
      one.empties = 0;
      one.all     = false;
      one.explanation.reset();
      one.message.reset();
      auto p = ctx.params.lookup(words[k]);
      struct stat sb{};
      if (p and p->get().is_set() and not p->get().is_array()
          and ::stat(p->get().scalar().c_str(), &sb) == 0 and S_ISDIR(sb.st_mode))
        one.dir_suffix = "/";
      matched += st->add(one, {words[k]});
    }
    spec.display.clear();
    st->add(spec, {});
  } else {
    matched = st->add(spec, words);
  }
  // 0 when a word matched, even one the group had already.
  return matched > 0 or st->nmatches() > before ? 0 : 1;
}

// compset -p n | -P [n] pattern | -s n | -S [n] pattern | -n begin [end]
//         | -N beg-pat [end-pat] | -q
int builtin_compset(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  CompState* st = state_of(ctx);
  if (not st) return builtin_error(cmd, "can only be called from completion function");
  if (args.size() < 2) return builtin_error(cmd, "not enough arguments");
  const str& o = args[1];
  if (o.size() != 2 or o[0] != '-' or str("pPsSnNq").find(o[1]) == str::npos)
    return builtin_error(cmd, "bad option " + o);
  char c = o[1];
  vec<str> rest(args.begin() + 2, args.end());
  if (c != 'q' and rest.empty()) return builtin_error(cmd, "missing string for option " + o);
  auto number = [](const str& w, long& n) {
    char* end = nullptr;
    n = s::strtol(w.c_str(), &end, 10);
    return not w.empty() and *end == '\0';
  };

  switch (c) {
  case 'p': case 's': {
    long n = s::atol(rest[0].c_str());
    str& from = c == 'p' ? st->prefix : st->suffix;
    vec<s::size_t> at = char_starts(from);
    if (n < 0 or static_cast<s::size_t>(n) >= at.size()) return 1;
    if (c == 'p') {
      s::size_t len = at[static_cast<s::size_t>(n)];
      st->iprefix += from.substr(0, len);
      from.erase(0, len);
    } else {
      s::size_t len = from.size() - at[at.size() - 1 - static_cast<s::size_t>(n)];
      st->isuffix.insert(0, from.substr(from.size() - len));
      from.resize(from.size() - len);
    }
    return 0;
  }

  break; case 'P': case 'S': {
    long n = 0;
    if (rest.size() >= 2 and number(rest[0], n)) rest.erase(rest.begin());
    auto pat = CompiledPattern::compile(rest[0], false, ctx.opts.get(OPT_EXTENDEDGLOB));
    str& from = c == 'P' ? st->prefix : st->suffix;
    // The lengths of the prefixes (suffixes) that match, shortest first;
    // the empty one counts from the longest end only.
    vec<s::size_t> lens;
    for (s::size_t at : char_starts(from)) {
      s::size_t len = c == 'P' ? at : from.size() - at;
      if (len == 0 and n > 0) continue;
      if (pat.matches(c == 'P' ? strv(from).substr(0, at) : strv(from).substr(at)))
        lens.push_back(len);
    }
    s::sort(lens.begin(), lens.end());
    if (lens.empty()) return 1;
    s::size_t pick;
    if (n == 0)     pick = lens.back();
    else if (n > 0) {
      if (static_cast<s::size_t>(n) > lens.size()) return 1;
      pick = lens[static_cast<s::size_t>(n - 1)];
    } else {
      if (static_cast<s::size_t>(-n) > lens.size()) return 1;
      pick = lens[lens.size() - static_cast<s::size_t>(-n)];
    }
    if (c == 'P') {
      st->iprefix += from.substr(0, pick);
      from.erase(0, pick);
    } else {
      st->isuffix.insert(0, from.substr(from.size() - pick));
      from.resize(from.size() - pick);
    }
    return 0;
  }

  break; case 'n': {
    long b = 0, e = 0;
    if (not number(rest[0], b)) return 1;
    bool has_end = rest.size() > 1 and number(rest[1], e);
    auto len = static_cast<long>(st->words.size());
    if (b < 0) b += len + 1;
    if (e < 0) e += len + 1;
    if (st->current < b or (has_end and st->current > e)) return 1;
    if (has_end and e < len) st->words.resize(static_cast<s::size_t>(s::max(0L, e)));
    auto drop = static_cast<s::size_t>(
        s::clamp(b - 1, 0L, static_cast<long>(st->words.size())));
    st->words.erase(st->words.begin(), st->words.begin() + static_cast<s::ptrdiff_t>(drop));
    st->current -= static_cast<s::int64_t>(drop);
    return 0;
  }

  break; case 'N': {
    bool ext = ctx.opts.get(OPT_EXTENDEDGLOB);
    auto beg = CompiledPattern::compile(rest[0], false, ext);
    // The last word before the current one that matches.
    s::size_t found = 0;
    for (s::size_t k = 1; k < static_cast<s::size_t>(s::max<s::int64_t>(st->current, 1))
                          and k <= st->words.size(); ++k)
      if (beg.matches(st->words[k - 1])) found = k;
    if (found == 0) return 1;
    if (rest.size() > 1) {
      auto end = CompiledPattern::compile(rest[1], false, ext);
      for (s::size_t k = found + 1; k <= st->words.size(); ++k) {
        if (not end.matches(st->words[k - 1])) continue;
        if (static_cast<s::int64_t>(k) <= st->current) return 1;
        st->words.resize(k - 1);
        break;
      }
    }
    st->words.erase(st->words.begin(), st->words.begin() + static_cast<s::ptrdiff_t>(found));
    st->current -= static_cast<s::int64_t>(found);
    return 0;
  }

  break; default: {
    // -q: one level of quoting comes off the word, and what is left is
    // split into words as the shell would; the one the cursor is in is
    // completed.
    str raw = st->prefix + st->suffix;
    Unquoted u = unquote_mapped(raw, st->quote);
    const str& text = u.text;
    s::size_t cursor = static_cast<s::size_t>(
        s::lower_bound(u.at.begin(), u.at.end() - 1, st->prefix.size()) - u.at.begin());
    struct Piece { s::size_t start, end; };
    vec<Piece> pieces;
    s::size_t k = 0;
    while (k < text.size()) {
      if (text[k] == ' ' or text[k] == '\t') {
        ++k;
        continue;
      }
      Piece p{k, k};
      char q = 0;
      while (k < text.size()) {
        char ch = text[k];
        // A backslash just before the cursor escapes nothing.
        bool escapes = ch == '\\' and k + 1 != cursor;
        if (q) {
          if (ch == q) q = 0;
          else if (escapes and q == '"') ++k;
        } else if (ch == ' ' or ch == '\t') {
          break;
        } else if (ch == '\'' or ch == '"') {
          q = ch;
        } else if (escapes) {
          ++k;
        }
        ++k;
      }
      p.end = s::min(k, text.size());
      pieces.push_back(p);
    }
    // The word at the cursor: the one it is in or at the end of, or a new
    // empty one between two.
    s::size_t at = pieces.size();
    for (s::size_t n = 0; n < pieces.size(); ++n)
      if (pieces[n].start <= cursor and cursor <= pieces[n].end) { at = n; break; }
    if (at == pieces.size()) {
      s::size_t n = 0;
      while (n < pieces.size() and pieces[n].end < cursor) ++n;
      pieces.insert(pieces.begin() + static_cast<s::ptrdiff_t>(n), Piece{cursor, cursor});
      at = n;
    }
    vec<str> words;
    for (const auto& p : pieces) words.push_back(text.substr(p.start, p.end - p.start));
    Piece cur = pieces[at];
    const str& word = words[at];
    char q = not word.empty() and (word[0] == '\'' or word[0] == '"') ? word[0] : 0;
    s::size_t inner_start = cur.start + (q ? 1 : 0);
    s::size_t inner_end   = cur.end;
    if (q and word.size() > 1 and word.back() == q) --inner_end;
    s::size_t pos = s::clamp(cursor, inner_start, inner_end);
    st->qiprefix += raw.substr(0, u.at[inner_start]);
    st->qisuffix.insert(0, raw.substr(u.at[inner_end]));
    st->prefix = text.substr(inner_start, pos - inner_start);
    // … and is not part of $PREFIX.
    if (pos == cursor and pos > inner_start and text[pos - 1] == '\\'
        and (pos - 1 == inner_start or text[pos - 2] != '\\'))
      st->prefix.pop_back();
    st->suffix = text.substr(pos, inner_end - pos);
    st->words  = s::move(words);
    st->current = static_cast<s::int64_t>(at + 1);
    st->keys["all_quotes"].insert(0, 1, q ? q : '\\');
    st->quote = q;
    return 0;
  }
  }
}

// [[ -prefix [n] pat ]], [[ -suffix [n] pat ]], [[ -after pat ]] and
// [[ -between pat pat ]]: compset -P, -S and -N's tests, the parameters
// left alone (zsh's do_comp_vars without its modifications).
int cond_completion(strv op, const vec<str>& args, ExecContext& ctx) {
  bool psfix = op == "-prefix" or op == "-suffix";
  if (not psfix and op != "-after" and op != "-between") return -1;
  s::size_t least = op == "-between" ? 2 : 1, most = psfix ? 2 : least;
  if (args.size() < least or args.size() > most) return -1;
  CompState* st = state_of(ctx);
  if (not st) {
    s::fprintf(stderr, "jfsh: condition can only be used in completion function\n");
    ctx.aborting = ctx.abort_line = true;
    return 1;
  }
  bool ext = ctx.opts.get(OPT_EXTENDEDGLOB);
  auto status = [](bool b) { return b ? 0 : 1; };

  if (not psfix) {
    // The last word before the current one that matches, and no word
    // matching the end pattern from there to the current one.
    auto beg = CompiledPattern::compile(args[0], false, ext);
    auto l = static_cast<s::int64_t>(st->words.size());
    s::int64_t i = st->current - 1;
    if (i < 0 or i >= l) return 1;
    s::int64_t b = 0, e = l - 1;
    bool t = false;
    for (--i; i >= 0; --i) {
      if (beg.matches(st->words[static_cast<s::size_t>(i)])) {
        b = i + 1;
        t = true;
        break;
      }
    }
    if (t and args.size() > 1) {
      auto end = CompiledPattern::compile(args[1], false, ext);
      bool tt = false;
      for (++i; i < l; ++i) {
        if (end.matches(st->words[static_cast<s::size_t>(i)])) {
          e = i - 1;
          tt = true;
          break;
        }
      }
      if (tt and i < st->current) t = false;
    }
    return status(t and e >= b);
  }

  // The count is arithmetic, as zsh's cond_val evaluates it.
  s::int64_t na = -1;
  if (args.size() > 1) {
    auto n = Expander(*ctx.executor, ctx).arith(args[0]);
    if (not n) {
      s::fprintf(stderr, "jfsh: %s\n", n.error().message.c_str());
      ctx.aborting = true;
      return 1;
    }
    na = *n;
  }
  if (na == 0) return 1;
  auto pat = CompiledPattern::compile(args.back(), false, ext);
  const str& w = op == "-prefix" ? st->prefix : st->suffix;
  if (w.empty()) return status((na == 1 or na == -1) and pat.matches(w));
  auto prev = [&](s::size_t at) {
    s::size_t p = 0;
    for (s::size_t k = 0; k < at; k = next_char(w, k)) p = k;
    return p;
  };
  // The prefixes from the shortest (or, without a number, the longest);
  // the n-th that matches.
  bool up = na > 0;
  if (not up) na = -na;
  if (op == "-prefix") {
    s::size_t p = up ? next_char(w, 0) : w.size();
    for (;;) {
      if (pat.matches(strv(w).substr(0, p)) and --na == 0) return 0;
      if (up) {
        if (p == w.size()) return 1;
        p = next_char(w, p);
      } else {
        if (p == 0) return 1;
        p = prev(p);
      }
    }
  }
  // The suffixes from the shortest (or the longest).
  s::size_t p = up ? prev(w.size()) : 0;
  for (;;) {
    if (pat.matches(strv(w).substr(p)) and --na == 0) return 0;
    if (not up) {
      if (p == w.size()) return 1;
      p = next_char(w, p);
    } else {
      if (p == 0) return 1;
      p = prev(p);
    }
  }
}

// compquote [-p] names…: quotes the values of the named parameters as the
// word being completed is quoted.
int builtin_compquote(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  CompState* st = state_of(ctx);
  if (not st) return builtin_error(cmd, "can only be called from completion function");
  OptParse o;
  if (not parse_options(args, "p", "", o)) return 1;
  // -p: as if something came before, so a leading = stays as it is.
  auto quote = [&](const str& w) {
    str q = st->quoted(w);
    if (o.has('p') and st->quote == 0 and q.starts_with("\\=")) q.erase(0, 1);
    return q;
  };
  int rc = 0;
  for (s::size_t k = o.next; k < args.size(); ++k) {
    auto p = ctx.params.lookup(args[k]);
    if (not p or not p->get().is_set()) continue;
    Result<void> set;
    if (p->get().is_array()) {
      vec<str> v = p->get().array();
      for (auto& w : v) w = quote(w);
      set = ctx.params.set_array(args[k], s::move(v));
    } else {
      set = ctx.params.set_scalar(args[k], quote(p->get().scalar()));
    }
    if (not set) rc = builtin_error(cmd, set.error().message);
  }
  return rc;
}

}  // namespace jfsh
