// Line-editor builtins: bindkey, zle, vared, and the completion ones,
// compdef, compadd, compinit and the _files/_directories/_command_names
// helpers.  Also the glue that lets the editor run shell functions (user
// widgets, compdef functions), with $BUFFER, $CURSOR and friends bound to
// the editor's state while they run.

#include <cstdio>
#include <cstdlib>
#include <unistd.h>

#include <algorithm>
#include <map>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/util.h>
#include <jfsh/core/aliases.h>
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
// editor, assigning changes it.
void bind_editor_params(ExecContext& ctx, Editor& ed) {
  ParamTable& p = ctx.params;
  auto& b = ed.buffer();
  bound(p, "BUFFER", PM_SCALAR, [&b] { return b.text; },
        [&b](str v) { b.text = s::move(v); b.cursor = s::min(b.cursor, b.text.size()); });
  bound(p, "CURSOR", PM_INTEGER, [&b] { return s::to_string(byte_to_char(b.text, b.cursor)); },
        [&b](str v) {
          long n = s::strtol(v.c_str(), nullptr, 10);
          b.cursor = char_to_byte(b.text, s::max(0L, n));
        });
  bound(p, "LBUFFER", PM_SCALAR, [&b] { return b.text.substr(0, b.cursor); },
        [&b](str v) {
          str rest = b.text.substr(b.cursor);
          b.cursor = v.size();
          b.text   = s::move(v) + rest;
        });
  bound(p, "RBUFFER", PM_SCALAR, [&b] { return b.text.substr(b.cursor); },
        [&b](str v) { b.text = b.text.substr(0, b.cursor) + v; });
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
  bound(p, "REGION_ACTIVE", PM_INTEGER, [&ed] { return str(ed.region_active() ? "1" : "0"); });
  bound(p, "MARK", PM_INTEGER, [&ed] { return s::to_string(ed.mark()); },
        [&ed](str v) { ed.set_mark(s::strtol(v.c_str(), nullptr, 10)); });
  bound(p, "CUTBUFFER", PM_SCALAR, [&ed] { return ed.cut_buffer(); },
        [&ed](str v) { ed.set_cut_buffer(s::move(v)); });
  if (auto n = ed.numeric())
    bound(p, "NUMERIC", PM_INTEGER, [&ed] { return s::to_string(ed.numeric().value_or(1)); },
          [&ed](str v) { ed.set_numeric(static_cast<int>(s::strtol(v.c_str(), nullptr, 10))); });
}

int call_shell_function(ExecContext& ctx, const str& fn, vec<str> args) {
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
  zle.hooks.run_widget = [&ctx](Editor& ed, const str& widget, const str& fn) {
    ctx.params.push_scope();
    bind_editor_params(ctx, ed);
    int rc = call_shell_function(ctx, fn, {});
    ctx.params.pop_scope();
    if (rc < 0) {
      ed.set_message("No such shell function `" + fn + "'");
      return 1;
    }
    (void)widget;
    return rc;
  };
  zle.hooks.run_command = [&ex](const str& cmd) { return ex.exec_string(cmd); };
  if (not zle.completion) return;
  CompletionSources src;
  src.command_names = [&ctx, &ex] {
    vec<str> names = ex.builtin_names();
    for (const auto& [name, body] : ctx.functions) names.push_back(name);
    for (const auto& name : ctx.autoloads) names.push_back(name);
    for (const auto& [name, def] : ctx.aliases.regular) names.push_back(name);
    for (strv r : RESERVED_WORDS) names.emplace_back(r);
    return names;
  };
  src.call_function = [&ctx](const str& fn, const CompContext& c) {
    ctx.params.push_scope();
    ctx.params.assign("words", c.args, PM_ARRAY | PM_LOCAL);
    ctx.params.assign("CURRENT", s::int64_t{c.current_arg + 1}, PM_INTEGER | PM_LOCAL);
    ctx.params.assign("PREFIX", c.prefix, PM_SCALAR | PM_LOCAL);
    ctx.params.assign("SUFFIX", str{}, PM_SCALAR | PM_LOCAL);
    ctx.params.assign("service", c.command, PM_SCALAR | PM_LOCAL);
    call_shell_function(ctx, fn, {});
    ctx.params.pop_scope();
  };
  zle.completion->set_sources(s::move(src));
}

// ---- bindkey ---------------------------------------------------------------

namespace {

// Prints `km`'s bindings as zsh does: runs of single keys bound to the
// same widget as a range, then the sequences.
void list_bindings(const Keymap& km, strv km_name, bool as_commands, bool is_main,
                   const opt<str>& prefix) {
  auto quoted = [](strv seq) { return "\"" + format_key(seq) + "\""; };
  const auto& b = km.bindings;
  for (auto it = b.begin(); it != b.end(); ++it) {
    const str& key = it->first;
    if (prefix and not (strv(key).starts_with(*prefix) and key.size() > prefix->size())) continue;
    const KeyBinding& kb = it->second;
    // A range: single bytes in a row, bound alike.
    auto last = it;
    if (key.size() == 1 and not kb.is_macro) {
      auto next = s::next(it);
      while (next != b.end() and next->first.size() == 1 and not next->second.is_macro
             and next->second.widget == kb.widget
             and static_cast<unsigned char>(next->first[0])
                 == static_cast<unsigned char>(last->first[0]) + 1) {
        last = next;
        ++next;
      }
    }
    str keys = quoted(key);
    if (last != it) keys += "-" + quoted(last->first);
    str value = kb.is_macro ? quoted(kb.macro) : kb.widget;
    if (as_commands) {
      str cmd = "bindkey";
      if (not is_main) cmd += " -M " + str(km_name);
      if (last != it) cmd += " -R";
      if (kb.is_macro) cmd += " -s";
      s::printf("%s %s %s\n", cmd.c_str(), keys.c_str(), value.c_str());
    } else {
      s::printf("%s %s\n", keys.c_str(), value.c_str());
    }
    it = last;
  }
}

}  // namespace

int builtin_bindkey(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  Zle& zle = zle_of(ctx);
  OptParse o;
  if (not parse_options(args, "ldevaAMNDrsRLmpu", "M", o)) return 1;
  vec<str> rest(args.begin() + static_cast<s::ptrdiff_t>(o.next), args.end());

  if (o.has('d')) {
    zle.reset();
    return 0;
  }
  if (o.has('l')) {
    vec<str> names;
    for (const auto& [name, km] : zle.keymaps) names.push_back(name);
    s::sort(names.begin(), names.end());
    if (not rest.empty()) names = rest;
    for (const auto& n : names) {
      auto km = zle.keymap(n);
      if (not km) return builtin_error(cmd, "no such keymap `" + n + "'");
      if (o.has('L')) {
        str primary = zle.keymap_name(km);
        if (primary != n) s::printf("bindkey -A %s %s\n", primary.c_str(), n.c_str());
        else              s::printf("bindkey -N %s\n", n.c_str());
      } else {
        s::printf("%s\n", n.c_str());
      }
    }
    return 0;
  }
  if (o.has('D')) {
    int rc = 0;
    for (const auto& n : rest) {
      if (not zle.keymaps.erase(n)) rc = builtin_error(cmd, "no such keymap `" + n + "'");
    }
    return rc;
  }
  if (o.has('A')) {
    if (rest.size() != 2) return builtin_error(cmd, "-A needs two keymap names");
    if (not zle.link(rest[0], rest[1]))
      return builtin_error(cmd, "no such keymap `" + rest[0] + "'");
    return 0;
  }
  if (o.has('N')) {
    if (rest.empty() or rest.size() > 2) return builtin_error(cmd, "-N needs one or two keymap names");
    if (not zle.create(rest[0], rest.size() > 1 ? rest[1] : str{}))
      return builtin_error(cmd, "no such keymap `" + rest[1] + "'");
    return 0;
  }

  // The keymap the rest works on.
  str km_name = "main";
  if (o.has('e')) { zle.link("emacs", "main"); km_name = "emacs"; }
  if (o.has('v')) { zle.link("viins", "main"); km_name = "viins"; }
  if (o.has('a')) km_name = "vicmd";
  if (auto* m = o.arg('M')) km_name = *m;
  auto km = zle.keymap(km_name);
  if (not km) return builtin_error(cmd, "no such keymap `" + km_name + "'");
  bool is_main = km_name == "main";

  if (o.has('r')) {
    int rc = 0;
    for (const auto& k : rest) {
      str seq = parse_key_notation(k);
      if (o.has('p')) {
        for (auto it = km->bindings.begin(); it != km->bindings.end();) {
          if (strv(it->first).starts_with(seq) and it->first.size() > seq.size()) it = km->bindings.erase(it);
          else ++it;
        }
      } else if (not km->bindings.erase(seq)) {
        rc = 1;
      }
    }
    return rc;
  }
  if (rest.empty()) {
    if (o.has('e') or o.has('v')) return 0;   // just selecting
    list_bindings(*km, km_name, o.has('L'), is_main, s::nullopt);
    return 0;
  }
  if (rest.size() == 1 and not o.has('s')) {
    str seq = parse_key_notation(rest[0]);
    if (o.has('p')) {
      list_bindings(*km, km_name, o.has('L'), is_main, seq);
      return 0;
    }
    const KeyBinding* b = km->lookup(seq);
    str value = not b ? str("undefined-key")
              : b->is_macro ? "\"" + format_key(b->macro) + "\"" : b->widget;
    if (o.has('L')) {
      if (not b) return 0;
      s::printf("bindkey%s%s \"%s\" %s\n", is_main ? "" : " -M ", is_main ? "" : km_name.c_str(),
                format_key(seq).c_str(), value.c_str());
    } else {
      s::printf("\"%s\" %s\n", format_key(seq).c_str(), value.c_str());
    }
    return 0;
  }
  if (rest.size() % 2 != 0) return builtin_error(cmd, "odd number of arguments");
  for (s::size_t i = 0; i + 1 < rest.size(); i += 2) {
    if (o.has('R')) {
      // "a-z": every key in the range.
      str spec = parse_key_notation(rest[i]);
      if (spec.size() != 3 or spec[1] != '-') return builtin_error(cmd, "malformed key range `" + rest[i] + "'");
      for (int c = static_cast<unsigned char>(spec[0]); c <= static_cast<unsigned char>(spec[2]); ++c)
        km->bind(str(1, static_cast<char>(c)), rest[i + 1]);
      continue;
    }
    str seq = parse_key_notation(rest[i]);
    if (seq.empty()) return builtin_error(cmd, "cannot bind to an empty key sequence");
    if (o.has('s')) km->bind_macro(seq, parse_key_notation(rest[i + 1]));
    else            km->bind(seq, rest[i + 1]);
  }
  return 0;
}

// ---- zle ------------------------------------------------------------------

int builtin_zle(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  Zle& zle = zle_of(ctx);
  Editor* ed = zle.active;
  // `zle widget args…`: run a widget, only while a line is being edited.
  if (args.size() >= 2 and not args[1].starts_with('-')) {
    if (not ed or not ed->active())
      return builtin_error(cmd, "widgets can only be called when ZLE is active");
    if (not zle.is_widget(args[1])) return builtin_error(cmd, "no such widget `" + args[1] + "'");
    if (args[1] == "reset-prompt") {
      ed->reset_prompt();
      return 0;
    }
    return ed->call_widget(args[1]) ? 0 : 1;
  }
  OptParse o;
  if (not parse_options(args, "lLaNDARMUKIcCFT", "", o)) return 1;
  vec<str> rest(args.begin() + static_cast<s::ptrdiff_t>(o.next), args.end());
  if (o.has('N') or o.has('C')) {
    if (rest.empty() or rest.size() > (o.has('C') ? 3u : 2u))
      return builtin_error(cmd, "-N needs a widget name and at most one function");
    str fn = rest.size() > 1 ? rest.back() : rest[0];
    if (rest[0].starts_with('.')) return builtin_error(cmd, "widget name `" + rest[0] + "' is protected");
    zle.user_widgets[rest[0]] = fn;
    zle.widget_aliases.erase(rest[0]);
    return 0;
  }
  if (o.has('D')) {
    int rc = 0;
    for (const auto& w : rest) {
      if (zle.user_widgets.erase(w) == 0 and zle.widget_aliases.erase(w) == 0)
        rc = builtin_error(cmd, "no such widget `" + w + "'");
    }
    return rc;
  }
  if (o.has('A')) {
    if (rest.size() != 2) return builtin_error(cmd, "-A needs two widget names");
    if (not zle.is_widget(rest[0])) return builtin_error(cmd, "no such widget `" + rest[0] + "'");
    zle.widget_aliases[rest[1]] = rest[0];
    zle.user_widgets.erase(rest[1]);
    return 0;
  }
  if (o.has('M')) {
    if (rest.size() != 1) return builtin_error(cmd, "-M needs one message");
    if (ed and ed->active()) ed->set_message(rest[0]);
    else                     s::fprintf(stderr, "%s\n", rest[0].c_str());
    return 0;
  }
  if (o.has('U')) {
    if (not ed or not ed->active()) return builtin_error(cmd, "can only be called from widget function");
    for (auto it = rest.rbegin(); it != rest.rend(); ++it) ed->push_input(*it);
    return 0;
  }
  if (o.has('K')) {
    if (not ed or not ed->active()) return builtin_error(cmd, "can only be called from widget function");
    if (rest.size() != 1 or not ed->select_keymap(rest[0]))
      return builtin_error(cmd, "no such keymap `" + (rest.empty() ? str{} : rest[0]) + "'");
    return 0;
  }
  if (o.has('R') or o.has('I')) {
    if (ed and ed->active()) {
      if (not rest.empty()) ed->set_message(rest[0]);
      if (o.has('I')) ed->trash();
      else            ed->redraw();
    }
    return 0;
  }
  if (o.has('F') or o.has('T'))
    return builtin_error(cmd, str("-") + (o.has('F') ? 'F' : 'T') + ": not implemented");
  // zle, zle -l [-L] [-a] [names]: the widgets.
  bool all = o.has('a');
  if (not rest.empty() and o.has('l')) {
    int rc = 0;
    for (const auto& w : rest) if (not zle.is_widget(w)) rc = 1;
    return rc;
  }
  vec<str> names;
  if (all)
    for (const auto& w : Zle::builtin_widgets()) names.push_back(w);
  for (const auto& [w, fn] : zle.user_widgets) names.push_back(w);
  for (const auto& [w, to] : zle.widget_aliases) names.push_back(w);
  s::sort(names.begin(), names.end());
  names.erase(s::unique(names.begin(), names.end()), names.end());
  for (const auto& w : names) {
    if (auto u = zle.user_widgets.find(w); u != zle.user_widgets.end()) {
      if (o.has('L')) s::printf("zle -N %s%s%s\n", w.c_str(), u->second == w ? "" : " ", u->second == w ? "" : u->second.c_str());
      else if (u->second == w) s::printf("%s\n", w.c_str());
      else s::printf("%s (%s)\n", w.c_str(), u->second.c_str());
    } else if (auto a = zle.widget_aliases.find(w); a != zle.widget_aliases.end()) {
      if (o.has('L')) s::printf("zle -A %s %s\n", a->second.c_str(), w.c_str());
      else            s::printf("%s (%s)\n", w.c_str(), a->second.c_str());
    } else if (not o.has('L')) {
      s::printf("%s\n", w.c_str());
    }
  }
  return 0;
}

// ---- vared ------------------------------------------------------------------

int builtin_vared(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  OptParse o;
  if (not parse_options(args, "AacghepriMmftI", "prMmfitI", o)) return 1;
  if (o.next + 1 != args.size()) return builtin_error(cmd, "one parameter name expected");
  const str& name = args[o.next];
  if (not is_identifier(name)) return builtin_error(cmd, "not an identifier: " + name);
  auto p = ctx.params.lookup(name);
  bool array = o.has('a') or o.has('A') or (p and p->get().is_array());
  str initial;
  if (p and p->get().is_set()) {
    if (array) {
      for (const auto& w : p->get().array()) initial += (initial.empty() ? "" : " ") + w;
    } else {
      initial = p->get().scalar();
    }
  } else if (not o.has('c')) {
    return builtin_error(cmd, "no such variable: " + name);
  }
  if (not ctx.opts.get(OPT_ZLE) or not ::isatty(STDIN_FILENO))
    return builtin_error(cmd, "ZLE not enabled");
  Zle& zle = zle_of(ctx);
  if (zle.active and zle.active->active()) return builtin_error(cmd, "ZLE cannot be used recursively (yet)");

  HistoryManager scratch{HistOptions{}};
  HistoryManager& hist = (o.has('h') and ctx.history) ? *ctx.history : scratch;
  PromptRenderer prompt(ctx.params, ctx.opts);
  prompt.set_hooks(ctx.executor->prompt_hooks());
  Editor ed(ctx.opts, hist, prompt, zle);
  if (auto* km = o.arg('M')) ed.select_keymap(*km);
  str ps  = o.arg('p') ? *o.arg('p') : str{};
  str rps = o.arg('r') ? *o.arg('r') : str{};
  auto result = ed.edit(initial, ps, rps);
  if (not result or ed.interrupted()) return 1;
  Result<void> set;
  if (array) {
    vec<str> words;
    for (const auto& w : HistoryManager::split_words(*result)) words.push_back(w);
    set = ctx.params.set_array(name, s::move(words));
  } else {
    set = ctx.params.set_scalar(name, *result);
  }
  if (not set) return builtin_error(cmd, set.error().message);
  return 0;
}

// ---- Completion ------------------------------------------------------------------

// compinit: completion is always on in jfsh; this is for .zshrc files.
int builtin_compinit(vec<str>&, ExecContext&) { return 0; }

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

// compadd [-S suf] [-P pre] [-d disparray] [-a] [-U] [-qfQ] [-X expl] [-J/-V grp] [--] words…
int builtin_compadd(vec<str>& args, ExecContext& ctx) {
  strv cmd = args[0];
  CompletionEngine* engine = zle_of(ctx).completion;
  if (not engine or not engine->collecting())
    return builtin_error(cmd, "can only be called from completion function");
  OptParse o;
  if (not parse_options(args, "SPpdaUqfQ1X2JVxlonkeiIWFrRMEsO/", "SPpdXJVxiIWFrRMEsOo", o)) return 1;
  vec<str> words(args.begin() + static_cast<s::ptrdiff_t>(o.next), args.end());
  if (o.has('a')) {
    vec<str> expanded;
    for (const auto& n : words) {
      auto p = ctx.params.lookup(n);
      if (p and p->get().is_set())
        for (const auto& w : p->get().is_array() ? p->get().array() : vec<str>{p->get().scalar()})
          expanded.push_back(w);
    }
    words = s::move(expanded);
  }
  vec<str> display;
  if (auto* d = o.arg('d')) {
    auto p = ctx.params.lookup(*d);
    if (p) display = p->get().array();
  }
  str prefix = o.arg('P') ? *o.arg('P') : str{};
  for (s::size_t i = 0; i < words.size(); ++i) {
    CompCandidate c;
    c.word = prefix + words[i];
    if (i < display.size()) c.display = display[i];
    if (auto* suf = o.arg('S')) c.suffix = *suf;
    if (o.has('/') and c.word.ends_with('/')) c.suffix = "";
    engine->add(s::move(c));
  }
  return words.empty() ? 1 : 0;
}

namespace {

int add_files(vec<str>& args, ExecContext& ctx, bool dirs_only) {
  strv cmd = args[0];
  CompletionEngine* engine = zle_of(ctx).completion;
  if (not engine or not engine->collecting() or not engine->current())
    return builtin_error(cmd, "can only be called from completion function");
  OptParse o;
  if (not parse_options(args, "/g", "gWFPS", o)) return 1;
  str pattern = o.arg('g') ? *o.arg('g') : str{};
  const CompContext& c = *engine->current();
  auto found = engine->complete_files(c.prefix, dirs_only or o.has('/'), false, pattern);
  for (auto& m : found) engine->add(s::move(m));
  return found.empty() ? 1 : 0;
}

}  // namespace

int builtin__files(vec<str>& args, ExecContext& ctx)       { return add_files(args, ctx, false); }
int builtin__directories(vec<str>& args, ExecContext& ctx) { return add_files(args, ctx, true); }

int builtin__command_names(vec<str>& args, ExecContext& ctx) {
  CompletionEngine* engine = zle_of(ctx).completion;
  if (not engine or not engine->collecting() or not engine->current())
    return builtin_error(args[0], "can only be called from completion function");
  auto found = engine->complete_commands(engine->current()->prefix);
  for (auto& m : found) engine->add(s::move(m));
  return found.empty() ? 1 : 0;
}

}  // namespace jfsh
