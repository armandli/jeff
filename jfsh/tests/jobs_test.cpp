// Job control, signals and traps (M6) without a terminal: the job table,
// job specs and status lines, the one-line job text, the trap table and
// its function frames, and the trap, kill, wait, jobs, disown, fg and bg
// builtins run through Executor::exec_string.  Expected output is what
// zsh 5.9 (`zsh -f -c`) prints.  Interactive job control is in
// interactive_test.cpp.

#include <cerrno>
#include <csignal>
#include <fcntl.h>
#include <poll.h>
#include <cstdio>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <initializer_list>
#include <string>
#include <string_view>
#include <tuple>

#include <gtest/gtest.h>

#include <jfsh/core/aliases.h>
#include <jfsh/core/jobs.h>
#include <jfsh/core/signals.h>
#include <jfsh/exec/executor.h>
#include <jfsh/lexer/input.h>
#include <jfsh/lexer/lexer.h>
#include <jfsh/parser/parser.h>
#include <shell_fixture.h>

namespace jfsh {
namespace {

using test::Shell;

str out(Shell& sh, strv src) { return sh.run(src).out; }

// The wait status of a child that exits with `code`, or (code < 0) is
// killed by signal -code, or (stop) stops with signal `code`.  A stopping
// child gets its own group: stop signals to an orphaned group are dropped.
int child_status(int code, bool stop = false) {
  pid_t pid = ::fork();
  if (pid == 0) {
    if (stop) { ::setpgid(0, 0); ::raise(code); _exit(0); }
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

ProgRef compile(str src) {
  StringInputSource in{s::move(src)};
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
  s::FILE* f = s::tmpfile();
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

str text_of(str src) {
  auto prog = compile(s::move(src));
  // list → sublist → pipe: the first pipeline is at 2.
  return prog ? pipeline_text(*prog, 2) : "";
}

TEST(JobText, PipelinesAndSimpleCommandsAsWritten) {
  EXPECT_EQ(text_of("sleep 30 | cat"), "sleep 30 | cat");
  EXPECT_EQ(text_of("a |& b"), "a 2>&1 | b");
  EXPECT_EQ(text_of("x=1 echo \"$y\" >out 2>&1"), "x=1 echo \"$y\" > out 2>&1");
  EXPECT_EQ(text_of("cat <(ls) <<< $w"), "cat <(ls) <<< $w");
  EXPECT_EQ(text_of("typeset -i n=3"), "typeset -i n=3");
}

TEST(JobText, CompoundCommandsOnOneLine) {
  // zsh 5.9's getjobtext: every list in a compound command ends in "; ".
  EXPECT_EQ(text_of("( sleep 0.4; kill -USR1 $$ )"), "( sleep 0.4; kill -USR1 $$; )");
  EXPECT_EQ(text_of("{ a; b & }"), "{ a; b &; }");
  EXPECT_EQ(text_of("{ a & b }"), "{ a & b; }");
  EXPECT_EQ(text_of("while true; do sleep 1; done"), "while true; do; sleep 1; done");
  EXPECT_EQ(text_of("until a\nb; do c; done"), "until a; b; do; c; done");
  EXPECT_EQ(text_of("if a; then b; elif c; then d; else e; fi"),
            "if a; then; b; elif c; then; d; else; e; fi");
  EXPECT_EQ(text_of("for x in a b; do echo $x; done"), "for x in a b; do; echo $x; done");
  EXPECT_EQ(text_of("for x; do b; done"), "for x; do; b; done");
  EXPECT_EQ(text_of("for ((i=0;i<1;i++)) do b; done"), "for ((i=0; i<1; i++)) do; b; done");
  EXPECT_EQ(text_of("repeat 2 b"), "repeat 2; do; b; done");
  EXPECT_EQ(text_of("case $x in a|b) echo y;; c) ;& esac"),
            "case $x in (a | b) echo y ;; (c)  ;& esac");
  EXPECT_EQ(text_of("[[ -f x && $a == b* ]]"), "[[ -f x && $a == b* ]]");
  EXPECT_EQ(text_of("(( i++ ))"), "(( i++ ))");
  EXPECT_EQ(text_of("{ a } always { b }"), "{; a; } always {; b; }");
  EXPECT_EQ(text_of("( (a) )"), "( ( a; ); )");
  EXPECT_EQ(text_of("{ a; b } >/dev/null"), "{ a; b; } > /dev/null");
  // Function bodies are left out.
  EXPECT_EQ(text_of("f() { echo hi; }"), "f () { ... }");
  EXPECT_EQ(text_of("() { a; } x"), "() { ... }");
}

TEST(JobText, SublistChainsShowTheirLastPipeline) {
  // zsh 5.9 lists `a && b &` as `b`, and `! a &` as `a`.
  auto prog = compile("! a && b || c | d");
  ASSERT_TRUE(prog);
  EXPECT_EQ(sublist_text(*prog, 1), "c | d");
  auto neg = compile("! a");
  ASSERT_TRUE(neg);
  EXPECT_EQ(sublist_text(*neg, 1), "a");
}

// ---- The trap table ------------------------------------------------------------

TEST(SignalTable, NumbersAndAliases) {
  EXPECT_EQ(SignalTable::name_to_sig("0"), SIGEXIT);
  EXPECT_EQ(SignalTable::name_to_sig("2"), SIGINT);
  EXPECT_EQ(SignalTable::name_to_sig("ERR"), SIGZERR);
#ifdef __linux__
  EXPECT_EQ(SignalTable::name_to_sig("CLD"), SIGCHLD);
#else
  EXPECT_FALSE(SignalTable::name_to_sig("CLD").has_value());   // zsh: Linux only
#endif
  EXPECT_EQ(SignalTable::name_to_sig("ABRT"), SIGABRT);
  EXPECT_FALSE(SignalTable::name_to_sig("99").has_value());
  EXPECT_FALSE(SignalTable::name_to_sig("1234").has_value());
  EXPECT_STREQ(SignalTable::sig_message(SIGTERM), "terminated");
  EXPECT_STREQ(SignalTable::sig_message(SIGTTOU), "suspended (tty output)");
}

TEST(SignalTable, FramesRestoreOnlyWhenAsked) {
  SignalTable t;
  opt<Trap> exit_trap;
  t.push_frame();
  t.set(SIGUSR1, TrapAction::Command, "x");
  t.pop_frame(false, exit_trap);
  EXPECT_EQ(t.get(SIGUSR1).command, "x");          // kept without LOCAL_TRAPS

  t.push_frame();
  t.set(SIGUSR1, TrapAction::Ignore);
  auto changed = t.pop_frame(true, exit_trap);
  EXPECT_EQ(t.get(SIGUSR1).command, "x");          // restored
  ASSERT_EQ(changed.size(), 1u);
  EXPECT_EQ(changed[0].first, SIGUSR1);
  EXPECT_EQ(changed[0].second, TrapAction::Ignore);   // what the function had
}

TEST(SignalTable, AnOuterLocalTrapsFrameRestoresWhatAnInnerOneKept) {
  SignalTable t;
  opt<Trap> exit_trap;
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
  opt<Trap> exit_trap;
  t.push_frame();
  t.set(SIGEXIT, TrapAction::Command, "inner");
  t.pop_frame(false, exit_trap);
  ASSERT_TRUE(exit_trap.has_value());
  EXPECT_EQ(exit_trap->command, "inner");
  EXPECT_EQ(t.get(SIGEXIT).command, "outer");
}

TEST(SignalTable, ChildrenResetTrapsByKind) {
  SignalTable t;
  t.set(SIGINT, TrapAction::Command, "x");
  t.set(SIGQUIT, TrapAction::Ignore);
  t.set(SIGUSR1, TrapAction::Function);
  t.set(SIGEXIT, TrapAction::Command, "bye");
  t.set(SIGZERR, TrapAction::Command, "z");
  // `( … )`: signal traps and ignores go, TRAPxxx, ZERR and DEBUG stay.
  SignalTable sub = t;
  sub.reset_for_child(ChildKind::Subshell, false);
  EXPECT_EQ(sub.get(SIGINT).action, TrapAction::Default);
  EXPECT_EQ(sub.get(SIGQUIT).action, TrapAction::Default);
  EXPECT_EQ(sub.get(SIGUSR1).action, TrapAction::Function);
  EXPECT_EQ(sub.get(SIGEXIT).action, TrapAction::Default);
  EXPECT_EQ(sub.get(SIGZERR).command, "z");
  // POSIX_TRAPS keeps the ignores.
  SignalTable posix = t;
  posix.reset_for_child(ChildKind::Subshell, true);
  EXPECT_EQ(posix.get(SIGQUIT).action, TrapAction::Ignore);
  // A foreground pipeline stage keeps everything but EXIT.
  SignalTable stage = t;
  stage.reset_for_child(ChildKind::Stage, false);
  EXPECT_EQ(stage.get(SIGINT).command, "x");
  EXPECT_EQ(stage.get(SIGQUIT).action, TrapAction::Ignore);
  EXPECT_EQ(stage.get(SIGEXIT).action, TrapAction::Default);
  // An external command keeps only the ignores.
  t.reset_for_child(ChildKind::Other, false);
  EXPECT_EQ(t.get(SIGINT).action, TrapAction::Default);
  EXPECT_EQ(t.get(SIGQUIT).action, TrapAction::Ignore);
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
    // USR1 is 10 on Linux but 30 on macOS, so it lists before or after TERM (15).
    str usr1 = "trap -- 'echo t' USR1\n";
    str term = "trap -- 'echo t' TERM\n";
    EXPECT_EQ(out(sh, "trap 'echo hi' INT; trap '' QUIT; trap 'echo t' TERM USR1; trap"),
              "trap -- 'echo hi' INT\n"
              "trap -- '' QUIT\n" + (SIGUSR1 < SIGTERM ? usr1 + term : term + usr1));
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
            "got " + s::to_string(SIGUSR1) + "\nafter\n");
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
  // zsh 5.9: a -c string ends with `kill`'s status, a script with 1.
  EXPECT_EQ(sh.ctx.exit_val, 0);
  Shell script;
  EXPECT_EQ(script.run_script("TRAPUSR1() { echo got; return 3; }\nkill -USR1 $$\necho after\n")
                .out,
            "got\n");
  EXPECT_TRUE(script.ctx.exiting);
  EXPECT_EQ(script.ctx.exit_val, 1);
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
            s::to_string(128 + SIGUSR1) + "\n");
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
            "usr\n" + s::to_string(128 + SIGUSR1) + "\n");
  sh.run("kill %1; wait");
}

// ---- ZERR, ERR_EXIT, ERR_RETURN ---------------------------------------------------

TEST(Zerr, OnlyTheFailingCommandSetsItOff) {
  auto zerr = [](strv src) {
    Shell sh;
    return sh.run("trap 'echo z' ZERR; " + str(src)).out;
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
str with_option(OptIndex option, strv src, bool* exiting = nullptr) {
  Shell sh;
  sh.opts.assign(option, true);
  str o = sh.run(src).out;
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
            s::to_string(128 + SIGTERM) + "\n");
  EXPECT_EQ(out(sh, "sleep 5 & kill -9 $!; wait $!; echo $?"),
            s::to_string(128 + SIGKILL) + "\n");
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
  EXPECT_NE(r.err.find("is not a child of this shell"), str::npos);
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

// ---- M6 review: zsh 5.9's behaviour, probe by probe ------------------------------

TEST(JobsM6Review, KillListsAndTranslatesAsZsh) {
  Shell sh;
  // A number is an exit status as WIFSIGNALED / WTERMSIG read it.
  auto r = sh.run("kill -l 1000 -5 256 -1 ' 3' 3x; echo $?");
  EXPECT_EQ(r.out, "104\n123\n256\n-1\nQUIT\n1\n");
  EXPECT_EQ(r.err, "jfsh: kill: unknown signal: SIGx\n");
  // Names ignore case, but SIG must be upper case; errors are counted.
  r = sh.run("kill -l sigint Int SIGsigint; echo $?");
  EXPECT_EQ(r.out, "2\n2\n");
  EXPECT_EQ(r.err, "jfsh: kill: unknown signal: SIGsigint\n"
                   "jfsh: kill: unknown signal: SIGsigint\n");
  // Only `-l` lists; `-L` is a signal name.
  r = sh.run("kill -L; echo $?");
  EXPECT_EQ(r.out, "1\n");
  EXPECT_EQ(r.err, "jfsh: kill: unknown signal: SIGL\n"
                   "jfsh: kill: type kill -l for a list of signals\n");
}

TEST(JobsM6Review, KillSignalSpecsAsZsh) {
  Shell sh;
  auto r = sh.run("kill -s; echo $?; kill -n INT $$; echo $?; kill -q $$; echo $?; "
                  "kill - ; echo $?; kill -1x $$; echo $?; kill -s 30 $$; echo $?");
  EXPECT_EQ(r.out, "1\n1\n1\n1\n1\n1\n");
  EXPECT_EQ(r.err, "jfsh: kill: -s: argument expected\n"
                   "jfsh: kill: invalid signal number: INT\n"
                   "jfsh: kill: unknown signal: SIGQ\n"
                   "jfsh: kill: type kill -l for a list of signals\n"
                   "jfsh: kill: -: signal name expected\n"
                   "jfsh: kill: invalid signal number: -1x\n"
                   "jfsh: kill: unknown signal: SIG30\n"
                   "jfsh: kill: type kill -l for a list of signals\n");
  // Any case, with or without SIG, for -s and -NAME; -n takes a number.
  EXPECT_EQ(out(sh, "trap 'echo g' USR1; kill -sigusr1 $$; kill -s usr1 $$; "
                    "kill -n " + s::to_string(SIGUSR1) + " $$; echo $?"),
            "g\ng\ng\n0\n");
}

TEST(JobsM6Review, KillTargetsAsZsh) {
  Shell sh;
  // The status counts the failures; a lone `-` after the signal is skipped.
  auto r = sh.run("kill -0 1x 2y; echo $?; kill -0 -; echo $?; kill -0 - $$; echo $?");
  EXPECT_EQ(r.out, "2\n1\n0\n");
  EXPECT_EQ(r.err, "jfsh: kill: illegal pid: 1x\n"
                   "jfsh: kill: illegal pid: 2y\n"
                   "jfsh: kill: not enough arguments\n");
  // A pid that doesn't fit is no process (zsh's atoi could make it -1).
  r = sh.run("kill -0 99999999999; echo $?");
  EXPECT_EQ(r.out, "1\n");
  EXPECT_EQ(r.err, "jfsh: kill: kill 99999999999 failed: no such process\n");
}

TEST(JobsM6Review, FinishedBackgroundJobsAreForgottenWithoutMonitor) {
  Shell sh;
  auto r = sh.run("sleep 0.1 & sleep 0.4; jobs; wait %1; echo $?");
  EXPECT_EQ(r.out, "127\n");
  EXPECT_EQ(r.err, "jfsh: wait: %1: no such job\n");
  // `wait $!` still has the status.
  EXPECT_EQ(out(sh, "(exit 3) & p=$!; sleep 0.3; jobs; wait $p; echo $?"), "3\n");
  EXPECT_EQ(out(sh, "sleep 5 & kill %1; sleep 0.3; jobs; echo e"), "e\n");
}

TEST(JobsM6Review, JobSpecErrorsInOrderWithStatus127) {
  Shell sh;
  auto r = sh.run("sleep 1 & jobs %1 %2; echo $?; disown %3; echo $?; jobs %; kill %1");
  EXPECT_EQ(r.out, "[1]  + running    sleep 1\n127\n127\n[1]  + running    sleep 1\n");
  EXPECT_EQ(r.err, "jfsh: jobs: %2: no such job\njfsh: disown: %3: no such job\n");
  // The missing current job is status 1.
  r = sh.run("wait; disown; echo $?; wait --; echo $?");
  EXPECT_EQ(r.out, "1\n0\n");
  EXPECT_EQ(r.err, "jfsh: disown: no current job\n");
}

TEST(JobsM6Review, PercentQuestionIsAJobSpecNotAPattern) {
  Shell sh;
  auto r = sh.run("sleep 0.3 & jobs %?lee; wait %?lee; echo $?; echo %?ee");
  EXPECT_EQ(r.out, "[1]  + running    sleep 0.3\n0\n%?ee\n");
  // Only the `?` right after a leading `%`.
  r = sh.run("echo %?ee*");
  EXPECT_EQ(r.err, "jfsh: no matches found: %?ee*\n");
}

TEST(JobsM6Review, ManyBackgroundExternalsDontRefreshEverySpecial) {
  // Each external command refreshed every special parameter for its
  // environment ($userdirs reads the whole user database): 2000 of them
  // in the background took 51 s, zsh 1.1 s.
  Shell sh;
  auto start = s::chrono::steady_clock::now();
  EXPECT_EQ(out(sh, "for i in {1..200}; do /usr/bin/true & done; wait; echo ok"), "ok\n");
  EXPECT_LT(s::chrono::steady_clock::now() - start, s::chrono::seconds(4));
}

// Runs the jfsh binary on `script` (-c) with the `ignored` signals ignored
// from the start; its output, and its wait status in `status`.
// With `file`, `script` is the path of a script to run instead.
str run_binary(const char* script, std::initializer_list<int> ignored, int& status,
               bool file = false) {
  int pfd[2];
  if (::pipe(pfd) != 0) return "<pipe>";
  pid_t pid = ::fork();
  if (pid == 0) {
    for (int sig : ignored) ::signal(sig, SIG_IGN);
    // Never the test's own stdin: a command that reads it would wait for ever.
    if (int null = ::open("/dev/null", O_RDONLY); null >= 0) {
      ::dup2(null, 0);
      ::close(null);
    }
    ::dup2(pfd[1], 1);
    ::dup2(pfd[1], 2);
    ::close(pfd[0]);
    if (file) ::execl(JFSH_BINARY, JFSH_BINARY, "-f", script, static_cast<char*>(nullptr));
    else      ::execl(JFSH_BINARY, JFSH_BINARY, "-f", "-c", script, static_cast<char*>(nullptr));
    _exit(127);
  }
  ::close(pfd[1]);
  // A shell that loops (a trap that keeps raising its signal) fails the
  // test after 30 s instead of hanging the suite.
  str  got;
  char buf[256];
  auto deadline = s::chrono::steady_clock::now() + s::chrono::seconds(30);
  for (;;) {
    auto left = s::chrono::duration_cast<s::chrono::milliseconds>(
        deadline - s::chrono::steady_clock::now());
    struct pollfd p{pfd[0], POLLIN, 0};
    int ready = left.count() > 0 ? ::poll(&p, 1, static_cast<int>(left.count())) : 0;
    if (ready < 0 and errno == EINTR) continue;
    if (ready <= 0) {
      ADD_FAILURE() << "jfsh -c didn't finish: " << script;
      ::kill(pid, SIGKILL);
      break;
    }
    ssize_t n = ::read(pfd[0], buf, sizeof buf);
    if (n <= 0) break;
    got.append(buf, static_cast<s::size_t>(n));
  }
  ::close(pfd[0]);
  ::waitpid(pid, &status, 0);
  return got;
}

TEST(JobsM6Review, AScriptThatIsHungUpRunsItsExitTrap) {
  // zsh: status 1, and the EXIT trap runs.
  int status = 0;
  EXPECT_EQ(run_binary("trap 'echo exit' EXIT; kill -HUP $$; echo after", {}, status),
            "exit\n");
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 1);
}

TEST(JobsM6Review, BareWaitKeepsStatusesForWaitPid) {
  // zsh 5.9: `wait` and `wait %1` leave each status for one `wait PID`.
  Shell sh;
  auto r = sh.run("(exit 4) & p=$!; wait; wait $p; echo $?; wait $p; echo $?");
  EXPECT_EQ(r.out, "4\n127\n");
  EXPECT_EQ(r.err.find("is not a child of this shell"), r.err.rfind("is not a child"));
  EXPECT_EQ(out(sh, "(exit 3) & p=$!; wait %1; wait $p; echo $?"), "3\n");
  EXPECT_EQ(out(sh, "(exit 4) | (exit 5) & p=$!; wait; wait $p; echo $?"), "5\n");
}

TEST(JobsM6Review, PipelineStagesListedSeparatelyWithoutMonitor) {
  Shell sh;
  EXPECT_EQ(out(sh, "sleep 0.3 | cat & jobs; wait"),
            "[1]  + running    sleep 0.3 | \n"
            "       running    cat\n");
}

TEST(TrapsM6Review, CommandIsParsedWhenSet) {
  Shell sh;
  auto r = sh.run("trap 'echo x |' INT; echo $?; trap");
  EXPECT_EQ(r.out, "1\n");
  EXPECT_EQ(r.err, "jfsh:1: parse error near `|'\njfsh: trap: couldn't parse trap command\n");
}

TEST(TrapsM6Review, ListingIsTheParsedTextQuotedAsZsh) {
  Shell sh;
  str usr1 = "trap -- 'echo '\\''a b'\\' USR1\n";
  str quit = "trap -- 'a=b ' QUIT\n";
  str term = "trap -- $'echo a\\necho b' TERM\n";
  EXPECT_EQ(out(sh, "trap \"echo 'a b'\" USR1; trap \"''\" INT; trap 'a=b' QUIT; "
                    "trap $'echo a\\n  echo  b' TERM; trap 'if true; then echo a; fi' HUP; trap"),
            "trap -- $'if true\\nthen\\n\\techo a\\nfi' HUP\n"
            "trap -- \\'\\' INT\n" + quit
            + (SIGUSR1 < SIGTERM ? usr1 + term : term + usr1));
  // typeset -p quotes the same way.
  EXPECT_EQ(out(sh, "x=\"'a'\"; typeset -p x; y=\"it's\"; typeset -p y"),
            "typeset x=\\''a'\\'\ntypeset y='it'\\''s'\n");
}

TEST(TrapsM6Review, NamesAreUpperCaseAndErrIsListedAsErr) {
  Shell sh;
  auto r = sh.run("trap 'echo x' sigint SIGint; echo $?; trap");
  EXPECT_EQ(r.out, "1\n");
  EXPECT_EQ(r.err, "jfsh: trap: undefined signal: sigint\njfsh: trap: undefined signal: SIGint\n");
  EXPECT_EQ(out(sh, "trap 'echo x' ERR; trap; trap y ZERR; trap"),
            "trap -- 'echo x' ERR\ntrap -- y ZERR\n");
}

TEST(TrapsM6Review, ExitTrapIsListedOnlyInItsFunction) {
  Shell sh;
  EXPECT_EQ(out(sh, "trap 'echo top' EXIT; f() { trap; }; f; trap - EXIT"), "");
  EXPECT_EQ(out(sh, "f() { trap 'echo in' EXIT; trap; }; f"),
            "trap -- 'echo in' EXIT\nin\n");
}

TEST(TrapsM6Review, ReturnInATrap) {
  Shell sh;
  // In a function: it returns, with the status of the interrupted command.
  EXPECT_EQ(out(sh, "trap 'echo t; return 3' USR1; f() { false; kill -USR1 $$; echo nr; }; "
                    "f; echo $?"),
            "t\n0\n");
  EXPECT_EQ(out(sh, "trap 'return 3' USR1; f() { (sleep 0.1; kill -USR1 $$) & wait; echo nr; }; "
                    "f; echo $?"),
            s::to_string(128 + SIGUSR1) + "\n");
  // At the top it ends the script with its status.
  auto r = sh.run("trap 'return 3' USR1; kill -USR1 $$; echo after");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.rc, 3);
  sh.run("trap - USR1");
  // ZERR's return returns from the function with its own status.
  EXPECT_EQ(out(sh, "f() { trap 'return 5' ZERR; false; echo nr; }; f; echo $?"), "5\n");
  // An EXIT trap's return gives the exit status.
  r = sh.run("trap 'echo t; return 2' EXIT; echo in");
  EXPECT_EQ(r.out, "in\n");
}

TEST(TrapsM6Review, ATrapFunctionAbortsOnlyWithAnExplicitReturn) {
  {
    Shell sh;
    EXPECT_EQ(out(sh, "TRAPUSR1() { false; }; kill -USR1 $$; echo after"), "after\n");
  }
  {
    Shell sh;
    auto r = sh.run("TRAPUSR1() { return 3; }; f() { kill -USR1 $$; echo inf; }; f; echo out");
    EXPECT_EQ(r.out, "");
    EXPECT_TRUE(sh.ctx.exiting);
    EXPECT_EQ(sh.ctx.exit_val, 0);   // `kill`'s status, as in zsh -c
  }
  {
    // A subshell ends with the trap's status.
    Shell sh;
    EXPECT_EQ(out(sh, "TRAPUSR1() { return 3; }; ( sh -c 'kill -USR1 $PPID'; echo insub ); "
                      "echo out $?"),
              "out 3\n");
  }
}

TEST(TrapsM6Review, SubshellsResetSignalTrapsAsZsh) {
  Shell sh;
  str killed = s::to_string(128 + SIGUSR1) + "\n";
  // `( … )`, `$( … )` and `&` reset ignored signals; a foreground pipeline
  // stage and a plain command keep them.
  EXPECT_EQ(out(sh, "trap '' USR1; ( sh -c 'kill -USR1 $$; echo alive' ); echo $?"), killed);
  EXPECT_EQ(out(sh, "trap '' USR1; x=$(sh -c 'kill -USR1 $$; echo alive'); echo $? $x"),
            killed);
  EXPECT_EQ(out(sh, "trap '' USR1; sh -c 'kill -USR1 $$; echo alive' & wait $!; echo $?"),
            killed);
  EXPECT_EQ(out(sh, "trap '' USR1; sh -c 'kill -USR1 $$; echo alive' | cat; echo $?"),
            "alive\n0\n");
  EXPECT_EQ(out(sh, "trap '' USR1; sh -c 'kill -USR1 $$; echo alive'; echo $?"), "alive\n0\n");
  // POSIX_TRAPS keeps them.
  EXPECT_EQ(out(sh, "trap '' USR1; setopt posixtraps; ( sh -c 'kill -USR1 $$; echo alive' ); "
                    "echo $?; unsetopt posixtraps"),
            "alive\n0\n");
  sh.run("trap - USR1");
  // A stage keeps string traps; a subshell keeps TRAPxxx functions, ZERR
  // and DEBUG.
  EXPECT_EQ(out(sh, "trap 'echo t' USR1; { sh -c 'kill -USR1 $PPID'; sleep 0.1; echo x } | cat; "
                    "trap - USR1"),
            "t\nx\n");
  EXPECT_EQ(out(sh, "TRAPUSR1() { echo t $1; }; ( sh -c 'kill -USR1 $PPID'; sleep 0.1; echo x ); "
                    "trap - USR1"),
            "t " + s::to_string(SIGUSR1) + "\nx\n");
  EXPECT_EQ(out(sh, "trap 'echo z' ZERR; x=$(false; echo in); echo \"[$x]\"; trap - ZERR"),
            "[z\nin]\n");
  // A background job without job control lists `trap '' INT QUIT`.
  EXPECT_EQ(out(sh, "trap 'echo t' USR1; { trap } & wait; trap - USR1"),
            "trap -- '' INT\ntrap -- '' QUIT\n");
}

TEST(TrapsM6Review, ChildrenRunTheirOwnExitTraps) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=$(trap 'echo e6' EXIT; exit 3); echo \"$x $?\""), "e6 3\n");
  EXPECT_EQ(out(sh, "cat <(trap 'echo e1' EXIT; echo a)"), "a\ne1\n");
  EXPECT_EQ(out(sh, "{ trap 'echo e4' EXIT; echo c } & wait"), "c\ne4\n");
  EXPECT_EQ(out(sh, "{ trap 'echo e5' EXIT; echo c } | cat"), "c\ne5\n");
}

TEST(TrapsM6Review, ABackgroundJobsOwnFailureSetsOffNoZerr) {
  Shell sh;
  EXPECT_EQ(out(sh, "trap 'echo z' ZERR; false & wait; true && false & wait; "
                    "true && { false; echo a } & wait; trap - ZERR"),
            "z\na\n");
}

TEST(TrapsM6Review, DebugTrapCommandTextAndAfterMode) {
  Shell sh;
  EXPECT_EQ(out(sh, "trap 'echo \"[$ZSH_DEBUG_CMD]\"' DEBUG; x=1 && echo b; trap - DEBUG; "
                    "echo ${+ZSH_DEBUG_CMD}"),
            "[x=1  && echo b]\nb\n[trap - DEBUG]\n0\n");
  EXPECT_EQ(out(sh, "unsetopt debugbeforecmd; trap 'echo \"[$?]\"' DEBUG; false; echo b; "
                    "trap - DEBUG; setopt debugbeforecmd"),
            "[0]\n[1]\nb\n[0]\n");
  // TRAPZERR and TRAPDEBUG get zsh's numbers for them.
  EXPECT_EQ(out(sh, "TRAPZERR() { echo $1; }; false; unfunction TRAPZERR"),
            s::to_string(SignalTable::zsh_number(SIGZERR)) + "\n");
}

TEST(TrapsM6Review, ATrapIsParsedOnce) {
  // A DEBUG trap was parsed again before each command: 100000 commands
  // took 0.12 s, now 0.07 (zsh 0.14).
  Shell sh;
  auto start = s::chrono::steady_clock::now();
  EXPECT_EQ(out(sh, "trap '(( c++ ))' DEBUG; for ((i=0;i<20000;i++)); do :; done; "
                    "trap - DEBUG; echo $c"),
            "20002\n");
  EXPECT_LT(s::chrono::steady_clock::now() - start, s::chrono::seconds(3));
}

TEST(TrapsM6Review, ASubshellStageResetsTraps) {
  // A `( … )` stage of a foreground pipeline is a subshell: its traps go.
  Shell sh;
  EXPECT_EQ(out(sh, "trap 'echo u' USR1; (trap) | cat; (trap) | cat | cat; (trap) |& cat; "
                    "{ (trap) } | cat; (trap) 2>/dev/null | cat; cat </dev/null | (trap); "
                    "trap - USR1"),
            "");
  // A brace stage keeps them.
  EXPECT_EQ(out(sh, "trap 'echo u' USR1; { trap } | cat; trap - USR1"),
            "trap -- 'echo u' USR1\n");
}

TEST(TrapsM6Review, PosixTrapsKeepsOnlyTrapIgnores) {
  // QUIT ignored at start is zsh's `trap '' QUIT`; under POSIX_TRAPS a
  // subshell keeps it and other `trap ''`, not other start-ignored ones.
  int status = 0;
  str killed = s::to_string(128 + SIGUSR1);
  EXPECT_EQ(run_binary("trap; setopt posixtraps; (sh -c 'kill -QUIT $$; echo alive-q'); "
                       "(sh -c 'kill -USR1 $$; echo alive-u'); echo $?; trap '' USR1; "
                       "(sh -c 'kill -USR1 $$; echo alive-u2')",
                       {SIGQUIT, SIGUSR1}, status),
            "trap -- '' QUIT\nalive-q\n" + killed + "\nalive-u2\n");
}

TEST(TrapsM6Review, AFunctionsTrapexitIsItsExitTrap) {
  Shell sh;
  EXPECT_EQ(out(sh, "TRAPEXIT() { echo te }; f() { TRAPEXIT() { echo fe }; }; f; "
                    "functions TRAPEXIT; echo after; unfunction TRAPEXIT"),
            "fe\nTRAPEXIT () {\n\techo te\n}\nafter\n");
  EXPECT_EQ(out(sh, "TRAPEXIT() { echo te }; f() { trap 'echo fs' EXIT; }; f; "
                    "functions TRAPEXIT; unfunction TRAPEXIT"),
            "fs\nTRAPEXIT () {\n\techo te\n}\n");
  auto r = sh.run("f() { TRAPEXIT() { echo fe } }; f; functions TRAPEXIT; echo after $?");
  EXPECT_EQ(r.out, "fe\nafter 1\n");
  // `trap - EXIT` in a function hides the outer TRAPEXIT only there.
  EXPECT_EQ(out(sh, "TRAPEXIT() { echo top }; f() { trap - EXIT; echo inf }; f; "
                    "functions TRAPEXIT; trap - EXIT"),
            "inf\nTRAPEXIT () {\n\techo top\n}\n");
  // Under LOCAL_TRAPS a TRAPxxx defined in the function goes with it.
  r = sh.run("f() { setopt localtraps; TRAPUSR1() { echo inner } }; f; functions TRAPUSR1; "
             "echo $?");
  EXPECT_EQ(r.out, "1\n");
  // A TRAPEXIT's `return N` is the function's `return N`.
  r = sh.run("f() { TRAPEXIT() { echo fe; return 3 }; true }; f; echo after $?");
  EXPECT_EQ(r.out, "fe\n");
  EXPECT_EQ(r.rc, 3);
}

TEST(TrapsM6Review, NoExitTrapRunsInsideATrap) {
  // zsh runs no EXIT trap while a trap runs, and a TRAPxxx that returns
  // non-zero still ends the line.
  Shell sh;
  auto r = sh.run("TRAPUSR1() { TRAPEXIT() { echo te }; echo tu; return 3 }; kill -USR1 $$; "
                  "echo next");
  EXPECT_EQ(r.out, "tu\n");
  EXPECT_TRUE(sh.ctx.exiting);
  Shell sh2;
  r = sh2.run("TRAPUSR1() { trap 'echo te' EXIT; echo tu }; kill -USR1 $$; echo next");
  EXPECT_EQ(r.out, "tu\nnext\n");
}

TEST(TrapsM6Review, AFailingTrapFunctionIsAnErrorWhereverItStrikes) {
  // zsh's trapreturn sets ERRFLAG_ERROR (an interrupt for INT and QUIT):
  // eval and $( … ) fail with 1, a sourced file ends just itself, a read
  // gives 1 (the trap's status for SIGINT), a loop keeps its status.
  const char* fn = "TRAPUSR1() { echo tu; return 3 }; ";
  for (auto [body, want_out, want_rc] : vec<s::tuple<str, str, int>>{
         {"eval 'kill -USR1 $$; echo ine'; echo next", "tu\n", 1},
         {"x=$(kill -USR1 $$; echo inner); echo n $x", "tu\n", 1},
         {"while true; do kill -USR1 $$; done; echo n", "tu\n", 0},
         {"(sleep 0.3; kill -USR1 $$) & read x < <(sleep 1); echo st $?", "tu\n", 1}}) {
    int status = 0;
    EXPECT_EQ(run_binary((str(fn) + body).c_str(), {}, status), want_out) << body;
    ASSERT_TRUE(WIFEXITED(status)) << body;
    EXPECT_EQ(WEXITSTATUS(status), want_rc) << body;
  }
  int status = 0;
  EXPECT_EQ(run_binary("TRAPINT() { echo ti; return 3 }; (sleep 0.3; kill -INT $$) & "
                       "read x < <(sleep 1); echo st $?", {}, status),
            "ti\n");
  EXPECT_EQ(WEXITSTATUS(status), 3);
  // A sourced file ends; the script goes on.
  char tmpl[] = "/tmp/jfsh_src_XXXXXX";
  int fd = ::mkstemp(tmpl);
  ASSERT_GE(fd, 0);
  str body = "kill -USR1 $$; echo insrc\n";
  ASSERT_EQ(::write(fd, body.data(), body.size()), static_cast<ssize_t>(body.size()));
  ::close(fd);
  EXPECT_EQ(run_binary((str(fn) + ". " + tmpl + "; echo n").c_str(), {}, status), "tu\nn\n");
  EXPECT_EQ(WEXITSTATUS(status), 0);
  ::unlink(tmpl);
  // A command trap's `return` in a function interrupted in `read`: read's 1.
  EXPECT_EQ(run_binary("trap 'echo ti; return 3' INT; f() { (sleep 0.3; kill -INT $$) & "
                       "read x < <(sleep 1); echo st $? }; f; echo after $?", {}, status),
            "ti\nafter 1\n");
}

TEST(TrapsM6Review, ASignalWhileItsTrapRunsIsHandledAsZsh) {
  // From a builtin in the trap: once it ends.  From elsewhere: dropped,
  // though it still cuts a `wait` short.
  Shell sh;
  EXPECT_EQ(out(sh, "trap 'echo u; sleep 0.4; echo done' USR1; (sleep 0.1; kill -USR1 $$) & "
                    "kill -USR1 $$; wait; echo next; trap - USR1"),
            "u\ndone\nnext\n");
  EXPECT_EQ(out(sh, "trap 'echo u; (sleep 0.1; kill -USR1 $$) & wait; echo w $?' USR1; "
                    "kill -USR1 $$; echo next; trap - USR1"),
            "u\nw " + s::to_string(128 + SIGUSR1) + "\nnext\n");
  // A subshell forked by the trap ignores the signal; a pipeline stage and
  // an external command don't.
  EXPECT_EQ(out(sh, "trap 'echo u; (sh -c \"kill -USR1 \\$\\$; echo alive\")' USR1; "
                    "kill -USR1 $$; echo next; trap - USR1"),
            "u\nalive\nnext\n");
  EXPECT_EQ(out(sh, "trap 'echo u; sh -c \"kill -USR1 \\$\\$; echo alive\" | cat' USR1; "
                    "kill -USR1 $$; echo next; trap - USR1"),
            "u\nnext\n");
  // `trap ''` and a new trap inside take effect after it.
  EXPECT_EQ(out(sh, "trap 'echo u; trap \"echo v\" USR1; kill -USR1 $$; echo x' USR1; "
                    "kill -USR1 $$; echo st; trap - USR1"),
            "u\nx\nv\nst\n");
  // `exec` in the trap: the program doesn't inherit anything blocked.
  int status = 0;
  EXPECT_EQ(run_binary("trap 'echo u; exec /bin/sh -c \"kill -USR1 \\$\\$; echo alive\"' USR1; "
                       "kill -USR1 $$", {}, status),
            "u\n");
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGUSR1);
}

TEST(TrapsM6Review, TrappingASignalForgetsItWasIgnoredAtStart) {
  int status = 0;
  EXPECT_EQ(run_binary("trap 'echo h' TERM; trap - TERM; kill -TERM $$; echo alive",
                       {SIGQUIT, SIGTERM}, status),
            "");
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGTERM);
  EXPECT_EQ(run_binary("trap - QUIT; trap; sh -c 'kill -QUIT $$; echo alive'; echo $?",
                       {SIGQUIT}, status),
            s::to_string(128 + SIGQUIT) + "\n");
}

TEST(TrapsM6Review, ProcessSubstitutionIgnoresIntAndQuitWithoutJobControl) {
  Shell sh;
  EXPECT_EQ(out(sh, "cat <(trap)"), "trap -- '' INT\ntrap -- '' QUIT\n");
}

TEST(JobsM6Review, AHangupEndsAWaitAtOnce) {
  int status = 0;
  EXPECT_EQ(run_binary("( (sleep 1; echo inner) ; echo after ) & sleep 0.3; kill -HUP $!; "
                       "wait $!; echo $?; sleep 1", {}, status),
            "1\ninner\n");
}

TEST(TrapsM6Review, ASignalFromItsOwnTrapIsHandledWhenTheTrapEnds) {
  Shell sh;
  EXPECT_EQ(out(sh, "n=0; trap 'n=$((n+1)); echo u$n; ((n<4)) && kill -USR1 $$; echo end$n' "
                    "USR1; kill -USR1 $$; echo st $?; trap - USR1"),
            "u1\nend1\nu2\nend2\nu3\nend3\nu4\nend4\nst 0\n");
  // It waits for the trap, even after `trap - USR1` in it (zsh queues it).
  int status = 0;
  EXPECT_EQ(run_binary("trap 'echo u; trap - USR1; kill -USR1 $$; echo x' USR1; "
                       "kill -USR1 $$; echo st", {}, status),
            "u\nx\n");
  ASSERT_TRUE(WIFSIGNALED(status));
  EXPECT_EQ(WTERMSIG(status), SIGUSR1);
}

TEST(TrapsM6Review, ArrayAssignmentsAreWrittenAsZsh) {
  Shell sh;
  EXPECT_EQ(out(sh, "trap 'x=( a  b )' USR1; trap; trap - USR1"), "trap -- 'x=(a b) ' USR1\n");
  EXPECT_EQ(out(sh, "f() { v=( 'a  b'   \"c  d\" $(echo  q) ) }; functions f"),
            "f () {\n\tv=('a  b' \"c  d\" $(echo  q)) \n}\n");
  EXPECT_EQ(out(sh, "f() { x=( <(echo  a) =(echo  b) $[1 +  2]   c ) }; functions f"),
            "f () {\n\tx=(<(echo  a) =(echo  b) $[1 +  2] c) \n}\n");
}

TEST(TrapsM6Review, TrapFailuresQueueingAndExitTrapsAsZshC) {
  // Expected output and status from zsh 5.9 -f -c.  A TRAPxxx that
  // returns non-zero sets errflag, retflag, breaks and lastval as zsh's
  // dotrapargs; a signal is queued while its trap runs and dropped where
  // the shell waits for a child.
  struct Case {
    const char* script;
    const char* out;
    int         rc;
  };
  const Case cases[] = {
    {"TRAPUSR1() { return 3 }; echo a $(kill -USR1 $$; echo 4) b",
     "", 1},
    {"TRAPUSR1() { return 3 }; echo $(( $(kill -USR1 $$; echo 4) + 1 ))",
     "", 1},
    {"TRAPUSR1() { return 3 }; /bin/echo $(kill -USR1 $$; echo 4) ext",
     "", 1},
    {"TRAPUSR1() { return 3 }; echo `kill -USR1 $$; echo 4` bq",
     "", 1},
    {"TRAPUSR1() { return 3 }; echo ${$(kill -USR1 $$; echo 4)}",
     "", 1},
    {"TRAPUSR1() { return 3 }; : $(kill -USR1 $$; echo 4); echo after",
     "", 1},
    {"TRAPUSR1() { return 1 }; trap 'kill -USR1 $$; echo in' USR2; kill -USR2 $$; echo aft"
     "er $?; eval true; echo ev $?",
     "after 0\nev 0\n", 0},
    {"TRAPUSR1() { return 1 }; TRAPUSR2() { kill -USR1 $$; echo in }; kill -USR2 $$; echo "
     "after $?; eval true; echo ev $?",
     "after 0\nev 0\n", 0},
    {"TRAPUSR1() { return 1 }; { kill -USR1 $$ } always { TRY_BLOCK_ERROR=0 }; echo n; eva"
     "l true; echo ev $?; read -t 0 x </dev/null; echo rd $?",
     "", 1},
    {"TRAPUSR1() { return 7 }\nf() { eval \"kill -USR1 \\$\\$\"; echo st $?; }; f; echo f "
     "$?\necho after $?",
     "f 1\nafter 0\n", 0},
    {"TRAPUSR1() { return 7 }\ng() { f; echo g $?; }; f() { eval \"kill -USR1 \\$\\$\"; ec"
     "ho st $?; }; g; echo after1 $?\necho after $?",
     "g 1\nafter1 0\nafter 0\n", 0},
    {"TRAPUSR1() { return 7 }\nf() { for i in 1 2; do eval \"kill -USR1 \\$\\$\"; echo in;"
     " done; echo f; }; f; echo after $?",
     "after 1\n", 0},
    {"TRAPUSR1() { return 7 }\nf() { { eval \"kill -USR1 \\$\\$\"; echo in } always { echo"
     " al }; echo f }; f; echo after $?",
     "al\nafter 1\n", 0},
    {"TRAPUSR1() { return 7 }\neval 'kill -USR1 $$'; echo st $?\necho after $?",
     "", 1},
    {"TRAPUSR1() { return 7 }; (sleep 0.2; kill -USR1 $$) & { sleep 0.5 }; echo st $?",
     "", 7},
    {"TRAPUSR1() { return 7 }; (sleep 0.2; kill -USR1 $$) & if true; then sleep 0.5; fi; e"
     "cho st",
     "", 7},
    {"TRAPUSR1() { return 7 }; (sleep 0.2; kill -USR1 $$) & case a in a) sleep 0.5;; esac;"
     " echo st",
     "", 7},
    {"TRAPUSR1() { return 7 }; (sleep 0.2; kill -USR1 $$) & while true; do sleep 0.5; done",
     "", 7},
    {"TRAPUSR1() { return 7 }; (sleep 0.2; kill -USR1 $$) & for i in 1 2 3 4 5 6 7; do sle"
     "ep 0.1; done",
     "", 7},
    {"TRAPUSR1() { return 7 }; (sleep 0.2; kill -USR1 $$) & repeat 3 sleep 0.2",
     "", 7},
    {"TRAPUSR1() { return 7 }; (sleep 0.2; kill -USR1 $$) & f() { sleep 0.5; echo in }; f",
     "", 7},
    {"TRAPINT() { return 7 }; (sleep 0.2; kill -INT $$) & { sleep 0.5 }",
     "", 7},
    {"TRAPQUIT() { return 5 }; kill -QUIT $$; echo st $?",
     "", 5},
    {"TRAPQUIT() { return 5 }; f() { kill -QUIT $$; echo in }; f",
     "", 5},
    {"TRAPQUIT() { return 5 }; for i in 1 2; do kill -QUIT $$; done",
     "", 5},
    {"TRAPUSR1() { return 3 }; setopt errexit; kill -USR1 $$; echo st $?",
     "", 3},
    {"TRAPUSR1() { return 3 }; setopt errexit; f() { kill -USR1 $$; echo in }; f",
     "", 3},
    {"trap 'return 4' USR1; setopt errexit; f() { kill -USR1 $$; echo in }; f",
     "", 4},
    {"g() { trap \"echo gexit\" EXIT; }; f() { trap \"g; echo fexit\" EXIT; }; f; echo end",
     "fexit\nend\n", 0},
    {"trap 'echo u; (trap)' USR1; kill -USR1 $$",
     "u\n", 0},
    {"TRAPUSR1() { (trap; echo in) }; kill -USR1 $$",
     "TRAPUSR1 () {\n\t(\n\t\ttrap\n\t\techo in\n\t)\n}\nin\n", 0},
    {"cat <(trap); cat > >(trap) </dev/null; sleep 0.1",
     "trap -- '' INT\ntrap -- '' QUIT\ntrap -- '' INT\ntrap -- '' QUIT\n", 0},
    {"exec 3< <(sleep 0.6; echo l1; sleep 0.6; echo l2)\ntrap 'echo t; read x <&3; echo go"
     "t $? $x' USR1\n(sleep 0.1; kill -USR1 $$; sleep 0.2; kill -USR1 $$) &\nsleep 2; echo"
     " end",
     "t\ngot 0 l1\nt\ngot 0 l2\nend\n", 0},
    {"trap 'echo t; for i in {1..300000}; do :; done; echo done' USR1\n(sleep 0.1; kill -U"
     "SR1 $$; sleep 0.1; kill -USR1 $$) &\nwait; echo end",
     "t\ndone\nt\ndone\nend\n", 0},
    {"trap 'echo t; sleep 0.4; echo done' USR1\n(sleep 0.1; kill -USR1 $$; sleep 0.1; kill"
     " -USR1 $$) &\nsleep 1; echo end",
     "t\ndone\nend\n", 0},
    {"trap 'echo u; read x < <(sleep 0.4; echo r); echo got $x' USR1; (sleep 0.1; kill -US"
     "R1 $$) & kill -USR1 $$; wait; echo next",
     "u\ngot r\nnext\n", 0},
    // Round 5.
    {"TRAPUSR1() { return 3 }; trap 'eval \"readonly r=1; r=2; echo in\" 2>/dev/null; echo"
     " ev $?' EXIT; kill -USR1 $$",
     "", 0},
    {"TRAPINT() { return 5 }; echo $(kill -INT $$; echo 4); echo st $?",
     "", 1},
    {"TRAPINT() { return 5 }; x=$(kill -INT $$); echo st $?",
     "", 2},
    {"TRAPQUIT() { return 5 }; x=`kill -QUIT $$`; echo st $?",
     "", 2},
    {"TRAPUSR1() { return 3 }; (( $(kill -USR1 $$; echo 4) > 1 )); echo st $?",
     "", 2},
    {"trap '( trap \"echo inner\" USR1; sh -c \"kill -USR1 \\$PPID\"; echo subalive )' USR"
     "1; kill -USR1 $$",
     "inner\nsubalive\n", 0},
    {"trap 'return 3' USR1; echo a $(kill -USR1 $$; echo 4) b",
     "", 3},
    {"trap 'return 3' USR1; f() { x=$(kill -USR1 $$; echo 4); echo inf $? $x; }; f; echo s"
     "t $?",
     "st 0\n", 0},
    {"trap 'return 3' USR1; f() { echo a $(kill -USR1 $$; echo 4) b; echo inf; }; f; echo "
     "st $?",
     "a 4 b\nst 0\n", 0},
    {"trap \"false; echo t\" DEBUG; setopt errexit; true; echo st $?",
     "t\nt\nt\nst 0\n", 0},
    {"TRAPDEBUG() { return 5 }; true; echo st $?",
     "", 5},
    {"TRAPZERR() { return 6 }; false; echo st $?",
     "", 6},
    {"trap 'echo e1; f() { echo inf; exit 3; }; f; echo e2' EXIT; echo main",
     "main\ne1\ninf\ne2\n", 3},
    {"trap 'setopt errexit' DEBUG; false; echo after",
     "", 0},
    {"trap '[[ $ZSH_DEBUG_CMD == *skip* ]] && setopt errexit' DEBUG; echo a; echo skip; ec"
     "ho b",
     "a\nb\n", 0},
    {"trap 'echo u; exit 5' USR1; trap 'echo ex; kill -USR1 $$; echo exb' EXIT; echo hi; e"
     "xit 2",
     "hi\nex\nu\nexb\n", 5},
    {"trap 'echo \"ex\"' EXIT; readonly r=1; r=2 2>/dev/null",
     "", 1},
    {"trap 'echo u; (sh -c \"kill -USR1 \\$PPID\"; echo sub)' USR1; kill -USR1 $$; echo ne"
     "xt",
     "u\nnext\n", 0},
    {"trap 'echo u; (trap \"\" USR1; sh -c \"kill -USR1 \\$PPID\"; echo sub)' USR1; kill -"
     "USR1 $$; echo next",
     "u\nsub\nnext\n", 0},
    {"trap 'echo u; (sh -c \"kill -USR1 \\$PPID\"; echo sub) | cat' USR1; kill -USR1 $$; e"
     "cho next",
     "u\nnext\n", 0},
    // Round 6.
    {"{ x=$( trap \"echo e\" EXIT; echo $((1/0)) ); echo st $? x=$x; } 2>/dev/null",
     "st 1 x=\n", 0},
    {"{ x=$( trap \"echo e\" EXIT; false ); echo st $? x=$x; } 2>/dev/null",
     "st 1 x=e\n", 0},
    {"{ x=$( trap \"echo e\" EXIT; echo ${x?} ); echo st $? x=$x; } 2>/dev/null",
     "st 1 x=\n", 0},
    {"{ ( trap \"echo e\" EXIT; echo $((1/0)) ); echo st $?; } 2>/dev/null",
     "st 1\n", 0},
    {"{ { trap \"echo e\" EXIT; echo $((1/0)); } | cat; echo st; } 2>/dev/null",
     "st\n", 0},
    {"{ cat <( trap \"echo e\" EXIT; echo $((1/0)) ); echo st $?; } 2>/dev/null",
     "st 0\n", 0},
    {"{ TRAPUSR1() { return 3 }; typeset x=$(kill -USR1 $$; echo 4); echo st $? x=$x; } 2>"
     "/dev/null",
     "", 0},
    {"{ TRAPUSR1() { return 3 }; f() { local y=$(kill -USR1 $$; echo 4); echo in $? y=$y; "
     "}; f; echo st $?; } 2>/dev/null",
     "", 0},
    {"{ TRAPUSR1() { return 3 }; false; [[ $(kill -USR1 $$) = \"\" ]]; echo st $?; } 2>/de"
     "v/null",
     "", 0},
    {"{ TRAPUSR1() { return 3 }; case $(kill -USR1 $$; exit 7) in *) echo in;; esac; echo "
     "st $?; } 2>/dev/null",
     "", 0},
    {"{ TRAPUSR1() { return 3 }; false; x=$(kill -USR1 $$; echo 4); echo st $?; } 2>/dev/n"
     "ull",
     "", 1},
    {"{ TRAPUSR1() { return 3 }; trap 'echo ex $?' EXIT; (( $(kill -USR1 $$; echo 4) )); e"
     "cho st $?; } 2>/dev/null",
     "ex 2\n", 2},
    {"{ TRAPUSR1() { return 3 }; f() { (( $(kill -USR1 $$; echo 4) )); echo in $?; }; f; e"
     "cho st $?; } 2>/dev/null",
     "st 2\n", 0},
    {"{ trap 'return 3' USR1; f() { for i in $(kill -USR1 $$; exit 7); do echo in $i; done"
     "; echo f2; }; f; echo st $?; } 2>/dev/null",
     "st 0\n", 0},
    {"{ trap 'return 3' USR1; f() { case $(kill -USR1 $$; echo 4) in *) echo c;; esac; ech"
     "o f2; }; f; echo st $?; } 2>/dev/null",
     "st 0\n", 0},
    {"{ trap 'return 3' USR1; f() { [[ $(kill -USR1 $$; exit 7) = 4 ]]; echo f2 $?; }; f; "
     "echo st $?; } 2>/dev/null",
     "st 1\n", 0},
    {"{ trap 'return 3' USR1; f() { (( $(kill -USR1 $$; echo 4) )); echo f2 $?; }; f; echo"
     " st $?; } 2>/dev/null",
     "st 0\n", 0},
    {"{ trap 'return 3' USR1; g() { echo g; }; f() { g $(kill -USR1 $$; exit 7); echo inf "
     "$?; }; f; echo st $?; } 2>/dev/null",
     "inf 7\nst 0\n", 0},
    {"{ trap 'echo exit-start; g; echo exit-end $?' EXIT; g() { echo g; exit 4; }; } 2>/de"
     "v/null",
     "exit-start\ng\nexit-end 0\n", 4},
    {"{ trap 'echo exit-start; g; echo exit-end $?' EXIT; h() { echo h; exit 5; echo noth;"
     " }; g() { h; echo afterh $?; }; } 2>/dev/null",
     "exit-start\nh\nafterh 0\nexit-end 0\n", 5},
    {"{ TRAPEXIT() { echo te; g; echo te-end $?; }; g() { echo g; exit 4; }; } 2>/dev/null",
     "te\ng\nte-end 0\n", 4},
    {"{ trap 'echo u1; exit 5' USR1; trap 'echo ex; kill -USR1 $$; echo exb' EXIT; echo hi"
     "; } 2>/dev/null",
     "hi\nex\nu1\n", 5},
    {"{ trap 'echo gexit' EXIT; f() { echo ${nope?u}; }; f; } 2>/dev/null",
     "", 1},
    {"{ trap 'echo u; (sh -c \"kill -USR1 \\$PPID\"; echo sub)' USR1; kill -USR1 $$; echo "
     "next; } 2>/dev/null",
     "u\nnext\n", 0},
    // Round 7.
    {"{ trap 'return 5' USR1; f() { repeat $(kill -USR1 $$; echo 2) echo r; echo in $?; };"
     " f; echo st $?; } 2>/dev/null",
     "st 0\n", 0},
    {"{ trap 'return 5' USR1; f() { for (( i=$(kill -USR1 $$; echo 0); i<2; i++ )); do ech"
     "o r; done; echo in $?; }; f; echo st $?; } 2>/dev/null",
     "st 0\n", 0},
    {"{ trap 'return 5' USR1; f() { select x in $(kill -USR1 $$; echo a); do echo r; break"
     "; done </dev/null; echo in $?; }; f; echo st $?; } 2>/dev/null",
     "st 0\n", 0},
    {"{ g() { echo g; exit 4 }; trap 'echo t; x=$(g); echo after $? $x' EXIT; true; } 2>/d"
     "ev/null",
     "t\nafter 4 g\n", 0},
    {"{ g() { echo g; exit 4 }; trap 'echo t; g & wait $!; echo after $?' EXIT; true; } 2>"
     "/dev/null",
     "t\ng\nafter 4\n", 0},
    {"{ g() { echo g; exit 4 }; trap 'echo t; (g; echo sub $?); echo after $?' EXIT; true;"
     " } 2>/dev/null",
     "t\ng\nsub 0\nafter 4\n", 0},
    {"{ g() { echo g; exit 4 }; trap 'echo t; x=$(g; echo no); echo after $? $x' EXIT; tru"
     "e; } 2>/dev/null",
     "t\nafter 4 g\nno\n", 0},
    {"{ TRAPUSR1() { echo u; exit 5 }; trap 'echo ex; kill -USR1 $$; echo exb' EXIT; echo "
     "hi; } 2>/dev/null",
     "hi\nex\nu\n", 5},
    {"{ g() { echo g; exit 4 }; trap 'g; echo more $?' USR1; trap 'echo ex; kill -USR1 $$;"
     " echo exb $?' EXIT; echo hi; } 2>/dev/null",
     "hi\nex\ng\nmore 0\n", 4},
    {"{ TRAPUSR1() { return 3 }; [[ -n $(kill -USR1 $$) ]]; echo st $?; } 2>/dev/null",
     "", 1},
    {"{ TRAPUSR1() { return 3 }; [[ x = $(kill -USR1 $$) ]]; echo st $?; } 2>/dev/null",
     "", 1},
    {"{ TRAPUSR1() { return 3 }; [[ $(kill -USR1 $$; echo a) = a ]]; echo st $?; } 2>/dev/"
     "null",
     "", 1},
    {"{ TRAPUSR1() { return 3 }; [[ -z $(kill -USR1 $$) ]]; echo st $?; } 2>/dev/null",
     "", 0},
    {"{ TRAPUSR1() { return 3 }; f() { [[ -n $(kill -USR1 $$) ]]; echo in; }; f; echo st $"
     "?; } 2>/dev/null",
     "", 1},
    {"{ TRAPUSR1() { return 3 }; typeset x=$(kill -USR1 $$; false); echo st $?; } 2>/dev/n"
     "ull",
     "", 0},
    {"{ TRAPUSR1() { return 3 }; f() { local y=$(kill -USR1 $$; (exit 6)); echo in; }; f; "
     "echo st $?; } 2>/dev/null",
     "", 0},
    {"{ TRAPUSR1() { return 3 }; readonly x=$(kill -USR1 $$; false); echo st $?; } 2>/dev/"
     "null",
     "", 0},
    {"{ TRAPUSR1() { return 3 }; export x=$(kill -USR1 $$; sh -c \"exit 6\"); echo st $?; "
     "} 2>/dev/null",
     "", 0},
    {"{ ( trap \"echo e\" EXIT; { echo $((1/0)) } always { echo al } ); echo st $?; } 2>/d"
     "ev/null",
     "al\ne\nst 1\n", 0},
    {"{ x=$( trap \"echo e\" EXIT; { echo $((1/0)) } always { echo al } ); echo st $? x=$x"
     "; } 2>/dev/null",
     "st 1 x=al\ne\n", 0},
    {"{ ( trap \"echo e\" EXIT; { echo $((1/0)) } always { echo al }; echo n ); echo st $?"
     "; } 2>/dev/null",
     "al\nst 1\n", 0},
    {"{ ( trap \"echo e\" EXIT; { { echo $((1/0)) } always { echo al } } ); echo st $?; } "
     "2>/dev/null",
     "al\ne\nst 1\n", 0},
    {"{ ( trap \"echo e\" EXIT; echo $((1/0)) ); echo st $?; } 2>/dev/null",
     "st 1\n", 0},
    // Round 8.
    {"{ TRAPUSR1() { echo u; exit 5 }; f() { kill -USR1 $$; echo fb }; trap 'echo ex; f; e"
     "cho exb' EXIT; echo hi; } 2>/dev/null",
     "hi\nex\nu\n", 5},
    {"{ TRAPUSR1() { echo u; exit 5 }; f() { kill -USR1 $$; echo fb }; trap 'echo ex; f; e"
     "cho exb' EXIT; echo hi; false; } 2>/dev/null",
     "hi\nex\nu\n", 5},
    {"{ TRAPUSR1() { echo u; exit 5 }; h() { kill -USR1 $$; echo hb }; f() { h; echo fb };"
     " trap 'echo ex; f; echo exb' EXIT; echo hi; } 2>/dev/null",
     "hi\nex\nu\n", 5},
    {"{ TRAPUSR1() { echo u; exit 5 }; f() { kill -USR1 $$; echo fb }; TRAPEXIT() { echo e"
     "x; f; echo exb }; echo hi; } 2>/dev/null",
     "hi\nex\nu\n", 5},
    {"{ TRAPUSR1() { return 3 }; [[ 5 -eq $(kill -USR1 $$; echo 5) ]]; echo st $?; } 2>/de"
     "v/null",
     "", 0},
    {"{ TRAPUSR1() { return 3 }; [[ 5 -ne $(kill -USR1 $$; echo 5) ]]; echo st $?; } 2>/de"
     "v/null",
     "", 1},
    {"{ TRAPUSR1() { return 3 }; [[ 1 -lt $(kill -USR1 $$; echo 5) ]]; echo st $?; } 2>/de"
     "v/null",
     "", 1},
    {"{ TRAPUSR1() { return 3 }; [[ 1 -ge $(kill -USR1 $$; echo 5) ]]; echo st $?; } 2>/de"
     "v/null",
     "", 0},
    {"{ TRAPUSR1() { return 3 }; f() { [[ 5 -eq $(kill -USR1 $$; echo 5) ]]; echo in; }; f"
     "; echo st $?; } 2>/dev/null",
     "", 0},
    {"{ TRAPUSR1() { return 3 }; f() { [[ 2 -gt $(kill -USR1 $$; echo 1) ]]; echo in; }; f"
     "; echo st $?; } 2>/dev/null",
     "", 1},
    // M7 review: `||` and `&&` go on after a failed TRAPxxx, the words
    // after it taken as written (zsh's evalcond has no errflag check).
    {"TRAPUSR1() { return 3 }; [[ 1 -lt $(kill -USR1 $$; echo 5) || 1 ]]; echo st $?",
     "", 0},
    {"TRAPUSR1() { return 3 }; [[ $(kill -USR1 $$; echo 1) == 2 || 1 ]]; echo st $?",
     "", 0},
    {"TRAPUSR1() { return 3 }; x=lit; [[ -n $(kill -USR1 $$; echo 1) || $x == lit ]]",
     "", 1},
    {"TRAPUSR1() { return 3 }; x=lit; [[ -n $(kill -USR1 $$; echo 1) || $x == '$x' ]]",
     "", 0},
    {"TRAPUSR1() { return 3 }; [[ $(kill -USR1 $$; echo 1) == '' && '' ]]",
     "", 0},
    {"TRAPUSR1() { return 3 }; [[ $(kill -USR1 $$; echo 1) == '' && \"*\" == '\"*\"' ]]",
     "", 0},
    {"{ TRAPUSR1() { return 3 }; [[ -n $(kill -USR1 $$) || -n $(echo side >&2; echo y) ]];"
     " } 2>&1",
     "", 0},
    {"TRAPUSR1() { return 3 }; f() { [[ $(kill -USR1 $$; echo 1) == '' && 1 ]]; echo in $?;"
     " }; f; echo out $?",
     "", 0},
    {"TRAPUSR1() { return 3 }; f() { [[ 1 -lt $(kill -USR1 $$; echo 5) || 1 -eq 2 ]]; echo "
     "in $?; }; f; echo out $?",
     "", 0},
    // M7 review round 2: under errflag zsh's patmatch matches no glob operator, so
    // only a plain string can be equal.
    {"TRAPUSR1() { return 3 }; [[ $(kill -USR1 $$; echo 1) == 2 || ab == a* ]]",
     "", 1},
    {"TRAPUSR1() { return 3 }; [[ $(kill -USR1 $$; echo 1) == 2 || ab == [a]b ]]",
     "", 1},
    {"TRAPUSR1() { return 3 }; [[ $(kill -USR1 $$; echo 1) == 2 || a == ? ]]",
     "", 1},
    {"TRAPUSR1() { return 3 }; [[ $(kill -USR1 $$; echo 1) == 2 || ab == \"a\"* ]]",
     "", 1},
    {"TRAPUSR1() { return 3 }; [[ $(kill -USR1 $$; echo 1) == 2 || ab != a* ]]",
     "", 0},
    {"TRAPUSR1() { return 3 }; [[ $(kill -USR1 $$; echo 1) == 2 || ab == ab ]]",
     "", 0},
    {"TRAPUSR1() { return 3 }; [[ $(kill -USR1 $$; echo 1) == 2 || ab != ab ]]",
     "", 1},
    {"TRAPUSR1() { return 3 }; [[ $(kill -USR1 $$; echo 1) == 2 || ab == a(b|c) ]]",
     "", 1},
  };
  for (const auto& c : cases) {
    int status = 0;
    EXPECT_EQ(run_binary(c.script, {}, status), c.out) << c.script;
    ASSERT_TRUE(WIFEXITED(status)) << c.script;
    EXPECT_EQ(WEXITSTATUS(status), c.rc) << c.script;
  }
}

TEST(TrapsM6Review, AnEvalErrorInTheExitTrapAfterATrapFailure) {
  // A script: the EXIT trap runs, and its eval's error ends just the eval
  // (zsh 5.9: `ev 1`, status 1).  A -c string runs no EXIT trap there.
  char tmpl[] = "/tmp/jfsh_st3_XXXXXX";
  int fd = ::mkstemp(tmpl);
  ASSERT_GE(fd, 0);
  str body = "TRAPUSR1() { return 3 }\n"
             "trap 'eval \"readonly r=1; r=2; echo in\" 2>/dev/null; echo ev $?' EXIT\n"
             "kill -USR1 $$\n";
  ASSERT_EQ(::write(fd, body.data(), body.size()), static_cast<ssize_t>(body.size()));
  ::close(fd);
  int status = 0;
  EXPECT_EQ(run_binary(tmpl, {}, status, true), "ev 1\n");
  ASSERT_TRUE(WIFEXITED(status));
  EXPECT_EQ(WEXITSTATUS(status), 1);
  ::unlink(tmpl);
}

}  // namespace
// M7 review round 3: a script read from standard input that an error
// ended with status 0 exits 1, as zsh's main loop (zsh 5.9: `printf
// 'print -v 1x a\necho next\n' | zsh -f` exits 1 without `next`).
TEST(BuiltinsM7Review, StdinScriptEndedByAnErrorExitsOne) {
  str jfsh = JFSH_BINARY;
  for (const char* body : {"print -v 1x a\\necho next\\n",
                           "TRAPUSR1() { return 3 }\\nkill -USR1 $$\\necho next\\n"}) {
    str script = "printf '" + str(body) + "' | " + jfsh + " -f 2>/dev/null; echo st $?";
    int status = 0;
    EXPECT_EQ(run_binary(script.c_str(), {}, status), "st 1\n") << body;
  }
}

// M7 review round 4: a script read from standard input is read unbuffered,
// a line at a time (zsh's shingetline), so what it runs reads the lines
// after; $LINENO counts on; a `return` at the top ends it (zsh 5.9, from a
// pipe and from a file).
TEST(BuiltinsM7Review, StdinScriptIsReadALineAtATime) {
  str jfsh = JFSH_BINARY;
  struct Case {
    const char* body;
    const char* out;        // from a pipe
    const char* file_out;   // from a file, where `head` reads a block
  };
  const Case cases[] = {
    {"read x\\nhello there\\necho got $x\\n", "got hello there\nst 0\n",
     "got hello there\nst 0\n"},
    {"head -1\\nskipped\\necho after\\n", "skipped\nst 0\n", "skipped\nafter\nst 0\n"},
    {"echo $LINENO\\necho $LINENO\\n", "1\n2\nst 0\n", "1\n2\nst 0\n"},
    {"return 4\\necho after\\n", "st 4\n", "st 4\n"},
  };
  for (const auto& c : cases) {
    str body = c.body;
    str pipe = "printf '%b' '" + body + "' | " + jfsh + " -f; echo st $?";
    str file = "f=$(mktemp); printf '%b' '" + body + "' > $f; " + jfsh
             + " -f < $f; echo st $?; rm -f $f";
    int status = 0;
    EXPECT_EQ(run_binary(pipe.c_str(), {}, status), c.out) << pipe;
    EXPECT_EQ(run_binary(file.c_str(), {}, status), c.file_out) << file;
  }
}

}  // namespace jfsh
