// History builtins: fc, history and r.

#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <unistd.h>

#include <algorithm>
#include <span>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/glob/glob.h>
#include <jfsh/history/history.h>
#include <jfsh/parser/parser.h>

namespace jfsh {

namespace {

str format_time(s::time_t t, char style) {
  char buf[64];
  struct tm tm{};
  ::localtime_r(&t, &tm);
  const char* fmt = style == 'i' ? "%Y-%m-%d %H:%M" : style == 'f' ? "%m/%d/%Y %H:%M" : "%H:%M";
  s::strftime(buf, sizeof buf, fmt, &tm);
  return buf;
}

// Applies `old=new` substitutions (the first occurrence of each); `done`
// is set when one applied (zsh's fcsubs).
str substitute(str text, const vec<str>& subs, bool& done) {
  for (const auto& s : subs) {
    auto eq = s.find('=');
    str from = s.substr(0, eq), to = s.substr(eq + 1);
    if (from.empty()) continue;
    if (auto at = text.find(from); at != str::npos) {
      text.replace(at, from.size(), to);
      done = true;
    }
  }
  return text;
}

// zsh's stuff() and loop(): the command is shown on stderr, then read as
// the next history event, after the line that ran fc.
int run_again(strv cmd, str text, ExecContext& ctx) {
  (void)cmd;
  s::fflush(stdout);
  s::fprintf(stderr, "%s\n", text.c_str());
  s::fflush(stderr);
  // Read as an input line, it goes into the history as one (zsh's hend,
  // with histreduceblanks).
  if (ctx.history) {
    bool comments = not ctx.opts.interactive() or ctx.opts.get(OPT_INTERACTIVECOMMENTS);
    const auto& o = ctx.history->options();
    const AliasTable* aliases = ctx.opts.get(OPT_ALIASES) ? &ctx.aliases : nullptr;
    ctx.history->add(HistoryManager::hend_text(
        text, o.reduce_blanks ? word_spans(text, comments, aliases) : s::nullopt, o));
  }
  return ctx.executor->exec_string(text);
}

int fc_common(vec<str>& args, ExecContext& ctx, bool list_mode) {
  // zsh names fc in these messages, for history and r too.
  strv cmd = "fc";
  OptParse o;
  if (not parse_options(args, "lnrdfiDEILmsRWAPap", "e", o, false, true)) return 1;
  if (o.has('p') or o.has('P') or o.has('a'))
    return builtin_error(cmd, "pushing history is not implemented");
  // Writing and reading the history file.  zsh writes nothing without
  // $SAVEHIST (or history).
  if (o.has('W') or o.has('A')) {
    auto p = ctx.params.lookup("SAVEHIST");
    if (not ctx.history or not p or not p->get().is_set()
        or s::strtol(p->get().scalar().c_str(), nullptr, 10) <= 0)
      return 0;
  }
  if (not ctx.history and o.has('R')) return 0;
  if (ctx.history and (o.has('W') or o.has('R') or o.has('A'))) {
    auto& hist = *ctx.history;
    str file = o.next < args.size() ? args[o.next] : str{};
    if (file.empty())
      if (auto p = ctx.params.lookup("HISTFILE"); p and p->get().is_set()) file = p->get().scalar();
    if (file.empty()) return builtin_error(cmd, "no history file");
    auto r = o.has('R') ? hist.load(file)
           : hist.save(file, o.has('A') ? HistSave::Append : HistSave::Rewrite);
    if (not r) return builtin_error(cmd, r.error().message);
    return 0;
  }
  vec<str> rest(args.begin() + static_cast<s::ptrdiff_t>(o.next), args.end());
  // -m: the first word is a pattern the events must match.
  opt<CompiledPattern> match;
  if (o.has('m') and not rest.empty()) {
    match = CompiledPattern::compile(rest.front());
    rest.erase(rest.begin());
  }
  // `old=new` words come before the event range.
  vec<str> subs;
  while (not rest.empty() and rest.front().find('=') != str::npos) {
    subs.push_back(rest.front());
    rest.erase(rest.begin());
  }
  const str* editor = o.arg('e');
  bool reexec = o.has('s') or (editor and *editor == "-");
  bool listing = list_mode or o.has('l');

  if (rest.size() > 2) return builtin_error(cmd, "too many arguments");
  // The line being run is left out, so -1 is the command before it, as in
  // zsh.  A shell without history (zsh -c) has an empty one.
  static const vec<HistEntry> none;
  const auto& all = ctx.history ? ctx.history->entries() : none;
  s::span<const HistEntry> entries(all.data(),
      all.size() - (ctx.hist_current and not all.empty() ? 1 : 0));
  // zsh's curhist: the number of the line being run.
  long curhist = all.empty() ? 1 : static_cast<long>(all.back().num) + (ctx.hist_current ? 0 : 1);
  // zsh's fcgetcomm: a number (negative ones count back from the current
  // line, down to 0), else the last event that starts with the text.
  auto resolve = [&](const str& spec, long& num) {
    long n = s::atol(spec.c_str());
    if (n != 0 or spec[0] == '0') {
      num = n < 0 ? s::max(0L, curhist + n) : n;
      return true;
    }
    for (s::size_t i = entries.size(); i-- > 0;)
      if (strv(entries[i].text).starts_with(spec)) {
        num = static_cast<long>(entries[i].num);
        return true;
      }
    builtin_error(cmd, "event not found: " + spec);
    return false;
  };
  long first_num = -1, last_num = -1;
  if (not rest.empty() and not resolve(rest[0], first_num)) return 1;
  if (rest.size() > 1 and not resolve(rest[1], last_num)) return 1;
  if (not listing and entries.empty())
    return builtin_error(cmd, "current history line would recurse endlessly, aborted");
  if (last_num < 0) {
    // Listing stops before the line being run, but never before the first
    // event (zsh's firsthist), so a first `history` lists itself.
    last_num = first_num;
    if (listing and first_num < curhist) {
      last_num = ctx.hist_current or not ctx.history ? curhist - 1 : curhist;
      if (not all.empty()) last_num = s::max(last_num, static_cast<long>(all.front().num));
    }
  }
  // A listing sees the line being run as well (zsh's fclist walks the
  // whole ring); re-running leaves it out.
  if (listing) entries = all;
  if (first_num < 0) {
    first_num = listing ? s::max(1L, curhist - 16) : curhist - 1;
    if (last_num < first_num) last_num = first_num;
  }
  // The events between the two; none is zsh's fclist error.
  long lo = s::min(first_num, last_num), hi = s::max(first_num, last_num);
  s::size_t first = entries.size(), last = 0;
  for (s::size_t i = 0; i < entries.size(); ++i) {
    long n = static_cast<long>(entries[i].num);
    if (n < lo or n > hi) continue;
    if (first == entries.size()) first = i;
    last = i;
  }
  if (first == entries.size()) {
    if (first_num == last_num)
      return builtin_error(cmd, "no such event: " + s::to_string(first_num));
    return builtin_error(cmd, "no events in that range");
  }
  bool reverse = o.has('r') != (first_num > last_num);
  bool substituted = false;

  if (listing) {
    vec<s::size_t> order;
    for (s::size_t i = first; i <= last; ++i) order.push_back(i);
    if (reverse) s::reverse(order.begin(), order.end());
    bool any = false;
    for (s::size_t i : order) {
      const auto& e = entries[i];
      if (match and not match->matches(e.text)) continue;
      any = true;
      str line;
      if (not o.has('n')) {
        char num[32];
        s::snprintf(num, sizeof num, "%5u  ", e.num);
        line += num;
      }
      if (o.has('d') or o.has('f') or o.has('i') or o.has('E')) {
        char style = o.has('i') ? 'i' : o.has('f') ? 'f' : 'd';
        line += format_time(e.timestamp, style) + "  ";
      }
      if (o.has('D')) {
        char d[32];
        s::snprintf(d, sizeof d, "%u:%02u  ", e.elapsed / 60, e.elapsed % 60);
        line += d;
      }
      line += substitute(e.text, subs, substituted);
      s::printf("%s\n", line.c_str());
    }
    // zsh's fclist when -m matched nothing in the range, or no `old=new`
    // applied.
    if (not subs.empty() and any and not substituted)
      return builtin_error(cmd, "no substitutions performed");
    if (not any) return builtin_error(cmd, "no matching events found");
    return 0;
  }

  // Re-execute, after an editor if one is asked for.
  str text;
  bool any = false;
  for (s::size_t i = first; i <= last; ++i) {
    if (match and not match->matches(entries[i].text)) continue;
    if (any) text += '\n';
    any = true;
    text += substitute(entries[i].text, subs, substituted);
  }
  // zsh's fclist: with `old=new`, nothing is done unless one applied.
  if (not subs.empty() and any and not substituted)
    return builtin_error(cmd, "no substitutions performed");
  if (not any) return builtin_error(cmd, "no matching events found");
  if (reexec) return run_again(cmd, text, ctx);

  str ed = editor ? *editor : str{};
  auto param = [&](strv n) {
    auto p = ctx.params.lookup(n);
    return (p and p->get().is_set()) ? p->get().scalar() : str{};
  };
  if (ed.empty()) ed = param("FCEDIT");
  if (ed.empty()) ed = param("EDITOR");
  if (ed.empty()) ed = "vi";
  char tmpl[] = "/tmp/jfsh_fcXXXXXX";
  int fd = ::mkstemp(tmpl);
  if (fd < 0) return builtin_error(cmd, "can't create temp file");
  str body = text + "\n";
  bool wrote = ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
  ::close(fd);
  if (not wrote) {
    ::unlink(tmpl);
    return builtin_error(cmd, "can't write temp file");
  }
  int erc = ctx.executor->exec_string(ed + " " + shell_quote(tmpl));
  str edited;
  if (erc == 0) {
    if (s::FILE* f = s::fopen(tmpl, "r")) {
      char buf[4096];
      s::size_t n;
      while ((n = s::fread(buf, 1, sizeof buf, f)) > 0) edited.append(buf, n);
      s::fclose(f);
    }
  }
  ::unlink(tmpl);
  if (erc != 0) return erc;
  while (not edited.empty() and edited.back() == '\n') edited.pop_back();
  if (edited.empty()) return 0;
  return run_again(cmd, edited, ctx);
}

}  // namespace

// fc [-e editor] [-lnrdfiD] [old=new …] [first [last]]; fc -s; fc -RWA.
int builtin_fc(vec<str>& args, ExecContext& ctx) { return fc_common(args, ctx, false); }

// history: fc -l.
int builtin_history(vec<str>& args, ExecContext& ctx) { return fc_common(args, ctx, true); }

// r: fc -e -, re-running the last command (or the one named).
int builtin_r(vec<str>& args, ExecContext& ctx) {
  vec<str> fargs{args[0], "-e", "-"};
  fargs.insert(fargs.end(), args.begin() + 1, args.end());
  return fc_common(fargs, ctx, false);
}

}  // namespace jfsh
