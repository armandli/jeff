#include <unistd.h>

#include <fstream>
#include <iterator>

#include <gtest/gtest.h>
#include <shell_fixture.h>

#include <jfsh/core/aliases.h>
#include <jfsh/exec/loop.h>
#include <jfsh/history/history.h>
#include <jfsh/parser/parser.h>

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
  // Kept until the next line (zsh's HIST_TMPSTORE), so !! still sees it.
  EXPECT_EQ(h.add(" secret --token=x"), 1u);
  EXPECT_EQ(texts(h), vec<str>{" secret --token=x"});
  EXPECT_EQ(h.add("ls"), 1u);
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

// zsh's histreduceblanks: the first character of each gap between words
// stays; blanks inside words and a comment after the last stay as typed.
TEST(History, ReduceBlanksCollapsesWhitespaceRuns) {
  auto reduce = [](strv line, bool keep_leading = false) {
    HistOptions o;
    o.reduce_blanks = true;
    o.ignore_space  = keep_leading;
    return HistoryManager::hend_text(line, word_spans(line, true), o);
  };
  EXPECT_EQ(reduce("ls    -la\t\t/tmp"), "ls -la\t/tmp");
  EXPECT_EQ(reduce("echo $(  echo   a  )   b"), "echo $(  echo   a  ) b");
  EXPECT_EQ(reduce("echo \"a  b\"   c   "), "echo \"a  b\" c");
  EXPECT_EQ(reduce("echo a  # c   d"), "echo a  # c   d");
  EXPECT_EQ(reduce("   echo  lead"), "echo lead");
  EXPECT_EQ(reduce("   echo  lead", true), "   echo lead");
  // `(( … ))` is one word; `for ((` has one per expression, blanks before
  // its `;` kept.
  EXPECT_EQ(reduce("(( 1 ))   ;   echo   a"), "(( 1 )) ; echo a");
  EXPECT_EQ(reduce("if  ((  1  )) ; then  echo  y ;  fi"), "if ((  1  )) ; then echo y ; fi");
  EXPECT_EQ(reduce("for ((  i=0  ;  i<1  ;  i++  ))  {  echo  $i  }"),
            "for (( i=0  ; i<1  ; i++  )) { echo $i }");
  EXPECT_EQ(reduce("[[   a  ==  a   ]]"), "[[ a == a ]]");
  // An array assignment: `name=(`, each element and `)`.
  EXPECT_EQ(reduce("a=(  1   2  )   ;  echo $a"), "a=( 1 2 ) ; echo $a");
  EXPECT_EQ(reduce("a+=(  3  )"), "a+=( 3 )");
  EXPECT_EQ(reduce("typeset  -a   b=(  1   2  )"), "typeset -a b=( 1 2 )");
  EXPECT_EQ(reduce("x=(  $(echo  a)  )"), "x=( $(echo  a) )");
  // A here-document or a parse error stops zsh's lexer before the end of
  // the line: it stays as typed.
  EXPECT_EQ(reduce("cat  <<E   >/dev/null\n  x    y\nE"), "cat  <<E   >/dev/null\n  x    y\nE");
  EXPECT_EQ(reduce("echo  a  ;  cat <<E\nz\nE"), "echo  a  ;  cat <<E\nz\nE");
  EXPECT_EQ(reduce("echo   a  )  b"), "echo   a  )  b");
  EXPECT_EQ(reduce("echo a;;  "), "echo a;;  ");
  // A line that parses through an alias: the alias's name is a word, its
  // text adds none (zsh's ihwend).
  AliasTable aliases;
  aliases.regular["b"] = {"{"};
  aliases.regular["x"] = {"if true; then"};
  auto reduce_aliased = [&](strv line) {
    HistOptions o;
    o.reduce_blanks = true;
    return HistoryManager::hend_text(line, word_spans(line, true, &aliases), o);
  };
  EXPECT_EQ(reduce_aliased("b   echo   a ;  }"), "b echo a ; }");
  EXPECT_EQ(reduce_aliased("x   echo  a ;  fi"), "x echo a ; fi");
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
  EXPECT_EQ(x("[[ ! -f x ]] && x != y; echo hi! !(x)"), "[[ ! -f x ]] && x != y; echo hi! !(x)");
  // zsh's !" goes and ends expansion for the line, even inside quotes.
  EXPECT_EQ(x("echo \"a!\" !!"), "echo \"a !!");
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
  EXPECT_EQ(x("x !!*"), "error: no such word in event");
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
  // Each kept only until the next line comes (HIST_TMPSTORE).
  EXPECT_EQ(h.add("history"), 1u);
  EXPECT_EQ(h.add("fc -l 1"), 1u);
  EXPECT_EQ(texts(h), vec<str>{"fc -l 1"});
  h.add("ls");
  EXPECT_EQ(h.add("ls"), 1u);   // the dup is the line before
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
  ASSERT_TRUE(a.save(file));
  ASSERT_TRUE(b.sync(file));
  b.add("from-b");
  ASSERT_TRUE(b.save(file));
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
  // An interactive shell's history, with a line being run (zsh's curline).
  sh.opts.assign(OPT_INTERACTIVE, true);
  h.add("one");
  h.add("two");
  // zsh writes nothing without $SAVEHIST.
  EXPECT_EQ(sh.run("fc -W " + file).rc, 0);
  EXPECT_FALSE(s::filesystem::exists(file));
  sh.run("SAVEHIST=100");
  EXPECT_EQ(sh.run("fc -W " + file).rc, 0);
  h.add("three");
  // -A appends the whole list (zsh's savehistfile without -I).
  EXPECT_EQ(sh.run("fc -A " + file).rc, 0);
  EXPECT_EQ(read_file(file), "one\ntwo\none\ntwo\nthree\n");
  EXPECT_EQ(sh.run("fc -W " + file).rc, 0);   // -W writes it all again
  EXPECT_EQ(read_file(file), "one\ntwo\nthree\n");
  sh.ctx.history = nullptr;
}

// ---- M9 review: zsh 5.9's histsubchar ---------------------------------------
//
// Expected values from zsh -f -i reading these lines (scratchpad
// m9/p/p_bang.py).

TEST(HistoryM9Review, EventsAsZshFindsThem) {
  Bang x;
  x.h.add("echo a1 b2 c3 /usr/lib/x.tar.gz");
  x.h.add("echo second \"q w\" e");
  EXPECT_EQ(x("echo !0"), "error: event not found: 0");
  EXPECT_EQ(x("echo !-99"), "error: no such event: 0");
  EXPECT_EQ(x("echo !:%"), "error: % with no previous word matched");
  EXPECT_EQ(x("echo !?a1?%"), "echo a1");
  // A word ends the event only at a blank or ; : ^ $ * % } ' " `.
  EXPECT_EQ(x("case x in [!a]) echo ok;; esac"), "error: event not found: a])");
  // ${!} and !" aren't events; !" goes and stops expansion for the line.
  EXPECT_EQ(x("echo ${!}x"), "echo ${!}x");
  EXPECT_EQ(x("echo a !\"x !!"), "echo a x !!");
}

TEST(HistoryM9Review, WordsKeepTheirSpacing) {
  Bang x;
  x.h.add("echo \"a b\"  c");
  EXPECT_EQ(x("echo !!:1-2"), "echo \"a b\"  c");
  EXPECT_EQ(x("echo !-2:x"), "echo 'echo' '\"a' 'b\"' '' 'c'");
  x.h.add("echo second \"q w\" e");
  EXPECT_EQ(x("echo !!:q"), "echo 'echo' 'second' '\"q' 'w\"' 'e'");
}

TEST(HistoryM9Review, ModifiersCanFail) {
  auto x = [](strv line) {
    Bang b;
    b.h.add("echo second e /usr/lib/x.tar.gz");
    return b(line);
  };
  EXPECT_EQ(x("echo !:2:h"), "error: modifier failed: h");
  EXPECT_EQ(x("echo !:0:t"), "error: modifier failed: t");
  EXPECT_EQ(x("echo !:3:h2 !:3:t2"), "echo /usr lib/x.tar.gz");
  EXPECT_EQ(x("echo !:s/e/E/:G"), "echo Echo sEcond E /usr/lib/x.tar.gz");
  EXPECT_EQ(x("echo !:3:Q"), "echo /usr/lib/x.tar.gz");
  // \& is &, the text matched.
  EXPECT_EQ(x("!:s/e/\\&/"), "echo second e /usr/lib/x.tar.gz");
  EXPECT_EQ(x("^e^E^:G"), "Echo sEcond E /usr/lib/x.tar.gz");
  Bang y;
  y.h.add("echo e");
  EXPECT_EQ(y("echo !:s//X/"), "error: no previous substitution");
}

TEST(HistoryM9Review, LinesKeptUntilTheNext) {
  HistOptions o;
  o.ignore_space = true;
  HistoryManager h{o};
  h.add("echo a");
  // ` echo hidden` is there for !! until the next line comes.
  EXPECT_EQ(h.add(" echo hidden"), 2u);
  auto r = h.expand_line("echo !!");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->text, "echo  echo hidden");
  EXPECT_EQ(h.add("echo b"), 2u);
  EXPECT_EQ(texts(h), (vec<str>{"echo a", "echo b"}));
}

TEST(HistoryM9Review, FcListingAndStacks) {
  test::TempDir dir;
  test::Shell sh;
  HistoryManager h{HistOptions{}};
  sh.ctx.history = &h;
  // An interactive shell's history, with a line being run (zsh's curline).
  sh.opts.assign(OPT_INTERACTIVE, true);
  h.add("echo a; \\\necho b");
  h.add("if true\nthen echo c\nfi");
  sh.ctx.hist_line = h.add("fc -l");
  // zsh's fclist writes a command with nicezputs.
  EXPECT_EQ(sh.run("fc -l").out, "    1  echo a; \\\\necho b\n    2  if true\\nthen echo c\\nfi\n");
  // fc -p starts an empty history (the line that ran it is in the old
  // one); fc -P brings the old one back.
  EXPECT_EQ(sh.run("fc -p").rc, 0);
  EXPECT_EQ(sh.ctx.hist_line, 0u);
  h.add("echo pushed");
  sh.ctx.hist_line = h.add("fc -l");
  EXPECT_EQ(sh.run("fc -l").out, "    1  echo pushed\n");
  EXPECT_EQ(sh.run("fc -P").rc, 0);
  EXPECT_EQ(sh.run("fc -l").out, "    1  echo a; \\\\necho b\n    2  if true\\nthen echo c\\nfi\n");
  // A file that can't be written: zsh's message, and fc goes on with 0.
  str file = (dir.path / "nodir" / "h").string();
  sh.run("SAVEHIST=10");
  auto r = sh.run("fc -W " + file);
  EXPECT_EQ(r.err, "jfsh: locking failed for " + file + ": no such file or directory\n");
  EXPECT_EQ(r.rc, 0);
  sh.ctx.history = nullptr;
}

// ---- M9 review, round 1 ------------------------------------------------------
//
// Expected values from zsh -f -i (scratchpad m9rv1/p/r2.py, r3.py, r12.py).

// The quoting goes on from one line of a command to the next (zsh's lexer
// state), and every line read can be ^old^new (zsh's inputline).
TEST(HistoryM9Review, QuotesAndCaretsAcrossLines) {
  HistoryManager h{HistOptions{}};
  h.add("echo abc");
  auto r = h.expand_line("echo 'x");
  ASSERT_TRUE(r);
  EXPECT_FALSE(r->changed);
  r = h.expand_line("!! y'", "echo 'x\n", true, r->quoting);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->text, "!! y'");
  r = h.expand_line("echo 'x");
  ASSERT_TRUE(r);
  r = h.expand_line("y' !!", "echo 'x\n", true, r->quoting);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->text, "y' echo abc");
  r = h.expand_line("echo x \\");
  ASSERT_TRUE(r);
  r = h.expand_line("^a^b", "echo x \\\n", true, r->quoting);
  ASSERT_TRUE(r);
  EXPECT_EQ(r->text, "echo bbc");
  // Without a state each line starts afresh.
  r = h.expand_line("!! y'");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->text, "echo abc y'");
}

// The lexer's three quoting states: in '…' a backslash is a plain
// character, in $'…' it escapes and in "…" too; ^old^new isn't done in
// '…' or $'…' (history is off there), but is in "…".
TEST(HistoryM9Review, ThreeQuotingStates) {
  HistoryManager h{HistOptions{}};
  h.add("echo hi");
  auto run = [&](vec<str> lines) {
    HistQuoting q;
    str before, last;
    for (const auto& l : lines) {
      auto r = h.expand_line(l, before, true, q);
      if (not r) return str("error");
      last = r->text;
      q    = r->quoting;
      before += l + "\n";
    }
    return last;
  };
  EXPECT_EQ(run({"echo 'a", "^hi^yo'"}), "^hi^yo'");
  EXPECT_EQ(run({"echo $'a", "^hi^yo'"}), "^hi^yo'");
  EXPECT_EQ(run({"echo \"a", "^hi^yo\""}), "echo yo\"");
  EXPECT_EQ(run({"echo 'a\\' !!"}), "echo 'a\\' echo hi");
  EXPECT_EQ(run({"echo 'a\\' 'b", "!!'"}), "!!'");
  EXPECT_EQ(run({"echo 'a\\' \"x", "!!\""}), "echo hi\"");
  EXPECT_EQ(run({"echo $'a\\'' !!"}), "echo $'a\\'' echo hi");
  EXPECT_EQ(run({"echo $'a\\' !!'"}), "echo $'a\\' !!'");
  EXPECT_EQ(run({"echo \"$'x\" !!"}), "echo \"$'x\" echo hi");
}

// fc in a shell without a line being run (zsh's curline.histnum 0): -1
// is event 0, re-running stops short of the newest event (bin_fc), and
// fc -W/-A write nothing (savehistfile).
TEST(HistoryM9Review, FcWithoutACurrentLine) {
  test::TempDir dir;
  test::Shell sh;
  HistoryManager h{HistOptions{}};
  sh.ctx.history = &h;
  auto r = sh.run("print -s 'echo hi'; fc -e -; echo rc $?");
  EXPECT_EQ(r.err, "jfsh: fc: current history line would recurse endlessly, aborted\n");
  EXPECT_EQ(r.out, "rc 1\n");
  r = sh.run("print -s 'echo b'; fc -e - -1; echo rc $?");
  EXPECT_EQ(r.err, "jfsh: fc: no such event: 0\n");
  r = sh.run("r");
  EXPECT_EQ(r.out, "hi\n");
  EXPECT_EQ(sh.run("fc -l -1").out, "    1  echo hi\n    2  echo b\n    3  echo hi\n");
  EXPECT_EQ(sh.run("fc -l 2 1").out, "    2  echo b\n    1  echo hi\n");
  EXPECT_EQ(sh.run("fc -e - 2 1").err,
            "jfsh: fc: history events can't be executed backwards, aborted\n");
  str file = (dir.path / "hf").string();
  sh.run("SAVEHIST=10; fc -W " + file + "; fc -A " + file);
  EXPECT_FALSE(s::filesystem::exists(file));
  sh.ctx.history = nullptr;
  // A string names the newest event below the newest one (hcomsearch
  // starts at the entry before it).
  HistoryManager h2{HistOptions{}};
  sh.ctx.history = &h2;
  sh.run("print -s 'echo a'; print -s 'echo b'; print -s 'echo c'");
  EXPECT_EQ(sh.run("fc -l echo").out, "    2  echo b\n    3  echo c\n");
  EXPECT_EQ(sh.run("fc -l ''").out, "    2  echo b\n    3  echo c\n");
  EXPECT_EQ(sh.run("r echo").out, "b\n");
  sh.ctx.history = nullptr;
}

// The lexer's nesting for history: $(…) inside "…" is read as a command
// (its '…' stops history), "…" inside $(…) doesn't, a '…' inside `…`
// does, and with INTERACTIVE_COMMENTS a comment is read without history.
TEST(HistoryM9Review, NestingAndComments) {
  HistOptions o;
  o.comments = true;
  HistoryManager h{o};
  h.add("echo hi");
  auto run = [&](vec<str> lines) {
    HistQuoting q;
    str before, last;
    for (const auto& l : lines) {
      auto r = h.expand_line(l, before, true, q);
      if (not r) return str("error");
      last = r->text;
      q    = r->quoting;
      before += l + "\n";
    }
    return last;
  };
  EXPECT_EQ(run({"echo \"$(echo 'x !! y')\""}), "echo \"$(echo 'x !! y')\"");
  EXPECT_EQ(run({"echo \"$(echo \"it's\") !!\""}), "echo \"$(echo \"it's\") echo hi\"");
  EXPECT_EQ(run({"echo \"$(echo 'a", "!!')\""}), "!!')\"");
  EXPECT_EQ(run({"echo a # !!"}), "echo a # !!");
  EXPECT_EQ(run({"echo a#!!"}), "echo a#echo hi");
  EXPECT_EQ(run({"echo \"`echo 'x !!'`\" !!"}), "echo \"`echo 'x !!'`\" echo hi");
  EXPECT_EQ(run({"echo $(echo a # it's", ") !!"}), ") echo hi");
  // Without INTERACTIVE_COMMENTS `#` is an ordinary character.
  HistoryManager plain{HistOptions{}};
  plain.add("echo hi");
  EXPECT_EQ(plain.expand_line("echo a # !!")->text, "echo a # echo hi");
  // gettok's rule: a comment only where a token starts, never inside
  // ${…} or math.
  EXPECT_EQ(run({"echo ${x:-a #!!}"}), "echo ${x:-a #echo hi}");
  EXPECT_EQ(run({"echo ${x:-a #b} !!"}), "echo ${x:-a #b} echo hi");
  EXPECT_EQ(run({"echo $(echo ${x:-a #!!})"}), "echo $(echo ${x:-a #echo hi})");
  EXPECT_EQ(run({"(( 1 #!! ))"}), "(( 1 #echo hi ))");
  EXPECT_EQ(run({"echo $[ 1 #!! ]"}), "echo $[ 1 #echo hi ]");
  EXPECT_EQ(run({"echo x(#a) !!"}), "echo x(#a) echo hi");
  EXPECT_EQ(run({"echo a(#!!)"}), "echo a(#echo hi)");
  EXPECT_EQ(run({"echo (#!!)"}), "echo (#echo hi)");
  EXPECT_EQ(run({"f() #!!"}), "f() #!!");
  EXPECT_EQ(run({"( #!!"}), "( #!!");
  EXPECT_EQ(run({"echo a;#!!"}), "echo a;#!!");
  EXPECT_EQ(run({"a=(x) #!!"}), "a=(x) #!!");
  // In math (dquote_parse) a '…' is no quote: history goes on.
  EXPECT_EQ(plain.expand_line("echo $((1 '!!'))")->text, "echo $((1 'echo hi'))");
  EXPECT_EQ(run({"echo $(( 1 + '", "!! ' ))"}), "echo hi ' ))");
  EXPECT_EQ(run({"echo $[ 1 + ${#${:-'!!'}} ]"}), "echo $[ 1 + ${#${:-'echo hi'}} ]");
  // A here-document opened inside $(…): its body as it is, to its end.
  EXPECT_EQ(run({"echo $(cat <<E", "!!"}), "!!");
  EXPECT_EQ(run({"echo $(cat <<E", "it's", "E", ") !!"}), ") echo hi");
  EXPECT_EQ(run({"echo $(cat <<-E", "\t!!", "\tE", ") !!"}), ") echo hi");
  EXPECT_EQ(run({"echo $(cat <<A; cat <<B", "!!", "A", "it's", "B", ") !!"}), ") echo hi");
  EXPECT_EQ(run({"echo $(cat <<'E'", "!!", "E", ") !!"}), ") echo hi");
  // gettokstr: in a glob group (a `(` that starts no subshell) blanks
  // and `|` are the word's, so no comment starts; `;`, `&` and a
  // redirection still end the word.
  auto expanded = [&](str l) {
    str want = l;
    want.replace(want.find("!!"), 2, "echo hi");
    return run({l}) == want;
  };
  for (const char* l : {
         "echo x(a #!!)", "echo x(a|#!!)", "echo (a|#b) !!", "echo x((a) #!!)",
         "builtin echo (a #!!)", "echo a (b #!!)", "repeat 1 echo a (b #!!)",
         "$(echo echo) (a #!!)", "echo x($((1)) #!!)", "echo x(${y:-a} #!!)",
         "echo x('a #' #!!)", "echo x(a\t#!!)", "echo x(<1-2> #!!)", "echo $(echo x(a #!!))",
         "echo $(echo (a #!!))", "echo x($(echo a) #!!)", "echo {a,(b #!!)}",
         "case a in (a|#b) echo m !!;; esac", "select x in (a #!!) </dev/null; do break; done",
         "echo x(a\\ #!!)", "echo x(\"a #\" #!!)", "echo x(`echo a` #!!)",
         // `=(` is an array only for a name in command position or
         // after typeset; elsewhere `(` is a glob group or =(…).
         "echo a=(x #!!)", "echo b c=(x #!!)", "\"a\"=(x #!!)", "a=(x (y #!!))",
         "a=b echo c=(x #!!)", "builtin typeset a=(x #!!)", "echo a=(echo #!!)",
         // A redirection's word, then the command goes on as before it.
         "echo a 2>/dev/null (b #!!)", "cat </dev/null (a #!!)", "echo 2>&1 (a #!!)",
         "echo &>/dev/null (a #!!)", "echo >|/dev/null (a #!!)", "echo 12>/dev/null (a #!!)",
         // Precommand words are no reserved words; for's list after `in`,
         // the third word of a test in [[ … ]].
         "noglob echo (a #!!)", "command (a #!!)", "exec (a #!!)",
         "for x in (a #!!); do :; done", "for x (a) echo (b #!!)", "[[ a = (b #!!) ]]",
         "[[ a = b && c = (d #!!) ]]", "[[ ( a = (b #!!) ) ]]", "[[ a == (a|#b) ]] && echo !!",
         "function f (a #!!)", "echo function (a #!!)", "echo x(a)#!!"})
    EXPECT_TRUE(expanded(l)) << l;
  // A comment where a token starts.
  for (const char* l : {
         "echo x(a;#!!)", "echo x(a&#!!)", "echo x(a>#!!)", "echo x(a<#!!)", "echo x<1-2> #!!",
         "echo x(a) #!!", "a=(x #!!)", "typeset a=(x #!!)", "f(){ local a=(x #!!) }",
         "a+=(x #!!)", "a[1]=(x #!!)", "1=(x #!!)", "a=b c=(x #!!)", "export a b=(x #!!)",
         "typeset a b=(x #!!)", "cat <(echo a #!!)", "echo >(cat #!!)", "echo =(echo a #!!)",
         "echo a<(echo #!!)", ">/dev/null (b #!!)", "(echo a #!!)", "( (echo a) #!!)",
         "f () #!!", "function f() #!!", "[[ (a == a) ]] #!!", "[[ ( a #!! ) ]]",
         "for ((i=0; i<1; i++)) echo #!!", "for ((;;)) (b #!!)", "case a in a|#b) echo m !!;; esac",
         "{ echo a #!! }", "for x (a #!!) :", "for x (a) (echo #!!)", "foreach x (a #!!)",
         "select x (a #!!) break", "[[ (a #!!) ]]", "[[ ! (a #!!) ]]", "[[ a = b && (c #!!) ]]",
         "[[ -n (a #!!) ]]", "[[ a = b ]] && (echo #!!)", "nocorrect (echo #!!)",
         "time (echo #!!)", "! (echo #!!)", "{ (echo #!!) }", "if true; then (echo #!!) fi",
         "function f { (echo #!!) }", "function f g { (echo #!!) }", "function f () (echo #!!)",
         "function { (echo #!!) }", "a=1 (echo #!!)", "repeat 1 (echo a #!!"})
    EXPECT_EQ(run({l}), l);
  EXPECT_EQ(run({"echo a \\", "#!!"}), "#!!");
  EXPECT_EQ(run({"echo a\\", "#!!"}), "#echo hi");
  EXPECT_EQ(run({"for x", "in (a #!!); do :; done"}), "in (a #echo hi); do :; done");
  // par_case: after a pattern's `)` a command can start; `;;`, `;&` and
  // `;|` make the next word a pattern again.
  for (vec<str> ls : vec<vec<str>>{
         {"case a in a) (echo m #!!", ");; esac"},
         {"case a in (a) (echo m #!!", ");; esac"},
         {"case a in b) :;; a) (echo m #!!", ");; esac"},
         {"case a in a) :;& b) (echo m #!!", ");; esac"},
         {"case a in a) :;| a) (echo m #!!", ");; esac"},
         {"case a in b|a) (echo m #!!", ");; esac"},
         {"case in in in) (echo m #!!", ");; esac"},
         {"case a in a) case b in b) :;; esac; (echo m #!!", ");; esac"}})
    EXPECT_EQ(run({ls[0]}), ls[0]);
  EXPECT_EQ(run({"case a in", "a) (echo m #!!"}), "a) (echo m #!!");
  EXPECT_EQ(run({"case a in a) echo (b #!!);; esac"}), "case a in a) echo (b #echo hi);; esac");
  EXPECT_EQ(run({"echo $(case a in a) echo m;; esac) !!"}),
            "echo $(case a in a) echo m;; esac) echo hi");
  EXPECT_EQ(run({"echo \"$(case a in a) echo m;; esac)\" !!"}),
            "echo \"$(case a in a) echo m;; esac)\" echo hi");
  // Text of several lines (a paste, a recalled entry) is read a line at a
  // time: a newline ends a comment and a command; only the first line may
  // be ^old^new.
  auto whole = [&](str text) {
    auto r = h.expand_line(text);
    return r ? r->text : str("error");
  };
  EXPECT_EQ(whole("echo a #c\necho b !!"), "echo a #c\necho b echo hi");
  EXPECT_EQ(whole("echo a #c !!\necho b"), "echo a #c !!\necho b");
  EXPECT_EQ(whole("echo a #c\necho b #d\necho !!"), "echo a #c\necho b #d\necho echo hi");
  EXPECT_EQ(whole("true\n(echo a #!!\n)"), "true\n(echo a #!!\n)");
  EXPECT_EQ(whole("true\na=(x #!!\n); echo $a"), "true\na=(x #!!\n); echo $a");
  EXPECT_EQ(whole("echo a \\\n#!!"), "echo a \\\n#!!");
  EXPECT_EQ(whole("echo a\n^hi^ho"), "echo a\n^hi^ho");
  EXPECT_EQ(whole("echo 'a\n!!' !!"), "echo 'a\n!!' echo hi");
  EXPECT_EQ(whole("echo a !\"\n!!"), "echo a \n!!");
  // expand-history lexes the buffer as a whole, and reads no
  // here-document body (doexpandhist).
  EXPECT_EQ(h.expand_buffer("cat <<E\na!!\nE")->text, "cat <<E\naecho hi\nE");
  EXPECT_EQ(h.expand_buffer("echo a #c\necho !!")->text, "echo a #c\necho echo hi");
}

// A here-document's body is read without history wherever its operator
// is, at any nesting (gethere under STOPHIST), up to its end word.
TEST(HistoryM9Review, HereDocumentBodies) {
  HistoryManager h{HistOptions{}};
  h.add("echo hi");
  auto run = [&](vec<str> lines) {
    HistQuoting q;
    str before, out;
    for (const auto& l : lines) {
      auto r = h.expand_line(l, before, true, q);
      if (not r) return str("error");
      out += r->text + "\n";
      q    = r->quoting;
      before += l + "\n";
    }
    return out;
  };
  auto same = [&](vec<str> lines) {
    str all;
    for (const auto& l : lines) all += l + "\n";
    return run(lines) == all;
  };
  EXPECT_TRUE(same({"cat <<E", "x!!", "E"}));
  EXPECT_TRUE(same({"{ cat <<E", "x!!", "E", "}"}));
  EXPECT_TRUE(same({"if true; then cat <<E", "Hello!\"", "E", "fi"}));
  EXPECT_TRUE(same({"f() {", "cat <<E", "a !! b", "E", "}"}));
  EXPECT_TRUE(same({"for i in 1; do cat <<E", "!!", "E", "done"}));
  EXPECT_TRUE(same({"while true; do cat <<E", "!!", "E", "break; done"}));
  EXPECT_TRUE(same({"case a in a) cat <<E", "!!", "E", ";; esac"}));
  EXPECT_TRUE(same({"(cat <<E", "!!", "E", ")"}));
  EXPECT_TRUE(same({"cat <<E |", "!!", "E", "cat"}));
  EXPECT_TRUE(same({"cat <<E | cat", "!!", "E"}));
  EXPECT_TRUE(same({"true && cat <<E", "!!", "E"}));
  EXPECT_TRUE(same({"{ cat <<-E", "\t!!", "\tE", "}"}));
  EXPECT_TRUE(same({"{ cat <<'E'", "!!", "E", "}"}));
  EXPECT_TRUE(same({"{ cat <<A; cat <<B", "!!", "A", "it's !!", "B", "}"}));
  EXPECT_TRUE(same({"{ cat <<A <<B", "!!", "A", "b!!", "B", "}"}));
  EXPECT_TRUE(same({"{ cat 0<<E", "!!", "E", "}"}));
  EXPECT_TRUE(same({"{ cat <<E!", "!!", "E!", "}"}));
  EXPECT_TRUE(same({"g() { if true; then", "cat <<E", "!!", "E", "fi }"}));
  // After the end word history comes back; <<< and math's << are no
  // here-documents.
  EXPECT_EQ(run({"{ cat <<E", "x", "E", "echo !!", "}"}), "{ cat <<E\nx\nE\necho echo hi\n}\n");
  EXPECT_EQ(run({"{ cat <<E", "a!\"b", "E", "echo !!; }"}),
            "{ cat <<E\na!\"b\nE\necho echo hi; }\n");
  EXPECT_EQ(run({"{ cat <<E >/dev/null", "!!", "E", "echo !!; }"}),
            "{ cat <<E >/dev/null\n!!\nE\necho echo hi; }\n");
  EXPECT_EQ(run({"{ cat <<<!!", "}"}), "{ cat <<<echo hi\n}\n");
  EXPECT_EQ(run({"{ echo $((1<<2)) !!", "}"}), "{ echo $((1<<2)) echo hi\n}\n");
  EXPECT_EQ(run({"{ ((x=1<<2)); echo !!", "}"}), "{ ((x=1<<2)); echo echo hi\n}\n");
  // The end word is read as any word is: history expanded, then unquoted.
  EXPECT_EQ(run({"cat <<!!", "x", "!!", "echo", "echo !!"}),
            "cat <<echo hi\nx\n!!\necho\necho echo hi\n");
  EXPECT_EQ(run({"cat <<'!!'", "x", "!!", "echo !!"}), "cat <<'!!'\nx\n!!\necho echo hi\n");
  EXPECT_EQ(run({"cat <<\\!!", "x", "!!", "echo !!"}), "cat <<\\!!\nx\n!!\necho echo hi\n");
  EXPECT_EQ(run({"cat <<-!!", "\tx", "\techo", "echo !!"}),
            "cat <<-echo hi\n\tx\n\techo\necho echo hi\n");
  // The same text in one piece (a paste, an entry recalled with Up).
  auto whole = [&](str text) {
    auto r = h.expand_line(text);
    return r ? r->text : str("error");
  };
  for (str t : {"cat <<E\na!!\nE", "cat <<-E\n\t!!\n\tE", "{ cat <<E\n!!\nE\n}",
                "f() {\ncat <<E\nf!!\nE\n}", "cat <<A; cat <<B\n!!\nA\nb!!\nB",
                "cat <<'E'\n!!\nE", "echo $(cat <<E\n!!\nE\n)"})
    EXPECT_EQ(whole(t), t);
  EXPECT_EQ(whole("cat <<E\na!!\nE\necho !!"), "cat <<E\na!!\nE\necho echo hi");
  EXPECT_EQ(whole("cat <<!!\nx\n!!\necho\necho !!"), "cat <<echo hi\nx\n!!\necho\necho echo hi");
}

// fc -W and fc -A write the whole list; with -I only what wasn't read or
// written yet; fc -RI leaves out lines already there.  The time formats
// are fclist's (-t's, -i, -E, -f, -d).
TEST(HistoryM9Review, FcWriteReadAndTimes) {
  test::TempDir dir;
  test::Shell sh;
  HistoryManager h{HistOptions{}};
  sh.ctx.history = &h;
  sh.opts.assign(OPT_INTERACTIVE, true);
  sh.run("SAVEHIST=5");
  h.add("echo a");
  h.add("echo b");
  str file = (dir.path / "hf").string();
  sh.run("fc -W " + file + "; fc -A " + file);
  EXPECT_EQ(sh.run("cat " + file).out, "echo a\necho b\necho a\necho b\n");
  str two = (dir.path / "h2").string();
  sh.run("fc -AI " + two + "; fc -AI " + two);
  EXPECT_EQ(sh.run("cat " + two).out, "echo a\necho b\n");
  str in = (dir.path / "in").string();
  sh.run("print -l 'echo a' x > " + in + "; fc -RI " + in);
  EXPECT_EQ(h.entries().back().text, "x");
  EXPECT_EQ(h.size(), 3u);
  EXPECT_EQ(sh.run("fc -lt '[%Y]' 1 1").out.find("[2"), 7u);
  EXPECT_EQ(sh.run("fc -lt").err, "jfsh: fc: argument expected: -t\n");
  sh.ctx.history = nullptr;
  // INC_APPEND_HISTORY's writes, line by line, add only the new entries.
  str inc = (dir.path / "inc").string();
  HistOptions io;
  io.inc_append = true;
  HistoryManager hi{io};
  hi.add("one");
  ASSERT_TRUE(hi.save(inc, HistSave::Incremental));
  hi.add("two");
  ASSERT_TRUE(hi.save(inc, HistSave::Incremental));
  EXPECT_EQ(sh.run("cat " + inc).out, "one\ntwo\n");
  // Past SAVEHIST and a fifth the file is cut back to SAVEHIST (zsh's
  // histfile_linect), and with INC_APPEND_HISTORY that goes on each line.
  str trim = (dir.path / "trim").string();
  HistOptions to;
  to.inc_append = true;
  to.savehist   = 2;
  HistoryManager ht{to};
  for (const char* l : {"echo 1", "echo 2", "echo 3", "echo 4"}) {
    ht.add(l);
    ASSERT_TRUE(ht.save(trim, HistSave::Incremental));
  }
  EXPECT_EQ(sh.run("cat " + trim).out, "echo 3\necho 4\n");
}

// :c is the command's path (zsh's equalsubstr), failing as zsh's does.
TEST(HistoryM9Review, CommandPathModifier) {
  HistoryManager h{HistOptions{}};
  h.add(": ls");
  h.set_command_lookup([](const str& name) -> opt<str> {
    return name == "ls" ? opt<str>("/bin/ls") : s::nullopt;
  });
  auto r = h.expand_line("echo !!:1:c");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->text, "echo /bin/ls");
  r = h.expand_line("echo !!:1:c:t");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->text, "echo ls");
  h.add(": nosuch");
  r = h.expand_line("echo !!:1:c");
  ASSERT_FALSE(r);
  EXPECT_EQ(r.error().message, "modifier failed: c");
}

// histchars set empty turns bang history off; unset, the defaults; and
// HISTCHARS is the same parameter.  setopt knows other shells' names.
TEST(HistoryM9Review, HistcharsAndOptionAliases) {
  test::Shell sh;
  EXPECT_EQ(sh.run("print -r -- \"[$histchars][$HISTCHARS]\"").out, "[!^#][!^#]\n");
  sh.run("histchars=''");
  EXPECT_EQ(history_options(sh.ctx).histchars, "");
  HistoryManager h{history_options(sh.ctx)};
  h.add("echo abc");
  auto r = h.expand_line("echo !!");
  ASSERT_TRUE(r);
  EXPECT_EQ(r->text, "echo !!");
  sh.run("unset histchars");
  EXPECT_EQ(history_options(sh.ctx).histchars, "!^#");
  // Each name keeps its own unset flag: histchars stays unset.
  EXPECT_EQ(sh.run("HISTCHARS=x; print -r -- \"[$histchars][$HISTCHARS]\"").out, "[][x]\n");
  EXPECT_EQ(sh.run("histchars=y; print -r -- $HISTCHARS").out, "y\n");
  // histcharssetfn: three ASCII characters at most; unset, the defaults
  // under the other name.
  EXPECT_EQ(sh.run("histchars=abcd; print -r -- $histchars $HISTCHARS").out, "abc abc\n");
  auto bad = sh.run("histchars=é; print -r -- $histchars");
  EXPECT_EQ(bad.err, "jfsh: HISTCHARS can only contain ASCII characters\n");
  EXPECT_EQ(bad.out, "abc\n");
  EXPECT_EQ(sh.run("unset histchars; print -r -- \"[$HISTCHARS] ${+HISTCHARS} ${+histchars}\"").out,
            "[!^#] 1 0\n");
  EXPECT_EQ(sh.run("set -o histexpand; echo $?; set -o braceexpand; [[ -o ignorebraces ]]; echo $?")
                .out, "0\n1\n");
  EXPECT_EQ(sh.run("setopt nodotglob; [[ -o globdots ]]; echo $?; [[ -o nolog ]]; echo $?").out,
            "1\n1\n");
}

// fc -p applies its sizes to the file it reads at once; in a shell that
// isn't interactive it works too, and fc -P saves nothing there.
TEST(HistoryM9Review, FcPushReadsWithItsSizes) {
  test::TempDir dir;
  test::Shell sh;
  HistoryManager h{HistOptions{}};
  sh.ctx.history = &h;
  str file = (dir.path / "hh").string();
  sh.run("print -l l{1..9} > " + file);
  EXPECT_EQ(sh.run("fc -p " + file + " 3; fc -l 1").out, "    7  l7\n    8  l8\n    9  l9\n");
  EXPECT_EQ(sh.run("fc -P; echo $?").out, "0\n");
  str saved = (dir.path / "saved").string();
  sh.run("fc -p " + saved + " 3 3; print -s one; fc -P");
  EXPECT_FALSE(s::filesystem::exists(saved));
  sh.ctx.history = nullptr;
}

}  // namespace
}  // namespace jfsh
