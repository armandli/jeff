// M8: invocation, startup files, the hooks of the interactive loop, sched,
// mail and the special parameters.  Where zsh's behaviour is the reference,
// it is zsh 5.9's (`zsh -f`), with "jfsh" for "zsh".

#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#if defined(__APPLE__)
#include <util.h>
#else
#include <pty.h>
#endif

#include <ctime>
#include <filesystem>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <shell_fixture.h>

#include <jfsh/exec/invocation.h>
#include <jfsh/exec/loop.h>
#include <jfsh/history/history.h>

namespace jfsh {
namespace {

using test::Run;
using test::Shell;
using test::TempDir;
namespace sf = s::filesystem;

str out(Shell& sh, strv src) { return sh.run(src).out; }

Invocation parse(vec<str> argv) {
  auto r = parse_invocation(argv);
  EXPECT_TRUE(r.has_value()) << (r ? "" : r.error().message);
  return r ? *r : Invocation{};
}

str parse_error(vec<str> argv) {
  auto r = parse_invocation(argv);
  return r ? "<ok>" : r.error().message;
}

// ---- invocation -----------------------------------------------------------------

TEST(Invocation, CommandTakesArg0AndPositionals) {
  auto inv = parse({"jfsh", "-c", "echo $0 $1", "name", "a", "b"});
  ASSERT_TRUE(inv.command);
  EXPECT_EQ(*inv.command, "echo $0 $1");
  EXPECT_EQ(inv.arg0, "name");
  EXPECT_EQ(inv.positional, (vec<str>{"a", "b"}));
  EXPECT_FALSE(inv.shinstdin);

  inv = parse({"jfsh", "-c", "true"});
  EXPECT_EQ(inv.arg0, "jfsh");
  EXPECT_TRUE(inv.positional.empty());
  // -c only marks the first argument after the options as the command.
  inv = parse({"jfsh", "-c", "-x", "true"});
  EXPECT_EQ(*inv.command, "true");
  EXPECT_EQ(inv.options, (vec<s::pair<OptIndex, bool>>{{OPT_XTRACE, true}}));
  inv = parse({"jfsh", "-xec", "true"});
  EXPECT_EQ(*inv.command, "true");
  EXPECT_EQ(inv.options.size(), 2u);
}

TEST(Invocation, ScriptAndStdin) {
  auto inv = parse({"jfsh", "s.sh", "a", "-x"});
  EXPECT_EQ(inv.script, "s.sh");
  EXPECT_EQ(inv.arg0, "jfsh");   // $0 becomes the script after the startup files
  EXPECT_EQ(inv.positional, (vec<str>{"a", "-x"}));   // options end at the script
  EXPECT_TRUE(inv.options.empty());

  inv = parse({"jfsh", "-s", "a", "b"});
  EXPECT_TRUE(inv.shinstdin);
  EXPECT_FALSE(inv.script);
  EXPECT_EQ(inv.positional, (vec<str>{"a", "b"}));

  inv = parse({"jfsh"});
  EXPECT_TRUE(inv.shinstdin);
  inv = parse({"jfsh", "--", "-script"});
  EXPECT_EQ(inv.script, "-script");
  inv = parse({"jfsh", "-", "x"});
  EXPECT_EQ(inv.script, "x");
}

TEST(Invocation, OptionsByLetterNameAndLongName) {
  auto inv = parse({"jfsh", "-o", "errexit", "+o", "nomatch", "-oxtrace", "--no-glob",
                    "--extended-glob", "+u", "-c", "true"});
  using P = s::pair<OptIndex, bool>;
  EXPECT_EQ(inv.options, (vec<P>{{OPT_ERREXIT, true}, {OPT_NOMATCH, false}, {OPT_XTRACE, true},
                                 {OPT_GLOB, false}, {OPT_EXTENDEDGLOB, true},
                                 {OPT_UNSET, true}}));   // +u is UNSET: -u means NO_UNSET
  inv = parse({"jfsh", "-u", "-c", "true"});
  EXPECT_EQ(inv.options, (vec<P>{{OPT_UNSET, false}}));
}

TEST(Invocation, InvocationOptionsHaveFields) {
  auto inv = parse({"jfsh", "-il", "-f", "-d", "-m"});   // -f, -d: NO_RCS, NO_GLOBAL_RCS
  EXPECT_EQ(inv.interactive, true);
  EXPECT_TRUE(inv.login);
  EXPECT_FALSE(inv.rcs);
  EXPECT_FALSE(inv.global_rcs);
  EXPECT_EQ(inv.monitor, true);
  EXPECT_TRUE(inv.options.empty());
  EXPECT_TRUE(parse({"-jfsh"}).login);   // $0 starting with '-'
  EXPECT_TRUE(parse({"jfsh", "--login"}).login);
  EXPECT_FALSE(parse({"jfsh", "--no-rcs"}).rcs);
  EXPECT_FALSE(parse({"jfsh"}).interactive.has_value());
}

TEST(Invocation, JfshLongOptions) {
  auto inv = parse({"jfsh", "--norouter", "--router", "jev", "--provider=anthropic",
                    "--llm-model", "m", "--zsh-compat-rc", "-c", "true"});
  EXPECT_TRUE(inv.norouter);
  EXPECT_EQ(inv.router, "jev");
  EXPECT_EQ(inv.provider, "anthropic");
  EXPECT_EQ(inv.llm_model, "m");
  EXPECT_TRUE(inv.zsh_compat_rc);
  EXPECT_TRUE(parse({"jfsh", "--help"}).help);
  EXPECT_TRUE(parse({"jfsh", "-h"}).help);
  EXPECT_TRUE(parse({"jfsh", "--version"}).version);
}

// B32: nothing unknown is ignored any more.
TEST(Invocation, Errors) {
  // Named by $0, or by the shell once -c is seen, as zsh's zerr.
  EXPECT_EQ(parse_error({"jfsh", "-q"}), "jfsh: bad option: -q");
  EXPECT_EQ(parse_error({"/x/jfsh", "+q"}), "/x/jfsh: bad option: -q");
  EXPECT_EQ(parse_error({"/x/jfsh", "-c"}), "jfsh: string expected after -c");
  EXPECT_EQ(parse_error({"jfsh", "-cx"}), "jfsh: string expected after -cx");
  EXPECT_EQ(parse_error({"jfsh", "-o"}), "jfsh: string expected after -o");
  EXPECT_EQ(parse_error({"jfsh", "-o", "nosuch"}), "jfsh: no such option: nosuch");
  EXPECT_EQ(parse_error({"jfsh", "--nosuch"}), "jfsh: no such option: nosuch");
  EXPECT_EQ(parse_error({"jfsh", "--router"}), "jfsh: --router: argument expected");
  EXPECT_EQ(parse_error({"jfsh", "--router", "x"}),
            "jfsh: --router: unknown router: x (jeff, jev or heuristic)");
  EXPECT_EQ(parse_error({"jfsh", "--provider", "x"}),
            "jfsh: --provider: unknown provider: x (openai, anthropic, ollama or openrouter)");
  EXPECT_EQ(parse_error({"jfsh", "--norouter=1"}), "jfsh: no such option: norouter=1");
}

// ---- special parameters ---------------------------------------------------------

TEST(Specials, Seconds) {
  Shell sh;
  EXPECT_EQ(out(sh, "echo $SECONDS ${(t)SECONDS}"), "0 integer-special\n");
  EXPECT_EQ(out(sh, "SECONDS=100; echo $SECONDS"), "100\n");
  EXPECT_EQ(out(sh, "(( SECONDS = 7 )); echo $SECONDS; (( SECONDS += 3 )); echo $SECONDS"),
            "7\n10\n");
  EXPECT_EQ(out(sh, "SECONDS=2*3; echo $SECONDS"), "6\n");
  EXPECT_EQ(out(sh, "(( SECONDS > 5 )) && echo yes"), "yes\n");
}

TEST(Specials, RandomRepeatsAfterSeeding) {
  Shell sh;
  auto a = out(sh, "RANDOM=42; echo $RANDOM $RANDOM $RANDOM");
  auto b = out(sh, "RANDOM=42; echo $RANDOM $RANDOM $RANDOM");
  EXPECT_EQ(a, b);
  EXPECT_NE(out(sh, "echo $RANDOM $RANDOM $RANDOM $RANDOM"), "0 0 0 0\n");
  EXPECT_EQ(out(sh, "(( RANDOM >= 0 && RANDOM < 32768 )) && echo ok"), "ok\n");
}

TEST(Specials, LinenoCountsScriptFunctionAndEvalLines) {
  Shell sh;
  EXPECT_EQ(out(sh, "echo $LINENO\n\necho $LINENO"), "1\n3\n");
  EXPECT_EQ(sh.run_script("echo $LINENO\nf() {\n\n\n  echo in $LINENO\n}\nf\n"
                          "eval 'echo ev $LINENO'\necho $LINENO\n").out,
            "1\nin 3\nev 1\n9\n");
  EXPECT_EQ(out(sh, "for i in 1 2; do\n  echo $LINENO\ndone"), "2\n2\n");
}

TEST(Specials, StatusAndPipestatus) {
  Shell sh;
  EXPECT_EQ(out(sh, "false; echo $status $?"), "1 1\n");
  EXPECT_EQ(out(sh, "true | false | (exit 3); echo $pipestatus"), "0 1 3\n");
  EXPECT_EQ(out(sh, "false; echo $pipestatus"), "1\n");
  // An array assignment is a pipeline too, so the next command sees its
  // status; a scalar one or a function definition leaves it (zsh 5.9).
  EXPECT_EQ(out(sh, "pipestatus=(4 5); echo $pipestatus"), "0\n");
  EXPECT_EQ(out(sh, "true | false; x=1; f() { :; }; echo $pipestatus"), "0 1\n");
  EXPECT_EQ(sh.run("status=3").err, "jfsh: read-only variable: status\n");
}

TEST(Specials, PpidIsFixedAndReadonly) {
  Shell sh;
  EXPECT_EQ(out(sh, "echo $PPID"), s::to_string(::getppid()) + "\n");
  EXPECT_EQ(out(sh, "[[ $(echo $PPID) == $PPID ]] && echo same"), "same\n");
  EXPECT_EQ(sh.run("PPID=1").err, "jfsh: read-only variable: PPID\n");
}

TEST(Specials, EpochAndHistcmd) {
  Shell sh;
  long now = static_cast<long>(s::time(nullptr));
  long got = s::stol(out(sh, "echo $EPOCHSECONDS"));
  EXPECT_LE(s::labs(got - now), 2);
  EXPECT_EQ(out(sh, "[[ $EPOCHREALTIME == <->.<-> ]] && echo ok"), "ok\n");
  EXPECT_EQ(out(sh, "echo $HISTCMD"), "0\n");
  HistoryManager hist{HistOptions{}};
  hist.add("one");
  hist.add("two");
  sh.ctx.history = &hist;
  EXPECT_EQ(out(sh, "echo $HISTCMD"), "2\n");
  sh.ctx.history = nullptr;
}

TEST(Specials, UnsetLosesTheSpecialMeaning) {
  Shell sh;
  EXPECT_EQ(out(sh, "unset SECONDS; echo \"[$SECONDS]\"; SECONDS=abc; echo $SECONDS"),
            "[]\nabc\n");
}

// ---- startup files --------------------------------------------------------------

// A system directory and a $ZDOTDIR whose files each append their name to
// $order.
struct Rc {
  TempDir  dir;
  sf::path sys = dir.path / "etc";
  sf::path home = dir.path / "home";
  Shell    sh;

  Rc() {
    sf::create_directories(sys);
    sf::create_directories(home);
    for (auto n : {"jfshenv", "jfsh_profile", "jfshrc", "jfsh_login", "jfsh_logout"})
      write(sys / n, str("order+=(/") + n + ")\n");
    for (auto n : {".jfshenv", ".jfsh_profile", ".jfshrc", ".jfsh_login", ".jfsh_logout",
                   ".zshenv", ".zprofile", ".zshrc", ".zlogin", ".zlogout"})
      write(home / n, str("order+=(") + n + ")\n");
    sh.params.assign("ZDOTDIR", home.string(), PM_SCALAR);
  }
  static void write(const sf::path& p, strv body) { s::ofstream{p} << body; }

  str start(bool login, bool interactive, bool zsh = false) {
    sh.opts.assign(OPT_LOGIN, login);
    sh.opts.assign(OPT_INTERACTIVE, interactive);
    StartupConfig cfg{sys.string(), zsh};
    sh.call([&] { run_startup_files(sh.exec, cfg); return 0; });
    return out(sh, "echo $order");
  }
  str logout(bool zsh = false) {
    StartupConfig cfg{sys.string(), zsh};
    sh.call([&] { run_logout_files(sh.exec, cfg); return 0; });
    return out(sh, "echo $order");
  }
};

TEST(Startup, ScriptReadsOnlyTheEnvFiles) {
  Rc rc;
  EXPECT_EQ(rc.start(false, false), "/jfshenv .jfshenv\n");
}

TEST(Startup, InteractiveLoginReadsEverythingInZshOrder) {
  Rc rc;
  EXPECT_EQ(rc.start(true, true),
            "/jfshenv .jfshenv /jfsh_profile .jfsh_profile /jfshrc .jfshrc "
            "/jfsh_login .jfsh_login\n");
  EXPECT_EQ(rc.logout(), "/jfshenv .jfshenv /jfsh_profile .jfsh_profile /jfshrc .jfshrc "
                         "/jfsh_login .jfsh_login .jfsh_logout /jfsh_logout\n");
}

TEST(Startup, InteractiveNonLogin) {
  Rc rc;
  EXPECT_EQ(rc.start(false, true), "/jfshenv .jfshenv /jfshrc .jfshrc\n");
  EXPECT_EQ(rc.logout(), "/jfshenv .jfshenv /jfshrc .jfshrc\n");   // not a login shell
}

TEST(Startup, NoRcsReadsOnlyTheSystemEnvFile) {
  Rc rc;
  rc.sh.opts.unset(OPT_RCS);
  EXPECT_EQ(rc.start(true, true), "/jfshenv\n");
}

TEST(Startup, AFileCanTurnOffTheFilesAfterIt) {
  Rc rc;
  Rc::write(rc.home / ".jfshenv", "order+=(.jfshenv); setopt no_global_rcs\n");
  EXPECT_EQ(rc.start(true, true), "/jfshenv .jfshenv .jfsh_profile .jfshrc .jfsh_login\n");
  Rc rc2;
  Rc::write(rc2.sys / "jfshenv", "order+=(/jfshenv); unsetopt rcs\n");
  EXPECT_EQ(rc2.start(true, true), "/jfshenv\n");
}

TEST(Startup, ZdotdirSetInTheEnvFileMovesTheRest) {
  Rc rc;
  sf::create_directories(rc.dir.path / "other");
  Rc::write(rc.dir.path / "other" / ".jfshrc", "order+=(other)\n");
  Rc::write(rc.home / ".jfshenv", "order+=(.jfshenv); ZDOTDIR=" + (rc.dir.path / "other").string() + "\n");
  EXPECT_EQ(rc.start(false, true), "/jfshenv .jfshenv /jfshrc other\n");
}

TEST(Startup, ZshCompatAlsoReadsZshFiles) {
  Rc rc;
  EXPECT_EQ(rc.start(true, true, true),
            "/jfshenv .jfshenv .zshenv /jfsh_profile .jfsh_profile .zprofile /jfshrc .jfshrc "
            ".zshrc /jfsh_login .jfsh_login .zlogin\n");
}

TEST(Startup, ExitInAFileStopsTheRest) {
  Rc rc;
  Rc::write(rc.home / ".jfshenv", "order+=(.jfshenv); exit 3\n");
  rc.sh.opts.set(OPT_LOGIN);
  rc.sh.opts.set(OPT_INTERACTIVE);
  StartupConfig cfg{rc.sys.string(), false};
  run_startup_files(rc.sh.exec, cfg);
  EXPECT_TRUE(rc.sh.ctx.exiting);
  EXPECT_EQ(rc.sh.ctx.exit_val, 3);
  EXPECT_EQ(out(rc.sh, "echo $order"), "/jfshenv .jfshenv\n");
}

TEST(Startup, MissingFilesAreNoError) {
  Shell sh;
  TempDir dir;
  sh.params.assign("ZDOTDIR", dir.path.string(), PM_SCALAR);
  sh.opts.set(OPT_LOGIN);
  sh.opts.set(OPT_INTERACTIVE);
  StartupConfig cfg{(dir.path / "nope").string(), true};
  auto r = sh.call([&] { run_startup_files(sh.exec, cfg); return 0; });
  EXPECT_EQ(r.err, "");
  EXPECT_EQ(r.out, "");
}

// zsh's setupvals: an environment $PWD with a `.` or `..` element is not
// kept, and $OLDPWD starts as $PWD whatever the environment says.
TEST(StartupM8Review, PwdFromTheEnvironment) {
  char buf[4096];
  str cwd = ::getcwd(buf, sizeof buf);
  for (str env : {cwd + "/.", cwd + "/../" + sf::path(cwd).filename().string()}) {
    Shell sh;
    sh.params.assign("PWD", env, PM_SCALAR | PM_EXPORTED);
    sh.params.assign("OLDPWD", str("/"), PM_SCALAR | PM_EXPORTED);
    init_shell_params(sh.ctx);
    EXPECT_EQ(out(sh, "echo $PWD $OLDPWD"), cwd + " " + cwd + "\n") << env;
  }
}

TEST(Startup, InitShellParams) {
  Shell sh;
  sh.params.assign("SHLVL", str("4"), PM_SCALAR | PM_EXPORTED);
  sh.params.assign("PWD", str("/nonexistent/dir"), PM_SCALAR | PM_EXPORTED);
  init_shell_params(sh.ctx);
  char buf[4096];
  str cwd = ::getcwd(buf, sizeof buf);
  EXPECT_EQ(out(sh, "echo $SHLVL; echo $PWD; echo $OLDPWD"), "5\n" + cwd + "\n" + cwd + "\n");
  EXPECT_EQ(out(sh, "env | grep -c -e '^SHLVL=5$' -e '^PWD=' -e '^OLDPWD='"), "3\n");
}

// ---- hooks ----------------------------------------------------------------------

TEST(Hooks, PrecmdThenItsFunctionsArray) {
  Shell sh;
  sh.run("precmd() { print -n 'p ' }; a() { print -n \"a$# \" }; b() { print -n 'b ' }; "
         "precmd_functions=(a nosuch b)");
  sh.run("false");
  auto r = sh.call([&] { return run_hook(sh.exec, "precmd", {"x"}) ? 0 : 1; });
  EXPECT_EQ(r.out, "p a1 b ");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(sh.ctx.last_exit, 1);   // $? survives the hook
  EXPECT_FALSE(run_hook(sh.exec, "nohook"));
}

TEST(Hooks, PreexecGetsTheLine) {
  Shell sh;
  sh.run("preexec() { print -r -- \"[$1][$2][$3]\" }");
  // $2 and $3 are the parsed line, as zsh's getjobtext and getpermtext
  // give it (zsh 5.9).
  auto prog = sh.exec.parse_string("ls  -l; for i in 1; do echo $i; done");
  ASSERT_TRUE(prog);
  auto r = sh.call([&] { before_exec(sh.exec, "ls  -l", **prog); return 0; });
  EXPECT_EQ(r.out, "[ls  -l][ls -l; for i in 1; do; echo $i; done]"
                   "[ls -l\nfor i in 1\ndo\n\techo $i\ndone]\n");
}

TEST(Hooks, PeriodicRunsEveryPeriodSeconds) {
  Shell sh;
  sh.run("n=0; periodic() { (( n++ )) }; PERIOD=10");
  auto prompt = [&](s::time_t t) { sh.call([&] { before_prompt(sh.exec, t); return 0; }); };
  prompt(1000);
  prompt(1005);
  EXPECT_EQ(sh.get("n"), "1");
  prompt(1011);
  EXPECT_EQ(sh.get("n"), "2");
  sh.run("unset PERIOD");
  prompt(5000);
  EXPECT_EQ(sh.get("n"), "2");
}

// ---- sched ----------------------------------------------------------------------

TEST(Sched, QueuesListsRemovesAndRuns) {
  Shell sh;
  EXPECT_EQ(sh.run("sched +30 echo later").rc, 0);
  EXPECT_EQ(sh.run("sched +0:00:10 echo sooner").rc, 0);
  EXPECT_EQ(sh.run("sched -o +1:00 -- -x").rc, 0);
  auto list = out(sh, "sched");
  // "  1 Mon Nov 16 14:00:10 echo sooner"
  EXPECT_TRUE(list.starts_with("  1 ")) << list;
  EXPECT_NE(list.find(" echo sooner\n  2 "), str::npos) << list;
  EXPECT_NE(list.find(" echo later\n  3 "), str::npos) << list;
  // `--` after the time is part of the command, shown after zsh's own.
  EXPECT_TRUE(list.ends_with(" -o -- -- -x\n")) << list;
  EXPECT_EQ(sh.run("sched -3").rc, 0);
  EXPECT_EQ(sh.run("sched -3").err, "jfsh: sched: not that many entries\n");
  ASSERT_EQ(sh.ctx.loop.sched.size(), 2u);

  auto r = sh.call([&] { run_sched(sh.exec, s::time(nullptr) + 15); return 0; });
  EXPECT_EQ(r.out, "sooner\n");
  r = sh.call([&] { run_sched(sh.exec, s::time(nullptr) + 60); return 0; });
  EXPECT_EQ(r.out, "later\n");
  EXPECT_TRUE(sh.ctx.loop.sched.empty());
}

TEST(Sched, Errors) {
  Shell sh;
  EXPECT_EQ(sh.run("sched +10").err, "jfsh: sched: not enough arguments\n");
  EXPECT_EQ(sh.run("sched soon echo").err, "jfsh: sched: bad time specifier\n");
  EXPECT_EQ(sh.run("sched 1:2:3:4 echo").err, "jfsh: sched: bad time specifier\n");
  EXPECT_EQ(sh.run("sched -z +1 echo").err, "jfsh: sched: bad option: -z\n");
}

TEST(Sched, AbsoluteClockTimeIsTheNextOne) {
  Shell sh;
  s::time_t now = s::time(nullptr);
  sh.run("sched 0:00 echo midnight");
  ASSERT_EQ(sh.ctx.loop.sched.size(), 1u);
  s::time_t when = sh.ctx.loop.sched[0].when;
  EXPECT_GT(when, now - 1);
  EXPECT_LE(when, now + 25 * 3600);   // a DST change makes a day 25 hours
  struct tm tm{};
  ::localtime_r(&when, &tm);
  EXPECT_EQ(tm.tm_hour, 0);
  EXPECT_EQ(tm.tm_min, 0);
}

// ---- mail -----------------------------------------------------------------------

TEST(Mail, ReportsFilesWrittenSinceTheLastCheck) {
  Shell sh;
  TempDir dir;
  auto box = dir.file("box", "From x\n", sf::perms::owner_read | sf::perms::owner_write);
  auto set_times = [&](s::time_t atime, s::time_t mtime) {
    struct timespec ts[2] = {{atime, 0}, {mtime, 0}};
    ::utimensat(AT_FDCWD, box.c_str(), ts, 0);
  };
  set_times(100, 200);   // written after it was read
  sh.params.assign("MAIL", box.string(), PM_SCALAR);
  auto check = [&](s::time_t now) {
    return sh.call([&] { check_mail(sh.exec, now); return 0; }).err;
  };
  EXPECT_EQ(check(1000), "You have new mail.\n");
  EXPECT_EQ(check(2000), "");          // nothing new since 1000
  set_times(100, 2500);
  EXPECT_EQ(check(3000), "You have new mail.\n");
  set_times(4000, 3500);               // read since
  EXPECT_EQ(check(5000), "");

  set_times(100, 6000);
  sh.run("mailpath=(" + box.string() + "'?new in $_')");
  EXPECT_EQ(check(7000), "new in " + box.string() + "\n");
}

TEST(Mail, BeforePromptChecksEveryMailcheckSeconds) {
  Shell sh;
  TempDir dir;
  auto box = dir.file("box", "x\n", sf::perms::owner_read | sf::perms::owner_write);
  struct timespec ts[2] = {{100, 0}, {200, 0}};
  ::utimensat(AT_FDCWD, box.c_str(), ts, 0);
  sh.params.assign("MAIL", box.string(), PM_SCALAR);
  sh.run("MAILCHECK=60");   // as the shell starts it
  sh.ctx.loop.last_mailcheck = 0;
  auto prompt = [&](s::time_t t) {
    return sh.call([&] { before_prompt(sh.exec, t); return 0; }).err;
  };
  EXPECT_EQ(prompt(1000), "You have new mail.\n");
  ts[1] = {1010, 0};
  ::utimensat(AT_FDCWD, box.c_str(), ts, 0);
  EXPECT_EQ(prompt(1030), "");                     // within MAILCHECK (60)
  EXPECT_EQ(prompt(1070), "You have new mail.\n");
}

// ---- fc and the line being run ----------------------------------------------------

TEST(History, TheLineBeingRunIsLeftOut) {
  Shell sh;
  HistoryManager hist{HistOptions{}};
  hist.add("echo a");
  hist.add("echo b");
  sh.ctx.history = &hist;
  // The interactive loop added `fc -l` before running it.
  hist.add("fc -l");
  sh.ctx.hist_current = true;
  EXPECT_EQ(out(sh, "fc -l"), "    1  echo a\n    2  echo b\n");
  // Listed by number, the line being run is there (zsh's fclist).
  EXPECT_EQ(out(sh, "fc -l 2 3"), "    2  echo b\n    3  fc -l\n");
  sh.ctx.hist_current = false;
  sh.ctx.history = nullptr;
  HistoryManager hist2{HistOptions{}};
  hist2.add("echo a");
  hist2.add("echo b");
  hist2.add("r");
  sh.ctx.history = &hist2;
  sh.ctx.hist_current = true;
  auto r = sh.run("r");
  EXPECT_EQ(r.out, "b\n");
  EXPECT_EQ(r.err, "echo b\n");
  // zsh keeps the `r` line and adds what it ran after it.
  EXPECT_EQ(hist2.entries()[2].text, "r");
  EXPECT_EQ(hist2.entries().back().text, "echo b");
  EXPECT_EQ(hist2.size(), 4u);
  hist2.add("fc -s 2");
  EXPECT_EQ(sh.run("fc -s 2").err, "echo b\n");
  sh.ctx.hist_current = false;
  sh.ctx.history = nullptr;
}

// ---- the jfsh binary ----------------------------------------------------------------

struct Proc {
  int rc;
  str out;
  str err;
};

// Runs the jfsh binary with `args`, `env` added to the environment and
// `input` on stdin.
Proc jfsh(const vec<str>& args, const vec<str>& env = {}, strv input = "") {
  TempDir io;
  auto in_path = io.file("in", input, sf::perms::owner_read | sf::perms::owner_write);
  sf::path out_path = io.path / "out", err_path = io.path / "err";
  pid_t pid = ::fork();
  if (pid == 0) {
    int in  = ::open(in_path.c_str(), O_RDONLY);
    int out = ::open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    int err = ::open(err_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    ::dup2(in, 0);
    ::dup2(out, 1);
    ::dup2(err, 2);
    for (const auto& e : env) ::putenv(const_cast<char*>(e.c_str()));
    vec<char*> argv{const_cast<char*>(JFSH_BINARY)};
    for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
    argv.push_back(nullptr);
    ::execv(JFSH_BINARY, argv.data());
    _exit(127);
  }
  int st = 0;
  ::waitpid(pid, &st, 0);
  auto slurp = [](const sf::path& p) {
    s::ifstream f{p};
    return str(s::istreambuf_iterator<char>(f), {});
  };
  return {WIFEXITED(st) ? WEXITSTATUS(st) : -1, slurp(out_path), slurp(err_path)};
}

TEST(Binary, BadOptionsFail) {
  // zsh names these by its $0 (here the binary's path), or by the shell
  // after -c.
  auto p = jfsh({"-q"});
  EXPECT_EQ(p.rc, 1);
  EXPECT_EQ(p.err, JFSH_BINARY ": bad option: -q\n");
  p = jfsh({"-c"});
  EXPECT_EQ(p.err, "jfsh: string expected after -c\n");
  p = jfsh({"/nonexistent/script"});
  EXPECT_EQ(p.rc, 127);
  EXPECT_EQ(p.err, JFSH_BINARY ": can't open input file: /nonexistent/script\n");
}

TEST(Binary, OptionsReachTheShell) {
  TempDir home;
  str zd = "ZDOTDIR=" + home.path.string();
  EXPECT_EQ(jfsh({"-c", "echo $0 $# $1 $-", "a", "b", "c"}, {zd}).out, "a 2 b 569X\n");
  EXPECT_EQ(jfsh({"-l", "-c", "echo $-"}, {zd}).out, "569Xl\n");
  EXPECT_EQ(jfsh({"-o", "errexit", "-c", "echo $-"}, {zd}).out, "569Xe\n");
  EXPECT_EQ(jfsh({"-s", "a", "b"}, {zd}, "echo $@ $-\n").out, "a b 569Xs\n");
  auto x = jfsh({"-xc", "echo x"}, {zd});
  EXPECT_EQ(x.out, "x\n");
  EXPECT_EQ(x.err, "+jfsh:1> echo x\n");   // %N of a -c string, as zsh's "zsh"
  EXPECT_EQ(jfsh({"--version"}).out.starts_with("jfsh "), true);
}

TEST(Binary, StartupFilesAndLogout) {
  TempDir home;
  home.file(".jfshenv", "print env\n", sf::perms::owner_all);
  home.file(".jfsh_profile", "print profile\n", sf::perms::owner_all);
  home.file(".jfsh_logout", "print logout\n", sf::perms::owner_all);
  str zd = "ZDOTDIR=" + home.path.string();
  EXPECT_EQ(jfsh({"-c", "print cmd"}, {zd}).out, "env\ncmd\n");
  EXPECT_EQ(jfsh({"-f", "-c", "print cmd"}, {zd}).out, "cmd\n");
  // Only an interactive login shell reads its logout files (zsh's zexit).
  auto p = jfsh({"-l", "-c", "print cmd; exit 4"}, {zd});
  EXPECT_EQ(p.out, "env\nprofile\ncmd\n");
  EXPECT_EQ(p.rc, 4);
  p = jfsh({"-l", "-i"}, {zd}, "print cmd; exit 4\n");
  EXPECT_EQ(p.out, "env\nprofile\ncmd\nlogout\n");
  EXPECT_EQ(p.rc, 4);
  // The EXIT trap and zshexit come after the logout file.
  home.file(".jfshenv", "trap 'print trap' EXIT; zshexit() { print zshexit }\n",
            sf::perms::owner_all);
  EXPECT_EQ(jfsh({"-l", "-i"}, {zd}, "print cmd\n").out, "profile\ncmd\nlogout\ntrap\nzshexit\n");
  EXPECT_EQ(jfsh({"-l", "-c", "print cmd"}, {zd}).out, "profile\ncmd\ntrap\nzshexit\n");
  // `exit` in a startup file ends the shell before the command.
  home.file(".jfshenv", "exit 7\n", sf::perms::owner_all);
  p = jfsh({"-c", "print cmd"}, {zd});
  EXPECT_EQ(p.out, "");
  EXPECT_EQ(p.rc, 7);
}

TEST(Binary, ShlvlIsIncremented) {
  TempDir home;
  str zd = "ZDOTDIR=" + home.path.string();
  EXPECT_EQ(jfsh({"-c", "echo $SHLVL"}, {zd, "SHLVL=6"}).out, "7\n");
}

TEST(Binary, ScriptRunsUpToASyntaxError) {
  TempDir home;
  auto script = home.file("s.sh", "echo a\necho b\nif true\n", sf::perms::owner_all);
  auto p = jfsh({script.string()}, {"ZDOTDIR=" + home.path.string()});
  EXPECT_EQ(p.out, "a\nb\n");
  EXPECT_EQ(p.rc, 1);
}


TEST(Binary, B5HistoryFileGetsOnlyNewLines) {
  TempDir home;
  str file = (home.path / "hist").string();
  // Nothing is saved without $SAVEHIST (zsh's default is 0).
  vec<str> env{"ZDOTDIR=" + home.path.string(), "HISTFILE=" + file, "SAVEHIST=100"};
  jfsh({"-i"}, env, "echo a\n");
  jfsh({"-i"}, env, "echo b\n");
  s::ifstream f{file};
  EXPECT_EQ(str(s::istreambuf_iterator<char>(f), {}), "echo a\necho b\n");
}

TEST(Binary, B54PromptsGoToStderrWithoutATerminal) {
  TempDir home;
  auto p = jfsh({"-i"}, {"ZDOTDIR=" + home.path.string(), "PS1=P> "}, "echo out\n");
  EXPECT_EQ(p.out, "out\n");
  EXPECT_EQ(p.err.substr(0, 3), "P> ");
}

// An interactive jfsh on a terminal gives the terminal back to the process
// group that started it when it exits, as zsh's release_pgrp does.  Before,
// the second `jfsh -i` found its group in the background and stopped it
// with SIGTTIN, and a `make test` run from a terminal hung.
TEST(Binary, InteractiveShellGivesTheTerminalBack) {
  TempDir home;
  auto input = home.file("in", "echo a\n", sf::perms::owner_all);
  int master = -1, slave = -1;
  ASSERT_EQ(::openpty(&master, &slave, nullptr, nullptr, nullptr), 0);
  str j = JFSH_BINARY;
  str cmd = "ZDOTDIR=" + home.path.string() + " " + j + " -i <" + input.string()
          + " >/dev/null 2>&1; ZDOTDIR=" + home.path.string() + " " + j + " -i <"
          + input.string() + " >/dev/null 2>&1; echo done";
  pid_t pid = ::fork();
  if (pid == 0) {
    // A session of its own with the pty as its controlling terminal.
    ::setsid();
    ::ioctl(slave, TIOCSCTTY, 0);
    ::dup2(slave, STDOUT_FILENO);
    ::dup2(slave, STDERR_FILENO);
    ::close(master);
    ::execl("/bin/sh", "sh", "-c", cmd.c_str(), static_cast<char*>(nullptr));
    _exit(127);
  }
  ::close(slave);
  str got;
  for (int k = 0; k < 100 and got.find("done") == str::npos; ++k) {
    struct pollfd p{master, POLLIN, 0};
    if (::poll(&p, 1, 100) <= 0) continue;
    char buf[256];
    ssize_t n = ::read(master, buf, sizeof buf);
    if (n <= 0) break;
    got.append(buf, static_cast<s::size_t>(n));
  }
  ::kill(pid, SIGKILL);
  ::kill(-pid, SIGKILL);
  ::waitpid(pid, nullptr, 0);
  ::close(master);
  EXPECT_NE(got.find("done"), str::npos) << got;
}

// ---- M8 review: zsh 5.9's output for the same runs ------------------------------
//
// Each row runs the jfsh binary with HOME and ZDOTDIR set to a fresh
// directory holding `files`; `@B` in the expected text stands for the
// binary's path (zsh's $0) and `@D` for the directory.  The startup files
// are named as jfsh's (zsh's .zshenv is .jfshenv, and so on).

struct M8Row {
  vec<str> args;
  vec<s::pair<str, str>> files;
  str input;
  str out;
  str err;
  vec<str> env;   // added to the environment
  int rc;
};

str m8_expand(str text, const str& dir) {
  for (auto [token, value] : {s::pair<strv, str>{"@B", JFSH_BINARY}, s::pair<strv, str>{"@D", dir}})
    for (s::size_t at; (at = text.find(token)) != str::npos;) text.replace(at, 2, value);
  return text;
}

void m8_rows(const vec<M8Row>& rows) {
  for (const auto& row : rows) {
    TempDir home;
    str dir = home.path.string();
    for (const auto& [name, body] : row.files) {
      sf::create_directories((home.path / name).parent_path());
      home.file(name, m8_expand(body, dir), sf::perms::owner_all);
    }
    vec<str> env{"HOME=" + dir, "ZDOTDIR=" + dir, "SHLVL=1", "PWD=" + dir};
    for (const auto& e : row.env) env.push_back(m8_expand(e, dir));
    vec<str> args;
    for (const auto& a : row.args) args.push_back(m8_expand(a, dir));
    auto p = jfsh(args, env, row.input);
    str what;
    for (const auto& a : row.args) what += " [" + a + "]";
    EXPECT_EQ(p.out, m8_expand(row.out, dir)) << what;
    EXPECT_EQ(p.err, m8_expand(row.err, dir)) << what;
    EXPECT_EQ(p.rc, row.rc) << what;
  }
}

TEST(StartupM8Review, InvocationAsZsh) {
  m8_rows({
    // -s: every argument is positional, also with -c.
    {{"-f", "-s", "-c", "echo $0 $# $*", "a0", "b"}, {}, "", "@B 2 a0 b\n", "", {}, 0},
    // `+-name` turns an option off, `+-no-name` on; a lone `+` ends the
    // options as `-` does.
    {{"-f", "+-no-errexit", "-c", "[[ -o errexit ]] && echo E"}, {}, "", "E\n", "", {}, 0},
    {{"-f", "-e", "+-errexit", "-c", "[[ -o errexit ]] || echo noE"}, {}, "", "noE\n", "", {}, 0},
    {{"-f", "+"}, {}, "echo read\n", "read\n", "", {}, 0},
    {{"-f", "+", "-c", "echo"}, {}, "", "", "@B: can't open input file: -c\n", {}, 127},
    {{"-f", "-x-errexit", "-c", ":"}, {}, "", "", "@B: bad option string: '-x-errexit'\n", {}, 1},
    {{"-f", "--errexit=1", "-c", ":"}, {}, "", "", "@B: no such option: errexit=1\n", {}, 1},
    {{"-f", "-x y", "-c", ":"}, {}, "", "", "@B: bad option string: '-x y'\n", {}, 1},
    {{"-f", "-e ", "-c", "[[ -o errexit ]] && echo E"}, {}, "", "E\n", "", {}, 0},
    {{"-f", "+q"}, {}, "", "", "@B: bad option: -q\n", {}, 1},
    {{"-f", "-cx"}, {}, "", "", "jfsh: string expected after -cx\n", {}, 1},
    // -b ends the options with its argument.
    {{"-f", "-bx", "-c", ":"}, {}, "", "", "@B: can't open input file: -c\n", {}, 127},
    {{"-f", "--emulate", "sh", "-c", ":"}, {}, "", "",
     "@B: --emulate: must precede other options\n", {}, 1},
    {{"--emulate"}, {}, "", "", "@B: --emulate: argument required\n", {}, 1},
    {{"--emulate", "sh", "-c", "[[ -o shwordsplit ]] && echo SWS"}, {}, "", "SWS\n", "", {}, 0},
    // A directory is no script.
    {{"-f", "@D/sub"}, {{"sub/x", ""}}, "", "", "@B: can't open input file: @D/sub\n", {}, 127},
    // -t: one command, then the shell exits.
    {{"-f", "-t"}, {}, "\necho a; false\necho b\n", "a\n", "", {}, 1},
    {{"-f", "-t", "@D/s"}, {{"s", "echo a\necho b\n"}}, "", "a\n", "", {}, 0},
    // VERBOSE shows each line as it is read, and a -c string whole.
    {{"-f", "-v", "-c", "echo hi"}, {}, "", "hi\n", "echo hi\n", {}, 0},
    {{"-f"}, {}, "echo a\nset -v\necho b\n", "a\nb\n", "echo b\n", {}, 0},
    {{"-f", "-c", "echo $ZSH_EXECUTION_STRING"}, {}, "", "echo $ZSH_EXECUTION_STRING\n", "", {}, 0},
    // An empty name is a script that can't be opened.
    {{"-f", ""}, {}, "echo no\n", "", "@B: can't open input file: \n", {}, 127},
    // zshexit sees the exit status, not the EXIT trap's; -t ending a
    // script runs only the EXIT trap.
    {{"-f", "-c", "trap 'echo T $?' EXIT; zshexit() { echo ZE $?; }; exit 2"}, {}, "",
     "T 2\nZE 2\n", "", {}, 2},
    {{"-f", "-c", "trap 'echo T; return 5' EXIT; zshexit() { echo ZE $?; }; exit 2"}, {}, "",
     "T\nZE 2\n", "", {}, 5},
    {{"-f", "-t"}, {}, "zshexit() { echo ZE; }; trap 'echo T' EXIT; echo a\necho b\n",
     "a\nT\n", "", {}, 0},
  });
}

TEST(StartupM8Review, StartupFilesAsZsh) {
  m8_rows({
    // An error abandons the file, not the shell; $? stays as it left it.
    {{"-c", "echo body $?"}, {{".jfshenv", "echo a\nreadonly r=1; r=2\necho b\n"}}, "",
     "a\nbody 1\n", "jfsh: read-only variable: r\n", {}, 0},
    {{"-c", "echo body $?"}, {{".jfshenv", "echo a\nfi\n"}}, "", "a\nbody 0\n",
     "jfsh:2: parse error near `fi'\n", {}, 0},
    // No ERR_EXIT, ZERR or ERR_RETURN while the startup files run.
    {{"-e", "-c", "echo body"}, {{".jfshenv", "false; echo after\n"}}, "",
     "after\nbody\n", "", {}, 0},
    {{"-c", "false; echo body"}, {{".jfshenv", "TRAPZERR() { echo ZERR }; false; echo after\n"}},
     "", "after\nZERR\nbody\n", "", {}, 0},
    {{"-c", "echo body"}, {{".jfshenv", "setopt errreturn; false; echo after\n"}}, "",
     "after\nbody\n", "", {}, 0},
    // $0 is the shell's there, the script's only after; `0=x` stays.
    {{"-c", "echo c0=$0", "zz"}, {{".jfshenv", "echo env0=$0\n"}}, "",
     "env0=zz\nc0=zz\n", "", {}, 0},
    {{"@D/s"}, {{".jfshenv", "echo env0=$0\n"}, {"s", "echo s0=$0\n"}}, "",
     "env0=@B\ns0=@D/s\n", "", {}, 0},
    {{"-c", "echo $0"}, {{".jfshenv", "0=foo\n"}}, "", "foo\n", "", {}, 0},
    // SHINSTDIN is off while a file is read.
    {{}, {{".jfshenv", "[[ -o shinstdin ]] && echo S || echo noS\n"}}, "echo body\n",
     "noS\nbody\n", "", {}, 0},
    // A non-interactive login shell reads no logout file.
    {{"-l", "-c", "exit 3"}, {{".jfsh_logout", "echo logout\n"}}, "", "", "", {}, 3},
    // An empty $ZDOTDIR is a directory too: nothing is read from $HOME.
    {{"-c", "echo body"}, {{".jfshenv", "echo env\n"}}, "", "body\n", "", {"ZDOTDIR="}, 0},
  });
}

TEST(StartupM8Review, SpecialParametersAsZsh) {
  m8_rows({
    {{"-f", "-c", "SECONDS=-5; echo $SECONDS"}, {}, "", "-5\n", "", {}, 0},
    {{"-f", "-c", "SECONDS=9223372036854775807; echo $SECONDS"}, {}, "",
     "9223372036854775807\n", "", {}, 0},
    {{"-f", "-c", "typeset -F SECONDS; [[ $SECONDS == *.* ]] && echo float"}, {}, "", "float\n",
     "", {}, 0},
    {{"-f", "-c", "echo $SECONDS"}, {}, "", "1000\n", "", {"SECONDS=1000"}, 0},
    {{"-f", "-c", "typeset -p HISTSIZE SAVEHIST SHLVL"}, {}, "",
     "typeset -i10 HISTSIZE=30\ntypeset -i10 SAVEHIST=0\nexport -i10 SHLVL=2\n", "", {}, 0},
    {{"-f", "-c", "HISTSIZE=0; SAVEHIST=-4; echo $HISTSIZE $SAVEHIST $MAILCHECK"}, {}, "",
     "1 0 60\n", "", {}, 0},
    {{"-f", "-c", "echo \"[$pipestatus]\"; false; x=1; f() { :; }; echo $pipestatus; a=(1)"
                  "; echo $pipestatus"}, {}, "", "[]\n1\n0\n", "", {}, 0},
    {{"-f", "-c", "echo a b; echo $_; f() { echo x y; }; f c; echo $_; _=z; echo \"[$_]\""},
     {}, "", "a b\nb\nx y\nc\n[]\n", "", {}, 0},
    {{"-f", "-c", "f() { echo $LINENO; }\nf\ng() echo $LINENO\n\ng\n"
                  "echo $(\necho $LINENO)\ntrap 'echo t $LINENO' USR1\n\nkill -USR1 $$"},
     {}, "", "0\n5\n7\nt 10\n", "", {}, 0},
    // $_ is left by words that expand to nothing and by a bare `builtin`;
    // `exec` with only redirections empties it.
    {{"-f", "-c", "echo k l >/dev/null; $empty; echo \"<$_>\"; echo o p >/dev/null; x=1 $empty"
                  "; echo \"<$_>\"; echo q r >/dev/null; exec 3>/dev/null; echo \"<$_>\""
                  "; echo u v >/dev/null; builtin; echo \"<$_>\""},
     {}, "", "<l>\n<p>\n<>\n<v>\n", "", {}, 0},
    // A compound command empties $_ unless nothing in it is a command
    // word, a redirection, a subshell, `!` or `&` (zsh's cmplx clear: it
    // runs by execsimple); array assignments and a bare `exec` run nothing
    // and leave it.
    {{"-f", "-c",
      "echo a q >/dev/null; ( ); echo \"[$_]\"; echo a q >/dev/null; { }; echo \"[$_]\"; "
      "echo a q >/dev/null; case a in b) ;; esac; echo \"[$_]\"; echo a q >/dev/null; "
      "{ } >/dev/null; echo \"[$_]\"; echo a q >/dev/null; ! { }; echo \"[$_]\"; "
      "echo a q >/dev/null; x=(1 2); echo \"[$_]\"; echo a q >/dev/null; exec; "
      "echo \"[$_]\"; echo a q >/dev/null; if [[ a = b ]]; then :; fi; echo \"[$_]\""},
     {}, "", "[]\n[q]\n[q]\n[]\n[]\n[q]\n[q]\n[]\n", "", {}, 0},
    // $OPTARG is taken from the environment.
    {{"-f", "-c", "echo \"[$OPTARG]\""}, {}, "", "[e]\n", "", {"OPTARG=e"}, 0},
    // $LINENO is put back after a list (zsh's execlist): an EXIT trap sees
    // the line before the -c string, or where reading got to in a script;
    // DEBUG and ZERR see the command's line.
    {{"-f", "-c", "trap 'echo ex $LINENO' EXIT\n\n\ntrue"}, {}, "", "ex 1\n", "", {}, 0},
    {{"-f", "@D/s"}, {{"s", "trap 'echo ex $LINENO' EXIT\n\n\ntrue\n"}}, "", "ex 5\n", "", {}, 0},
    {{"-f", "-c", "trap 'echo dbg $LINENO' DEBUG\n\necho x"}, {}, "", "dbg 3\nx\n", "", {}, 0},
    // ... also in a sourced file, and at the end of standard input, blank
    // lines counted.
    {{"-f", "-c", "trap 'echo T $LINENO' EXIT; . @D/sx"}, {{"sx", ":\n\n\nexit 3\n"}}, "",
     "T 4\n", "", {}, 3},
    {{"-f"}, {}, "trap 'echo T $LINENO' EXIT\n:\n\n\n\n", "T 6\n", "", {}, 0},
    // ... but an `exit` leaves at once: the trap sees its line.
    {{"-f", "-c", "\n\ntrap 'echo ex $LINENO' EXIT\n\nexit"}, {}, "", "ex 5\n", "", {}, 0},
    {{"-f", "-c", "f() {\n exit 3\n}\ntrap 'echo ex $LINENO' EXIT\n\nf"}, {}, "", "ex 6\n", "",
     {}, 3},
    {{"-f", "-c", "trap 'echo err $LINENO' ZERR\n\nfalse\necho $LINENO"}, {}, "", "err 3\n4\n",
     "", {}, 0},
    // $SHLVL is one less once the shell becomes another program.
    {{"-f", "-c", "exec /bin/sh -c 'echo $SHLVL'"}, {}, "", "2\n", "", {}, 0},
  });
}

// $_ after a compound command, one behaviour per row: zsh's parser marks a
// sublist "simple" (cmplx clear) when nothing in it, at any depth, is a
// command word, a redirection, a subshell, select, `|`, `!` or `&`; such a
// sublist runs by execsimple and leaves $_, any other empties it (zsh 5.9).
TEST(StartupM8Review, UnderscoreFollowsZshsCmplx) {
  vec<s::pair<str, str>> cases = {
    {"case x in y) echo q;; esac", "[]"},
    {"{ $empty; }", "[]"},
    {"{ [[ -n x ]] || echo q; }", "[]"},
    {"{ [[ -n x ]] } always { [[ -z x ]] && echo q }", "[]"},
    {"if [[ -n x ]]; then fi", "[b]"},
    {"while [[ -z x ]]; do done", "[b]"},
    {"for i in x; do done", "[b]"},
    {"for ((i=0;i<1;i++)); do (( 1 )); done", "[b]"},
    {"repeat 1 [[ -n x ]]", "[b]"},
    {"{ x=1 }", "[]"},
    {"{ x=(1) }", "[]"},
    {"{ } >/dev/null", "[]"},
    {"! [[ a = a ]]", "[]"},
    {"select i in; do :; done", "[]"},
    {"{ (( 1 )) && [[ a = a ]] }", "[b]"},
  };
  vec<M8Row> rows;
  for (const auto& [cmd, want] : cases)
    rows.push_back({{"-f", "-c", "echo a b >/dev/null; " + cmd + "; echo \"[$_]\""}, {}, "",
                    want + "\n", "", {}, 0});
  m8_rows(rows);
}

// `exec` in an interactive shell that has read no line writes no history
// file (zsh's savehistfile without a hist_ring).
TEST(StartupM8Review, ExecWithoutHistoryWritesNoFile) {
  TempDir home;
  str dir = home.path.string();
  home.file(".jfshenv", "", sf::perms::owner_all);
  auto p = jfsh({"-d", "-l", "-i", "-c", "exec /bin/echo EXEC"},
                {"HOME=" + dir, "ZDOTDIR=" + dir, "HISTFILE=" + dir + "/hf", "SAVEHIST=10"});
  EXPECT_EQ(p.out, "EXEC\n");
  EXPECT_FALSE(sf::exists(home.path / "hf"));
}

TEST(StartupM8Review, SchedAsZsh) {
  str tail = " | sed -E 's/^( *[0-9]+) .{3} .{3} .. ..:..:.. /\\1 T /'";
  m8_rows({
    {{"-f", "-c", "sched +100 -- -x; sched -o +200 echo o; sched" + tail}, {}, "",
     "  1 T -- -- -x\n  2 T -o echo o\n", "", {}, 0},
    {{"-f", "-c", "sched -0; sched - 1 cmd; sched +1x a; sched +-5 a; sched | wc -l | tr -d ' '"},
     {}, "", "1\n",
     "jfsh: sched: usage for delete: sched -<item#>.\njfsh: sched: option expected\n"
     "jfsh: sched: bad time specifier\n", {}, 0},
    {{"-f", "-c",
      "sched 100 a; sched 100 b; sched 50 c; sched 100 d; print -l $zsh_scheduled_events"},
     {}, "", "50::c\n100::d\n100::a\n100::b\n", "", {}, 0},
  });
}

}  // namespace
}  // namespace jfsh
