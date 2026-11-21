#include <cstdio>
#include <cstdlib>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <jfsh/core/aliases.h>
#include <jfsh/core/jobs.h>
#include <jfsh/core/options.h>
#include <jfsh/core/param.h>
#include <jfsh/core/signals.h>
#include <jfsh/exec/executor.h>
#include <shell_fixture.h>

namespace jfsh {
namespace {

using test::Run;
using test::Shell;
using test::TempDir;
namespace sf = s::filesystem;

constexpr sf::perms EXEC = sf::perms::owner_all;
constexpr sf::perms NO_EXEC = sf::perms::owner_read | sf::perms::owner_write;

// ---- lists, sublists, $? ---------------------------------------------------

TEST(Exec, StatusIsTheLastCommands) {
  Shell sh;
  EXPECT_EQ(sh.run("true; false").rc, 1);
  EXPECT_EQ(sh.ctx.last_exit, 1);
  EXPECT_EQ(sh.run("false; true").rc, 0);
  EXPECT_EQ(sh.ctx.last_exit, 0);
}

TEST(Exec, AndOrSkipsAPipelineButKeepsGoing) {
  Shell sh;
  EXPECT_EQ(sh.run("false && echo no || echo yes").out, "yes\n");
  EXPECT_EQ(sh.run("true || echo no && echo yes").out, "yes\n");
  EXPECT_EQ(sh.run("false && echo a && echo b").out, "");
  EXPECT_EQ(sh.run("false && echo a").rc, 1);
}

TEST(Exec, NegatedPipeline) {
  Shell sh;
  EXPECT_EQ(sh.run("! false").rc, 0);
  EXPECT_EQ(sh.run("! true").rc, 1);
}

TEST(Exec, ExitStopsTheProgram) {
  Shell sh;
  auto r = sh.run("exit 3; echo no");
  EXPECT_EQ(r.rc, 3);
  EXPECT_EQ(r.out, "");
  EXPECT_TRUE(sh.ctx.exiting);
}

TEST(Exec, MultiLineProgramsAndComments) {
  Shell sh;
  EXPECT_EQ(sh.run("echo one # gone\n\necho two;\n").out, "one\ntwo\n");
}

TEST(Exec, InteractiveCommentsNeedTheOption) {
  Shell sh;
  sh.opts.assign(OPT_INTERACTIVE, true);
  EXPECT_EQ(sh.run("echo a # b").out, "a # b\n");
  sh.opts.assign(OPT_INTERACTIVECOMMENTS, true);
  EXPECT_EQ(sh.run("echo a # b").out, "a\n");
}

// ---- words ----------------------------------------------------------------

TEST(Exec, QuoteRemoval) {
  Shell sh;
  EXPECT_EQ(sh.run(R"(echo 'a  b' "c\"d" e\ f 'x'"y" "\q")").out,
            "a  b c\"d e f xy \\q\n");
}

TEST(Exec, ReservedWordsAsArguments) {
  Shell sh;
  EXPECT_EQ(sh.run("echo done in then").out, "done in then\n");
}

// ---- assignments ------------------------------------------------------------

TEST(Exec, AssignmentSetsAShellParameter) {
  Shell sh;
  EXPECT_EQ(sh.run("X='a b'; Y=1 Z=2").rc, 0);
  EXPECT_EQ(sh.get("X"), "a b");
  EXPECT_EQ(sh.get("Y"), "1");
  EXPECT_EQ(sh.get("Z"), "2");
  EXPECT_FALSE(sh.params.lookup("X")->get().is_exported());
}

TEST(Exec, AppendAssignment) {
  Shell sh;
  sh.run("X=a; X+=b; N+=new");
  EXPECT_EQ(sh.get("X"), "ab");
  EXPECT_EQ(sh.get("N"), "new");
}

// An assignment inside a function scope updates the global it names instead
// of creating a shadowing local (bug B13).
TEST(Exec, AssignmentUpdatesTheExistingParameter) {
  Shell sh;
  sh.params.assign("G", str("old"), PM_SCALAR);
  sh.params.push_scope();
  sh.run("G=new; L=created");
  sh.params.pop_scope();
  EXPECT_EQ(sh.get("G"), "new");
  EXPECT_EQ(sh.get("L"), "created");
}

TEST(Exec, ReadonlyParameterRejectsAssignment) {
  Shell sh;
  sh.params.assign("R", str("fixed"), PM_SCALAR | PM_READONLY);
  auto r = sh.run("R=changed");
  EXPECT_EQ(r.rc, 1);
  EXPECT_NE(r.err.find("read-only variable: R"), str::npos);
  EXPECT_EQ(sh.get("R"), "fixed");

  r = sh.run("R=changed echo ran");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.out, "");

  // In front of a builtin or function it ends the command line, as zsh's
  // errflag does; in front of an external command the line goes on.
  r = sh.run("R=changed :; echo no");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.out, "");
  r = sh.run("f() { :; }; R=changed f; echo no");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.out, "");
  r = sh.run("setopt posixbuiltins; R=changed :; echo st=$?");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.out, "");
  r = sh.run("R=changed /bin/echo hi; echo $?");
  EXPECT_EQ(r.out, "1\n");
  EXPECT_EQ(r.err, "jfsh: read-only variable: R\n");
}

// Each assignment is expanded and made before the next is expanded, with
// or without a command, and the command's words are expanded first.
TEST(Exec, AssignmentsSeeTheOnesBeforeThem) {
  Shell sh;
  EXPECT_EQ(sh.run("a=1 b=$a c=$b; echo $c").out, "1\n");
  EXPECT_EQ(sh.run("x=1 y=$x; x=5 y=$x; echo $x $y").out, "5 5\n");
  EXPECT_EQ(sh.run("unset x y; x=1 y=$x eval 'echo $y'").out, "1\n");
  EXPECT_EQ(sh.run("unset x y; x=1 y=$x sh -c 'echo $y'").out, "1\n");
  EXPECT_EQ(sh.run("x=1; x=2 y=$x eval 'echo $x $y'; echo $x").out, "2 2\n1\n");
  EXPECT_EQ(sh.run("x=$(echo A >&2) : $(echo B >&2) > $(echo C >&2; echo /dev/null)").err,
            "B\nC\nA\n");
}

// `name+=word` on an array appends an element; on an associative array it
// makes a scalar, as in zsh.
TEST(Exec, AppendToAnArrayAddsAnElement) {
  Shell sh;
  EXPECT_EQ(sh.run("a=(1 2); a+=x; typeset -p a").out, "typeset -a a=( 1 2 x )\n");
  EXPECT_EQ(sh.run("a=(); a+=x; typeset -p a").out, "typeset -a a=( x )\n");
  EXPECT_EQ(sh.run("a=(1 2); a+=x eval 'echo $a'; echo $a").out, "1 2 x\n1 2\n");
  EXPECT_EQ(sh.run("typeset -A h=(k v); h+=x; typeset -p h").out, "typeset h=x\n");
}

// In front of a command an assignment keeps the parameter's type and
// attributes: arithmetic for an integer, case for -u/-l.
TEST(Exec, PrefixAssignmentKeepsTheParametersType) {
  Shell sh;
  EXPECT_EQ(sh.run("integer i=3; i=2+3 eval 'echo $i'").out, "5\n");
  EXPECT_EQ(sh.run("integer i=3; i+=1 sh -c 'echo $i'; echo $i").out, "4\n3\n");
  EXPECT_EQ(sh.run("integer i=3; i=7 eval 'typeset -p i'").out, "typeset -i i=7\n");
  EXPECT_EQ(sh.run("typeset -u U=a; U=b eval 'echo $U'").out, "B\n");
}

// In front of a function they are exported to what it runs, and the old
// value comes back afterwards, even when the function assigns.
TEST(Exec, PrefixAssignmentBeforeAFunctionIsExportedThenRestored) {
  Shell sh;
  auto r = sh.run("a=0; f() { echo in $a; sh -c 'echo env $a'; a=5; }; a=1 f; echo out $a;"
                  " sh -c 'echo after [$a]'");
  EXPECT_EQ(r.out, "in 1\nenv 1\nout 0\nafter []\n");
}

// In front of `exec` with redirections the assignments last only for it;
// alone, or with only redirections, they stay.  (Subshells: the exec
// redirections would otherwise stay in the test process.)
TEST(Exec, AssignmentsBeforeExecRedirectionsAreTemporary) {
  Shell sh;
  EXPECT_EQ(sh.run("(u=2 exec 8>/dev/null; echo \"[$u]\"; u=5; u=3 exec 8>/dev/null;"
                   " echo \"[$u]\")").out, "[]\n[5]\n");
  EXPECT_EQ(sh.run("(export v=0; v=1 exec 8>/dev/null; echo $v)").out, "0\n");
  EXPECT_EQ(sh.run("x=1 exec; y=2 >/dev/null; echo $x $y").out, "1 2\n");
  auto r = sh.run("(readonly r; r=1 exec 8>/dev/null; echo next)");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: read-only variable: r\n");
}

// typeset's `name=value` arguments are expanded after the assignments in
// front of it, which a new local does not land among.
TEST(Exec, PrefixAssignmentsComeBeforeTypesetArguments) {
  Shell sh;
  EXPECT_EQ(sh.run("x=1 typeset y=$x; echo \"[$x] [$y]\"").out, "[] [1]\n");
  EXPECT_EQ(sh.run("x=2 export w=($x); echo \"[$w]\"").out, "[2]\n");
  EXPECT_EQ(sh.run("f() { y=1 local z=$y; echo \"in [$z]\"; }; f; echo \"[$z]\"").out,
            "in [1]\n[]\n");
  EXPECT_EQ(sh.run("x=0; x=1 typeset x=5; echo \"[$x]\"").out, "[0]\n");
  EXPECT_EQ(sh.run("setopt posixbuiltins; x=1 export y=$x; echo \"[$x] [$y]\"").out,
            "[1] [1]\n");
}

// The typeset variants are POSIX-special in zsh: under POSIX_BUILTINS the
// assignments in front of them stay.
TEST(Exec, PrefixAssignmentsBeforeTypesetPersistUnderPosixBuiltins) {
  Shell sh;
  EXPECT_EQ(sh.run("setopt posixbuiltins; x=1 typeset q; y=1 local r; z=1 declare t;"
                   " echo \"[$x$y$z]\"").out, "[111]\n");
  EXPECT_EQ(sh.run("setopt posixbuiltins; unset x y; x=1 typeset y=$x; echo \"[$x$y]\"").out,
            "[11]\n");
  EXPECT_EQ(sh.run("zmodload zsh/param/private; setopt posixbuiltins; unset x;"
                   " f() { x=1 private q; echo \"[$x]\"; }; f").out, "[1]\n");
}

// `command` takes away the special status; `builtin` doesn't.
TEST(Exec, CommandMakesASpecialBuiltinOrdinary) {
  Shell sh;
  EXPECT_EQ(sh.run("setopt posixbuiltins; unset x x2 x3 y; x=1 command typeset q;"
                   " x2=1 command export q2; x3=1 command typeset y=$x3;"
                   " echo \"[$x$x2$x3$y]\"").out, "[]\n");
  EXPECT_EQ(sh.run("setopt posixbuiltins; unset x; x=1 builtin export q; echo \"[$x]\"").out,
            "[1]\n");
}

// typeset's arguments are expanded after the command's redirections.
TEST(Exec, TypesetArgumentsExpandAfterRedirections) {
  Shell sh;
  auto r = sh.run("x=1 typeset y=${zz?boom} 2>/dev/null; echo after");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "");
  r = sh.run("f() { x=1 local y=${zz?boom} 2>/dev/null; echo in; }; f; echo out");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "");
}

// An unset prefix copy is gone for typeset -p.
TEST(Exec, UnsetPrefixCopyIsNoSuchVariable) {
  Shell sh;
  auto r = sh.run("unset x; f() { unset x; typeset -p x; echo rc=$?; }; x=1 f");
  EXPECT_EQ(r.out, "rc=1\n");
  EXPECT_EQ(r.err, "jfsh: typeset: no such variable: x\n");
  EXPECT_EQ(sh.run("y=0; h() { unset y; typeset -p y; echo rc=$?; }; y=1 h 2>/dev/null").out,
            "rc=1\n");
}

// A failed expansion in a typeset argument ends the command line.
TEST(Exec, FailedTypesetArgumentExpansionEndsTheLine) {
  Shell sh;
  auto r = sh.run("x=1 typeset y=${zz?boom}; echo after");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: zz: boom\n");
  EXPECT_EQ(sh.run("f() { typeset y=${zz?boom}; echo in; }; f; echo out").out, "");
}

// local/typeset in a function or eval run with prefix assignments declare
// in the function, not in the prefix scope; a private parameter is seen
// through a prefix scope but not by a function called from it.
TEST(Exec, LocalsUnderAPrefixScope) {
  Shell sh;
  EXPECT_EQ(sh.run("f() { local y=L; typeset z=Z; echo $x$y$z; }; x=1 f;"
                   " echo \"[$x$y$z]\"").out, "1LZ\n[]\n");
  EXPECT_EQ(sh.run("f() { x=1 eval 'local w=2'; echo \"[$w]\"; }; f").out, "[2]\n");
  EXPECT_EQ(sh.run("f() { zmodload zsh/param/private; private p=1; x=2 eval 'echo \"[$p]\"';"
                   " x=2 g; }; g() { echo \"g[$p]\"; }; f").out, "[1]\ng[]\n");
}

// In a function, typeset -p shows a parameter from outside it with -g; a
// prefix copy counts as the parameter it stands for.
TEST(Exec, TypesetPrintMarksOuterParametersGlobal) {
  Shell sh;
  EXPECT_EQ(sh.run("unset x; f() { x=1 typeset -p x; }; f").out, "typeset -g x=1\n");
  EXPECT_EQ(sh.run("x=1; f() { typeset -p x; }; f").out, "typeset -g x=1\n");
  EXPECT_EQ(sh.run("f() { local y=2; y=3 typeset -p y; }; f").out, "typeset y=3\n");
  EXPECT_EQ(sh.run("integer i=3; f() { i=7 typeset -p i; }; f").out, "typeset -g -i i=7\n");
  EXPECT_EQ(sh.run("export E=1; f() { typeset -p E; }; f").out, "export E=1\n");
  EXPECT_EQ(sh.run("unset x; x=1 typeset -p x").out, "typeset x=1\n");
}

// `a[i]=x` in front of a builtin or function changes the parameter itself;
// in front of an external command (made in the child) it doesn't.
TEST(Exec, SubscriptedPrefixAssignmentPersists) {
  Shell sh;
  EXPECT_EQ(sh.run("a=(1 2); a[2]=z :; echo $a").out, "1 z\n");
  EXPECT_EQ(sh.run("typeset -A h; h[k]=v :; echo ${(kv)h}").out, "k v\n");
  EXPECT_EQ(sh.run("a=(1 2); f() { echo in $a; }; a[2]=z f; echo $a").out,
            "in 1 z\n1 z\n");
  EXPECT_EQ(sh.run("a=(1 2); a[2]=z /usr/bin/true; echo $a").out, "1 2\n");
}

// zsh doesn't export arrays; one in front of a command hides an exported
// scalar of that name.
TEST(Exec, PrefixArrayIsNotExported) {
  Shell sh;
  EXPECT_EQ(sh.run("FOO=(a b) sh -c 'echo [$FOO]'").out, "[]\n");
  EXPECT_EQ(sh.run("export FOO=z; FOO=(a b) sh -c 'echo [$FOO]'").out, "[]\n");
}

TEST(Exec, PrefixAssignmentGoesOnlyToTheChildsEnvironment) {
  Shell sh;
  auto r = sh.run("V=tmp env");
  EXPECT_NE(("\n" + r.out).find("\nV=tmp\n"), str::npos);
  EXPECT_EQ(sh.get("V"), "<unset>");
}

TEST(Exec, PrefixAssignmentBeforeABuiltinIsTemporary) {
  Shell sh;
  sh.run("V=tmp true; S=tmp :");
  EXPECT_EQ(sh.get("V"), "<unset>");
  EXPECT_EQ(sh.get("S"), "<unset>");
}

TEST(Exec, PrefixAssignmentBeforeASpecialBuiltinPersistsUnderPosixBuiltins) {
  Shell sh;
  sh.opts.assign(OPT_POSIXBUILTINS, true);
  sh.run("S=kept :; V=tmp true");
  EXPECT_EQ(sh.get("S"), "kept");
  EXPECT_EQ(sh.get("V"), "<unset>");
}

// Children see the shell's parameters, not the process environment (B12).
TEST(Exec, ExportedParametersReachChildren) {
  Shell sh;
  sh.params.assign("E", str("1"), PM_SCALAR | PM_EXPORTED);
  auto r = sh.run("E=2; U=unexported; env");
  EXPECT_NE(("\n" + r.out).find("\nE=2\n"), str::npos);
  EXPECT_EQ(("\n" + r.out).find("\nU="), str::npos);
}

// ---- external commands -----------------------------------------------------

TEST(Exec, CommandNotFoundIs127) {
  Shell sh;
  auto r = sh.run("jfsh_no_such_command_xyz");
  EXPECT_EQ(r.rc, 127);
  EXPECT_EQ(r.err, "jfsh: command not found: jfsh_no_such_command_xyz\n");
}

TEST(Exec, MissingPathIs127) {
  Shell sh;
  TempDir dir;
  auto r = sh.run((dir.path / "missing").string());
  EXPECT_EQ(r.rc, 127);
  EXPECT_NE(r.err.find("no such file or directory"), str::npos);
}

TEST(Exec, NotExecutableIs126) {
  Shell sh;
  TempDir dir;
  auto file = dir.file("data", "x\n", NO_EXEC);
  auto r = sh.run(file.string());
  EXPECT_EQ(r.rc, 126);
  EXPECT_NE(r.err.find("permission denied"), str::npos);

  // Found on $PATH but not executable.
  sh.params.set_scalar("PATH", dir.path.string());
  r = sh.run("data");
  EXPECT_EQ(r.rc, 126);
  EXPECT_EQ(r.err, "jfsh: permission denied: data\n");
}

TEST(Exec, SignalledCommandIs128PlusSignal) {
  Shell sh;
  EXPECT_EQ(sh.run("sh -c 'kill -TERM $$'").rc, 128 + 15);
}

TEST(Exec, ExitStatusOfExternalCommand) {
  Shell sh;
  EXPECT_EQ(sh.run("sh -c 'exit 7'").rc, 7);
}

TEST(Exec, ScriptWithoutShebangRunsWithSh) {
  Shell sh;
  TempDir dir;
  auto script = dir.file("plain", "echo from-sh \"$1\"\n", EXEC);
  EXPECT_EQ(sh.run(script.string() + " arg").out, "from-sh arg\n");
  // Also as a pipeline stage, a background job and under exec.
  EXPECT_EQ(sh.run(script.string() + " arg | cat").out, "from-sh arg\n");
  EXPECT_EQ(sh.run(script.string() + " arg & wait").out, "from-sh arg\n");
  EXPECT_EQ(sh.run("(exec " + script.string() + " arg)").out, "from-sh arg\n");
}

// exec failures report zsh's message (lowercased strerror, then the name)
// and status: 126 for a file that can't be run, 127 for anything else.
TEST(Exec, ExecFailureMessagesAndStatuses) {
  Shell sh;
  TempDir dir;
  auto plain  = dir.file("plain", "echo hi\n", EXEC).string();
  auto noexec = dir.file("noexec", "echo hi\n", NO_EXEC).string();
  auto badsb  = dir.file("badsb", "#!/nonexistent/interp\necho x\n", EXEC).string();
  auto binary = dir.file("binary", s::string("echo a\0b\n", 9), EXEC).string();

  auto r = sh.run(plain + "/x");
  EXPECT_EQ(r.rc, 127);
  EXPECT_EQ(r.err, "jfsh: not a directory: " + plain + "/x\n");

  r = sh.run(badsb);
  EXPECT_EQ(r.rc, 127);
  EXPECT_EQ(r.err, "jfsh: " + badsb
                   + ": bad interpreter: /nonexistent/interp: no such file or directory\n");

  // A file with NUL bytes is not handed to /bin/sh.
  r = sh.run(binary);
  EXPECT_EQ(r.rc, 126);
  EXPECT_EQ(r.err, "jfsh: exec format error: " + binary + "\n");

  r = sh.run("(exec " + noexec + ")");
  EXPECT_EQ(r.rc, 126);
  EXPECT_EQ(r.err, "jfsh: permission denied: " + noexec + "\n");

  r = sh.run(dir.path.string());
  EXPECT_EQ(r.rc, 126);
  EXPECT_EQ(r.err, "jfsh: permission denied: " + dir.path.string() + "\n");
}

// A directory named like the command on $PATH is "permission denied".
TEST(Exec, DirectoryOnPathIs126) {
  Shell sh;
  TempDir dir;
  sf::create_directory(dir.path / "dircmd");
  auto r = sh.run("PATH=" + dir.path.string() + ":$PATH; dircmd; echo $?");
  EXPECT_EQ(r.out, "126\n");
  EXPECT_EQ(r.err, "jfsh: permission denied: dircmd\n");
}

// `PATH=… cmd` searches the new $PATH, as zsh does.
TEST(Exec, PrefixPathIsSearched) {
  Shell sh;
  TempDir dir;
  dir.file("plain", "echo from-sh \"$0\" \"$1\"\n", EXEC);
  auto r = sh.run("PATH=" + dir.path.string() + " plain a");
  EXPECT_EQ(r.out, "from-sh " + (dir.path / "plain").string() + " a\n");

  r = sh.run("PATH=/nonexistent ls; echo $?");
  EXPECT_EQ(r.out, "127\n");
  EXPECT_EQ(r.err, "jfsh: command not found: ls\n");
  r = sh.run("PATH=/nonexistent command ls; echo $?");
  EXPECT_EQ(r.out, "127\n");
  EXPECT_EQ(r.err, "jfsh: command not found: ls\n");
  r = sh.run("(PATH=" + dir.path.string() + " exec plain b)");
  EXPECT_EQ(r.out, "from-sh " + (dir.path / "plain").string() + " b\n");

  // The shell still looks the name up (and hashes it) in its own $PATH.
  r = sh.run("PATH=/bin ls -d / >/dev/null; hash | grep -c '^ls='");
  EXPECT_EQ(r.out, "1\n");
}

// `command -p` searches only the default $PATH: a name found there alone
// is not run from the user's.
TEST(Exec, CommandDashPSearchesOnlyTheDefaultPath) {
  Shell sh;
  TempDir dir;
  dir.file("jfshcmd", "echo ran\n", EXEC);
  auto r = sh.run("PATH=" + dir.path.string() + ":$PATH; command -p jfshcmd; echo $?");
  EXPECT_EQ(r.out, "127\n");
  EXPECT_EQ(r.err, "jfsh: command not found: jfshcmd\n");
  EXPECT_EQ(sh.run("command -p ls -d /").out, "/\n");
}

TEST(Exec, BuiltinOutputComesBeforeTheNextChilds) {
  Shell sh;
  EXPECT_EQ(sh.run("echo first; sh -c 'echo second'; echo third").out,
            "first\nsecond\nthird\n");
}

TEST(Exec, PathChangesInvalidateTheCommandHash) {
  Shell sh;
  TempDir one, two;
  auto in_one = one.file("jfshcmd", "#!/bin/sh\n", EXEC);
  auto in_two = two.file("jfshcmd", "#!/bin/sh\n", EXEC);

  sh.params.set_scalar("PATH", one.path.string());
  EXPECT_EQ(sh.exec.find_command("jfshcmd"), in_one.string());
  EXPECT_EQ(sh.exec.find_command("jfshcmd"), in_one.string());  // hashed

  sh.params.set_scalar("PATH", two.path.string() + ":" + one.path.string());
  EXPECT_EQ(sh.exec.find_command("jfshcmd"), in_two.string());
}

// Any assignment to $PATH drops the hash, even one that keeps its text.
TEST(Exec, AnyPathAssignmentDropsTheCommandHash) {
  Shell sh;
  TempDir a, b;
  a.file("jfshcmd", "echo A\n", NO_EXEC);
  b.file("jfshcmd", "echo B\n", EXEC);
  sh.params.set_scalar("PATH", a.path.string() + ":" + b.path.string() + ":/bin:/usr/bin");
  EXPECT_EQ(sh.run("jfshcmd; chmod +x " + (a.path / "jfshcmd").string() + "; jfshcmd;"
                   " PATH=$PATH; jfshcmd").out, "B\nB\nA\n");
  // Also through export and typeset.
  EXPECT_EQ(sh.run("hash ls=/bin/echo; export PATH=$PATH; ls -d /").out, "/\n");
  EXPECT_EQ(sh.run("hash ls=/bin/echo; typeset PATH=$PATH; ls -d /").out, "/\n");
}

TEST(Exec, NoHashCmdsDoesNotCache) {
  Shell sh;
  TempDir dir;
  dir.file("jfshcmd", "echo ran\n", EXEC);
  sh.params.set_scalar("PATH", dir.path.string() + ":/bin:/usr/bin");
  EXPECT_EQ(sh.run("setopt nohashcmds; jfshcmd").out, "ran\n");
  EXPECT_FALSE(sh.exec.command_hash().contains("jfshcmd"));
  EXPECT_EQ(sh.run("setopt hashcmds; jfshcmd").out, "ran\n");
  EXPECT_TRUE(sh.exec.command_hash().contains("jfshcmd"));
  // Entries made with `hash` are still used.
  EXPECT_EQ(sh.run("setopt nohashcmds; hash ls=/bin/echo; ls -d /").out, "-d /\n");
}

TEST(Exec, EmptyPathComponentMeansCurrentDirectory) {
  Shell sh;
  TempDir dir;
  dir.file("jfshcmd", "#!/bin/sh\n", EXEC);
  auto cwd = sf::current_path();
  sf::current_path(dir.path);
  sh.params.set_scalar("PATH", "/nonexistent:");
  auto found = sh.exec.find_command("jfshcmd");
  sf::current_path(cwd);
  // zsh runs the name as it is, so $0 has no `./`.
  EXPECT_EQ(found, "jfshcmd");
}

// `PATH=` alone is one empty component: the current directory.
TEST(Exec, EmptyPathSearchesTheCurrentDirectory) {
  Shell sh;
  TempDir dir;
  dir.file("plain", "echo from-sh \"$0\"\n", EXEC);
  EXPECT_EQ(sh.run("(cd " + dir.path.string() + "; PATH=; plain)").out, "from-sh plain\n");
}

// ---- xtrace -----------------------------------------------------------------

// The trace is printed before the command runs (bug B33), with quoting.
TEST(Exec, XtracePrintsPs4AndTheExpandedCommandFirst) {
  Shell sh;
  sh.opts.assign(OPT_XTRACE, true);
  // Assignments in front of a builtin get their own PS4, as in zsh.
  auto r = sh.run("X='a b' echo hi 'c d'");
  EXPECT_EQ(r.err, "+jfsh:1> X='a b' +jfsh:1> echo hi 'c d'\n");
  EXPECT_EQ(r.out, "hi c d\n");

  sh.params.set_scalar("PS4", "trace> ");
  EXPECT_EQ(sh.run("echo it''s").err, "trace> echo its\n");
}

// zsh's quoting: `=` is quoted, `!` and non-ASCII are not, empty words are
// '', and control characters use $'…'.
TEST(Exec, XtraceQuotesAsZshDoes) {
  Shell sh;
  sh.opts.assign(OPT_XTRACE, true);
  sh.params.set_scalar("PS4", "+");
  EXPECT_EQ(sh.run("echo x=y ! é $'\\t' '' \"a'b\" $'\\x01' >/dev/null").err,
            "+echo 'x=y' ! é $'\\t' '' 'a'\\''b' $'\\C-A'\n");
  EXPECT_EQ(sh.run("a=1 b='x y'").err, "+a=1 b='x y' \n");
  EXPECT_EQ(sh.run("c=() d=(1 '2 3')").err, "+c=( ) d=( 1 '2 3' ) \n");
  EXPECT_EQ(sh.run("a=1 b=$a :").err, "+a=1 b=1 +:\n");
  EXPECT_EQ(sh.run("a=1 /usr/bin/true").err, "+a=1 /usr/bin/true\n");
  // Precommand modifiers are not shown.
  EXPECT_EQ(sh.run("command /usr/bin/true; builtin :").err, "+/usr/bin/true\n+:\n");
  // The trace goes to the stderr from before the command's redirections.
  EXPECT_EQ(sh.run("a=1 : 2>/dev/null").err, "+a=1 +:\n");
  // PS4 is expanded before the assignments, and again before the words.
  EXPECT_EQ(sh.run("PS4=@ :").err, "+PS4=@ @:\n");
  EXPECT_EQ(sh.run("PS4=x; PS4=+").err, "+PS4=x \nxPS4=+ \n");
  // A command of only redirections is not traced.
  EXPECT_EQ(sh.run("(exec 8>/dev/null; echo hi >/dev/null)").err, "+echo hi\n");
  // typeset's arguments are shown as assignments.
  EXPECT_EQ(sh.run("typeset w=2 v=(1 'a b') u='x y'").err,
            "+typeset w=2 v=( 1 'a b' ) u='x y'\n");
  // With PS4 unset there is no prefix.
  EXPECT_EQ(sh.run("unset PS4; true").err, "+unset PS4\ntrue\n");
}

// ---- M2 features ---------------------------------------------------------

TEST(Exec, PipelinePassesDataBetweenStages) {
  Shell sh;
  auto r = sh.run("echo hello | tr a-z A-Z");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "HELLO\n");
}

TEST(Exec, ThreeStagePipeline) {
  Shell sh;
  auto r = sh.run("echo abc | tr a-z A-Z | tr A-Z 0-9");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "012\n");
}

TEST(Exec, PipefailTakesTheRightmostFailure) {
  Shell sh;
  auto r = sh.run("false | true");
  EXPECT_EQ(r.rc, 0);  // without pipefail, last stage wins
  EXPECT_EQ(sh.run("(exit 3) | (exit 2) | true; echo $? $pipestatus").out, "0 3 2 0\n");
  sh.opts.set(OPT_PIPEFAIL);
  r = sh.run("false | true");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(sh.run("(exit 3) | (exit 2) | true; echo $? $pipestatus").out, "2 3 2 0\n");
}

TEST(Exec, PipelineStderrMerge) {
  Shell sh;
  // sh -c 'echo out; echo err 1>&2' writes to both; |& should capture both.
  auto r = sh.run("sh -c 'echo xx 1>&2' |& cat");
  EXPECT_EQ(r.rc, 0);
  EXPECT_NE(r.out.find("xx"), str::npos);
}

TEST(Exec, BackgroundJobReturnsImmediately) {
  Shell sh;
  auto r = sh.run("sleep 0.05 &");
  EXPECT_EQ(r.rc, 0);
  // $! should be set to a positive pid.
  EXPECT_GT(sh.ctx.last_bg_pid, 0);
  // Wait for the child to exit so it doesn't leak into the next test.
  int status = 0;
  waitpid(sh.ctx.last_bg_pid, &status, 0);
}

TEST(Exec, RedirectWriteCreatesFile) {
  Shell sh;
  TempDir dir;
  auto out = (dir.path / "o").string();
  auto r = sh.run("echo hi > " + out);
  EXPECT_EQ(r.rc, 0);
  s::ifstream f{out};
  str got((s::istreambuf_iterator<char>(f)),
          s::istreambuf_iterator<char>());
  EXPECT_EQ(got, "hi\n");
}

TEST(Exec, RedirectAppendPreservesFile) {
  Shell sh;
  TempDir dir;
  auto out = (dir.path / "o").string();
  sh.run("echo a > " + out);
  auto r = sh.run("echo b >> " + out);
  EXPECT_EQ(r.rc, 0);
  s::ifstream f{out};
  str got((s::istreambuf_iterator<char>(f)),
          s::istreambuf_iterator<char>());
  EXPECT_EQ(got, "a\nb\n");
}

TEST(Exec, RedirectReadDeliversFileAsStdin) {
  Shell sh;
  TempDir dir;
  auto in = (dir.path / "i").string();
  s::ofstream{in} << "hello\n";
  auto r = sh.run("cat < " + in);
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "hello\n");
}

TEST(Exec, HeredocDeliversBody) {
  Shell sh;
  auto r = sh.run("cat <<EOF\nhello\nworld\nEOF\n");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "hello\nworld\n");
}

TEST(Exec, HeredocStripTabsWithDashOperator) {
  Shell sh;
  auto r = sh.run("cat <<-END\n\tx\n\ty\n\tEND\n");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "x\ny\n");
}

TEST(Exec, HerestringDeliversStringPlusNewline) {
  Shell sh;
  auto r = sh.run("cat <<< hello");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "hello\n");
}

TEST(Exec, StderrRedirectToFile) {
  Shell sh;
  TempDir dir;
  auto errfile = (dir.path / "e").string();
  auto r = sh.run("sh -c 'echo err 1>&2' 2> " + errfile);
  EXPECT_EQ(r.rc, 0);
  s::ifstream f{errfile};
  str got((s::istreambuf_iterator<char>(f)),
          s::istreambuf_iterator<char>());
  EXPECT_EQ(got, "err\n");
}

TEST(Exec, MergeStderrIntoStdout) {
  Shell sh;
  auto r = sh.run("sh -c 'echo out; echo err 1>&2' 2>&1");
  EXPECT_EQ(r.rc, 0);
  // Both lines reach stdout; order is "out\nerr\n" for the sh above.
  EXPECT_NE(r.out.find("out"), str::npos);
  EXPECT_NE(r.out.find("err"), str::npos);
}

TEST(Exec, NoClobberRefusesExistingFile) {
  Shell sh;
  TempDir dir;
  auto path = (dir.path / "o").string();
  s::ofstream{path} << "orig";
  sh.opts.unset(OPT_CLOBBER);
  auto r = sh.run("echo new > " + path);
  EXPECT_NE(r.rc, 0);
  s::ifstream f{path};
  str got((s::istreambuf_iterator<char>(f)),
          s::istreambuf_iterator<char>());
  EXPECT_EQ(got, "orig");
}

TEST(Exec, ClobberOperatorOverridesNoClobber) {
  Shell sh;
  TempDir dir;
  auto path = (dir.path / "o").string();
  s::ofstream{path} << "orig";
  sh.opts.unset(OPT_CLOBBER);
  auto r = sh.run("echo new >| " + path);
  EXPECT_EQ(r.rc, 0);
  s::ifstream f{path};
  str got((s::istreambuf_iterator<char>(f)),
          s::istreambuf_iterator<char>());
  EXPECT_EQ(got, "new\n");
}

TEST(Exec, ProcessSubstitutionInput) {
  Shell sh;
  auto r = sh.run("cat <(echo via-procsub)");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "via-procsub\n");
}

TEST(Exec, WaitBuiltinCollectsBackgroundExit) {
  Shell sh;
  sh.run("true &");
  auto r = sh.run("wait");
  EXPECT_EQ(r.rc, 0);
}

// The tee is waited for: the next command reads both files whole.
TEST(Exec, MultiosTeesToBothTargets) {
  Shell sh;
  TempDir dir;
  str d = dir.path.string();
  auto r = sh.run("(cd " + d + " && for i in {1..100}; do "
                  "echo x$i >a >b; read v <a; read w <b; "
                  "[[ $v == x$i && $w == x$i ]] || ((bad++)); done; echo bad=$bad)");
  EXPECT_EQ(r.out, "bad=\n");
  // A builtin with a forked tee for its output, and a function.
  r = sh.run("(cd " + d + " && f() { echo in-f; }; f >c >d; cat c d)");
  EXPECT_EQ(r.out, "in-f\nin-f\n");
}

str out(Shell& sh, strv src) { return sh.run(src).out; }

// Runs `jfsh -c src`, whose own fds are its own (unlike this test
// process's), with stderr merged.
s::pair<str, int> run_jfsh(strv src) {
  str cmd = str(JFSH_BINARY) + " --norouter -f -c '" + str(src) + "' 2>&1 </dev/null";
  s::FILE* p = ::popen(cmd.c_str(), "r");
  if (not p) return {"", -1};
  str o;
  char buf[256];
  while (s::fgets(buf, sizeof buf, p)) o += buf;
  int st = ::pclose(p);
  return {o, WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st)};
}

// `<&-` closes stdin, `>&-` stdout, and `N>&-` fd N.
TEST(Exec, CloseOperatorsCloseTheirOwnFd) {
  Shell sh;
  auto r = sh.run("cat <&-; echo rc=$?");
  EXPECT_EQ(r.out, "rc=1\n");
  EXPECT_NE(r.err.find("stdin"), str::npos) << r.err;
  EXPECT_EQ(out(sh, "(exec <&-; echo still-here $?)"), "still-here 0\n");
  r = sh.run("(exec 3>&1; echo to3 >&3; exec 3>&-; echo x >&3; echo rc=$?)");
  EXPECT_EQ(r.out, "to3\nrc=1\n");
  EXPECT_EQ(r.err, "jfsh: 3: bad file descriptor\n");
  EXPECT_EQ(out(sh, "f() { cat <&-; echo 3<&- 2>&-; }; functions f"),
            "f () {\n\tcat <&-\n\techo 3<&- 2>&-\n}\n");
}

// Only what the parser saw as `<(…)`, `>(…)` or `=(…)` is a process
// substitution: not quoted text, nor a parameter's value.
TEST(Exec, ProcessSubstitutionOnlyFromTheParser) {
  Shell sh;
  TempDir dir;
  str d = dir.path.string();
  EXPECT_EQ(out(sh, "(cd " + d + " && x=\">(touch pwned)\"; echo $x; "
                    "echo \"<(echo hi)\" '=(x)'; ls)"),
            ">(touch pwned)\n<(echo hi) =(x)\n");
  EXPECT_EQ(out(sh, "cat <(echo a) =(echo b); echo <(true) | grep -c '^/dev/fd/[0-9]*$'"),
            "a\nb\n1\n");
}

// =(…) names its file after $TMPPREFIX and removes it after the command;
// <(…) and =(…) read nothing from the command's stdin.
TEST(Exec, ProcessSubstitutionFilesAndStdin) {
  Shell sh;
  TempDir dir;
  str d = dir.path.string();
  EXPECT_EQ(out(sh, "TMPPREFIX=" + d + "/zz; f=$(echo =(true)); "
                    "echo ${#f} $((${#f} - ${#TMPPREFIX})); "
                    "[[ $f == " + d + "/zz* && ! -e $f ]] && echo gone"),
            s::to_string(d.size() + 9) + " 6\ngone\n");
  EXPECT_EQ(out(sh, "echo x | cat <(cat); echo y | cat =(cat); echo end"), "end\n");
  // As the last stage of a pipeline, the file outlives the forked command.
  EXPECT_EQ(out(sh, "echo y | cat =(echo q)"), "q\n");
}

// As in zsh, the fd in front of a redirection is one digit; `12>f` is the
// argument 12.  `>&N` refuses the shell's own fds (above 9).
TEST(Exec, FdPrefixIsOneDigit) {
  Shell sh;
  TempDir dir;
  str d = dir.path.string();
  EXPECT_EQ(out(sh, "(cd " + d + " && echo y 12>f; cat f)"), "y 12\n");
  auto [o, rc] = run_jfsh("echo x >&11; echo rc=$?; echo y >&12; echo rc=$?; "
                          "/usr/bin/true; echo survived");
  EXPECT_EQ(o, "jfsh: 11: bad file descriptor\nrc=1\njfsh: 12: bad file descriptor\nrc=1\n"
               "survived\n");
  EXPECT_EQ(rc, 0);
  auto r = sh.run("echo c >&99999999999; echo rc=$?");
  EXPECT_EQ(r.err, "jfsh: 1215752191: bad file descriptor\n");
  EXPECT_EQ(r.out, "rc=1\n");
}

// The redirection errors read as zsh's; a target that expands to nothing
// fails the command, not the command line.
TEST(Exec, RedirectionErrors) {
  Shell sh;
  auto r = sh.run("cat </nonexistent; echo rc=$?; echo b >/nonexistent/dir/x; echo rc=$?");
  EXPECT_EQ(r.err, "jfsh: no such file or directory: /nonexistent\n"
                   "jfsh: no such file or directory: /nonexistent/dir/x\n");
  EXPECT_EQ(r.out, "rc=1\nrc=1\n");
  r = sh.run("echo x >$nothing; echo rc=$?; echo end");
  EXPECT_EQ(r.err, "jfsh: no such file or directory: \n");
  EXPECT_EQ(r.out, "rc=1\nend\n");
  r = sh.run("x=o; echo x <&$x; echo not-here");
  EXPECT_EQ(r.err, "jfsh: file number expected\n");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.rc, 1);
}

// NO_CLOBBER: `>` refuses an existing regular file but opens a device;
// `>>` creates nothing unless APPEND_CREATE; `>!`, `>|`, `>>!`, `>>|`,
// `&>|`, `>&|` force it.  `>& file` and `>>& file` are `&>` and `&>>`.
TEST(Exec, NoClobberAndItsOverrides) {
  Shell sh;
  TempDir dir;
  str d = dir.path.string();
  auto r = sh.run("(cd " + d + " && setopt noclobber; echo a >in; echo b >in; echo rc=$?; "
                  "echo c >/dev/null; echo rc=$?; echo d >>missing; echo rc=$?; "
                  "echo e >!in; echo f >>!in; echo g >>|in; cat in; echo h &>|in; echo i >>!new; "
                  "cat in new; echo j >&|in; echo k >& o2; echo l >>& o2; cat in o2; "
                  "setopt appendcreate; echo m >>made; cat made)");
  EXPECT_EQ(r.out, "rc=1\nrc=0\nrc=1\ne\nf\ng\nh\ni\nj\nk\nl\nm\n");
  EXPECT_EQ(r.err, "jfsh: file exists: in\njfsh: no such file or directory: missing\n");
  // Also for each target of a MULTIOS group.
  r = sh.run("(cd " + d + " && setopt noclobber; echo n >in >b2; echo rc=$?; cat in; ls b2)");
  EXPECT_EQ(r.out, "rc=1\nj\n");
  EXPECT_EQ(r.err, "jfsh: file exists: in\nls: b2: No such file or directory\n");
}

// The last stage of a pipeline runs in the shell, as in zsh.
TEST(Exec, LastPipelineStageRunsInTheShell) {
  Shell sh;
  EXPECT_EQ(out(sh, "echo x | read v; echo v=$v"), "v=x\n");
  EXPECT_EQ(out(sh, "printf 'a\\nb\\n' | while read l; do n=$((n+1)); done; echo n=$n"), "n=2\n");
  EXPECT_EQ(out(sh, "f() { g=1; cat; }; echo in | f; echo g=$g"), "in\ng=1\n");
  auto r = sh.run("echo y | exit 4; echo after");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.rc, 4);
  // An external last stage is waited for with the others.
  EXPECT_EQ(out(sh, "echo a | cat; echo b | (cat; exit 3); echo $? $pipestatus"), "a\nb\n3 0 3\n");
}

// The pipe is the stage's first stdout redirection, so under MULTIOS a
// `>file` adds to it, and `|&` is a `2>&1` after the stage's own.
TEST(Exec, PipeTakesPartInMultios) {
  Shell sh;
  TempDir dir;
  str d = dir.path.string();
  EXPECT_EQ(out(sh, "(cd " + d + " && echo y >c | cat; echo --; cat c)"), "y\n--\ny\n");
  EXPECT_EQ(out(sh, "{ echo e >&2 } 2>/dev/null |& sed s/^/P:/"), "P:e\n");
  EXPECT_EQ(out(sh, "{ echo o } >/dev/null |& sed s/^/Q:/"), "Q:o\n");
  auto r = sh.run("(cd " + d + " && echo m >f >&2; cat f)");
  EXPECT_EQ(r.out, "m\n");
  EXPECT_EQ(r.err, "m\n");
  sh.opts.unset(OPT_MULTIOS);
  EXPECT_EQ(out(sh, "(cd " + d + " && echo z >c | cat; echo --; cat c)"), "--\nz\n");
}

// Redirections are made in order: `2>&1` copies fd 1 as it is then.  A
// failed one stops the command, once.
TEST(Exec, RedirectionsApplyInOrder) {
  Shell sh;
  TempDir dir;
  str d = dir.path.string();
  EXPECT_EQ(out(sh, "(cd " + d + " && { echo out; echo err >&2 } >a 2>&1 >b; "
                    "sort a; echo --; cat b)"),
            "err\nout\n--\nout\n");
  auto r = sh.run("(cd " + d + " && echo r >g >/nonexist/x >h; echo rc=$?; cat g; ls h)");
  EXPECT_EQ(r.out, "rc=1\n");
  EXPECT_EQ(r.err, "jfsh: no such file or directory: /nonexist/x\n"
                   "ls: h: No such file or directory\n");
  EXPECT_EQ(out(sh, "(cd " + d + " && echo rw 1<>rwf; cat rwf; { echo g; echo h >&2 } &>both; "
                    "{ echo i } &>>both; cat both)"),
            "rw\ng\nh\ni\n");
}

// Pipelines work with the shell's own stdin or stdout closed.
TEST(Exec, PipelineWithAStdFdClosed) {
  Shell sh;
  EXPECT_EQ(out(sh, "(exec 0>&-; echo hi | cat)"), "hi\n");
  EXPECT_EQ(sh.run("(exec 1>&-; echo hi | cat >&2)").err, "hi\n");
}

// `a | b &` forks the stages from the shell: $! is the last one, and
// killing it ends the job.
TEST(Exec, BackgroundPipelineBangIsTheLastStage) {
  Shell sh;
  EXPECT_EQ(out(sh, "sleep 0.1 | (exit 3) & wait $!; echo $?"), "3\n");
  EXPECT_EQ(out(sh, "true | sleep 5 & p=$!; kill $p; wait $p; echo $?"), "143\n");
  // The pause lets the stage exec sleep before ps looks at it.
  EXPECT_EQ(out(sh, "sleep 5 | sleep 5 & sleep 0.3; ps -o comm= -p $! | grep -c sleep; "
                    "kill %1; wait"),
            "1\n");
}

// A command of nothing but redirections runs $NULLCMD, or $READNULLCMD
// for a lone `<file`; with a precommand modifier it is an error.
TEST(Exec, NullCommand) {
  Shell sh;
  TempDir dir;
  str d = dir.path.string();
  EXPECT_EQ(out(sh, "(cd " + d + " && echo data >in; <in; >out2 <in; cat out2; "
                    "echo hi | >f; cat f; "
                    "READNULLCMD=rev; <in)"),
            "data\ndata\nhi\natad\n");
  EXPECT_EQ(out(sh, "(cd " + d + " && NULLCMD=false; >g; echo rc=$?; x=1 >h; echo x=$x; ls g h)"),
            "rc=1\nx=1\ng\nh\n");
  auto r = sh.run("(cd " + d + " && command >f2; echo not-here)");
  EXPECT_EQ(r.err, "jfsh: redirection with no command\n");
  EXPECT_EQ(r.out, "");
  r = sh.run("(cd " + d + " && NULLCMD=; >f3; echo not-here)");
  EXPECT_EQ(r.err, "jfsh: redirection with no command\n");
  r = sh.run("(cd " + d + " && setopt shnullcmd xtrace; >f4 </dev/null; cat f4)");
  EXPECT_EQ(r.err, "+jfsh:1> :\n+jfsh:1> cat f4\n");
}

// A quoted here-document delimiter goes through quote removal.
TEST(Exec, HeredocDelimiterQuoteRemoval) {
  Shell sh;
  EXPECT_EQ(out(sh, "cat <<'a\"b'\nbody $x\na\"b\necho next"), "body $x\nnext\n");
  EXPECT_EQ(out(sh, "cat <<E\\\\OF\nb2 $x\nE\\OF\necho next"), "b2 $x\nnext\n");
  EXPECT_EQ(out(sh, "x=v; cat <<\"E\\\"Q\"\nlit $x\nE\"Q\ncat <<E\"\"F\nlit2 $x\nEF"),
            "lit $x\nlit2 $x\n");
}

// Here-documents in a pipeline, in a function, and longer than a pipe
// holds.
TEST(Exec, HeredocsInPipelinesAndFunctions) {
  Shell sh;
  EXPECT_EQ(out(sh, "f() { cat <<E\nin func $1\nE\n}; f arg | tr a-z A-Z"), "IN FUNC ARG\n");
  EXPECT_EQ(out(sh, "cat <<E | tr a-z A-Z\npiped\nE"), "PIPED\n");
  str big(200000, 'x');
  EXPECT_EQ(out(sh, "cat <<E | wc -c | tr -d ' '\n" + big + "\nE"), "200001\n");
  EXPECT_EQ(out(sh, "read -r v <<< \"$(printf '%.0sy' {1..70000})\"; echo ${#v}"), "70000\n");
}

// A process substitution inside a word, in an assignment, a `for` list
// or an array runs in place; its fd lasts as long as the command.
TEST(Exec, ProcessSubstitutionInsideWords) {
  Shell sh;
  auto masked = [&](strv src) {
    str o = out(sh, src);
    str r;
    for (s::size_t i = 0; i < o.size(); ++i) {
      if (o.compare(i, 8, "/dev/fd/") == 0) {
        r += "/dev/fd/N";
        i += 8;
        while (i < o.size() and s::isdigit(static_cast<unsigned char>(o[i]))) ++i;
        --i;
      } else {
        r += o[i];
      }
    }
    return r;
  };
  EXPECT_EQ(masked("echo --f=<(true); echo <(echo a)x; echo a<(true)b; x=w; echo $x<(true)"),
            "--f=/dev/fd/N\n/dev/fd/Nx\na/dev/fd/Nb\nw/dev/fd/N\n");
  EXPECT_EQ(masked("x=<(echo hi); echo $x; typeset y=<(true); echo $y; z=e=<(true); echo $z; "
                   "f() { local q=<(echo L); echo $q; }; f"),
            "/dev/fd/N\n/dev/fd/N\ne=/dev/fd/N\n/dev/fd/N\n");
  EXPECT_EQ(out(sh, "for f in <(echo a) <(echo b); do cat $f; done; "
                    "foreach g (<(echo c)) cat $g; end"),
            "a\nb\nc\n");
  EXPECT_EQ(masked("arr=(x <(echo c) y); echo ${#arr} $arr[2]"), "3 /dev/fd/N\n");
  // Quoted or expanded, it is text.
  EXPECT_EQ(out(sh, "x='<(echo hi)'; echo $x \"a<(b)\" \\<\\(c\\) ${v:-'<(d)'}"),
            "<(echo hi) a<(b) <(c) <(d)\n");
  EXPECT_EQ(out(sh, "f() { cat 3< <(echo q) > >(cat) }; functions f"),
            "f () {\n\tcat 3< <(echo q) > >(cat)\n}\n");
}

// A process substitution's command ends at its own `)`, past quotes and
// nested substitutions; it works as the `case` subject, a `<<<` target and
// `< =(…)`.
TEST(Exec, ProcessSubstitutionEndsAtItsOwnParen) {
  Shell sh;
  EXPECT_EQ(out(sh, "cat <(echo \")\"); cat < <(echo \")\"); cat <(echo ')'; echo second); "
                    "cat <(echo \"(\")"),
            ")\n)\n)\nsecond\n(\n");
  EXPECT_EQ(out(sh, "echo a<(echo ')')b | sed 's/[0-9]//g'; cat <(echo $(echo \")\"))"),
            "a/dev/fd/b\n)\n");
  EXPECT_EQ(out(sh, "case <(true) in /dev/fd/*) echo fd;; esac; "
                    "cat <<<<(echo q) | sed 's/[0-9]//g'"),
            "fd\n/dev/fd/\n");
  EXPECT_EQ(out(sh, "f() { cat < =(echo hi) }; functions f; f"),
            "f () {\n\tcat < =(echo hi)\n}\nhi\n");
}

// A trap runs while `read` blocks, in a subshell too (where the shell's
// signal self-pipe is closed).
TEST(Exec, TrapRunsWhileReadBlocksInASubshell) {
  auto [o, rc] = run_jfsh("zmodload zsh/datetime; s=$EPOCHREALTIME; "
                          "( trap \"echo trap \\$(( EPOCHREALTIME - s < 1 )); exit 0\" USR1; "
                          "sh -c \"sleep 0.3; kill -USR1 \\$PPID\" & "
                          "read y < <(sleep 2; echo late); echo \"got $y\" ); echo end");
  EXPECT_EQ(o, "trap 1\nend\n");
  EXPECT_EQ(rc, 0);
}

// A CHLD trap runs while `read` blocks; a trap ends `read -t` as a
// timeout; a trapped signal makes a blocked fifo open fail, quietly.
TEST(Exec, TrapsInterruptReadAndFifoOpens) {
  auto [o, rc] = run_jfsh("zmodload zsh/datetime; s=$EPOCHREALTIME; "
                          "trap \"echo chld \\$((EPOCHREALTIME-s<0.5))\" CHLD; sleep 0.2 & "
                          "read x < <(sleep 1; echo hi); echo $x");
  EXPECT_EQ(o, "chld 1\nhi\n");
  auto [o2, rc2] = run_jfsh("trap \"echo T\" USR1; /bin/sh -c \"sleep 0.3; kill -USR1 \\$PPID\" & "
                            "read -t 2 y < <(sleep 1; echo data); echo A st=$? y=$y");
  EXPECT_EQ(o2, "T\nA st=1 y=\n");
  TempDir dir;
  str f = (dir.path / "ff").string();
  auto [o3, rc3] = run_jfsh("trap \"echo usr1\" USR1; me=$$; mkfifo " + f + "; "
                            "( sleep 0.3; kill -USR1 $me ) & read y < " + f
                            + "; echo st=$? y=$y; wait");
  EXPECT_EQ(o3, "usr1\nst=1 y=\n");
}

// Opening a fifo waits for its other end; a job that ends meanwhile
// doesn't make the open fail.
TEST(Exec, FifoOpenSurvivesAJobEnding) {
  Shell sh;
  TempDir dir;
  str d = dir.path.string();
  EXPECT_EQ(out(sh, "(cd " + d + " || exit; mkfifo f; sleep 0.1 & (sleep 0.4; cat f) & "
                    "echo hi > f; echo rc=$?; wait)"),
            "hi\nrc=0\n");
  EXPECT_EQ(out(sh, "(cd " + d + " || exit; sleep 0.1 & (sleep 0.4; echo w > f) & "
                    "while read l; do echo \"<$l>\"; done < f; wait)"),
            "<w>\n");
  EXPECT_EQ(out(sh, "(cd " + d + " || exit; sleep 0.1 & (sleep 0.4; echo w > f) & "
                    "exec 3< f; read l <&3; echo \"<$l>\"; wait)"),
            "<w>\n");
}

// A `$'…'` with `\'` inside a substitution doesn't end it early; an
// unfinished `<(` is a parse error.
TEST(Exec, ProcessSubstitutionBodiesAndErrors) {
  Shell sh;
  EXPECT_EQ(out(sh, "cat <(echo $'a\\')b'); echo $(echo $'c\\')d'); echo \"$(echo $'x\\'')\""),
            "a')b\nc')d\nx'\n");
  auto r = sh.run("cat <(echo hi");
  EXPECT_EQ(r.err, "jfsh:1: parse error near `<(echo hi'\n");
  EXPECT_EQ(r.rc, 1);
}

// `&|` and `&!` run in the background without making a job.
TEST(Exec, DisownedBackgroundList) {
  Shell sh;
  EXPECT_EQ(out(sh, "true &| echo y"), "y\n");
  auto r = sh.run("(exit 3) &! p=$!; [[ $p == <-> ]] && echo pid; jobs; wait $p; echo w=$?");
  EXPECT_EQ(r.out, "pid\nw=127\n");
  EXPECT_NE(r.err.find("is not a child of this shell"), str::npos) << r.err;
  EXPECT_EQ(out(sh, "true | true &|; jobs; echo end"), "end\n");
  EXPECT_EQ(out(sh, "f() { sleep 1 &|; a | b &! }; functions f"),
            "f () {\n\tsleep 1 &|\n\ta | b &|\n}\n");
}

// `exit` as the last stage leaves at once, without waiting for the
// other stages.
TEST(Exec, ExitInTheLastStageDoesNotWait) {
  Shell sh;
  TempDir dir;
  str f = (dir.path / "f").string();
  EXPECT_EQ(out(sh, "( (sleep 0.5; echo x >" + f + ") | exit 4 ); echo $?; [[ -e " + f
                    + " ]] || echo not-waited"),
            "4\nnot-waited\n");
}

// `>&N` refuses only the shell's own fds: a user's close-on-exec fd above 9
// is fine.
TEST(Exec, DupOfAUsersCloexecFd) {
  Shell sh;
  TempDir dir;
  str d = dir.path.string();
  EXPECT_EQ(out(sh, "(cd " + d + " && zmodload zsh/system; for i in {1..9}; do "
                    "sysopen -w -o create,cloexec -u g out$i; done; "
                    "echo hi >&$g; echo rc=$?; cat out9)"),
            "rc=0\nhi\n");
}

// `wait %N` gives the last process's status, PIPEFAIL or not, as zsh's.
TEST(Exec, WaitForAPipelineJob) {
  Shell sh;
  EXPECT_EQ(out(sh, "setopt pipefail; (exit 3) | true & wait %1; echo $?; "
                    "true | (exit 4) & wait %1; echo $?"),
            "0\n4\n");
}

TEST(Exec, ParseErrorReportsTheLineAndRunsNothing) {
  Shell sh;
  auto r = sh.run("echo a\necho )");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh:2: parse error near `)'\n");
}

// ---- M15c: shell features compsys needs ------------------------------------------

// {fd}>file opens a new fd and names it.
TEST(Exec15c, VaridRedirection) {
  Shell sh;
  EXPECT_EQ(out(sh, "exec {fd}>/dev/null; (( fd >= 10 )) && echo ok; exec {fd}>&-"),
            "ok\n");
}

// >&p fails as zsh's does with no coprocess.
TEST(Exec15c, CoprocessFdWithoutCoprocess) {
  Shell sh;
  EXPECT_EQ(out(sh, "(:>&p) 2>/dev/null || echo no; echo done"),
            "no\ndone\n");
}

// $funcstack names the calls; $signals the signals.
TEST(Exec15c, FuncstackAndSignals) {
  Shell sh;
  EXPECT_EQ(out(sh, "f() { g; }; g() { echo $funcstack; }; f; "
                "echo $signals[3] $signals[10]"),
            "g f\nINT KILL\n");
}

// UID, USERNAME, ZSH_VERSION.
TEST(Exec15c, UserAndShellSpecials) {
  Shell sh;
  EXPECT_EQ(out(sh, "(( UID >= 0 )) && echo uid; [[ -n $USERNAME ]] && echo user; "
                "echo $ZSH_VERSION"),
            "uid\nuser\n5.9\n");
}

// $parameters gives types, the expander's specials too.
TEST(Exec15c, ParametersHash) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=1; typeset -a a; export Y=1; "
                "echo $parameters[x] $parameters[a] $parameters[Y] $parameters[0] "
                "$parameters[ARGC]"),
            "scalar array scalar-export scalar-special integer-readonly-special\n");
}

// An autoload function not loaded yet reads as zsh shows it.
TEST(Exec15c, FunctionsHashAutoload) {
  Shell sh;
  EXPECT_EQ(out(sh, "autoload -Uz foo; "
                "echo \"$functions[foo]\" ${+functions[foo]} ${+functions[nope]}"),
            "builtin autoload -XU 1 0\n");
}

// set - is a no-op on options in zsh mode.
TEST(Exec15c, SetMinusOnlyOutsideZsh) {
  Shell sh;
  EXPECT_EQ(out(sh, "set -x; set -; [[ -o xtrace ]] && echo still; set +x"),
            "still\n");
}

// Patterns are weighed by their parts, not their order.
TEST(Exec15c, ZstyleMostSpecificPatternWins) {
  Shell sh;
  EXPECT_EQ(out(sh, "zstyle ':completion:*' menu a; "
                "zstyle ':completion:*:*:ls:*' menu b; "
                "zstyle -s ':completion:x:y:ls:z' menu v; echo $v; "
                "zstyle -s ':completion:x:y:cp:z' menu v; echo $v"),
            "b\na\n");
}

// -e evaluates the value, which sets reply.
TEST(Exec15c, ZstyleEvalStyle) {
  Shell sh;
  EXPECT_EQ(out(sh, "zstyle -e ':t:*' st 'reply=(computed $#)'; zstyle -a ':t:x' st r; "
                "echo $r"),
            "computed 0\n");
}

// -t, -T and -L.
TEST(Exec15c, ZstyleTestsAndLists) {
  Shell sh;
  EXPECT_EQ(out(sh, "zstyle ':a' b yes; zstyle -t ':a' b && echo t; "
                "zstyle -T ':a' nope && echo T; zstyle -t ':a' b no || echo f; "
                "zstyle -L ':a'"),
            "t\nT\nf\nzstyle :a b yes\n");
}

// -g lists the patterns, or a pattern's styles.
TEST(Exec15c, ZstyleGetPatternsAndStyles) {
  Shell sh;
  EXPECT_EQ(out(sh, "zstyle -d; zstyle ':p1' s1 v; zstyle ':p1' s2 w; zstyle -g out; echo $out; "
                "zstyle -g out ':p1'; echo ${(o)out}; zstyle -g out ':p1' s2; "
                "echo $out"),
            ":p1\ns1 s2\nw\n");
}

// -D removes the options, -E keeps going past unknown ones.
TEST(Exec15c, ZparseoptsArraysAndRemoval) {
  Shell sh;
  EXPECT_EQ(out(sh, "set -- -a -b val -x rest; zparseopts -D -E a=o1 b:=o2; "
                "echo $o1 \"|\" $o2 \"|\" $@"),
            "-a | -b val | -x rest\n");
}

TEST(Exec15c, ZparseoptsAssociation) {
  Shell sh;
  EXPECT_EQ(out(sh, "zparseopts -A h -- a: b; echo ${(kv)h}"),
            "\n");
}

// %(x.yes.no) tests a spec; %-5x pads.
TEST(Exec15c, ZformatTernaryAndPadding) {
  Shell sh;
  EXPECT_EQ(out(sh, "zformat -f out '%(n.has:%n.none) [%-5n] [%5n]' n:ab; echo $out; "
                "zformat -f out '%(z.y.n)' z:; echo $out"),
            "has:ab [   ab] [ab   ]\ny\n");
}

// -a lines up descriptions.
TEST(Exec15c, ZformatAlign) {
  Shell sh;
  EXPECT_EQ(out(sh, "zformat -a out ' -- ' a:first longer:second; print -l $out"),
            "a      -- first\nlonger -- second\n");
}

// $LOGNAME is set, and exported, when the environment has none, as zsh
// does; compaudit needs it.
TEST(Exec15c, LognameDefaultsToTheUser) {
  Shell sh;
  EXPECT_EQ(out(sh, "[[ -n $LOGNAME ]] && echo set; echo ${(t)LOGNAME}"), "set\nscalar-export\n");
}

}  // namespace
}  // namespace jfsh
