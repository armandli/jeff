// The line editor (M9), in process: an Editor reads from the slave side of
// a pty on another thread while the test types on the master side and
// feeds what comes back into a small terminal emulator, so tests can look
// at the screen as a user would.  Key bindings and behaviour follow zsh
// 5.9's (`zsh -f`).

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

namespace jfsh {
namespace {

using namespace s::chrono_literals;
using namespace s::string_literals;
using test::Shell;
using test::TempDir;

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
    for (int x = col; x < cols; ++x) cell[static_cast<s::size_t>(row)][static_cast<s::size_t>(x)] = " ";
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
      if (w == 2 and c + 1 < cols) cell[static_cast<s::size_t>(r)][static_cast<s::size_t>(c + 1)] = "";
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
    pending = s::async(s::launch::async, [this, continuation] { return ed->read_line(continuation); });
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
    while (pending.wait_for(5ms) != s::future_status::ready) drain(5);
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

// ---- Emacs mode -------------------------------------------------------------------

TEST(Editor, InsertAndMove) {
  Rig t;
  EXPECT_EQ(t.line("abc\x02\x02X\r"), "aXbc");
  EXPECT_EQ(t.line("abc\x01X\x05Y\r"), "XabcY");
  EXPECT_EQ(t.line("abc\x1b[D\x1b[DX\x1b[CY\r"), "aXbYc");
  EXPECT_EQ(t.line("abc\x1b[H<\x1b[F>\x1b[1~^\r"), "^<abc>");
  EXPECT_EQ(t.line("abc\x01\x1b[3~\r"), "bc");
  // forward-word stops at the start of the next word (zsh, not emacs).
  EXPECT_EQ(t.line("one two three\x01\x1b" "fX\x1b" "bY\r"), "one YXtwo three");
}

TEST(Editor, Killing) {
  Rig t;
  EXPECT_EQ(t.line("one two\x17\r"), "one ");
  EXPECT_EQ(t.line("one two three\x01\x1b" "d\r"), " two three");
  EXPECT_EQ(t.line("hello world\x01\x1b" "f\x0b\x05 \x19\r"), "hello  world");
  EXPECT_EQ(t.line("abc def\x15\r"), "");
  // Kills in a row join up; yank-pop walks the ring.
  EXPECT_EQ(t.line("one two\x17\x17\x19\r"), "one two");
  EXPECT_EQ(t.line("aaa bbb\x17x\x01\x0b\x19\x1by\r"), "bbb");
  EXPECT_EQ(t.line("abc\x08\x7f\r"), "a");
}

TEST(Editor, TransposeAndCase) {
  Rig t;
  EXPECT_EQ(t.line("ab\x14\r"), "ba");
  EXPECT_EQ(t.line("abc\x02\x14\r"), "acb");
  EXPECT_EQ(t.line("one two\x1bt\r"), "two one");
  EXPECT_EQ(t.line("hello WORLD\x01\x1b" "c\x1b" "l\r"), "Hello world");
  EXPECT_EQ(t.line("hello world\x01\x1bu\r"), "HELLO world");
}

TEST(Editor, UndoAndNumericArguments) {
  Rig t;
  EXPECT_EQ(t.line("abc\x1f\r"), "");          // typing is one undo step
  EXPECT_EQ(t.line("abc def\x17\x1f\r"), "abc def");
  EXPECT_EQ(t.line("ab\x18u\r"), "");          // ^Xu
  EXPECT_EQ(t.line("\x1b" "3x\r"), "xxx");
  EXPECT_EQ(t.line("abcdef\x1b" "3\x08\r"), "abc");
  EXPECT_EQ(t.line("a b c\x1b" "2\x17\r"), "a ");
}

TEST(Editor, MarkAndRegion) {
  Rig t;
  EXPECT_EQ(t.line("abc def\x01\x1b" "f\x00\x05\x1bw\x19\r"s), "abc defdef");
  EXPECT_EQ(t.line("abc\x00\x01\x18\x18X\r"s), "abcX");
  EXPECT_EQ(t.line("it's\x1b'\r"), "'it'\\''s'");
}

TEST(Editor, QuotedInsertAndPaste) {
  Rig t;
  EXPECT_EQ(t.line("a\x16\x01" "b\r"), "a\x01" "b");
  // A bracketed paste goes in as text, newlines and all.
  EXPECT_EQ(t.line("\x1b[200~echo a\rls\x1b[201~\r"), "echo a\nls");
  // Unknown escape sequences don't come out as text.
  EXPECT_EQ(t.line("a\x1b[1;5Cb\r"), "ab");
}

TEST(Editor, EofAndInterrupt) {
  Rig t;
  EXPECT_EQ(t.line("\x04"), s::nullopt);
  EXPECT_EQ(t.line("ab\x01\x04\r"), "b");      // not on an empty line: delete-char
  EXPECT_EQ(t.line("abc\x03"), "");
  EXPECT_TRUE(t.ed->interrupted());
  t.sh.opts.set(OPT_IGNOREEOF);
  EXPECT_EQ(t.line("\x04x\r"), "x");
}

// ---- History ------------------------------------------------------------------

TEST(Editor, HistoryNavigation) {
  Rig t;
  t.hist.add("first");
  t.hist.add("second");
  EXPECT_EQ(t.line("\x1b[A\r"), "second");
  EXPECT_EQ(t.line("\x10\x10\r"), "first");
  EXPECT_EQ(t.line("typed\x1b[A\x1b[B\r"), "typed");
  // An edited history line keeps its edit while moving about.
  EXPECT_EQ(t.line("\x10X\x10\x0e\r"), "secondX");
  EXPECT_EQ(t.line("\x1b<\r"), "first");
  EXPECT_EQ(t.line("x\x1b<\x1b>\r"), "x");
}

TEST(Editor, IncrementalSearch) {
  Rig t;
  for (const char* h : {"git status", "ls -l", "git commit"}) t.hist.add(h);
  EXPECT_EQ(t.line("\x12git\r"), "git commit");
  EXPECT_EQ(t.line("\x12git\x12\r"), "git status");
  EXPECT_EQ(t.line("\x12gi\x7f\x7fls\r"), "ls -l");
  EXPECT_EQ(t.line("keep\x12ls\x07\r"), "keep");          // ^G: back where it was
  EXPECT_EQ(t.line("\x12stat\x05 -s\r"), "git status -s");  // another key ends it
  t.type("\x12zzz");
  EXPECT_EQ(t.screen.row(1), "failing bck-i-search: zzz_");
  t.send("\x07\r");
  t.finish();
}

TEST(Editor, HistorySearchAndLastWord) {
  Rig t;
  for (const char* h : {"git status", "echo foo bar", "git commit -m x"}) t.hist.add(h);
  EXPECT_EQ(t.line("git\x1bp\r"), "git commit -m x");
  EXPECT_EQ(t.line("git\x1bp\x1bp\r"), "git status");
  EXPECT_EQ(t.line("ls \x1b.\r"), "ls x");
  EXPECT_EQ(t.line("ls \x1b.\x1b.\r"), "ls bar");
  EXPECT_EQ(t.line("ls \x1b" "2\x1b.\r"), "ls -m");
  EXPECT_EQ(t.line("cp a \x1b\x1f\r"), "cp a a");
}

TEST(Editor, BufferStack) {
  Rig t;
  EXPECT_EQ(t.line("first\x1bq\r"), "");
  EXPECT_EQ(t.line("\r"), "first");            // push-line: back at the next prompt
  EXPECT_EQ(t.line("again\x1b" "a"), "again");   // accept-and-hold
  EXPECT_EQ(t.line("\r"), "again");
  EXPECT_EQ(t.line("x\x1bqy\x1bg\r"), "yx");    // get-line
}

TEST(Editor, ExpandHistoryWidget) {
  Rig t;
  t.hist.add("echo hello");
  EXPECT_EQ(t.line("x !!\x1b!\r"), "x echo hello");
}

// ---- The display ---------------------------------------------------------------

TEST(Display, PromptAndCursor) {
  Rig t;
  t.type("abc\x02");
  EXPECT_EQ(t.screen.row(0), "> abc");
  EXPECT_EQ(t.screen.cursor(), s::make_pair(0, 4));
  t.send("\r");
  EXPECT_EQ(t.finish(), "abc");
}

TEST(Display, B25Utf8AndWideCharacters) {
  Rig t;
  EXPECT_EQ(t.line("aé€😀\x02\x7f\r"), "aé😀");
  EXPECT_EQ(t.line("e\xcc\x81x\x02\x02Y\r"), "Ye\xcc\x81x");   // e + combining acute is one
  t.type("日本語\x02");
  EXPECT_EQ(t.screen.row(0), "> 日本語");
  EXPECT_EQ(t.screen.cursor(), s::make_pair(0, 6));
  t.send("\r");
  t.finish();
}

TEST(Display, B26LongLinesWrap) {
  Rig t(20);
  str text(30, 'x');
  t.type(text);
  EXPECT_EQ(t.screen.row(0), "> " + str(18, 'x'));
  EXPECT_EQ(t.screen.row(1), str(12, 'x'));
  EXPECT_EQ(t.screen.cursor(), s::make_pair(1, 12));
  t.send("\x01");
  t.settle();
  EXPECT_EQ(t.screen.cursor(), s::make_pair(0, 2));
  // Deleting back to one line leaves no stale second row.
  t.send("\x05" + str(15, '\x7f'));
  t.settle();
  EXPECT_EQ(t.screen.row(0), "> " + str(15, 'x'));
  EXPECT_EQ(t.screen.row(1), "");
  t.send("\r");
  EXPECT_EQ(t.finish(), str(15, 'x'));
}

TEST(Display, ExactlyFullRowMovesTheCursorDown) {
  Rig t(10);
  t.type("12345678");   // "> " + 8 fills the row
  EXPECT_EQ(t.screen.row(0), "> 12345678");
  EXPECT_EQ(t.screen.cursor(), s::make_pair(1, 0));
  t.send("\r");
  t.finish();
}

TEST(Display, MultiLinePromptAndBuffer) {
  Rig t;
  t.sh.params.set_scalar("PS1", "top line\n%% ");
  t.type("one\x16\x0atwo");   // a literal newline in the buffer
  EXPECT_EQ(t.screen.row(0), "top line");
  EXPECT_EQ(t.screen.row(1), "% one");
  EXPECT_EQ(t.screen.row(2), "two");
  t.send("\x10");   // up-line: on the buffer's first line
  t.settle();
  EXPECT_EQ(t.screen.cursor(), s::make_pair(1, 5));
  t.send("\r");
  EXPECT_EQ(t.finish(), "one\ntwo");
}

TEST(Display, RightPrompt) {
  Rig t(40);
  t.sh.params.set_scalar("RPROMPT", "[%B<R>%b]");
  t.type("ls");
  str row = t.screen.row(0);
  // ZLE_RPROMPT_INDENT: one column is left free at the right.
  EXPECT_EQ(row, "> ls" + str(30, ' ') + "[<R>]");
  EXPECT_EQ(t.screen.cursor(), s::make_pair(0, 4));
  // No room: it goes.
  t.send(str(34, 'x'));
  t.settle();
  EXPECT_EQ(t.screen.row(0).find("[<R>]"), str::npos);
  t.send("\r");
  t.finish();
}

TEST(Display, PromptSpMarksAnUnfinishedLine) {
  Rig t(20);
  t.type("x");
  // The mark, then spaces to the margin and back.
  EXPECT_NE(t.raw.find("%" "\x1b[27m"), str::npos) << t.raw;
  EXPECT_EQ(t.screen.row(0), "> x");
  t.send("\r");
  t.finish();
}

// ---- Keymaps, bindkey, zle -----------------------------------------------------------

TEST(Bindkey, ListingMatchesZsh) {
  Shell sh;
  EXPECT_EQ(sh.run("bindkey -M emacs '^A'").out, "\"^A\" beginning-of-line\n");
  EXPECT_EQ(sh.run("bindkey -M emacs | head -4").out,
            "\"^@\" set-mark-command\n\"^A\" beginning-of-line\n\"^B\" backward-char\n"
            "\"^D\" delete-char-or-list\n");
  EXPECT_EQ(sh.run("bindkey -M viins | head -2").out,
            "\"^A\"-\"^C\" self-insert\n\"^D\" list-choices\n");
  EXPECT_EQ(sh.run("bindkey -M emacs | grep -e ' self-insert$' -e '^\"\\^\\[\\[C'").out,
            "\"^[[C\" forward-char\n\" \"-\"~\" self-insert\n");
  EXPECT_EQ(sh.run("bindkey -M vicmd | grep -e '^\"\\\\\\$' -e '^\"\\\\^'").out,
            "\"\\$\" vi-end-of-line\n\"\\^\" vi-first-non-blank\n");
  EXPECT_EQ(sh.run("bindkey -l").out,
            ".safe\ncommand\nemacs\nisearch\nmain\nvicmd\nviins\nviopp\nvisual\n");
  EXPECT_EQ(sh.run("bindkey -M emacs -L '^A'").out, "bindkey -M emacs \"^A\" beginning-of-line\n");
}

TEST(Bindkey, BindUnbindAndMacros) {
  Shell sh;
  sh.run("bindkey -e");
  EXPECT_EQ(sh.run("bindkey -s '^Xa' hello; bindkey '^Xa'").out, "\"^Xa\" \"hello\"\n");
  EXPECT_EQ(sh.run("bindkey -L | grep -- 'bindkey -s'").out, "bindkey -s \"^Xa\" \"hello\"\n");
  EXPECT_EQ(sh.run("bindkey '\\eq' kill-line; bindkey '^[q'").out, "\"^[q\" kill-line\n");
  EXPECT_EQ(sh.run("bindkey -r '^A'; bindkey '^A'").out, "\"^A\" undefined-key\n");
  EXPECT_EQ(sh.run("bindkey -R 'a-c' beep; bindkey | grep beep").out, "\"a\"-\"c\" beep\n");
  EXPECT_EQ(sh.run("bindkey -v; bindkey -lL main").out, "bindkey -A viins main\n");
  EXPECT_EQ(sh.run("bindkey -N mine emacs; bindkey -M mine '^E'").out, "\"^E\" end-of-line\n");
  EXPECT_EQ(sh.run("bindkey -M nosuch").err, "jfsh: bindkey: no such keymap `nosuch'\n");
  sh.run("bindkey -d");
  EXPECT_EQ(sh.run("bindkey -M emacs '^A'").out, "\"^A\" beginning-of-line\n");
}

TEST(Bindkey, EveryDefaultBindingNamesAWidget) {
  Zle zle;
  for (const auto& [name, km] : zle.keymaps)
    for (const auto& [seq, b] : km->bindings)
      if (not b.is_macro) EXPECT_TRUE(zle.is_widget(b.widget)) << name << " " << format_key(seq) << " " << b.widget;
}

TEST(Bindkey, KeyNotation) {
  EXPECT_EQ(parse_key_notation("^A\\e\\M-x\\C-b^?\\n\\033\\x41"), "\x01\x1b\xf8\x02\x7f\n\x1b" "A");
  EXPECT_EQ(format_key("\x01\x1b[A\x7f \"$\\"), "^A^[[A^? \\\"\\$\\\\");
}

TEST(Bindkey, MacrosAndCustomBindingsDriveTheEditor) {
  Rig t;
  t.sh.run("bindkey -s '^Xh' 'hello '; bindkey '^Xk' kill-buffer");
  EXPECT_EQ(t.line("\x18hworld\r"), "hello world");
  EXPECT_EQ(t.line("junk\x18k" "ok\r"), "ok");
}

TEST(Zle, UserWidgets) {
  Rig t;
  t.sh.run("wrap() { BUFFER=\"[$BUFFER]\"; CURSOR=1 }; zle -N wrap; bindkey '^Xw' wrap");
  EXPECT_EQ(t.line("abc\x18wX\r"), "[Xabc]");
  t.sh.run("lr() { LBUFFER+=L; RBUFFER=R$RBUFFER }; zle -N lr; bindkey '^Xl' lr");
  EXPECT_EQ(t.line("ab\x02\x18l\r"), "aLRb");
  // A widget can run other widgets; their changes show at once.
  t.sh.run("eol() { zle end-of-line; LBUFFER+=\"<$WIDGET:$KEYS>\" }; zle -N eol; bindkey '^Xe' eol");
  EXPECT_EQ(t.line("ab\x01\x18" "e\r"), "ab<eol:\x18" "e>");
  // zle -N can rebind a builtin name; .name is still the builtin.
  t.sh.run("self-insert() { LBUFFER+=\"${KEYS}${KEYS}\" }; zle -N self-insert");
  EXPECT_EQ(t.line("ab\r"), "aabb");
  t.sh.run("zle -D self-insert");
  EXPECT_EQ(t.line("ab\r"), "ab");
  t.sh.run("num() { BUFFER=${NUMERIC:-none} }; zle -N num; bindkey '^Xn' num");
  EXPECT_EQ(t.line("\x1b" "5\x18n\r"), "5");
}

TEST(Zle, Builtin) {
  Shell sh;
  EXPECT_EQ(sh.run("zle -N foo; zle -N bar baz; zle -l").out, "bar (baz)\nfoo\n");
  EXPECT_EQ(sh.run("zle -lL").out, "zle -N bar baz\nzle -N foo\n");
  EXPECT_EQ(sh.run("zle -A beginning-of-line bol; zle -l bol && echo yes").out, "yes\n");
  EXPECT_EQ(sh.run("zle -D foo bar bol; zle -l").out, "");
  EXPECT_EQ(sh.run("zle -l -a | grep -c '^vi-'").out.empty(), false);
  auto r = sh.run("zle beginning-of-line");
  EXPECT_EQ(r.err, "jfsh: zle: widgets can only be called when ZLE is active\n");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(sh.run("vared nosuch").err, "jfsh: vared: no such variable: nosuch\n");
}

// ---- vi mode -----------------------------------------------------------------------

struct ViRig : Rig {
  ViRig() {
    sh.ctx.zle->select_main(true);
    // Esc alone needn't wait long for the rest of an escape sequence.
    sh.params.set_scalar("KEYTIMEOUT", "1");
  }
};

TEST(ViMode, MotionsAndOperators) {
  ViRig t;
  EXPECT_EQ(t.line("one two three\x1b" "0dw\r"), "two three");
  EXPECT_EQ(t.line("one two\x1b" "0cwONE\x1b\r"), "ONE two");
  EXPECT_EQ(t.line("one two\x1b" "dd\r"), "");
  EXPECT_EQ(t.line("abc\x1b" "0xp\r"), "bac");
  EXPECT_EQ(t.line("a.b.c\x1b" "0f.;x\r"), "a.bc");
  EXPECT_EQ(t.line("a.b.c\x1b" "0dt.\r"), ".b.c");
  EXPECT_EQ(t.line("abc\x1b" "0IX\x1b" "AY\x1b\r"), "XabcY");
  EXPECT_EQ(t.line("one two three\x1b" "0wD\r"), "one ");
  EXPECT_EQ(t.line("one two three\x1b" "0wC2\x1b\r"), "one 2");
  EXPECT_EQ(t.line("a b c d\x1b" "02dw\r"), "c d");
  EXPECT_EQ(t.line("a b c d\x1b" "0d2w\r"), "c d");
  EXPECT_EQ(t.line("hello\x1b" "0d$\r"), "");
  EXPECT_EQ(t.line("abc def\x1b" "bd0\r"), "def");
  EXPECT_EQ(t.line("foo.bar baz\x1b" "0dW\r"), "baz");
  EXPECT_EQ(t.line("abcd\x1b" "hhrx\r"), "axcd");
  EXPECT_EQ(t.line("abc\x1b" "0~~\r"), "ABc");
  EXPECT_EQ(t.line("abc def\x1b" "0gUiw\r"), "ABC def");
}

TEST(ViMode, TextObjectsAndVisual) {
  ViRig t;
  EXPECT_EQ(t.line("foo bar\x1b" "0ciwX\x1b\r"), "X bar");
  EXPECT_EQ(t.line("foo bar baz\x1b" "0wdaw\r"), "foo baz");
  EXPECT_EQ(t.line("echo \"abc\" x\x1b" "0f\"di\"\r"), "echo \"\" x");
  EXPECT_EQ(t.line("f(a, (b)) z\x1b" "0fada(\r"), "f z");
  EXPECT_EQ(t.line("f(a, (b)) z\x1b" "0f,ci(Q\x1b\r"), "f(Q) z");
  EXPECT_EQ(t.line("hello world\x1b" "0vlld\r"), "lo world");
  EXPECT_EQ(t.line("hello world\x1b" "0viwU\r"), "HELLO world");
  EXPECT_EQ(t.line("hello world\x1b" "0wvey0P\r"), "worldhello world");
}

TEST(ViMode, UndoRepeatRegisters) {
  ViRig t;
  EXPECT_EQ(t.line("abc\x1b" "0xu\r"), "abc");
  EXPECT_EQ(t.line("abc\x1b" "0xu\x12\r"), "bc");
  EXPECT_EQ(t.line("a b c\x1b" "0x.\r"), "b c");
  EXPECT_EQ(t.line("one two three\x1b" "0dw.\r"), "three");
  EXPECT_EQ(t.line("x y\x1b" "0ciwab\x1b" "w.\r"), "ab ab");
  // `.` repeats the latest change, an append here.
  EXPECT_EQ(t.line("x\x1b" "A y\x1b" "0.\r"), "x y y");
  EXPECT_EQ(t.line("one two\x1b" "0\"ayw$\"ap\r"), "one twoone ");
  EXPECT_EQ(t.line("abc\x1b" "0yl$p\r"), "abca");
  // The whole insert is one undo step.
  EXPECT_EQ(t.line("abc\x1b" "adef\x1bu\r"), "abc");
}

TEST(ViMode, HistoryAndModes) {
  ViRig t;
  t.hist.add("older");
  t.hist.add("newer");
  EXPECT_EQ(t.line("\x1bk\r"), "newer");
  EXPECT_EQ(t.line("\x1bkk\r"), "older");
  EXPECT_EQ(t.line("\x1b/old\r\r"), "older");
  EXPECT_EQ(t.line("x\x1b" "ccnew\r"), "new");
  // Esc steps back onto the last character.
  EXPECT_EQ(t.line("ab\x1bX\r"), "b");
  t.type("abc\x1b");
  EXPECT_EQ(t.screen.cursor(), s::make_pair(0, 4));
  t.send("\r");
  t.finish();
}

// ---- Completion ------------------------------------------------------------------------

// Wide: the scratch directory's path is long.
struct CompRig : Rig {
  TempDir dir;
  str d;
  CompRig() : Rig(240) {
    d = dir.path.string();
    auto rw = s::filesystem::perms::owner_read | s::filesystem::perms::owner_write;
    dir.file("alpha.txt", "", rw);
    dir.file("alpine.c", "", rw);
    dir.file("beta", "", rw | s::filesystem::perms::owner_exec);
    dir.file("my file", "", rw);
    dir.file(".hidden", "", rw);
    s::filesystem::create_directory(dir.path / "alps");
  }
};

TEST(Completion, B28CompletesTheWordAtTheCursor) {
  CompRig t;
  EXPECT_EQ(t.line("cat " + t.d + "/be\t\r"), "cat " + t.d + "/beta ");
  EXPECT_EQ(t.line("cat " + t.d + "/my\t\r"), "cat " + t.d + "/my\\ file ");
  EXPECT_EQ(t.line("cat '" + t.d + "/my\t\r"), "cat '" + t.d + "/my file' ");
  EXPECT_EQ(t.line("cd " + t.d + "/al\t\r"), "cd " + t.d + "/alps/");
  EXPECT_EQ(t.line("cat " + t.d + "/.h\t\r"), "cat " + t.d + "/.hidden ");
  // In the middle of the line: the word under the cursor.
  EXPECT_EQ(t.line("cat " + t.d + "/be | wc\x02\x02\x02\x02\x02\t\r"),
            "cat " + t.d + "/beta  | wc");
}

TEST(Completion, AmbiguousListsThenMenus) {
  CompRig t;
  t.type("cat " + t.d + "/al\t");
  EXPECT_EQ(t.screen.row(0), "> cat " + t.d + "/alp");
  t.send("\t");   // nothing more to add: the list
  t.settle();
  EXPECT_EQ(t.screen.row(1), "alpha.txt  alpine.c   alps/");
  t.send("\t");   // and then the menu
  t.settle();
  EXPECT_EQ(t.screen.row(0), "> cat " + t.d + "/alpha.txt");
  t.send("\t");
  t.settle();
  EXPECT_EQ(t.screen.row(0), "> cat " + t.d + "/alpine.c");
  t.send("\x1b[Z");   // shift-tab: back
  t.settle();
  EXPECT_EQ(t.screen.row(0), "> cat " + t.d + "/alpha.txt");
  t.send("\r");
  EXPECT_EQ(t.finish(), "cat " + t.d + "/alpha.txt");
}

TEST(Completion, CommandsParametersUsers) {
  CompRig t;
  t.sh.run("jfsh_cmd_one() {}; alias jfsh_alias_x=ls");
  EXPECT_EQ(t.line("jfsh_cmd_o\t\r"), "jfsh_cmd_one ");
  EXPECT_EQ(t.line("jfsh_alias_\t\r"), "jfsh_alias_x ");
  EXPECT_EQ(t.line("ls; jfsh_cmd\t\r"), "ls; jfsh_cmd_one ");
  EXPECT_EQ(t.line("x=1 jfsh_cmd\t\r"), "x=1 jfsh_cmd_one ");
  EXPECT_EQ(t.line(t.d + "/be\t\r"), t.d + "/beta ");   // ./script style: executables
  t.sh.params.set_scalar("JFSH_TEST_VAR", "1");
  EXPECT_EQ(t.line("echo $JFSH_TEST_V\t\r"), "echo $JFSH_TEST_VAR");
  EXPECT_EQ(t.line("echo ${JFSH_TEST_V\t\r"), "echo ${JFSH_TEST_VAR}");
  EXPECT_EQ(t.line("echo '$JFSH_TEST_V\t\r"), "echo '$JFSH_TEST_V");
  EXPECT_EQ(t.line("cat < " + t.d + "/be\t\r"), "cat < " + t.d + "/beta ");
}

TEST(Completion, CompdefFunctions) {
  CompRig t;
  t.sh.run("_mycmd() { compadd foo bar baz }; compdef _mycmd mycmd");
  EXPECT_EQ(t.line("mycmd f\t\r"), "mycmd foo ");
  EXPECT_EQ(t.line("mycmd b\t\r"), "mycmd ba");
  t.sh.run("_wc() { compadd -S = -- \"w$CURRENT\" \"${words[2]}x\" }; compdef _wc wcmd");
  EXPECT_EQ(t.line("wcmd a w\t\r"), "wcmd a w3=");
  t.sh.run("_onlydirs() { _directories }; compdef _onlydirs od");
  EXPECT_EQ(t.line("od " + t.d + "/al\t\r"), "od " + t.d + "/alps/");
  t.sh.run("_c() { _files -g '*.c' }; compdef _c cc1");
  EXPECT_EQ(t.line("cc1 " + t.d + "/alpi\t\r"), "cc1 " + t.d + "/alpine.c ");
  EXPECT_EQ(t.sh.run("compdef").out, "cc1 _c\nmycmd _mycmd\nod _onlydirs\nwcmd _wc\n");
  EXPECT_EQ(t.sh.run("compadd x").err, "jfsh: compadd: can only be called from completion function\n");
}

TEST(CompletionEngine, Context) {
  Shell sh;
  CompletionEngine& e = *sh.ctx.zle->completion;
  auto c = e.build_context("ls -l 'my fi", 12);
  EXPECT_EQ(c.command, "ls");
  EXPECT_EQ(c.prefix, "my fi");
  EXPECT_EQ(c.quote, '\'');
  EXPECT_EQ(c.current_arg, 2);
  EXPECT_FALSE(c.command_position);
  c = e.build_context("echo a | gr", 11);
  EXPECT_TRUE(c.command_position);
  c = e.build_context("sort > ou", 9);
  EXPECT_TRUE(c.redirection);
  c = e.build_context("if tr", 5);
  EXPECT_TRUE(c.command_position);
  c = e.build_context("echo $(ca", 9);
  EXPECT_TRUE(c.command_position);
  c = e.build_context("echo ab\\ cd ef", 9);
  EXPECT_EQ(c.prefix, "ab ");
  EXPECT_EQ(c.suffix, "cd");
  EXPECT_EQ(CompletionEngine::quote_word("a b$c*", 0), "a\\ b\\$c\\*");
  EXPECT_EQ(CompletionEngine::quote_word("it's", '\''), "it'\\''s");
  EXPECT_EQ(CompletionEngine::quote_word("~x#", 0), "\\~x#");
}

TEST(CompletionEngine, B52CommandsAreExecutableAndUnique) {
  Shell sh;
  TempDir a, b;
  auto rwx = s::filesystem::perms::owner_all;
  auto rw  = s::filesystem::perms::owner_read | s::filesystem::perms::owner_write;
  a.file("jfsh_tool", "", rwx);
  b.file("jfsh_tool", "", rwx);
  a.file("jfsh_tool_noexec", "", rw);
  sh.params.set_scalar("PATH", a.path.string() + ":" + b.path.string());
  auto r = sh.ctx.zle->completion->complete("jfsh_to", 7);
  ASSERT_EQ(r.matches.size(), 1u);
  EXPECT_EQ(r.matches[0].word, "jfsh_tool");
}

}  // namespace
}  // namespace jfsh
