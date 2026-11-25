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

#include <array>
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

// What a widget does to an auto-removable suffix completion left before
// the cursor (zsh's ZLE_KEEPSUFFIX): removes it, keeps it, or keeps it
// and leaves it removable by the widget after.
enum class SuffixRule : s::uint8_t { Remove, Keep, Pending };

// A widget written in C++ by a loaded module.
struct ModuleWidget {
  func<bool(Editor&)> fn;
  SuffixRule          suffix = SuffixRule::Remove;
};

struct KeyBinding {
  str  widget;     // the widget to run, or
  str  macro;      // for bindkey -s, the keys to read instead
  bool is_macro = false;
};

struct Keymap {
  // Bound sequences, sorted by their bytes.
  s::map<str, KeyBinding> bindings;
  // The name it was created under (zsh's primary name), for `bindkey -lL`.
  str primary;

  const KeyBinding* lookup(strv seq) const;
  // Whether some binding is longer than `seq` and starts with it.
  bool is_prefix(strv seq) const;
  void bind(strv seq, str widget);
  void bind_macro(strv seq, str keys);
  void unbind(strv seq) { bindings.erase(str(seq)); }
  // The bindings in the order zsh's scankeymap visits them: each
  // sequence of several keys after the single keys up to its first byte.
  vec<const s::pair<const str, KeyBinding>*> scan() const;
};

// zsh's notation for keys, as bindkey reads them: ^X, ^?, \e, \M-x,
// \C-x, \n, \t, \NNN, \xNN and the like; format_key lists a sequence
// (in double quotes) as bindkey does.
str parse_key_notation(strv text);
str bind_text(strv seq);
str format_key(strv seq);

// A cut buffer: $CUTBUFFER, a kill-ring entry or a vi register.
struct CutBuffer {
  str  text;
  bool set   = false;
  bool lines = false;   // whole lines (vi's yy, dd)
};

// What the editor needs the rest of the shell for.
struct ZleHooks {
  // Runs the shell function `fn` for a user widget (zle -N), with $BUFFER,
  // $CURSOR, … set from `ed` and read back afterwards, and `args` as its
  // arguments.  Returns its status.
  func<int(Editor& ed, const str& widget, const str& fn, const vec<str>& args)> run_widget;
  // Runs a command line (edit-command-line's editor).
  func<int(const str& cmd)>                               run_command;
  // Around a trap run while `ed` reads (zsh's zlebeforetrap and
  // zleaftertrap): a parameter scope with $BUFFER and the others read-only.
  func<void(Editor& ed, bool begin)>                      trap_scope;
};

struct Zle {
  Zle();

  // Keymaps by name; aliases share the object.
  umap<str, sptr<Keymap>> keymaps;
  // User widgets (zle -N, zle -C): widget name → shell function.
  s::map<str, str>        user_widgets;
  // Completion widgets (zle -C): widget name → the builtin completion
  // widget whose behaviour they have.
  s::map<str, str>        comp_widgets;
  // zle -A: widget name → the widget it stands for (a builtin's `.name`).
  s::map<str, str>        widget_aliases;
  // Builtin widgets' names removed with zle -D (their `.name` stays).
  uset<str>               deleted_widgets;
  // Widgets written in C++ by a loaded module (zsh/deltochar): they act
  // as builtin widgets while the module is loaded.
  s::map<str, ModuleWidget> module_widgets;
  // push-line / push-input: lines to give back at the next prompt.
  vec<str>                buffer_stack;
  // The cut buffers, one set for every editor (the prompt's, vared's), as
  // zsh's globals: the one yank inserts ($CUTBUFFER), the kill ring
  // behind it (kring, $killring: 8 buffers, the newest first) and the
  // vi registers ("a-"z, "0 the last yank, "1-"9 the last deletions).
  CutBuffer               cut_buf;
  vec<CutBuffer>          kring;
  s::array<CutBuffer, 36> vibuf;
  // accept-and-hold and friends: the buffer and history position the
  // next prompt starts from.
  opt<str>                next_buffer;
  int                     next_hist_index = -1;
  // zsh's stackcs: where the cursor goes in the line taken back from the
  // buffer stack (push-line, accept-and-hold), -1 at its end.
  int                     stack_cs        = -1;
  // The terminal editors draw on (zsh's SHTTY/shout), opened by the shell
  // at startup; -1 for standard output.
  int                     tty_out         = -1;

  ZleHooks                hooks;
  const ParamTable*       params     = nullptr;
  CompletionEngine*       completion = nullptr;
  Editor*                 active     = nullptr;   // the editor reading a line
  // What the interrupt key (VINTR) does in any editor (the prompt's,
  // vared's), as the shell's SIGINT handler would: true to end the read as
  // an interrupt; false to go on editing (INT ignored, or its trap ran).
  // A trap that broke the read leaves its status in `trap_break`.  Unset,
  // the key always interrupts.
  func<bool(Editor&)>     on_interrupt;
  // What a signal arriving while any editor reads does (traps, $TMOUT,
  // job notices), as Editor::set_wake's hook: true when it printed
  // something; `quit` to end the read because the shell exits.
  func<bool(Editor&, bool& quit)> on_wake;
  opt<int>                trap_break;

  sptr<Keymap> keymap(strv name) const;
  // The name a keymap is listed under (its primary name), for $KEYMAP.
  str keymap_name(const sptr<Keymap>& km) const;
  // Makes `new_name` another name for keymap `existing` (bindkey -e / -v
  // / -A); false for .safe or a keymap that doesn't exist.
  bool link(strv existing, strv new_name);
  void link_keymap(const sptr<Keymap>& km, strv name);
  // bindkey -D: drops the name.
  bool unlink(strv name);
  // zsh's scanprimaryname: another name of `km` (not main) becomes its
  // primary one, once that one has gone.
  void pick_primary(const sptr<Keymap>& km);
  // Creates keymap `name`, a copy of `from` (or empty).
  bool create(strv name, strv from);
  // Restores the default keymaps (bindkey -d).
  void reset();
  // Selects emacs or viins as `main`, as $VISUAL/$EDITOR or `vi` ask.
  void select_main(bool vi) { link(vi ? "viins" : "emacs", "main"); }

  // Resolves zle -A aliases; empty when `name` names no widget.
  str resolve_widget(strv name) const;
  bool is_widget(strv name) const { return not resolve_widget(name).empty(); }

  // Every widget name, sorted (zsh's thingytab: the builtins, their
  // `.names`, module, user and alias widgets).
  vec<str> widget_names() const;

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
