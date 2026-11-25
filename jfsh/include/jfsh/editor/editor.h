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

#include <array>
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
  // zsh's spaceinline and shiftchars (foredel, backdel) ran: text was
  // inserted or deleted, which ends an active region.  A rewrite in place
  // (transpose, a case change, quote-line, a history line) edits `text`
  // without these.
  bool      spaced    = false;

  void insert(strv s) {
    text.insert(cursor, s);
    cursor += s.size();
    spaced = true;
  }
  void insert_at(s::size_t at, strv s) {
    text.insert(at, s);
    spaced = true;
  }
  void erase(s::size_t at, s::size_t n) {
    text.erase(at, n);
    spaced = true;
  }
  void replace(s::size_t at, s::size_t n, strv s) {   // foredel, then spaceinline
    text.replace(at, n, s);
    spaced = true;
  }
  void move_to(s::size_t pos) { cursor = s::min(pos, text.size()); }
  strv before_cursor() const noexcept { return strv(text).substr(0, cursor); }
  strv after_cursor()  const noexcept { return strv(text).substr(cursor); }
};

struct CompletionResult;
struct CompCandidate;

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

  // An auto-removable suffix stays for good: a widget function set
  // $BUFFER, $CURSOR, $LBUFFER or $RBUFFER (zsh's fixsuffix).
  void fix_suffix() { mAutoSuffix.reset(); }

  // vared: the name of the parameter edited ($compstate[vared]).
  void set_vared_name(str name) { mVaredName = s::move(name); }

  // The lines of an unfinished command read so far ($PREBUFFER).
  void set_prebuffer(str text) { mPrebuffer = s::move(text); }

  void suspend() { if (mRawModeActive) exit_raw_mode(); }
  // After a widget function ran a command (a pipeline, `$(…)`): the job
  // code put the shell's terminal modes back, so the editor's go on again.
  void resume_raw_mode();
  void resume() { if (mReading and not mRawModeActive) enter_raw_mode(); }

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
  // zsh's trashzle: leaves the prompt and line on the screen and moves
  // below them, so other output can go there; the next refresh draws
  // them again after it.
  void trash();
  // Output went where the cursor was (a trap run at the interrupt key):
  // the next refresh draws the prompt and line again from there.
  void forget_display() { mDrawn = mTrashed = false; mCursorRow = 0; }
  // An `exit` in such a trap: the read ends without drawing the line again.
  void quit() { mQuit = true; }

  // The last read_line ended with Ctrl-C: the line (and, for a PS2 line,
  // the whole command) is to be dropped.
  bool interrupted() const noexcept { return mInterrupted; }
  // The last read_line ended with Ctrl-D under IGNORE_EOF.
  bool eof_ignored() const noexcept { return mEofIgnored; }
  // The interrupted read was ended by send-break (^G), not by VINTR.
  bool sent_break() const noexcept { return mSentBreak; }

  // ---- What `zle` and user widgets see -----------------------------------
  bool active() const noexcept { return mReading; }
  LineBuffer&       buffer()       noexcept { return mBuf; }
  const LineBuffer& buffer() const noexcept { return mBuf; }
  // Ends the line being edited as send-break does (an error in a widget).
  void abort_line() { mInterrupted = mDone = true; }
  // Runs widget `name` (`zle name`); false when it failed or is unknown.
  bool call_widget(strv name, vec<str> args = {}, bool set_bindk = false, bool nolast = false);
  void set_widget_flag(strv flag);   // zle -f
  void push_input(strv keys) { mPending.insert(0, keys); }   // zle -U
  void set_message(str msg)  { mMessage = s::move(msg); }    // zle -M
  // Message text as zsh draws it: zle -M through showmsg (nicechar: `\t`,
  // `^X`, `\M-…` for a byte that is no character, newlines kept); the
  // status line (isearch, the minibuffers, zle -R) with every control
  // character, a newline too, as `^X`.
  static str nice_message(strv msg);
  static str status_text(strv text);
  void reset_prompt();                                       // zle reset-prompt
  bool select_keymap(strv name);                             // zle -K
  str  keymap_name() const;
  const str& widget_name()      const noexcept { return mWidget; }
  // The builtin completion widget a completion widget acts like
  // ($WIDGETSTYLE), empty in other widgets.
  const str& widget_style()     const noexcept { return mWidgetStyle; }
  const str& last_widget_name() const noexcept { return mLastWidget; }
  const str& keys()             const noexcept { return mKeys; }
  opt<int>   numeric()          const noexcept { return mArg; }
  void       set_numeric(opt<int> n) { mArg = n; }
  int        hist_number() const;
  const str& prebuffer()   const noexcept { return mPrebuffer; }
  str        cut_buffer()  const { return mZle.cut_buf.text; }
  void       set_cut_buffer(str s);
  long       mark() const noexcept { return mMark ? static_cast<long>(*mMark) : 0; }
  void       set_mark(long m);
  bool       region_active() const noexcept { return mRegionActive; }
  void       set_region_active(bool on) noexcept { mRegionActive = on; }

  static vec<str> builtin_widget_names();

  // For widgets written by modules (Zle::module_widgets).
  // Reads one character (UTF-8) from the keyboard; empty on ^G/^C/ESC/EOF.
  str  read_char() { return vi_read_char(); }
  // The numeric argument, 1 without one.
  int  repeat_count() const noexcept { return count(); }
  // Kills [from, to) into the kill ring, as kill-word does.
  void kill_range(s::size_t from, s::size_t to, bool backward) {
    kill_text(from, to, backward);
  }

  // The widget functions, by name.
  using WidgetFn = bool (Editor::*)();
  struct WidgetInfo {
    WidgetInfo(WidgetFn f, SuffixRule r = SuffixRule::Remove) : fn(f), suffix(r) {}
    WidgetFn   fn;
    SuffixRule suffix;
  };

  static const umap<str, WidgetInfo>& widget_table();

protected:
  // A cut buffer: the kill ring's and the vi registers' entries.
  // An auto-removable suffix a completion left before the cursor.
  struct AutoSuffix {
    s::size_t end = 0;      // where the suffix ends in the line
    s::size_t len = 0;
    str       chars;        // compadd -r's characters
    str       func;         // compadd -R's function
  };

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
  void reset_modifiers() {
    mArg.reset();
    mTMult    = 1;
    mArgBase  = 10;
    mTMultSet = mArgNeg = false;
    mViRegister = 0;
  }
  int  keytimeout() const;            // $KEYTIMEOUT in milliseconds
  bool use_tab() const;               // zsh's usetab
  void do_insert(strv s);
  bool read_rest_char(str& keys);
  str  last_full_char();
  bool is_word_char(char32_t c) const;
  s::size_t next_word_start(s::size_t pos) const;   // forward-word
  s::size_t prev_word_start(s::size_t pos) const;   // backward-word
  s::size_t prev_word_start_n(s::size_t pos, int n) const;
  s::size_t word_end(s::size_t pos) const;          // kill-word's end
  s::size_t line_start(s::size_t pos) const;        // of the buffer line
  s::size_t line_end(s::size_t pos) const;
  // zsh's cuttext: saves `text` in the vi register asked for, or "0 / "1,
  // and in the cut buffer (the kill ring takes the old one unless the
  // last widget killed too).
  enum CutFlags : unsigned { CUT_FRONT = 1, CUT_REPLACE = 2, CUT_YANK = 4 };
  void cut_text(strv text, unsigned flags, bool lines = false);
  // zsh's forekill/backkill: cuts `n` bytes after / before the cursor.
  void fore_kill(s::size_t n, unsigned flags);
  void back_kill(s::size_t n, unsigned flags);
  void kill_text(s::size_t from, s::size_t to, bool backward);
  // The buffer yank and put use: the vi register given, else mCutBuf.
  // nullptr when there is none (or it is empty).
  const CutBuffer* put_buffer(int* index = nullptr);
  void set_history_line(int index);   // shows history entry `index`
  int  up_line(int n);
  int  down_line(int n);
  void to_last_col();
  bool line_or(bool up, bool (Editor::*hist)());
  int  infer_next(int from) const;
  bool history_matches(int index, strv prefix) const;
  // The words of history entry `index` (insert-last-word, copy-prev-word).
  vec<str> history_words(int index) const;
  bool incremental_search(bool backward);
  str  read_paste();
  opt<str> minibuffer(strv prompt);   // reads a line below the buffer
  opt<str> read_command_name(strv prompt);
  void list_names(const vec<str>& names);

  // ---- Completion (completion.cpp) ---------------------------------------
  // Completes the word at the cursor: with the builtin completers, or for
  // a completion widget (zle -C) with shell function `fn`.
  bool do_complete(bool list_only, bool menu, bool reverse = false, const str& fn = {});
  // Acts on the matches as $compstate[insert] and [list] say.
  bool apply_completion(CompletionResult& r, bool reverse);
  // `mode` is $compstate[list]: "explanations", "messages", "rows" …
  void list_matches(const CompletionResult& r, strv mode = {});
  // Puts match `index` in the line, as menu completion does or, `final`,
  // as a unique match with its suffix.
  void insert_match(const CompletionResult& r, s::size_t index, bool final);
  // Before widget `widget` runs: drops an auto-removable suffix
  // (compadd -q / -r) or calls its function (-R).
  void remove_suffix(strv widget);
  // The auto-removable suffix `m` leaves in the line, if any.
  opt<AutoSuffix> auto_suffix(const CompCandidate& m, bool final) const;
  bool is_completion_widget(strv name) const;
  // A completion widget acting like builtin widget `builtin`.
  bool complete_widget(strv builtin, const str& fn);
  // -X / -x text for a listing: %% is %, attributes are left out.
  static str format_explanation(strv text);

  // ---- Builtin widgets ----------------------------------------------------
  bool w_accept_line();
  bool w_accept_and_hold();
  bool w_accept_line_and_down_history();
  bool w_send_break();
  bool w_undefined_key();
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
  bool w_gosmacs_transpose_chars();
  bool case_words(int kind);   // 0 up, 1 down, 2 capitalize
  void paste_buffer(const CutBuffer& buf, int n, int position);
  static bool is_yank_widget(strv name);
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
  bool w_argument_base();
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
  bool w_auto_suffix_remove();
  bool w_auto_suffix_retain() { return true; }
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
  bool w_reset_prompt();
  bool w_delete_word();
  bool w_backward_delete_word();
  bool w_beginning_of_line_hist();
  bool w_end_of_line_hist();
  bool w_up_line_or_search();
  bool w_down_line_or_search();
  bool w_vi_yank_eol();
  bool w_vi_undo_change();
  bool w_vi_pound_insert();
  bool w_vi_caps_lock_panic();
  bool w_accept_and_infer_next_history();
  bool w_describe_key_briefly();
  bool w_where_is();
  bool w_end_of_list();
  bool w_set_local_history();
  bool w_split_undo();
  bool w_recursive_edit();
  bool w_read_command();
  bool w_expand_cmd_path();
  bool w_copy_prev_shell_word();
  bool process_cmd();

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
  str                      mKeymapName;       // $KEYMAP: the name selected
  vec<str>                 mWidgetArgs;       // `zle widget args…`
  str                      mPending;          // input to read before the terminal
  str                      mPrebuffer;
  str                      mWidget, mLastWidget, mKeys;
  str                      mWidgetStyle;
  opt<int>                 mArg;
  int                      mTMult        = 1;      // zsh's zmod.tmult
  int                      mArgBase      = 10;     // argument-base
  bool                     mTMultSet     = false;  // MOD_TMULT
  bool                     mArgSet       = false;  // the widget was a prefix
  bool                     mArgNeg       = false;  // neg-argument, no digits yet
  bool                     mDone         = false;
  bool                     mReading      = false;
  bool                     mVared        = false;   // vared is editing
  bool                     mRegionActive = false;
  bool                     mRegionLines  = false;

  // Prompts, expanded once per line (B26).
  str                      mPromptText, mRPromptText;
  strv                     mVaredPrompt, mVaredRPrompt;

  // History navigation: index into mHist.entries(), size() for the line
  // being typed.  Edits to history lines are kept until the line is done.
  int                      mHistIndex    = 0;
  int                      mWakeFd       = -1;   // set_wake's descriptor
  umap<int, str>           mHistEdits;
  str                      mSearchPrefix;     // history-search-*
  int                      mLastWordIndex = 0;  // insert-last-word
  int                      mLastWordHist  = 0;
  s::size_t                mLastWordStart = 0, mLastWordEnd = 0;

  // The last yank, for yank-pop: where it went, which buffer it came from
  // (-1 mCutBuf, else a vi register; -2 none) and its kill-ring place
  // (zsh's kct, -1 the cut buffer itself).
  s::size_t                mYankB = 0, mYankE = 0, mYankCs = 0;
  opt<s::size_t>           mMark;
  int                      mYankFrom     = -2;
  int                      mKct          = -1;

  // Undo: each change is what was replaced at `at` (zsh's struct change), so
  // the stack grows with what was typed, not with the buffer's size.
  struct Change {
    str       del, ins;
    s::size_t at = 0, cursor = 0, cursor_after = 0;
    long      hist = 0;   // the history line it was made in (zsh's ch->hist)
  };
  static Change diff_change(strv before, strv after, s::size_t cursor, s::size_t cursor_after);
  vec<Change>              mUndo, mRedo;
  str                      mUndoBase;           // the text a group started from
  s::size_t                mUndoBaseCursor = 0;
  // Undo grouping: 0 none, 1 a group (vi insert) waiting for its first
  // change, 2 a group whose snapshot is taken.
  int                      mUndoGroup    = 0;
  int                      mLastCol      = -1;  // zsh's lastcol, for line moves
  // zsh's lastcmd flags: the last widget (not counting prefixes like
  // digit-argument) killed (ZLE_KILL) or yanked (ZLE_YANK, after the
  // cursor for vi-put-after).
  bool                     mLastWasKill  = false;
  bool                     mLastWasYank  = false;
  bool                     mLastYankAfter = false;
  s::uint8_t               mWidgetFlags  = 0;  // zle -f: 1 kill, 2 yank, 4 yankbefore
  // Display (and completion's, below).
  bool                     mDrawn        = false;
  bool                     mFinalDraw    = false;   // accept: cursor to the end
  bool                     mMenuActive   = false;
  bool                     mListed       = false;   // the last Tab listed
  int                      mCursorRow    = 0;   // cursor row, from the prompt's first row
  int                      mEndRow       = 0;   // the buffer's last row, likewise
  int                      mRowsDrawn    = 0;
  int                      mCols         = 0;
  str                      mLastIsearch;
  str                      mMessage;            // zle -M, isearch, …
  str                      mListing;            // completion lists

  // Completion state between Tabs.
  // Menu completion: each candidate's text and auto-removable suffix.
  vec<s::pair<str, opt<AutoSuffix>>> mMenu;
  s::size_t                mMenuIndex    = 0;
  s::size_t                mMenuStart    = 0, mMenuEnd = 0;
  str                      mMenuSuffixKind;
  str                      mCompleteLine;           // buffer the last Tab saw
  bool                     mLastCompleting = false; // the last widget completed
  int                      mRunDepth       = 0;     // run_widget calls under way
  s::size_t                mNestedCalls    = 0;     // widgets run with `zle name`
  uptr<CompletionResult>   mLastResult;             // the last matches, for old_list
  opt<AutoSuffix>          mAutoSuffix;
  str                      mVaredName;              // the parameter vared edits

  // vi state.
  s::size_t                mViInsBegin   = 0;   // zsh's viinsbegin
  char                     mViRegister   = 0;
  bool                     mViOperating  = false;   // a motion is an operator's
  char32_t                 mViFindChar   = 0;
  int                      mViFindDir    = 0;
  bool                     mViFindSkip   = false;
  str                      mViSearch;
  int                      mViSearchDir  = -1;
  int                      mStartCursor  = -1;   // read_line's Zle::stack_cs, for run
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
  bool                     mEofIgnored    = false;  // Ctrl-D under IGNORE_EOF
  bool                     mSentBreak     = false;  // send-break ended the read
  bool                     mQuit          = false;  // a trap's exit ended the read
  bool                     mTrashed       = false;  // trash() left the line
  WakeFn                   mWake;
};

}  // namespace jfsh

#endif  // JFSH_EDITOR_EDITOR_H
