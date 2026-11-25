// The emacs widgets in this file (movement, killing, yanking, transposing,
// case, the numeric argument, self-insert, what-cursor-position,
// execute-named-cmd and the others ported in the M9 review) are derived
// from zsh 5.9's Src/Zle/zle_move.c, zle_word.c, zle_misc.c, zle_utils.c
// (cuttext, the kill ring) and zle_hist.c.
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

#include <jfsh/editor/editor.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cwctype>
#include <fcntl.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/core/utf8.h>
#include <jfsh/editor/completion.h>

namespace jfsh {

namespace {

struct termios OrigTermios;

char32_t to_upper(char32_t c) { return static_cast<char32_t>(s::towupper(static_cast<wint_t>(c))); }
char32_t to_lower(char32_t c) { return static_cast<char32_t>(s::towlower(static_cast<wint_t>(c))); }

// Maps each character of text[from, to) through `f`.
void map_chars(str& text, s::size_t from, s::size_t to, char32_t (*f)(char32_t)) {
  str out;
  for (s::size_t i = from; i < to;) {
    s::size_t len;
    char32_t c = utf8_decode(text, i, len);
    utf8_append(out, c < 0x80 or len > 1 ? f(c) : c);
    i += len;
  }
  text.replace(from, to - from, out);
}

}  // namespace

// ---- Setup and the terminal -------------------------------------------------

Editor::Editor(const OptionState& opts, HistoryManager& hist, PromptRenderer& prompt, Zle& zle)
  : mOpts(opts), mHist(hist), mPrompt(prompt), mZle(zle) {
  if (mZle.tty_out >= 0) mOut = mZle.tty_out;
  if (mOpts.get(OPT_VI)) mZle.select_main(true);
}

Editor::~Editor() {
  if (mRawModeActive) exit_raw_mode();
  if (mZle.active == this) mZle.active = nullptr;
}

EditorMode Editor::mode() const {
  return mZle.keymap("main") == mZle.keymap("viins") ? EditorMode::Vi : EditorMode::Emacs;
}

namespace {

// With FLOW_CONTROL (zsh's zsetterm) ^S and ^Q stay the terminal's.
struct termios raw_modes(const struct termios& orig, bool flow_control) {
  struct termios raw = orig;
  raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | ISIG | IEXTEN));
  raw.c_iflag &= static_cast<tcflag_t>(~(ICRNL | BRKINT | INPCK | ISTRIP));
  if (not flow_control) raw.c_iflag &= static_cast<tcflag_t>(~IXON);
  raw.c_cflag |= CS8;
  raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
  raw.c_cc[VMIN]  = 1;
  raw.c_cc[VTIME] = 0;
  return raw;
}

}  // namespace

void Editor::enter_raw_mode() {
  tcgetattr(mIn, &OrigTermios);
  struct termios raw = raw_modes(OrigTermios, mOpts.get(OPT_FLOWCONTROL));
  // TCSADRAIN, not TCSAFLUSH: input typed or pasted ahead (the next lines
  // of a multi-line command) must survive the switch.
  tcsetattr(mIn, TCSADRAIN, &raw);
  mRawModeActive = true;
}

void Editor::resume_raw_mode() {
  if (not mRawModeActive) return;
  struct termios raw = raw_modes(OrigTermios, mOpts.get(OPT_FLOWCONTROL));
  tcsetattr(mIn, TCSANOW, &raw);
}

// TCSANOW: on macOS TCSADRAIN on a pty waits until the other side has read
// the echoed line, and a Ctrl-C typed meanwhile arrives with ISIG still off
// and sits in the queue as a plain byte instead of interrupting the command.
void Editor::exit_raw_mode() {
  tcsetattr(mIn, TCSANOW, &OrigTermios);
  mRawModeActive = false;
}

void Editor::write_out(strv data) {
  while (not data.empty()) {
    ssize_t n = ::write(mOut, data.data(), data.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    data.remove_prefix(static_cast<s::size_t>(n));
  }
}

void Editor::beep() {
  if (mOpts.get(OPT_BEEP)) write_out("\a");
}

// ---- Reading a line -----------------------------------------------------------

opt<str> Editor::read_line(bool continuation) {
  mContinuation = continuation;
  mVared        = false;
  str initial;
  int cursor = -1;
  if (not continuation) {
    bool stacked = mZle.next_buffer or not mZle.buffer_stack.empty();
    if (mZle.next_buffer) {
      initial = s::move(*mZle.next_buffer);
      mZle.next_buffer.reset();
    } else if (not mZle.buffer_stack.empty()) {
      initial = s::move(mZle.buffer_stack.back());
      mZle.buffer_stack.pop_back();
    }
    if (stacked) cursor = s::exchange(mZle.stack_cs, -1);
  }
  mStartCursor = cursor;
  return run(s::move(initial));
}

opt<str> Editor::edit(str initial, strv prompt, strv rprompt) {
  mContinuation = false;
  mVared        = true;
  mVaredPrompt  = prompt;
  mVaredRPrompt = rprompt;
  auto r = run(s::move(initial));
  mVared = false;
  return r;
}

opt<str> Editor::run(str initial) {
  mBuf = LineBuffer{};
  mBuf.text   = s::move(initial);
  mBuf.cursor = mBuf.text.size();
  if (int at = s::exchange(mStartCursor, -1); at >= 0)
    mBuf.cursor = s::min(static_cast<s::size_t>(at), mBuf.text.size());
  mHistIndex  = static_cast<int>(mHist.entries().size());
  mHistEdits.clear();
  reset_modifiers();
  mUndo.clear();
  mRedo.clear();
  // Text the read starts with (vared's, push-line's) comes off zsh's
  // bufstack through setline and handleundo: undoing can take it away.
  if (not mBuf.text.empty())
    mUndo.push_back({{}, mBuf.text, 0, 0, mBuf.cursor, mHistIndex});
  mUndoGroup    = 0;
  mLastCol      = -1;
  mMark.reset();
  mRegionActive = false;
  mMessage.clear();
  mListing.clear();
  mMenuActive   = false;
  mListed       = false;
  mLastResult.reset();
  mAutoSuffix.reset();
  mLastWasKill  = mLastWasYank = false;
  mYankFrom     = -2;
  mInterrupted  = false;
  mEofIgnored   = false;
  mSentBreak    = false;
  mEof          = false;
  mDone         = false;
  mDrawn        = false;
  mCursorRow    = 0;
  // ($LASTWIDGET goes on from the last read: zsh's lbindk.)
  mLastCompleting = false;
  mLocalKeymap.reset();
  mViRegister   = 0;
  mViRecord     = false;
  if (not select_keymap("main")) select_keymap(".safe");
  if (mZle.next_hist_index >= 0 and not mVared and not mContinuation) {
    int idx = mZle.next_hist_index;
    mZle.next_hist_index = -1;
    if (idx < static_cast<int>(mHist.entries().size())) set_history_line(idx);
  }
  // zsh's zleread: with main linked to viins, the read starts in a vi
  // insert (viinsert_init), so what is typed before Esc is one undo step.
  if (mZle.keymap("main") == mZle.keymap("viins")) vi_enter_insert();

  if (mVared) {
    mPromptText  = mPrompt.expand(mVaredPrompt);
    mRPromptText = mPrompt.expand(mVaredRPrompt);
  } else {
    mPromptText  = mContinuation ? mPrompt.continuation() : mPrompt.primary();
    mRPromptText = mContinuation ? mPrompt.right_continuation() : mPrompt.right();
  }

  if (not ::isatty(mIn)) {
    // B54: without a terminal the prompt goes to stderr, as zsh's does
    // when it has no tty, and the line is read unbuffered, so a command
    // reading the rest of the input gets all of it.
    str p = PromptRenderer::strip_markers(mPromptText);
    if (::write(STDERR_FILENO, p.data(), p.size()) < 0) {}
    str line;
    bool any = false;
    for (;;) {
      char c;
      ssize_t n = ::read(mIn, &c, 1);
      if (n < 0 and errno == EINTR) continue;
      if (n <= 0) break;
      any = true;
      if (c == '\n') break;
      line += c;
    }
    if (not any) return s::nullopt;
    // Without the line editor nothing uses the buffer stack or a line
    // HIST_VERIFY put back: they are dropped, as zsh never sees them.
    return line;
  }

  Editor* outer = mZle.active;
  mZle.active = this;
  mReading = true;
  // $TMOUT: SIGALRM if no line comes in time, for every read the editor
  // does on the terminal (zsh's zleread: alarm(tmout), alarm(0) at the end).
  struct Alarm {
    explicit Alarm(long t) { if (t > 0) ::alarm(static_cast<unsigned>(t)); }
    ~Alarm() { ::alarm(0); }
    Alarm(const Alarm&) = delete;
    Alarm& operator=(const Alarm&) = delete;
  } alarm_guard{s::strtol(mZle.param("TMOUT").c_str(), nullptr, 10)};
  enter_raw_mode();
  str term = mZle.param("TERM");
  bool paste = term != "dumb" and not term.empty();
  if (paste) write_out("\x1b[?2004h");

  // PROMPT_SP / PROMPT_CR: when the last output didn't end in a newline,
  // mark the spot (PROMPT_EOL_MARK) and start the prompt on a new line.
  if (not mContinuation and not mVared and mOpts.get(OPT_PROMPTCR)) {
    if (mOpts.get(OPT_PROMPTSP)) {
      auto p = mZle.params ? mZle.params->lookup("PROMPT_EOL_MARK") : s::nullopt;
      str mark = mPrompt.expand(p and p->get().is_set() ? p->get().scalar() : str("%B%S%#%s%b"));
      int w = PromptRenderer::visual_width(mark);
      int cols = terminal_width();
      write_out(PromptRenderer::strip_markers(mark)
                + str(static_cast<s::size_t>(s::max(0, cols - w - 1)), ' ')
                + "\r" + str(static_cast<s::size_t>(w), ' ') + "\r");
    } else {
      write_out("\r");
    }
  }
  refresh();

  bool eof_key = false;
  while (not mDone) {
    // vi: the keys of each command in vicmd are kept, so `.` can repeat
    // the last one that changed the buffer (through any insert it began).
    bool vicmd = in_vicmd() and not mLocalKeymap;
    if (vicmd and not mViRecord and not mViReplaying) {
      mViRecording.clear();
      mViRecord = true;
    }
    str text_before = mBuf.text;
    const KeyBinding* b = nullptr;
    str seq = read_binding(b);
    if (mEof) break;
    if (seq.empty()) continue;
    if (seq == "\x03") {                // Ctrl-C (VINTR): drop the line
      if (mZle.on_interrupt and not mZle.on_interrupt(*this)) continue;
      mInterrupted = true;
      mDone = true;
      break;
    }
    // Ctrl-D (VEOF) on an empty line is EOF, whatever it is bound to.
    if (seq == "\x04" and mBuf.text.empty() and not mVared) {
      // IGNORE_EOF: the read ends all the same, with a message; the shell
      // counts the ignored EOFs (zsh's zleread and use_exit_printed).
      mEofIgnored = mOpts.get(OPT_IGNOREEOF);
      eof_key = true;
      mDone = true;
      break;
    }
    if (not mMessage.empty() and mWidget != "execute-named-cmd") mMessage.clear();
    mKeys = seq;
    if (b and b->is_macro) {
      push_input(b->macro);
      continue;
    }
    if (b) {
      // $WIDGET is the widget a key ran (zsh's bindk), until the next key:
      // a trap sees it too.
      mWidget = b->widget;
      run_widget(b->widget);
    } else {
      // A key bound to nothing runs undefined-key (getkeycmd).
      mWidget = "undefined-key";
      run_widget(mWidget);
    }
    if (mViRecord and not mViReplaying and in_vicmd()) {
      // A count or register still waiting for its command.
      bool prefix_only = mArg.has_value() or mViRegister != 0;
      if (not prefix_only or mBuf.text != text_before) {
        // (The widget the key ran: after `.`, $LASTWIDGET is the repeated
        // one.)
        str ran = b ? mZle.resolve_widget(b->widget) : str{};
        if (ran.starts_with('.')) ran.erase(0, 1);
        if (mBuf.text != text_before and ran != "undo" and ran != "redo"
            and ran != "vi-repeat-change")
          mViLastChange = mViRecording;
        mViRecord = false;
      }
    } else if (mViRecord and not in_vicmd() and mKeymap != mZle.keymap("viins")) {
      mViRecord = false;
    }
    // zsh's zlecore: the line is drawn unless keys pushed back (a macro's,
    // kungetct) are still to be read.  (Waiting for typed-ahead keys is
    // only with $BAUD set: not done.)
    if (not mDone and mPending.empty()) refresh();
  }

  if (paste) write_out("\x1b[?2004l");
  if (mQuit) {
    // An `exit` in a trap run while the line was read: the screen stays as
    // the trap left it, the cursor going to a line of its own (zsh exits
    // from the signal handler without redrawing).
    mQuit = false;
    if (mDrawn or not mTrashed) write_out(mRawModeActive ? "\r\n" : "\n");
    mDrawn = false;
    mCursorRow = 0;
    mListing.clear();
    exit_raw_mode();
    mReading = false;
    mZle.active = outer;
    return mDone ? opt<str>(str{}) : s::nullopt;
  }
  if (mEof and not mDone) {
    finish_display();
    exit_raw_mode();
    mReading = false;
    mZle.active = outer;
    return s::nullopt;
  }
  finish_display();
  exit_raw_mode();
  mReading = false;
  mZle.active = outer;
  if (mEofIgnored) {
    s::fprintf(stderr, "\a%s\n", mOpts.login() ? "jfsh: use 'logout' to logout."
                                                 : "jfsh: use 'exit' to exit.");
    s::fflush(stderr);
  }
  if (eof_key) return s::nullopt;
  if (mInterrupted) return str{};
  return mBuf.text;
}

int Editor::read_byte(int timeout_ms) {
  if (not mPending.empty()) {
    unsigned char c = static_cast<unsigned char>(mPending[0]);
    mPending.erase(0, 1);
    if (mViRecord) mViRecording += static_cast<char>(c);
    return c;
  }
  for (;;) {
    struct pollfd fds[2] = {{mIn, POLLIN, 0}, {mWakeFd, POLLIN, 0}};
    nfds_t nfds = (mWakeFd >= 0 and mWake) ? 2 : 1;
    int r = ::poll(fds, nfds, timeout_ms);
    if (r == 0) return -1;
    if (r < 0) {
      if (errno == EINTR) continue;
      mEof = true;
      return -1;
    }
    if (nfds == 2 and (fds[1].revents & POLLIN)) {
      // A signal arrived: let the shell act on it with the terminal in
      // its normal mode, then put the prompt and line back.
      exit_raw_mode();
      bool quit    = false;
      bool printed = mWake(quit);
      if (quit) {
        mEof = mQuit = true;
        return -1;
      }
      enter_raw_mode();
      if (printed or not mDrawn or terminal_width() != mCols) refresh();
      continue;
    }
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) {
      unsigned char c;
      ssize_t n = ::read(mIn, &c, 1);
      if (n < 0 and errno == EINTR) continue;
      if (n <= 0) {
        // EOF or a hung-up terminal: without this the caller would spin.
        mEof = true;
        return -1;
      }
      if (mViRecord) mViRecording += static_cast<char>(c);
      return c;
    }
  }
}

int Editor::keytimeout() const {
  str kt = mZle.param("KEYTIMEOUT");
  return kt.empty() ? 400 : s::max(0, s::atoi(kt.c_str())) * 10;
}

str Editor::read_binding(const KeyBinding*& binding) {
  binding = nullptr;
  auto lookup = [&](strv s) -> const KeyBinding* {
    if (mLocalKeymap)
      if (auto* b = mLocalKeymap->lookup(s)) return b;
    return mKeymap->lookup(s);
  };
  auto prefix = [&](strv s) {
    return (mLocalKeymap and mLocalKeymap->is_prefix(s)) or mKeymap->is_prefix(s);
  };
  int timeout_ms = keytimeout();

  str seq;
  const KeyBinding* match = nullptr;
  s::size_t match_len = 0;
  for (;;) {
    // Wait as long as it takes for the first key and for the rest of a
    // sequence nothing matches yet; KEYTIMEOUT when a shorter one does.
    int timeout = seq.empty() or not match ? -1 : timeout_ms;
    int c = read_byte(timeout);
    if (c < 0) break;
    seq += static_cast<char>(c);
    if (auto* b = lookup(seq)) {
      match = b;
      match_len = seq.size();
    }
    if (not prefix(seq)) break;
  }
  if (match) {
    if (seq.size() > match_len) push_input(strv(seq).substr(match_len));
    seq.resize(match_len);
    binding = match;
    return seq;
  }
  if (seq.empty()) return seq;
  // An unbound escape sequence: read the rest of it so it doesn't come
  // out as text.
  if (seq.size() >= 2 and seq[0] == '\x1b' and (seq[1] == '[' or seq[1] == 'O')) {
    while (seq.size() < 32) {
      unsigned char last = static_cast<unsigned char>(seq.back());
      if (seq.size() > 2 and last >= 0x40 and last <= 0x7e) break;
      int c = read_byte(50);
      if (c < 0) break;
      seq += static_cast<char>(c);
    }
    return seq;
  }
  // The rest of a UTF-8 character.
  unsigned char lead = static_cast<unsigned char>(seq[0]);
  s::size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
  while (seq.size() < need) {
    int c = read_byte(50);
    if (c < 0) break;
    if ((c & 0xc0) != 0x80) {
      push_input(str(1, static_cast<char>(c)));
      break;
    }
    seq += static_cast<char>(c);
  }
  return seq;
}

bool Editor::run_widget(const str& name) {
  str w = mZle.resolve_widget(name);
  if (w.empty()) {
    // zsh's execzlefunc: the message, then the builtin of that name if
    // there is one (execimmortal).
    mMessage = "No such widget `" + name + "'";
    if (not name.starts_with('.') and Zle::is_builtin_widget(name)) return run_widget("." + name);
    beep();
    reset_modifiers();
    return false;
  }
  remove_suffix(w);
  // (Removing an auto-suffix deletes before the widget runs.)
  if (s::exchange(mBuf.spaced, false)) mRegionActive = false;
  ++mRunDepth;
  struct { str text; s::size_t cursor; } before{mBuf.text, mBuf.cursor};
  int hist_before = mHistIndex;
  s::size_t nested_before = mNestedCalls;
  mArgSet   = false;
  // zsh's ZLE_LASTCOL widgets keep the column a run of line moves keeps to.
  static const uset<str> lastcol_widgets = {
    "up-line", "down-line", "up-line-or-history", "down-line-or-history",
    "up-line-or-search", "down-line-or-search"};
  if (not lastcol_widgets.contains(w.starts_with('.') ? w.substr(1) : w)) mLastCol = -1;
  s::uint8_t outer_flags = s::exchange(mWidgetFlags, 0);
  bool ok = dispatch(w);
  s::uint8_t flags = s::exchange(mWidgetFlags, outer_flags);
  --mRunDepth;
  // zlecore's handlefeep: a failure rings for the key's own widget, not
  // for one run inside it (`zle w`, execute-named-cmd's).
  if (not ok and not mDone and mRunDepth == 0) beep();
  str base = w.starts_with('.') ? w.substr(1) : w;
  // zsh's spaceinline and shiftchars: a widget that inserted or deleted
  // text ended an active region (a user widget's assignments to BUFFER
  // don't; the widgets it calls with `zle` come through here first).
  if (s::exchange(mBuf.spaced, false)) mRegionActive = false;
  // Each widget that changes the line is one step for undo (zsh's
  // handleundo after each command); a vi insert is one in all.
  // A move to another history line is none: zsh's zle_setline takes the
  // new line as it is (setlastline), and each change keeps its line.
  if (base != "undo" and base != "redo" and before.text != mBuf.text
      and mHistIndex == hist_before) {
    if (mUndoGroup == 2 and not mUndo.empty()) {
      mUndo.back() = diff_change(mUndoBase, mBuf.text, mUndoBaseCursor, mBuf.cursor);
      mUndo.back().hist = mHistIndex;
    } else {
      mUndo.push_back(diff_change(before.text, mBuf.text, before.cursor, mBuf.cursor));
      mUndo.back().hist = mHistIndex;
      if (mUndoGroup == 1) {
        mUndoBase       = s::move(before.text);
        mUndoBaseCursor = before.cursor;
        mUndoGroup      = 2;
      }
    }
    mRedo.clear();
  }
  // zsh's handleprefixes: a prefix's count takes effect; anything else
  // starts the modifiers afresh.
  if (mArgSet) {
    if (mTMultSet) mArg = mTMult;
  } else {
    reset_modifiers();
  }
  if (in_vicmd() and not mViOperating) vi_clamp_cursor();
  // zsh's lastcmd: the flags of the last widget that wasn't a prefix.
  static const uset<str> not_commands = {
    "argument-base", "auto-suffix-remove", "auto-suffix-retain", "clear-screen",
    "digit-argument", "neg-argument", "read-command", "reset-prompt", "split-undo",
    "universal-argument", "yank-pop"};
  if (mZle.user_widgets.contains(w) and not mZle.comp_widgets.contains(w)) {
    // A user widget's flags are what `zle -f` gave it, whatever it ran.
    mLastWasKill   = flags & 1;
    mLastWasYank   = flags & 6;
    mLastYankAfter = flags & 2;
  } else if (not not_commands.contains(base)) {
    static const uset<str> kills = {
      "backward-kill-line", "backward-kill-word", "kill-buffer", "kill-line", "kill-region",
      "kill-whole-line", "kill-word"};
    mLastWasKill = kills.contains(base);
    mLastWasYank = is_yank_widget(base);
    mLastYankAfter = base == "vi-put-after";
  }
  // A user widget that leaves the line alone doesn't end the menu, nor
  // the run of completions automenu counts; one that ran widgets with
  // `zle name` leaves them as those did.
  bool passive = mZle.user_widgets.contains(w) and not mZle.comp_widgets.contains(w)
             and (mNestedCalls != nested_before
                  or (before.text == mBuf.text and before.cursor == mBuf.cursor));
  if (not passive) mLastCompleting = is_completion_widget(base);
  if (not mLastCompleting) {
    mMenuActive = false;
    mListed     = false;
  }
  // zsh's lbindk; after `.` the widgets it repeated ran last (from keys
  // pushed back), after execute-named-cmd the widget it ran.
  if (base != "vi-repeat-change" and base != "execute-named-cmd"
      and base != "execute-last-named-cmd")
    mLastWidget = name;
  return ok;
}

bool Editor::dispatch(const str& name) {
  vec<str> args = s::exchange(mWidgetArgs, {});
  {
    if (auto u = mZle.user_widgets.find(name); u != mZle.user_widgets.end()) {
      if (auto c = mZle.comp_widgets.find(name); c != mZle.comp_widgets.end())
        return complete_widget(c->second, u->second);
      if (not mZle.hooks.run_widget) return false;
      int rc = mZle.hooks.run_widget(*this, name, u->second, args);
      // No such function: the builtin of the widget's name, if any
      // (execimmortal).
      if (rc == -1) return Zle::is_builtin_widget(name) and dispatch("." + name);
      mBuf.cursor = s::min(mBuf.cursor, mBuf.text.size());
      while (mBuf.cursor > 0 and mBuf.cursor < mBuf.text.size()
             and (static_cast<unsigned char>(mBuf.text[mBuf.cursor]) & 0xc0) == 0x80)
        --mBuf.cursor;
      return rc == 0;
    }
  }
  strv base = name.starts_with('.') ? strv(name).substr(1) : strv(name);
  if (auto m = mZle.module_widgets.find(str(base)); m != mZle.module_widgets.end())
    return m->second.fn(*this);
  const auto& table = Editor::widget_table();
  auto it = table.find(str(base));
  if (it == table.end()) return false;
  return (this->*(it->second.fn))();
}

// zsh's bin_zle_call: $WIDGET is the widget called only for a builtin one
// or with -w (setbindk), back to the caller's after; with -f nolast
// $LASTWIDGET stays as it was.
bool Editor::call_widget(strv name, vec<str> args, bool set_bindk, bool nolast) {
  if (not mReading) return false;
  ++mNestedCalls;
  mWidgetArgs = s::move(args);
  str w = mZle.resolve_widget(name);
  bool builtin = not w.empty() and not mZle.user_widgets.contains(w);
  str saved = mWidget, saved_last = mLastWidget;
  if (set_bindk or builtin) mWidget = str(name);
  bool ok = run_widget(str(name));
  mWidget = s::move(saved);
  if (nolast) mLastWidget = s::move(saved_last);
  return ok;
}

void Editor::set_widget_flag(strv flag) {
  if (flag == "kill")            mWidgetFlags |= 1;
  else if (flag == "yank")       mWidgetFlags |= 2;
  else if (flag == "yankbefore") mWidgetFlags |= 4;
}

void Editor::reset_prompt() {
  mPrompt.reload();
  if (mVared) {
    mPromptText  = mPrompt.expand(mVaredPrompt);
    mRPromptText = mPrompt.expand(mVaredRPrompt);
  } else {
    mPromptText  = mContinuation ? mPrompt.continuation() : mPrompt.primary();
    mRPromptText = mContinuation ? mPrompt.right_continuation() : mPrompt.right();
  }
  if (mReading) refresh();
}

// zsh's selectkeymap: $KEYMAP is the name selected (`main` at the
// prompt), not the keymap's own name.
bool Editor::select_keymap(strv name) {
  auto km = mZle.keymap(name);
  if (not km) return false;
  mKeymap = km;
  mKeymapName = str(name);
  mLocalKeymap.reset();
  return true;
}

str Editor::keymap_name() const {
  return mKeymapName;
}

int Editor::hist_number() const {
  const auto& e = mHist.entries();
  if (mHistIndex < static_cast<int>(e.size())) return static_cast<int>(e[static_cast<s::size_t>(mHistIndex)].num);
  return static_cast<int>(mHist.last_num()) + 1;
}

void Editor::set_cut_buffer(str s) {
  mZle.cut_buf = {s::move(s), true, false};
}

void Editor::set_mark(long m) {
  mMark = static_cast<s::size_t>(s::clamp(m, 0L, static_cast<long>(mBuf.text.size())));
}

// ---- Helpers ----------------------------------------------------------------

bool Editor::is_word_char(char32_t c) const {
  if (c >= 0x80) return s::iswalnum(static_cast<wint_t>(c)) or c > 0xff;
  if (s::isalnum(static_cast<int>(c))) return true;
  auto p = mZle.params ? mZle.params->lookup("WORDCHARS") : s::nullopt;
  str wc = p and p->get().is_set() ? p->get().scalar() : str("*?_-.[]~=/&;!#$%^(){}<>");
  return wc.find(static_cast<char>(c)) != str::npos;
}

namespace {
char32_t char_at(strv s, s::size_t pos) {
  s::size_t len;
  return pos < s.size() ? utf8_decode(s, pos, len) : 0;
}
}  // namespace

s::size_t Editor::next_word_start(s::size_t pos) const {
  strv t = mBuf.text;
  while (pos < t.size() and is_word_char(char_at(t, pos))) pos = next_char(t, pos);
  while (pos < t.size() and not is_word_char(char_at(t, pos))) pos = next_char(t, pos);
  return pos;
}

s::size_t Editor::word_end(s::size_t pos) const {
  strv t = mBuf.text;
  while (pos < t.size() and not is_word_char(char_at(t, pos))) pos = next_char(t, pos);
  while (pos < t.size() and is_word_char(char_at(t, pos))) pos = next_char(t, pos);
  return pos;
}

s::size_t Editor::prev_word_start(s::size_t pos) const {
  strv t = mBuf.text;
  while (pos > 0 and not is_word_char(char_at(t, prev_char(t, pos)))) pos = prev_char(t, pos);
  while (pos > 0 and is_word_char(char_at(t, prev_char(t, pos)))) pos = prev_char(t, pos);
  return pos;
}

s::size_t Editor::prev_word_start_n(s::size_t pos, int n) const {
  while (n-- > 0) pos = prev_word_start(pos);
  return pos;
}

s::size_t Editor::line_start(s::size_t pos) const {
  auto nl = pos == 0 ? str::npos : mBuf.text.rfind('\n', pos - 1);
  return nl == str::npos ? 0 : nl + 1;
}

s::size_t Editor::line_end(s::size_t pos) const {
  auto nl = mBuf.text.find('\n', pos);
  return nl == str::npos ? mBuf.text.size() : nl;
}

void Editor::cut_text(strv text, unsigned flags, bool lines) {
  int reg = mViRegister;
  mViRegister = 0;
  if ((text.empty() and not lines) or reg == '_') return;
  if (reg >= 'a' and reg <= 'z') {
    mZle.vibuf[static_cast<s::size_t>(reg - 'a')] = {str(text), true, lines};
  } else if (reg >= 'A' and reg <= 'Z') {
    CutBuffer& b = mZle.vibuf[static_cast<s::size_t>(reg - 'A')];
    if (not b.set) {
      b = {str(text), true, lines};
    } else {
      if (lines) b.lines = true;
      if (b.lines) b.text += '\n';
      b.text += text;
    }
  } else if (reg >= '0' and reg <= '9') {
    mZle.vibuf[static_cast<s::size_t>(26 + reg - '0')] = {str(text), true, lines};
  } else if (flags & CUT_YANK) {
    mZle.vibuf[26] = {str(text), true, lines};
  } else {
    for (s::size_t n = 35; n > 27; --n) mZle.vibuf[n] = s::move(mZle.vibuf[n - 1]);
    mZle.vibuf[27] = {str(text), true, lines};
  }
  if (not mZle.cut_buf.set) {
    mZle.cut_buf = {{}, true, false};
  } else if (not mLastWasKill or (flags & CUT_REPLACE)) {
    // The cut buffer goes to the front of the ring, its oldest entry out
    // (zsh's kringnum steps on, over it).
    auto& ring = mZle.kring;
    if (ring.empty()) ring.resize(8);
    else              s::rotate(ring.rbegin(), ring.rbegin() + 1, ring.rend());
    ring.front() = s::move(mZle.cut_buf);
    mZle.cut_buf = {{}, true, false};
  }
  if (flags & (CUT_FRONT | CUT_REPLACE)) mZle.cut_buf.text.insert(0, text);
  else                                   mZle.cut_buf.text += text;
  mZle.cut_buf.lines = lines;
}

void Editor::fore_kill(s::size_t n, unsigned flags) {
  n = s::min(n, mBuf.text.size() - mBuf.cursor);
  cut_text(strv(mBuf.text).substr(mBuf.cursor, n), flags);
  mBuf.erase(mBuf.cursor, n);
}

void Editor::back_kill(s::size_t n, unsigned flags) {
  n = s::min(n, mBuf.cursor);
  mBuf.cursor -= n;
  cut_text(strv(mBuf.text).substr(mBuf.cursor, n), flags);
  mBuf.erase(mBuf.cursor, n);
}

void Editor::kill_text(s::size_t from, s::size_t to, bool backward) {
  if (from > to) s::swap(from, to);
  to = s::min(to, mBuf.text.size());
  mBuf.cursor = from;
  fore_kill(to - from, backward ? CUT_FRONT : 0);
}

const CutBuffer* Editor::put_buffer(int* index) {
  int reg = mViRegister;
  mViRegister = 0;
  int i = -1;
  if (reg >= 'a' and reg <= 'z')      i = reg - 'a';
  else if (reg >= 'A' and reg <= 'Z') i = reg - 'A';
  else if (reg >= '0' and reg <= '9') i = 26 + reg - '0';
  if (index) *index = i;
  const CutBuffer& b = i < 0 ? mZle.cut_buf : mZle.vibuf[static_cast<s::size_t>(i)];
  return b.set ? &b : nullptr;
}

// ---- Accepting and inserting -------------------------------------------------

bool Editor::w_accept_line() {
  mDone = true;
  return true;
}

bool Editor::w_accept_and_hold() {
  mZle.next_buffer = mBuf.text;
  mZle.stack_cs    = static_cast<int>(mBuf.cursor);
  mDone = true;
  return true;
}

bool Editor::w_accept_line_and_down_history() {
  int size = static_cast<int>(mHist.entries().size());
  if (mHistIndex < size) mZle.next_hist_index = mHistIndex + 1;
  mDone = true;
  return true;
}

// zsh's undefinedkey only fails; the failure rings the bell (handlefeep).
bool Editor::w_undefined_key() { return false; }

bool Editor::w_send_break() {
  beep();   // as zsh's send-break, which rings the bell
  mSentBreak   = true;
  mInterrupted = true;
  mDone = true;
  return true;
}

// zsh's doinsert: `s` count times, after the cursor for a negative count;
// in overwrite mode over as many characters, but not over a newline.
void Editor::do_insert(strv s) {
  int m = count();
  bool neg = m < 0;
  if (neg) m = -m;
  str ins;
  for (int k = 0; k < m; ++k) ins += s;
  s::size_t at = mBuf.cursor;
  if (mBuf.overwrite and at < mBuf.text.size() and mBuf.text[at] != '\n') {
    s::size_t chars = char_count(ins), pos = at;
    while (pos < mBuf.text.size() and mBuf.text[pos] != '\n' and chars-- > 0)
      pos = next_char(mBuf.text, pos);
    // Over as many bytes as it puts there, a write in place; else zsh's
    // spaceinline or shiftchars make the room.
    if (pos - at == ins.size()) mBuf.text.replace(at, pos - at, ins);
    else                        mBuf.replace(at, pos - at, ins);
  } else {
    mBuf.insert_at(at, ins);
  }
  if (not neg) mBuf.cursor = at + ins.size();
}

// The rest of a UTF-8 character whose first byte ends `keys` (zsh's
// getrestchar); false if it never comes.
bool Editor::read_rest_char(str& keys) {
  if (keys.empty()) return false;
  s::size_t lead_at = keys.size() - 1;
  while (lead_at > 0 and (static_cast<unsigned char>(keys[lead_at]) & 0xc0) == 0x80) --lead_at;
  unsigned char lead = static_cast<unsigned char>(keys[lead_at]);
  s::size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
  while (keys.size() - lead_at < need) {
    int c = read_byte(keytimeout());
    if (c < 0) return false;
    if ((c & 0xc0) != 0x80) {
      push_input(str(1, static_cast<char>(c)));
      return false;
    }
    keys += static_cast<char>(c);
  }
  return true;
}

// The last character of the keys that ran the widget (zsh's
// LASTFULLCHAR).
str Editor::last_full_char() {
  str keys = mKeys;
  if (not read_rest_char(keys)) return {};
  mKeys = keys;
  s::size_t start = keys.size() - 1;
  while (start > 0 and (static_cast<unsigned char>(keys[start]) & 0xc0) == 0x80) --start;
  return keys.substr(start);
}

bool Editor::w_self_insert() {
  str ch = last_full_char();
  if (ch.empty()) return false;
  do_insert(ch);
  return true;
}

bool Editor::w_self_insert_unmeta() {
  str keys = mKeys;
  if (keys.empty()) return false;
  // zsh's fixunmeta: the last byte without its meta bit, \r as \n.
  char c = static_cast<char>(static_cast<unsigned char>(keys.back()) & 0x7f);
  if (c == '\r') c = '\n';
  mKeys = str(1, c);
  return w_self_insert();
}

bool Editor::w_quoted_insert() {
  int c = read_byte(-1);
  if (c < 0) return false;
  mKeys = str(1, static_cast<char>(c));
  return w_self_insert();
}

bool Editor::w_bracketed_paste() {
  mBuf.insert(read_paste());
  return true;
}

// The text up to the end of a bracketed paste, its carriage returns as
// newlines.
str Editor::read_paste() {
  str text;
  for (;;) {
    int c = read_byte(1000);
    if (c < 0) break;
    text += static_cast<char>(c);
    if (text.ends_with("\x1b[201~")) {
      text.resize(text.size() - 6);
      break;
    }
  }
  str clean;
  for (s::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '\r') {
      clean += '\n';
      if (i + 1 < text.size() and text[i + 1] == '\n') ++i;
    } else {
      clean += text[i];
    }
  }
  return clean;
}

// ---- Movement ----------------------------------------------------------------
//
// Ports of zsh's zle_move.c and zle_word.c: a negative count runs the
// widget of the other direction.

namespace {
// Runs `fn` with the count negated (zsh's `zmult = -n; ret = other(args)`).
template <typename F> bool negated(opt<int>& arg, F fn) {
  opt<int> saved = arg;
  arg = -*arg;
  bool ok = fn();
  arg = saved;
  return ok;
}
}  // namespace

bool Editor::w_forward_char() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_backward_char(); });
  while (mBuf.cursor < mBuf.text.size() and n-- > 0)
    mBuf.cursor = next_char(mBuf.text, mBuf.cursor);
  return true;
}

bool Editor::w_backward_char() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_forward_char(); });
  while (mBuf.cursor > 0 and n-- > 0) mBuf.cursor = prev_char(mBuf.text, mBuf.cursor);
  return true;
}

bool Editor::w_forward_word() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_backward_word(); });
  strv t = mBuf.text;
  s::size_t& cs = mBuf.cursor;
  while (n-- > 0) {
    while (cs < t.size() and is_word_char(char_at(t, cs))) cs = next_char(t, cs);
    while (cs < t.size() and not is_word_char(char_at(t, cs))) cs = next_char(t, cs);
  }
  return true;
}

bool Editor::w_backward_word() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_forward_word(); });
  mBuf.cursor = prev_word_start_n(mBuf.cursor, n);
  return true;
}

bool Editor::w_emacs_forward_word() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_emacs_backward_word(); });
  s::size_t& cs = mBuf.cursor;
  while (n-- > 0) cs = word_end(cs);
  return true;
}

bool Editor::w_emacs_backward_word() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_emacs_forward_word(); });
  mBuf.cursor = prev_word_start_n(mBuf.cursor, n);
  return true;
}

bool Editor::w_beginning_of_line() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_end_of_line(); });
  strv t = mBuf.text;
  s::size_t& cs = mBuf.cursor;
  while (n-- > 0) {
    if (cs == 0) return true;
    if (t[cs - 1] == '\n') {
      --cs;
      if (cs == 0) return true;
    }
    while (cs > 0 and t[cs - 1] != '\n') --cs;
  }
  return true;
}

bool Editor::w_end_of_line() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_beginning_of_line(); });
  strv t = mBuf.text;
  s::size_t& cs = mBuf.cursor;
  while (n-- > 0) {
    if (cs >= t.size()) {
      cs = t.size();
      return true;
    }
    if (in_vicmd() and (cs = next_char(t, cs)) == t.size()) break;
    if (t[cs] == '\n' and ++cs == t.size()) return true;
    while (cs != t.size() and t[cs] != '\n') ++cs;
  }
  return true;
}

bool Editor::w_beginning_of_buffer_or_history() {
  if (mBuf.cursor > 0) {
    mBuf.cursor = 0;
    return true;
  }
  return w_beginning_of_history();
}

bool Editor::w_end_of_buffer_or_history() {
  if (mBuf.cursor < mBuf.text.size()) {
    mBuf.cursor = mBuf.text.size();
    return true;
  }
  return w_end_of_history();
}

// ---- Deleting and killing ----------------------------------------------------

bool Editor::w_backward_delete_char() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_delete_char(); });
  s::size_t from = mBuf.cursor;
  while (from > 0 and n-- > 0) from = prev_char(mBuf.text, from);
  mBuf.erase(from, mBuf.cursor - from);
  mBuf.cursor = from;
  return true;
}

bool Editor::w_delete_char() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_backward_delete_char(); });
  s::size_t to = mBuf.cursor;
  while (n-- > 0) {
    if (to == mBuf.text.size()) return false;
    to = next_char(mBuf.text, to);
  }
  mBuf.erase(mBuf.cursor, to - mBuf.cursor);
  return true;
}

bool Editor::w_delete_char_or_list() {
  if (mBuf.cursor >= mBuf.text.size()) return w_list_choices();
  return w_delete_char();
}

bool Editor::w_backward_kill_word() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_kill_word(); });
  s::size_t x = prev_word_start_n(mBuf.cursor, n);
  back_kill(mBuf.cursor - x, CUT_FRONT);
  return true;
}

bool Editor::w_kill_word() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_backward_kill_word(); });
  s::size_t x = mBuf.cursor;
  while (n-- > 0) x = word_end(x);
  fore_kill(x - mBuf.cursor, 0);
  return true;
}

bool Editor::w_kill_line() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_backward_kill_line(); });
  strv t = mBuf.text;
  s::size_t& cs = mBuf.cursor;
  s::size_t i = 0;
  while (n-- > 0) {
    if (cs < t.size() and t[cs] == '\n') {
      ++cs;
      ++i;
    } else {
      while (cs != t.size() and t[cs] != '\n') ++cs, ++i;
    }
  }
  back_kill(i, 0);
  return true;
}

bool Editor::w_backward_kill_line() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_kill_line(); });
  strv t = mBuf.text;
  s::size_t& cs = mBuf.cursor;
  s::size_t i = 0;
  while (n-- > 0) {
    if (cs > 0 and t[cs - 1] == '\n') {
      --cs;
      ++i;
    } else {
      while (cs > 0 and t[cs - 1] != '\n') --cs, ++i;
    }
  }
  fore_kill(i, CUT_FRONT);
  return true;
}

bool Editor::w_kill_whole_line() {
  int n = count();
  if (n < 0) return false;
  s::size_t& cs = mBuf.cursor;
  while (n-- > 0) {
    strv t = mBuf.text;
    bool fg = cs > 0 and cs == t.size();
    if (fg) --cs;
    while (cs > 0 and t[cs - 1] != '\n') --cs;
    s::size_t i = cs;
    while (i != t.size() and t[i] != '\n') ++i;
    fore_kill(i - cs + (i != t.size()), fg ? CUT_FRONT : 0);
  }
  return true;
}

bool Editor::w_kill_buffer() {
  mBuf.cursor = 0;
  fore_kill(mBuf.text.size(), 0);
  return true;
}

bool Editor::w_kill_region() {
  s::size_t mark = s::min(mMark.value_or(0), mBuf.text.size());
  if (mRegionActive and mRegionLines) {
    s::size_t a = line_start(s::min(mark, mBuf.cursor));
    s::size_t b = line_end(s::max(mark, mBuf.cursor));
    mRegionActive = false;
    mBuf.cursor = a;
    cut_text(strv(mBuf.text).substr(a, b - a), 0);
    mBuf.erase(a, b - a);
    if (not mBuf.text.empty()) {
      if (mBuf.cursor == mBuf.text.size()) mBuf.cursor = prev_char(mBuf.text, mBuf.cursor);
      mBuf.erase(mBuf.cursor, next_char(mBuf.text, mBuf.cursor) - mBuf.cursor);
      w_vi_first_non_blank();
    }
  } else if (mark > mBuf.cursor) {
    if (in_vicmd()) mark = next_char(mBuf.text, mark);
    fore_kill(mark - mBuf.cursor, 0);
  } else {
    if (in_vicmd() and mBuf.cursor < mBuf.text.size())
      mBuf.cursor = next_char(mBuf.text, mBuf.cursor);
    back_kill(mBuf.cursor - mark, CUT_FRONT);
  }
  mRegionActive = false;
  return true;
}

bool Editor::w_copy_region_as_kill() {
  s::size_t mark = s::min(mMark.value_or(0), mBuf.text.size());
  s::size_t a = s::min(mark, mBuf.cursor), b = s::max(mark, mBuf.cursor);
  if (in_vicmd() and b < mBuf.text.size()) b = next_char(mBuf.text, b);
  cut_text(strv(mBuf.text).substr(a, b - a), mark > mBuf.cursor ? 0 : CUT_FRONT);
  mRegionActive = false;
  return true;
}

// zsh's pastebuf: `n` copies of `buf` at the cursor; whole lines go on
// a line of their own, before the cursor's line (position 0), after it
// (1) or splitting it (2).
void Editor::paste_buffer(const CutBuffer& buf, int n, int position) {
  str& t = mBuf.text;
  s::size_t& cs = mBuf.cursor;
  if (buf.lines) {
    if (position == 2) {
      if (cs == 0)             position = 0;
      else if (cs == t.size()) position = 1;
    }
    if (position == 2) {
      mYankB = cs;
      mBuf.insert_at(cs, "\n" + buf.text + "\n");
      cs += 1 + buf.text.size();
      mYankE = cs + 1;
    } else if (position != 0) {
      mYankB = cs = line_end(cs);
      mBuf.insert_at(cs, "\n" + buf.text);
      ++cs;
      mYankE = cs + buf.text.size();
    } else {
      mYankB = cs = line_start(cs);
      mBuf.insert_at(cs, buf.text + "\n");
      mYankE = cs + buf.text.size() + 1;
    }
    w_vi_first_non_blank();
  } else {
    if (position == 1 and cs != line_end(cs)) cs = next_char(t, cs);
    mYankB = cs;
    while (n-- > 0) {
      mBuf.insert_at(cs, buf.text);
      cs += buf.text.size();
    }
    mYankE = cs;
    if (cs > 0 and in_vicmd()) cs = prev_char(t, cs);
  }
}

bool Editor::w_yank() {
  int n = count();
  if (n < 0) return false;
  const CutBuffer* b = put_buffer(&mYankFrom);
  if (not b) {
    mYankFrom = -2;
    return false;
  }
  CutBuffer buf = *b;
  mYankB = mYankCs = mBuf.cursor;
  mMark = mBuf.cursor;
  mKct = -1;
  while (n-- > 0) {
    mBuf.insert(buf.text);
    mYankE = mBuf.cursor;
  }
  return true;
}

bool Editor::w_yank_pop() {
  if (not mLastWasYank or mZle.kring.empty() or mYankFrom == -2) {
    mYankFrom = -2;
    return false;
  }
  // mKct: how far into the ring (newest first) the last yank went; -1
  // the cut buffer (or register) yanked first.
  int size = static_cast<int>(mZle.kring.size());
  int start = mKct;
  const CutBuffer* buf = nullptr;
  do {
    mKct = mKct + 1 == size ? -1 : mKct + 1;
    if (mKct != -1)        buf = &mZle.kring[static_cast<s::size_t>(mKct)];
    else if (mYankFrom < 0) buf = &mZle.cut_buf;
    else                    buf = &mZle.vibuf[static_cast<s::size_t>(mYankFrom)];
    if (mKct == start) return false;
  } while (not buf->set or buf->text.empty());
  CutBuffer copy = *buf;
  mBuf.erase(mYankB, mYankE - mYankB);
  mBuf.cursor = mYankCs;
  paste_buffer(copy, 1, mLastYankAfter ? 1 : 0);
  return true;
}

// ---- Transposing and case ------------------------------------------------------

namespace {
// Swaps text[a, b) and text[b, c).
void transpose_swap(str& t, s::size_t a, s::size_t b, s::size_t c) {
  str first = t.substr(a, b - a), second = t.substr(b, c - b);
  t.replace(a, c - a, second + first);
}
}  // namespace

bool Editor::w_transpose_chars() {
  str& t = mBuf.text;
  s::size_t& cs = mBuf.cursor;
  int n = count();
  bool neg = n < 0;
  if (neg) n = -n;
  while (n-- > 0) {
    s::size_t ct = cs;
    if (ct == 0 or t[cs - 1] == '\n') {
      if (cs == t.size() or t[cs] == '\n') return false;
      if (not neg) cs = next_char(t, cs);
      ct = next_char(t, ct);
    }
    if (neg) {
      if (cs > 0 and t[cs - 1] != '\n') {
        cs = prev_char(t, cs);
        s::size_t p = prev_char(t, ct);
        if (p > 0 and t[p - 1] != '\n') ct = p;
      }
    } else if (cs != t.size() and t[cs] != '\n') {
      cs = next_char(t, cs);
    }
    if (ct == t.size() or t[ct] == '\n') ct = prev_char(t, ct);
    if (ct < 1 or t[ct - 1] == '\n') return false;
    transpose_swap(t, prev_char(t, ct), ct, next_char(t, ct));
  }
  return true;
}

bool Editor::w_gosmacs_transpose_chars() {
  str& t = mBuf.text;
  s::size_t& cs = mBuf.cursor;
  if (cs < 2 or t[cs - 1] == '\n' or t[prev_char(t, cs) - 1] == '\n') {
    bool twice = cs == 0 or t[cs - 1] == '\n';
    if (cs == t.size() or t[cs] == '\n') return false;
    cs = next_char(t, cs);
    if (twice) {
      if (cs == t.size() or t[cs] == '\n') return false;
      cs = next_char(t, cs);
    }
  }
  s::size_t middle = prev_char(t, cs), start = prev_char(t, middle);
  transpose_swap(t, start, middle, cs);
  return true;
}

bool Editor::w_transpose_words() {
  strv t = mBuf.text;
  auto word = [&](s::size_t p) { return is_word_char(char_at(t, p)); };
  int n = count();
  bool neg = n < 0;
  s::size_t ocs = mBuf.cursor, x = mBuf.cursor, pos;
  if (neg) n = -n;
  while (x != t.size() and t[x] != '\n' and not word(x)) x = next_char(t, x);
  if (x == t.size() or t[x] == '\n') {
    x = mBuf.cursor;
    while (x) {
      if (x < t.size() and word(x)) break;
      pos = prev_char(t, x);
      if (t[pos] == '\n') break;
      x = pos;
    }
    if (not x) return false;
    pos = prev_char(t, x);
    if (t[pos] == '\n') return false;
  }
  s::size_t p4 = x;
  while (p4 != t.size() and word(p4)) p4 = next_char(t, p4);
  s::size_t p3 = p4;
  while (p3) {
    pos = prev_char(t, p3);
    if (not word(pos)) break;
    p3 = pos;
  }
  if (not p3) return false;
  s::size_t p1 = p3, p2 = p3, pt = p3;
  while (n-- > 0) {
    for (p2 = pt; p2;) {
      pos = prev_char(t, p2);
      if (word(pos)) break;
      p2 = pos;
    }
    if (not p2) return false;
    for (p1 = p2; p1;) {
      pos = prev_char(t, p1);
      if (not word(pos)) break;
      p1 = pos;
    }
    pt = p1;
  }
  str swapped = str(t.substr(p3, p4 - p3)) + str(t.substr(p2, p3 - p2))
              + str(t.substr(p1, p2 - p1));
  mBuf.text.replace(p1, p4 - p1, swapped);   // in place (transpose)
  mBuf.cursor = neg ? ocs : p4;
  return true;
}

// up-case-word, down-case-word, capitalize-word: zsh's loops; a negative
// count changes the words after the cursor and leaves the cursor alone.
bool Editor::case_words(int kind) {
  int n = count();
  bool neg = n < 0;
  s::size_t ocs = mBuf.cursor;
  if (neg) n = -n;
  str& t = mBuf.text;
  s::size_t& cs = mBuf.cursor;
  auto word = [&](s::size_t p) { return is_word_char(char_at(t, p)); };
  while (n-- > 0) {
    while (cs != t.size() and not word(cs)) cs = next_char(t, cs);
    if (kind == 2)
      while (cs != t.size() and word(cs) and not s::iswalpha(static_cast<wint_t>(char_at(t, cs))))
        cs = next_char(t, cs);
    bool first = true;
    while (cs != t.size() and word(cs)) {
      s::size_t next = next_char(t, cs);
      bool up = kind == 0 or (kind == 2 and first);
      map_chars(t, cs, next, up ? to_upper : to_lower);
      first = false;
      cs = next_char(t, cs);
    }
  }
  if (neg) cs = ocs;
  return true;
}

bool Editor::w_up_case_word()    { return case_words(0); }
bool Editor::w_down_case_word()  { return case_words(1); }
bool Editor::w_capitalize_word() { return case_words(2); }

// ---- The mark, modes, undo ---------------------------------------------------

bool Editor::w_set_mark_command() {
  if (mArg and *mArg < 0) {
    mRegionActive = false;
    return true;
  }
  mMark = mBuf.cursor;
  mRegionActive = true;
  return true;
}

// zsh's exchangepointandmark: a mark never set is 0; a count of 0 only
// activates the region, a negative one swaps without activating it.
bool Editor::w_exchange_point_and_mark() {
  int n = count();
  if (n == 0) {
    mRegionActive = true;
    return true;
  }
  s::size_t m = s::min(mMark.value_or(0), mBuf.text.size());
  mMark = mBuf.cursor;
  mBuf.cursor = m;
  if (n > 0) mRegionActive = true;
  return true;
}

bool Editor::w_deactivate_region() {
  mRegionActive = false;
  if (mLocalKeymap) {   // leaving visual mode
    mLocalKeymap.reset();
    vi_clamp_cursor();
  }
  return true;
}

bool Editor::w_overwrite_mode() {
  mBuf.overwrite = not mBuf.overwrite;
  return true;
}

Editor::Change Editor::diff_change(strv before, strv after, s::size_t cursor,
                                   s::size_t cursor_after) {
  s::size_t pre = 0, n = s::min(before.size(), after.size());
  while (pre < n and before[pre] == after[pre]) ++pre;
  s::size_t suf = 0;
  while (suf < n - pre and before[before.size() - 1 - suf] == after[after.size() - 1 - suf]) ++suf;
  return {str(before.substr(pre, before.size() - pre - suf)),
          str(after.substr(pre, after.size() - pre - suf)), pre, cursor, cursor_after, 0};
}

// zsh's undo and redo: a change made in another history line takes the
// editor to that line first, the change itself waiting for the next undo
// (unapplychange, applychange).
bool Editor::w_undo() {
  if (mUndo.empty()) return false;
  if (mUndo.back().hist != mHistIndex) {
    set_history_line(static_cast<int>(mUndo.back().hist));
    mBuf.cursor = s::min(mUndo.back().cursor_after, mBuf.text.size());
    return true;
  }
  Change ch = s::move(mUndo.back());
  mUndo.pop_back();
  mBuf.replace(ch.at, ch.ins.size(), ch.del);
  mBuf.cursor = s::min(ch.cursor, mBuf.text.size());
  mRedo.push_back(s::move(ch));
  mUndoGroup = 0;
  return true;
}

bool Editor::w_redo() {
  if (mRedo.empty()) return false;
  if (mRedo.back().hist != mHistIndex) {
    set_history_line(static_cast<int>(mRedo.back().hist));
    mBuf.cursor = s::min(mRedo.back().cursor, mBuf.text.size());
    return true;
  }
  Change ch = s::move(mRedo.back());
  mRedo.pop_back();
  mBuf.replace(ch.at, ch.del.size(), ch.ins);
  mBuf.cursor = s::min(ch.cursor_after, mBuf.text.size());
  mUndo.push_back(s::move(ch));
  return true;
}

bool Editor::is_yank_widget(strv name) {
  return name == "yank" or name == "vi-put-after" or name == "vi-put-before"
      or name == "bracketed-paste";
}

bool Editor::w_clear_screen() {
  write_out("\x1b[H\x1b[2J");
  mDrawn = false;
  mCursorRow = 0;
  return true;
}

bool Editor::w_redisplay() {
  return true;
}

// zsh's digit-argument, neg-argument and universal-argument build the
// count in tmult; it becomes the count (mArg) once the prefix is done.
bool Editor::w_digit_argument() {
  int sign = count() < 0 ? -1 : 1;
  char d = mKeys.empty() ? 0 : static_cast<char>(mKeys.back() & 0x7f);
  int digit = (d >= '0' and d <= '9') ? d - '0' : -1;
  if (digit < 0 or digit >= mArgBase) return false;
  if (not mTMultSet) mTMult = 0;
  if (mArgNeg) {
    mTMult  = sign * digit;
    mArgNeg = false;
  } else {
    mTMult = mTMult * mArgBase + sign * digit;
  }
  mTMultSet = true;
  mArgSet   = true;
  return true;
}

bool Editor::w_neg_argument() {
  if (mTMultSet) return false;
  mTMult    = -1;
  mTMultSet = true;
  mArgNeg   = true;
  mArgSet   = true;
  return true;
}

bool Editor::w_universal_argument() {
  int digits = 0, pref = 0, minus = 1;
  for (;;) {
    int c = read_byte(-1);
    if (c < 0) break;
    if (c == '-' and not digits) {
      minus = -1;
      ++digits;
    } else if (c >= '0' and c <= '9' and c - '0' < mArgBase) {
      pref = pref * mArgBase + (c - '0');
      ++digits;
    } else {
      push_input(str(1, static_cast<char>(c)));
      break;
    }
  }
  if (digits) mTMult = minus * (pref ? pref : 1);
  else        mTMult *= 4;
  mTMultSet = true;
  mArgSet   = true;
  return true;
}

// zsh's argumentbase: the count is the base for the digits that follow,
// and the modifiers start afresh, still a prefix.
bool Editor::w_argument_base() {
  int n = mWidgetArgs.empty() ? count()
        : static_cast<int>(s::strtol(mWidgetArgs[0].c_str(), nullptr, 0));
  if (n < 2 or n > 36) return false;
  reset_modifiers();
  mArgBase = n;
  mArgSet  = true;
  return true;
}

// ---- History -----------------------------------------------------------------

void Editor::set_history_line(int index) {
  const auto& e = mHist.entries();
  int size = static_cast<int>(e.size());
  index = s::clamp(index, 0, size);
  // Keep what was typed (or changed in a history line) for coming back.
  str original = mHistIndex < size ? e[static_cast<s::size_t>(mHistIndex)].text : str{};
  if (mHistIndex >= size or mBuf.text != original) mHistEdits[mHistIndex] = mBuf.text;
  mHistIndex = index;
  if (auto it = mHistEdits.find(index); it != mHistEdits.end()) mBuf.text = it->second;
  else mBuf.text = index < size ? e[static_cast<s::size_t>(index)].text : str{};
  mBuf.cursor = mBuf.text.size();
}

bool Editor::history_matches(int index, strv prefix) const {
  const auto& e = mHist.entries();
  const str& t = e[static_cast<s::size_t>(index)].text;
  if (not strv(t).starts_with(prefix)) return false;
  if (t == mBuf.text) return false;
  return true;
}

vec<str> Editor::history_words(int index) const {
  const auto& e = mHist.entries();
  if (index < 0 or index >= static_cast<int>(e.size())) return {};
  return HistoryManager::split_words(e[static_cast<s::size_t>(index)].text);
}

// zsh's upline and downline: `n` lines up or down, to the column the
// first of a run of line moves started from (lastcol, in characters);
// what is left of `n` when the buffer ran out.
int Editor::up_line(int n) {
  if (n < 0) return -down_line(-n);
  const str& t = mBuf.text;
  if (mLastCol == -1) {
    s::size_t bol = line_start(mBuf.cursor);
    mLastCol = static_cast<int>(char_count(strv(t).substr(bol, mBuf.cursor - bol)));
  }
  mBuf.cursor = line_start(mBuf.cursor);
  for (; n; --n) {
    if (mBuf.cursor == 0) break;
    mBuf.cursor = line_start(mBuf.cursor - 1);
  }
  if (not n) to_last_col();
  return n;
}

int Editor::down_line(int n) {
  if (n < 0) return -up_line(-n);
  const str& t = mBuf.text;
  if (mLastCol == -1) {
    s::size_t bol = line_start(mBuf.cursor);
    mLastCol = static_cast<int>(char_count(strv(t).substr(bol, mBuf.cursor - bol)));
  }
  for (; n; --n) {
    s::size_t x = line_end(mBuf.cursor);
    if (x == t.size()) break;
    mBuf.cursor = x + 1;
  }
  if (not n) to_last_col();
  return n;
}

// lastcol characters on from the cursor, no further than the line's end
// (in vicmd, its last character).
void Editor::to_last_col() {
  const str& t = mBuf.text;
  s::size_t x = line_end(mBuf.cursor);
  s::size_t pos = mBuf.cursor;
  int i = 0;
  for (; i < mLastCol and pos < x; ++i) pos = next_char(t, pos);
  mBuf.cursor = pos;
  if (i < mLastCol or pos >= x) {
    mBuf.cursor = x;
    if (x > line_start(x) and in_vicmd()) mBuf.cursor = prev_char(t, x);
  }
}

bool Editor::w_up_line() { return up_line(count()) == 0; }

bool Editor::w_down_line() { return down_line(count()) == 0; }

// zsh's uplineorhistory and friends: what is left of the count once the
// buffer's lines ran out goes to the history widget, from where the
// cursor was.
bool Editor::line_or(bool up, bool (Editor::*hist)()) {
  s::size_t ocs = mBuf.cursor;
  int n = up ? up_line(count()) : down_line(count());
  if (not n) return true;
  mBuf.cursor = ocs;
  opt<int> m = mArg;
  mArg = n;
  bool ok = (this->*hist)();
  mArg = m;
  return ok;
}

bool Editor::w_up_history() {
  bool nodups = mOpts.get(OPT_HISTFINDNODUPS);
  const auto& e = mHist.entries();
  int idx = mHistIndex;
  for (int n = count(); n > 0; --n) {
    int prev = idx - 1;
    if (nodups)
      while (prev >= 0 and e[static_cast<s::size_t>(prev)].text == mBuf.text) --prev;
    if (prev < 0) break;
    idx = prev;
  }
  if (idx == mHistIndex) return false;
  set_history_line(idx);
  return true;
}

bool Editor::w_down_history() {
  bool nodups = mOpts.get(OPT_HISTFINDNODUPS);
  const auto& e = mHist.entries();
  int size = static_cast<int>(e.size());
  int idx = mHistIndex;
  for (int n = count(); n > 0; --n) {
    int next = idx + 1;
    if (nodups)
      while (next < size and e[static_cast<s::size_t>(next)].text == mBuf.text) ++next;
    if (next > size) break;
    idx = next;
  }
  if (idx == mHistIndex) return false;
  set_history_line(idx);
  return true;
}

bool Editor::w_up_line_or_history() { return line_or(true, &Editor::w_up_history); }

bool Editor::w_down_line_or_history() { return line_or(false, &Editor::w_down_history); }

bool Editor::w_beginning_of_history() {
  if (mHist.entries().empty()) return false;
  set_history_line(0);
  return true;
}

bool Editor::w_end_of_history() {
  set_history_line(static_cast<int>(mHist.entries().size()));
  return true;
}

bool Editor::w_history_search_backward() {
  // The first word of the line, kept while the search is repeated.
  if (mLastWidget != "history-search-backward" and mLastWidget != "history-search-forward") {
    auto words = HistoryManager::split_words(mBuf.text);
    mSearchPrefix = words.empty() ? str{} : words[0];
  }
  for (int i = mHistIndex - 1; i >= 0; --i) {
    if (history_matches(i, mSearchPrefix)) {
      set_history_line(i);
      return true;
    }
  }
  return false;
}

bool Editor::w_history_search_forward() {
  if (mLastWidget != "history-search-backward" and mLastWidget != "history-search-forward") {
    auto words = HistoryManager::split_words(mBuf.text);
    mSearchPrefix = words.empty() ? str{} : words[0];
  }
  int size = static_cast<int>(mHist.entries().size());
  for (int i = mHistIndex + 1; i < size; ++i) {
    if (history_matches(i, mSearchPrefix)) {
      set_history_line(i);
      return true;
    }
  }
  if (mHistIndex < size) {
    set_history_line(size);
    return true;
  }
  return false;
}

bool Editor::w_history_beginning_search_backward() {
  str prefix = mBuf.text.substr(0, mBuf.cursor);
  s::size_t cur = mBuf.cursor;
  for (int i = mHistIndex - 1; i >= 0; --i) {
    if (history_matches(i, prefix)) {
      set_history_line(i);
      mBuf.cursor = cur;
      return true;
    }
  }
  return false;
}

bool Editor::w_history_beginning_search_forward() {
  str prefix = mBuf.text.substr(0, mBuf.cursor);
  s::size_t cur = mBuf.cursor;
  int size = static_cast<int>(mHist.entries().size());
  for (int i = mHistIndex + 1; i < size; ++i) {
    if (history_matches(i, prefix)) {
      set_history_line(i);
      mBuf.cursor = cur;
      return true;
    }
  }
  return false;
}

bool Editor::w_history_incremental_search_backward() { return incremental_search(true); }
bool Editor::w_history_incremental_search_forward()  { return incremental_search(false); }

bool Editor::incremental_search(bool backward) {
  const auto& entries = mHist.entries();
  int size = static_cast<int>(entries.size());
  // `fwd`: found going forward, where the cursor goes to the match's end.
  struct State { s::size_t pos; str pattern; int index; bool failed; bool fwd = false; };
  int  orig_index  = mHistIndex;
  str  orig_text   = mBuf.text;
  s::size_t orig_cursor = mBuf.cursor;
  State st{mBuf.cursor, {}, mHistIndex, false};
  vec<State> stack;
  // zsh's odir: vi-repeat-search goes this way, vi-rev-repeat-search the
  // other.
  bool orig_backward = backward;

  auto line_text = [&](int i) -> str {
    if (i == orig_index) return orig_text;
    if (auto it = mHistEdits.find(i); it != mHistEdits.end()) return it->second;
    return i < size ? entries[static_cast<s::size_t>(i)].text : str{};
  };
  // zlinecmp: with no numeric argument (zmult 1) a lower-case letter of
  // the search also matches its upper case; the text at `at` begins with
  // `pat`.
  bool fold = count() == 1;
  auto match_at = [&](const str& t, s::size_t at, const str& pat) {
    if (at > t.size()) return false;
    if (t.compare(at, pat.size(), pat) == 0) return true;
    if (not fold) return false;
    for (s::size_t i = at, j = 0, hl = 1, pl = 1; j < pat.size(); i += hl, j += pl) {
      if (i >= t.size()) return false;
      char32_t h = utf8_decode(t, i, hl), p = utf8_decode(pat, j, pl);
      if (static_cast<char32_t>(s::towlower(static_cast<wint_t>(h))) != p) return false;
    }
    return true;
  };
  // Finds `pattern` from (index, pos), moving to other lines as needed;
  // `skip` steps past the current match first.
  auto find = [&](State& s, bool skip) -> bool {
    if (s.pattern.empty()) return true;
    int i = s.index;
    s::size_t pos = s.pos;
    str last = line_text(i);
    for (;;) {
      str t = line_text(i);
      s::size_t at = str::npos;
      if (backward) {
        if (not (skip and i == s.index and pos == 0)) {
          s::size_t from = skip and i == s.index ? pos - 1 : pos;
          for (s::size_t k = s::min(from, t.size()) + 1; k-- > 0;)
            if (match_at(t, k, s.pattern)) {
              at = k;
              break;
            }
        }
      } else {
        s::size_t from = skip and i == s.index ? pos + 1 : pos;
        for (s::size_t k = from; k <= t.size(); ++k)
          if (match_at(t, k, s.pattern)) {
            at = k;
            break;
          }
      }
      // A line the same as the one the step began on is skipped (zsh's
      // last_line), with HIST_FIND_NO_DUPS or not.
      bool dup = i != s.index and t == last;
      if (at != str::npos and not dup) {
        s.index = i;
        s.pos   = at;
        s.fwd   = not backward;
        return true;
      }
      if (backward) {
        if (--i < 0) return false;
        pos = str::npos;
      } else {
        if (++i > size) return false;
        pos = 0;
      }
    }
  };
  auto show = [&](const State& s) {
    if (s.index != mHistIndex) set_history_line(s.index);
    mBuf.cursor = s::min(s.pos + (s.fwd ? s.pattern.size() : 0), mBuf.text.size());
  };

  // zsh's doisearch reads its keys in the main keymap with the isearch
  // keymap over it, and puts back the keymap it found at the end (after
  // an accept-line it ran).
  struct KeymapBack {
    Editor& e;
    str     name;
    ~KeymapBack() {
      e.select_keymap(name);
      e.mLocalKeymap.reset();
    }
  } keymap_back{*this, mKeymapName};
  select_keymap("main");
  mLocalKeymap = mZle.keymap("isearch");

  for (;;) {
    mMessage = str(st.failed ? "failing " : "") + (backward ? "bck-i-search: " : "fwd-i-search: ")
             + status_text(st.pattern) + "_";
    refresh();
    const KeyBinding* b = nullptr;
    str seq = read_binding(b);
    if (mEof) return false;
    if (seq == "\x03") {
      if (mZle.on_interrupt and not mZle.on_interrupt(*this)) continue;
      mMessage.clear();
      mInterrupted = true;
      mDone = true;
      return true;
    }
    // A string binding's keys are read as typed (getkeycmd).
    if (b and b->is_macro) {
      push_input(b->macro);
      continue;
    }
    str w = b ? mZle.resolve_widget(b->widget) : str{};
    if (w.starts_with('.')) w.erase(0, 1);
    bool insert = w == "self-insert" or w == "self-insert-unmeta" or w == "magic-space"
               or (not b and not seq.empty() and static_cast<unsigned char>(seq[0]) >= 0x80);
    // (vi-)quoted-insert: the next key, read raw, goes into the search;
    // bracketed-paste: the pasted text.
    if (w == "quoted-insert" or w == "vi-quoted-insert") {
      if (w == "vi-quoted-insert") {
        mMessage.back() = '^';
        refresh();
      }
      // getfullchar: the whole character.
      int c = read_byte(-1);
      seq = c < 0 ? str{} : str(1, static_cast<char>(c));
      if (c < 0 or not read_rest_char(seq)) {
        beep();
        continue;
      }
      insert = true;
    } else if (w == "bracketed-paste") {
      seq    = read_paste();
      insert = true;
    } else if (insert and w != "magic-space" and not read_rest_char(seq)) {
      // getrestchar: a character's other bytes, or a beep.
      beep();
      continue;
    }
    if (insert) {
      stack.push_back(st);
      st.pattern += w == "magic-space" ? str(" ") : seq;
      if (not st.failed) {
        State next = st;
        if (find(next, false)) {
          st = next;
          show(st);
        } else {
          st.failed = true;
        }
      }
    } else if (w == "clear-screen" or w == "redisplay") {
      run_widget(w);
    } else if (w == "vi-cmd-mode") {
      // The keys go on being read, in the other vi keymap.
      if (not select_keymap(keymap_name() == "vicmd" ? "main" : "vicmd")) beep();
      mLocalKeymap = mZle.keymap("isearch");
    } else if (w == "backward-delete-char" or w == "vi-backward-delete-char"
               or w == "backward-kill-word" or w == "vi-backward-kill-word"
               or w == "backward-delete-word") {
      // A character off the search; a word's widget goes back past the
      // repeated searches to where the text was shorter.
      bool only_one = w == "backward-delete-char" or w == "vi-backward-delete-char";
      if (not stack.empty()) {
        s::size_t old_len = st.pattern.size();
        do {
          st = stack.back();
          stack.pop_back();
        } while (not only_one and not stack.empty() and st.pattern.size() == old_len);
        show(st);
      } else {
        beep();
      }
    } else if (w == "history-incremental-search-backward"
               or w == "history-incremental-search-forward"
               or w == "history-incremental-pattern-search-backward"
               or w == "history-incremental-pattern-search-forward"
               or w == "vi-repeat-search" or w == "vi-rev-repeat-search") {
      bool dir_back = w == "vi-repeat-search"       ? orig_backward
                    : w == "vi-rev-repeat-search"   ? not orig_backward
                    : w.ends_with("search-backward");
      // An empty search takes the last one, going the first way (rpt).
      if (st.pattern.empty() and not mLastIsearch.empty() and dir_back == orig_backward) {
        backward = dir_back;
        st.pattern = mLastIsearch;
        State next = st;
        if (find(next, false)) { st = next; show(st); } else st.failed = true;
        continue;
      }
      // The same way again steps past this match; the other way starts
      // from it (skip_pos only when dir doesn't change), but the vi
      // repeats always step.
      bool skip = dir_back == backward or w.starts_with("vi-");
      backward = dir_back;
      stack.push_back(st);
      State next = st;
      if (find(next, skip)) {
        st = next;
        st.failed = false;
        show(st);
      } else {
        st.failed = true;
        beep();
      }
    } else if (w == "send-break") {
      // Back to where the search started.
      mMessage.clear();
      set_history_line(orig_index);
      mBuf.text   = orig_text;
      mBuf.cursor = orig_cursor;
      return true;
    } else {
      if (not st.pattern.empty()) mLastIsearch = st.pattern;
      mMessage.clear();
      // The accepting widgets end the search and run in it (exitfn);
      // accept-search only ends it.
      static const uset<str> accepts = {
        "accept-and-hold", "accept-and-infer-next-history", "accept-line-and-down-history",
        "accept-line"};
      if (accepts.contains(w)) {
        mKeys = seq;
        return run_widget(w);
      }
      // Any other key ends the search and is read again (ungetkeycmd):
      // the main loop runs it, with the search as $LASTWIDGET.
      if (w != "accept-search") push_input(seq);
      return true;
    }
  }
}

// zsh's infernexthist: from two entries before `from` (the line being
// edited is `from`) back, the first that is the line; the entry after it,
// or -1.
int Editor::infer_next(int from) const {
  const auto& e = mHist.entries();
  for (int i = s::min(from, static_cast<int>(e.size())) - 2; i >= 0; --i)
    if (e[static_cast<s::size_t>(i)].text == mBuf.text) return i + 1;
  return -1;
}

bool Editor::w_infer_next_history() {
  int i = infer_next(mHistIndex);
  if (i < 0) return false;
  set_history_line(i);
  return true;
}

bool Editor::w_insert_last_word() {
  int hist = static_cast<int>(mHist.entries().size()) - 1;
  bool again = mLastWidget == "insert-last-word"
           and mLastWordEnd <= mBuf.text.size() and mBuf.cursor == mLastWordEnd;
  if (again) {
    hist = mLastWordHist - 1;
    mBuf.erase(mLastWordStart, mLastWordEnd - mLastWordStart);
    mBuf.cursor = mLastWordStart;
  }
  for (; hist >= 0; --hist) {
    vec<str> words = history_words(hist);
    if (words.empty()) continue;
    int n = mArg ? *mArg : 1;
    long idx = n > 0 ? static_cast<long>(words.size()) - n : -n;
    if (idx < 0 or idx >= static_cast<long>(words.size())) return false;
    mLastWordStart = mBuf.cursor;
    mBuf.insert(words[static_cast<s::size_t>(idx)]);
    mLastWordEnd   = mBuf.cursor;
    mLastWordHist  = hist;
    return true;
  }
  return false;
}

// zsh's copyprevword: the count-th word before the cursor, by word
// characters.
bool Editor::w_copy_prev_word() {
  int n = count();
  if (n <= 0) return false;
  strv t = mBuf.text;
  s::size_t t0 = mBuf.cursor, t1 = t0;
  for (;;) {
    t1 = t0;
    while (t0 and not is_word_char(char_at(t, prev_char(t, t0)))) t0 = prev_char(t, t0);
    while (t0 and is_word_char(char_at(t, prev_char(t, t0)))) t0 = prev_char(t, t0);
    if (not --n) break;
    if (t0 == 0) return false;
  }
  str word(t.substr(t0, t1 - t0));
  mBuf.insert(word);
  return true;
}

// zsh's copyprevshellword: the shell word at the cursor, or count-1
// words before it, as bufferwords finds it.  That is the first word (not
// an operator) that ends no more than a character before the cursor
// (gotword: zlemetacs, one back, against the word's end and the lexer's
// look-ahead), else the last word.
bool Editor::w_copy_prev_shell_word() {
  int n = count();
  if (n <= 0) return false;
  const str& t = mBuf.text;
  auto spans = HistoryManager::word_positions(t);
  if (spans.empty()) return true;
  s::size_t cs = mBuf.cursor ? mBuf.cursor - 1 : 0;
  long i = -1;
  for (s::size_t k = 0; k < spans.size(); ++k) {
    auto [b, e] = spans[k];
    bool op = t.find_first_not_of(";|&<>()", b) >= e;
    if (not op and cs <= e + 1) {
      i = static_cast<long>(k);
      break;
    }
  }
  if (i < 0) i = static_cast<long>(spans.size()) - 1;
  i -= n - 1;
  if (i < 0) return false;
  auto [b, e] = spans[static_cast<s::size_t>(i)];
  mBuf.insert(t.substr(b, e - b));
  return true;
}

// zsh's pushline: a count pushes empty lines over it, taken back first.
bool Editor::w_push_line() {
  int n = count();
  if (n < 0) return false;
  mZle.buffer_stack.push_back(mBuf.text);
  while (--n > 0) mZle.buffer_stack.emplace_back();
  mZle.stack_cs = static_cast<int>(mBuf.cursor) + (in_vicmd() ? 1 : 0);
  mBuf.text.clear();
  mBuf.cursor = 0;
  return true;
}

bool Editor::w_get_line() {
  if (mZle.buffer_stack.empty()) return false;
  mBuf.insert(mZle.buffer_stack.back());
  mZle.buffer_stack.pop_back();
  return true;
}

bool Editor::w_expand_history() {
  auto r = mHist.expand_buffer(mBuf.text);
  if (not r) {
    mMessage = "jfsh: " + r.error().message;
    return false;
  }
  if (not r->changed) return true;
  mBuf.text   = r->text;
  mBuf.cursor = mBuf.text.size();
  mBuf.spaced = true;   // doexpandhist rebuilds the line: the region ends
  return true;
}

bool Editor::w_magic_space() {
  w_expand_history();
  mBuf.insert(" ");
  return true;
}

bool Editor::w_edit_command_line() {
  char tmpl[] = "/tmp/jfsh_editXXXXXX";
  int fd = ::mkstemp(tmpl);
  if (fd < 0) return false;
  str body = mBuf.text + "\n";
  bool wrote = ::write(fd, body.data(), body.size()) == static_cast<ssize_t>(body.size());
  ::close(fd);
  if (not wrote) {
    ::unlink(tmpl);
    return false;
  }
  str ed = mZle.param("VISUAL");
  if (ed.empty()) ed = mZle.param("EDITOR");
  if (ed.empty()) ed = "vi";
  str cmd = ed + " '" + tmpl + "' </dev/tty >/dev/tty";
  trash();
  exit_raw_mode();
  int rc = mZle.hooks.run_command ? mZle.hooks.run_command(cmd) : ::system(cmd.c_str());
  enter_raw_mode();
  str edited;
  if (rc == 0) {
    int in = ::open(tmpl, O_RDONLY | O_CLOEXEC);
    if (in >= 0) {
      char buf[4096];
      ssize_t n;
      while ((n = ::read(in, buf, sizeof buf)) > 0) edited.append(buf, static_cast<s::size_t>(n));
      ::close(in);
    }
  }
  ::unlink(tmpl);
  if (rc != 0) return false;
  while (not edited.empty() and edited.back() == '\n') edited.pop_back();
  mBuf.text   = edited;
  mBuf.cursor = mBuf.text.size();
  return true;
}

namespace {
str quoted(strv s) {
  str out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else           out += c;
  }
  return out + "'";
}
}  // namespace

bool Editor::w_quote_line() {
  mBuf.text   = quoted(mBuf.text);
  mBuf.cursor = mBuf.text.size();
  return true;
}

bool Editor::w_quote_region() {
  if (not mMark) return false;
  s::size_t a = s::min(*mMark, mBuf.cursor), b = s::min(s::max(*mMark, mBuf.cursor), mBuf.text.size());
  str q = quoted(strv(mBuf.text).substr(a, b - a));
  mBuf.replace(a, b - a, q);
  mBuf.cursor = a + q.size();
  mRegionActive = false;
  return true;
}

bool Editor::w_what_cursor_position() {
  // zsh's whatcursorposition: points and columns count characters.
  strv t = mBuf.text;
  long cs  = static_cast<long>(char_count(t.substr(0, mBuf.cursor)));
  long ll  = static_cast<long>(char_count(t));
  long bol = static_cast<long>(char_count(t.substr(0, line_start(mBuf.cursor))));
  str msg;
  if (mBuf.cursor >= t.size()) {
    msg = "EOF";
  } else {
    s::size_t len;
    char32_t c = utf8_decode(t, mBuf.cursor, len);
    msg = "Char: ";
    if (c == ' ')       msg += "SPC";
    else if (c == '\t') msg += "TAB";
    else if (c == '\n') msg += "LFD";
    else                msg += t.substr(mBuf.cursor, len);
    char code[64];
    s::snprintf(code, sizeof code, " (0%o, %u, 0x%x)", static_cast<unsigned>(c),
                static_cast<unsigned>(c), static_cast<unsigned>(c));
    msg += code;
  }
  char pos[96];
  s::snprintf(pos, sizeof pos, "  point %ld of %ld(%ld%%)  column %ld", cs + 1, ll + 1,
              ll ? 100 * cs / ll : 0, cs - bol);
  mMessage = msg + pos;
  return true;
}

opt<str> Editor::minibuffer(strv prompt) {
  str text;
  for (;;) {
    mMessage = str(prompt) + status_text(text) + "_";
    refresh();
    int c = read_byte(-1);
    if (c < 0 or c == 0x03 or c == 0x07) {   // EOF, ^C, ^G
      mMessage.clear();
      return s::nullopt;
    }
    if (c == '\r' or c == '\n') break;
    if (c == 0x7f or c == 0x08) {
      if (text.empty()) {
        mMessage.clear();
        return s::nullopt;
      }
      text.resize(prev_char(text, text.size()));
      continue;
    }
    if (c == 0x15) { text.clear(); continue; }   // ^U
    if (c < 0x20) continue;
    text += static_cast<char>(c);
  }
  mMessage.clear();
  return text;
}

// zsh's executenamedcommand: reads a widget name below the line through
// the `command` keymap over `main`, completing it on Tab, space or
// accept-line and listing the names on ^D.  The widget's name, or nullopt
// for send-break.
opt<str> Editor::read_command_name(strv prompt) {
  constexpr s::size_t NAMLEN = 60;
  str cmd;
  bool listed = false;
  auto saved_keymap = mKeymap;
  str saved_name = mKeymapName;
  auto saved_local = mLocalKeymap;
  auto restore = [&] {
    mKeymap = saved_keymap;
    mKeymapName = saved_name;
    mLocalKeymap = saved_local;
    mMessage.clear();
    if (listed) mListing.clear();
  };
  if (not select_keymap("main")) select_keymap(".safe");
  mLocalKeymap = mZle.keymap("command");
  for (;;) {
    mMessage = str(prompt) + status_text(cmd) + "_";
    refresh();
    const KeyBinding* b = nullptr;
    str seq = read_binding(b);
    if (mEof) {
      restore();
      return s::nullopt;
    }
    mKeys = seq;
    str w = b and not b->is_macro ? mZle.resolve_widget(b->widget) : str{};
    if (w.starts_with('.')) w.erase(0, 1);
    if (seq == "\x03" or w == "send-break") {
      restore();
      return s::nullopt;
    }
    bool feep = false;
    if (w == "clear-screen") {
      w_clear_screen();
    } else if (w == "redisplay") {
    } else if (w == "quoted-insert" or w == "vi-quoted-insert") {
      int c = read_byte(-1);
      if (c <= 0 or cmd.size() >= NAMLEN) feep = true;
      else cmd += static_cast<char>(c);
    } else if (w == "backward-delete-char" or w == "vi-backward-delete-char") {
      if (not cmd.empty()) cmd.resize(prev_char(cmd, cmd.size()));
    } else if (w == "kill-region" or w == "backward-kill-word" or w == "vi-backward-kill-word") {
      while (not cmd.empty()) {
        s::size_t p = prev_char(cmd, cmd.size());
        bool dash = cmd[p] == '-';
        cmd.resize(p);
        if (dash) break;
      }
    } else if (w == "kill-whole-line" or w == "vi-kill-line" or w == "backward-kill-line") {
      cmd.clear();
      if (listed) {
        mListing.clear();
        listed = false;
      }
    } else {
      bool accept = w == "accept-line" or w == "vi-cmd-mode";
      if (accept and mZle.is_widget(cmd)) {
        restore();
        return cmd;
      }
      if (w == "self-insert-unmeta") {
        mKeys = str(1, static_cast<char>(static_cast<unsigned char>(seq.back()) & 0x7f));
        seq = mKeys;
        w = "self-insert";
      }
      char last = seq.empty() ? 0 : seq.back();
      bool list = w == "list-choices" or w == "delete-char-or-list";
      if (list or w == "expand-or-complete" or w == "complete-word"
          or w == "expand-or-complete-prefix" or accept or last == ' ' or last == '\t') {
        vec<str> names;
        for (const auto& n : mZle.widget_names())
          if (strv(n).starts_with(cmd)) names.push_back(n);
        if (names.empty()) {
          feep = true;
        } else if (list) {
          list_names(names);
          listed = true;
        } else if (names.size() == 1) {
          cmd = names[0];
          if (accept) {
            restore();
            return cmd;
          }
        } else {
          s::size_t amb = names[0].size();
          for (const auto& n : names) {
            s::size_t l = 0;
            while (l < amb and l < n.size() and n[l] == names[0][l]) ++l;
            amb = l;
          }
          bool longer = amb > cmd.size();
          cmd = names[0].substr(0, amb);
          if (mOpts.get(OPT_AUTOLIST) and not (mOpts.get(OPT_LISTAMBIGUOUS) and longer)) {
            if (mOpts.get(OPT_LISTBEEP)) feep = true;
            list_names(names);
            listed = true;
          }
        }
      } else if (cmd.size() >= NAMLEN or w != "self-insert") {
        feep = true;
      } else {
        str ch = last_full_char();
        unsigned char c0 = ch.empty() ? 0 : static_cast<unsigned char>(ch[0]);
        if (ch.empty() or c0 < 0x20 or c0 == 0x7f) {
          feep = true;
        } else {
          cmd += ch;
          if (listed) {
            mListing.clear();
            listed = false;
          }
        }
      }
    }
    if (feep) beep();
  }
}

void Editor::list_names(const vec<str>& names) {
  CompletionResult r;
  for (const auto& n : names) {
    CompCandidate m;
    m.word = n;
    r.matches.push_back(s::move(m));
  }
  list_matches(r);
}

// getkeycmd runs the widget named as the key's own: $WIDGET and then
// $LASTWIDGET are its name; none named, undefined-key's.
bool Editor::w_execute_named_cmd() {
  auto name = read_command_name("execute: ");
  if (not name) {
    mWidget = "undefined-key";
    return run_widget(mWidget);   // fails: one bell, the outer widget's
  }
  mLastNamedCmd = *name;
  mWidget       = *name;
  return run_widget(*name);
}

bool Editor::w_execute_last_named_cmd() {
  if (mLastNamedCmd.empty()) return false;
  mWidget = mLastNamedCmd;
  return run_widget(mLastNamedCmd);
}

bool Editor::w_vi_cmd_mode() {
  if (in_vicmd()) return false;
  // Leaving insert mode: the cursor steps back onto the last character.
  if (mBuf.cursor > line_start(mBuf.cursor)) mBuf.cursor = prev_char(mBuf.text, mBuf.cursor);
  select_keymap("vicmd");
  mUndoGroup = 0;
  mBuf.overwrite = false;
  if (mViRecord and not mViReplaying) {
    mViLastChange = mViRecording;
    mViRecord = false;
  }
  return true;
}

bool Editor::w_beep() {
  beep();
  return true;
}

bool Editor::w_pound_insert() {
  if (mBuf.text.starts_with('#')) mBuf.erase(0, 1);
  else mBuf.insert_at(0, "#");
  mDone = true;
  return true;
}

bool Editor::w_vi_match_bracket() {
  strv t = mBuf.text;
  static constexpr strv open = "([{", close = ")]}";
  s::size_t pos = mBuf.cursor;
  while (pos < t.size() and open.find(t[pos]) == strv::npos and close.find(t[pos]) == strv::npos
         and t[pos] != '\n')
    ++pos;
  if (pos >= t.size() or t[pos] == '\n') return false;
  char c = t[pos];
  bool fwd = open.find(c) != strv::npos;
  char other = fwd ? close[open.find(c)] : open[close.find(c)];
  int depth = 0;
  if (fwd) {
    for (s::size_t i = pos; i < t.size(); ++i) {
      if (t[i] == c) ++depth;
      else if (t[i] == other and --depth == 0) { mBuf.cursor = i; return true; }
    }
  } else {
    for (s::size_t i = pos + 1; i-- > 0;) {
      if (t[i] == c) ++depth;
      else if (t[i] == other and --depth == 0) { mBuf.cursor = i; return true; }
    }
  }
  return false;
}

bool Editor::w_vi_find_next_char() {
  str ch = vi_read_char();
  if (ch.empty()) return false;
  s::size_t len;
  mViFindChar = utf8_decode(ch, 0, len);
  mViFindDir  = 1;
  mViFindSkip = false;
  return vi_find(mViFindChar, 1, false, count());
}

bool Editor::w_vi_join() {
  s::size_t end = line_end(mBuf.cursor);
  if (end >= mBuf.text.size()) return false;
  s::size_t next = end + 1;
  while (next < mBuf.text.size() and (mBuf.text[next] == ' ' or mBuf.text[next] == '\t')) ++next;
  mBuf.replace(end, next - end, " ");
  mBuf.cursor = end;
  return true;
}

bool Editor::w_vi_goto_column() {
  s::size_t start = line_start(mBuf.cursor), end = line_end(mBuf.cursor);
  s::size_t pos = start;
  for (int i = 1; i < count() and pos < end; ++i) pos = next_char(mBuf.text, pos);
  mBuf.cursor = pos;
  return true;
}

bool Editor::w_expand_word() { return use_tab() and w_self_insert(); }

namespace {
// zsh's quotename: special characters with a backslash.
str backslash_quote(strv w) {
  str out;
  for (char c : w) {
    if (strv(" \t\n'\"\\$`|&;<>()*?[]#~={}^!").find(c) != strv::npos) out += '\\';
    out += c;
  }
  return out;
}
}  // namespace

// zsh's processcmd (run-help, which-command): the line goes on the buffer
// stack and the widget's own name runs with the command word, quoted.
bool Editor::process_cmd() {
  auto words = HistoryManager::split_words(mBuf.text);
  if (words.empty()) return false;
  mZle.buffer_stack.push_back(mBuf.text);
  mZle.stack_cs = static_cast<int>(mBuf.cursor) + (in_vicmd() ? 1 : 0);
  mBuf.text   = mWidget + " " + backslash_quote(words[0]);
  mBuf.cursor = mBuf.text.size();
  mDone = true;
  return true;
}

bool Editor::w_run_help()      { return process_cmd(); }
bool Editor::w_which_command() { return process_cmd(); }

// ---- Widgets zsh has that the others don't cover ------------------------------

bool Editor::w_reset_prompt() {
  reset_prompt();
  return true;
}

// zsh's deleteword and backwarddeleteword: as kill-word and
// backward-kill-word, without the kill ring.
bool Editor::w_delete_word() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_backward_delete_word(); });
  s::size_t x = mBuf.cursor;
  while (n-- > 0) x = word_end(x);
  mBuf.erase(mBuf.cursor, x - mBuf.cursor);
  return true;
}

bool Editor::w_backward_delete_word() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_delete_word(); });
  s::size_t x = prev_word_start_n(mBuf.cursor, n);
  mBuf.erase(x, mBuf.cursor - x);
  mBuf.cursor = x;
  return true;
}

// zsh's beginningoflinehist / endoflinehist: past the buffer's first or
// last line, the history line before or after.
bool Editor::w_beginning_of_line_hist() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_end_of_line_hist(); });
  strv t = mBuf.text;
  s::size_t& cs = mBuf.cursor;
  while (n) {
    if (cs == 0) break;
    if (t[cs - 1] == '\n') {
      --cs;
      if (cs == 0) break;
    }
    while (cs > 0 and t[cs - 1] != '\n') --cs;
    --n;
  }
  if (n) {
    opt<int> saved = mArg;
    mArg = n;
    bool ok = w_up_history();
    mArg = saved;
    mBuf.cursor = 0;
    return ok;
  }
  return true;
}

bool Editor::w_end_of_line_hist() {
  int n = count();
  if (n < 0) return negated(mArg, [&] { return w_beginning_of_line_hist(); });
  s::size_t& cs = mBuf.cursor;
  while (n) {
    strv t = mBuf.text;
    if (cs >= t.size()) {
      cs = t.size();
      break;
    }
    if (in_vicmd() and (cs = next_char(t, cs)) == t.size()) break;
    if (t[cs] == '\n' and ++cs == t.size()) break;
    while (cs != t.size() and t[cs] != '\n') ++cs;
    --n;
  }
  if (n) {
    opt<int> saved = mArg;
    mArg = n;
    bool ok = w_down_history();
    mArg = saved;
    return ok;
  }
  return true;
}

// zsh's uplineorsearch / downlineorsearch.
bool Editor::w_up_line_or_search() { return line_or(true, &Editor::w_history_search_backward); }

bool Editor::w_down_line_or_search() {
  return line_or(false, &Editor::w_history_search_forward);
}

bool Editor::w_vi_yank_eol() {
  s::size_t x = line_end(mBuf.cursor);
  if (x == mBuf.cursor) return false;
  cut_text(strv(mBuf.text).substr(mBuf.cursor, x - mBuf.cursor), CUT_YANK);
  return true;
}

bool Editor::w_vi_undo_change() { return w_undo(); }

// zsh's vipoundinsert: a `#` at the start of the cursor's line, or the one
// there taken away.
bool Editor::w_vi_pound_insert() {
  s::size_t oldcs = mBuf.cursor;
  w_vi_first_non_blank();
  s::size_t& cs = mBuf.cursor;
  if (cs >= mBuf.text.size() or mBuf.text[cs] != '#') {
    mBuf.insert_at(cs, "#");
    if (cs <= mViInsBegin) ++mViInsBegin;
    if (cs <= oldcs) ++oldcs;
  } else {
    mBuf.erase(cs, 1);
    if (cs < mViInsBegin) --mViInsBegin;
    if (cs < oldcs) --oldcs;
  }
  cs = s::min(oldcs, mBuf.text.size());
  return true;
}

// zsh's vicapslockpanic: a beep, and keys are read (and dropped) under
// a message until a lowercase one.
bool Editor::w_vi_caps_lock_panic() {
  beep();
  mMessage = "press a lowercase key to continue";
  refresh();
  for (;;) {
    str k;
    int c = read_byte(-1);
    if (c < 0) break;
    k += static_cast<char>(c);
    if (not read_rest_char(k)) continue;
    char32_t wc = char_at(k, 0);
    if (s::iswlower(static_cast<wint_t>(wc))) break;
  }
  mMessage.clear();
  return true;
}

// zsh's acceptandinfernexthistory: nothing to infer, nothing happens;
// else the line runs and the next prompt starts at the inferred entry.
bool Editor::w_accept_and_infer_next_history() {
  int i = infer_next(static_cast<int>(mHist.entries().size()));
  if (i < 0) return false;
  mZle.next_hist_index = i;
  mDone = true;
  return true;
}

bool Editor::w_describe_key_briefly() {
  if (not mMessage.empty()) return false;
  mMessage = "Describe key briefly: _";
  refresh();
  const KeyBinding* b = nullptr;
  str seq = read_binding(b);
  mMessage.clear();
  if (seq.empty()) return false;
  // A character's other bytes come with it (getrestchar), for a key that
  // inserts itself or has no binding.
  str w = b and not b->is_macro ? mZle.resolve_widget(b->widget) : str{};
  if (w.starts_with('.')) w.erase(0, 1);
  if ((w == "self-insert" or w == "self-insert-unmeta"
       or (not b and static_cast<unsigned char>(seq[0]) >= 0x80))
      and not read_rest_char(seq))
    return false;
  // bindztrdup: the keys (and a macro's text) quoted as bindkey lists them.
  str is = not b ? str("undefined-key") : b->is_macro ? format_key(b->macro) : b->widget;
  mMessage = format_key(seq) + " is " + is;
  return true;
}

bool Editor::w_where_is() {
  auto name = read_command_name("Where is: ");
  if (not name) return false;
  str msg = *name;
  int found = 0;
  for (const auto* kv : mKeymap->scan()) {
    const KeyBinding& b = kv->second;
    if (b.is_macro or b.widget != *name) continue;
    if (not found++) msg += " is on";
    if (found <= 4) msg += " " + format_key(kv->first);
  }
  if (not found)     msg += " is not bound to any key";
  else if (found > 4) msg += " et al";
  mMessage = msg;
  return true;
}

// zsh's endoflist: the listing shown stays on the screen, the prompt and
// line start again below it.
bool Editor::w_end_of_list() {
  if (mListing.empty() or not mDrawn) return false;
  // Below the line's last row: the message's rows, then the listing's
  // (refresh cuts each of their lines to one row).
  auto rows = [](const str& t) {
    return t.empty() ? 0 : 1 + static_cast<int>(s::count(t.begin(), t.end(), '\n'));
  };
  int end = mEndRow + rows(mMessage) + rows(mListing);
  str seq = "\r";
  for (int row = mCursorRow; row <= end; ++row) seq += '\n';
  write_out(seq);
  mListing.clear();
  mDrawn     = false;
  mCursorRow = 0;
  return true;
}

bool Editor::w_set_local_history() { return true; }

bool Editor::w_split_undo() { return true; }

bool Editor::w_recursive_edit() { return false; }

bool Editor::w_read_command() { return false; }

// zsh's expandcmdpath: the command word as its full path.
bool Editor::w_expand_cmd_path() {
  auto words = HistoryManager::split_words(mBuf.text);
  if (words.empty() or words[0].find('/') != str::npos) return false;
  str path = mZle.param("PATH");
  for (s::size_t i = 0; i <= path.size();) {
    s::size_t colon = path.find(':', i);
    if (colon == str::npos) colon = path.size();
    str dir = path.substr(i, colon - i);
    str full = (dir.empty() ? str(".") : dir) + "/" + words[0];
    if (::access(full.c_str(), X_OK) == 0) {
      s::size_t at = mBuf.text.find(words[0]);
      mBuf.replace(at, words[0].size(), full);
      if (mBuf.cursor > at) mBuf.cursor += full.size() - words[0].size();
      return true;
    }
    i = colon + 1;
  }
  return false;
}

// ---- The widget table ----------------------------------------------------------

const umap<str, Editor::WidgetInfo>& Editor::widget_table() {
  // KEEP and PEND mark zsh's ZLE_KEEPSUFFIX widgets (Src/Zle/iwidgets.list):
  // after PEND ones the suffix can still be removed.
  constexpr auto KEEP = SuffixRule::Keep;
  constexpr auto PEND = SuffixRule::Pending;
  static const umap<str, WidgetInfo> table = {
    {"where-is",                          &Editor::w_where_is},
    {"vi-yank-eol",                       &Editor::w_vi_yank_eol},
    {"vi-undo-change",                    {&Editor::w_vi_undo_change, KEEP}},
    {"vi-pound-insert",                   &Editor::w_vi_pound_insert},
    {"vi-caps-lock-panic",                &Editor::w_vi_caps_lock_panic},
    {"up-line-or-search",                 &Editor::w_up_line_or_search},
    {"split-undo",                        {&Editor::w_split_undo, PEND}},
    {"set-local-history",                 &Editor::w_set_local_history},
    {"reset-prompt",                      {&Editor::w_reset_prompt, PEND}},
    {"recursive-edit",                    &Editor::w_recursive_edit},
    {"read-command",                      &Editor::w_read_command},
    {"history-incremental-pattern-search-forward", &Editor::w_history_incremental_search_forward},
    {"history-incremental-pattern-search-backward", &Editor::w_history_incremental_search_backward},
    {"gosmacs-transpose-chars",           &Editor::w_gosmacs_transpose_chars},
    {"expand-cmd-path",                   &Editor::w_expand_cmd_path},
    {"end-of-list",                       &Editor::w_end_of_list},
    {"down-line-or-search",               &Editor::w_down_line_or_search},
    {"describe-key-briefly",              &Editor::w_describe_key_briefly},
    {"copy-prev-shell-word",              {&Editor::w_copy_prev_shell_word, KEEP}},
    {"argument-base",                     {&Editor::w_argument_base, PEND}},
    {"accept-search",                     &Editor::w_beep},
    {"accept-and-menu-complete",          &Editor::w_menu_complete},
    {"accept-and-infer-next-history",     &Editor::w_accept_and_infer_next_history},
    {"accept-and-hold",                     &Editor::w_accept_and_hold},
    {"accept-line",                         &Editor::w_accept_line},
    {"accept-line-and-down-history",        &Editor::w_accept_line_and_down_history},
    {"auto-suffix-remove",                  {&Editor::w_auto_suffix_remove, PEND}},
    {"auto-suffix-retain",                  {&Editor::w_auto_suffix_retain, KEEP}},
    {"backward-char",                       &Editor::w_backward_char},
    {"backward-delete-char",                {&Editor::w_backward_delete_char, KEEP}},
    {"backward-delete-word",                {&Editor::w_backward_delete_word, KEEP}},
    {"backward-kill-line",                  {&Editor::w_backward_kill_line, KEEP}},
    {"backward-kill-word",                  {&Editor::w_backward_kill_word, KEEP}},
    {"backward-word",                       &Editor::w_backward_word},
    {"beep",                                &Editor::w_beep},
    {"beginning-of-buffer-or-history",      &Editor::w_beginning_of_buffer_or_history},
    {"beginning-of-history",                &Editor::w_beginning_of_history},
    {"beginning-of-line",                   &Editor::w_beginning_of_line},
    {"beginning-of-line-hist",              &Editor::w_beginning_of_line_hist},
    {"bracketed-paste",                     {&Editor::w_bracketed_paste, KEEP}},
    {"capitalize-word",                     &Editor::w_capitalize_word},
    {"clear-screen",                        {&Editor::w_clear_screen, PEND}},
    {"complete-word",                       &Editor::w_complete_word},
    {"copy-prev-word",                      {&Editor::w_copy_prev_word, KEEP}},
    {"copy-region-as-kill",                 {&Editor::w_copy_region_as_kill, KEEP}},
    {"deactivate-region",                   &Editor::w_deactivate_region},
    {"delete-char",                         {&Editor::w_delete_char, KEEP}},
    {"delete-char-or-list",                 &Editor::w_delete_char_or_list},
    {"delete-word",                         {&Editor::w_delete_word, KEEP}},
    {"digit-argument",                      {&Editor::w_digit_argument, PEND}},
    {"down-case-word",                      &Editor::w_down_case_word},
    {"down-history",                        &Editor::w_down_history},
    {"down-line",                           &Editor::w_down_line},
    {"down-line-or-history",                &Editor::w_down_line_or_history},
    {"edit-command-line",                   &Editor::w_edit_command_line},
    {"emacs-backward-word",                 &Editor::w_emacs_backward_word},
    {"emacs-forward-word",                  &Editor::w_emacs_forward_word},
    {"end-of-buffer-or-history",            &Editor::w_end_of_buffer_or_history},
    {"end-of-history",                      &Editor::w_end_of_history},
    {"end-of-line",                         &Editor::w_end_of_line},
    {"end-of-line-hist",                    &Editor::w_end_of_line_hist},
    {"exchange-point-and-mark",             &Editor::w_exchange_point_and_mark},
    {"execute-last-named-cmd",              &Editor::w_execute_last_named_cmd},
    {"execute-named-cmd",                   &Editor::w_execute_named_cmd},
    {"expand-history",                      &Editor::w_expand_history},
    {"expand-or-complete",                  &Editor::w_expand_or_complete},
    {"expand-or-complete-prefix",           &Editor::w_expand_or_complete},
    {"expand-word",                         &Editor::w_expand_word},
    {"forward-char",                        &Editor::w_forward_char},
    {"forward-word",                        &Editor::w_forward_word},
    {"get-line",                            &Editor::w_get_line},
    {"history-beginning-search-backward",   &Editor::w_history_beginning_search_backward},
    {"history-beginning-search-forward",    &Editor::w_history_beginning_search_forward},
    {"history-incremental-search-backward", &Editor::w_history_incremental_search_backward},
    {"history-incremental-search-forward",  &Editor::w_history_incremental_search_forward},
    {"history-search-backward",             &Editor::w_history_search_backward},
    {"history-search-forward",              &Editor::w_history_search_forward},
    {"infer-next-history",                  &Editor::w_infer_next_history},
    {"insert-last-word",                    {&Editor::w_insert_last_word, KEEP}},
    {"kill-buffer",                         {&Editor::w_kill_buffer, KEEP}},
    {"kill-line",                           {&Editor::w_kill_line, KEEP}},
    {"kill-region",                         {&Editor::w_kill_region, KEEP}},
    {"kill-whole-line",                     {&Editor::w_kill_whole_line, KEEP}},
    {"kill-word",                           {&Editor::w_kill_word, KEEP}},
    {"list-choices",                        &Editor::w_list_choices},
    {"list-expand",                         &Editor::w_expand_word},
    {"magic-space",                         &Editor::w_magic_space},
    {"menu-complete",                       &Editor::w_menu_complete},
    {"menu-expand-or-complete",             &Editor::w_menu_complete},
    {"neg-argument",                        {&Editor::w_neg_argument, PEND}},
    {"overwrite-mode",                      &Editor::w_overwrite_mode},
    {"pound-insert",                        &Editor::w_pound_insert},
    {"push-input",                          &Editor::w_push_line},
    {"push-line",                           &Editor::w_push_line},
    {"push-line-or-edit",                   &Editor::w_push_line},
    {"put-replace-selection",               {&Editor::w_put_replace_selection, KEEP}},
    {"quote-line",                          &Editor::w_quote_line},
    {"quote-region",                        &Editor::w_quote_region},
    {"quoted-insert",                       {&Editor::w_quoted_insert, KEEP}},
    {"redisplay",                           {&Editor::w_redisplay, PEND}},
    {"redo",                                {&Editor::w_redo, KEEP}},
    {"reverse-menu-complete",               &Editor::w_reverse_menu_complete},
    {"run-help",                            &Editor::w_run_help},
    {"select-a-blank-word",                 {&Editor::w_select_a_blank_word, KEEP}},
    {"select-a-bracketed",                  {&Editor::w_select_a_bracketed, KEEP}},
    {"select-a-quoted",                     {&Editor::w_select_a_quoted, KEEP}},
    {"select-a-shell-word",                 {&Editor::w_select_a_shell_word, KEEP}},
    {"select-a-word",                       {&Editor::w_select_a_word, KEEP}},
    {"select-in-blank-word",                {&Editor::w_select_in_blank_word, KEEP}},
    {"select-in-bracketed",                 {&Editor::w_select_in_bracketed, KEEP}},
    {"select-in-quoted",                    {&Editor::w_select_in_quoted, KEEP}},
    {"select-in-shell-word",                {&Editor::w_select_in_shell_word, KEEP}},
    {"select-in-word",                      {&Editor::w_select_in_word, KEEP}},
    {"self-insert",                         &Editor::w_self_insert},
    {"self-insert-unmeta",                  {&Editor::w_self_insert_unmeta, KEEP}},
    {"send-break",                          &Editor::w_send_break},
    {"set-mark-command",                    {&Editor::w_set_mark_command, PEND}},
    {"spell-word",                          &Editor::w_expand_word},
    {"transpose-chars",                     &Editor::w_transpose_chars},
    {"transpose-words",                     &Editor::w_transpose_words},
    {"undefined-key",                       &Editor::w_undefined_key},
    {"undo",                                &Editor::w_undo},
    {"universal-argument",                  {&Editor::w_universal_argument, PEND}},
    {"up-case-word",                        &Editor::w_up_case_word},
    {"up-history",                          &Editor::w_up_history},
    {"up-line",                             &Editor::w_up_line},
    {"up-line-or-history",                  &Editor::w_up_line_or_history},
    {"vi-add-eol",                          &Editor::w_vi_add_eol},
    {"vi-add-next",                         &Editor::w_vi_add_next},
    {"vi-backward-blank-word",              &Editor::w_vi_backward_blank_word},
    {"vi-backward-blank-word-end",          &Editor::w_vi_backward_blank_word_end},
    {"vi-backward-char",                    &Editor::w_vi_backward_char},
    {"vi-backward-delete-char",             {&Editor::w_vi_backward_delete_char, KEEP}},
    {"vi-backward-kill-word",               {&Editor::w_vi_backward_kill_word, KEEP}},
    {"vi-backward-word",                    &Editor::w_vi_backward_word},
    {"vi-backward-word-end",                &Editor::w_vi_backward_word_end},
    {"vi-beginning-of-line",                &Editor::w_vi_beginning_of_line},
    {"vi-change",                           &Editor::w_vi_change},
    {"vi-change-eol",                       &Editor::w_vi_change_eol},
    {"vi-change-whole-line",                &Editor::w_vi_change_whole_line},
    {"vi-cmd-mode",                         &Editor::w_vi_cmd_mode},
    {"vi-delete",                           {&Editor::w_vi_delete, KEEP}},
    {"vi-delete-char",                      {&Editor::w_vi_delete_char, KEEP}},
    {"vi-digit-or-beginning-of-line",       &Editor::w_vi_digit_or_beginning_of_line},
    {"vi-down-case",                        &Editor::w_vi_down_case},
    {"vi-down-line-or-history",             &Editor::w_vi_down_line_or_history},
    {"vi-end-of-line",                      &Editor::w_vi_end_of_line},
    {"vi-fetch-history",                    &Editor::w_vi_fetch_history},
    {"vi-find-next-char",                   &Editor::w_vi_find_next_char},
    {"vi-find-next-char-skip",              &Editor::w_vi_find_next_char_skip},
    {"vi-find-prev-char",                   &Editor::w_vi_find_prev_char},
    {"vi-find-prev-char-skip",              &Editor::w_vi_find_prev_char_skip},
    {"vi-first-non-blank",                  &Editor::w_vi_first_non_blank},
    {"vi-forward-blank-word",               &Editor::w_vi_forward_blank_word},
    {"vi-forward-blank-word-end",           &Editor::w_vi_forward_blank_word_end},
    {"vi-forward-char",                     &Editor::w_vi_forward_char},
    {"vi-forward-word",                     &Editor::w_vi_forward_word},
    {"vi-forward-word-end",                 &Editor::w_vi_forward_word_end},
    {"vi-goto-column",                      &Editor::w_vi_goto_column},
    {"vi-goto-mark",                        &Editor::w_vi_goto_mark},
    {"vi-goto-mark-line",                   &Editor::w_vi_goto_mark_line},
    {"vi-history-search-backward",          &Editor::w_vi_history_search_backward},
    {"vi-history-search-forward",           &Editor::w_vi_history_search_forward},
    {"vi-indent",                           &Editor::w_vi_indent},
    {"vi-insert",                           &Editor::w_vi_insert},
    {"vi-insert-bol",                       &Editor::w_vi_insert_bol},
    {"vi-join",                             &Editor::w_vi_join},
    {"vi-kill-eol",                         {&Editor::w_vi_kill_eol, KEEP}},
    {"vi-kill-line",                        {&Editor::w_vi_kill_line, KEEP}},
    {"vi-match-bracket",                    &Editor::w_vi_match_bracket},
    {"vi-open-line-above",                  &Editor::w_vi_open_line_above},
    {"vi-open-line-below",                  &Editor::w_vi_open_line_below},
    {"vi-oper-swap-case",                   &Editor::w_vi_oper_swap_case},
    {"vi-put-after",                        {&Editor::w_vi_put_after, KEEP}},
    {"vi-put-before",                       {&Editor::w_vi_put_before, KEEP}},
    {"vi-quoted-insert",                    {&Editor::w_vi_quoted_insert, KEEP}},
    {"vi-repeat-change",                    &Editor::w_vi_repeat_change},
    {"vi-repeat-find",                      &Editor::w_vi_repeat_find},
    {"vi-repeat-search",                    &Editor::w_vi_repeat_search},
    {"vi-replace",                          &Editor::w_vi_replace},
    {"vi-replace-chars",                    &Editor::w_vi_replace_chars},
    {"vi-rev-repeat-find",                  &Editor::w_vi_rev_repeat_find},
    {"vi-rev-repeat-search",                &Editor::w_vi_rev_repeat_search},
    {"vi-set-buffer",                       {&Editor::w_vi_set_buffer, PEND}},
    {"vi-set-mark",                         {&Editor::w_vi_set_mark, PEND}},
    {"vi-substitute",                       &Editor::w_vi_substitute},
    {"vi-swap-case",                        &Editor::w_vi_swap_case},
    {"vi-unindent",                         &Editor::w_vi_unindent},
    {"vi-up-case",                          &Editor::w_vi_up_case},
    {"vi-up-line-or-history",               &Editor::w_vi_up_line_or_history},
    {"vi-yank",                             &Editor::w_vi_yank},
    {"vi-yank-whole-line",                  &Editor::w_vi_yank_whole_line},
    {"visual-line-mode",                    &Editor::w_visual_line_mode},
    {"visual-mode",                         &Editor::w_visual_mode},
    {"what-cursor-position",                {&Editor::w_what_cursor_position, PEND}},
    {"which-command",                       &Editor::w_which_command},
    {"yank",                                {&Editor::w_yank, KEEP}},
    {"yank-pop",                            {&Editor::w_yank_pop, KEEP}},
  };
  return table;
}

vec<str> Editor::builtin_widget_names() {
  vec<str> names;
  for (const auto& [name, fn] : widget_table()) names.push_back(name);
  return names;
}

}  // namespace jfsh
