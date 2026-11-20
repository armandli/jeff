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
}

TEST(Exec, PipefailReflectsFirstFailingStage) {
  Shell sh;
  auto r = sh.run("false | true");
  EXPECT_EQ(r.rc, 0);  // without pipefail, last stage wins
  sh.opts.set(OPT_PIPEFAIL);
  r = sh.run("false | true");
  EXPECT_EQ(r.rc, 1);
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

TEST(Exec, MultiosTeesToBothTargets) {
  Shell sh;
  TempDir dir;
  sh.opts.set(OPT_MULTIOS);
  auto a = (dir.path / "a").string();
  auto b = (dir.path / "b").string();
  auto r = sh.run("echo x > " + a + " > " + b);
  EXPECT_EQ(r.rc, 0);
  // The tee helper runs asynchronously; give it a moment to flush and
  // exit so both target files have their data on disk.
  for (int i = 0; i < 50; ++i) {
    s::ifstream fa{a}, fb{b};
    str va((s::istreambuf_iterator<char>(fa)),
           s::istreambuf_iterator<char>());
    str vb((s::istreambuf_iterator<char>(fb)),
           s::istreambuf_iterator<char>());
    if (va == "x\n" and vb == "x\n") return;
    usleep(10'000);
  }
  FAIL() << "multios did not deliver x\\n to both targets";
}

TEST(Exec, ParseErrorReportsTheLineAndRunsNothing) {
  Shell sh;
  auto r = sh.run("echo a\necho )");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh:2: parse error near `)'\n");
}

}  // namespace
}  // namespace jfsh
