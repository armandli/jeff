#include <cstdio>
#include <cstring>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <jfsh/editor/completion.h>
#include <jfsh/editor/editor.h>

namespace jfsh {

void LineBuffer::insert(std::string_view s) {
  text.insert(cursor, s);
  cursor += s.size();
}

void LineBuffer::delete_char_at(std::size_t pos) {
  if (pos < text.size()) text.erase(pos, 1);
}

void LineBuffer::move_to(std::size_t pos) {
  cursor = std::min(pos, text.size());
}

std::string_view LineBuffer::before_cursor() const noexcept {
  return std::string_view(text).substr(0, cursor);
}

std::string_view LineBuffer::after_cursor() const noexcept {
  return std::string_view(text).substr(cursor);
}

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

void Editor::enter_raw_mode() {
  tcgetattr(STDIN_FILENO, &OrigTermios);
  struct termios raw = OrigTermios;
  raw.c_lflag &= static_cast<tcflag_t>(~(ECHO | ICANON | ISIG | IEXTEN));
  raw.c_iflag &= static_cast<tcflag_t>(~(IXON | ICRNL | BRKINT | INPCK | ISTRIP));
  raw.c_cflag |= CS8;
  raw.c_oflag &= static_cast<tcflag_t>(~OPOST);
  raw.c_cc[VMIN]  = 1;
  raw.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &raw);
  mRawModeActive = true;
}

void Editor::exit_raw_mode() {
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &OrigTermios);
  mRawModeActive = false;
}

std::optional<std::string> Editor::read_line() {
  mBuf.text.clear();
  mBuf.cursor = 0;
  mHistPos    = -1;

  std::string ps1 = mPrompt.primary();
  std::fwrite(ps1.c_str(), 1, ps1.size(), stdout);
  std::fflush(stdout);

  if (not isatty(STDIN_FILENO)) {
    std::string line;
    int c;
    while ((c = std::fgetc(stdin)) != EOF and c != '\n')
      line += static_cast<char>(c);
    if (c == EOF and line.empty()) return std::nullopt;
    return line;
  }

  enter_raw_mode();
  bool done = false;
  while (not done) {
    std::string key = read_key_sequence();
    if (key == "\x04" and mBuf.text.empty()) {  // Ctrl-D on empty line = EOF
      exit_raw_mode();
      std::putchar('\n');
      return std::nullopt;
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
  std::putchar('\n');
  std::fflush(stdout);
  return mBuf.text;
}

std::string Editor::read_key_sequence() {
  char buf[8] = {};
  int n = static_cast<int>(read(STDIN_FILENO, buf, 1));
  if (n <= 0) return {};
  if (buf[0] == '\x1b') {
    // Try to read more bytes (escape sequence)
    struct timeval tv{0, 50000};
    fd_set fds; FD_ZERO(&fds); FD_SET(STDIN_FILENO, &fds);
    if (select(STDIN_FILENO + 1, &fds, nullptr, nullptr, &tv) > 0) {
      auto more = read(STDIN_FILENO, buf + n,
                       sizeof(buf) - static_cast<std::size_t>(n));
      n += static_cast<int>(more);
    }
  }
  return std::string(buf, static_cast<std::size_t>(n));
}

void Editor::refresh_display() {
  std::string ps1    = mPrompt.primary();
  int         ps1_width = PromptRenderer::visual_width(ps1);
  // CR + erase line
  write(STDOUT_FILENO, "\r\x1b[K", 4);
  write(STDOUT_FILENO, ps1.c_str(), ps1.size());
  write(STDOUT_FILENO, mBuf.text.c_str(), mBuf.text.size());
  int col = ps1_width + static_cast<int>(mBuf.cursor);
  if (col > 0) {
    char seq[32];
    int len = std::snprintf(seq, sizeof seq, "\r\x1b[%dC", col);
    if (len > 0) write(STDOUT_FILENO, seq, static_cast<std::size_t>(len));
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
  buf.text   = entries[static_cast<std::size_t>(mHistPos)].text;
  buf.cursor = buf.text.size();
  return true;
}

bool Editor::widget_down_line_or_history(LineBuffer& buf) {
  if (mHistPos < 0) return false;
  const auto& entries = mHist.entries();
  if (mHistPos < static_cast<int>(entries.size()) - 1) {
    ++mHistPos;
    buf.text   = entries[static_cast<std::size_t>(mHistPos)].text;
    buf.cursor = buf.text.size();
  } else {
    mHistPos   = -1;
    buf.text   = mSavedLine;
    buf.cursor = buf.text.size();
  }
  return true;
}

bool Editor::widget_self_insert(LineBuffer&)          { return false; }
bool Editor::widget_backward_delete_char(LineBuffer&) { return false; }
bool Editor::widget_beginning_of_line(LineBuffer& buf){ buf.cursor = 0; return true; }
bool Editor::widget_end_of_line(LineBuffer& buf)      { buf.cursor = buf.text.size(); return true; }
bool Editor::widget_accept_line(LineBuffer&)          { return true; }
bool Editor::widget_complete_word(LineBuffer&)        { return false; }
bool Editor::widget_expand_or_complete(LineBuffer&)   { return false; }

void Editor::suspend() { if (mRawModeActive) exit_raw_mode(); }
void Editor::resume()  {}

void Editor::bind_key(std::string /*km*/, std::string /*seq*/, std::string /*widget*/) {}
void Editor::register_widget(ZleWidget /*w*/) {}
void Editor::set_mode(EditorMode mode) { mMode = mode; }
void Editor::redraw() { refresh_display(); }

int Editor::terminal_width() const noexcept {
  struct winsize ws{}; ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
  return ws.ws_col > 0 ? ws.ws_col : 80;
}

int Editor::terminal_height() const noexcept {
  struct winsize ws{}; ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws);
  return ws.ws_row > 0 ? ws.ws_row : 24;
}

bool Editor::dispatch_widget(std::string_view /*name*/) { return false; }

void Editor::install_default_bindings() {
  if (mMode == EditorMode::Vi) install_vi_bindings();
  else                         install_emacs_bindings();
}

void Editor::install_vi_bindings()    {}
void Editor::install_emacs_bindings() {}

}  // namespace jfsh
