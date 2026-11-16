// Job control, signals and traps (M6) without a terminal: the job table,
// job specs and status lines, the one-line job text, the trap table and
// its function frames, and the trap, kill, wait, jobs, disown, fg and bg
// builtins run through Executor::exec_string.  Expected output is what
// zsh 5.9 (`zsh -f -c`) prints.  Interactive job control is in
// interactive_test.cpp.

#include <csignal>
#include <cstdio>
#include <string>
#include <string_view>
#include <sys/wait.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include <jfsh/core/jobs.h>
#include <jfsh/core/signals.h>
#include <jfsh/exec/executor.h>
#include <jfsh/lexer/input.h>
#include <jfsh/lexer/lexer.h>
#include <jfsh/parser/parser.h>

#include "shell_fixture.h"

namespace jfsh {
namespace {

using test::Shell;

std::string out(Shell& sh, std::string_view src) { return sh.run(src).out; }

// The wait status of a child that exits with `code`, or (code < 0) is
// killed by signal -code, or (stop) stops with signal `code`.
int child_status(int code, bool stop = false) {
  pid_t pid = ::fork();
  if (pid == 0) {
    if (stop) { ::raise(code); _exit(0); }
    if (code < 0) { ::signal(-code, SIG_DFL); ::raise(-code); }
    _exit(code < 0 ? 0 : code);
  }
  int status = 0;
  ::waitpid(pid, &status, stop ? WUNTRACED : 0);
  if (stop) {
    ::kill(pid, SIGKILL);
    int ignored;
    ::waitpid(pid, &ignored, 0);
  }
  return status;
}

ProgRef compile(std::string src) {
  StringInputSource in{std::move(src)};
  Lexer lex{in};
  Parser parser{lex};
  auto prog = parser.parse();
  EXPECT_TRUE(prog.has_value());
  return prog ? *prog : nullptr;
}

// ---- JobTable ----------------------------------------------------------------

TEST(JobTable, CurrentAndPreviousFollowTheNewestJobs) {
  JobTable jobs;
  int a = jobs.alloc(100, JOB_ASYNC, "sleep 31");
  int b = jobs.alloc(200, JOB_ASYNC, "sleep 32 | cat");
  EXPECT_EQ(a, 1);
  EXPECT_EQ(b, 2);
  EXPECT_EQ(jobs.current(), 2);
  EXPECT_EQ(jobs.previous(), 1);
  // B41: removing the current job promotes the previous one.
  jobs.remove(b);
  EXPECT_EQ(jobs.current(), 1);
  EXPECT_EQ(jobs.previous(), 0);
  EXPECT_EQ(jobs.alloc(300, JOB_ASYNC, "x"), 2);
}

TEST(JobTable, AForegroundJobOnlyBecomesCurrentWhenItStops) {
  JobTable jobs;
  int bg = jobs.alloc(100, JOB_ASYNC, "sleep 1");
  int fg = jobs.alloc(200, 0, "vi", false);
  jobs.add_process(fg, Process{.pid = 200});
  EXPECT_EQ(jobs.current(), bg);
  EXPECT_FALSE(jobs.find("%2").has_value());   // not one the user can name
  EXPECT_TRUE(jobs.update(200, child_status(SIGTSTP, true)));
  EXPECT_EQ(jobs.current(), fg);
  EXPECT_EQ(jobs.previous(), bg);
  EXPECT_EQ(jobs.get(fg)->get().status_text(), "suspended");
}

TEST(JobTable, JobSpecs) {
  JobTable jobs;
  jobs.alloc(100, JOB_ASYNC, "sleep 31");
  jobs.alloc(200, JOB_ASYNC, "sleep 32 | cat");
  EXPECT_EQ(jobs.find("%%"), 2);
  EXPECT_EQ(jobs.find("%+"), 2);
  EXPECT_EQ(jobs.find("%"), 2);
  EXPECT_EQ(jobs.find("%-"), 1);
  EXPECT_EQ(jobs.find("%1"), 1);
  EXPECT_EQ(jobs.find("%sl"), 2);        // the newest match
  EXPECT_EQ(jobs.find("%?31"), 1);
  EXPECT_EQ(jobs.find("%?cat"), 2);
  EXPECT_EQ(jobs.find("%9").error().message, "%9: no such job");
  EXPECT_EQ(jobs.find("%zz").error().message, "job not found: zz");
  JobTable none;
  EXPECT_EQ(none.find("%%").error().message, "no current job");
  EXPECT_EQ(none.find("%-").error().message, "no previous job");
}

TEST(JobTable, StatusLinesAsZshPrintsThem) {
  JobTable jobs;
  auto job_with = [&](int status) {
    int id = jobs.alloc(static_cast<Pid>(1000 + jobs.ids().size()), JOB_ASYNC, "cmd");
    Pid pid = jobs.get(id)->get().pgid;
    jobs.add_process(id, Process{.pid = pid});
    if (status >= 0) jobs.update(pid, status);
    return jobs.format(jobs.get(id)->get());
  };
  EXPECT_EQ(job_with(-1), "[1]  + running    cmd");
  EXPECT_EQ(job_with(child_status(0)), "[2]  + done       cmd");
  EXPECT_EQ(job_with(child_status(1)), "[3]  + exit 1     cmd");
  EXPECT_EQ(job_with(child_status(-SIGTERM)), "[4]  + terminated  cmd");
  EXPECT_EQ(job_with(child_status(-SIGKILL)), "[5]  + killed     cmd");
  EXPECT_EQ(job_with(child_status(SIGSTOP, true)), "[6]  + suspended (signal)  cmd");
  EXPECT_EQ(job_with(child_status(SIGTTIN, true)), "[7]  + suspended (tty input)  cmd");
}

TEST(JobTable, LongFormatListsEachProcess) {
  JobTable jobs;
  int id = jobs.alloc(7703, JOB_ASYNC, "sleep 31 | cat");
  jobs.add_process(id, Process{.pid = 7703, .text = "sleep 31"});
  jobs.add_process(id, Process{.pid = 7704, .text = "cat"});
  const Job& j = jobs.get(id)->get();
  EXPECT_EQ(jobs.format(j, true),
            "[1]  + 7703 running    sleep 31 | \n"
            "       7704 running    cat");
  EXPECT_EQ(jobs.format(j, false, true),
            "[1]  + 7703 running    sleep 31 | \n"
            "            running    cat");
  EXPECT_EQ(jobs.format_status(j, "continued"), "[1]  + continued  sleep 31 | cat");
}

TEST(JobTable, NotifyReportsOnceAndDropsFinishedJobs) {
  JobTable jobs;
  int id = jobs.alloc(100, JOB_ASYNC, "sleep 0.3");
  jobs.add_process(id, Process{.pid = 100});
  jobs.update(100, child_status(0));
  EXPECT_TRUE(jobs.has_changes());
  std::FILE* f = std::tmpfile();
  EXPECT_EQ(jobs.notify_changes(f), 1);
  EXPECT_EQ(test::slurp(f), "[1]  + done       sleep 0.3\n");
  EXPECT_FALSE(jobs.get(id).has_value());
  // `wait $!` still finds the status afterwards, once.
  EXPECT_EQ(jobs.take_remembered(100), 0);
  EXPECT_FALSE(jobs.take_remembered(100).has_value());
}

TEST(JobTable, PipefailStatus) {
  Job j;
  j.procs = {Process{.exit_code = 3}, Process{.exit_code = 0}};
  EXPECT_EQ(j.exit_status(), 0);
  EXPECT_EQ(j.exit_status(true), 3);
}

// ---- Job text ----------------------------------------------------------------

std::string text_of(std::string src) {
  auto prog = compile(std::move(src));
  // list → sublist → pipe: the first pipeline is at 2.
  return prog ? pipeline_text(*prog, 2) : "";
}

TEST(JobText, PipelinesAndSimpleCommandsAsWritten) {
  EXPECT_EQ(text_of("sleep 30 | cat"), "sleep 30 | cat");
  EXPECT_EQ(text_of("a |& b"), "a |& b");
  EXPECT_EQ(text_of("x=1 echo \"$y\" >out 2>&1"), "x=1 echo \"$y\" > out 2>&1");
  EXPECT_EQ(text_of("cat <(ls) <<< $w"), "cat <(ls) <<< $w");
  EXPECT_EQ(text_of("typeset -i n=3"), "typeset -i n=3");
}

TEST(JobText, CompoundCommandsOnOneLine) {
  EXPECT_EQ(text_of("( sleep 0.4; kill -USR1 $$ )"), "( sleep 0.4; kill -USR1 $$ )");
  EXPECT_EQ(text_of("{ a; b & }"), "{ a; b & }");
  EXPECT_EQ(text_of("while true; do sleep 1; done"), "while true; do sleep 1; done");
  EXPECT_EQ(text_of("until a\nb; do c; done"), "until a; b; do c; done");
  EXPECT_EQ(text_of("if a; then b; elif c; then d; else e; fi"),
            "if a; then b; elif c; then d; else e; fi");
  EXPECT_EQ(text_of("for x in a b; do echo $x; done"), "for x in a b; do echo $x; done");
  EXPECT_EQ(text_of("case $x in a|b) echo y;; c) ;& esac"),
            "case $x in (a|b) echo y ;; (c)  ;& esac");
  EXPECT_EQ(text_of("[[ -f x && $a == b* ]]"), "[[ -f x && $a == b* ]]");
  EXPECT_EQ(text_of("(( i++ ))"), "(( i++ ))");
  EXPECT_EQ(text_of("{ a } always { b }"), "{ a } always { b }");
  EXPECT_EQ(text_of("f() { echo hi; }"), "f () { echo hi }");
}

TEST(JobText, SublistChains) {
  auto prog = compile("! a && b || c | d");
  ASSERT_TRUE(prog);
  EXPECT_EQ(sublist_text(*prog, 1), "! a && b || c | d");
}

// ---- The trap table ------------------------------------------------------------

TEST(SignalTable, NumbersAndAliases) {
  EXPECT_EQ(SignalTable::name_to_sig("0"), SIGEXIT);
  EXPECT_EQ(SignalTable::name_to_sig("2"), SIGINT);
  EXPECT_EQ(SignalTable::name_to_sig("ERR"), SIGZERR);
  EXPECT_EQ(SignalTable::name_to_sig("CLD"), SIGCHLD);
  EXPECT_EQ(SignalTable::name_to_sig("ABRT"), SIGABRT);
  EXPECT_FALSE(SignalTable::name_to_sig("99").has_value());
  EXPECT_FALSE(SignalTable::name_to_sig("1234").has_value());
  EXPECT_STREQ(SignalTable::sig_message(SIGTERM), "terminated");
  EXPECT_STREQ(SignalTable::sig_message(SIGTTOU), "suspended (tty output)");
}

TEST(SignalTable, FramesRestoreOnlyWhenAsked) {
  SignalTable t;
  std::optional<Trap> exit_trap;
  t.push_frame();
  t.set(SIGUSR1, TrapAction::Command, "x");
  t.pop_frame(false, exit_trap);
  EXPECT_EQ(t.get(SIGUSR1).command, "x");          // kept without LOCAL_TRAPS

  t.push_frame();
  t.set(SIGUSR1, TrapAction::Ignore);
  auto changed = t.pop_frame(true, exit_trap);
  EXPECT_EQ(t.get(SIGUSR1).command, "x");          // restored
  EXPECT_EQ(changed, std::vector<int>{SIGUSR1});
}

TEST(SignalTable, AnOuterLocalTrapsFrameRestoresWhatAnInnerOneKept) {
  SignalTable t;
  std::optional<Trap> exit_trap;
  t.push_frame();                                  // outer, LOCAL_TRAPS
  t.push_frame();                                  // inner, not
  t.set(SIGUSR2, TrapAction::Command, "inner");
  t.pop_frame(false, exit_trap);
  EXPECT_EQ(t.get(SIGUSR2).command, "inner");
  t.pop_frame(true, exit_trap);
  EXPECT_EQ(t.get(SIGUSR2).action, TrapAction::Default);
}

TEST(SignalTable, AnExitTrapSetInAFunctionIsHandedBack) {
  SignalTable t;
  t.set(SIGEXIT, TrapAction::Command, "outer");
  std::optional<Trap> exit_trap;
  t.push_frame();
  t.set(SIGEXIT, TrapAction::Command, "inner");
  t.pop_frame(false, exit_trap);
  ASSERT_TRUE(exit_trap.has_value());
  EXPECT_EQ(exit_trap->command, "inner");
  EXPECT_EQ(t.get(SIGEXIT).command, "outer");
}

TEST(SignalTable, ChildrenKeepIgnoredSignalsOnly) {
  SignalTable t;
  t.set(SIGINT, TrapAction::Command, "x");
  t.set(SIGQUIT, TrapAction::Ignore);
  t.set(SIGEXIT, TrapAction::Command, "bye");
  t.set(SIGZERR, TrapAction::Command, "z");
  SignalTable sub = t;
  sub.reset_for_child(true);
  EXPECT_EQ(sub.get(SIGINT).action, TrapAction::Default);
  EXPECT_EQ(sub.get(SIGQUIT).action, TrapAction::Ignore);
  EXPECT_EQ(sub.get(SIGEXIT).action, TrapAction::Default);
  EXPECT_EQ(sub.get(SIGZERR).command, "z");         // `( … )` keeps ZERR
  t.reset_for_child(false);
  EXPECT_EQ(t.get(SIGZERR).action, TrapAction::Default);
}

TEST(SignalManager, KillJobRefusesInitAndOurOwnGroup) {
  // B17: pgid -1 or 0 used to signal init or the caller's process group.
  EXPECT_FALSE(SignalManager::kill_job(-1, 0).has_value());
  EXPECT_FALSE(SignalManager::kill_job(0, 0).has_value());
  EXPECT_FALSE(SignalManager::kill_job(1, 0).has_value());
}

// ---- trap ----------------------------------------------------------------------

// Traps live in the process-wide Signals, so each Shell below gets a scope
// of its own: its destructor resets them.
TEST(Trap, ListingIsInSignalOrderWithZshQuoting) {
  {
    Shell sh;
    EXPECT_EQ(out(sh, "trap 'echo hi' INT; trap '' QUIT; trap 'echo t' TERM USR1; trap"),
              "trap -- 'echo hi' INT\n"
              "trap -- '' QUIT\n"
              "trap -- 'echo t' USR1\n"
              "trap -- 'echo t' TERM\n");
  }
  {
    Shell sh;
    EXPECT_EQ(out(sh, "trap 'echo e' EXIT; trap 'echo z' ZERR; trap x DEBUG; trap '' HUP; trap"),
              "trap -- 'echo e' EXIT\n"
              "trap -- '' HUP\n"
              "trap -- 'echo z' ZERR\n"
              "trap -- x DEBUG\n");
  }
  Shell sh;
  EXPECT_EQ(out(sh, "trap -- 'echo two' 2; trap"), "trap -- 'echo two' INT\n");
}

TEST(Trap, Resetting) {
  Shell sh;
  EXPECT_EQ(out(sh, "trap 'echo hi' INT; trap - INT; trap; trap '' INT; trap"),
            "trap -- '' INT\n");
  // A signal as the first word resets every signal named.
  EXPECT_EQ(out(sh, "trap 'a' INT TERM; trap INT TERM; trap"), "");
  EXPECT_EQ(out(sh, "trap 'a' INT TERM; trap -; trap"), "");
  // An action with no signal does nothing.
  EXPECT_EQ(out(sh, "trap 'a' INT; trap ''; echo $?; trap - INT"), "0\n");
}

TEST(Trap, UndefinedSignal) {
  Shell sh;
  auto r = sh.run("trap 'echo x' FOO");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: trap: undefined signal: FOO\n");
  EXPECT_EQ(sh.run("trap 'echo x' 99").err, "jfsh: trap: undefined signal: 99\n");
}

TEST(Trap, RunsAfterTheBuiltinThatRaisedItWithTheOldStatus) {
  Shell sh;
  EXPECT_EQ(out(sh, "trap 'echo caught $?' USR1; false; kill -USR1 $$; echo after $?"),
            "caught 1\nafter 0\n");
}

TEST(Trap, TrapFunctions) {
  Shell sh;
  EXPECT_EQ(out(sh, "TRAPUSR1() { echo got $1; }; kill -USR1 $$; echo after"),
            "got " + std::to_string(SIGUSR1) + "\nafter\n");
  // `trap` replaces the function, and the function the trap.
}

TEST(Trap, TrapAndTrapFunctionReplaceEachOther) {
  Shell sh;
  EXPECT_EQ(out(sh, "trap 'echo a' USR1; TRAPUSR1() { echo f; }; kill -USR1 $$; "
                    "trap 'echo b' USR1; kill -USR1 $$"),
            "f\nb\n");
  EXPECT_EQ(sh.ctx.functions.count("TRAPUSR1"), 0u);
}

TEST(Trap, AFailingTrapFunctionAbortsANonInteractiveShell) {
  Shell sh;
  EXPECT_EQ(out(sh, "TRAPUSR1() { echo got; return 3; }; kill -USR1 $$; echo after"),
            "got\n");
  EXPECT_TRUE(sh.ctx.exiting);
  EXPECT_EQ(sh.ctx.exit_val, 0);
}

TEST(Trap, ExitTrapOfAFunctionRunsWhenItReturns) {
  Shell sh;
  EXPECT_EQ(out(sh, "f() { trap 'echo fexit' EXIT; echo in; }; trap 'echo outer' EXIT; "
                    "f; echo after"),
            "in\nfexit\nafter\n");
  EXPECT_EQ(Signals.traps().get(SIGEXIT).command, "echo outer");
}

TEST(Trap, TrapsSetInAFunctionStayUnlessLocalTraps) {
  {
    Shell sh;
    EXPECT_EQ(out(sh, "f() { trap 'echo inner' USR1; }; f; trap"),
              "trap -- 'echo inner' USR1\n");
  }
  Shell sh;
  sh.opts.assign(OPT_LOCALTRAPS, true);
  EXPECT_EQ(out(sh, "g() { trap 'echo g' USR2; kill -USR2 $$; }; g; trap; echo end"),
            "g\nend\n");
}

TEST(Trap, SubshellsDropSignalTraps) {
  Shell sh;
  EXPECT_EQ(out(sh, "trap 'echo int' INT; (trap)"), "");
  // The subshell's USR1 is back to the default, so it dies of it.
  EXPECT_EQ(out(sh, "trap 'echo usr' USR1; (sh -c 'kill -USR1 $PPID'; echo survived); echo $?"),
            std::to_string(128 + SIGUSR1) + "\n");
}

TEST(Trap, DebugRunsBeforeEachCommandButNotInsideTraps) {
  {
    Shell sh;
    EXPECT_EQ(out(sh, "trap 'echo dbg' DEBUG; echo a; echo b"), "dbg\na\ndbg\nb\n");
  }
  Shell sh;
  auto r = sh.run("trap 'echo z' ZERR; trap 'no-such-command-x' DEBUG; true");
  EXPECT_EQ(r.out, "");   // the failing DEBUG trap sets off no ZERR
}

TEST(Trap, ATrappedSignalInterruptsWait) {
  Shell sh;
  EXPECT_EQ(out(sh, "trap 'echo usr' USR1; (sleep 0.2; kill -USR1 $$; sleep 1) & wait; echo $?"),
            "usr\n" + std::to_string(128 + SIGUSR1) + "\n");
  sh.run("kill %1; wait");
}

// ---- ZERR, ERR_EXIT, ERR_RETURN ---------------------------------------------------

TEST(Zerr, OnlyTheFailingCommandSetsItOff) {
  auto zerr = [](std::string_view src) {
    Shell sh;
    return sh.run("trap 'echo z' ZERR; " + std::string(src)).out;
  };
  EXPECT_EQ(zerr("false; true; if false; then :; fi; false || true; ! true; echo x"), "z\nx\n");
  EXPECT_EQ(zerr("{ false; }; echo x"), "z\nx\n");
  EXPECT_EQ(zerr("if true; then false; fi; echo x"), "z\nx\n");
  EXPECT_EQ(zerr("f() { false; }; f; echo x"), "z\nx\n");
  EXPECT_EQ(zerr("f() { false; }; f && true; echo x"), "x\n");
  EXPECT_EQ(zerr("f() { false && true; }; f; echo x"), "z\nx\n");
  EXPECT_EQ(zerr("f() { return 3; }; f; echo x"), "z\nx\n");
  EXPECT_EQ(zerr("(false); echo x"), "z\nz\nx\n");     // in the subshell and for it
  EXPECT_EQ(zerr("false | true; true | false; echo x"), "z\nx\n");
  EXPECT_EQ(zerr("while false; do :; done; for x in a; do false; done; echo x"), "z\nx\n");
  EXPECT_EQ(zerr("x=$(false); echo x; case a in a) false;; esac; echo x"), "z\nx\nz\nx\n");
  EXPECT_EQ(zerr("[[ a = b ]]; (( 0 )); x=1; $(exit 2); echo x"), "z\nz\nz\nx\n");
  EXPECT_EQ(zerr("{ false && true; }; echo x; if false; then :; else false; fi; echo y"),
            "x\nz\ny\n");
  EXPECT_EQ(zerr("for x in 1; do false && true; done; echo x"), "x\n");
  EXPECT_EQ(zerr("y=$(false; true); echo x"), "x\n");  // not in command substitution
}

TEST(Zerr, TrapFunction) {
  Shell sh;
  EXPECT_EQ(out(sh, "TRAPZERR() { echo tz; }; false; echo x"), "tz\nx\n");
}

// The output of src with ERR_EXIT or ERR_RETURN set, and whether the shell
// is exiting afterwards.
std::string with_option(OptIndex opt, std::string_view src, bool* exiting = nullptr) {
  Shell sh;
  sh.opts.assign(opt, true);
  std::string o = sh.run(src).out;
  if (exiting) *exiting = sh.ctx.exiting;
  return o;
}

TEST(ErrExit, ExitsWithTheFailingStatusAfterZerr) {
  bool exiting = false;
  EXPECT_EQ(with_option(OPT_ERREXIT, "false && true; echo a; { false || false; }; echo b",
                        &exiting),
            "a\n");
  EXPECT_TRUE(exiting);
  EXPECT_EQ(with_option(OPT_ERREXIT, "trap 'echo zerr' ZERR; false; echo notreached"),
            "zerr\n");
  EXPECT_EQ(with_option(OPT_ERREXIT, "f() { false && true; }; f; echo x"), "");
}

TEST(ErrExit, ConditionsAndAndOrChainsAreExemptAllTheWayDown) {
  EXPECT_EQ(with_option(OPT_ERREXIT, "f() { false; echo in f; }; if f; then echo y; fi; "
                                     "while false; do :; done; echo z"),
            "in f\ny\nz\n");
  EXPECT_EQ(with_option(OPT_ERREXIT, "f() { false; echo in; }; f || echo caller $?"), "in\n");
}

TEST(ErrReturn, OnlyTheCommandsOwnPlaceExemptsIt) {
  // zsh 5.9: unlike ERR_EXIT, a caller's condition or && / || does not.
  EXPECT_EQ(with_option(OPT_ERRRETURN, "f() { false; echo in; }; f || echo caller $?"),
            "caller 1\n");
  EXPECT_EQ(with_option(OPT_ERRRETURN, "f() { false; echo in; }; if f; then echo y; fi; echo end"),
            "end\n");
  EXPECT_EQ(with_option(OPT_ERRRETURN, "f() { false && true; echo in; }; f; echo end $?"),
            "in\nend 0\n");
  EXPECT_EQ(with_option(OPT_ERRRETURN, "f() { ! true; echo in; }; f; echo end $?"),
            "in\nend 0\n");
  // At the top level it ends the script.
  EXPECT_EQ(with_option(OPT_ERRRETURN, "f() { false; echo in; }; f; echo after"), "");
}

// ---- kill ----------------------------------------------------------------------

TEST(Kill, ListsSignals) {
  Shell sh;
#ifdef __linux__
  EXPECT_EQ(out(sh, "kill -l"),
            "HUP INT QUIT ILL TRAP IOT BUS FPE KILL USR1 SEGV USR2 PIPE ALRM TERM STKFLT "
            "CHLD CONT STOP TSTP TTIN TTOU URG XCPU XFSZ VTALRM PROF WINCH POLL PWR SYS\n");
#endif
  EXPECT_EQ(out(sh, "kill -l 15 INT 130 TERM"), "TERM\n2\nINT\n15\n");
  auto r = sh.run("kill -l FOO; echo $?");
  EXPECT_EQ(r.out, "1\n");
  EXPECT_EQ(r.err, "jfsh: kill: unknown signal: SIGFOO\n");
}

TEST(Kill, Errors) {
  Shell sh;
  auto r = sh.run("kill 999999; echo $?; kill -s FOO $$; echo $?; kill; echo $?; "
                  "kill %1; echo $?; kill %foo; echo $?; kill abc; echo $?");
  EXPECT_EQ(r.out, "1\n1\n1\n1\n1\n1\n");
  EXPECT_EQ(r.err,
            "jfsh: kill: kill 999999 failed: no such process\n"
            "jfsh: kill: unknown signal: SIGFOO\n"
            "jfsh: kill: type kill -l for a list of signals\n"
            "jfsh: kill: not enough arguments\n"
            "jfsh: kill: %1: no such job\n"
            "jfsh: kill: job not found: foo\n"
            "jfsh: kill: illegal pid: abc\n");
  EXPECT_EQ(out(sh, "kill -n 0 $$; echo $?; kill -0 $$; echo $?; kill -s 0 $$; echo $?"),
            "0\n0\n0\n");
}

TEST(Kill, JobsAndPids) {
  Shell sh;
  EXPECT_EQ(out(sh, "sleep 5 & kill %1; wait %1; echo $?"),
            std::to_string(128 + SIGTERM) + "\n");
  EXPECT_EQ(out(sh, "sleep 5 & kill -9 $!; wait $!; echo $?"),
            std::to_string(128 + SIGKILL) + "\n");
  // The background child execs the command itself: killing $! kills sleep.
  EXPECT_EQ(out(sh, "sleep 5 & kill -KILL $!; wait; echo done"), "done\n");
}

// ---- wait, jobs, disown, fg, bg --------------------------------------------------

TEST(Wait, Statuses) {
  Shell sh;
  EXPECT_EQ(out(sh, "true & wait $!; echo $?; false & wait $!; echo $?"), "0\n1\n");
  EXPECT_EQ(out(sh, "(exit 7) & wait %1; echo $?"), "7\n");
  auto r = sh.run("(exit 7) & p=$!; sleep 0.2; wait $p; echo $?; wait $p; echo $?");
  EXPECT_EQ(r.out, "7\n127\n");
  EXPECT_NE(r.err.find("is not a child of this shell"), std::string::npos);
  r = sh.run("wait 999999; echo $?; wait %1; echo $?");
  EXPECT_EQ(r.out, "127\n127\n");
  EXPECT_EQ(r.err, "jfsh: wait: pid 999999 is not a child of this shell\n"
                   "jfsh: wait: %1: no such job\n");
  EXPECT_EQ(out(sh, "sleep 0.1 & sleep 0.1 & wait; echo $?"), "0\n");
}

TEST(Jobs, ListsBackgroundJobs) {
  Shell sh;
  EXPECT_EQ(out(sh, "sleep 0.2 & jobs; echo $?; wait"), "[1]  + running    sleep 0.2\n0\n");
  EXPECT_EQ(out(sh, "sleep 1 & sleep 1 & jobs; disown %1; jobs; kill %2; wait"),
            "[1]  - running    sleep 1\n"
            "[2]  + running    sleep 1\n"
            "[2]  + running    sleep 1\n");
  EXPECT_EQ(out(sh, "jobs | cat"), "");          // a subshell has no jobs
  auto r = sh.run("jobs -x");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: jobs: bad option: -x\n");
}

TEST(Jobs, FinishedJobsAreListedOnce) {
  Shell sh;
  EXPECT_EQ(out(sh, "(exit 2) & wait $!; jobs"), "");
  EXPECT_EQ(out(sh, "true & sleep 0.2; jobs -s; jobs -r"), "");
}

TEST(Jobs, NoJobControlWithoutATerminal) {
  Shell sh;
  auto r = sh.run("fg; echo $?; bg; echo $?; fg %1; echo $?; disown; echo $?");
  EXPECT_EQ(r.out, "1\n1\n1\n1\n");
  EXPECT_EQ(r.err,
            "jfsh: fg: no job control in this shell.\n"
            "jfsh: bg: no job control in this shell.\n"
            "jfsh: fg: no job control in this shell.\n"
            "jfsh: disown: no current job\n");
  EXPECT_EQ(out(sh, "sleep 1 & disown; echo $?; jobs; wait; echo $?"), "0\n0\n");
}

}  // namespace
}  // namespace jfsh
