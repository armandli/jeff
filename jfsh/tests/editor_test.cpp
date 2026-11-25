// The line editor (M9), in process, through the pty rig in editor_rig.h.
// Key bindings and behaviour follow zsh 5.9's (`zsh -f`).

#include <chrono>
#include <string>
#include <string_view>

#include <editor_rig.h>
#include <gtest/gtest.h>
#include <shell_fixture.h>

#include <jfsh/core/aliases.h>
#include <jfsh/editor/completion.h>
#include <jfsh/editor/editor.h>

namespace jfsh {
namespace {

using namespace s::chrono_literals;
using namespace s::string_literals;
using test::Rig;
using test::Shell;
using test::TempDir;

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

// zsh/deltochar (M11): delete-to-char kills through the character read,
// zap-to-char up to it; a numeric argument picks the nth, a negative one
// looks backwards.
TEST(Editor, DeltocharWidgets) {
  Rig t;
  ASSERT_EQ(t.sh.run("zmodload zsh/deltochar; bindkey '^Xd' delete-to-char; bindkey '^Xz' zap-to-char").rc, 0);
  EXPECT_EQ(t.line("one,two,three\x01\x18" "d,\r"), "two,three");
  EXPECT_EQ(t.line("one,two,three\x01\x18" "z,\r"), ",two,three");
  EXPECT_EQ(t.line("one,two,three\x01\x1b" "2\x18" "d,\r"), "three");
  EXPECT_EQ(t.line("one,two,three\x1b-\x18" "d,\r"), "one,two");
  EXPECT_EQ(t.line("one,two,three\x1b-\x18" "z,\r"), "one,two,");
  // No such character: the line stays.
  EXPECT_EQ(t.line("abc\x01\x18" "dq\r"), "abc");
  // Killed text can be yanked back.
  EXPECT_EQ(t.line("ab-cd\x01\x18" "d-\x05\x19\r"), "cdab-");
  EXPECT_EQ(t.sh.run("zmodload -u zsh/deltochar").rc, 0);
  EXPECT_FALSE(t.sh.ctx.zle->is_widget("zap-to-char"));
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
  EXPECT_EQ(t.line("abc\x1f\r"), "ab");        // each key typed is a step (zsh 5.9)
  EXPECT_EQ(t.line("abc def\x17\x1f\r"), "abc def");
  EXPECT_EQ(t.line("ab\x18u\r"), "a");         // ^Xu
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
  EXPECT_FALSE(t.ed->eof_ignored());
  // IGNORE_EOF: the read ends all the same and says so; the shell counts
  // the ignored EOFs (zsh's zleread).
  t.sh.opts.set(OPT_IGNOREEOF);
  EXPECT_EQ(t.line("\x04"), s::nullopt);
  EXPECT_TRUE(t.ed->eof_ignored());
  EXPECT_EQ(t.line("x\r"), "x");
  EXPECT_FALSE(t.ed->eof_ignored());
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
  EXPECT_EQ(t.line("cp a \x1b\x1f\r"), "cp a a ");   // zsh copies up to the cursor
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
            "\"^[[C\" forward-char\n\" \"-\"~\" self-insert\n\"\\M-^@\"-\"\\M-^?\" self-insert\n");
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
  // zsh's printbind: bindztrdup in dquotedztrdup's quotes.
  EXPECT_EQ(format_key("\x01\x1b[A\x7f \"$\\"), "\"^A^[[A^? \\\"\\$\\\\\\\\\"");
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
  // An alias of a builtin widget is a builtin one: only -a lists it.
  EXPECT_EQ(sh.run("zle -A beginning-of-line bol; zle -l bol || zle -la bol && echo yes").out,
            "yes\n");
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
  EXPECT_EQ(t.line("cat " + t.d + "/be\t\r"), "cat " + t.d + "/beta");
  EXPECT_EQ(t.line("cat " + t.d + "/my\t\r"), "cat " + t.d + "/my\\ file");
  EXPECT_EQ(t.line("cat '" + t.d + "/my\t\r"), "cat '" + t.d + "/my file'");
  EXPECT_EQ(t.line("cd " + t.d + "/al\t\r"), "cd " + t.d + "/alps");
  EXPECT_EQ(t.line("cat " + t.d + "/.h\t\r"), "cat " + t.d + "/.hidden");
  // In the middle of the line: the word under the cursor.
  EXPECT_EQ(t.line("cat " + t.d + "/be | wc\x02\x02\x02\x02\x02\t\r"),
            "cat " + t.d + "/beta | wc");
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
  EXPECT_EQ(t.line("jfsh_cmd_o\t\r"), "jfsh_cmd_one");
  EXPECT_EQ(t.line("jfsh_alias_\t\r"), "jfsh_alias_x");
  EXPECT_EQ(t.line("ls; jfsh_cmd\t\r"), "ls; jfsh_cmd_one");
  EXPECT_EQ(t.line("x=1 jfsh_cmd\t\r"), "x=1 jfsh_cmd_one");
  EXPECT_EQ(t.line(t.d + "/be\t\r"), t.d + "/beta");   // ./script style: executables
  t.sh.params.set_scalar("JFSH_TEST_VAR", "1");
  EXPECT_EQ(t.line("echo $JFSH_TEST_V\t\r"), "echo $JFSH_TEST_VAR");
  EXPECT_EQ(t.line("echo ${JFSH_TEST_V\t\r"), "echo ${JFSH_TEST_VAR}");
  EXPECT_EQ(t.line("echo '$JFSH_TEST_V\t\r"), "echo '$JFSH_TEST_V");
  EXPECT_EQ(t.line("cat < " + t.d + "/be\t\r"), "cat < " + t.d + "/beta");
}

TEST(Completion, CompdefFunctions) {
  CompRig t;
  t.sh.run("_mycmd() { compadd foo bar baz }; compdef _mycmd mycmd");
  EXPECT_EQ(t.line("mycmd f\t\r"), "mycmd foo");
  EXPECT_EQ(t.line("mycmd b\t\r"), "mycmd ba");
  t.sh.run("_wc() { compadd -S = -- \"w$CURRENT\" \"${words[2]}x\" }; compdef _wc wcmd");
  EXPECT_EQ(t.line("wcmd a w\t\r"), "wcmd a w3=");
  t.sh.run("_onlydirs() { _directories }; compdef _onlydirs od");
  EXPECT_EQ(t.line("od " + t.d + "/al\t\r"), "od " + t.d + "/alps");
  t.sh.run("_c() { _files -g '*.c' }; compdef _c cc1");
  EXPECT_EQ(t.line("cc1 " + t.d + "/alpi\t\r"), "cc1 " + t.d + "/alpine.c");
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
  EXPECT_EQ(CompletionEngine::quote_word("~x#", 0), "\\~x\\#");
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

// ---- M9 review: the widgets as zsh 5.9's ------------------------------------
//
// Expected values from zsh -f -i in a pty (scratchpad m9/p_tv.py).

TEST(EditorM9Review, NegativeCountsRunTheOtherWay) {
  Rig t;
  EXPECT_EQ(t.line("abc\x01\x1b-\x02X\r"), "aXbc");
  EXPECT_EQ(t.line("echo foo bar\x1b-\x1b" "d\r"), "echo foo ");
  EXPECT_EQ(t.line("echo foo bar\x02\x02\x02\x1b-\x0b\r"), "bar");
  // self-insert with a negative count inserts after the cursor.
  EXPECT_EQ(t.line("ab\x01\x1b-xY\r"), "Yxab");
  EXPECT_EQ(t.line("a\x1bxuniversal-argument\r3x\r"), "axxx");
  EXPECT_EQ(t.line("a\x1bxuniversal-argument\rx\r"), "axxxx");
}

TEST(EditorM9Review, UndoIsPerWidget) {
  Rig t;
  EXPECT_EQ(t.line("abc\x1f\r"), "ab");
  EXPECT_EQ(t.line("echo abc\x01\x1b" "d\x05 x\x1f\x1f\r"), " abc");
  // Each step is what changed, so a long line typed costs little.
  str many(300, 'x');
  EXPECT_EQ(t.line(many + "\x1f\x1f\r"), str(298, 'x'));
}

TEST(EditorM9Review, TransposeCopyAndDelete) {
  Rig t;
  EXPECT_EQ(t.line("abc\x01\x14\r"), "bac");
  EXPECT_EQ(t.line("a b c\x1b" "2\x1b\x1f\r"), "a b cb ");
  EXPECT_EQ(t.line("cp a \x1b\x1f\r"), "cp a a ");
  // delete-word doesn't go to the kill ring.
  EXPECT_EQ(t.line("echo foo bar\x01\x1bxdelete-word\r\x19\r"), " foo bar");
  EXPECT_EQ(t.line("abc\x1bxgosmacs-transpose-chars\r\r"), "acb");
}

TEST(EditorM9Review, KillRingAsZsh) {
  Rig t;
  EXPECT_EQ(t.line("one two three\x01\x1b" "d\x06\x1b" "d\x06\x1b" "d\x05\x19\x1by\x1by\r"),
            "  one");
}

TEST(EditorM9Review, MessagesAndNamedCommands) {
  Rig t;
  t.type("echo foo\x01\x06\x18=");
  EXPECT_EQ(t.screen.row(1), "Char: c (0143, 99, 0x63)  point 2 of 9(12%)  column 1");
  t.send("\x03");
  t.finish();
  t.type("echo foo\x18=");
  EXPECT_EQ(t.screen.row(1), "EOF  point 9 of 9(100%)  column 8");
  t.send("\x03");
  t.finish();
  // execute-named-cmd completes the name.
  EXPECT_EQ(t.line("abc\x1bxbackward-ch\t\rX\r"), "abXc");
  t.sh.run("k() { zle -M \"$KEYMAP\" }; zle -N k; bindkey '^Xk' k");
  t.type("\x18k");
  EXPECT_EQ(t.screen.row(1), "main");
  t.send("\x03");
  t.finish();
}

TEST(EditorM9Review, TabsShowAsBlanks) {
  Rig t;
  t.type("echo \x16\tx");
  EXPECT_EQ(t.screen.row(0), "> echo  x");
  t.send("\x03");
  t.finish();
}

TEST(EditorM9Review, ViInsertStopsWhereTheInsertBegan) {
  ViRig t;
  EXPECT_EQ(t.line("echo foo bar\x1b" "a\x17\r"), "echo foo bar");
  EXPECT_EQ(t.line("echo foo bar\x1b" "A baz\x15\r"), "echo foo bar");
}

TEST(EditorM9Review, ViRegistersLinesAndIndent) {
  ViRig t;
  // "1 is the last deletion, "2 the one before, "0 the last yank.
  EXPECT_EQ(t.line("one two three\x1b" "0dwdw\"2p\r"), "tone hree");
  EXPECT_EQ(t.line("one two three\x1b" "0dwdw\"1p\r"), "ttwo hree");
  EXPECT_EQ(t.line("one two three\x1b" "0ywdw\"0P\r"), "one two three");
  EXPECT_EQ(t.line("echo a\x16\necho b\x1bkVd\r"), "echo b");
  // Indenting needs lines: within a line it does nothing.
  EXPECT_EQ(t.line("echo a b\x1b" "0w>w\r"), "echo a b");
  EXPECT_EQ(t.line("echo a\x16\necho b\x1bk>j\r"), "\techo a\n\techo b");
  EXPECT_EQ(t.line("echo a b\x1b" "03$x\r"), "cho a b");
}

TEST(EditorM9Review, BindkeyAsBinBindkey) {
  Shell sh;
  // printbind: bindztrdup inside dquotedztrdup's quotes.
  EXPECT_EQ(sh.run("bindkey '\\\\' backward-char; bindkey -L '\\\\'; bindkey '^\\\\'").out,
            "bindkey \"\\\\\\\\\" backward-char\n\"^\\\\\\\\\" undefined-key\n");
  Shell fresh;
  EXPECT_EQ(fresh.run("bindkey -M viins | grep '\"~\"'").out, "\"^\\\\\\\\\"-\"~\" self-insert\n");
  EXPECT_EQ(fresh.run("bindkey -LM vicmd | grep -e ' \"a\" ' -e '\"gUU\"'").out,
            "bindkey -a \"a\" vi-add-next\nbindkey -s -a \"gUU\" \"gUgU\"\n");
  EXPECT_EQ(fresh.run("bindkey -LM emacs | grep -e '^bindkey -R'").out,
            "bindkey -R -M emacs \" \"-\"~\" self-insert\n"
            "bindkey -R -M emacs \"\\M-^@\"-\"\\M-^?\" self-insert\n");
  EXPECT_EQ(sh.run("bindkey -lL | head -2").out, "bindkey -N command\nbindkey -N emacs\n");
  EXPECT_EQ(sh.run("bindkey -M .safe '^A'").out, "\"^A\" .self-insert\n");
  EXPECT_EQ(sh.run("bindkey -M .safe x y").err, "jfsh: bindkey: keymap `.safe' is protected\n");
  EXPECT_EQ(sh.run("bindkey -e -v").err, "jfsh: bindkey: incompatible keymap selection options\n");
  EXPECT_EQ(sh.run("bindkey -rR 'a-c'; bindkey a").out, "\"a\" undefined-key\n");
  EXPECT_EQ(sh.run("bindkey a b c").err, "jfsh: bindkey: even number of arguments required\n");
  EXPECT_EQ(sh.run("bindkey -M command").out,
            "\"^G\" send-break\n\"^J\" accept-line\n\"^M\" accept-line\n");
}

TEST(EditorM9Review, ZleAsBinZle) {
  Shell sh;
  EXPECT_EQ(sh.run("zle -A forward-char fc2; zle -l fc2").rc, 1);   // a builtin one
  EXPECT_EQ(sh.run("zle -D forward-char; zle -la | grep -c '^forward-char$'").out, "0\n");
  EXPECT_EQ(sh.run("zle -la .forward-char").rc, 0);
  auto r = sh.run("zle -M hi");
  EXPECT_EQ(r.err, "jfsh: zle: can only be called from widget function\n");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(sh.run("zle -N").err, "jfsh: zle: not enough arguments for -N\n");
  EXPECT_EQ(sh.run("zle -R").rc, 1);
  EXPECT_EQ(sh.run("zle").rc, 1);
  EXPECT_EQ(sh.run("zle -N .f g; zle -l").out, ".f (g)\n");
}

// ---- M9 review, round 1 ------------------------------------------------------
//
// Expected values from zsh -f -i in a pty (scratchpad m9rv1/p/r*.py and
// m9rv1/p2_*.py).

// zleread's viinsert_init: with main as viins, what is typed before the
// first Esc is one undo step, at every prompt.
TEST(EditorM9Review, ViUndoAtAFreshPrompt) {
  ViRig t;
  EXPECT_EQ(t.line("abc\x1bu\r"), "");
  EXPECT_EQ(t.line("abc\x1buAqq\x1bu\r"), "");
  EXPECT_EQ(t.line("日本é\x1bu\r"), "");
  EXPECT_EQ(t.line("echo \"a\x16\nb\"\x1bu\r"), "");
}

// Undo knows the history line each change was made in (zsh's ch->hist):
// moving between lines is no change, and text a read starts with is one.
TEST(EditorM9Review, UndoFollowsHistoryLines) {
  Rig t;
  t.hist.add("echo one");
  EXPECT_EQ(t.line("\x10Z\x0e\x10\x1f\r"), "echo one");
  EXPECT_EQ(t.line("abc\x10\x1f\x1f\r"), "ab");
  t.pending = s::async(s::launch::async, [&t] { return t.ed->edit("abc", "", ""); });
  t.send("\x1f\x1f\x1fZ\r");
  EXPECT_EQ(t.finish(), "Z");
}

// zsh's upline/downline: counts, and the column a run of moves keeps to.
TEST(EditorM9Review, LineMovesTakeCountsAndKeepTheColumn) {
  Rig t;
  EXPECT_EQ(t.line("echo \"a\x16\nb\x16\nc\"\x1b<\x1b" "2\x0eX\r"), "echo \"a\nb\nXc\"");
  EXPECT_EQ(t.line("echo \"ab\x16\ncdef\x16\ng\x16\nhijk\"\x1b<\x06\x06\x06\x0e\x0e\x0eX\r"),
            "echo \"ab\ncdef\ng\nhijXk\"");
  t.hist.add("echo one");
  t.hist.add("echo two");
  EXPECT_EQ(t.line("echo \"a\x16\nb\"\x1b" "3\x10X\r"), "echo oneX");
}

// copy-prev-shell-word takes the word bufferwords finds at the cursor.
TEST(EditorM9Review, CopyPrevShellWordAsBufferwords) {
  Rig t;
  t.sh.run("bindkey '^T' copy-prev-shell-word");
  EXPECT_EQ(t.line("echo \"a b\" \x14\r"), "echo \"a b\" \"a b\"");
  EXPECT_EQ(t.line("echo a\x01\x14\r"), "echoecho a");
  EXPECT_EQ(t.line("echo abc def\x01\x1b" "f\x1b" "f\x1b" "b\x14\r"), "echo echoabc def");
  EXPECT_EQ(t.line("echo a b c\x1b" "3\x14\r"), "echo a b cecho");
  EXPECT_EQ(t.line("echo a; ls \x14\r"), "echo a; ls ls");
}

// The other widgets the review found wrong.
TEST(EditorM9Review, WidgetsAsZshHasThem) {
  Rig t;
  t.sh.run("bindkey '^T' argument-base");
  EXPECT_EQ(t.line("\x1b" "1\x1b" "6\x14\x1b" "1\x1b" "0x\r"), str(16, 'x'));
  t.sh.run("bindkey '^T' vi-caps-lock-panic");
  t.type("ab\x14");
  EXPECT_EQ(t.screen.row(1), "press a lowercase key to continue");
  t.send("QÉa");
  t.send("c\r");
  EXPECT_EQ(t.finish(), "abc");
  t.sh.run("bindkey '^T' describe-key-briefly");
  t.type("\x14\x01");
  EXPECT_EQ(t.screen.row(1), "\"^A\" is beginning-of-line");
  t.send("\x03");
  t.finish();
  // A character that inserts itself is read whole (getrestchar) and
  // nothing goes into the line.
  t.type("\x14日");
  EXPECT_EQ(t.screen.row(1), "\"\\M-f\\M-^W\\M-%\" is self-insert");
  t.send("\r");
  EXPECT_EQ(t.finish(), "");
  t.type("\x14é");
  EXPECT_EQ(t.screen.row(1), "\"\\M-C\\M-)\" is self-insert");
  t.send("\r");
  EXPECT_EQ(t.finish(), "");
  t.sh.run("bindkey '^T' where-is");
  t.type("\x14" "forward-char\r");
  EXPECT_EQ(t.screen.row(1), "forward-char is on \"^F\" \"^[OC\" \"^[[C\"");
  t.send("\x03");
  t.finish();
  // accept-and-infer-next-history: the entry after the one like the line.
  t.hist.add("echo one");
  t.hist.add("echo two");
  t.sh.run("bindkey '^T' accept-and-infer-next-history");
  EXPECT_EQ(t.line("\x10\x10\x14"), "echo one");
  EXPECT_EQ(t.line("\r"), "echo two");
  // Tab with only blanks before it on its line inserts itself (usetab).
  EXPECT_EQ(t.line("\tx\r"), "\tx");
  EXPECT_EQ(t.line("echo \"a\x16\n\tb\"\r"), "echo \"a\n\tb\"");
}

// The cut buffer, kill ring and vi registers are the shell's, not one
// editor's (vared shares them); $killring shows the ring, newest first.
TEST(EditorM9Review, KillRingIsGlobal) {
  Rig t;
  EXPECT_EQ(t.line("abc def\x17\r"), "abc ");
  t.ed = s::make_unique<Editor>(t.sh.opts, t.hist, t.prompt, *t.sh.ctx.zle);
  t.ed->set_io(t.slave, t.slave);
  EXPECT_EQ(t.line("\x19\r"), "def");
  t.sh.run("w(){ BUFFER=\"${(j:,:)killring}|$CUTBUFFER|${#killring}\" }; zle -N w; "
           "bindkey '^T' w");
  EXPECT_EQ(t.line("echo a1 a2\x17\x02\x17\x14\r"), "a2,def,,,,,,|a1|8");
  t.sh.run("w(){ killring=(k1 k2); CUTBUFFER=c0 }; zle -N w");
  EXPECT_EQ(t.line("\x14\x19\x1by\x1by\r"), "k2");
  // unset killring empties the ring and unsets the name (stdunsetfn).
  t.sh.run("w(){ unset killring; BUFFER=\"[${(t)killring}] ${#killring}\" }; zle -N w");
  EXPECT_EQ(t.line("\x14\r"), "[] 0");
}

// $WIDGET is the key's widget (zsh's bindk); `zle name` changes it only
// for a builtin widget or with -w, and -f nolast keeps $LASTWIDGET.
// $LASTWIDGET goes on from the last line.
TEST(EditorM9Review, WidgetAndLastWidget) {
  Rig t;
  t.sh.run("inr(){ X=$WIDGET }; out(){ zle inr; BUFFER=\"[$WIDGET|$X]\" }; "
           "zle -N inr; zle -N out; bindkey '^T' out");
  EXPECT_EQ(t.line("\x14\r"), "[out|out]");
  t.sh.run("out(){ zle inr -w; BUFFER=\"[$WIDGET|$X]\" }");
  EXPECT_EQ(t.line("\x14\r"), "[out|inr]");
  t.sh.run("out(){ zle inr -f nolast; BUFFER=\"[$WIDGET|$LASTWIDGET]\" }");
  EXPECT_EQ(t.line("ab\x14\r"), "[out|self-insert]");
  t.sh.run("out(){ BUFFER=\"[$LASTWIDGET]\" }");
  EXPECT_EQ(t.line("\x14\r"), "[accept-line]");
  // A key that ends an isearch runs after it (zsh pushes it back).
  t.hist.add("echo abc");
  t.sh.run("out(){ BUFFER=\"[$WIDGET|$LASTWIDGET]\" }");
  EXPECT_EQ(t.line("\x12" "abc\x14\r"), "[out|history-incremental-search-backward]");
}

// The key that ends an isearch is read again after it (ungetkeycmd), so
// its widget runs from the main loop: $LASTWIDGET is the search's caller
// and then the key's widget; the search's keys are read in the main
// keymap with the isearch keymap over it.
TEST(EditorM9Review, IsearchEndsByPushingItsKeyBack) {
  Rig t;
  t.hist.add("echo abc");
  t.sh.run("out(){ BUFFER=\"[$WIDGET|$LASTWIDGET|$CURSOR|$KEYMAP|$BUFFER]\" }; zle -N out; "
           "bindkey '^Y' out");
  EXPECT_EQ(t.line("\x12" "ab\x02\x19\r"), "[out|backward-char|4|main|echo abc]");
  EXPECT_EQ(t.line("\x12" "ab\x06\x19\r"), "[out|forward-char|6|main|echo abc]");
  // A numeric argument goes to the search, not to the key after it.
  EXPECT_EQ(t.line("\x1b" "2\x12" "ab\x02\x19\r"), "[out|backward-char|4|main|echo abc]");
  // A string binding's keys go on into the search.
  t.sh.run("bindkey -s '^T' c");
  EXPECT_EQ(t.line("\x12" "ab\x14\x19\r"),
            "[out|history-incremental-search-backward|5|main|echo abc]");
  // An unbound key runs undefined-key.
  t.sh.run("bindkey -r '^V'");
  EXPECT_EQ(t.line("\x12" "ab\x16\x19\r"), "[out|undefined-key|5|main|echo abc]");
  // Started from a user widget, the search leaves that one as lbindk.
  t.sh.run("my(){ zle .history-incremental-search-backward }; zle -N my; bindkey '^R' my");
  EXPECT_EQ(t.line("\x12" "ab\x19\r"), "[out|my|5|main|echo abc]");
  t.sh.run("bindkey '^R' history-incremental-search-backward");
  // A lower-case letter matches either case, unless a count is given.
  t.hist.add("echo ABC");
  EXPECT_EQ(t.line("\x12" "ab\x19\r"),
            "[out|history-incremental-search-backward|5|main|echo ABC]");
  EXPECT_EQ(t.line("\x1b" "2\x12" "ab\x19\r"),
            "[out|history-incremental-search-backward|5|main|echo abc]");
  // vi: Esc in the search switches the keymap it reads with; the keymap
  // the search began in comes back after it.
  t.sh.run("bindkey -v; bindkey -M viins '^Y' out; bindkey -M vicmd '^Y' out; "
           "bindkey -M viins '^R' history-incremental-search-backward");
  EXPECT_EQ(t.line("\x12" "ab\x1b\x19\r"),
            "[out|history-incremental-search-backward|5|main|echo ABC]");
  t.sh.run("bindkey -e");
  // accept-and-hold leaves the cursor where it was for the next line.
  t.sh.run("bindkey '^Y' out");
  EXPECT_EQ(t.line("echo abc\x02\x02\x1b" "a"), "echo abc");
  EXPECT_EQ(t.line("\x19\r"), "[out|accept-and-hold|6|main|echo abc]");
  // execute-named-cmd: the widget named is the key's.
  EXPECT_EQ(t.line("abc\x1bxbackward-char\r\x19\r"), "[out|backward-char|2|main|abc]");
}

// doisearch's other branches: a paste and a quoted key go into the
// search, the vi repeats search again (the first way, or the other), the
// pattern widgets repeat as the plain ones do; a line the same as the one
// a step starts on is passed over.
TEST(EditorM9Review, IsearchPasteQuotedAndRepeats) {
  Rig t;
  for (const char* l : {"echo xab", "echo abc", "echo abd"}) t.hist.add(l);
  t.sh.run("out(){ BUFFER=\"[$LASTWIDGET|$BUFFER|$CURSOR]\" }; zle -N out; bindkey '^Y' out");
  const str isearch = "history-incremental-search-backward";
  EXPECT_EQ(t.line("\x12\x1b[200~bc\x1b[201~\x19\r"), "[" + isearch + "|echo abc|6]");
  EXPECT_EQ(t.line("\x12\x1b[200~abc\x1b[201~\x08\x19\r"), "[" + isearch + "||0]");
  EXPECT_EQ(t.line("\x12" "a\x16" "b\x19\r"), "[" + isearch + "|echo abd|5]");
  t.sh.run("bindkey '^T' history-incremental-pattern-search-backward");
  EXPECT_EQ(t.line("\x12" "ab\x14\x19\r"), "[" + isearch + "|echo abc|5]");
  // The other way starts from the match, and a forward match leaves the
  // cursor at its end.
  t.sh.run("bindkey '^T' history-incremental-pattern-search-forward");
  EXPECT_EQ(t.line("\x12" "ab\x12\x14\x19\r"), "[" + isearch + "|echo abc|7]");
  t.sh.run("bindkey -v; bindkey -M viins '^Y' out; bindkey -M vicmd '^Y' out; "
           "bindkey -M viins '^R' " + isearch);
  EXPECT_EQ(t.line("\x12" "ab\x1bn\x19\r"), "[" + isearch + "|echo abc|5]");
  EXPECT_EQ(t.line("\x12" "ab\x12\x12\x1bN\x19\r"), "[" + isearch + "|echo abc|7]");
  t.sh.run("bindkey -e; bindkey '^Y' out");
  Rig d;
  for (const char* l : {"echo xab", "echo ab", "echo ab"}) d.hist.add(l);
  d.sh.run("out(){ BUFFER=\"[$BUFFER|$CURSOR]\" }; zle -N out; bindkey '^Y' out");
  EXPECT_EQ(d.line("\x12" "ab\x12\x19\r"), "[echo xab|6]");
}

// A character typed into the search goes in whole (getrestchar,
// getfullchar), so a backspace takes off a whole character; the search's
// text is drawn on the status line, its control characters as ^X.
TEST(EditorM9Review, IsearchWholeCharacters) {
  Rig t;
  for (const char* l : {"echo aéb", "echo 日本語"}) t.hist.add(l);
  t.sh.run("out(){ BUFFER=\"[$BUFFER|$CURSOR]\" }; zle -N out; bindkey '^Y' out");
  EXPECT_EQ(t.line("\x12本語\x08\x19\r"), "[echo 日本語|6]");
  EXPECT_EQ(t.line("\x12本\x08\x19\r"), "[|0]");
  EXPECT_EQ(t.line("\x12本語\x08\x08\x19\r"), "[|0]");
  EXPECT_EQ(t.line("\x12x本\x08\x19\r"), "[|0]");
  EXPECT_EQ(t.line("\x12" "a\x16é\x19\r"), "[echo aéb|5]");
  t.type("\x12本語\x08");
  EXPECT_EQ(t.screen.row(1), "bck-i-search: 本_");
  t.send("\x07\r");
  t.finish();
  t.type("\x12" "a\x16\x0a");
  EXPECT_EQ(t.screen.row(1), "failing bck-i-search: a^J_");
  t.send("\x07\r");
  t.finish();
  t.type("\x12" "a\x16\x09");
  EXPECT_EQ(t.screen.row(1), "failing bck-i-search: a^I_");
  t.send("\x07\r");
  t.finish();
  t.type("abc\x1bx\x16\x01");
  EXPECT_EQ(t.screen.row(0), "> abc");
  EXPECT_EQ(t.screen.row(1), "execute: ^A_");
  t.send("\x07\r");
  t.finish();
  t.sh.run("bindkey -v; bindkey -M viins '^Y' out; "
           "bindkey -M viins '^R' history-incremental-search-backward");
  EXPECT_EQ(t.line("\x12本語\x08\x19\r"), "[echo 日本語|6]");
}

// zle -M is drawn as showmsg draws it (nicechar: `\t`, `^X`, `\M-…` for a
// byte that is no character, newlines kept).
TEST(EditorM9Review, MessageAsShowmsg) {
  Rig t;
  auto shown = [&](const char* msg) {
    t.sh.run(str("m(){ zle -M $'") + msg + "' }; zle -N m; bindkey '^T' m");
    t.type("\x14");
    str rows = t.screen.row(1) + "|" + t.screen.row(2);
    t.send("\r");
    t.finish();
    return rows;
  };
  EXPECT_EQ(shown("a\\tb"), "a\\tb|");
  EXPECT_EQ(shown("a\\nb"), "a|b");
  EXPECT_EQ(shown("a\\x01b\\x7fc"), "a^Ab^?c|");
  EXPECT_EQ(shown("a\\x80b\\xffc"), "a\\M-^@b\\M-^?c|");
  EXPECT_EQ(shown("a\\u0085b"), "a\\M-^Eb|");
}

// zsh's exchangepointandmark: a mark never set is 0, a count of 0 only
// activates the region, a negative one swaps without it; undefined-key
// fails.
TEST(EditorM9Review, ExchangePointAndMarkAndUndefinedKey) {
  Rig t;
  t.sh.run("out(){ BUFFER=\"[$BUFFER|$CURSOR|$MARK|$REGION_ACTIVE]\" }; zle -N out; "
           "bindkey '^Y' out");
  EXPECT_EQ(t.line("abc\x18\x18\x19\r"), "[abc|0|3|1]");
  EXPECT_EQ(t.line("abc\x18\x18\x18\x18\x19\r"), "[abc|3|0|1]");
  EXPECT_EQ(t.line("abc\x1b" "0\x18\x18\x19\r"), "[abc|3|0|1]");
  EXPECT_EQ(t.line("abc\x1b-\x18\x18\x19\r"), "[abc|0|3|0]");
  t.sh.run("u(){ zle undefined-key; BUFFER=\"rc=$?\" }; zle -N u; bindkey '^T' u; "
           "v(){ zle beep; BUFFER=\"rc=$?\" }; zle -N v; bindkey '^V' v");
  EXPECT_EQ(t.line("\x14\r"), "rc=1");
  EXPECT_EQ(t.line("\x16\r"), "rc=0");
  // An edit ends an active region (spaceinline, foredel); a move doesn't,
  // nor a user widget's own assignment.
  EXPECT_EQ(t.line("abc\x1b" "0\x18\x18" "d\x19\r"), "[abcd|4|0|0]");
  EXPECT_EQ(t.line("abc\x1b" "0\x18\x18\x08\x19\r"), "[ab|2|0|0]");
  EXPECT_EQ(t.line("abc\x1b" "0\x18\x18\x02\x19\r"), "[abc|2|0|1]");
  // A rewrite in place (no spaceinline, no shiftchars) keeps it.
  auto marked = [](const char* before, const char* after) {
    return str(before) + '\0' + after;
  };
  t.sh.run("bindkey '^T' transpose-chars");
  EXPECT_EQ(t.line(marked("abcd\x02", "\x14\x19\r")), "[abdc|4|3|1]");
  EXPECT_EQ(t.line(marked("abc def\x01", "\x1bu\x19\r")), "[ABC def|3|0|1]");
  EXPECT_EQ(t.line(marked("abc def\x01", "\x1b" "c\x19\r")), "[Abc def|3|0|1]");
  EXPECT_EQ(t.line(marked("ab cd\x01", "\x1b" "f\x1bt\x19\r")), "[cd ab|5|0|1]");
  EXPECT_EQ(t.line(marked("abc", "\x1b'\x19\r")), "['abc'|5|3|1]");
  t.sh.run("bindkey '^O' overwrite-mode");
  EXPECT_EQ(t.line(marked("abcd\x01", "\x0fxy\x19\r")), "[xycd|2|0|1]");
  t.sh.run("bindkey '^O' accept-line-and-down-history");
  t.sh.run("w(){ BUFFER+=x; REGION_ACTIVE=1 }; zle -N w; bindkey '^T' w");
  EXPECT_EQ(t.line("abc\x14\x19\r"), "[abcx|3|0|1]");
  t.sh.run("w(){ REGION_ACTIVE=1; zle self-insert }; zle -N w");
  EXPECT_EQ(t.line("abc\x14\x19\r"), "[abc\x14|4|0|0]");
  // $MARK counts characters, as $CURSOR, within the buffer.
  t.sh.run("w(){ MARK=-1 }; zle -N w");
  EXPECT_EQ(t.line("abc\x14\x19\r"), "[abc|3|0|0]");
  t.sh.run("w(){ MARK=100 }; zle -N w");
  EXPECT_EQ(t.line("abc\x14\x19\r"), "[abc|3|3|0]");
  t.sh.run("w(){ MARK=2 }; zle -N w");
  EXPECT_EQ(t.line("日本語\x14\x18\x18\x19\r"), "[日本語|2|3|1]");
  // (^@, set-mark-command, is a NUL: the keys can't be one C string.)
  EXPECT_EQ(t.line(str("日本語\x01\x06") + '\0' + "\x05\x18\x18\x19\r"), "[日本語|1|3|1]");
  // One bell for a key with no widget, and none for a widget's own
  // `zle undefined-key` when the widget itself succeeds (handlefeep).
  t.sh.run("bindkey -r '^V'; w(){ zle undefined-key; zle -M x }; zle -N w");
  t.line("x\x16\r");
  EXPECT_EQ(s::count(t.raw.begin(), t.raw.end(), '\a'), 1);
  t.line("x\x14\r");
  EXPECT_EQ(s::count(t.raw.begin(), t.raw.end(), '\a'), 0);
  t.sh.run("w(){ zle undefined-key }");
  t.line("x\x14\r");
  EXPECT_EQ(s::count(t.raw.begin(), t.raw.end(), '\a'), 1);
}

// zsh's invicmdmode tests the keymap's name: under vared -M vicmd (the
// name main) the cursor may sit on a newline or past the end.
TEST(EditorM9Review, VicmdModeIsTheName) {
  Rig t;
  t.sh.ctx.zle->link("vicmd", "main");
  t.pending = s::async(s::launch::async, [&t] { return t.ed->edit("ab\ncd", "", ""); });
  t.send("kD\r");
  EXPECT_EQ(t.finish(), "ab\ncd");
  t.pending = s::async(s::launch::async, [&t] { return t.ed->edit("ab\ncd", "", ""); });
  t.send("kkx\r");
  EXPECT_EQ(t.finish(), "ab\ncd");
  t.sh.ctx.zle->link("emacs", "main");
}

// vi: "1p.. puts "1, "2, … one after the other; dd in an empty buffer
// stores nothing.
TEST(EditorM9Review, ViRepeatStepsNumberedRegisters) {
  ViRig t;
  EXPECT_EQ(t.line("echo abc def ghi x\x1b" "0dwdw\"1p..\r"), "dabc echo ef ghi x");
  ViRig empty;
  EXPECT_EQ(empty.line("\x1b" "ddp\r"), "");
  // `.` after `.` repeats the same put again (the change is the put's
  // keys, not the `.`).
  ViRig put;
  str once = *put.line("abc def\x1b" "0\"ayw$\"ap\r");
  str yanked = once.substr(7);
  EXPECT_FALSE(yanked.empty());
  EXPECT_EQ(put.line("abc def\x1b" "0\"ayw$\"ap..\r"), "abc def" + yanked + yanked + yanked);
  // After `.` $LASTWIDGET is the widget it repeated.
  ViRig dot;
  dot.sh.run("b(){ BUFFER=\"[$WIDGET|$LASTWIDGET]\" }; zle -N b; bindkey -M vicmd '^T' b");
  EXPECT_EQ(dot.line("abc\x1b" "0x.\x14\r"), "[b|vi-delete-char]");
}

}  // namespace
}  // namespace jfsh
