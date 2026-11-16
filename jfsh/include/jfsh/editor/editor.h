#ifndef JFSH_EDITOR_EDITOR_H
#define JFSH_EDITOR_EDITOR_H
// jfsh/editor/editor.h — ZLE (Zsh Line Editor).
//
// Mirrors the ZLE subsystem in zsh's Src/Zle/.  Provides vi and emacs
// editing modes, keymaps, history navigation, and inline completion.

#include <cstdint>

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/options.h>
#include <jfsh/core/types.h>
#include <jfsh/history/history.h>
#include <jfsh/prompt/prompt.h>

namespace jfsh {

enum class EditorMode : s::uint8_t { Emacs, Vi };

struct KeyBinding {
  str key_seq;
  str widget;
};

struct Keymap {
  str             name;
  vec<KeyBinding> bindings;

  strv lookup(strv key_seq) const noexcept;
};

using ZleWidgetFn = func<bool(struct LineBuffer&)>;

struct ZleWidget {
  str         name;
  ZleWidgetFn fn;
  s::uint32_t flags = 0;
};

struct LineBuffer {
  str       text;
  s::size_t cursor    = 0;      // cursor position (byte offset)
  int       mark      = -1;     // yank mark (-1 = unset)
  bool      overwrite = false;

  void insert(strv s) {
    text.insert(cursor, s);
    cursor += s.size();
  }
  void delete_char_at(s::size_t pos) {
    if (pos < text.size()) text.erase(pos, 1);
  }
  void move_to(s::size_t pos) { cursor = s::min(pos, text.size()); }
  strv before_cursor() const noexcept {
    return strv(text).substr(0, cursor);
  }
  strv after_cursor() const noexcept {
    return strv(text).substr(cursor);
  }
};

struct Editor {
  Editor(const OptionState& opts, HistoryManager& hist, PromptRenderer& prompt);
  ~Editor();

  // Reads one line after PS1, or after PS2 for a `continuation` line of an
  // unfinished command.
  opt<str> read_line(bool continuation = false);

  void suspend() { if (mRawModeActive) exit_raw_mode(); }
  void resume() {}

  void bind_key(strv keymap, strv key_seq, strv widget) {}
  void register_widget(ZleWidget widget) {}
  void set_mode(EditorMode mode) { mMode = mode; }
  void redraw() { refresh_display(); }

  EditorMode mode() const noexcept { return mMode; }

  using CompleteFn = func<vec<str>(strv)>;
  void set_completer(CompleteFn fn) { mCompleter = s::move(fn); }

  // While a line is being read, `fd` (the shell's signal self-pipe) is
  // watched too; when it becomes readable the editor leaves raw mode and
  // calls `hook`, which handles the signals (traps, job notices).  The
  // hook returns whether it printed anything, so the prompt and line are
  // redrawn, and sets `quit` to end the read as at EOF (SIGHUP).
  using WakeFn = func<bool(bool& quit)>;
  void set_wake(int fd, WakeFn hook) { mWakeFd = fd; mWake = s::move(hook); }

  // The last read_line ended with Ctrl-C: the line (and, for a PS2 line,
  // the whole command) is to be dropped.
  bool interrupted() const noexcept { return mInterrupted; }

protected:
  void enter_raw_mode();
  void exit_raw_mode();

  str read_key_sequence();
  bool        dispatch_widget(strv name) { return false; }

  bool widget_self_insert(LineBuffer&) { return false; }
  bool widget_backward_delete_char(LineBuffer&) { return false; }
  bool widget_beginning_of_line(LineBuffer& buf) {
    buf.cursor = 0;
    return true;
  }
  bool widget_end_of_line(LineBuffer& buf) {
    buf.cursor = buf.text.size();
    return true;
  }
  bool widget_up_line_or_history(LineBuffer&);
  bool widget_down_line_or_history(LineBuffer&);
  bool widget_accept_line(LineBuffer&) { return true; }
  bool widget_complete_word(LineBuffer&) { return false; }
  bool widget_expand_or_complete(LineBuffer&) { return false; }

  void install_default_bindings() {
    if (mMode == EditorMode::Vi) install_vi_bindings();
    else                         install_emacs_bindings();
  }
  void install_vi_bindings() {}
  void install_emacs_bindings() {}

  void refresh_display();
  int  terminal_width()  const noexcept;
  int  terminal_height() const noexcept;

private:
  const OptionState&       mOpts;
  HistoryManager&          mHist;
  PromptRenderer&          mPrompt;
  EditorMode               mMode        = EditorMode::Emacs;
  LineBuffer               mBuf;
  CompleteFn               mCompleter;
  vec<Keymap>              mKeymaps;
  HashTable<ZleWidget>     mWidgets;
  int                      mHistPos     = -1;  // -1 = not navigating
  str                      mSavedLine;
  bool                     mRawModeActive = false;
  bool                     mContinuation  = false;  // prompting with PS2
  bool                     mInterrupted   = false;  // Ctrl-C ended the read
  bool                     mEof           = false;  // input closed or hung up
  int                      mWakeFd        = -1;
  WakeFn                   mWake;
};

}  // namespace jfsh

#endif  // JFSH_EDITOR_EDITOR_H
