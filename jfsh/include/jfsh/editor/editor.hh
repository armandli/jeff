#ifndef JFSH_EDITOR_EDITOR_HH
#define JFSH_EDITOR_EDITOR_HH
// jfsh/editor/editor.hh — ZLE (Zsh Line Editor).
//
// Mirrors the ZLE subsystem in zsh's Src/Zle/.  Provides vi and emacs
// editing modes, keymaps, history navigation, and inline completion.

#include <cstdint>

#include <functional>
#include <optional>
#include <string>
#include <vector>

#include <jfsh/core/options.hh>
#include <jfsh/core/types.hh>
#include <jfsh/history/history.hh>
#include <jfsh/prompt/prompt.hh>

namespace jfsh {

enum class EditorMode : std::uint8_t { Emacs, Vi };

struct KeyBinding {
  std::string key_seq;
  std::string widget;
};

struct Keymap {
  std::string             name;
  std::vector<KeyBinding> bindings;

  std::string_view lookup(std::string_view key_seq) const noexcept;
};

using ZleWidgetFn = std::function<bool(struct LineBuffer&)>;

struct ZleWidget {
  std::string   name;
  ZleWidgetFn   fn;
  std::uint32_t flags = 0;
};

struct LineBuffer {
  std::string text;
  std::size_t cursor    = 0;      // cursor position (byte offset)
  int         mark      = -1;     // yank mark (-1 = unset)
  bool        overwrite = false;

  void             insert(std::string_view s);
  void             delete_char_at(std::size_t pos);
  void             move_to(std::size_t pos);
  std::string_view before_cursor() const noexcept;
  std::string_view after_cursor()  const noexcept;
};

struct Editor {
  Editor(const OptionState& opts, HistoryManager& hist, PromptRenderer& prompt);
  ~Editor();

  std::optional<std::string> read_line();

  void suspend();
  void resume();

  void bind_key(std::string keymap, std::string key_seq, std::string widget);
  void register_widget(ZleWidget widget);
  void set_mode(EditorMode mode);
  void redraw();

  EditorMode mode() const noexcept { return mMode; }

  using CompleteFn = std::function<std::vector<std::string>(std::string_view)>;
  void set_completer(CompleteFn fn) { mCompleter = std::move(fn); }

protected:
  void enter_raw_mode();
  void exit_raw_mode();

  std::string read_key_sequence();
  bool        dispatch_widget(std::string_view name);

  bool widget_self_insert(LineBuffer&);
  bool widget_backward_delete_char(LineBuffer&);
  bool widget_beginning_of_line(LineBuffer&);
  bool widget_end_of_line(LineBuffer&);
  bool widget_up_line_or_history(LineBuffer&);
  bool widget_down_line_or_history(LineBuffer&);
  bool widget_accept_line(LineBuffer&);
  bool widget_complete_word(LineBuffer&);
  bool widget_expand_or_complete(LineBuffer&);

  void install_default_bindings();
  void install_vi_bindings();
  void install_emacs_bindings();

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
  std::vector<Keymap>      mKeymaps;
  HashTable<ZleWidget>     mWidgets;
  int                      mHistPos     = -1;  // -1 = not navigating
  std::string              mSavedLine;
  bool                     mRawModeActive = false;
};

}  // namespace jfsh

#endif  // JFSH_EDITOR_EDITOR_HH
