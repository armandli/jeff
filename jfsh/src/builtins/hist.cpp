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
#include <jfsh/history/history.h>

namespace jfsh {

namespace {

// The index into `entries` an event names: a number (negative counts
// back from the last), or the latest event that starts with the text.
opt<s::size_t> find_event(s::span<const HistEntry> entries, strv spec) {
  if (entries.empty()) return s::nullopt;
  bool numeric = not spec.empty()
      and spec.find_first_not_of("0123456789", spec[0] == '-' or spec[0] == '+' ? 1 : 0) == strv::npos
      and spec != "-" and spec != "+";
  if (numeric) {
    long n = s::strtol(str(spec).c_str(), nullptr, 10);
    if (n < 0) {
      long idx = static_cast<long>(entries.size()) + n;
      if (idx < 0) return s::nullopt;
      return static_cast<s::size_t>(idx);
    }
    // The entry numbered n, or the nearest one after it.
    for (s::size_t i = 0; i < entries.size(); ++i)
      if (static_cast<long>(entries[i].num) >= n) return i;
    return s::nullopt;
  }
  for (s::size_t i = entries.size(); i-- > 0;)
    if (strv(entries[i].text).starts_with(spec)) return i;
  return s::nullopt;
}

str format_time(s::time_t t, char style) {
  char buf[64];
  struct tm tm{};
  ::localtime_r(&t, &tm);
  const char* fmt = style == 'i' ? "%Y-%m-%d %H:%M" : style == 'f' ? "%m/%d/%Y %H:%M" : "%H:%M";
  s::strftime(buf, sizeof buf, fmt, &tm);
  return buf;
}

// Applies `old=new` substitutions (the first occurrence of each).
str substitute(str text, const vec<str>& subs) {
  for (const auto& s : subs) {
    auto eq = s.find('=');
    str from = s.substr(0, eq), to = s.substr(eq + 1);
    if (from.empty()) continue;
    if (auto at = text.find(from); at != str::npos) text.replace(at, from.size(), to);
  }
  return text;
}

int run_again(strv cmd, str text, ExecContext& ctx) {
  (void)cmd;
  s::printf("%s\n", text.c_str());
  s::fflush(stdout);
  // The line that ran fc is replaced by the command it runs (it is in the
  // history already when the interactive loop added it before running it).
  if (ctx.history) {
    if (ctx.hist_current) ctx.history->replace_last(text);
    else                  ctx.history->add(text);
  }
  return ctx.executor->exec_string(text);
}

int fc_common(vec<str>& args, ExecContext& ctx, bool list_mode) {
  strv cmd = args[0];
  OptParse o;
  if (not parse_options(args, "lnrdfiDEILmsRWAPap", "e", o, false, true)) return 1;
  if (o.has('p') or o.has('P') or o.has('a'))
    return builtin_error(cmd, "pushing history is not implemented");
  if (not ctx.history) return builtin_error(cmd, "no history");
  auto& hist = *ctx.history;
  // Writing and reading the history file.
  if (o.has('W') or o.has('R') or o.has('A')) {
    str file = o.next < args.size() ? args[o.next] : str{};
    if (file.empty())
      if (auto p = ctx.params.lookup("HISTFILE"); p and p->get().is_set()) file = p->get().scalar();
    if (file.empty()) return builtin_error(cmd, "no history file");
    auto r = o.has('R') ? hist.load(file)
           : hist.save(file, o.has('A') ? HistSave::Append : HistSave::Rewrite);
    if (not r) return builtin_error(cmd, r.error().message);
    return 0;
  }
  // The line being run is left out, so -1 is the command before it, as in
  // zsh.
  const auto& all = hist.entries();
  s::span<const HistEntry> entries(all.data(),
      all.size() - (ctx.hist_current and not all.empty() ? 1 : 0));
  vec<str> rest(args.begin() + static_cast<s::ptrdiff_t>(o.next), args.end());
  // `old=new` words come before the event range.
  vec<str> subs;
  while (not rest.empty() and rest.front().find('=') != str::npos) {
    subs.push_back(rest.front());
    rest.erase(rest.begin());
  }
  const str* editor = o.arg('e');
  bool reexec = o.has('s') or (editor and *editor == "-");
  bool listing = list_mode or o.has('l');

  if (entries.empty()) return builtin_error(cmd, "no events in that range");
  s::size_t first, last;
  if (rest.empty()) {
    if (listing) {
      first = entries.size() > 16 ? entries.size() - 16 : 0;
      last  = entries.size() - 1;
    } else {
      first = last = entries.size() - 1;
    }
  } else {
    auto f = find_event(entries, rest[0]);
    if (not f) return builtin_error(cmd, "event not found: " + rest[0]);
    first = *f;
    if (rest.size() > 1) {
      auto l = find_event(entries, rest[1]);
      if (not l) return builtin_error(cmd, "event not found: " + rest[1]);
      last = *l;
    } else {
      last = listing ? entries.size() - 1 : first;
    }
  }
  bool reverse = o.has('r');
  if (first > last) {
    s::swap(first, last);
    reverse = not reverse;
  }

  if (listing) {
    vec<s::size_t> order;
    for (s::size_t i = first; i <= last; ++i) order.push_back(i);
    if (reverse) s::reverse(order.begin(), order.end());
    for (s::size_t i : order) {
      const auto& e = entries[i];
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
      line += substitute(e.text, subs);
      s::printf("%s\n", line.c_str());
    }
    return 0;
  }

  // Re-execute, after an editor if one is asked for.
  str text;
  for (s::size_t i = first; i <= last; ++i) {
    if (not text.empty()) text += '\n';
    text += substitute(entries[i].text, subs);
  }
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
