// bindkey, zle and vared in this file are derived from zsh 5.9's
// Src/Zle/zle_keymap.c (bin_bindkey), zle_thingy.c (bin_zle) and
// zle_main.c (bin_vared).
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

// Line-editor builtins: bindkey, zle, vared, and the completion ones,
// compdef and the _files/_directories/_command_names helpers (compadd,
// compset and compquote are in complete.cpp).  These completion ones are
// jfsh's own simple completion system, there before compinit has run;
// compinit (zsh's, from the shipped function library) defines functions
// of the same names, and functions come before builtins.  Also the glue
// that lets the editor run shell functions (user widgets, completion
// widgets, compdef functions), with $BUFFER, $CURSOR and friends bound to
// the editor's state while they run.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <map>
#include <memory>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
#include <jfsh/core/signals.h>
#include <jfsh/core/utf8.h>
#include <jfsh/editor/completion.h>
#include <jfsh/editor/editor.h>
#include <jfsh/editor/zle.h>
#include <jfsh/exec/executor.h>
#include <jfsh/history/history.h>
#include <jfsh/prompt/prompt.h>

namespace jfsh {

namespace {

constexpr strv RESERVED_WORDS[] = {
  "!", "[[", "]]", "{", "}", "case", "coproc", "do", "done", "elif", "else", "end",
  "esac", "fi", "for", "foreach", "function", "if", "nocorrect", "repeat", "select",
  "then", "time", "until", "while"};

// Character offset ↔ byte offset in UTF-8 text ($CURSOR counts characters).
long byte_to_char(strv text, s::size_t pos) {
  long n = 0;
  for (s::size_t i = 0; i < pos and i < text.size(); ++n) {
    s::size_t len;
    utf8_decode(text, i, len);
    i += len;
  }
  return n;
}

s::size_t char_to_byte(strv text, long chars) {
  s::size_t i = 0;
  for (long n = 0; n < chars and i < text.size(); ++n) {
    s::size_t len;
    utf8_decode(text, i, len);
    i += len;
  }
  return i;
}

// Adds special parameter `name` in the current (innermost) scope.
void bound(ParamTable& params, str name, s::uint32_t flags, func<str()> get,
           func<void(str)> set = {}) {
  Param& p = params.assign(name, flags & PM_INTEGER ? ParamValue{s::int64_t{0}} : ParamValue{str{}},
                           flags | PM_SPECIAL | PM_LOCAL | (set ? 0 : PM_READONLY));
  p.handlers = s::make_unique<ParamHandlers>();
  p.handlers->get_scalar = s::move(get);
  p.handlers->set_scalar = s::move(set);
}

// $BUFFER, $CURSOR, … for a user widget, live: reading one reads the
// editor, assigning changes it.  In a completion widget they can only be
// read.
void bind_editor_params(ExecContext& ctx, Editor& ed, bool readonly = false) {
  ParamTable& p = ctx.params;
  auto& b = ed.buffer();
  bound(p, "BUFFER", PM_SCALAR, [&b] { return b.text; },
        [&b, &ed](str v) {
          ed.fix_suffix();
          b.text = s::move(v);
          b.cursor = s::min(b.cursor, b.text.size());
        });
  bound(p, "CURSOR", PM_INTEGER, [&b] { return s::to_string(byte_to_char(b.text, b.cursor)); },
        [&b, &ed](str v) {
          ed.fix_suffix();
          long n = s::strtol(v.c_str(), nullptr, 10);
          b.cursor = char_to_byte(b.text, s::max(0L, n));
        });
  bound(p, "LBUFFER", PM_SCALAR, [&b] { return b.text.substr(0, b.cursor); },
        [&b, &ed](str v) {
          ed.fix_suffix();
          str rest = b.text.substr(b.cursor);
          b.cursor = v.size();
          b.text   = s::move(v) + rest;
        });
  bound(p, "RBUFFER", PM_SCALAR, [&b] { return b.text.substr(b.cursor); },
        [&b, &ed](str v) {
          ed.fix_suffix();
          b.text = b.text.substr(0, b.cursor) + v;
        });
  bound(p, "PREBUFFER", PM_SCALAR, [&ed] { return ed.prebuffer(); });
  bound(p, "WIDGET", PM_SCALAR, [&ed] { return ed.widget_name(); });
  bound(p, "LASTWIDGET", PM_SCALAR, [&ed] { return ed.last_widget_name(); });
  bound(p, "KEYS", PM_SCALAR, [&ed] { return ed.keys(); });
  bound(p, "KEYMAP", PM_SCALAR, [&ed] { return ed.keymap_name(); });
  bound(p, "HISTNO", PM_INTEGER, [&ed] { return s::to_string(ed.hist_number()); });
  bound(p, "BUFFERLINES", PM_INTEGER, [&b] {
    return s::to_string(1 + s::count(b.text.begin(), b.text.end(), '\n'));
  });
  bound(p, "PENDING", PM_INTEGER, [] { return str("0"); });
  bound(p, "REGION_ACTIVE", PM_INTEGER, [&ed] { return str(ed.region_active() ? "1" : "0"); },
        [&ed](str v) { ed.set_region_active(s::strtol(v.c_str(), nullptr, 10) != 0); });
  // In characters, as $CURSOR (zsh's mark counts ZLE characters).
  bound(p, "MARK", PM_INTEGER,
        [&b, &ed] {
          auto at = s::min(static_cast<s::size_t>(ed.mark()), b.text.size());
          return s::to_string(byte_to_char(b.text, at));
        },
        [&b, &ed](str v) {
          long n = s::strtol(v.c_str(), nullptr, 10);
          ed.set_mark(static_cast<long>(char_to_byte(b.text, s::max(0L, n))));
        });
  bound(p, "CUTBUFFER", PM_SCALAR, [&ed] { return ed.cut_buffer(); },
        [&ed](str v) { ed.set_cut_buffer(s::move(v)); });
  // $killring: the kill ring, newest first; zsh's get_killring makes 8
  // empty entries when there are none yet.  Assigning replaces the ring.
  {
    Zle& z = *ctx.zle;
    Param& k = p.assign("killring", ParamValue{vec<str>{}}, PM_ARRAY | PM_SPECIAL | PM_LOCAL);
    k.handlers = s::make_unique<ParamHandlers>();
    k.handlers->get_array = [&z] {
      if (z.kring.empty()) z.kring.resize(8);
      vec<str> v;
      for (const auto& c : z.kring) v.push_back(c.text);
      return v;
    };
    k.handlers->set_array = [&z, &p](vec<str> v) {
      z.kring.clear();
      for (auto& x : v) z.kring.push_back({s::move(x), true, false});
      if (auto q = p.lookup("killring")) q->get().flags &= ~PM_UNSET;
    };
    // zsh's unset_killring: the ring goes, and the parameter is unset
    // (stdunsetfn) for the rest of the widget.
    k.handlers->unset_fn = [&z, &p] {
      z.kring.clear();
      if (auto q = p.lookup("killring")) q->get().flags |= PM_UNSET;
    };
  }
  bound(p, "WIDGETSTYLE", PM_SCALAR, [&ed] { return ed.widget_style(); });
  if (auto n = ed.numeric())
    bound(p, "NUMERIC", PM_INTEGER, [&ed] { return s::to_string(ed.numeric().value_or(1)); },
          [&ed](str v) { ed.set_numeric(static_cast<int>(s::strtol(v.c_str(), nullptr, 10))); });
  if (not readonly) return;
  for (strv name : {"BUFFER", "CURSOR", "LBUFFER", "RBUFFER", "MARK", "CUTBUFFER", "NUMERIC"}) {
    if (auto q = p.lookup_local(name)) {
      q->get().handlers->set_scalar = nullptr;
      q->get().flags |= PM_READONLY;
    }
  }
}

// The editor's fds 0, 1 and 2 while a widget function runs: `exec
// </dev/null` in one (compsys's _comp_setup does it) lasts only as long
// as the function, as in zsh.
struct KeepStdFds {
  KeepStdFds() {
    for (int fd = 0; fd < 3; ++fd) saved[fd] = ::fcntl(fd, F_DUPFD_CLOEXEC, 10);
  }
  ~KeepStdFds() {
    s::fflush(nullptr);
    for (int fd = 0; fd < 3; ++fd) {
      if (saved[fd] < 0) continue;
      ::dup2(saved[fd], fd);
      ::close(saved[fd]);
    }
  }
  KeepStdFds(const KeepStdFds&) = delete;
  KeepStdFds& operator=(const KeepStdFds&) = delete;
  int saved[3] = {-1, -1, -1};
};

int call_shell_function(ExecContext& ctx, const str& fn, vec<str> args) {
  KeepStdFds keep;
  auto it = ctx.functions.find(fn);
  if (it == ctx.functions.end() and ctx.autoloads.contains(fn) and ctx.executor->load_autoload(fn))
    it = ctx.functions.find(fn);
  if (it == ctx.functions.end()) return -1;
  args.insert(args.begin(), fn);
  ProgRef body = it->second;
  return ctx.executor->call_function(body, args);
}

Zle& zle_of(ExecContext& ctx) { return *ctx.zle; }

}  // namespace

void install_zle_hooks(Executor& ex) {
  ExecContext& ctx = ex.context();
  Zle& zle = *ctx.zle;
  zle.params = &ctx.params;
  // Widget functions running (one runs others with `zle name`): an error
  // unwinds them all, and only the outermost clears it.
  auto depth = s::make_shared<int>(0);
  zle.hooks.run_widget = [&ctx, depth](Editor& ed, const str& widget, const str& fn,
                                       const vec<str>& args) {
    ctx.params.push_scope();
    bind_editor_params(ctx, ed);
    ctx.abort_line = false;
    ++*depth;
    // Without XTRACE, as zsh runs widget functions.
    bool xtrace = ctx.opts.get(OPT_XTRACE);
    ctx.opts.unset(OPT_XTRACE);
    int rc = call_shell_function(ctx, fn, args);
    ctx.opts.assign(OPT_XTRACE, xtrace);
    ed.resume_raw_mode();
    // An error throws the line away, as zsh does.
    if (ctx.aborting) ed.abort_line();
    if (--*depth == 0) {
      ctx.aborting = ctx.abort_line = false;
      ctx.end_trap_abort();
    }
    ctx.params.pop_scope();
    if (rc < 0) {
      ed.set_message("No such shell function `" + fn + "'");
      return -1;
    }
    (void)widget;
    return rc;
  };
  zle.hooks.run_command = [&ex](const str& cmd) { return ex.exec_string(cmd); };
  zle.hooks.trap_scope = [&ctx](Editor& ed, bool begin) {
    if (not begin) {
      ctx.params.pop_scope();
      return;
    }
    ctx.params.push_scope();
    bind_editor_params(ctx, ed, true);
  };
  if (not zle.completion) return;
  CompletionSources src;
  src.command_names = [&ctx, &ex] {
    vec<str> names = ex.builtin_names();
    for (const auto& [name, body] : ctx.functions) names.push_back(name);
    for (const auto& [name, _] : ctx.autoloads) names.push_back(name);
    for (const auto& [name, def] : ctx.aliases.regular) names.push_back(name);
    for (strv r : RESERVED_WORDS) names.emplace_back(r);
    return names;
  };
  src.call_function = [&ctx, depth](const str& fn, CompState& st) {
    ctx.params.push_scope();
    if (Editor* ed = ctx.zle->active; ed and ed->active()) bind_editor_params(ctx, *ed, true);
    bind_completion_params(ctx, st);
    ctx.params.assign("service", st.words.empty() ? str{} : st.words[0], PM_SCALAR | PM_LOCAL);
    ctx.abort_line = false;
    ++*depth;
    int rc = call_shell_function(ctx, fn, {});
    if (Editor* ed = ctx.zle->active) ed->resume_raw_mode();
    // An error throws away what the function found; an expansion error
    // the line too.
    if (ctx.aborting) {
      st.matches.clear();
      if (Editor* ed = ctx.zle->active; ed and ctx.abort_line) ed->abort_line();
    }
    // Only an expansion error goes on to unwind the widgets that ran this
    // one; others end here.
    if (--*depth == 0 or not ctx.abort_line) {
      ctx.aborting = ctx.abort_line = false;
      ctx.end_trap_abort();
    }
    unbind_completion_params(ctx);
    ctx.params.pop_scope();
    return rc;
  };
  zle.completion->set_sources(s::move(src));
}

// ---- bindkey ---------------------------------------------------------------
//
// bin_bindkey and its helpers from zsh's Zle/zle_keymap.c.

namespace {

bool undefined_key(const KeyBinding* b) {
  return not b
      or (not b->is_macro and (b->widget == "undefined-key" or b->widget == ".undefined-key"));
}

// zsh's bindstate and bindlistout.
struct BindList {
  strv              kmname;
  bool              as_commands = false;
  bool              all         = false;   // BS_ALL: show undefined-key too
  str               first, last;
  const KeyBinding* bind = nullptr;

  void out() const {
    if (undefined_key(bind) and not all) return;
    bool range = first != last;
    str line;
    if (as_commands) {
      bool nodash = true;
      line = "bindkey ";
      if (range) line += "-R ";
      if (bind and bind->is_macro) line += "-s ";
      if (kmname == "main") {
      } else if (kmname == "vicmd") {
        line += "-a ";
      } else {
        line += "-M " + shell_quote(kmname) + " ";
        nodash = false;
      }
      if (nodash and not first.empty() and first[0] == '-') line += "-- ";
    }
    line += format_key(first);
    if (range) line += "-" + format_key(last);
    line += ' ';
    if (not bind)              line += "undefined-key";
    else if (bind->is_macro)   line += format_key(bind->macro);
    else if (as_commands)      line += shell_quote(bind->widget);
    else                       line += bind->widget;
    s::printf("%s\n", line.c_str());
  }
};

bool same_binding(const KeyBinding* a, const KeyBinding* b) {
  if (undefined_key(a) or undefined_key(b)) return undefined_key(a) and undefined_key(b);
  if (a->is_macro != b->is_macro) return false;
  return a->is_macro ? a->macro == b->macro : a->widget == b->widget;
}

void list_keymap(const Keymap& km, strv kmname, bool as_commands, const opt<str>& prefix) {
  BindList bs{kmname, as_commands, false, {}, {}, nullptr};
  for (const auto* kv : km.scan()) {
    const str& seq = kv->first;
    if (prefix and not prefix->empty()
        and (not strv(seq).starts_with(*prefix) or seq.size() == prefix->size()))
      continue;
    const KeyBinding* b = &kv->second;
    if (same_binding(b, bs.bind) and seq.size() == 1 and bs.last.size() == 1
        and static_cast<unsigned char>(seq[0]) == static_cast<unsigned char>(bs.last[0]) + 1) {
      bs.last = seq;
      continue;
    }
    bs.out();
    bs.first = bs.last = seq;
    bs.bind = b;
  }
  bs.out();
}

// zsh's metabind table: the meta key bindings bindkey -m adds.
str meta_binding(int c) {
  static const Keymap emacs = [] {
    Zle defaults;
    return *defaults.keymap("emacs");
  }();
  const KeyBinding* b = emacs.lookup(str("\x1b") + static_cast<char>(c & 0x7f));
  return b and not b->is_macro and c != 0x9b ? b->widget : str{};
}

}  // namespace

int builtin_bindkey(vec<str>& args, ExecContext& ctx) {
  strv name = args[0];
  Zle& zle = zle_of(ctx);
  OptParse o;
  if (not parse_options(args, "evaldDANmrsLRp", "M", o)) return 1;
  vec<str> argv(args.begin() + static_cast<s::ptrdiff_t>(o.next), args.end());

  // The operation, as zsh's opns table: the first of these flags given.
  struct Op { char o; bool selp; int min, max; };
  static constexpr Op ops[] = {
    {'l', false, 0, -1}, {'d', false, 0, 0}, {'D', false, 1, -1}, {'A', false, 2, 2},
    {'N', false, 1, 2}, {'m', true, 0, 0}, {'r', true, 1, -1}, {'s', true, 2, -1},
    {0, true, 0, -1},
  };
  const Op* op = ops;
  while (op->o and not o.has(op->o)) ++op;
  if (op->o)
    for (const Op* opp = op + 1; opp->o; ++opp)
      if (o.has(opp->o)) return builtin_error(name, "incompatible operation selection options");
  int n = o.has('e') + o.has('v') + o.has('a') + (o.arg('M') != nullptr);
  if (not op->selp and n)
    return builtin_error(name, str("keymap cannot be selected with -") + op->o);
  if (n > 1) return builtin_error(name, "incompatible keymap selection options");

  str kmname;
  sptr<Keymap> km;
  if (op->selp) {
    if (o.has('e'))       kmname = "emacs";
    else if (o.has('v'))  kmname = "viins";
    else if (o.has('a'))  kmname = "vicmd";
    else if (o.arg('M'))  kmname = *o.arg('M');
    else                  kmname = "main";
    km = zle.keymap(kmname);
    if (not km) return builtin_error(name, "no such keymap `" + kmname + "'");
    if (o.has('e') or o.has('v')) zle.link_keymap(km, "main");
  }

  // Listing.
  if (not op->o and (argv.empty() or argv.size() == 1)) {
    if (o.has('e') or o.has('v')) return 0;
    if (not argv.empty() and not o.has('p')) {
      str seq = parse_key_notation(argv[0]);
      BindList bs{kmname, o.has('L'), true, seq, seq, km->lookup(seq)};
      bs.out();
      return 0;
    }
    opt<str> prefix;
    if (o.has('p')) {
      if (argv.empty()) return builtin_error(name, "option -p requires a prefix string");
      prefix = parse_key_notation(argv[0]);
    }
    list_keymap(*km, kmname, o.has('L'), prefix);
    return 0;
  }

  int argc = static_cast<int>(argv.size());
  if (argc < op->min) return builtin_error(name, str("not enough arguments for -") + op->o);
  if (op->max != -1 and argc > op->max)
    return builtin_error(name, str("too many arguments for -") + op->o);

  switch (op->o) {
    case 'l': {
      // bin_bindkey_lsmaps
      int ret = 0;
      vec<str> names;
      if (argv.empty()) {
        for (const auto& [kn, k] : zle.keymaps) names.push_back(kn);
        s::sort(names.begin(), names.end());
      } else {
        names = argv;
      }
      for (const auto& kn : names) {
        auto k = zle.keymap(kn);
        if (not k) {
          ret = builtin_error(name, "no such keymap: `" + kn + "'");
          continue;
        }
        if (not o.has('L')) {
          s::printf("%s\n", kn.c_str());
          continue;
        }
        if (kn == ".safe") continue;
        str line = "bindkey -";
        if (not k->primary.empty() and k->primary != kn) {
          line += "A ";
          if (k->primary[0] == '-') line += "-- ";
          line += shell_quote(k->primary) + " ";
        } else {
          line += "N ";
          if (kn[0] == '-') line += "-- ";
        }
        line += shell_quote(kn);
        s::printf("%s\n", line.c_str());
      }
      return ret;
    }
    case 'd':
      zle.reset();
      return 0;
    case 'D': {
      int ret = 0;
      for (const auto& kn : argv) {
        if (kn == ".safe")
          ret = builtin_error(name, "keymap name `" + kn + "' is protected");
        else if (not zle.unlink(kn)) ret = builtin_error(name, "no such keymap `" + kn + "'");
      }
      return ret;
    }
    case 'A':
      if (not zle.keymap(argv[0])) return builtin_error(name, "no such keymap `" + argv[0] + "'");
      if (not zle.link(argv[0], argv[1]))
        return builtin_error(name, "keymap name `" + argv[1] + "' is protected");
      return 0;
    case 'N':
      if (argv[0] == ".safe")
        return builtin_error(name, "keymap name `" + argv[0] + "' is protected");
      if (argc > 1 and not zle.keymap(argv[1]))
        return builtin_error(name, "no such keymap `" + argv[1] + "'");
      zle.create(argv[0], argc > 1 ? argv[1] : str{});
      return 0;
    case 'm':
      if (km->primary == ".safe")
        return builtin_error(name, "keymap `" + kmname + "' is protected");
      builtin_error(name, "warning: `bindkey -m' disables multibyte support");
      for (int c = 128; c < 256; ++c) {
        str w = meta_binding(c);
        if (w.empty()) continue;
        str seq(1, static_cast<char>(c));
        const KeyBinding* b = km->lookup(seq);
        bool self_insert = b and not b->is_macro
                       and (b->widget == "self-insert" or b->widget == ".self-insert");
        if (undefined_key(b) or self_insert)
          km->bind(seq, w);
      }
      return 0;
    default:
      break;
  }

  // bin_bindkey_bind: -r, -s or plain, with -R for ranges.
  char func = op->o;
  if (not func or func == 's')
    for (s::size_t i = 2; i < argv.size(); i += 2)
      if (i + 1 >= argv.size()) return builtin_error(name, "even number of arguments required");
  if (km->primary == ".safe") return builtin_error(name, "keymap `" + kmname + "' is protected");
  if (func == 'r' and o.has('p')) {
    for (const auto& useq : argv) {
      str prefix = parse_key_notation(useq);
      for (auto it = km->bindings.begin(); it != km->bindings.end();) {
        if (strv(it->first).starts_with(prefix) and it->first.size() > prefix.size())
          it = km->bindings.erase(it);
        else ++it;
      }
    }
    return 0;
  }
  int ret = 0;
  for (s::size_t i = 0; i < argv.size(); ++i) {
    const str& useq = argv[i];
    KeyBinding kb;
    if (func == 's') {
      kb.is_macro = true;
      kb.macro = parse_key_notation(argv[++i]);
    } else if (func != 'r') {
      kb.widget = argv[++i];
    }
    str seq = parse_key_notation(useq);
    auto bind = [&](const str& k) {
      if (func == 'r' or (not kb.is_macro and kb.widget == "undefined-key")) km->unbind(k);
      else if (kb.is_macro) km->bind_macro(k, kb.macro);
      else                  km->bind(k, kb.widget);
    };
    if (o.has('R')) {
      int first, last;
      if (seq.size() < 2 or seq.size() > 2 + (seq[1] == '-')
          or (first = static_cast<unsigned char>(seq[0]))
             > (last = static_cast<unsigned char>(seq.back()))) {
        ret = builtin_error(name, "malformed key range `" + useq + "'");
      } else {
        for (; first <= last; ++first) bind(str(1, static_cast<char>(first)));
      }
    } else if (seq.empty()) {
      ret = builtin_error(name, "cannot bind to an empty key sequence");
    } else {
      bind(seq);
    }
  }
  return ret;
}

// ---- zle ------------------------------------------------------------------
//
// bin_zle and its operations from zsh's Zle/zle_thingy.c.

namespace {

// Whether `name` is a widget's name the user may not rebind (zsh's
// TH_IMMORTAL: the `.name` of a builtin widget).
bool protected_widget(strv name) {
  return name.starts_with('.') and Zle::is_builtin_widget(name.substr(1));
}

// Binds widget name `name` to what `src` names now (zsh's bindwidget):
// a user or completion widget's function, or the builtin.
void bind_widget(Zle& zle, const str& name, const str& src) {
  str target = zle.resolve_widget(src);
  zle.deleted_widgets.erase(name);
  zle.user_widgets.erase(name);
  zle.comp_widgets.erase(name);
  zle.widget_aliases.erase(name);
  if (auto u = zle.user_widgets.find(target); u != zle.user_widgets.end()) {
    str fn = u->second;
    if (auto c = zle.comp_widgets.find(target); c != zle.comp_widgets.end())
      zle.comp_widgets[name] = c->second;
    zle.user_widgets[name] = fn;
  } else if (target != name) {
    zle.widget_aliases[name] = target.starts_with('.') or zle.module_widgets.contains(target)
                             ? target : "." + target;
  }
}

// zsh's scanlistwidgets: -1 names only, 0 `name (fn)`, 1 as commands.
void list_widget(const Zle& zle, const str& w, int list) {
  if (list < 0) {
    s::printf("%s\n", w.c_str());
    return;
  }
  auto u = zle.user_widgets.find(w);
  if (u == zle.user_widgets.end()) return;   // builtin (WIDGET_INT)
  auto c = zle.comp_widgets.find(w);
  str line;
  if (list) {
    line = c != zle.comp_widgets.end() ? "zle -C " : "zle -N ";
    if (w[0] == '-') line += "-- ";
    line += shell_quote(w);
    if (c != zle.comp_widgets.end())
      line += " " + shell_quote(c->second) + " " + shell_quote(u->second);
    else if (u->second != w)         line += " " + shell_quote(u->second);
  } else {
    line = w;
    if (c != zle.comp_widgets.end()) line += " -C " + c->second + " " + u->second;
    else if (u->second != w)         line += " (" + u->second + ")";
  }
  s::printf("%s\n", line.c_str());
}

}  // namespace

int builtin_zle(vec<str>& args, ExecContext& ctx) {
  strv name = args[0];
  Zle& zle = zle_of(ctx);
  Editor* ed = zle.active;
  bool active = ed and ed->active();
  // `zle widget [-n num] [-Nw] [-K keymap] [-f nolast] args`: bin_zle_call
  // reads the options after the widget's name itself.
  if (args.size() >= 2 and not args[1].starts_with('-')) {
    if (not active) return builtin_error(name, "widgets can only be called when ZLE is active");
    str wname = args[1];
    s::size_t i = 2;
    opt<opt<int>> numeric;
    opt<str> keymap_restore;
    bool set_bindk = false, nolast = false;
    for (; i < args.size() and args[i].starts_with('-'); ++i) {
      const str& a = args[i];
      if (a.size() == 1 or a[1] == '-') {
        ++i;
        break;
      }
      for (s::size_t k = 1; k < a.size(); ++k) {
        char c = a[k];
        auto value = [&]() -> opt<str> {
          if (k + 1 < a.size()) {
            str v = a.substr(k + 1);
            k = a.size();
            return v;
          }
          if (i + 1 < args.size()) return args[++i];
          return s::nullopt;
        };
        if (c == 'f') {
          auto f = value();
          if (not f or *f != "nolast") {
            if (keymap_restore) ed->select_keymap(*keymap_restore);
            return builtin_error(name, "'nolast' expected after -f");
          }
          nolast = true;
        } else if (c == 'n') {
          auto num = value();
          if (not num) return builtin_error(name, "number expected after -n");
          numeric = opt<int>(s::atoi(num->c_str()));
        } else if (c == 'N') {
          numeric = opt<int>{};
        } else if (c == 'K') {
          auto km = value();
          if (not km) return builtin_error(name, "keymap expected after -K");
          keymap_restore = ed->keymap_name();
          if (not ed->select_keymap(*km)) {
            ed->set_message("No such keymap `" + *km + "'");
            return 1;
          }
        } else if (c == 'w') {
          set_bindk = true;
        } else {
          if (keymap_restore) ed->select_keymap(*keymap_restore);
          return builtin_error(name, "unknown option: " + a);
        }
      }
    }
    vec<str> wargs(args.begin() + static_cast<s::ptrdiff_t>(s::min(i, args.size())), args.end());
    opt<int> saved_numeric = ed->numeric();
    if (numeric) ed->set_numeric(*numeric);
    bool ok = ed->call_widget(wname, wargs, set_bindk, nolast);
    if (numeric) ed->set_numeric(saved_numeric);
    if (keymap_restore) ed->select_keymap(*keymap_restore);
    return ok ? 0 : 1;
  }

  OptParse o;
  if (not parse_options(args, "aAcCDfFgGIKlLmMNrRTUw", "", o)) return 1;
  vec<str> argv(args.begin() + static_cast<s::ptrdiff_t>(o.next), args.end());
  struct Op { char o; int min, max; };
  static constexpr Op ops[] = {
    {'l', 0, -1}, {'D', 1, -1}, {'A', 2, 2}, {'N', 1, 2}, {'C', 3, 3}, {'R', 0, -1},
    {'M', 1, 1}, {'U', 1, 1}, {'K', 1, 1}, {'I', 0, 0}, {'f', 1, -1}, {'F', 0, 2},
    {'T', 0, 2}, {0, 0, -1},
  };
  const Op* op = ops;
  while (op->o and not o.has(op->o)) ++op;
  if (op->o)
    for (const Op* opp = op + 1; opp->o; ++opp)
      if (o.has(opp->o)) return builtin_error(name, "incompatible operation selection options");
  int argc = static_cast<int>(argv.size());
  if (argc < op->min) return builtin_error(name, str("not enough arguments for -") + op->o);
  if (op->max != -1 and argc > op->max)
    return builtin_error(name, str("too many arguments for -") + op->o);

  switch (op->o) {
    case 'l': {
      if (argv.empty()) {
        for (const auto& w : zle.widget_names()) list_widget(zle, w, o.has('a') ? -1 : o.has('L'));
        return 0;
      }
      for (const auto& w : argv) {
        // A builtin's name taken away with -D is still found (getnode2).
        bool known = zle.is_widget(w) or zle.deleted_widgets.contains(w);
        if (not known or (not o.has('a') and not zle.user_widgets.contains(w))) return 1;
        if (o.has('L')) list_widget(zle, w, 1);
      }
      return 0;
    }
    case 'D': {
      int ret = 0;
      for (const auto& w : argv) {
        if (not zle.is_widget(w) or zle.deleted_widgets.contains(w)) {
          ret = builtin_error(name, "no such widget `" + w + "'");
        } else if (protected_widget(w)) {
          ret = builtin_error(name, "widget name `" + w + "' is protected");
        } else {
          bool known = zle.user_widgets.erase(w) + zle.widget_aliases.erase(w) > 0;
          zle.comp_widgets.erase(w);
          if (not known and Zle::is_builtin_widget(w)) zle.deleted_widgets.insert(w);
        }
      }
      return ret;
    }
    case 'A':
      if (not zle.is_widget(argv[0]))
        return builtin_error(name, "no such widget `" + argv[0] + "'");
      if (protected_widget(argv[1]))
        return builtin_error(name, "widget name `" + argv[1] + "' is protected");
      bind_widget(zle, argv[1], argv[0]);
      return 0;
    case 'N':
      if (protected_widget(argv[0]))
        return builtin_error(name, "widget name `" + argv[0] + "' is protected");
      zle.deleted_widgets.erase(argv[0]);
      zle.widget_aliases.erase(argv[0]);
      zle.comp_widgets.erase(argv[0]);
      zle.user_widgets[argv[0]] = argc > 1 ? argv[1] : argv[0];
      return 0;
    case 'C': {
      static const uset<str> styles = {
        "complete-word", "expand-or-complete", "expand-or-complete-prefix", "menu-complete",
        "menu-expand-or-complete", "reverse-menu-complete", "list-choices", "delete-char-or-list",
        "accept-and-menu-complete", "menu-select"};
      strv style = strv(argv[1]).starts_with('.') ? strv(argv[1]).substr(1) : strv(argv[1]);
      if (not styles.contains(str(style)))
        return builtin_error(name, "invalid widget `" + argv[1] + "'");
      if (protected_widget(argv[0]))
        return builtin_error(name, "widget name `" + argv[0] + "' is protected");
      zle.deleted_widgets.erase(argv[0]);
      zle.widget_aliases.erase(argv[0]);
      zle.user_widgets[argv[0]] = argv[2];
      zle.comp_widgets[argv[0]] = argv[1];
      return 0;
    }
    case 'R':
      if (not active) return 1;
      if (not argv.empty() and not argv[0].empty())
        ed->set_message(Editor::status_text(argv[0]));
      ed->redraw();
      return 0;
    case 'M':
      if (not active) return builtin_error(name, "can only be called from widget function");
      ed->set_message(Editor::nice_message(argv[0]));
      return 0;
    case 'U':
      if (not active) return builtin_error(name, "can only be called from widget function");
      ed->push_input(argv[0]);
      return 0;
    case 'K':
      if (not active) return builtin_error(name, "can only be called from widget function");
      if (not ed->select_keymap(argv[0])) {
        ed->set_message("No such keymap `" + argv[0] + "'");
        return 1;
      }
      return 0;
    case 'I':
      if (not active) return 1;
      ed->trash();
      return 0;
    case 'f': {
      if (not active) return builtin_error(name, "can only set flags from a widget");
      int ret = 0;
      for (const auto& f : argv) {
        if (f == "yank" or f == "yankbefore" or f == "kill" or f == "vichange")
          ed->set_widget_flag(f);
        else ret = builtin_error(name, "invalid flag `" + f + "' given to zle -f");
      }
      return ret;
    }
    case 'F': case 'T':
      return builtin_error(name, str("-") + op->o + ": not implemented");
    default:
      // bin_zle_call without a widget: whether a widget could be called.
      return active ? 0 : 1;
  }
}

// ---- vared ------------------------------------------------------------------

// vared, as zsh's bin_vared: the checks in its order, an array's elements
// joined with their separators and backslashes quoted, split back the
// same way.
int builtin_vared(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  OptParse o;
  if (not parse_options(args, "AacghepriMmftI", "prMmfitI", o)) return 1;
  if (o.next >= args.size()) return builtin_error(cmd, "not enough arguments");
  if (o.next + 1 < args.size()) return builtin_error(cmd, "too many arguments");
  const str& name = args[o.next];
  auto term = ctx.params.lookup("TERM");
  bool emacs = term and term->get().is_set() and term->get().scalar() == "emacs";
  if ((ctx.opts.get(OPT_INTERACTIVE) and not ctx.opts.get(OPT_ZLE)) or emacs)
    return builtin_error(cmd, "ZLE not enabled");
  if (zle_of(ctx).active and zle_of(ctx).active->active())
    return builtin_error(cmd, "ZLE cannot be used recursively (yet)");
  if (o.has('A') and o.has('a')) return builtin_error(cmd, "specify only one of -a and -A");
  if ((o.has('a') or o.has('A')) and not o.has('c'))
    builtin_error(cmd, o.has('a') ? "-a ignored" : "-A ignored");
  auto p = ctx.params.lookup(name);
  bool array = (o.has('c') and (o.has('a') or o.has('A'))) or (p and p->get().is_array());
  str initial;
  if (p and p->get().is_set()) {
    if (not is_identifier(name)) return builtin_error(cmd, "not an identifier: `" + name + "'");
    if (array) {
      // A separator or backslash in an element is quoted with a backslash.
      for (const auto& w : p->get().array()) {
        if (not initial.empty()) initial += ' ';
        for (char c : w) {
          if (c == '\\' or c == ' ' or c == '\t' or c == '\n') initial += '\\';
          initial += c;
        }
      }
    } else {
      initial = p->get().scalar();
    }
  } else if (not o.has('c')) {
    return builtin_error(cmd, "no such variable: " + name);
  } else if (not is_identifier(name)) {
    // zsh's fetchvalue takes a leading number as a positional parameter.
    if (s::isdigit(static_cast<unsigned char>(name[0])))
      return builtin_error(cmd, "not an identifier: `" + name + "'");
    return builtin_error(cmd, "invalid parameter name: " + name);
  }
  if (zle_of(ctx).tty_out < 0 and not ::isatty(STDIN_FILENO)) {
    int t = ::open("/dev/tty", O_RDWR | O_NOCTTY | O_CLOEXEC);
    if (t < 0) return builtin_error(cmd, "can't access terminal");
    ::close(t);
  }
  Zle& zle = zle_of(ctx);
  if (zle.active and zle.active->active()) return builtin_error(cmd, "ZLE cannot be used recursively (yet)");

  HistoryManager scratch{HistOptions{}};
  HistoryManager& hist = (o.has('h') and ctx.history) ? *ctx.history : scratch;
  PromptRenderer prompt(ctx.params, ctx.opts);
  prompt.set_hooks(ctx.executor->prompt_hooks());
  Editor ed(ctx.opts, hist, prompt, zle);
  if (zle.on_wake)
    ed.set_wake(Signals.wake_fd(), [&zle, &ed](bool& quit) { return zle.on_wake(ed, quit); });
  ed.set_vared_name(name);
  // zsh's savekeymap: -M links `main`, -m `vicmd`, to the keymap named
  // for the edit (a warning, and nothing, for one that doesn't exist);
  // restorekeymap links the old ones back after it.
  struct SavedKeymap {
    Zle&         zle;
    strv         link;
    sptr<Keymap> old;
    bool         given = false;
    SavedKeymap(Zle& z, strv l) : zle(z), link(l) {}
    ~SavedKeymap() {
      if (old)        zle.link_keymap(old, link);
      else if (given) s::fprintf(stderr, "vared: keymap %.*s was not defined, not restored\n",
                                 static_cast<int>(link.size()), link.data());
    }
    SavedKeymap(const SavedKeymap&) = delete;
    SavedKeymap& operator=(const SavedKeymap&) = delete;
  };
  auto save_keymap = [&](SavedKeymap& sk, const str* to) {
    if (not to) return;
    auto km = zle.keymap(*to);
    if (not km) {
      (void)builtin_error(cmd, "no such keymap: " + *to);
      return;
    }
    sk.given = true;
    if (auto cur = zle.keymap(sk.link); cur != km) {
      sk.old = cur;
      zle.link_keymap(km, sk.link);
    }
  };
  SavedKeymap main_save{zle, "main"}, vicmd_save{zle, "vicmd"};
  save_keymap(main_save, o.arg('M'));
  save_keymap(vicmd_save, o.arg('m'));
  str ps  = o.arg('p') ? *o.arg('p') : str{};
  str rps = o.arg('r') ? *o.arg('r') : str{};
  auto result = ed.edit(initial, ps, rps);
  // Ctrl-C ends the command line as an interrupt would (zsh's zleread
  // under its SIGINT handler): with $? 130, or the status of the INT trap
  // that broke the read.  send-break ends it leaving $? alone.
  if (ed.interrupted() and not ctx.exiting) {
    // zsh's handler sets errflag without breaks: a loop around vared ends
    // with 1, as after an error; after a TRAPINT's `return N`, with N
    // (dotrapargs, as `trap_status`).
    auto& tb = zle.trap_break;
    ctx.interrupt = tb.has_value();
    if (tb) {
      ctx.last_exit   = *s::exchange(tb, s::nullopt);
      ctx.trap_status = ctx.last_exit;
    } else if (not ed.sent_break()) {
      ctx.last_exit = 128 + SIGINT;
    }
    ctx.aborting = true;
    return ctx.last_exit;
  }
  if (not result or ed.interrupted()) return 1;
  Result<void> set;
  if (array) {
    // zsh's spacesplit with quoting: blanks separate the words, a
    // backslash keeps the next character.
    vec<str> words;
    str cur;
    bool any = false;
    for (s::size_t i = 0; i < result->size(); ++i) {
      char c = (*result)[i];
      if (c == '\\' and i + 1 < result->size()) {
        cur += (*result)[++i];
        any = true;
      } else if (c == ' ' or c == '\t' or c == '\n') {
        if (any) words.push_back(s::move(cur));
        cur.clear();
        any = false;
      } else {
        cur += c;
        any = true;
      }
    }
    if (any) words.push_back(s::move(cur));
    set = ctx.params.set_array(name, s::move(words));
  } else {
    set = ctx.params.set_scalar(name, *result);
  }
  if (not set) return builtin_error(cmd, set.error().message);
  return 0;
}

// ---- Completion ------------------------------------------------------------------

// compdef fn cmd…; compdef -d cmd…
int builtin_compdef(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  CompletionEngine* engine = zle_of(ctx).completion;
  if (not engine) return builtin_error(cmd, "completion is not available");
  OptParse o;
  if (not parse_options(args, "dapPkKenN", "", o)) return 1;
  vec<str> rest(args.begin() + static_cast<s::ptrdiff_t>(o.next), args.end());
  if (o.has('d')) {
    for (const auto& c : rest) engine->compdef_remove(c);
    return 0;
  }
  if (rest.empty()) {
    for (const auto& [c, fn] : engine->compdefs()) s::printf("%s %s\n", c.c_str(), fn.c_str());
    return 0;
  }
  if (rest.size() < 2) return builtin_error(cmd, "I need arguments");
  for (s::size_t i = 1; i < rest.size(); ++i) {
    str c = rest[i];
    if (auto eq = c.find('='); eq != str::npos) c.resize(eq);
    engine->compdef(c, rest[0]);
  }
  return 0;
}

namespace {

// _files, _directories: the files (directories) for the end of $PREFIX
// after its last /, added with the directory part as a hidden prefix.
int add_files(vec<str>& args, ExecContext& ctx, bool dirs_only) {
  strv cmd = args[0];
  CompletionEngine* engine = zle_of(ctx).completion;
  CompState* st = engine ? engine->state() : nullptr;
  if (not st) return builtin_error(cmd, "can only be called from completion function");
  OptParse o;
  if (not parse_options(args, "/g", "gWFPS", o)) return 1;
  str pattern = o.arg('g') ? *o.arg('g') : str{};
  auto slash = st->prefix.rfind('/');
  CompAddSpec spec;
  spec.hpre = slash == str::npos ? str{} : st->prefix.substr(0, slash + 1);
  auto found = engine->complete_files(comp_unquote(st->prefix, st->quote), dirs_only or o.has('/'),
                                      false, pattern);
  s::int64_t before = st->nmatches();
  for (auto& m : found) {
    spec.dir_suffix = m.suffix == "/" ? "/" : str{};
    s::size_t n = st->matches.size();
    st->add(spec, {m.word});
    if (st->matches.size() > n) st->matches.back().type = m.type;
  }
  return st->nmatches() > before ? 0 : 1;
}

}  // namespace

int builtin__files(vec<str>& args, ExecContext& ctx)       { return add_files(args, ctx, false); }
int builtin__directories(vec<str>& args, ExecContext& ctx) { return add_files(args, ctx, true); }

int builtin__command_names(vec<str>& args, ExecContext& ctx) {
  CompletionEngine* engine = zle_of(ctx).completion;
  CompState* st = engine ? engine->state() : nullptr;
  if (not st) return builtin_error(args[0], "can only be called from completion function");
  vec<str> names;
  for (auto& m : engine->complete_commands(comp_unquote(st->prefix, st->quote)))
    names.push_back(s::move(m.word));
  return st->add(CompAddSpec{}, names) > 0 ? 0 : 1;
}

}  // namespace jfsh
