#pragma once
// jfsh/editor/editor.hh — ZLE (Zsh Line Editor).
//
// Mirrors the ZLE subsystem in zsh's Src/Zle/.  Provides vi and emacs
// editing modes, keymaps, history navigation, and inline completion.
// The editor owns the terminal (raw mode) while reading a line.

#include <jfsh/core/types.hh>
#include <jfsh/core/options.hh>
#include <jfsh/history/history.hh>
#include <jfsh/prompt/prompt.hh>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace jfsh {

// ---------------------------------------------------------------------------
// EditorMode — vi or emacs
// ---------------------------------------------------------------------------
enum class EditorMode : std::uint8_t { Emacs, Vi };

// ---------------------------------------------------------------------------
// KeyBinding — maps a key sequence to a widget name
// ---------------------------------------------------------------------------
struct KeyBinding {
    std::string key_seq;    // raw escape sequence (e.g. "\x1b[A" for Up)
    std::string widget;     // ZLE widget name (e.g. "up-line-or-history")
};

// ---------------------------------------------------------------------------
// Keymap — a named set of key bindings
// ---------------------------------------------------------------------------
struct Keymap {
    std::string              name;
    std::vector<KeyBinding>  bindings;

    // Look up widget for a key sequence; returns "" if unbound.
    std::string_view lookup(std::string_view key_seq) const noexcept;
};

// ---------------------------------------------------------------------------
// ZleWidget — a named editor action
// ---------------------------------------------------------------------------
using ZleWidgetFn = std::function<bool(struct LineBuffer&)>;

struct ZleWidget {
    std::string  name;
    ZleWidgetFn  fn;
    std::uint32_t flags = 0;
};

// ---------------------------------------------------------------------------
// LineBuffer — the mutable edit buffer
// ---------------------------------------------------------------------------
struct LineBuffer {
    std::string text;
    std::size_t cursor = 0;      // cursor position (byte offset)
    int         mark   = -1;     // yank mark (-1 = unset)
    bool        overwrite = false; // overwrite vs insert mode

    // Helpers
    void insert(std::string_view s);
    void delete_char_at(std::size_t pos);
    void move_to(std::size_t pos);
    std::string_view before_cursor() const noexcept;
    std::string_view after_cursor()  const noexcept;
};

// ---------------------------------------------------------------------------
// Editor — the interactive line editor
// ---------------------------------------------------------------------------
class Editor {
public:
    Editor(const OptionState& opts,
           HistoryManager&    hist,
           PromptRenderer&    prompt);
    ~Editor();

    // Read a complete line from the terminal.
    // Returns the line (without trailing newline) or nullopt on EOF.
    std::optional<std::string> read_line();

    // Temporarily suspend the editor (e.g. while running a command).
    void suspend();
    // Restore after suspension.
    void resume();

    // Bind a key sequence to a widget in the named keymap.
    void bind_key(std::string keymap, std::string key_seq, std::string widget);

    // Register a custom ZLE widget.
    void register_widget(ZleWidget widget);

    // Switch between vi and emacs mode.
    void set_mode(EditorMode mode);
    EditorMode mode() const noexcept { return mode_; }

    // Force a full redraw (e.g. after SIGWINCH).
    void redraw();

    // Set completion function (called on Tab).
    using CompleteFn = std::function<std::vector<std::string>(std::string_view)>;
    void set_completer(CompleteFn fn) { completer_ = std::move(fn); }

private:
    const OptionState& opts_;
    HistoryManager&    hist_;
    PromptRenderer&    prompt_;
    EditorMode         mode_  = EditorMode::Emacs;
    LineBuffer         buf_;
    CompleteFn         completer_;

    // Keymaps: main, vicmd, viins, emacs, etc.
    std::vector<Keymap>              keymaps_;
    HashTable<ZleWidget>             widgets_;

    // History navigation state
    int     hist_pos_  = -1;    // -1 = not navigating
    std::string saved_line_;    // line before history navigation began

    // Terminal management
    bool raw_mode_active_ = false;
    void enter_raw_mode();
    void exit_raw_mode();

    // Key reading
    std::string read_key_sequence();

    // Widget dispatch
    bool dispatch_widget(std::string_view name);

    // Built-in widgets
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

    // Display
    void refresh_display();
    int  terminal_width()  const noexcept;
    int  terminal_height() const noexcept;
};

} // namespace jfsh
