#ifndef JFSH_EDITOR_ZLE_H
#define JFSH_EDITOR_ZLE_H
// jfsh/editor/zle.h — the line editor's state that outlives one line:
// keymaps, user-defined widgets and the buffer stack.
//
// Mirrors zsh's zle_keymap.c and zle_thingy.c.  A keymap maps key
// sequences (raw bytes) to widget names or, for `bindkey -s`, to strings
// that are read as input again.  Keymaps are shared objects with one or
// more names: `main` is another name for `emacs` or `viins`.  `bindkey`,
// `zle` and `vared` work on this state, so a .jfshrc can set it up before
// the first prompt.

#include <cstdint>

#include <map>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/param.h>
#include <jfsh/core/types.h>

namespace jfsh {

struct Editor;
struct CompletionEngine;

struct KeyBinding {
  str  widget;     // the widget to run, or
  str  macro;      // for bindkey -s, the keys to read instead
  bool is_macro = false;
};

struct Keymap {
  // Bound sequences, sorted by their bytes (the order bindkey lists them).
  s::map<str, KeyBinding> bindings;
  // Unbound bytes from 0x80 up (UTF-8) insert themselves: emacs, viins.
  bool insert_unbound = false;

  const KeyBinding* lookup(strv seq) const;
  // Whether some binding is longer than `seq` and starts with it.
  bool is_prefix(strv seq) const;
  void bind(strv seq, str widget);
  void bind_macro(strv seq, str keys);
  void unbind(strv seq) { bindings.erase(str(seq)); }
};

// zsh's notation for keys, as bindkey reads and lists them: ^X, ^?, \e,
// \M-x, \C-x, \n, \t, \NNN, \xNN and the like.
str parse_key_notation(strv text);
str format_key(strv seq);

// What the editor needs the rest of the shell for.
struct ZleHooks {
  // Runs the shell function `fn` for a user widget (zle -N), with $BUFFER,
  // $CURSOR, … set from `ed` and read back afterwards.  Returns its status.
  func<int(Editor& ed, const str& widget, const str& fn)> run_widget;
  // Runs a command line (edit-command-line's editor).
  func<int(const str& cmd)>                               run_command;
};

struct Zle {
  Zle();

  // Keymaps by name; aliases share the object.
  umap<str, sptr<Keymap>> keymaps;
  // User widgets (zle -N): widget name → shell function.
  s::map<str, str>        user_widgets;
  // zle -A: widget name → the widget it stands for.
  s::map<str, str>        widget_aliases;
  // Widgets written in C++ by a loaded module (zsh/deltochar): they act
  // as builtin widgets while the module is loaded.
  s::map<str, func<bool(Editor&)>> module_widgets;
  // push-line / push-input: lines to give back at the next prompt.
  vec<str>                buffer_stack;
  // accept-and-hold and friends: the buffer and history position the
  // next prompt starts from.
  opt<str>                next_buffer;
  int                     next_hist_index = -1;

  ZleHooks                hooks;
  const ParamTable*       params     = nullptr;
  CompletionEngine*       completion = nullptr;
  Editor*                 active     = nullptr;   // the editor reading a line

  sptr<Keymap> keymap(strv name) const;
  // The name a keymap is listed under (its primary name), for $KEYMAP.
  str keymap_name(const sptr<Keymap>& km) const;
  // Makes `main` another name for `name` (bindkey -e / -v / -A).
  bool link(strv existing, strv new_name);
  // Creates keymap `name`, a copy of `from` (or empty).
  bool create(strv name, strv from);
  // Restores the default keymaps (bindkey -d).
  void reset();
  // Selects emacs or viins as `main`, as $VISUAL/$EDITOR or `vi` ask.
  void select_main(bool vi) { link(vi ? "viins" : "emacs", "main"); }

  // Resolves zle -A aliases; empty when `name` names no widget.
  str resolve_widget(strv name) const;
  bool is_widget(strv name) const { return not resolve_widget(name).empty(); }

  // The builtin widgets' names, sorted.
  static const vec<str>& builtin_widgets();
  static bool is_builtin_widget(strv name);

  str param(strv name) const {
    if (not params) return {};
    auto p = params->lookup(name);
    return (p and p->get().is_set()) ? p->get().scalar() : str{};
  }
};

}  // namespace jfsh

#endif  // JFSH_EDITOR_ZLE_H
