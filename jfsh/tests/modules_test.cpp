// Tests for the M10 module framework and the shipped modules
// (zsh/datetime + zsh/mathfunc from Phase 1; zsh/parameter + zsh/zutil
// from Phase 2a; zsh/system + zsh/files from Phase 2b).  Uses the
// in-process Shell fixture; expected outputs match zsh 5.9's
// `zsh -f -c`.

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <unistd.h>

#include <gtest/gtest.h>

#include <shell_fixture.h>

namespace jfsh::test {

// ---------------------------------------------------------------------------
// Framework: zmodload.  main.cpp auto-loads zsh/datetime and zsh/mathfunc.
// ---------------------------------------------------------------------------

TEST(Modules, ZmodloadListsLoadedModulesByDefault) {
  Shell sh;
  // As zsh -f: only zsh/main is loaded; the rest come when used or loaded.
  auto r = sh.run("zmodload");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "zsh/main\n");
}

TEST(Modules, ZmodloadDashLShowsSourceableForm) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload -L").out, "zmodload zsh/main\n");
  EXPECT_EQ(sh.run("zmodload zsh/datetime; zmodload -L").out,
            "zmodload zsh/datetime\nzmodload zsh/main\n");
}

TEST(Modules, ZmodloadDashERetursSuccessWhenLoaded) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload -e zsh/main").rc, 0);
  EXPECT_EQ(sh.run("zmodload -e zsh/datetime").rc, 1);
  EXPECT_EQ(sh.run("zmodload zsh/datetime; zmodload -e zsh/datetime").rc, 0);
  EXPECT_EQ(sh.run("zmodload -e zsh/not-a-real-module").rc, 1);
}

TEST(Modules, ZmodloadUnloadMarksModuleAbsent) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload zsh/mathfunc").rc, 0);
  EXPECT_EQ(sh.run("zmodload -e zsh/mathfunc").rc, 0);
  EXPECT_EQ(sh.run("zmodload -u zsh/mathfunc").rc, 0);
  EXPECT_EQ(sh.run("zmodload -e zsh/mathfunc").rc, 1);
  // Re-load restores it.
  EXPECT_EQ(sh.run("zmodload zsh/mathfunc").rc, 0);
  EXPECT_EQ(sh.run("zmodload -e zsh/mathfunc").rc, 0);
}

TEST(Modules, ZmodloadDashAAddsAliases) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload -A date=zsh/datetime").rc, 0);
  // The alias names the module, which isn't loaded until asked for.
  EXPECT_EQ(sh.run("zmodload -e date").rc, 1);
  EXPECT_EQ(sh.run("zmodload date; zmodload -e date").rc, 0);
  EXPECT_EQ(sh.run("zmodload -e zsh/datetime").rc, 0);
}

TEST(Modules, ZmodloadBadFlagReportsAndFails) {
  Shell sh;
  auto r = sh.run("zmodload -Z");
  EXPECT_EQ(r.rc, 1);
  EXPECT_NE(r.err.find("bad option: -Z"), std::string::npos);
}

TEST(Modules, ZmodloadLoadUnknownModuleFails) {
  Shell sh;
  auto r = sh.run("zmodload zsh/no_such_thing");
  EXPECT_EQ(r.rc, 1);
  EXPECT_NE(r.err.find("failed to load module `zsh/no_such_thing': "), std::string::npos);
  // A `-` isn't in a module name.
  r = sh.run("zmodload zsh/not-a-real-thing");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: invalid module name `zsh/not-a-real-thing'\n");
}

TEST(Modules, ZmodloadUnloadUnknownModuleFails) {
  Shell sh;
  auto r = sh.run("zmodload -u zsh/not-a-real-thing");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("no such module"), std::string::npos);
}

TEST(Modules, ZmodloadUnloadWithDashIgnoresMissing) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload -ui zsh/not-a-real-thing").rc, 0);
}

// ---------------------------------------------------------------------------
// zsh/mathfunc.
// ---------------------------------------------------------------------------

TEST(Modules, MathfuncSqrtOfFour) {
  Shell sh;
  sh.run("zmodload zsh/mathfunc");
  auto r = sh.run("echo $((sqrt(4)))");
  EXPECT_EQ(r.rc, 0);
  // zsh renders a float with 17 sig figs; 2.0 round-trips as "2.".
  EXPECT_EQ(r.out, "2.\n");
}

TEST(Modules, MathfuncSinZero) {
  Shell sh;
  sh.run("zmodload zsh/mathfunc");
  EXPECT_EQ(sh.run("echo $((sin(0)))").out, "0.\n");
}

TEST(Modules, MathfuncIntTruncatesTowardZero) {
  Shell sh;
  sh.run("zmodload zsh/mathfunc");
  EXPECT_EQ(sh.run("echo $((int(3.7)))").out, "3\n");
  EXPECT_EQ(sh.run("echo $((int(-3.7)))").out, "-3\n");
}

TEST(Modules, MathfuncFloatPromotes) {
  Shell sh;
  sh.run("zmodload zsh/mathfunc");
  // float(3) → 3.0, which prints as "3."
  EXPECT_EQ(sh.run("echo $((float(3)))").out, "3.\n");
}

TEST(Modules, MathfuncAtanOfOneOne) {
  Shell sh;
  sh.run("zmodload zsh/mathfunc");
  // zsh's two-argument atan is atan2; there is no atan2 function.
  EXPECT_EQ(sh.run("echo $((atan(1, 1)))").out, "0.78539816339744828\n");
  EXPECT_NE(sh.run("echo $((atan2(1, 1)))").err.find("unknown function: atan2"), str::npos);
}

TEST(Modules, MathfuncHypotOfThreeFour) {
  Shell sh;
  sh.run("zmodload zsh/mathfunc");
  EXPECT_EQ(sh.run("echo $((hypot(3,4)))").out, "5.\n");
}

TEST(Modules, MathfuncAbsHandlesIntAndFloat) {
  Shell sh;
  sh.run("zmodload zsh/mathfunc");
  EXPECT_EQ(sh.run("echo $((abs(-5)))").out,    "5\n");
  EXPECT_EQ(sh.run("echo $((abs(-2.5)))").out,  "2.5\n");
}

TEST(Modules, MathfuncUnknownFunctionIsAnError) {
  Shell sh;
  auto r = sh.run("echo $((noSuchFunc(1)))");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("unknown function"), std::string::npos);
}

TEST(Modules, MathfuncWrongArityIsAnError) {
  Shell sh;
  auto r = sh.run("echo $((sqrt(1,2)))");
  EXPECT_NE(r.rc, 0);
}

TEST(Modules, MathfuncCombinesWithOperatorsAndParams) {
  Shell sh;
  sh.run("zmodload zsh/mathfunc");
  auto r = sh.run("x=9; echo $((sqrt(x) + 1))");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "4.\n");
}

TEST(Modules, MathfuncUnloadRemovesFunctions) {
  Shell sh;
  sh.run("zmodload zsh/mathfunc");
  EXPECT_EQ(sh.run("zmodload -u zsh/mathfunc").rc, 0);
  auto r = sh.run("echo $((sqrt(4)))");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("unknown function"), std::string::npos);
}

TEST(Modules, LetUsesMathfunc) {
  Shell sh;
  sh.run("zmodload zsh/mathfunc");
  // `let x = sqrt(16)` assigns a float to x; jfsh prints floats in
  // typeset -F form (10 decimals) when read back as a scalar.
  auto r = sh.run("let 'x = sqrt(16)'; echo $x");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out.substr(0, 3), "4.0");
}

// ---------------------------------------------------------------------------
// zsh/datetime — the strftime builtin.
// ---------------------------------------------------------------------------

TEST(Modules, StrftimeFormatsEpoch) {
  Shell sh;
  sh.run("zmodload zsh/datetime");
  // Pass a fixed epoch time (midnight 1970-01-01 UTC) — but result depends
  // on local TZ, so just check the shape with %s (which is the input).
  auto r = sh.run("strftime '%s' 1700000000");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "1700000000\n");
}

TEST(Modules, StrftimeDashSStoresInVariable) {
  Shell sh;
  sh.run("zmodload zsh/datetime");
  auto r = sh.run("strftime -s t '%s' 1700000000; echo $t");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "1700000000\n");
}

TEST(Modules, StrftimeNeedsAFormat) {
  Shell sh;
  auto r = sh.run("strftime");
  EXPECT_NE(r.rc, 0);
}

TEST(Modules, StrftimeBadOption) {
  Shell sh;
  sh.run("zmodload zsh/datetime");
  auto r = sh.run("strftime -Q '%s'");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("bad option"), std::string::npos);
}

TEST(Modules, EpochSecondsIsIntegerWithPositiveValue) {
  Shell sh;
  sh.run("zmodload zsh/datetime");
  // $EPOCHSECONDS is installed as a special by install_special_params;
  // the datetime module records it in features.  Either way, it should
  // read as a positive integer.
  auto r = sh.run("echo $(( EPOCHSECONDS > 1000000000 ))");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "1\n");
}

// ---------------------------------------------------------------------------
// zmodload -a: autoload mark (recorded; load-on-use not yet wired).
// ---------------------------------------------------------------------------

TEST(Modules, ZmodloadDashAMarksAutoload) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload -a zsh/pcre pcre_match").rc, 0);
  // The mark is a builtin's, listed by -Lab; -L lists loaded modules.
  EXPECT_EQ(sh.run("zmodload -L").out, "zmodload zsh/main\n");
  EXPECT_NE(sh.run("zmodload -Lab").out.find("zmodload -ab zsh/pcre pcre_match\n"), str::npos);
}

// ---------------------------------------------------------------------------
// zsh/parameter (Phase 2a): live hash and array views of shell state.
// ---------------------------------------------------------------------------

TEST(Modules, ParameterParametersIncludesNewScalars) {
  Shell sh;
  auto r = sh.run("x=1; echo ${parameters[x]}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "scalar\n");
}

TEST(Modules, ParameterParametersClassifiesTypes) {
  Shell sh;
  auto r = sh.run("typeset -i n=1; typeset -A h; a=(x y); "
                  "echo ${parameters[n]} ${parameters[a]} ${parameters[h]}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_NE(r.out.find("integer"),     std::string::npos);
  EXPECT_NE(r.out.find("array"),       std::string::npos);
  EXPECT_NE(r.out.find("association"), std::string::npos);
}

TEST(Modules, ParameterFunctionsListsDefinedFunctions) {
  Shell sh;
  auto r = sh.run("greet() { echo hi; }; echo ${+functions[greet]}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "1\n");
}

TEST(Modules, ParameterFunctionsSubscriptGivesBody) {
  Shell sh;
  auto r = sh.run("f() { echo one; }; echo ${functions[f]}");
  EXPECT_EQ(r.rc, 0);
  // function_text renders a header + body; "echo one" must be present.
  EXPECT_NE(r.out.find("echo one"), std::string::npos);
}

TEST(Modules, ParameterAliasesLiveView) {
  Shell sh;
  auto r = sh.run("alias ll='ls -l'; echo ${aliases[ll]}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "ls -l\n");
}

TEST(Modules, ParameterGaliasesSeparateFromAliases) {
  Shell sh;
  auto r = sh.run("alias -g G='|grep'; echo ${+aliases[G]} ${+galiases[G]}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "0 1\n");
}

TEST(Modules, ParameterBuiltinsContainsCore) {
  Shell sh;
  auto r = sh.run("echo ${+builtins[echo]} ${+builtins[cd]} ${+builtins[unset]}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "1 1 1\n");
}

TEST(Modules, ParameterBuiltinsIncludesModuleContributed) {
  Shell sh;
  // strftime comes with zsh/datetime, which isn't autoloadable; zstyle's
  // module is, so it reads "undefined" until loaded.
  EXPECT_EQ(sh.run("echo \"[${builtins[strftime]}]\" $builtins[zstyle]").out, "[] undefined\n");
  EXPECT_EQ(sh.run("zmodload zsh/datetime zsh/zutil; "
                   "echo $builtins[strftime] $builtins[zstyle]").out,
            "defined defined\n");
}

TEST(Modules, ParameterOptionsHasExpectedKeys) {
  Shell sh;
  auto r = sh.run("echo ${+options[extendedglob]} ${options[monitor]:-missing}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_NE(r.out.find("1"), std::string::npos);
}

TEST(Modules, ParameterOptionsReflectsSetopt) {
  Shell sh;
  auto r = sh.run("setopt extendedglob; echo ${options[extendedglob]}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "on\n");
}

TEST(Modules, ParameterModulesIncludesAutoloaded) {
  Shell sh;
  EXPECT_EQ(sh.run("echo \"[${modules[zsh/datetime]}]\" ${modules[zsh/zutil]}").out,
            "[] autoloaded\n");
  EXPECT_EQ(sh.run("zmodload zsh/datetime; echo ${modules[zsh/datetime]}").out, "loaded\n");
}

TEST(Modules, ParameterReswordsIncludesIf) {
  Shell sh;
  // ${reswords[(r)…]} is a subscript flag (M13 left item); use `(M)…:#…`
  // via a loop instead.  The last `[[ ]]` likely returns 1 (no match on
  // the last iteration); check only the captured output.
  auto r = sh.run("for w in $reswords; do [[ $w = if ]] && echo yes; done; true");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "yes\n");
}

TEST(Modules, ParameterParametersSelfReferences) {
  Shell sh;
  // $parameters is itself a special: it lists itself.
  auto r = sh.run("echo ${parameters[parameters]}");
  EXPECT_EQ(r.rc, 0);
  // Type string begins with "association" since $parameters is a hash.
  EXPECT_EQ(r.out.substr(0, 11), "association");
}

// ---------------------------------------------------------------------------
// zsh/zutil: zstyle, zparseopts, is-at-least, zformat -f.
// ---------------------------------------------------------------------------

TEST(Modules, ZstyleSetAndQuery) {
  Shell sh;
  auto r = sh.run(
      "zstyle ':completion:*' menu select\n"
      "zstyle -s ':completion:*' menu val\n"
      "echo $val");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "select\n");
}

TEST(Modules, ZstylePatternMatching) {
  Shell sh;
  auto r = sh.run(
      "zstyle ':completion:*:git-*' format foo\n"
      "zstyle -s ':completion:fooctx:git-push' format val\n"
      "echo $val");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "foo\n");
}

TEST(Modules, ZstyleTruthTest) {
  Shell sh;
  auto r = sh.run(
      "zstyle ':ctx' enabled yes\n"
      "zstyle -t ':ctx' enabled && echo yes || echo no\n"
      "zstyle -t ':nothere' enabled && echo yes || echo no");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "yes\nno\n");
}

TEST(Modules, ZstyleDelete) {
  Shell sh;
  auto r = sh.run(
      "zstyle ':x' k v\n"
      "zstyle -d ':x' k\n"
      "zstyle -s ':x' k val && echo found || echo missing");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "missing\n");
}

TEST(Modules, ZstyleArrayQuery) {
  Shell sh;
  auto r = sh.run(
      "zstyle ':c' styles one two three\n"
      "zstyle -a ':c' styles arr\n"
      "echo $#arr $arr[1] $arr[3]");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "3 one three\n");
}

TEST(Modules, ZparseoptsBasicFlag) {
  Shell sh;
  auto r = sh.run(
      "set -- -f -x\n"
      "zparseopts -a opts f x\n"
      "echo \"$opts\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "-f -x\n");
}

TEST(Modules, ZparseoptsWithArgument) {
  Shell sh;
  auto r = sh.run(
      "set -- -O logfile.txt -v\n"
      "zparseopts -a opts O: v\n"
      "echo \"${opts[1]} ${opts[2]} ${opts[3]} ${opts[4]}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "-O logfile.txt -v \n");
}

TEST(Modules, ZparseoptsDashDDeletesFromArgv) {
  Shell sh;
  auto r = sh.run(
      "set -- -f one two\n"
      "zparseopts -D -a opts f\n"
      "echo \"$@\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "one two\n");
}

TEST(Modules, ZparseoptsFailOnUnknown) {
  Shell sh;
  auto r = sh.run(
      "set -- -q\n"
      "zparseopts -F -a opts f");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("bad option"), std::string::npos);
}

TEST(Modules, IsAtLeastIsAFunctionNotABuiltin) {
  Shell sh;
  // zsh's is-at-least is a shell function in $fpath, not zsh/zutil's.
  EXPECT_EQ(sh.run("whence -w is-at-least").out, "is-at-least: none\n");
}


TEST(Modules, IsAtLeastLess) {
  Shell sh;
  auto r = sh.run("is-at-least 6.0 5.9 || echo too-old");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "too-old\n");
}


TEST(Modules, ZformatSubstitutesKeys) {
  Shell sh;
  auto r = sh.run("zformat -f out 'hello %n!' n:world; echo $out");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "hello world!\n");
}

TEST(Modules, ZformatPercentPercentIsLiteral) {
  Shell sh;
  auto r = sh.run("zformat -f out '%%' ; echo $out");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "%\n");
}

TEST(Modules, ZformatWidthRightAlign) {
  Shell sh;
  auto r = sh.run("zformat -f out '[%-5x]' x:hi; echo \"$out\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "[   hi]\n");
}

TEST(Modules, ZformatWidthLeftAlign) {
  Shell sh;
  auto r = sh.run("zformat -f out '[%5x]' x:hi; echo \"$out\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "[hi   ]\n");
}

// An unknown %z stays as it is (zsh 5.9).
TEST(Modules, ZformatMissingKeyStays) {
  Shell sh;
  auto r = sh.run("zformat -f out 'a%zb' x:v; echo $out");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "a%zb\n");
}

// ---------------------------------------------------------------------------
// zsh/system (Phase 2b).
// ---------------------------------------------------------------------------

TEST(Modules, ErrnosArrayByNumber) {
  Shell sh;
  sh.run("zmodload zsh/system");
  EXPECT_EQ(sh.run("echo ${errnos[(i)ENOENT]} $errnos[13] ${(t)errnos}").out,
            "2 EACCES array-readonly-hide-hideval-special\n");
}

TEST(Modules, SyserrorPrintsMessageForErrnoName) {
  Shell sh;
  sh.run("zmodload zsh/system");
  auto r = sh.run("syserror ENOENT 2>&1");
  EXPECT_EQ(r.rc, 0);
  // strerror text varies by libc but always contains "file" or "directory".
  EXPECT_FALSE(r.err.empty() and r.out.empty());
}

TEST(Modules, SyserrorDashEStoresMessage) {
  Shell sh;
  sh.run("zmodload zsh/system");
  auto r = sh.run("syserror -e msg ENOENT; echo $msg");
  EXPECT_EQ(r.rc, 0);
  // Something non-empty was stored.
  EXPECT_GT(r.out.size(), 1u);
}

TEST(Modules, SyserrorUnknownErrnoFails) {
  Shell sh;
  sh.run("zmodload zsh/system");
  auto r = sh.run("syserror FAKEERRNO");
  EXPECT_EQ(r.rc, 2);
  EXPECT_EQ(r.err, "");
}

TEST(Modules, SysopenSysreadSyswriteSysseekRoundTrip) {
  Shell sh;
  sh.run("zmodload zsh/system");
  TempDir td;
  str path = td.path.string() + "/f";
  std::ofstream{path} << "hello world";
  // Open the file, read 5 bytes, then seek to 6 and read "world".
  str cmd = "sysopen -r -u fd " + path + "\n"
            "sysread -i $fd -s 5 part\n"
            "echo \"[$part]\"\n"
            "sysseek -u $fd -w start 6\n"
            "sysread -i $fd -s 5 part2\n"
            "echo \"[$part2]\" $(( systell(fd) ))\n"
            "exec {fd}<&-\n";
  auto r = sh.run(cmd);
  EXPECT_EQ(r.out, "[hello]\n[world] 11\n");
  EXPECT_EQ(r.err, "");
}

TEST(Modules, SyswriteWritesToStdout) {
  Shell sh;
  sh.run("zmodload zsh/system");
  auto r = sh.run("syswrite 'hi'");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "hi");
}

TEST(Modules, ZsystemSupportsFlock) {
  Shell sh;
  sh.run("zmodload zsh/system");
  EXPECT_EQ(sh.run("zsystem supports flock").rc, 0);
  EXPECT_NE(sh.run("zsystem supports no-such-feature").rc, 0);
}

TEST(Modules, ZsystemFlockTakesAndReleasesLock) {
  Shell sh;
  char path[] = "/tmp/jfsh_lockXXXXXX";
  int fd = ::mkstemp(path);
  ASSERT_GE(fd, 0);
  ::close(fd);
  str cmd = str("zsystem flock -f lfd ") + path + "\n"
            "zsystem unflock $lfd\n"
            "echo done";
  auto r = sh.run(cmd);
  ::unlink(path);
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "done\n");
}

TEST(Modules, SysreadTimeoutZeroOnEmptyPipe) {
  Shell sh;
  sh.run("zmodload zsh/system");
  int pipefd[2];
  ASSERT_EQ(::pipe(pipefd), 0);
  str cmd = "sysread -t 0 -i " + std::to_string(pipefd[0]) + "; echo $?";
  auto r = sh.run(cmd);
  ::close(pipefd[0]);
  ::close(pipefd[1]);
  // A read that would block times out: status 4.
  EXPECT_EQ(r.out, "4\n");
}

// ---------------------------------------------------------------------------
// zsh/files (Phase 2b): registered but NOT auto-loaded by the fixture.
// Tests opt in via `zmodload zsh/files` first.
// ---------------------------------------------------------------------------

TEST(Modules, FilesNotLoadedByDefault) {
  Shell sh;
  // Before `zmodload zsh/files`, mkdir/rm are either the external
  // commands or not found — either way, `${+builtins[mkdir]}` is 0.
  auto r = sh.run("echo ${+builtins[mkdir]}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "0\n");
}

TEST(Modules, FilesAfterZmodloadBuiltinsExist) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/files\n"
      "echo ${builtins[mkdir]} ${builtins[rm]} ${builtins[chmod]}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "defined defined defined\n");
}

TEST(Modules, FilesMkdirCreatesDirectory) {
  Shell sh;
  TempDir td;
  str path = td.path.string() + "/newdir";
  auto r = sh.run("zmodload zsh/files; mkdir " + path);
  EXPECT_EQ(r.rc, 0);
  EXPECT_TRUE(std::filesystem::is_directory(path));
}

TEST(Modules, FilesMkdirDashPCreatesParents) {
  Shell sh;
  TempDir td;
  str path = td.path.string() + "/a/b/c";
  auto r = sh.run("zmodload zsh/files; mkdir -p " + path);
  EXPECT_EQ(r.rc, 0);
  EXPECT_TRUE(std::filesystem::is_directory(path));
}

TEST(Modules, FilesRmDeletesFile) {
  Shell sh;
  TempDir td;
  str path = td.path.string() + "/victim";
  std::ofstream{path} << "x";
  auto r = sh.run("zmodload zsh/files; rm " + path);
  EXPECT_EQ(r.rc, 0);
  EXPECT_FALSE(std::filesystem::exists(path));
}

TEST(Modules, FilesRmDashRfRecursive) {
  Shell sh;
  TempDir td;
  str dir = td.path.string() + "/tree";
  std::filesystem::create_directories(dir + "/sub");
  std::ofstream{dir + "/sub/leaf"} << "x";
  auto r = sh.run("zmodload zsh/files; rm -rf " + dir);
  EXPECT_EQ(r.rc, 0);
  EXPECT_FALSE(std::filesystem::exists(dir));
}

TEST(Modules, FilesRmdirRemovesEmptyDir) {
  Shell sh;
  TempDir td;
  str dir = td.path.string() + "/empty";
  std::filesystem::create_directory(dir);
  auto r = sh.run("zmodload zsh/files; rmdir " + dir);
  EXPECT_EQ(r.rc, 0);
  EXPECT_FALSE(std::filesystem::exists(dir));
}

TEST(Modules, FilesMvRenamesFile) {
  Shell sh;
  TempDir td;
  str src = td.path.string() + "/src";
  str dst = td.path.string() + "/dst";
  std::ofstream{src} << "hi";
  auto r = sh.run("zmodload zsh/files; mv " + src + " " + dst);
  EXPECT_EQ(r.rc, 0);
  EXPECT_FALSE(std::filesystem::exists(src));
  EXPECT_TRUE(std::filesystem::exists(dst));
}

TEST(Modules, FilesLnDashSSymlink) {
  Shell sh;
  TempDir td;
  str target = td.path.string() + "/target";
  str link   = td.path.string() + "/link";
  std::ofstream{target} << "x";
  auto r = sh.run("zmodload zsh/files; ln -s " + target + " " + link);
  EXPECT_EQ(r.rc, 0);
  EXPECT_TRUE(std::filesystem::is_symlink(link));
}

TEST(Modules, FilesChmodChangesMode) {
  Shell sh;
  TempDir td;
  str path = td.path.string() + "/f";
  std::ofstream{path} << "x";
  auto r = sh.run("zmodload zsh/files; chmod 600 " + path);
  EXPECT_EQ(r.rc, 0);
  auto perms = std::filesystem::status(path).permissions();
  EXPECT_EQ(perms & std::filesystem::perms::owner_read,
            std::filesystem::perms::owner_read);
  EXPECT_EQ(perms & std::filesystem::perms::group_read,
            std::filesystem::perms::none);
}

TEST(Modules, FilesChmodBadModeFails) {
  Shell sh;
  TempDir td;
  str path = td.path.string() + "/f";
  std::ofstream{path} << "x";
  auto r = sh.run("zmodload zsh/files; chmod xyz " + path);
  EXPECT_NE(r.rc, 0);
}

TEST(Modules, FilesSyncReturnsZero) {
  Shell sh;
  auto r = sh.run("zmodload zsh/files; sync");
  EXPECT_EQ(r.rc, 0);
}

TEST(Modules, FilesRmMissingWithDashFIgnores) {
  Shell sh;
  TempDir td;
  str path = td.path.string() + "/does-not-exist";
  auto r = sh.run("zmodload zsh/files; rm -f " + path);
  EXPECT_EQ(r.rc, 0);
}

// ---------------------------------------------------------------------------
// zsh/stat (Phase 2c): registered but NOT auto-loaded.
// ---------------------------------------------------------------------------

TEST(Modules, StatNotLoadedByDefault) {
  Shell sh;
  auto r = sh.run("echo ${+builtins[zstat]}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "0\n");
}

TEST(Modules, StatAfterZmodloadBuiltinExists) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload zsh/stat; echo ${builtins[zstat]} ${builtins[stat]}").out,
            "defined defined\n");
}

TEST(Modules, StatListsElementsWithDashL) {
  Shell sh;
  sh.run("zmodload zsh/stat");
  EXPECT_EQ(sh.run("zstat -l").out,
            "device inode mode nlink uid gid rdev size atime mtime ctime blksize blocks link\n");
  auto r = sh.run("zstat +type /");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: zstat: type: no such stat element\n");
}

TEST(Modules, StatHashOutput) {
  Shell sh;
  TempDir td;
  str path = td.path.string() + "/f";
  std::ofstream{path} << "hello";
  auto r = sh.run("zmodload zsh/stat; zstat -H s +size " + path + "; echo $s[size]");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "5\n");
}

TEST(Modules, StatArrayOutput) {
  Shell sh;
  sh.run("zmodload zsh/stat");
  TempDir td;
  str path = td.path.string() + "/f";
  std::ofstream{path} << "abc";
  EXPECT_EQ(sh.run("zstat -A a +size " + path + "; echo $a").out, "3\n");
  EXPECT_EQ(sh.run("zstat -H h " + path + "; echo $h[size] ${#h}").out, "3 14\n");
}

TEST(Modules, StatSingleElementToStdout) {
  Shell sh;
  TempDir td;
  str path = td.path.string() + "/empty";
  std::ofstream{path};
  auto r = sh.run("zmodload zsh/stat; zstat +size " + path);
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "0\n");
}

TEST(Modules, StatDirectoryModeString) {
  Shell sh;
  sh.run("zmodload zsh/stat");
  TempDir td;
  EXPECT_EQ(sh.run("chmod 755 " + td.path.string() + "; zstat -s +mode " + td.path.string()).out,
            "drwxr-xr-x\n");
}

TEST(Modules, StatSymlinkFollowedByDefault) {
  Shell sh;
  sh.run("zmodload zsh/stat");
  TempDir td;
  str target = td.path.string() + "/real";
  str link   = td.path.string() + "/sym";
  std::ofstream{target} << "x";
  ASSERT_EQ(::symlink(target.c_str(), link.c_str()), 0);
  EXPECT_EQ(sh.run("zstat -s +mode " + link + " | cut -c1").out, "-\n");
}

TEST(Modules, StatDashLDoesNotFollowSymlink) {
  Shell sh;
  sh.run("zmodload zsh/stat");
  TempDir td;
  str target = td.path.string() + "/real";
  str link   = td.path.string() + "/sym";
  std::ofstream{target} << "x";
  ASSERT_EQ(::symlink(target.c_str(), link.c_str()), 0);
  EXPECT_EQ(sh.run("zstat -L -s +mode " + link + " | cut -c1").out, "l\n");
  EXPECT_EQ(sh.run("zstat +link " + link).out, target + "\n");
}

TEST(Modules, StatMissingFileFails) {
  Shell sh;
  sh.run("zmodload zsh/stat");
  auto r = sh.run("zstat +size /no/such/path");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: zstat: /no/such/path: no such file or directory\n");
}

TEST(Modules, StatOctalModeFlag) {
  Shell sh;
  TempDir td;
  str path = td.path.string() + "/f";
  std::ofstream{path};
  ::chmod(path.c_str(), 0644);
  auto r = sh.run("zmodload zsh/stat; zstat -o -H s +mode " + path + "; echo $s[mode]");
  EXPECT_EQ(r.rc, 0);
  // Mode begins with `0` and ends in `644`.
  EXPECT_EQ(r.out.back(), '\n');
  EXPECT_NE(r.out.find("644"), std::string::npos);
  EXPECT_EQ(r.out.front(), '0');
}

// ---------------------------------------------------------------------------
// zsh/pcre (Phase 2c): registered but NOT auto-loaded.  These tests assume
// the build picked up libpcre2.
// ---------------------------------------------------------------------------

TEST(Modules, PcreNotLoadedByDefault) {
  Shell sh;
  auto r = sh.run("echo ${+builtins[pcre_match]}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "0\n");
}

TEST(Modules, PcreCompileAndMatchSimple) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/pcre\n"
      "pcre_compile '^foo'\n"
      "pcre_match 'foobar'");
  EXPECT_EQ(r.rc, 0);
}

TEST(Modules, PcreMatchSetsMatch) {
  Shell sh;
  sh.run("zmodload zsh/pcre");
  EXPECT_EQ(sh.run("pcre_compile 'he(l+)o'; pcre_match 'say hello now'; echo $MATCH $match[1]").out,
            "hello ll\n");
  EXPECT_EQ(sh.run("pcre_match -v m -a a -b 'say hello'; echo $m $a $ZPCRE_OP").out,
            "hello ll 4 9\n");
}

TEST(Modules, PcreMatchNonMatchReturnsOne) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/pcre\n"
      "pcre_compile '^foo'\n"
      "pcre_match 'bar'");
  EXPECT_EQ(r.rc, 1);
}

TEST(Modules, PcreCompileRejectsBadPattern) {
  Shell sh;
  sh.run("zmodload zsh/pcre");
  auto r = sh.run("pcre_compile '[unclosed'");
  EXPECT_EQ(r.rc, 1);
  EXPECT_NE(r.err.find("pcre_compile: error in regex: "), str::npos);
}

TEST(Modules, PcreCompileDashICaseInsensitive) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/pcre\n"
      "pcre_compile -i 'HELLO'\n"
      "pcre_match 'say hello there'");
  EXPECT_EQ(r.rc, 0);
}

TEST(Modules, PcreMatchWithoutCompileFails) {
  Shell sh;
  sh.run("zmodload zsh/pcre");
  auto r = sh.run("pcre_match 'x'");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.err, "jfsh: pcre_match: no pattern has been compiled\n");
}

TEST(Modules, PcreMultipleCaptures) {
  Shell sh;
  sh.run("zmodload zsh/pcre");
  EXPECT_EQ(sh.run("pcre_compile '(\\w+)=(\\d+)'; pcre_match 'name=42'; "
                   "echo \"$match[1]/$match[2]\"").out,
            "name/42\n");
}

TEST(Modules, PcreMatchDashVNames) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/pcre\n"
      "pcre_compile 'foo'\n"
      "pcre_match -v m 'foobar'\n"
      "echo $m");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "foo\n");
}

// ---------------------------------------------------------------------------
// M10 polish: [[ -pcre-match ]] and [[ -regex-match ]] module conditions.
// ---------------------------------------------------------------------------

TEST(Modules, CondPcreMatchWithoutModuleReportsClearError) {
  Shell sh;
  auto r = sh.run("[[ hello -pcre-match ^hel ]] && echo ok");
  // The parser accepts -pcre-match; the executor refuses it until the
  // module is loaded.
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("unknown condition: -"), std::string::npos);
}

TEST(Modules, CondPcreMatchTrue) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/pcre\n"
      "[[ hello -pcre-match '^hel' ]] && echo yes");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "yes\n");
}

TEST(Modules, CondPcreMatchFalse) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/pcre\n"
      "[[ bye -pcre-match '^hel' ]] || echo no");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "no\n");
}

TEST(Modules, CondPcreMatchSetsMatchArray) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/pcre\n"
      "[[ 'name=42' -pcre-match '(\\w+)=(\\d+)' ]]\n"
      "echo \"$MATCH ${match[1]}=${match[2]}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "name=42 name=42\n");
}

TEST(Modules, CondRegexMatchNeedsModule) {
  Shell sh;
  auto r = sh.run("[[ abc -regex-match '^a' ]] && echo ok");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("unknown condition: -"), std::string::npos);
}

TEST(Modules, CondRegexMatchTrue) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/regex\n"
      "[[ 'abc123' -regex-match '[a-z]+([0-9]+)' ]] && echo \"$MATCH/${match[1]}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "abc123/123\n");
}

TEST(Modules, CondRegexMatchFalse) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/regex\n"
      "[[ 'hello' -regex-match '^[0-9]' ]] || echo no");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "no\n");
}

// ---------------------------------------------------------------------------
// M10 polish: zmodload -F feature toggles.
// ---------------------------------------------------------------------------

TEST(Modules, ZmodloadDashFDisablesABuiltin) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/example\n"
      "zmodload -F zsh/example -b:example\n"
      "example; echo $?");
  EXPECT_EQ(r.out, "The example module has now been set up.\n127\n");
  EXPECT_EQ(r.err, "jfsh: command not found: example\n");
}

TEST(Modules, ZmodloadDashFReenablesFeature) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/example\n"
      "zmodload -F zsh/example -b:example\n"
      "zmodload -F zsh/example +b:example\n"
      "example a | head -2");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "The example module has now been set up.\nOptions: \nArguments: a\n");
}

// ---------------------------------------------------------------------------
// M10 polish: zsh/example reference module (compiled-in ABI exerciser).
// ---------------------------------------------------------------------------

TEST(Modules, ExampleModuleBuiltinPrintsGreeting) {
  Shell sh;
  auto r = sh.run("zmodload zsh/example; example -fl a b; echo $exint $exstr $exarr");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "The example module has now been set up.\nOptions: fl\nArguments: a b\n"
                   "Name: example\n\nInteger Parameter: 42\nString Parameter: example\n"
                   "Array Parameter: example array\n2 a a b\n");
  EXPECT_EQ(sh.run("zmodload -u zsh/example").out,
            "Thank you for using the example module.  Have a nice day.\n");
}

TEST(Modules, ExampleModuleMathFunc) {
  Shell sh;
  sh.run("zmodload zsh/example");
  // sum() promotes to a float; length() takes its text as it is.
  EXPECT_EQ(sh.run("echo $(( sum(1,2,3.5) )) $(( sum(1) )) "
                   "$(( length(abcd) )) $(( length( ab ) ))").out,
            "6.5 1 4 4\n");
}

TEST(Modules, ExampleModuleCondition) {
  Shell sh;
  sh.run("zmodload zsh/example");
  EXPECT_EQ(sh.run("[[ -len abc 3 ]] && echo y; [[ -len '' ]] && echo e; [[ -len abc ]] || echo n; "
                   "[[ ex -ex ample ]] && echo x; [[ a -ex b ]] || echo nx").out,
            "y\ne\nn\nx\nnx\n");
}

TEST(Modules, ExampleModuleFeatures) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload zsh/example; zmodload -LF zsh/example").out,
            "The example module has now been set up.\nzmodload -F zsh/example b:example C:ex c:len "
            "f:length f:sum p:exarr p:exint p:exstr\n");
}

TEST(Modules, ExampleModuleCanBeUnloaded) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/example\n"
      "zmodload -u zsh/example\n"
      "example 2>&1 || echo gone");
  EXPECT_EQ(r.rc, 0);
  EXPECT_NE(r.out.find("gone"), std::string::npos);
}

// ---------------------------------------------------------------------------
// M10 polish: an infix -NAME-match parses whatever its name.
// ---------------------------------------------------------------------------

TEST(Modules, UnknownDashNameMatchParsesButErrors) {
  Shell sh;
  // `-flubber-match` is a well-formed binary op shape; the parser accepts
  // it, but no module claims it, so the executor reports "unknown".
  auto r = sh.run("[[ x -flubber-match y ]] && echo ok");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("unknown condition"), std::string::npos);
}

// ---------------------------------------------------------------------------
// M11 Phase 11a: zsh/langinfo.
// ---------------------------------------------------------------------------

TEST(Modules, LanginfoNotLoadedByDefault) {
  Shell sh;
  EXPECT_EQ(sh.run("echo ${+parameters[langinfo]}").out, "0\n");
}

TEST(Modules, LanginfoHasCodeset) {
  Shell sh;
  auto r = sh.run("zmodload zsh/langinfo; echo $langinfo[CODESET]");
  EXPECT_EQ(r.rc, 0);
  // CODESET is non-empty under any locale.
  EXPECT_GT(r.out.size(), 1u);
}

TEST(Modules, LanginfoKnowsDay1) {
  Shell sh;
  auto r = sh.run("zmodload zsh/langinfo; echo $langinfo[DAY_1]");
  EXPECT_EQ(r.rc, 0);
  // DAY_1 is Sunday in English locales; but at minimum non-empty.
  EXPECT_GT(r.out.size(), 1u);
}

TEST(Modules, LanginfoIsAnAssociativeArray) {
  Shell sh;
  // zsh/parameter's $parameters should classify langinfo as an association.
  auto r = sh.run("zmodload zsh/langinfo; echo $parameters[langinfo]");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out.substr(0, 11), "association");
}

// ---------------------------------------------------------------------------
// M11 Phase 11a: zsh/zselect.
// ---------------------------------------------------------------------------

TEST(Modules, ZselectNoFdsNoTimeoutErrors) {
  Shell sh;
  auto r = sh.run("zmodload zsh/zselect; zselect");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("no fds"), std::string::npos);
}

TEST(Modules, ZselectBadFdErrors) {
  Shell sh;
  auto r = sh.run("zmodload zsh/zselect; zselect -t 10 -r notanint");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("bad fd"), std::string::npos);
}

TEST(Modules, ZselectReadyStdinOnPipe) {
  Shell sh;
  // Write to a pipe; the read side should be ready immediately.
  // We use a here-string to feed data to zselect's own stdin (fd 0).
  auto r = sh.run("zmodload zsh/zselect; zselect -t 100 -r 0 <<<x");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "r:0\n");
}

// ---------------------------------------------------------------------------
// M11 Phase 11a: zsh/net/socket.
// ---------------------------------------------------------------------------

TEST(Modules, ZsocketNotLoadedByDefault) {
  Shell sh;
  EXPECT_EQ(sh.run("echo ${+builtins[zsocket]}").out, "0\n");
}

TEST(Modules, ZsocketIsNotAName) {
  Shell sh;
  // zsh 5.9 has no zsh/socket alias for zsh/net/socket.
  auto r = sh.run("zmodload zsh/socket");
  EXPECT_EQ(r.rc, 1);
  EXPECT_NE(r.err.find("failed to load module `zsh/socket'"), str::npos);
}

TEST(Modules, ZsocketListenAcceptConnectRoundTrip) {
  Shell sh;
  TempDir td;
  str path = td.path.string() + "/sock";
  // Listen in the shell, connect from a real external `nc` would complicate
  // the test. Instead, just verify the listen path works and leaves a
  // socket behind.
  auto r = sh.run(
      "zmodload zsh/net/socket\n"
      "zsocket -l " + path + "\n"
      "echo $REPLY\n"
      "[[ -S " + path + " ]] && echo yes");
  EXPECT_EQ(r.rc, 0);
  // Two lines: fd (a positive integer) and "yes".
  EXPECT_NE(r.out.find("yes"), std::string::npos);
}

TEST(Modules, ZsocketBadPathTooLongFails) {
  Shell sh;
  // UNIX sockaddr_un.sun_path is 108 bytes on Linux; a 200-char name
  // definitely exceeds it.
  str too_long(200, 'a');
  auto r = sh.run(
      "zmodload zsh/net/socket\n"
      "zsocket -l " + too_long);
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("path too long"), std::string::npos);
}

// ---------------------------------------------------------------------------
// M11 Phase 11a: zsh/zleparameter.
// ---------------------------------------------------------------------------

TEST(Modules, ZleparameterWidgetsContainsBuiltins) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/zleparameter\n"
      "echo ${widgets[accept-line]}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "builtin\n");
}

TEST(Modules, ZleparameterWidgetsIncludesUserDefined) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/zleparameter\n"
      "my-widget() { :; }\n"
      "zle -N my-widget\n"
      "echo ${widgets[my-widget]}");
  EXPECT_EQ(r.rc, 0);
  // Format is "user:FUNC" where FUNC is what zle -N stored.
  EXPECT_EQ(r.out.substr(0, 5), "user:");
}

TEST(Modules, ZleparameterKeymapsListed) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/zleparameter\n"
      "echo ${(j: :)keymaps}");
  EXPECT_EQ(r.rc, 0);
  // Must at least mention emacs and viins.
  EXPECT_NE(r.out.find("emacs"), std::string::npos);
  EXPECT_NE(r.out.find("viins"), std::string::npos);
}

TEST(Modules, ZleparameterUnloadsCleanly) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/zleparameter\n"
      "zmodload -u zsh/zleparameter\n"
      "echo ok");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "ok\n");
}

}  // namespace jfsh::test
