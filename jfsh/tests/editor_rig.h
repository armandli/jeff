#ifndef JFSH_TESTS_EDITOR_RIG_H
#define JFSH_TESTS_EDITOR_RIG_H
// The line editor in process, for the editor and completion tests: an
// Editor reads from the slave side of a pty on another thread while the
// test types on the master side and feeds what comes back into a small
// terminal emulator, so tests can look at the screen as a user would.

#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <chrono>
#include <future>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <shell_fixture.h>

#include <jfsh/core/aliases.h>
#include <jfsh/core/utf8.h>
#include <jfsh/editor/completion.h>
#include <jfsh/editor/editor.h>
#include <jfsh/editor/zle.h>
#include <jfsh/history/history.h>
#include <jfsh/prompt/prompt.h>

namespace jfsh::test {

// Enough of a VT100 for the editor's output: printing with auto-margins,
// CR, LF, cursor motion (A B C D H), erasing (J K) and SGR (ignored).
struct Screen {
  int rows, cols;
  vec<vec<str>> cell;   // each cell one character ("" for a wide one's right half)
  int r = 0, c = 0;
  bool pending_wrap = false;
  str partial;          // an incomplete escape or UTF-8 sequence

  Screen(int rows_, int cols_) : rows(rows_), cols(cols_), cell(static_cast<s::size_t>(rows_)) {
    for (auto& row : cell) row.assign(static_cast<s::size_t>(cols), " ");
  }
  void scroll() {
    cell.erase(cell.begin());
    cell.emplace_back(static_cast<s::size_t>(cols), " ");
    --r;
  }
  void newline() {
    if (++r >= rows) scroll();
  }
  void clear_from(int row, int col) {
    auto& line = cell[static_cast<s::size_t>(row)];
    for (int x = col; x < cols; ++x) line[static_cast<s::size_t>(x)] = " ";
  }
  void feed(strv data) {
    str in = partial + str(data);
    partial.clear();
    s::size_t i = 0;
    while (i < in.size()) {
      char ch = in[i];
      if (ch == '\x1b') {
        if (i + 1 >= in.size()) { partial = in.substr(i); return; }
        if (in[i + 1] != '[') { i += 2; continue; }
        s::size_t j = i + 2;
        while (j < in.size() and not (in[j] >= 0x40 and in[j] <= 0x7e)) ++j;
        if (j >= in.size()) { partial = in.substr(i); return; }
        str params = in.substr(i + 2, j - i - 2);
        int n = params.empty() or params[0] == '?' ? 0 : s::atoi(params.c_str());
        switch (in[j]) {
          case 'A': r = s::max(0, r - s::max(n, 1)); pending_wrap = false; break;
          case 'B': r = s::min(rows - 1, r + s::max(n, 1)); pending_wrap = false; break;
          case 'C': c = s::min(cols - 1, c + s::max(n, 1)); pending_wrap = false; break;
          case 'D': c = s::max(0, c - s::max(n, 1)); pending_wrap = false; break;
          case 'H': r = 0; c = 0; pending_wrap = false; break;
          case 'K': clear_from(r, c); break;
          case 'J':
            if (n == 2) { for (int y = 0; y < rows; ++y) clear_from(y, 0); }
            else { clear_from(r, c); for (int y = r + 1; y < rows; ++y) clear_from(y, 0); }
            break;
          default: break;
        }
        i = j + 1;
        continue;
      }
      if (ch == '\r') { c = 0; pending_wrap = false; ++i; continue; }
      if (ch == '\n') { newline(); pending_wrap = false; ++i; continue; }
      if (ch == '\a') { ++i; continue; }
      if (ch == '\b') { if (c > 0) --c; ++i; continue; }
      unsigned char lead = static_cast<unsigned char>(ch);
      s::size_t need = lead >= 0xf0 ? 4 : lead >= 0xe0 ? 3 : lead >= 0xc0 ? 2 : 1;
      if (i + need > in.size()) { partial = in.substr(i); return; }
      str chr = in.substr(i, need);
      i += need;
      s::size_t len;
      int w = char_width(utf8_decode(chr, 0, len));
      if (w == 0) {   // a combining mark joins the cell before it
        int pc = pending_wrap ? c : s::max(0, c - 1);
        cell[static_cast<s::size_t>(r)][static_cast<s::size_t>(pc)] += chr;
        continue;
      }
      if (pending_wrap or c + w > cols) {
        c = 0;
        newline();
        pending_wrap = false;
      }
      cell[static_cast<s::size_t>(r)][static_cast<s::size_t>(c)] = chr;
      if (w == 2 and c + 1 < cols)
        cell[static_cast<s::size_t>(r)][static_cast<s::size_t>(c + 1)] = "";
      c += w;
      if (c >= cols) {
        c = cols - 1;
        pending_wrap = true;
      }
    }
  }
  str row(int y) const {
    str s;
    for (const auto& x : cell[static_cast<s::size_t>(y)]) s += x;
    while (not s.empty() and s.back() == ' ') s.pop_back();
    return s;
  }
  // The cursor's column, counting a pending wrap as the next row's start.
  s::pair<int, int> cursor() const { return {r, c}; }
};

// An Editor on a pty, with the shell around it for widgets and completion.
struct Rig {
  Shell sh;
  HistoryManager hist{HistOptions{}};
  PromptRenderer prompt{sh.params, sh.opts};
  int master = -1, slave = -1;
  uptr<Editor> ed;
  Screen screen;
  str raw;   // everything the editor wrote
  s::future<opt<str>> pending;

  explicit Rig(int cols = 80, int rows = 24) : screen(rows, cols) {
    ::openpty(&master, &slave, nullptr, nullptr, nullptr);
    struct winsize ws{};
    ws.ws_row = static_cast<unsigned short>(rows);
    ws.ws_col = static_cast<unsigned short>(cols);
    ::ioctl(slave, TIOCSWINSZ, &ws);
    // Raw before any key arrives, so the line discipline doesn't eat ^W,
    // ^U or DEL while the editor isn't reading yet.
    struct termios t{};
    ::tcgetattr(slave, &t);
    ::cfmakeraw(&t);
    ::tcsetattr(slave, TCSANOW, &t);
    sh.params.set_scalar("PS1", "> ");
    sh.ctx.history = &hist;
    ed = s::make_unique<Editor>(sh.opts, hist, prompt, *sh.ctx.zle);
    ed->set_io(slave, slave);
  }
  ~Rig() {
    if (pending.valid()) {
      send("\x03");
      finish();
    }
    sh.ctx.history = nullptr;
    ed.reset();
    ::close(master);
    ::close(slave);
  }

  void start(bool continuation = false) {
    // Each line on a fresh screen: the editor only moves relative to
    // where it starts.
    while (drain(0)) {}
    screen = Screen(screen.rows, screen.cols);
    raw.clear();
    prompt.reload();
    pending = s::async(s::launch::async,
                       [this, continuation] { return ed->read_line(continuation); });
  }
  void send(strv keys) const {
    while (not keys.empty()) {
      ssize_t n = ::write(master, keys.data(), keys.size());
      if (n <= 0) return;
      keys.remove_prefix(static_cast<s::size_t>(n));
    }
  }
  bool drain(int wait_ms) {
    struct pollfd p{master, POLLIN, 0};
    if (::poll(&p, 1, wait_ms) <= 0) return false;
    char buf[4096];
    ssize_t n = ::read(master, buf, sizeof buf);
    if (n <= 0) return false;
    raw.append(buf, static_cast<s::size_t>(n));
    screen.feed(strv(buf, static_cast<s::size_t>(n)));
    return true;
  }
  // Reads the editor's output until it has been quiet a while.
  void settle() {
    while (drain(150)) {}
  }
  opt<str> finish() {
    // A line that never ends fails the test instead of hanging the suite:
    // after 10 s the rig types Ctrl-C, after 20 s it gives up.
    auto start = s::chrono::steady_clock::now();
    bool interrupted = false;
    while (pending.wait_for(s::chrono::milliseconds(5)) != s::future_status::ready) {
      drain(5);
      auto waited = s::chrono::steady_clock::now() - start;
      if (not interrupted and waited > s::chrono::seconds(10)) {
        ADD_FAILURE() << "the editor didn't finish the line; sending Ctrl-C";
        send("\x03");
        interrupted = true;
      } else if (waited > s::chrono::seconds(20)) {
        s::fprintf(stderr, "editor rig: the line never ended\n");
        s::abort();
      }
    }
    while (drain(20)) {}
    return pending.get();
  }
  // Types `keys` (which should end the line) and returns the line read.
  opt<str> line(strv keys) {
    start();
    send(keys);
    return finish();
  }
  // Types `keys`, then shows the screen without ending the line.
  void type(strv keys) {
    if (not pending.valid()) start();
    send(keys);
    settle();
  }
};

}  // namespace jfsh::test

#endif  // JFSH_TESTS_EDITOR_RIG_H
