#ifndef JFSH_EDITOR_EDITOR_H
#define JFSH_EDITOR_EDITOR_H
// jfsh/editor/editor.h — ZLE (Zsh Line Editor).
//
// Mirrors the ZLE subsystem in zsh's Src/Zle/.  Keys are looked up in
// the current keymap (zle.h) and run named widgets: the builtin ones
// here (editing, history, vi mode in vi.cpp, completion in
// completion.cpp) or shell functions made widgets with `zle -N`.  The
// buffer is UTF-8; the cursor moves by character and the display uses
// each character's width.  refresh.cpp draws the prompt, the buffer
// (wrapped, over several lines if need be), RPROMPT and anything listed
// below it.

#include <cstdint>

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/options.h>
#include <jfsh/core/types.h>
#include <jfsh/editor/zle.h>
#include <jfsh/history/history.h>
#include <jfsh/prompt/prompt.h>

namespace jfsh {

enum class EditorMode : s::uint8_t { Emacs, Vi };

struct LineBuffer {
  str       text;
  s::size_t cursor    = 0;      // byte offset, always at a character start
  bool      overwrite = false;

  void insert(strv s) {
    text.insert(cursor, s);
    cursor += s.size();
  }
  void move_to(s::size_t pos) { cursor = s::min(pos, text.size()); }
  strv before_cursor() const noexcept { return strv(text).substr(0, cursor); }
  strv after_cursor()  const noexcept { return strv(text).substr(cursor); }
};

struct CompletionResult;

struct Editor {
  Editor(const OptionState& opts, HistoryManager& hist, PromptRenderer& prompt, Zle& zle);
  ~Editor();
  Editor(const Editor&)            = delete;
  Editor& operator=(const Editor&) = delete;

  // Reads one line after PS1, or after PS2 for a `continuation` line of an
  // unfinished command.
  opt<str> read_line(bool continuation = false);

  // vared: edits `initial` after `prompt` (and `rprompt`), both prompt
  // strings.  nullopt on EOF or Ctrl-C.
  opt<str> edit(str initial, strv prompt, strv rprompt = {});

  // The lines of an unfinished command read so far ($PREBUFFER).
  void set_prebuffer(str text) { mPrebuffer = s::move(text); }

  void suspend() { if (mRawModeActive) exit_raw_mode(); }
  void resume() {}

  // Reads from `in` and draws on `out` instead of the terminal on 0 and 1.
  void set_io(int in, int out) { mIn = in; mOut = out; }

  void set_mode(EditorMode mode) { mZle.select_main(mode == EditorMode::Vi); }
  EditorMode mode() const;
  void redraw() { refresh(); }

  // While a line is being read, `fd` (the shell's signal self-pipe) is
  // watched too; when it becomes readable the editor leaves raw mode and
  // calls `hook`, which handles the signals (traps, job notices).  The
  // hook returns whether it printed anything, so the prompt and line are
  // redrawn, and sets `quit` to end the read as at EOF (SIGHUP).  A hook
  // about to print calls trash() first.
  using WakeFn = func<bool(bool& quit)>;
  void set_wake(int fd, WakeFn hook) { mWakeFd = fd; mWake = s::move(hook); }

  // Clears the prompt and line from the screen, leaving the cursor where
  // the prompt started, so other output can go there; the next refresh
  // draws them again.
  void trash();

  // The last read_line ended with Ctrl-C: the line (and, for a PS2 line,
  // the whole command) is to be dropped.
  bool interrupted() const noexcept { return mInterrupted; }

  // ---- What `zle` and user widgets see -----------------------------------
  bool active() const noexcept { return mReading; }
  LineBuffer&       buffer()       noexcept { return mBuf; }
  const LineBuffer& buffer() const noexcept { return mBuf; }
  // Runs widget `name` (`zle name`); false when it failed or is unknown.
  bool call_widget(strv name);
  void push_input(strv keys) { mPending.insert(0, keys); }   // zle -U
  void set_message(str msg)  { mMessage = s::move(msg); }    // zle -M
  void reset_prompt();                                       // zle reset-prompt
  bool select_keymap(strv name);                             // zle -K
  str  keymap_name() const;
  const str& widget_name()      const noexcept { return mWidget; }
  const str& last_widget_name() const noexcept { return mLastWidget; }
  const str& keys()             const noexcept { return mKeys; }
  opt<int>   numeric()          const noexcept { return mArg; }
  void       set_numeric(opt<int> n) { mArg = n; }
  int        hist_number() const;
  const str& prebuffer()   const noexcept { return mPrebuffer; }
  str        cut_buffer()  const { return mKillRing.empty() ? str{} : mKillRing.front(); }
  void       set_cut_buffer(str s);
  long       mark() const noexcept { return mMark ? static_cast<long>(*mMark) : 0; }
  void       set_mark(long m);
  bool       region_active() const noexcept { return mRegionActive; }

  static vec<str> builtin_widget_names();

  // The widget functions, by name.
  using WidgetFn = bool (Editor::*)();
  static const umap<str, WidgetFn>& widget_table();

protected:
  void enter_raw_mode();
  void exit_raw_mode();

  // The editing loop shared by read_line and edit().
  opt<str> run(str initial);
  // Reads one byte: queued input first, then the terminal.  -1 on a
  // timeout (`timeout_ms` >= 0) or at EOF (mEof set).
  int  read_byte(int timeout_ms);
  // Reads the key sequence bound in the current keymap; `binding` gets
  // what it is bound to (null when unbound, `seq` holding the keys).
  str  read_binding(const KeyBinding*& binding);
  // Runs `name` with the numeric argument and undo bookkeeping.
  bool run_widget(const str& name);
  bool dispatch(const str& name);
  void beep();

  // ---- Display (refresh.cpp) ---------------------------------------------
  void refresh();
  void finish_display();          // cursor below the line, for accept
  void write_out(strv data);
  int  terminal_width() const noexcept;
  int  terminal_height() const noexcept;
  // Shows `rows` below the line until the next listing or accept.
  void show_listing(str rows) { mListing = s::move(rows); }

  // ---- Helpers for widgets ------------------------------------------------
  int  count() const noexcept { return mArg ? *mArg : 1; }
  bool is_word_char(char32_t c) const;
  s::size_t next_word_start(s::size_t pos) const;   // forward-word
  s::size_t prev_word_start(s::size_t pos) const;   // backward-word
  s::size_t word_end(s::size_t pos) const;          // kill-word's end
  s::size_t line_start(s::size_t pos) const;        // of the buffer line
  s::size_t line_end(s::size_t pos) const;
  void kill_text(s::size_t from, s::size_t to, bool backward);
  void push_kill(str text, bool backward);
  void set_history_line(int index);   // shows history entry `index`
  bool history_matches(int index, strv prefix) const;
  // The words of history entry `index` (insert-last-word, copy-prev-word).
  vec<str> history_words(int index) const;
  bool incremental_search(bool backward);
  opt<str> minibuffer(strv prompt);   // reads a line below the buffer

  // ---- Completion (completion.cpp) ---------------------------------------
  bool do_complete(bool list_only, bool menu, bool reverse = false);
  void list_matches(const CompletionResult& r);
  void apply_match(const CompletionResult& r, s::size_t index, bool final);

  // ---- Builtin widgets ----------------------------------------------------
  bool w_accept_line();
  bool w_accept_and_hold();
  bool w_accept_line_and_down_history();
  bool w_send_break();
  bool w_self_insert();
  bool w_self_insert_unmeta();
  bool w_quoted_insert();
  bool w_bracketed_paste();
  bool w_forward_char();
  bool w_backward_char();
  bool w_forward_word();
  bool w_backward_word();
  bool w_emacs_forward_word();
  bool w_emacs_backward_word();
  bool w_beginning_of_line();
  bool w_end_of_line();
  bool w_beginning_of_buffer_or_history();
  bool w_end_of_buffer_or_history();
  bool w_backward_delete_char();
  bool w_delete_char();
  bool w_delete_char_or_list();
  bool w_backward_kill_word();
  bool w_kill_word();
  bool w_kill_line();
  bool w_backward_kill_line();
  bool w_kill_whole_line();
  bool w_kill_buffer();
  bool w_kill_region();
  bool w_copy_region_as_kill();
  bool w_yank();
  bool w_yank_pop();
  bool w_transpose_chars();
  bool w_transpose_words();
  bool w_capitalize_word();
  bool w_up_case_word();
  bool w_down_case_word();
  bool w_set_mark_command();
  bool w_exchange_point_and_mark();
  bool w_deactivate_region();
  bool w_overwrite_mode();
  bool w_undo();
  bool w_redo();
  bool w_clear_screen();
  bool w_redisplay();
  bool w_digit_argument();
  bool w_neg_argument();
  bool w_universal_argument();
  bool w_up_line_or_history();
  bool w_down_line_or_history();
  bool w_up_line();
  bool w_down_line();
  bool w_up_history();
  bool w_down_history();
  bool w_beginning_of_history();
  bool w_end_of_history();
  bool w_history_search_backward();
  bool w_history_search_forward();
  bool w_history_beginning_search_backward();
  bool w_history_beginning_search_forward();
  bool w_history_incremental_search_backward();
  bool w_history_incremental_search_forward();
  bool w_infer_next_history();
  bool w_insert_last_word();
  bool w_copy_prev_word();
  bool w_push_line();
  bool w_get_line();
  bool w_expand_history();
  bool w_magic_space();
  bool w_edit_command_line();
  bool w_quote_line();
  bool w_quote_region();
  bool w_what_cursor_position();
  bool w_execute_named_cmd();
  bool w_execute_last_named_cmd();
  bool w_vi_cmd_mode();
  bool w_beep();
  bool w_pound_insert();
  bool w_vi_match_bracket();
  bool w_vi_find_next_char();
  bool w_vi_join();
  bool w_vi_goto_column();
  bool w_expand_or_complete();
  bool w_complete_word();
  bool w_menu_complete();
  bool w_reverse_menu_complete();
  bool w_list_choices();
  bool w_expand_word();
  bool w_run_help();
  bool w_which_command();

  // vi.cpp
  bool w_vi_insert();
  bool w_vi_add_next();
  bool w_vi_add_eol();
  bool w_vi_insert_bol();
  bool w_vi_open_line_below();
  bool w_vi_open_line_above();
  bool w_vi_forward_char();
  bool w_vi_backward_char();
  bool w_vi_forward_word();
  bool w_vi_forward_blank_word();
  bool w_vi_backward_word();
  bool w_vi_backward_blank_word();
  bool w_vi_forward_word_end();
  bool w_vi_forward_blank_word_end();
  bool w_vi_backward_word_end();
  bool w_vi_backward_blank_word_end();
  bool w_vi_beginning_of_line();
  bool w_vi_end_of_line();
  bool w_vi_first_non_blank();
  bool w_vi_digit_or_beginning_of_line();
  bool w_vi_find_prev_char();
  bool w_vi_find_next_char_skip();
  bool w_vi_find_prev_char_skip();
  bool w_vi_repeat_find();
  bool w_vi_rev_repeat_find();
  bool w_vi_delete();
  bool w_vi_change();
  bool w_vi_yank();
  bool w_vi_up_case();
  bool w_vi_down_case();
  bool w_vi_oper_swap_case();
  bool w_vi_indent();
  bool w_vi_unindent();
  bool w_vi_delete_char();
  bool w_vi_backward_delete_char();
  bool w_vi_kill_eol();
  bool w_vi_change_eol();
  bool w_vi_change_whole_line();
  bool w_vi_substitute();
  bool w_vi_yank_whole_line();
  bool w_vi_put_after();
  bool w_vi_put_before();
  bool w_put_replace_selection();
  bool w_vi_replace();
  bool w_vi_replace_chars();
  bool w_vi_swap_case();
  bool w_vi_set_buffer();
  bool w_vi_set_mark();
  bool w_vi_goto_mark();
  bool w_vi_goto_mark_line();
  bool w_vi_kill_line();
  bool w_vi_backward_kill_word();
  bool w_vi_quoted_insert();
  bool w_vi_up_line_or_history();
  bool w_vi_down_line_or_history();
  bool w_vi_fetch_history();
  bool w_vi_history_search_backward();
  bool w_vi_history_search_forward();
  bool w_vi_repeat_search();
  bool w_vi_rev_repeat_search();
  bool w_vi_repeat_change();
  bool w_visual_mode();
  bool w_visual_line_mode();
  bool w_select_in_word();
  bool w_select_a_word();
  bool w_select_in_blank_word();
  bool w_select_a_blank_word();
  bool w_select_in_shell_word();
  bool w_select_a_shell_word();
  bool w_select_in_quoted();
  bool w_select_a_quoted();
  bool w_select_in_bracketed();
  bool w_select_a_bracketed();

  // vi helpers
  enum class ViOp : s::uint8_t { Delete, Change, Yank, Upper, Lower, Swap, Indent, Unindent };
  bool vi_operator(ViOp op);
  void vi_apply(ViOp op, s::size_t from, s::size_t to, bool linewise);
  bool vi_find(char32_t c, int dir, bool skip, int n, bool repeat = false);
  s::size_t vi_word_forward(s::size_t pos, bool blank) const;
  s::size_t vi_word_end(s::size_t pos, bool blank) const;
  s::size_t vi_word_backward(s::size_t pos, bool blank) const;
  s::size_t vi_word_end_backward(s::size_t pos, bool blank) const;
  bool vi_select_word(bool around, bool blank);
  bool vi_select_quoted(bool around);
  bool vi_select_bracketed(bool around);
  void vi_store_register(str text, bool linewise);
  str  vi_read_char();
  void vi_enter_insert();
  void vi_clamp_cursor();     // vicmd: not past the last character
  bool vi_history_search(int dir, bool prompt);
  bool in_vicmd() const;

  friend struct CompletionEngine;

private:
  const OptionState&       mOpts;
  HistoryManager&          mHist;
  PromptRenderer&          mPrompt;
  Zle&                     mZle;
  int                      mIn  = 0;
  int                      mOut = 1;

  LineBuffer               mBuf;
  sptr<Keymap>             mKeymap;           // the current keymap
  sptr<Keymap>             mLocalKeymap;      // visual mode's overlay
  str                      mPending;          // input to read before the terminal
  str                      mPrebuffer;
  str                      mWidget, mLastWidget, mKeys;
  opt<int>                 mArg;
  bool                     mArgSet       = false;  // the widget set the argument
  bool                     mArgNeg       = false;  // neg-argument, no digits yet
  bool                     mDone         = false;
  bool                     mReading      = false;

  // Prompts, expanded once per line (B26).
  str                      mPromptText, mRPromptText;
  bool                     mVared        = false;
  strv                     mVaredPrompt, mVaredRPrompt;

  // History navigation: index into mHist.entries(), size() for the line
  // being typed.  Edits to history lines are kept until the line is done.
  int                      mHistIndex    = 0;
  umap<int, str>           mHistEdits;
  str                      mSearchPrefix;     // history-search-*
  int                      mLastWordIndex = 0;  // insert-last-word
  int                      mLastWordHist  = 0;
  s::size_t                mLastWordStart = 0, mLastWordEnd = 0;

  // Kill ring (newest first) and the last yank, for yank-pop.
  vec<str>                 mKillRing;
  int                      mYankIndex    = 0;
  s::size_t                mYankStart    = 0, mYankEnd = 0;
  opt<s::size_t>           mMark;
  bool                     mRegionActive = false;
  bool                     mRegionLines  = false;

  // Undo: snapshots of (text, cursor); merged while typing.
  struct Snapshot { str text; s::size_t cursor; };
  vec<Snapshot>            mUndo, mRedo;
  // Undo grouping: 0 none, 1 a group (vi insert) waiting for its first
  // change, 2 a group whose snapshot is taken.
  int                      mUndoGroup    = 0;
  bool                     mKilledNow    = false;   // this widget killed text
  bool                     mLastWasKill  = false;   // … and the one before did
  str                      mLastIsearch;

  // Display.
  int                      mCursorRow    = 0;   // cursor row, from the prompt's first row
  int                      mRowsDrawn    = 0;
  int                      mCols         = 0;
  bool                     mDrawn        = false;
  bool                     mFinalDraw    = false;   // accept: cursor to the end
  str                      mMessage;            // zle -M, isearch, …
  str                      mListing;            // completion lists

  // Completion state between Tabs.
  vec<str>                 mMenu;               // menu completion: candidates
  s::size_t                mMenuIndex    = 0;
  s::size_t                mMenuStart    = 0, mMenuEnd = 0;
  str                      mMenuSuffixKind;
  bool                     mMenuActive   = false;
  bool                     mListed       = false;   // the last Tab listed
  str                      mCompleteLine;           // buffer the last Tab saw

  // vi state.
  char                     mViRegister   = 0;
  umap<char, s::pair<str, bool>> mViRegisters;   // text, linewise
  bool                     mViUnnamedLines = false; // the kill ring's top is lines
  bool                     mViOperating  = false;   // a motion is an operator's
  char32_t                 mViFindChar   = 0;
  int                      mViFindDir    = 0;
  bool                     mViFindSkip   = false;
  str                      mViSearch;
  int                      mViSearchDir  = -1;
  str                      mViLastChange;
  str                      mViRecording;
  bool                     mViRecord     = false;
  bool                     mViReplaying  = false;
  umap<char, s::size_t>    mViMarks;
  // Set by a text object run as an operator's motion.
  opt<s::pair<s::size_t, s::size_t>> mViObject;
  bool                     mViObjectLines = false;

  str                      mLastNamedCmd;

  bool                     mRawModeActive = false;
  bool                     mContinuation  = false;  // prompting with PS2
  bool                     mInterrupted   = false;  // Ctrl-C ended the read
  bool                     mEof           = false;  // input closed or hung up
  int                      mWakeFd        = -1;
  WakeFn                   mWake;
};

}  // namespace jfsh

#endif  // JFSH_EDITOR_EDITOR_H
