#include <jfsh/editor/editor.h>

#include <cerrno>
#include <cstdio>
#include <cstring>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <jfsh/core/aliases.h>
#include <jfsh/editor/completion.h>

namespace jfsh {


Editor::Editor(const OptionState& opts,
               HistoryManager&    hist,
               PromptRenderer&    prompt)
  : mOpts(opts), mHist(hist), mPrompt(prompt) {
  if (mOpts.get(OPT_VI)) mMode = EditorMode::Vi;
  install_default_bindings();
}

Editor::~Editor() {
  if (mRawModeActive) exit_raw_mode();
}

static struct termios OrigTermios;

// Writes all of `data` to the terminal, retrying after EINTR and partial
// writes; a failed write leaves nothing to recover in a redraw.
static void term_write(strv data) {
  while (not data.empty()) {
    ssize_t n = ::write(STDOUT_FILENO, data.data(), data.size());
    if (n < 0) {
      if (errno == EINTR) continue;
      return;
    }
    data.remove_prefix(static_cast<s::size_t>(n));
  }
}

void Editor::enter_raw_mode() {
  tcgetattr(STDIN_FILENO, &OrigTermios);
  struct termios raw = OrigTermios;
  raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | ISIG | IEXTEN));
  raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | BRKINT | INPCK | ISTRIP));
  raw.c_cflag |= CS8;
  raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
  raw.c_cc[VMIN]  = 1;
  raw.c_cc[VTIME] = 0;
  // TCSADRAIN, not TCSAFLUSH: input typed or pasted ahead (the next lines
  // of a multi-line command) must survive the switch.
  tcsetattr(STDIN_FILENO, TCSADRAIN, &raw);
  mRawModeActive = true;
}

// TCSANOW: on macOS TCSADRAIN on a pty waits until the other side has read
// the echoed line, and a Ctrl-C typed meanwhile arrives with ISIG still off
// and sits in the queue as a plain byte instead of interrupting the command.
void Editor::exit_raw_mode() {
  tcsetattr(STDIN_FILENO, TCSANOW, &OrigTermios);
  mRawModeActive = false;
}

opt<str> Editor::read_line(bool continuation) {
  mBuf.text.clear();
  mBuf.cursor    = 0;
  mHistPos       = -1;
  mContinuation  = continuation;
  mInterrupted   = false;
  mEof           = false;

  str ps1 = continuation ? mPrompt.continuation() : mPrompt.primary();
  s::fwrite(ps1.c_str(), 1, ps1.size(), stdout);
  s::fflush(stdout);

  if (not isatty(STDIN_FILENO)) {
    str line;
    int c;
    while ((c = s::fgetc(stdin)) != EOF and c != '\n')
      line += static_cast<char>(c);
    if (c == EOF and line.empty()) return s::nullopt;
    return line;
  }

  enter_raw_mode();
  bool done = false;
  while (not done) {
    str key = read_key_sequence();
    if (mEof) {                      // the terminal went away
      exit_raw_mode();
      return s::nullopt;
    }
    if (key == "\x04" and mBuf.text.empty()) {  // Ctrl-D on empty line = EOF
      exit_raw_mode();
      s::putchar('\n');
      return s::nullopt;
    }
    if (key == "\x03") {            // Ctrl-C: drop the line
      mInterrupted = true;
      mBuf.text.clear();
      mBuf.cursor = 0;
      done = true;
      break;
    }
    if (key == "\r" or key == "\n") {
      done = true;
    } else if (key == "\x7f" or key == "\x08") {  // Backspace
      if (mBuf.cursor > 0) {
        --mBuf.cursor;
        mBuf.text.erase(mBuf.cursor, 1);
      }
    } else if (key == "\x1b[A") {  // Up arrow
      widget_up_line_or_history(mBuf);
    } else if (key == "\x1b[B") {  // Down arrow
      widget_down_line_or_history(mBuf);
    } else if (key == "\x1b[C") {  // Right
      if (mBuf.cursor < mBuf.text.size()) ++mBuf.cursor;
    } else if (key == "\x1b[D") {  // Left
      if (mBuf.cursor > 0) --mBuf.cursor;
    } else if (key == "\x01") {    // Ctrl-A
      mBuf.cursor = 0;
    } else if (key == "\x05") {    // Ctrl-E
      mBuf.cursor = mBuf.text.size();
    } else if (key == "\x09") {    // Tab
      if (mCompleter) {
        auto candidates = mCompleter(mBuf.before_cursor());
        if (candidates.size() == 1)
          mBuf.insert(candidates[0].substr(mBuf.cursor));
      }
    } else if (not key.empty()
               and static_cast<unsigned char>(key[0]) >= 0x20) {
      mBuf.insert(key);
    }
    refresh_display();
  }
  exit_raw_mode();
  s::putchar('\n');
  s::fflush(stdout);
  return mBuf.text;
}

str Editor::read_key_sequence() {
  char buf[8] = {};
  for (;;) {
    struct pollfd fds[2] = {{STDIN_FILENO, POLLIN, 0}, {mWakeFd, POLLIN, 0}};
    nfds_t nfds = (mWakeFd >= 0 and mWake) ? 2 : 1;
    int r = ::poll(fds, nfds, -1);
    if (r < 0) {
      if (errno == EINTR) continue;
      mEof = true;
      return {};
    }
    if (nfds == 2 and (fds[1].revents & POLLIN)) {
      // A signal arrived: let the shell act on it with the terminal in
      // its normal mode, then put the prompt and line back.
      exit_raw_mode();
      bool quit    = false;
      bool printed = mWake(quit);
      if (quit) {
        mEof = true;
        return {};
      }
      enter_raw_mode();
      if (printed) refresh_display();
      continue;
    }
    if (fds[0].revents & (POLLIN | POLLHUP | POLLERR)) break;
  }
  ssize_t r = read(STDIN_FILENO, buf, 1);
  if (r < 0 and errno == EINTR) return {};
  if (r <= 0) {
    // EOF or a hung-up terminal: without this the caller would spin.
    mEof = true;
    return {};
  }
  s::size_t n = 1;
  if (buf[0] == '\x1b') {
    // Try to read more bytes (escape sequence)
    struct timeval tv{0, 50000};
    fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
      ssize_t more = read(STDIN_FILENO, buf + n, sizeof(buf) - n);
      if (more > 0) n += static_cast<s::size_t>(more);
    }
  }
  return str(buf, n);
}

void Editor::refresh_display() {
  str ps1    = mContinuation ? mPrompt.continuation() : mPrompt.primary();
  int ps1_width = PromptRenderer::visual_width(ps1);
  // CR + erase line
  term_write("\r\x1b[K");
  term_write(ps1);
  term_write(mBuf.text);
  int col = ps1_width + static_cast<int>(mBuf.cursor);
  if (col > 0) {
    char seq[32];
    int len = s::snprintf(seq, sizeof seq, "\r\x1b[%dC", col);
    if (len > 0) term_write(strv(seq, static_cast<s::size_t>(len)));
  }
}

bool Editor::widget_up_line_or_history(LineBuffer& buf) {
  const auto& entries = mHist.entries();
  if (entries.empty()) return false;
  if (mHistPos < 0) {
    mSavedLine = buf.text;
    mHistPos   = static_cast<int>(entries.size()) - 1;
  } else if (mHistPos > 0) {
    --mHistPos;
  } else {
    return false;
  }
  buf.text   = entries[static_cast<s::size_t>(mHistPos)].text;
  buf.cursor = buf.text.size();
  return true;
}

bool Editor::widget_down_line_or_history(LineBuffer& buf) {
  if (mHistPos < 0) return false;
  const auto& entries = mHist.entries();
  if (mHistPos < static_cast<int>(entries.size()) - 1) {
    ++mHistPos;
    buf.text   = entries[static_cast<s::size_t>(mHistPos)].text;
    buf.cursor = buf.text.size();
  } else {
    mHistPos   = -1;
    buf.text   = mSavedLine;
    buf.cursor = buf.text.size();
  }
  return true;
}

int Editor::terminal_width() const noexcept {
  struct winsize ws{}; ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
  return ws.ws_col > 0 ? ws.ws_col : 80;
}

int Editor::terminal_height() const noexcept {
  struct winsize ws{}; ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
  return ws.ws_row > 0 ? ws.ws_row : 24;
}

}  // namespace jfsh
