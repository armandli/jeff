// History builtins: fc, history and r.  fc's event rules, listing and
// -p/-P are derived from zsh 5.9's Src/builtin.c (bin_fc, fcgetcomm,
// fclist) and Src/hist.c (addhistnum, hcomsearch, pushhiststack,
// saveandpophiststack).
// zsh's copyright and licence:
//
// The Z Shell is copyright (c) 1992-2017 Paul Falstad, Richard Coleman,
// Zoltán Hidvégi, Andrew Main, Peter Stephenson, Sven Wischnowsky, and
// others.  All rights reserved.  Individual authors, whether or not
// specifically named, retain copyright in all changes; in what follows, they
// are referred to as `the Zsh Development Group'.  This is for convenience
// only and this body has no legal status.  The Z shell is distributed under
// the following licence; any provisions made in individual files take
// precedence.
//
// Permission is hereby granted, without written agreement and without
// licence or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall the Zsh Development Group be liable to any party for
// direct, indirect, special, incidental, or consequential damages arising out
// of the use of this software and its documentation, even if the Zsh
// Development Group have been advised of the possibility of such damage.
//
// The Zsh Development Group specifically disclaim any warranties, including,
// but not limited to, the implied warranties of merchantability and fitness
// for a particular purpose.  The software provided hereunder is on an "as is"
// basis, and the Zsh Development Group have no obligation to provide
// maintenance, support, updates, enhancements, or modifications.


#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <span>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <jfsh/exec/loop.h>
#include <jfsh/glob/glob.h>
#include <jfsh/history/history.h>
#include <jfsh/parser/parser.h>
#include <jfsh/prompt/prompt.h>

namespace jfsh {

namespace {

// zsh's fclist: an entry's start time through ztrftime.
str format_time(s::time_t t, strv fmt) {
  struct tm tm{};
  ::localtime_r(&t, &tm);
  return ztrftime(fmt, tm);
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

// fc -p [-a] [file [histsize [savehist]]]: zsh's bin_fc -p and
// pushhiststack, with $HISTFILE, $HISTSIZE and $SAVEHIST saved too.
int fc_push(const vec<str>& args, const OptParse& o, ExecContext& ctx) {
  strv cmd = "fc";
  vec<str> argv(args.begin() + static_cast<s::ptrdiff_t>(o.next), args.end());
  auto value = [&](strv name) -> opt<str> {
    auto p = ctx.params.lookup(name);
    return p and p->get().is_set() ? opt<str>(p->get().scalar()) : s::nullopt;
  };
  opt<str> hf;
  str hs = "30", shs = "0";
  if (not argv.empty()) {
    hf = argv[0];
    if (argv.size() > 1) {
      char* end = nullptr;
      s::strtol(argv[1].c_str(), &end, 10);
      if (*end) return builtin_error(cmd, "HISTSIZE must be an integer");
      hs = argv[1];
      if (argv.size() > 2) {
        s::strtol(argv[2].c_str(), &end, 10);
        if (*end) return builtin_error(cmd, "SAVEHIST must be an integer");
        shs = argv[2];
      } else {
        shs = hs;
      }
      if (argv.size() > 3) return builtin_error(cmd, "too many arguments");
    } else {
      hs  = value("HISTSIZE").value_or("30");
      shs = value("SAVEHIST").value_or("0");
    }
  }
  if (not ctx.history) return 1;
  HistStackParams saved{hf ? value("HISTFILE") : s::nullopt, value("HISTSIZE"),
                        value("SAVEHIST"), hf.has_value(), ctx.hist_line};
  ctx.hist_stack_params.push_back(s::move(saved));
  ctx.history->push_stack(o.has('a') ? ctx.func_depth : -1);
  ctx.hist_line = 0;
  if (hf) {
    if (hf->empty()) (void)ctx.params.unset("HISTFILE");
    else             (void)ctx.params.set_scalar("HISTFILE", *hf);
  }
  (void)ctx.params.set_scalar("HISTSIZE", hs);
  (void)ctx.params.set_scalar("SAVEHIST", shs);
  // The new sizes hold for the file read now (zsh's pushhiststack sets
  // histsiz before readhistfile).
  ctx.history->set_options(history_options(ctx));
  if (hf and not hf->empty()) {
    struct stat st;
    if (::stat(hf->c_str(), &st) == 0 or errno != ENOENT) (void)ctx.history->load(*hf);
  }
  return 0;
}

// fc -P: zsh's saveandpophiststack, the pushed history saved to its file
// first if $SAVEHIST and $HISTFILE say so.
bool fc_pop(ExecContext& ctx) {
  if (not ctx.history or ctx.history->stack_depth() == 0 or ctx.hist_stack_params.empty())
    return false;
  auto value = [&](strv name) -> str {
    auto p = ctx.params.lookup(name);
    return p and p->get().is_set() ? p->get().scalar() : str{};
  };
  // (zsh's savehistfile writes nothing in a shell that isn't interactive.)
  str file = value("HISTFILE");
  if (ctx.opts.get(OPT_INTERACTIVE) and not file.empty()
      and s::strtol(value("SAVEHIST").c_str(), nullptr, 10) > 0) {
    auto opts = ctx.history->options();
    opts.savehist = static_cast<int>(s::strtol(value("SAVEHIST").c_str(), nullptr, 10));
    ctx.history->set_options(opts);
    if (auto r = ctx.history->save(file); not r)
      s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
  }
  ctx.history->pop_stack();
  HistStackParams saved = s::move(ctx.hist_stack_params.back());
  ctx.hist_stack_params.pop_back();
  ctx.hist_line = saved.hist_line;
  auto restore = [&](strv name, const opt<str>& v) {
    if (v) (void)ctx.params.set_scalar(str(name), *v);
    else   (void)ctx.params.unset(str(name));
  };
  if (saved.had_file) restore("HISTFILE", saved.histfile);
  restore("HISTSIZE", saved.histsize);
  restore("SAVEHIST", saved.savehist);
  return true;
}

int fc_common(vec<str>& args, ExecContext& ctx, bool list_mode) {
  // zsh names fc in these messages, for history and r too.
  strv cmd = "fc";
  OptParse o;
  if (not parse_options(args, "lnrdfiDEILmsRWAPap", "et", o, false, true)) return 1;
  if (o.has('p')) return fc_push(args, o, ctx);
  if (o.has('P')) {
    if (o.next < args.size()) return builtin_error(cmd, "too many arguments");
    return fc_pop(ctx) ? 0 : 1;
  }
  // Writing and reading the history file.  zsh writes nothing without
  // $SAVEHIST (or history).
  if (o.has('W') or o.has('A')) {
    // zsh's savehistfile writes nothing in a shell that isn't interactive.
    auto p = ctx.params.lookup("SAVEHIST");
    if (not ctx.history or not ctx.opts.get(OPT_INTERACTIVE) or not p or not p->get().is_set()
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
    bool skip_old = o.has('I');
    auto r = o.has('R') ? hist.load(file, skip_old)
           : hist.save(file, o.has('A') ? HistSave::Append : HistSave::Rewrite, skip_old);
    // zsh's savehistfile reports with zerr and fc goes on with 0.
    if (not r and not o.has('R')) s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
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
  // zsh's history ring (`all`), which holds the line being run when there
  // is one; curline is that line's number (0 in a shell that isn't
  // interactive, which has none), curhist the newest number.
  static const vec<HistEntry> none;
  const auto& all = ctx.history ? ctx.history->entries() : none;
  bool interactive = ctx.opts.get(OPT_INTERACTIVE);
  long newest  = all.empty() ? 0 : static_cast<long>(all.back().num);
  long curhist = interactive and not ctx.hist_line ? newest + 1 : newest;
  long curline = not interactive ? 0 : ctx.hist_line ? static_cast<long>(ctx.hist_line) : curhist;
  long oldest  = all.empty() ? curhist : static_cast<long>(all.front().num);
  // zsh's hcomsearch looks back from the entry before the newest.
  s::span<const HistEntry> entries(all.data(), all.empty() ? 0 : all.size() - 1);
  // zsh's addhistnum: `n` events on from event `hl` (or the nearest one
  // in that direction), past the ends firsthist()-1 or curhist+1.
  auto addhistnum = [&](long hl, long n) -> long {
    if (all.empty()) return 0;
    long dir = n < 0 ? -1 : n > 0 ? 1 : 0;
    long at = -1;
    for (long i = 0; i < static_cast<long>(all.size()); ++i) {
      long num = static_cast<long>(all[static_cast<s::size_t>(i)].num);
      if (num == hl) { at = i; break; }
      if (dir < 0 and num < hl) at = i;
      if (dir > 0 and num > hl) { at = i; break; }
    }
    if (at < 0) return 0;
    if (static_cast<long>(all[static_cast<s::size_t>(at)].num) != hl) n -= dir;
    at += n;
    if (at < 0) return oldest - 1;
    if (at >= static_cast<long>(all.size())) return curhist + 1;
    return static_cast<long>(all[static_cast<s::size_t>(at)].num);
  };
  // zsh's fcgetcomm: a number (negative ones count back from the current
  // line, down to 0), else the last event that starts with the text.
  auto resolve = [&](const str& spec, long& num) {
    long n = s::atol(spec.c_str());
    if (n != 0 or spec[0] == '0') {
      num = n < 0 ? s::max(0L, addhistnum(curline, n)) : n;
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
  // zsh's bin_fc: the defaults and range checks.
  long first_num = -1, last_num = -1;
  if (not rest.empty() and not resolve(rest[0], first_num)) return 1;
  if (rest.size() > 1 and not resolve(rest[1], last_num)) return 1;
  if (last_num == -1) {
    if (listing and first_num < curhist) {
      // The line that asked for the listing is left out, what print -s
      // added since is shown, and never less than the first event.
      last_num = curline == curhist ? addhistnum(curhist, -1) : curhist;
      if (last_num < oldest) last_num = oldest;
    } else {
      last_num = first_num;
    }
  }
  if (first_num == -1) {
    first_num = listing ? addhistnum(curhist, -16) : addhistnum(curline, -1);
    if (first_num < 1) first_num = 1;
    if (last_num < first_num) last_num = first_num;
  }
  if (not listing and last_num >= curhist) {
    last_num = curhist - 1;
    if (first_num > last_num)
      return builtin_error(cmd, "current history line would recurse endlessly, aborted");
  }
  // zsh's fclist: -r turns the range round; a command can't run backwards.
  if (o.has('r')) s::swap(first_num, last_num);
  if (not listing and first_num > last_num)
    return builtin_error(cmd, "history events can't be executed backwards, aborted");
  // The events between the two (the whole ring); none is fclist's error.
  entries = all;
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
  bool reverse = first_num > last_num;
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
      // zsh's fclist's tdfmt: -t's format, else -i, -E, -f, -d in turn.
      if (o.has('d') or o.has('f') or o.has('i') or o.has('E') or o.has('t')) {
        strv fmt = o.arg('t') ? strv(*o.arg('t'))
                 : o.has('i') ? "%Y-%m-%d %H:%M"
                 : o.has('E') ? "%f.%-m.%Y %H:%M"
                 : o.has('f') ? "%-m/%f/%Y %H:%M"
                              : "%H:%M";
        line += format_time(e.timestamp, fmt) + "  ";
      }
      if (o.has('D')) {
        char d[32];
        s::snprintf(d, sizeof d, "%u:%02u  ", e.elapsed / 60, e.elapsed % 60);
        line += d;
      }
      // zsh's fclist writes the command with nicezputs: a newline as \n,
      // a tab as \t, other control characters as ^X.
      for (char ch : substitute(e.text, subs, substituted)) {
        auto c = static_cast<unsigned char>(ch);
        if (c == '\n')                  line += "\\n";
        else if (c == '\t')             line += "\\t";
        else if (c < 0x20 or c == 0x7f) { line += '^'; line += static_cast<char>(c ^ 0x40); }
        else                            line += ch;
      }
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
bool hist_stack_pop(ExecContext& ctx) { return fc_pop(ctx); }

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
