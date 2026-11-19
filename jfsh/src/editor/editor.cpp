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
  if (mOpts.get(OPT_VI)) mZle.select_main(true);
}

Editor::~Editor() {
  if (mRawModeActive) exit_raw_mode();
  if (mZle.active == this) mZle.active = nullptr;
}

EditorMode Editor::mode() const {
  return mZle.keymap("main") == mZle.keymap("viins") ? EditorMode::Vi : EditorMode::Emacs;
}

void Editor::enter_raw_mode() {
  tcgetattr(mIn, &OrigTermios);
  struct termios raw = OrigTermios;
  raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | ISIG | IEXTEN));
  raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | BRKINT | INPCK | ISTRIP));
  raw.c_cflag |= CS8;
  raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
  raw.c_cc[VMIN]  = 1;
  raw.c_cc[VTIME] = 0;
  // TCSADRAIN, not TCSAFLUSH: input typed or pasted ahead (the next lines
  // of a multi-line command) must survive the switch.
  tcsetattr(mIn, TCSADRAIN, &raw);
  mRawModeActive = true;
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
  if (not continuation) {
    if (mZle.next_buffer) {
      initial = s::move(*mZle.next_buffer);
      mZle.next_buffer.reset();
    } else if (not mZle.buffer_stack.empty()) {
      initial = s::move(mZle.buffer_stack.back());
      mZle.buffer_stack.pop_back();
    }
  }
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
  mHistIndex  = static_cast<int>(mHist.entries().size());
  mHistEdits.clear();
  mArg.reset();
  mUndo.clear();
  mRedo.clear();
  mUndoGroup    = 0;
  mMark.reset();
  mRegionActive = false;
  mMessage.clear();
  mListing.clear();
  mMenuActive   = false;
  mListed       = false;
  mInterrupted  = false;
  mEof          = false;
  mDone         = false;
  mDrawn        = false;
  mCursorRow    = 0;
  mLastWidget.clear();
  mLocalKeymap.reset();
  mViRegister   = 0;
  mViRecord     = false;
  mKeymap = mZle.keymap("main");
  if (not mKeymap) mKeymap = mZle.keymap("emacs");
  if (mZle.next_hist_index >= 0 and not mVared and not mContinuation) {
    int idx = mZle.next_hist_index;
    mZle.next_hist_index = -1;
    if (idx < static_cast<int>(mHist.entries().size())) set_history_line(idx);
  }

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
    return mBuf.text + line;
  }

  Editor* outer = mZle.active;
  mZle.active = this;
  mReading = true;
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
      mInterrupted = true;
      mDone = true;
      break;
    }
    // Ctrl-D (VEOF) on an empty line is EOF, whatever it is bound to.
    if (seq == "\x04" and mBuf.text.empty() and not mVared) {
      if (mOpts.get(OPT_IGNOREEOF)) {
        mMessage = "jfsh: use 'exit' to exit.";
        refresh();
        continue;
      }
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
      run_widget(b->widget);
    } else if (static_cast<unsigned char>(seq[0]) >= 0x80 and mKeymap->insert_unbound) {
      run_widget("self-insert");
    } else {
      beep();
      mArg.reset();
    }
    if (mViRecord and not mViReplaying and in_vicmd()) {
      // A count or register still waiting for its command.
      bool prefix_only = mArg.has_value() or mViRegister != 0;
      if (not prefix_only or mBuf.text != text_before) {
        if (mBuf.text != text_before and mLastWidget != "undo" and mLastWidget != "redo"
            and mLastWidget != "vi-repeat-change")
          mViLastChange = mViRecording;
        mViRecord = false;
      }
    } else if (mViRecord and not in_vicmd() and mKeymap != mZle.keymap("viins")) {
      mViRecord = false;
    }
    if (not mDone) refresh();
  }

  if (paste) write_out("\x1b[?2004l");
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
        mEof = true;
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
  str kt = mZle.param("KEYTIMEOUT");
  int keytimeout = kt.empty() ? 400 : s::max(0, s::atoi(kt.c_str())) * 10;

  str seq;
  const KeyBinding* match = nullptr;
  s::size_t match_len = 0;
  for (;;) {
    // Wait as long as it takes for the first key and for the rest of a
    // sequence nothing matches yet; KEYTIMEOUT when a shorter one does.
    int timeout = seq.empty() or not match ? -1 : keytimeout;
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
    beep();
    mArg.reset();
    return false;
  }
  str saved_widget = mWidget;
  mWidget = name;
  Snapshot before{mBuf.text, mBuf.cursor};
  mArgSet   = false;
  mKilledNow = false;
  bool ok = dispatch(w);
  if (not ok and not mDone) beep();
  str base = w.starts_with('.') ? w.substr(1) : w;
  if (base != "undo" and base != "redo" and before.text != mBuf.text) {
    bool merge = (mUndoGroup == 2)
              or (mUndoGroup == 0 and base == "self-insert" and mLastWidget == "self-insert"
                  and not mUndo.empty());
    if (not merge) mUndo.push_back(s::move(before));
    if (mUndoGroup == 1) mUndoGroup = 2;
    mRedo.clear();
  }
  if (not mArgSet) mArg.reset();
  if (in_vicmd() and not mViOperating) vi_clamp_cursor();
  mLastWasKill = mKilledNow;
  static const uset<str> completion_widgets = {
    "expand-or-complete", "complete-word", "menu-complete", "reverse-menu-complete",
    "list-choices", "delete-char-or-list"};
  if (not completion_widgets.contains(base)) {
    mMenuActive = false;
    mListed     = false;
  }
  mLastWidget = name;
  mWidget     = saved_widget.empty() ? name : saved_widget;
  return ok;
}

bool Editor::dispatch(const str& name) {
  if (not name.starts_with('.')) {
    if (auto u = mZle.user_widgets.find(name); u != mZle.user_widgets.end()) {
      if (not mZle.hooks.run_widget) return false;
      int rc = mZle.hooks.run_widget(*this, name, u->second);
      mBuf.cursor = s::min(mBuf.cursor, mBuf.text.size());
      while (mBuf.cursor > 0 and mBuf.cursor < mBuf.text.size()
             and (static_cast<unsigned char>(mBuf.text[mBuf.cursor]) & 0xc0) == 0x80)
        --mBuf.cursor;
      return rc == 0;
    }
  }
  strv base = name.starts_with('.') ? strv(name).substr(1) : strv(name);
  if (auto m = mZle.module_widgets.find(str(base)); m != mZle.module_widgets.end())
    return m->second(*this);
  const auto& table = Editor::widget_table();
  auto it = table.find(str(base));
  if (it == table.end()) return false;
  return (this->*(it->second))();
}

bool Editor::call_widget(strv name) {
  if (not mReading) return false;
  return run_widget(str(name));
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

bool Editor::select_keymap(strv name) {
  auto km = mZle.keymap(name);
  if (not km) return false;
  mKeymap = km;
  mLocalKeymap.reset();
  return true;
}

str Editor::keymap_name() const {
  return mKeymap ? mZle.keymap_name(mKeymap) : str("main");
}

int Editor::hist_number() const {
  const auto& e = mHist.entries();
  if (mHistIndex < static_cast<int>(e.size())) return static_cast<int>(e[static_cast<s::size_t>(mHistIndex)].num);
  return static_cast<int>(mHist.last_num()) + 1;
}

void Editor::set_cut_buffer(str s) {
  if (mKillRing.empty()) mKillRing.push_back(s::move(s));
  else                   mKillRing.front() = s::move(s);
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

s::size_t Editor::line_start(s::size_t pos) const {
  auto nl = pos == 0 ? str::npos : mBuf.text.rfind('\n', pos - 1);
  return nl == str::npos ? 0 : nl + 1;
}

s::size_t Editor::line_end(s::size_t pos) const {
  auto nl = mBuf.text.find('\n', pos);
  return nl == str::npos ? mBuf.text.size() : nl;
}

void Editor::push_kill(str text, bool backward) {
  if (text.empty()) return;
  if (mLastWasKill and not mKillRing.empty()) {
    if (backward) mKillRing.front().insert(0, text);
    else          mKillRing.front() += text;
  } else {
    mKillRing.insert(mKillRing.begin(), s::move(text));
    if (mKillRing.size() > 8) mKillRing.pop_back();
  }
  mKilledNow = true;
}

void Editor::kill_text(s::size_t from, s::size_t to, bool backward) {
  if (from > to) s::swap(from, to);
  to = s::min(to, mBuf.text.size());
  push_kill(mBuf.text.substr(from, to - from), backward);
  mBuf.text.erase(from, to - from);
  mBuf.cursor = from;
}

// ---- Accepting and inserting -------------------------------------------------

bool Editor::w_accept_line() {
  mDone = true;
  return true;
}

bool Editor::w_accept_and_hold() {
  mZle.next_buffer = mBuf.text;
  mDone = true;
  return true;
}

bool Editor::w_accept_line_and_down_history() {
  int size = static_cast<int>(mHist.entries().size());
  if (mHistIndex < size) mZle.next_hist_index = mHistIndex + 1;
  mDone = true;
  return true;
}

bool Editor::w_send_break() {
  mInterrupted = true;
  mDone = true;
  return true;
}

bool Editor::w_self_insert() {
  str keys = mKeys;
  if (keys.empty()) return false;
  // A meta key bound to self-insert inserts the key without ESC.
  if (keys.size() > 1 and keys[0] == '\x1b') keys.erase(0, 1);
  for (int n = s::max(1, count()); n > 0; --n) {
    if (mBuf.overwrite and mBuf.cursor < mBuf.text.size() and mBuf.text[mBuf.cursor] != '\n') {
      s::size_t next = next_char(mBuf.text, mBuf.cursor);
      mBuf.text.replace(mBuf.cursor, next - mBuf.cursor, keys);
      mBuf.cursor += keys.size();
    } else {
      mBuf.insert(keys);
    }
  }
  return true;
}

bool Editor::w_self_insert_unmeta() {
  str keys = mKeys;
  if (not keys.empty() and keys[0] == '\x1b') keys.erase(0, 1);
  for (char& c : keys) {
    c = static_cast<char>(static_cast<unsigned char>(c) & 0x7f);
    if (c == '\r') c = '\n';
  }
  mBuf.insert(keys);
  return true;
}

bool Editor::w_quoted_insert() {
  int c = read_byte(-1);
  if (c < 0) return false;
  str ch(1, static_cast<char>(c));
  unsigned char lead = static_cast<unsigned char>(c);
  s::size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
  while (ch.size() < need) {
    int more = read_byte(50);
    if (more < 0) break;
    ch += static_cast<char>(more);
  }
  for (int n = s::max(1, count()); n > 0; --n) mBuf.insert(ch);
  return true;
}

bool Editor::w_bracketed_paste() {
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
  mBuf.insert(clean);
  return true;
}

// ---- Movement ----------------------------------------------------------------

bool Editor::w_forward_char() {
  if (mBuf.cursor >= mBuf.text.size()) return false;
  for (int n = count(); n > 0 and mBuf.cursor < mBuf.text.size(); --n)
    mBuf.cursor = next_char(mBuf.text, mBuf.cursor);
  return true;
}

bool Editor::w_backward_char() {
  if (mBuf.cursor == 0) return false;
  for (int n = count(); n > 0 and mBuf.cursor > 0; --n)
    mBuf.cursor = prev_char(mBuf.text, mBuf.cursor);
  return true;
}

bool Editor::w_forward_word() {
  for (int n = count(); n > 0; --n) mBuf.cursor = next_word_start(mBuf.cursor);
  return true;
}

bool Editor::w_backward_word() {
  for (int n = count(); n > 0; --n) mBuf.cursor = prev_word_start(mBuf.cursor);
  return true;
}

bool Editor::w_emacs_forward_word() {
  for (int n = count(); n > 0; --n) mBuf.cursor = word_end(mBuf.cursor);
  return true;
}

bool Editor::w_emacs_backward_word() { return w_backward_word(); }

bool Editor::w_beginning_of_line() {
  s::size_t start = line_start(mBuf.cursor);
  // On a line's start already: the start of the line before.
  if (mBuf.cursor == start and start > 0 and count() > 1) start = line_start(start - 1);
  mBuf.cursor = start;
  return true;
}

bool Editor::w_end_of_line() {
  mBuf.cursor = line_end(mBuf.cursor);
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
  if (mBuf.cursor == 0) return false;
  for (int n = count(); n > 0 and mBuf.cursor > 0; --n) {
    s::size_t prev = prev_char(mBuf.text, mBuf.cursor);
    mBuf.text.erase(prev, mBuf.cursor - prev);
    mBuf.cursor = prev;
  }
  return true;
}

bool Editor::w_delete_char() {
  if (mBuf.cursor >= mBuf.text.size()) return false;
  for (int n = count(); n > 0 and mBuf.cursor < mBuf.text.size(); --n) {
    s::size_t next = next_char(mBuf.text, mBuf.cursor);
    mBuf.text.erase(mBuf.cursor, next - mBuf.cursor);
  }
  return true;
}

bool Editor::w_delete_char_or_list() {
  if (mBuf.cursor >= mBuf.text.size()) return w_list_choices();
  return w_delete_char();
}

bool Editor::w_backward_kill_word() {
  s::size_t start = mBuf.cursor;
  for (int n = count(); n > 0; --n) start = prev_word_start(start);
  if (start == mBuf.cursor) return false;
  kill_text(start, mBuf.cursor, true);
  return true;
}

bool Editor::w_kill_word() {
  s::size_t end = mBuf.cursor;
  for (int n = count(); n > 0; --n) end = word_end(end);
  if (end == mBuf.cursor) return false;
  kill_text(mBuf.cursor, end, false);
  return true;
}

bool Editor::w_kill_line() {
  s::size_t end = line_end(mBuf.cursor);
  if (end == mBuf.cursor) {
    if (end >= mBuf.text.size()) return false;
    end += 1;   // at the end of a line: the newline
  }
  kill_text(mBuf.cursor, end, false);
  return true;
}

bool Editor::w_backward_kill_line() {
  s::size_t start = line_start(mBuf.cursor);
  if (start == mBuf.cursor) {
    if (start == 0) return false;
    start -= 1;
  }
  kill_text(start, mBuf.cursor, true);
  return true;
}

bool Editor::w_kill_whole_line() {
  if (mBuf.text.empty()) return true;
  s::size_t start = line_start(mBuf.cursor);
  s::size_t end   = line_end(mBuf.cursor);
  if (end < mBuf.text.size()) ++end;
  else if (start > 0) --start;
  kill_text(start, end, false);
  return true;
}

bool Editor::w_kill_buffer() {
  kill_text(0, mBuf.text.size(), false);
  return true;
}

bool Editor::w_kill_region() {
  if (not mMark) return false;
  kill_text(*mMark, mBuf.cursor, mBuf.cursor < *mMark);
  mRegionActive = false;
  return true;
}

bool Editor::w_copy_region_as_kill() {
  if (not mMark) return false;
  s::size_t a = s::min(*mMark, mBuf.cursor), b = s::max(*mMark, mBuf.cursor);
  b = s::min(b, mBuf.text.size());
  push_kill(mBuf.text.substr(a, b - a), false);
  mRegionActive = false;
  return true;
}

bool Editor::w_yank() {
  if (mKillRing.empty()) return false;
  mYankIndex = 0;
  mYankStart = mBuf.cursor;
  for (int n = count(); n > 0; --n) mBuf.insert(mKillRing.front());
  mYankEnd = mBuf.cursor;
  return true;
}

bool Editor::w_yank_pop() {
  if ((mLastWidget != "yank" and mLastWidget != "yank-pop") or mKillRing.empty()) return false;
  mYankIndex = (mYankIndex + 1) % static_cast<int>(mKillRing.size());
  const str& text = mKillRing[static_cast<s::size_t>(mYankIndex)];
  mBuf.text.replace(mYankStart, mYankEnd - mYankStart, text);
  mYankEnd    = mYankStart + text.size();
  mBuf.cursor = mYankEnd;
  return true;
}

// ---- Transposing and case ------------------------------------------------------

bool Editor::w_transpose_chars() {
  strv t = mBuf.text;
  s::size_t cur = mBuf.cursor;
  if (cur == 0 or t.size() < 2) return false;
  // At the end of a line, the two characters before the cursor.
  if (cur >= t.size() or t[cur] == '\n') cur = prev_char(t, cur);
  if (cur == 0) return false;
  s::size_t a = prev_char(t, cur), b = next_char(t, cur);
  str first(t.substr(a, cur - a)), second(t.substr(cur, b - cur));
  mBuf.text.replace(a, b - a, second + first);
  mBuf.cursor = b;
  return true;
}

bool Editor::w_transpose_words() {
  strv t = mBuf.text;
  // The word at or before the cursor, and the one before that.
  s::size_t e2 = mBuf.cursor;
  while (e2 < t.size() and is_word_char(char_at(t, e2))) e2 = next_char(t, e2);
  s::size_t s2 = prev_word_start(e2);
  if (s2 == e2) return false;
  s::size_t s1 = prev_word_start(s2);
  if (s1 == s2) return false;
  s::size_t e1 = s1;
  while (e1 < s2 and is_word_char(char_at(t, e1))) e1 = next_char(t, e1);
  str w1(t.substr(s1, e1 - s1)), mid(t.substr(e1, s2 - e1)), w2(t.substr(s2, e2 - s2));
  mBuf.text.replace(s1, e2 - s1, w2 + mid + w1);
  mBuf.cursor = e2;
  return true;
}

bool Editor::w_capitalize_word() {
  for (int n = count(); n > 0; --n) {
    s::size_t pos = mBuf.cursor;
    while (pos < mBuf.text.size() and not is_word_char(char_at(mBuf.text, pos))) pos = next_char(mBuf.text, pos);
    s::size_t end = pos;
    while (end < mBuf.text.size() and is_word_char(char_at(mBuf.text, end))) end = next_char(mBuf.text, end);
    if (pos < end) {
      s::size_t second = next_char(mBuf.text, pos);
      map_chars(mBuf.text, second, end, to_lower);
      map_chars(mBuf.text, pos, second, to_upper);
      end = pos;
      while (end < mBuf.text.size() and is_word_char(char_at(mBuf.text, end))) end = next_char(mBuf.text, end);
    }
    mBuf.cursor = end;
  }
  return true;
}

bool Editor::w_up_case_word() {
  for (int n = count(); n > 0; --n) {
    s::size_t end = word_end(mBuf.cursor);
    map_chars(mBuf.text, mBuf.cursor, end, to_upper);
    mBuf.cursor = word_end(mBuf.cursor);
  }
  return true;
}

bool Editor::w_down_case_word() {
  for (int n = count(); n > 0; --n) {
    s::size_t end = word_end(mBuf.cursor);
    map_chars(mBuf.text, mBuf.cursor, end, to_lower);
    mBuf.cursor = word_end(mBuf.cursor);
  }
  return true;
}

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

bool Editor::w_exchange_point_and_mark() {
  if (not mMark) return false;
  s::size_t m = s::min(*mMark, mBuf.text.size());
  mMark = mBuf.cursor;
  mBuf.cursor = m;
  if (not (mArg and *mArg == 0)) mRegionActive = true;
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

bool Editor::w_undo() {
  if (mUndo.empty()) return false;
  mRedo.push_back({mBuf.text, mBuf.cursor});
  mBuf.text   = s::move(mUndo.back().text);
  mBuf.cursor = s::min(mUndo.back().cursor, mBuf.text.size());
  mUndo.pop_back();
  mUndoGroup = 0;
  return true;
}

bool Editor::w_redo() {
  if (mRedo.empty()) return false;
  mUndo.push_back({mBuf.text, mBuf.cursor});
  mBuf.text   = s::move(mRedo.back().text);
  mBuf.cursor = s::min(mRedo.back().cursor, mBuf.text.size());
  mRedo.pop_back();
  return true;
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

bool Editor::w_digit_argument() {
  char d = mKeys.empty() ? '0' : mKeys.back();
  if (d < '0' or d > '9') return false;
  int v = d - '0';
  bool continuing = mLastWidget == "digit-argument" or mLastWidget == "neg-argument"
                 or mLastWidget == "universal-argument";
  if (continuing and mArg) {
    bool neg = *mArg < 0 or mArgNeg;
    int base = mArgNeg ? 0 : (*mArg < 0 ? -*mArg : *mArg);
    if (mLastWidget == "universal-argument") base = 0;
    int n = base * 10 + v;
    mArg = neg ? -n : n;
  } else {
    mArg = v;
  }
  mArgNeg = false;
  mArgSet = true;
  return true;
}

bool Editor::w_neg_argument() {
  mArg = -1;
  mArgNeg = true;
  mArgSet = true;
  return true;
}

bool Editor::w_universal_argument() {
  mArg = (mArg ? *mArg : 1) * 4;
  mArgNeg = false;
  mArgSet = true;
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

bool Editor::w_up_line() {
  s::size_t start = line_start(mBuf.cursor);
  if (start == 0) return false;
  int col = static_cast<int>(char_count(strv(mBuf.text).substr(start, mBuf.cursor - start)));
  s::size_t pstart = line_start(start - 1);
  s::size_t pend   = start - 1;
  s::size_t pos = pstart;
  for (int i = 0; i < col and pos < pend; ++i) pos = next_char(mBuf.text, pos);
  mBuf.cursor = s::min(pos, pend);
  return true;
}

bool Editor::w_down_line() {
  s::size_t end = line_end(mBuf.cursor);
  if (end >= mBuf.text.size()) return false;
  s::size_t start = line_start(mBuf.cursor);
  int col = static_cast<int>(char_count(strv(mBuf.text).substr(start, mBuf.cursor - start)));
  s::size_t nstart = end + 1;
  s::size_t nend   = line_end(nstart);
  s::size_t pos = nstart;
  for (int i = 0; i < col and pos < nend; ++i) pos = next_char(mBuf.text, pos);
  mBuf.cursor = s::min(pos, nend);
  return true;
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

bool Editor::w_up_line_or_history() {
  if (line_start(mBuf.cursor) > 0) return w_up_line();
  bool ok = w_up_history();
  return ok;
}

bool Editor::w_down_line_or_history() {
  if (line_end(mBuf.cursor) < mBuf.text.size()) return w_down_line();
  return w_down_history();
}

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
  struct State { int index; s::size_t pos; str pattern; bool failed; };
  int  orig_index  = mHistIndex;
  str  orig_text   = mBuf.text;
  s::size_t orig_cursor = mBuf.cursor;
  State st{mHistIndex, mBuf.cursor, {}, false};
  vec<State> stack;
  bool nodups = mOpts.get(OPT_HISTFINDNODUPS);

  auto line_text = [&](int i) -> str {
    if (i == orig_index) return orig_text;
    if (auto it = mHistEdits.find(i); it != mHistEdits.end()) return it->second;
    return i < size ? entries[static_cast<s::size_t>(i)].text : str{};
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
          at = t.rfind(s.pattern, s::min(from, t.size()));
        }
      } else {
        s::size_t from = skip and i == s.index ? pos + 1 : pos;
        if (from <= t.size()) at = t.find(s.pattern, from);
      }
      bool dup = nodups and i != s.index and t == last;
      if (at != str::npos and not dup) {
        s.index = i;
        s.pos   = at;
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
    mBuf.cursor = s::min(s.pos, mBuf.text.size());
  };

  for (;;) {
    mMessage = str(st.failed ? "failing " : "") + (backward ? "bck-i-search: " : "fwd-i-search: ")
             + st.pattern + "_";
    refresh();
    const KeyBinding* b = nullptr;
    str seq = read_binding(b);
    if (mEof) return false;
    if (seq == "\x03") {
      mMessage.clear();
      mInterrupted = true;
      mDone = true;
      return true;
    }
    str w = b and not b->is_macro ? mZle.resolve_widget(b->widget) : str{};
    if (w.starts_with('.')) w.erase(0, 1);
    bool insert = w == "self-insert" or w == "self-insert-unmeta"
               or (not b and not seq.empty() and static_cast<unsigned char>(seq[0]) >= 0x80);
    if (insert) {
      stack.push_back(st);
      st.pattern += seq;
      if (not st.failed) {
        State next = st;
        if (find(next, false)) {
          st = next;
          show(st);
        } else {
          st.failed = true;
        }
      }
    } else if (w == "backward-delete-char" or w == "vi-backward-delete-char") {
      if (not stack.empty()) {
        st = stack.back();
        stack.pop_back();
        show(st);
      } else {
        beep();
      }
    } else if (w == "history-incremental-search-backward" or w == "history-incremental-search-forward") {
      bool dir_back = w == "history-incremental-search-backward";
      if (st.pattern.empty() and not mLastIsearch.empty()) {
        backward = dir_back;
        st.pattern = mLastIsearch;
        State next = st;
        if (find(next, false)) { st = next; show(st); } else st.failed = true;
        continue;
      }
      backward = dir_back;
      stack.push_back(st);
      State next = st;
      if (find(next, true)) {
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
      if (b and b->is_macro) {
        push_input(b->macro);
        return true;
      }
      // Any other key ends the search and does what it normally does.
      if (w.empty() or w == "vi-cmd-mode" or w == "beep") return true;
      mKeys = seq;
      return run_widget(w);
    }
  }
}

bool Editor::w_infer_next_history() {
  const auto& e = mHist.entries();
  for (int i = static_cast<int>(e.size()) - 2; i >= 0; --i) {
    if (e[static_cast<s::size_t>(i)].text == mBuf.text) {
      set_history_line(i + 1);
      return true;
    }
  }
  return false;
}

bool Editor::w_insert_last_word() {
  int hist = static_cast<int>(mHist.entries().size()) - 1;
  bool again = mLastWidget == "insert-last-word"
           and mLastWordEnd <= mBuf.text.size() and mBuf.cursor == mLastWordEnd;
  if (again) {
    hist = mLastWordHist - 1;
    mBuf.text.erase(mLastWordStart, mLastWordEnd - mLastWordStart);
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

bool Editor::w_copy_prev_word() {
  vec<str> words = HistoryManager::split_words(mBuf.text.substr(0, mBuf.cursor));
  if (words.empty()) return false;
  mBuf.insert(words.back());
  return true;
}

bool Editor::w_push_line() {
  mZle.buffer_stack.push_back(mBuf.text);
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
  auto r = mHist.expand_line(mBuf.text);
  if (not r) {
    mMessage = "jfsh: " + r.error().message;
    return false;
  }
  if (not r->changed) return true;
  mBuf.text   = r->text;
  mBuf.cursor = mBuf.text.size();
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
  mBuf.text.replace(a, b - a, q);
  mBuf.cursor = a + q.size();
  mRegionActive = false;
  return true;
}

bool Editor::w_what_cursor_position() {
  char buf[160];
  s::size_t len;
  int col = static_cast<int>(char_count(strv(mBuf.text).substr(line_start(mBuf.cursor),
                                                               mBuf.cursor - line_start(mBuf.cursor))));
  int pct = mBuf.text.empty() ? 0 : static_cast<int>(100 * mBuf.cursor / mBuf.text.size());
  if (mBuf.cursor >= mBuf.text.size()) {
    s::snprintf(buf, sizeof buf, "point %zu of %zu(%d%%)  column %d",
                mBuf.cursor, mBuf.text.size(), pct, col);
  } else {
    char32_t c = utf8_decode(mBuf.text, mBuf.cursor, len);
    str shown = mBuf.text.substr(mBuf.cursor, len);
    if (c < 0x20) shown = str("^") + static_cast<char>(c | 0x40);
    s::snprintf(buf, sizeof buf, "Char: %s (0%o, %u, 0x%x)  point %zu of %zu(%d%%)  column %d",
                shown.c_str(), static_cast<unsigned>(c), static_cast<unsigned>(c),
                static_cast<unsigned>(c), mBuf.cursor, mBuf.text.size(), pct, col);
  }
  mMessage = buf;
  return true;
}

opt<str> Editor::minibuffer(strv prompt) {
  str text;
  for (;;) {
    mMessage = str(prompt) + text + "_";
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

bool Editor::w_execute_named_cmd() {
  auto name = minibuffer("execute: ");
  if (not name or name->empty()) return false;
  if (not mZle.is_widget(*name)) {
    mMessage = "No such widget `" + *name + "'";
    return false;
  }
  mLastNamedCmd = *name;
  return run_widget(*name);
}

bool Editor::w_execute_last_named_cmd() {
  if (mLastNamedCmd.empty()) return false;
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
  if (mBuf.text.starts_with('#')) mBuf.text.erase(0, 1);
  else mBuf.text.insert(0, "#");
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
  mBuf.text.replace(end, next - end, " ");
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

bool Editor::w_expand_word() { return false; }

bool Editor::w_run_help() {
  auto words = HistoryManager::split_words(mBuf.text);
  if (words.empty()) return false;
  mZle.buffer_stack.push_back(mBuf.text);
  mBuf.text   = "man " + words[0];
  mBuf.cursor = mBuf.text.size();
  mDone = true;
  return true;
}

bool Editor::w_which_command() {
  auto words = HistoryManager::split_words(mBuf.text);
  if (words.empty()) return false;
  mZle.buffer_stack.push_back(mBuf.text);
  mBuf.text   = "whence -c " + words[0];
  mBuf.cursor = mBuf.text.size();
  mDone = true;
  return true;
}

// ---- The widget table ----------------------------------------------------------

const umap<str, Editor::WidgetFn>& Editor::widget_table() {
  static const umap<str, WidgetFn> table = {
    {"accept-and-hold",                     &Editor::w_accept_and_hold},
    {"accept-line",                         &Editor::w_accept_line},
    {"accept-line-and-down-history",        &Editor::w_accept_line_and_down_history},
    {"backward-char",                       &Editor::w_backward_char},
    {"backward-delete-char",                &Editor::w_backward_delete_char},
    {"backward-delete-word",                &Editor::w_backward_kill_word},
    {"backward-kill-line",                  &Editor::w_backward_kill_line},
    {"backward-kill-word",                  &Editor::w_backward_kill_word},
    {"backward-word",                       &Editor::w_backward_word},
    {"beep",                                &Editor::w_beep},
    {"beginning-of-buffer-or-history",      &Editor::w_beginning_of_buffer_or_history},
    {"beginning-of-history",                &Editor::w_beginning_of_history},
    {"beginning-of-line",                   &Editor::w_beginning_of_line},
    {"beginning-of-line-hist",              &Editor::w_beginning_of_line},
    {"bracketed-paste",                     &Editor::w_bracketed_paste},
    {"capitalize-word",                     &Editor::w_capitalize_word},
    {"clear-screen",                        &Editor::w_clear_screen},
    {"complete-word",                       &Editor::w_complete_word},
    {"copy-prev-word",                      &Editor::w_copy_prev_word},
    {"copy-region-as-kill",                 &Editor::w_copy_region_as_kill},
    {"deactivate-region",                   &Editor::w_deactivate_region},
    {"delete-char",                         &Editor::w_delete_char},
    {"delete-char-or-list",                 &Editor::w_delete_char_or_list},
    {"delete-word",                         &Editor::w_kill_word},
    {"digit-argument",                      &Editor::w_digit_argument},
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
    {"end-of-line-hist",                    &Editor::w_end_of_line},
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
    {"insert-last-word",                    &Editor::w_insert_last_word},
    {"kill-buffer",                         &Editor::w_kill_buffer},
    {"kill-line",                           &Editor::w_kill_line},
    {"kill-region",                         &Editor::w_kill_region},
    {"kill-whole-line",                     &Editor::w_kill_whole_line},
    {"kill-word",                           &Editor::w_kill_word},
    {"list-choices",                        &Editor::w_list_choices},
    {"list-expand",                         &Editor::w_expand_word},
    {"magic-space",                         &Editor::w_magic_space},
    {"menu-complete",                       &Editor::w_menu_complete},
    {"menu-expand-or-complete",             &Editor::w_menu_complete},
    {"neg-argument",                        &Editor::w_neg_argument},
    {"overwrite-mode",                      &Editor::w_overwrite_mode},
    {"pound-insert",                        &Editor::w_pound_insert},
    {"push-input",                          &Editor::w_push_line},
    {"push-line",                           &Editor::w_push_line},
    {"push-line-or-edit",                   &Editor::w_push_line},
    {"put-replace-selection",               &Editor::w_put_replace_selection},
    {"quote-line",                          &Editor::w_quote_line},
    {"quote-region",                        &Editor::w_quote_region},
    {"quoted-insert",                       &Editor::w_quoted_insert},
    {"redisplay",                           &Editor::w_redisplay},
    {"redo",                                &Editor::w_redo},
    {"reverse-menu-complete",               &Editor::w_reverse_menu_complete},
    {"run-help",                            &Editor::w_run_help},
    {"select-a-blank-word",                 &Editor::w_select_a_blank_word},
    {"select-a-bracketed",                  &Editor::w_select_a_bracketed},
    {"select-a-quoted",                     &Editor::w_select_a_quoted},
    {"select-a-shell-word",                 &Editor::w_select_a_shell_word},
    {"select-a-word",                       &Editor::w_select_a_word},
    {"select-in-blank-word",                &Editor::w_select_in_blank_word},
    {"select-in-bracketed",                 &Editor::w_select_in_bracketed},
    {"select-in-quoted",                    &Editor::w_select_in_quoted},
    {"select-in-shell-word",                &Editor::w_select_in_shell_word},
    {"select-in-word",                      &Editor::w_select_in_word},
    {"self-insert",                         &Editor::w_self_insert},
    {"self-insert-unmeta",                  &Editor::w_self_insert_unmeta},
    {"send-break",                          &Editor::w_send_break},
    {"set-mark-command",                    &Editor::w_set_mark_command},
    {"spell-word",                          &Editor::w_expand_word},
    {"transpose-chars",                     &Editor::w_transpose_chars},
    {"transpose-words",                     &Editor::w_transpose_words},
    {"undefined-key",                       &Editor::w_beep},
    {"undo",                                &Editor::w_undo},
    {"universal-argument",                  &Editor::w_universal_argument},
    {"up-case-word",                        &Editor::w_up_case_word},
    {"up-history",                          &Editor::w_up_history},
    {"up-line",                             &Editor::w_up_line},
    {"up-line-or-history",                  &Editor::w_up_line_or_history},
    {"vi-add-eol",                          &Editor::w_vi_add_eol},
    {"vi-add-next",                         &Editor::w_vi_add_next},
    {"vi-backward-blank-word",              &Editor::w_vi_backward_blank_word},
    {"vi-backward-blank-word-end",          &Editor::w_vi_backward_blank_word_end},
    {"vi-backward-char",                    &Editor::w_vi_backward_char},
    {"vi-backward-delete-char",             &Editor::w_vi_backward_delete_char},
    {"vi-backward-kill-word",               &Editor::w_vi_backward_kill_word},
    {"vi-backward-word",                    &Editor::w_vi_backward_word},
    {"vi-backward-word-end",                &Editor::w_vi_backward_word_end},
    {"vi-beginning-of-line",                &Editor::w_vi_beginning_of_line},
    {"vi-change",                           &Editor::w_vi_change},
    {"vi-change-eol",                       &Editor::w_vi_change_eol},
    {"vi-change-whole-line",                &Editor::w_vi_change_whole_line},
    {"vi-cmd-mode",                         &Editor::w_vi_cmd_mode},
    {"vi-delete",                           &Editor::w_vi_delete},
    {"vi-delete-char",                      &Editor::w_vi_delete_char},
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
    {"vi-kill-eol",                         &Editor::w_vi_kill_eol},
    {"vi-kill-line",                        &Editor::w_vi_kill_line},
    {"vi-match-bracket",                    &Editor::w_vi_match_bracket},
    {"vi-open-line-above",                  &Editor::w_vi_open_line_above},
    {"vi-open-line-below",                  &Editor::w_vi_open_line_below},
    {"vi-oper-swap-case",                   &Editor::w_vi_oper_swap_case},
    {"vi-put-after",                        &Editor::w_vi_put_after},
    {"vi-put-before",                       &Editor::w_vi_put_before},
    {"vi-quoted-insert",                    &Editor::w_vi_quoted_insert},
    {"vi-repeat-change",                    &Editor::w_vi_repeat_change},
    {"vi-repeat-find",                      &Editor::w_vi_repeat_find},
    {"vi-repeat-search",                    &Editor::w_vi_repeat_search},
    {"vi-replace",                          &Editor::w_vi_replace},
    {"vi-replace-chars",                    &Editor::w_vi_replace_chars},
    {"vi-rev-repeat-find",                  &Editor::w_vi_rev_repeat_find},
    {"vi-rev-repeat-search",                &Editor::w_vi_rev_repeat_search},
    {"vi-set-buffer",                       &Editor::w_vi_set_buffer},
    {"vi-set-mark",                         &Editor::w_vi_set_mark},
    {"vi-substitute",                       &Editor::w_vi_substitute},
    {"vi-swap-case",                        &Editor::w_vi_swap_case},
    {"vi-unindent",                         &Editor::w_vi_unindent},
    {"vi-up-case",                          &Editor::w_vi_up_case},
    {"vi-up-line-or-history",               &Editor::w_vi_up_line_or_history},
    {"vi-yank",                             &Editor::w_vi_yank},
    {"vi-yank-whole-line",                  &Editor::w_vi_yank_whole_line},
    {"visual-line-mode",                    &Editor::w_visual_line_mode},
    {"visual-mode",                         &Editor::w_visual_mode},
    {"what-cursor-position",                &Editor::w_what_cursor_position},
    {"which-command",                       &Editor::w_which_command},
    {"yank",                                &Editor::w_yank},
    {"yank-pop",                            &Editor::w_yank_pop},
  };
  return table;
}

vec<str> Editor::builtin_widget_names() {
  vec<str> names;
  for (const auto& [name, fn] : widget_table()) names.push_back(name);
  return names;
}

}  // namespace jfsh
