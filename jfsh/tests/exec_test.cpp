#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <jfsh/core/jobs.hh>
#include <jfsh/core/options.hh>
#include <jfsh/core/param.hh>
#include <jfsh/core/signals.hh>
#include <jfsh/exec/executor.hh>

namespace jfsh {
namespace {

namespace fs = std::filesystem;

struct Run {
  int         rc;
  std::string out;
  std::string err;
};

std::string slurp(std::FILE* f) {
  std::string s;
  std::rewind(f);
  char buf[4096];
  std::size_t n;
  while ((n = std::fread(buf, 1, sizeof buf, f)) > 0) s.append(buf, n);
  std::fclose(f);
  return s;
}

// A fresh shell state with $PATH exported from the test's environment.
struct Shell {
  ParamTable  params;
  OptionState opts;
  JobTable    jobs;
  ExecContext ctx{params, opts, jobs, Signals};
  Executor    exec{ctx};

  Shell() {
    const char* path = std::getenv("PATH");
    params.assign("PATH", std::string(path ? path : "/usr/bin:/bin"),
                  PM_SCALAR | PM_EXPORTED);
  }

  // Runs src with fds 1 and 2 captured.  Child processes inherit the
  // redirected fds, so their output is captured too.
  Run run(std::string_view src) {
    std::fflush(nullptr);
    std::FILE* out = std::tmpfile();
    std::FILE* err = std::tmpfile();
    int saved_out = dup(STDOUT_FILENO);
    int saved_err = dup(STDERR_FILENO);
    dup2(fileno(out), STDOUT_FILENO);
    dup2(fileno(err), STDERR_FILENO);
    int rc = exec.exec_string(src);
    std::fflush(nullptr);
    dup2(saved_out, STDOUT_FILENO);
    dup2(saved_err, STDERR_FILENO);
    close(saved_out);
    close(saved_err);
    return {rc, slurp(out), slurp(err)};
  }

  std::string get(std::string_view name) {
    auto p = params.lookup(name);
    return (p and p->get().is_set()) ? p->get().scalar() : "<unset>";
  }
};

// A scratch directory removed when the test ends.
struct TempDir {
  fs::path path;

  TempDir() {
    std::string tmpl = (fs::temp_directory_path() / "jfsh_test_XXXXXX").string();
    path = mkdtemp(tmpl.data());
  }
  ~TempDir() { fs::remove_all(path); }

  fs::path file(std::string_view name, std::string_view body, fs::perms mode) {
    fs::path p = path / name;
    std::ofstream{p} << body;
    fs::permissions(p, mode);
    return p;
  }
};

constexpr fs::perms kExec = fs::perms::owner_all;
constexpr fs::perms kNoExec = fs::perms::owner_read | fs::perms::owner_write;

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
  sh.opts.assign(OPT_STDIN, true);  // interactive
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
  sh.params.assign("G", std::string("old"), PM_SCALAR);
  sh.params.push_scope();
  sh.run("G=new; L=created");
  sh.params.pop_scope();
  EXPECT_EQ(sh.get("G"), "new");
  EXPECT_EQ(sh.get("L"), "created");
}

TEST(Exec, ReadonlyParameterRejectsAssignment) {
  Shell sh;
  sh.params.assign("R", std::string("fixed"), PM_SCALAR | PM_READONLY);
  auto r = sh.run("R=changed");
  EXPECT_EQ(r.rc, 1);
  EXPECT_NE(r.err.find("read-only variable: R"), std::string::npos);
  EXPECT_EQ(sh.get("R"), "fixed");

  r = sh.run("R=changed echo ran");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.out, "");
}

TEST(Exec, PrefixAssignmentGoesOnlyToTheChildsEnvironment) {
  Shell sh;
  auto r = sh.run("V=tmp env");
  EXPECT_NE(("\n" + r.out).find("\nV=tmp\n"), std::string::npos);
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
  sh.params.assign("E", std::string("1"), PM_SCALAR | PM_EXPORTED);
  auto r = sh.run("E=2; U=unexported; env");
  EXPECT_NE(("\n" + r.out).find("\nE=2\n"), std::string::npos);
  EXPECT_EQ(("\n" + r.out).find("\nU="), std::string::npos);
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
  EXPECT_NE(r.err.find("no such file or directory"), std::string::npos);
}

TEST(Exec, NotExecutableIs126) {
  Shell sh;
  TempDir dir;
  auto file = dir.file("data", "x\n", kNoExec);
  auto r = sh.run(file.string());
  EXPECT_EQ(r.rc, 126);
  EXPECT_NE(r.err.find("permission denied"), std::string::npos);

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
  auto script = dir.file("plain", "echo from-sh \"$1\"\n", kExec);
  EXPECT_EQ(sh.run(script.string() + " arg").out, "from-sh arg\n");
}

TEST(Exec, BuiltinOutputComesBeforeTheNextChilds) {
  Shell sh;
  EXPECT_EQ(sh.run("echo first; sh -c 'echo second'; echo third").out,
            "first\nsecond\nthird\n");
}

TEST(Exec, PathChangesInvalidateTheCommandHash) {
  Shell sh;
  TempDir one, two;
  auto in_one = one.file("jfshcmd", "#!/bin/sh\n", kExec);
  auto in_two = two.file("jfshcmd", "#!/bin/sh\n", kExec);

  sh.params.set_scalar("PATH", one.path.string());
  EXPECT_EQ(sh.exec.find_command("jfshcmd"), in_one.string());
  EXPECT_EQ(sh.exec.find_command("jfshcmd"), in_one.string());  // hashed

  sh.params.set_scalar("PATH", two.path.string() + ":" + one.path.string());
  EXPECT_EQ(sh.exec.find_command("jfshcmd"), in_two.string());
}

TEST(Exec, EmptyPathComponentMeansCurrentDirectory) {
  Shell sh;
  TempDir dir;
  dir.file("jfshcmd", "#!/bin/sh\n", kExec);
  auto cwd = fs::current_path();
  fs::current_path(dir.path);
  sh.params.set_scalar("PATH", "/nonexistent:");
  auto found = sh.exec.find_command("jfshcmd");
  fs::current_path(cwd);
  EXPECT_EQ(found, "./jfshcmd");
}

// ---- xtrace -----------------------------------------------------------------

// The trace is printed before the command runs (bug B33), with quoting.
TEST(Exec, XtracePrintsPs4AndTheExpandedCommandFirst) {
  Shell sh;
  sh.opts.assign(OPT_XTRACE, true);
  auto r = sh.run("X='a b' echo hi 'c d'");
  EXPECT_EQ(r.err, "+ X='a b' echo hi 'c d'\n");
  EXPECT_EQ(r.out, "hi c d\n");

  sh.params.set_scalar("PS4", "trace> ");
  EXPECT_EQ(sh.run("echo it''s").err, "trace> echo its\n");
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
  EXPECT_NE(r.out.find("xx"), std::string::npos);
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
  std::ifstream f{out};
  std::string got((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
  EXPECT_EQ(got, "hi\n");
}

TEST(Exec, RedirectAppendPreservesFile) {
  Shell sh;
  TempDir dir;
  auto out = (dir.path / "o").string();
  sh.run("echo a > " + out);
  auto r = sh.run("echo b >> " + out);
  EXPECT_EQ(r.rc, 0);
  std::ifstream f{out};
  std::string got((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
  EXPECT_EQ(got, "a\nb\n");
}

TEST(Exec, RedirectReadDeliversFileAsStdin) {
  Shell sh;
  TempDir dir;
  auto in = (dir.path / "i").string();
  std::ofstream{in} << "hello\n";
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
  std::ifstream f{errfile};
  std::string got((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
  EXPECT_EQ(got, "err\n");
}

TEST(Exec, MergeStderrIntoStdout) {
  Shell sh;
  auto r = sh.run("sh -c 'echo out; echo err 1>&2' 2>&1");
  EXPECT_EQ(r.rc, 0);
  // Both lines reach stdout; order is "out\nerr\n" for the sh above.
  EXPECT_NE(r.out.find("out"), std::string::npos);
  EXPECT_NE(r.out.find("err"), std::string::npos);
}

TEST(Exec, NoClobberRefusesExistingFile) {
  Shell sh;
  TempDir dir;
  auto path = (dir.path / "o").string();
  std::ofstream{path} << "orig";
  sh.opts.unset(OPT_CLOBBER);
  auto r = sh.run("echo new > " + path);
  EXPECT_NE(r.rc, 0);
  std::ifstream f{path};
  std::string got((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
  EXPECT_EQ(got, "orig");
}

TEST(Exec, ClobberOperatorOverridesNoClobber) {
  Shell sh;
  TempDir dir;
  auto path = (dir.path / "o").string();
  std::ofstream{path} << "orig";
  sh.opts.unset(OPT_CLOBBER);
  auto r = sh.run("echo new >| " + path);
  EXPECT_EQ(r.rc, 0);
  std::ifstream f{path};
  std::string got((std::istreambuf_iterator<char>(f)),
                  std::istreambuf_iterator<char>());
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
    std::ifstream fa{a}, fb{b};
    std::string va((std::istreambuf_iterator<char>(fa)),
                   std::istreambuf_iterator<char>());
    std::string vb((std::istreambuf_iterator<char>(fb)),
                   std::istreambuf_iterator<char>());
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
