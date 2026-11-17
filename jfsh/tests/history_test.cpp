#include <unistd.h>

#include <fstream>
#include <iterator>

#include <gtest/gtest.h>
#include <shell_fixture.h>

#include <jfsh/core/aliases.h>
#include <jfsh/history/history.h>

namespace jfsh {
namespace {

vec<str> texts(const HistoryManager& h) {
  vec<str> out;
  for (const auto& e : h.entries()) out.push_back(e.text);
  return out;
}

TEST(History, AddAssignsIncreasingNumbers) {
  HistoryManager h{HistOptions{}};
  EXPECT_EQ(h.add("ls"), 1u);
  EXPECT_EQ(h.add("pwd"), 2u);
  EXPECT_EQ(h.last_num(), 2u);
  EXPECT_EQ(h.size(), 2u);
}

TEST(History, EmptyLinesAreNotStored) {
  HistoryManager h{HistOptions{}};
  EXPECT_EQ(h.add(""), 0u);
  EXPECT_EQ(h.size(), 0u);
}

TEST(History, IgnoreSpaceDropsLeadingSpaceLines) {
  HistOptions opts;
  opts.ignore_space = true;
  HistoryManager h{opts};
  EXPECT_EQ(h.add(" secret --token=x"), 0u);
  EXPECT_NE(h.add("ls"), 0u);
  EXPECT_EQ(texts(h), vec<str>{"ls"});
}

TEST(History, IgnoreDupsOnlyDropsConsecutiveRepeats) {
  HistOptions opts;
  opts.ignore_dups = true;
  HistoryManager h{opts};
  h.add("ls");
  h.add("ls");
  h.add("pwd");
  h.add("ls");
  EXPECT_EQ(texts(h), (vec<str>{"ls", "pwd", "ls"}));
}

TEST(History, IgnoreAllDupsMovesTheRepeatToTheEnd) {
  // B58: zsh drops the older copy and keeps the new one.
  HistOptions opts;
  opts.ignore_all = true;
  HistoryManager h{opts};
  h.add("ls");
  h.add("pwd");
  EXPECT_EQ(h.add("ls"), 3u);
  EXPECT_EQ(texts(h), (vec<str>{"pwd", "ls"}));
}

TEST(History, ReduceBlanksCollapsesWhitespaceRuns) {
  HistOptions opts;
  opts.reduce_blanks = true;
  HistoryManager h{opts};
  h.add("ls    -la\t\t/tmp");
  EXPECT_EQ(texts(h), vec<str>{"ls -la\t/tmp"});
}

TEST(History, HistsizeKeepsNewestEntries) {
  HistOptions opts;
  opts.histsize = 2;
  HistoryManager h{opts};
  h.add("a");
  h.add("b");
  h.add("c");
  EXPECT_EQ(texts(h), (vec<str>{"b", "c"}));
  EXPECT_EQ(h.last_num(), 3u);
}

TEST(HistoryExpand, NonBangLinesPassThrough) {
  HistoryManager h{HistOptions{}};
  auto r = h.expand("echo hi");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, "echo hi");
}

TEST(HistoryExpand, BangBangWithoutHistoryIsAnError) {
  HistoryManager h{HistOptions{}};
  EXPECT_FALSE(h.expand("!!").has_value());
}

TEST(HistoryExpand, BangBangRepeatsLastCommand) {
  HistoryManager h{HistOptions{}};
  h.add("make test");
  auto r = h.expand("!!");
  ASSERT_TRUE(r.has_value());
  EXPECT_EQ(*r, "make test");
}


// ---- M9: bang history anywhere in the line -------------------------------------
// Expected expansions are zsh 5.9's (`zsh -f -i`, the same lines in order).

struct Bang {
  HistoryManager h{HistOptions{}};
  // Expands `line` and, as the interactive loop does, adds it.
  str operator()(strv line) {
    auto r = h.expand_line(line);
    if (not r) return "error: " + r.error().message;
    if (not r->print_only) h.add(r->text);
    return r->text;
  }
};

TEST(HistoryExpand, EventsAndWords) {
  Bang x;
  x.h.add("PS1=''");
  x.h.add("echo one two three");
  EXPECT_EQ(x("echo !!"), "echo echo one two three");
  EXPECT_EQ(x("echo !$ !^ !*"), "echo three echo echo one two three");
  EXPECT_EQ(x("echo !-2:2"), "echo one");
  EXPECT_EQ(x("echo !echo:0"), "echo echo");
  EXPECT_EQ(x("echo !?two?"), "echo echo three echo echo one two three");
  EXPECT_EQ(x("echo !#"), "echo echo ");
  EXPECT_EQ(x("echo !1"), "echo PS1=''");
  EXPECT_EQ(x("echo !{1}x"), "echo PS1=''x");
}

TEST(HistoryExpand, QuotingAndLiteralBangs) {
  Bang x;
  x.h.add("echo echo");
  EXPECT_EQ(x("echo 'no !! here' \"yes !!\""), "echo 'no !! here' \"yes echo echo\"");
  EXPECT_EQ(x("echo \\!!"), "echo \\!!");
  EXPECT_EQ(x("[[ ! -f x ]] && x != y; echo hi! \"a!\" !(x)"), "[[ ! -f x ]] && x != y; echo hi! \"a!\" !(x)");
}

TEST(HistoryExpand, Modifiers) {
  Bang x;
  x.h.add("echo /a/b/c.txt");
  EXPECT_EQ(x("echo !$:h !$:t !$:r !$:e"), "echo /a/b c.txt /a/b/c txt");
  x.h.add("echo /a/b/c.txt");
  EXPECT_EQ(x("echo !!:s/b/B/ !!:gs/a/A/"), "echo echo /a/B/c.txt echo /A/b/c.txt");
  x.h.add("echo /a/b/c.txt");
  EXPECT_EQ(x("echo !!:1-2"), "error: no such word in event");
  EXPECT_EQ(x("echo !!:0-1"), "echo echo /a/b/c.txt");
  x.h.add("rm 'it''s' x");
  EXPECT_EQ(x("echo !!:2:q !!:u"), "echo 'x' RM 'IT''S' X");
  x.h.add("one two");
  EXPECT_EQ(x("echo !!:x"), "echo 'one' 'two'");
}

TEST(HistoryExpand, RangesAndErrors) {
  Bang x;
  x.h.add("a b c d e");
  EXPECT_EQ(x("!!:1-2 !!:2* !!:2- !!:-1 !!:3*"), "b c c d e c d a b d e");
  x.h.add("only");
  EXPECT_EQ(x("x !!*"), "x ");
  EXPECT_EQ(x("!999"), "error: no such event: 999");
  EXPECT_EQ(x("!nosuch"), "error: event not found: nosuch");
  EXPECT_EQ(x("echo a!zzz"), "error: event not found: zzz");
  x.h.add("short");
  EXPECT_EQ(x("!!:9"), "error: no such word in event");
  EXPECT_EQ(x("!!:z"), "error: illegal modifier: z");
}

TEST(HistoryExpand, QuickSubstitutionAndPrint) {
  Bang x;
  x.h.add("echo one two");
  EXPECT_EQ(x("^one^ONE^ tail"), "echo ONE two tail");
  EXPECT_EQ(x("^nope^x"), "error: substitution failed");
  EXPECT_EQ(x("!!:s/two/2/"), "echo ONE 2 tail");
  EXPECT_EQ(x("!!:s/O/0/:&"), "error: substitution failed");
  EXPECT_EQ(x("!!:gs/O/0/"), "echo 0NE 2 tail");
  auto r = x.h.expand_line("echo !!:p");
  ASSERT_TRUE(r);
  EXPECT_TRUE(r->print_only);
  EXPECT_TRUE(r->changed);
  EXPECT_FALSE(x.h.expand_line("echo plain")->changed);
}

TEST(HistoryExpand, HistcharsAndBangHist) {
  HistOptions o;
  o.histchars = "@%#";
  HistoryManager h{o};
  h.add("echo hi");
  EXPECT_EQ(*h.expand("echo @@ !!"), "echo echo hi !!");
  EXPECT_EQ(*h.expand("%hi%HI"), "echo HI");
  o.bang = false;
  h.set_options(o);
  EXPECT_EQ(*h.expand("@@"), "@@");
}

TEST(History, SplitWords) {
  EXPECT_EQ(HistoryManager::split_words("echo 'a b' \"c d\" x\\ y;ls|wc -l>f 2>&1"),
            (vec<str>{"echo", "'a b'", "\"c d\"", "x\\ y", ";", "ls", "|", "wc", "-l", ">", "f",
                      "2", ">&", "1"}));
  EXPECT_EQ(HistoryManager::split_words("a && b || c $(x y)"),
            (vec<str>{"a", "&&", "b", "||", "c", "$(x y)"}));
}

TEST(History, B40NoStoreDropsHistoryCommandsOnly) {
  HistOptions o;
  o.no_store    = true;
  o.ignore_dups = true;
  HistoryManager h{o};
  EXPECT_EQ(h.add("history"), 0u);
  EXPECT_EQ(h.add("fc -l 1"), 0u);
  h.add("ls");
  EXPECT_EQ(h.add("ls"), 0u);   // the dup check still runs
  EXPECT_EQ(texts(h), vec<str>{"ls"});
}

// ---- M9: the history file ---------------------------------------------------------

str read_file(const s::string& p) {
  s::ifstream f{p};
  return str(s::istreambuf_iterator<char>(f), {});
}

TEST(HistoryFile, B5AppendWritesOnlyNewEntries) {
  test::TempDir dir;
  str file = (dir.path / "hist").string();
  for (const char* cmd : {"one", "two", "three"}) {
    HistoryManager h{HistOptions{}};
    ASSERT_TRUE(h.load(file));
    h.add(cmd);
    ASSERT_TRUE(h.save(file));
  }
  EXPECT_EQ(read_file(file), "one\ntwo\nthree\n");
}

TEST(HistoryFile, SavehistCutsTheFile) {
  test::TempDir dir;
  str file = (dir.path / "hist").string();
  HistOptions o;
  o.savehist = 2;
  HistoryManager h{o};
  for (const char* cmd : {"a", "b", "c"}) h.add(cmd);
  ASSERT_TRUE(h.save(file));
  EXPECT_EQ(read_file(file), "b\nc\n");
  o.append = false;
  o.save_no_dups = true;
  o.savehist = 10;
  HistoryManager h2{o};
  for (const char* cmd : {"x", "y", "x"}) h2.add(cmd);
  ASSERT_TRUE(h2.save(file));
  EXPECT_EQ(read_file(file), "y\nx\n");
  o.savehist = 0;
  HistoryManager h3{o};
  h3.add("z");
  ASSERT_TRUE(h3.save(file));
  EXPECT_EQ(read_file(file), "y\nx\n");
}

TEST(HistoryFile, B39ExtendedAndMultiLineEntries) {
  test::TempDir dir;
  auto path = dir.file("hist", ": 1700000000:5;echo one\n"
                               "plain\n"
                               ": 1700000001:0;for x in a b\\\ndo echo $x\\\ndone\n"
                               "ends in a backslash\\\\\n", s::filesystem::perms::owner_all);
  HistoryManager h{HistOptions{}};
  ASSERT_TRUE(h.load(path.string()));
  ASSERT_EQ(h.size(), 4u);
  EXPECT_EQ(texts(h), (vec<str>{"echo one", "plain", "for x in a b\ndo echo $x\ndone",
                                "ends in a backslash\\\\"}));
  EXPECT_EQ(h.entries()[0].timestamp, 1700000000);
  EXPECT_EQ(h.entries()[0].elapsed, 5u);

  // Written back in the extended format, newlines as \ continuations.
  str out = (dir.path / "out").string();
  HistOptions o;
  o.extended = true;
  HistoryManager w{o};
  w.add("multi\nline");
  ASSERT_TRUE(w.save(out));
  str data = read_file(out);
  EXPECT_TRUE(data.starts_with(": ")) << data;
  EXPECT_TRUE(data.ends_with(":0;multi\\\nline\n")) << data;
  HistoryManager back{HistOptions{}};
  ASSERT_TRUE(back.load(out));
  EXPECT_EQ(texts(back), vec<str>{"multi\nline"});
}

TEST(HistoryFile, MetafiedBytesRoundTrip) {
  // zsh metafies 0x83-0xa2: "€" is e2 82 ac, "é" c3 a9, "ঃ" e0 a6 83.
  test::TempDir dir;
  str file = (dir.path / "hist").string();
  HistoryManager h{HistOptions{}};
  h.add("echo ঃ é");
  ASSERT_TRUE(h.save(file));
  str raw = read_file(file);
  EXPECT_EQ(raw, "echo \xe0\xa6\x83\xa3 \xc3\xa9\n");
  HistoryManager back{HistOptions{}};
  ASSERT_TRUE(back.load(file));
  EXPECT_EQ(texts(back), vec<str>{"echo ঃ é"});
}

TEST(HistoryFile, ShareHistoryReadsWhatOthersWrote) {
  test::TempDir dir;
  str file = (dir.path / "hist").string();
  HistOptions o;
  o.share = true;
  HistoryManager a{o}, b{o};
  ASSERT_TRUE(a.load(file));
  ASSERT_TRUE(b.load(file));
  a.add("from-a");
  ASSERT_TRUE(a.save(file, HistSave::Append));
  ASSERT_TRUE(b.sync(file));
  b.add("from-b");
  ASSERT_TRUE(b.save(file, HistSave::Append));
  ASSERT_TRUE(a.sync(file));
  EXPECT_EQ(texts(a), (vec<str>{"from-a", "from-b"}));
  EXPECT_EQ(texts(b), (vec<str>{"from-a", "from-b"}));
}

TEST(HistoryFile, FcWriteAndAppend) {
  test::TempDir dir;
  str file = (dir.path / "hist").string();
  test::Shell sh;
  HistoryManager h{HistOptions{}};
  sh.ctx.history = &h;
  h.add("one");
  h.add("two");
  EXPECT_EQ(sh.run("fc -W " + file).rc, 0);
  h.add("three");
  EXPECT_EQ(sh.run("fc -A " + file).rc, 0);
  EXPECT_EQ(read_file(file), "one\ntwo\nthree\n");
  EXPECT_EQ(sh.run("fc -W " + file).rc, 0);   // -W writes it all again
  EXPECT_EQ(read_file(file), "one\ntwo\nthree\n");
  sh.ctx.history = nullptr;
}

}  // namespace
}  // namespace jfsh
