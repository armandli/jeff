// The completion system (M15 phase 15c): compinit from the shipped zsh
// function library, compdef, compaudit, and completion through
// _main_complete, _arguments (comparguments), _describe, _values and the
// completer and matcher styles.  The library is the source tree's copy
// (share/jfsh/functions), so the tests do not depend on an installed zsh.
// Expected screens are zsh 5.9's (`zsh -f`, then `autoload -Uz compinit;
// compinit -D -u`), taken by typing the same keys into an interactive zsh
// on an 80x24 pty.

#include <sys/stat.h>

#include <fstream>
#include <string>
#include <string_view>

#include <editor_rig.h>
#include <gtest/gtest.h>
#include <shell_fixture.h>

#include <jfsh/core/aliases.h>

namespace jfsh {
namespace {

using test::Rig;
using test::Shell;
using test::TempDir;
namespace sf = s::filesystem;

const str FUNCTIONS_DIR = JFSH_SOURCE_FUNCTIONS_DIR;

str out(Shell& sh, strv src) { return sh.run(src).out; }

// A shell with compinit run on the library, as `compinit -D -u` does.
str compinit_setup() {
  return "fpath=(" + FUNCTIONS_DIR + "); autoload -Uz compinit; compinit -D -u";
}

// The process's working directory is `dir` while this lives: the editor
// completes files relative to it.
struct InDir {
  explicit InDir(const sf::path& dir) { sf::current_path(dir); }
  ~InDir() { sf::current_path(old); }
  sf::path old = sf::current_path();
};

// A scratch tree for file completion: rvt/ with Makefile, abc.txt,
// abd.txt, README, Readme.md and the directories other/ and sub.dir/.
struct FileTree {
  FileTree() {
    sf::path r = dir.path / "rvt";
    sf::create_directories(r / "other");
    sf::create_directories(r / "sub.dir");
    for (const char* f : {"Makefile", "abc.txt", "abd.txt", "README", "Readme.md"})
      dir.file("rvt/" + str(f), "", sf::perms::owner_read | sf::perms::owner_write);
  }
  TempDir dir;
};

// An editor whose shell has run compinit, with `setup` after it.
struct CRig : Rig {
  explicit CRig(strv setup = {}) {
    // zstyle's styles are the process's: none left from another test.
    sh.run("zstyle -d");
    sh.run(compinit_setup());
    if (not setup.empty()) sh.run(setup);
  }
  // Types `text`, then Tab, and waits for the completion to be shown: the
  // first Tab autoloads much of the library, so it may be slow to answer.
  void complete(strv text) {
    type(text);
    send("\t");
    for (int k = 0; k < 100 and not drain(100); ++k) {}
    settle();
  }
};

// ---- compinit ------------------------------------------------------------------------

// compinit's functions take over from the M9 builtins of the same names.
TEST(Compinit, FunctionsReplaceTheBuiltins) {
  Shell sh;
  EXPECT_EQ(out(sh, "whence -w compdef _files _directories"),
            "compdef: builtin\n_files: builtin\n_directories: builtin\n");
  sh.run(compinit_setup());
  EXPECT_EQ(out(sh, "whence -w compdef _files _directories"),
            "compdef: function\n_files: function\n_directories: function\n");
}

TEST(Compinit, DefinesTheSystem) {
  Shell sh;
  sh.run(compinit_setup());
  EXPECT_EQ(out(sh, "echo $_comps[ls] $_comps[-default-] ${+functions[compdef]} "
                    "${+functions[_main_complete]}"),
            "_ls _default 1 1\n");
  // The completion widgets now run _main_complete.
  EXPECT_NE(out(sh, "zle -lL").find("zle -C complete-word .complete-word _main_complete"),
            str::npos);
}

TEST(Compinit, CompdefAddsAndRemoves) {
  Shell sh;
  sh.run(compinit_setup());
  EXPECT_EQ(out(sh, "compdef _foo foo bar; echo $_comps[foo] $_comps[bar]; compdef -d foo; "
                    "echo ${+_comps[foo]} $_comps[bar]"),
            "_foo _foo\n0 _foo\n");
  EXPECT_EQ(out(sh, "compdef '_files -g \"*.c\"' cc1; echo $_comps[cc1]"),
            "_files -g \"*.c\"\n");
  EXPECT_EQ(out(sh, "compdef -p _pfoo 'pf*'; echo $_patcomps[pf*]"), "_pfoo\n");
}

TEST(Compinit, WritesADump) {
  TempDir dir;
  Shell sh;
  str dump = (dir.path / "dump").string();
  sh.run("fpath=(" + FUNCTIONS_DIR + "); autoload -Uz compinit; compinit -u -d " + dump);
  s::ifstream in{dump};
  str first;
  s::getline(in, first);
  EXPECT_EQ(first.rfind("#files: ", 0), 0u) << first;
  EXPECT_NE(out(sh, "cat " + dump).find("'ls' '_ls'"), str::npos);
  // A second compinit reads the dump.
  Shell again;
  again.run("fpath=(" + FUNCTIONS_DIR + "); autoload -Uz compinit; compinit -u -d " + dump);
  EXPECT_EQ(out(again, "echo $_comps[ls]"), "_ls\n");
}

TEST(Compinit, CompauditFindsInsecureDirectories) {
  TempDir dir;
  sf::path bad = dir.path / "bad";
  sf::create_directory(bad);
  ::chmod(bad.c_str(), 0777);
  Shell sh;
  EXPECT_EQ(out(sh, "fpath=(" + bad.string() + " " + FUNCTIONS_DIR + "); "
                    "autoload -Uz compaudit; compaudit 2>/dev/null; echo $?"),
            bad.string() + "\n1\n");
}

// ---- _arguments ----------------------------------------------------------------------

const str FOO_ARGS =
    "_foo() { _arguments '-opt[an option]' '--long=[long opt]:val:(a b)' '(- *)-h[help]' "
    "'*::rest:(r1 r2)' }; compdef _foo foo";

TEST(Compsys, ArgumentsListsOptionsWithDescriptions) {
  CRig t{FOO_ARGS};
  t.complete("foo -");
  EXPECT_EQ(t.screen.row(0), "> foo -");
  EXPECT_EQ(t.screen.row(1), "--long  -- long opt");
  EXPECT_EQ(t.screen.row(2), "-h      -- help");
  EXPECT_EQ(t.screen.row(3), "-opt    -- an option");
}

TEST(Compsys, ArgumentsCompletesAnOptionsValue) {
  CRig t{FOO_ARGS};
  t.complete("foo --long=");
  EXPECT_EQ(t.screen.row(0), "> foo --long=");
  EXPECT_EQ(t.screen.row(1), "a  b");
}

TEST(Compsys, ArgumentsRestArguments) {
  CRig t{FOO_ARGS};
  t.complete("foo -opt ");
  EXPECT_EQ(t.screen.row(0), "> foo -opt r");
}

TEST(Compsys, ArgumentsStateAction) {
  CRig t{"_foo() { local state; _arguments '1:first:->one' && return; "
         "case $state in one) compadd alpha beta;; esac }; compdef _foo foo"};
  t.complete("foo ");
  EXPECT_EQ(t.screen.row(1), "alpha  beta");
}

TEST(Compsys, ArgumentsCodeAction) {
  CRig t{"_foo() { _arguments '1:num:{compadd 11 22}' }; compdef _foo foo"};
  t.complete("foo ");
  EXPECT_EQ(t.screen.row(1), "11  22");
}

TEST(Compsys, ArgumentsExclusions) {
  CRig t{"_foo() { _arguments '(-b)-a[aaa]' '(-a)-b[bbb]' '-c[ccc]' }; compdef _foo foo"};
  t.complete("foo -a -");
  EXPECT_EQ(t.screen.row(0), "> foo -a -c");
}

TEST(Compsys, ArgumentsClusteredOptions) {
  CRig t{"_foo() { _arguments -s '-a[aa]' '-b[bb]' '-c+[cc]:cv:(x1 x2)' }; compdef _foo foo"};
  t.complete("foo -ac");
  EXPECT_EQ(t.screen.row(0), "> foo -acx");
}

// ---- _describe, _values, compstate -------------------------------------------------

TEST(Compsys, DescribeLinesUpDescriptions) {
  CRig t{"_foo() { local -a cmds; cmds=('add:add a file' 'commit:record changes' "
         "'checkout:switch branches'); _describe -t commands command cmds }; compdef _foo foo"};
  t.complete("foo ");
  EXPECT_EQ(t.screen.row(1), "add       -- add a file");
  EXPECT_EQ(t.screen.row(2), "checkout  -- switch branches");
  EXPECT_EQ(t.screen.row(3), "commit    -- record changes");
}

TEST(Compsys, ValuesWithSeparator) {
  CRig t{"_foo() { _values -s , 'item' alpha beta gamma }; compdef _foo foo"};
  t.complete("foo alpha,");
  EXPECT_EQ(t.screen.row(1), "beta   gamma");
}

TEST(Compsys, CompstateFeedback) {
  CRig t{"_foo() { compadd aa ab; compstate[insert]=menu; compstate[list]='' }; "
         "compdef _foo foo"};
  t.complete("foo a");
  EXPECT_EQ(t.screen.row(0), "> foo aa");
  EXPECT_EQ(t.screen.row(1), "");
}

// ---- styles ----------------------------------------------------------------------------

TEST(Compsys, ApproximateCompleter) {
  CRig t{"zstyle ':completion:*' completer _complete _approximate; "
         "_foo() { compadd foobar }; compdef _foo foo"};
  t.complete("foo fobar");
  EXPECT_EQ(t.screen.row(0), "> foo foobar");
}

TEST(Compsys, FormatAndGroupName) {
  CRig t{"zstyle ':completion:*:descriptions' format '%B%d%b'; "
         "zstyle ':completion:*' group-name ''; " + FOO_ARGS};
  t.complete("foo -");
  EXPECT_EQ(t.screen.row(1), "option");
  EXPECT_EQ(t.screen.row(2), "--long  -- long opt");
}

// ---- files ---------------------------------------------------------------------------

// A file in a subdirectory gets its space, a directory its slash.
TEST(Compsys, FilesInASubdirectory) {
  FileTree tree;
  InDir in{tree.dir.path};
  CRig t;
  t.complete("ls rvt/Mak");
  EXPECT_EQ(t.screen.row(0), "> ls rvt/Makefile");
  EXPECT_EQ(t.screen.cursor().second, 18);
}

TEST(Compsys, DirectoryInASubdirectory) {
  FileTree tree;
  InDir in{tree.dir.path};
  CRig t;
  t.complete("ls rvt/oth");
  EXPECT_EQ(t.screen.row(0), "> ls rvt/other/");
}

// cd lists the directories, marked as such.
TEST(Compsys, CdListsDirectories) {
  FileTree tree;
  InDir in{tree.dir.path};
  CRig t;
  t.complete("cd rvt/");
  EXPECT_EQ(t.screen.row(0), "> cd rvt/");
  EXPECT_EQ(t.screen.row(1), "other/    sub.dir/");
}

TEST(Compsys, LsListsFiles) {
  FileTree tree;
  InDir in{tree.dir.path / "rvt"};
  CRig t;
  // (Lower-case names: the test process sorts in the C locale.)
  t.complete("ls ab");
  EXPECT_EQ(t.screen.row(0), "> ls ab");
  EXPECT_EQ(t.screen.row(1), "abc.txt  abd.txt");
}

TEST(Compsys, MatcherListOnFiles) {
  FileTree tree;
  InDir in{tree.dir.path};
  CRig t{"zstyle ':completion:*' matcher-list 'm:{a-z}={A-Z}'"};
  t.complete("ls rvt/make");
  EXPECT_EQ(t.screen.row(0), "> ls rvt/Makefile");
  EXPECT_EQ(t.screen.cursor().second, 18);
}

// Several corrections: menu completion through them, the original listed
// after them (compstate[pattern_insert] is menu).
TEST(Compsys, ApproximateWithSeveralCorrections) {
  FileTree tree;
  InDir in{tree.dir.path / "rvt"};
  CRig t{"zstyle ':completion:*' completer _complete _approximate"};
  t.complete("ls abx");
  EXPECT_EQ(t.screen.row(0), "> ls abc.txt");
  EXPECT_EQ(t.screen.row(1), "abc.txt  abd.txt");
  EXPECT_EQ(t.screen.row(2), "abx");
  t.send("\t");
  t.settle();
  EXPECT_EQ(t.screen.row(0), "> ls abd.txt");
}

// A hidden -s suffix after a directory: no slash of its own (zsh), as
// _path_files adds the rest of the path (`_approximate` on `rvt/abx`).
TEST(Compsys, HiddenSuffixTakesNoSlash) {
  FileTree tree;
  InDir in{tree.dir.path};
  CRig t{"_foo() { compadd -Qf -s /abx -W '' rvt }; _bar() { compadd -Qf -W '' rvt }; "
         "compdef _foo foo; compdef _bar bar"};
  t.complete("foo r");
  EXPECT_EQ(t.screen.row(0), "> foo rvt/abx");
  EXPECT_EQ(t.screen.cursor().second, 13);
  t.send("\x03");
  t.finish();
  t.complete("bar r");
  EXPECT_EQ(t.screen.row(0), "> bar rvt/");
}

}  // namespace
}  // namespace jfsh
