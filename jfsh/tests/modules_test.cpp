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
  auto r = sh.run("zmodload");
  EXPECT_EQ(r.rc, 0);
  EXPECT_NE(r.out.find("zsh/datetime"),  std::string::npos);
  EXPECT_NE(r.out.find("zsh/mathfunc"),  std::string::npos);
}

TEST(Modules, ZmodloadDashLShowsSourceableForm) {
  Shell sh;
  auto r = sh.run("zmodload -L");
  EXPECT_EQ(r.rc, 0);
  EXPECT_NE(r.out.find("zmodload zsh/datetime"), std::string::npos);
  EXPECT_NE(r.out.find("zmodload zsh/mathfunc"), std::string::npos);
}

TEST(Modules, ZmodloadDashERetursSuccessWhenLoaded) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload -e zsh/datetime").rc, 0);
  EXPECT_EQ(sh.run("zmodload -e zsh/not-a-real-module").rc, 1);
}

TEST(Modules, ZmodloadUnloadMarksModuleAbsent) {
  Shell sh;
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
  // Loading through the alias finds the real module.
  auto r = sh.run("zmodload -e date");
  EXPECT_EQ(r.rc, 0);
}

TEST(Modules, ZmodloadBadFlagReportsAndFails) {
  Shell sh;
  auto r = sh.run("zmodload -Z");
  EXPECT_EQ(r.rc, 1);
  EXPECT_NE(r.err.find("bad option: -Z"), std::string::npos);
}

TEST(Modules, ZmodloadLoadUnknownModuleFails) {
  Shell sh;
  auto r = sh.run("zmodload zsh/not-a-real-thing");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("failed to load module"), std::string::npos);
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
  auto r = sh.run("echo $((sqrt(4)))");
  EXPECT_EQ(r.rc, 0);
  // zsh renders a float with 17 sig figs; 2.0 round-trips as "2.".
  EXPECT_EQ(r.out, "2.\n");
}

TEST(Modules, MathfuncSinZero) {
  Shell sh;
  EXPECT_EQ(sh.run("echo $((sin(0)))").out, "0.\n");
}

TEST(Modules, MathfuncIntTruncatesTowardZero) {
  Shell sh;
  EXPECT_EQ(sh.run("echo $((int(3.7)))").out, "3\n");
  EXPECT_EQ(sh.run("echo $((int(-3.7)))").out, "-3\n");
}

TEST(Modules, MathfuncFloatPromotes) {
  Shell sh;
  // float(3) → 3.0, which prints as "3."
  EXPECT_EQ(sh.run("echo $((float(3)))").out, "3.\n");
}

TEST(Modules, MathfuncAtan2OfOneOne) {
  Shell sh;
  auto r = sh.run("echo $((atan2(1, 1)))");
  EXPECT_EQ(r.rc, 0);
  // atan2(1,1) = π/4 ≈ 0.78539816339744828
  EXPECT_NE(r.out.find("0.78539816339"), std::string::npos);
}

TEST(Modules, MathfuncHypotOfThreeFour) {
  Shell sh;
  EXPECT_EQ(sh.run("echo $((hypot(3,4)))").out, "5.\n");
}

TEST(Modules, MathfuncAbsHandlesIntAndFloat) {
  Shell sh;
  EXPECT_EQ(sh.run("echo $((abs(-5)))").out,    "5\n");
  EXPECT_EQ(sh.run("echo $((abs(-2.5)))").out,  "2.5\n");
}

TEST(Modules, MathfuncUnknownFunctionIsAnError) {
  Shell sh;
  auto r = sh.run("echo $((noSuchFunc(1)))");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("unknown math function"), std::string::npos);
}

TEST(Modules, MathfuncWrongArityIsAnError) {
  Shell sh;
  auto r = sh.run("echo $((sqrt(1,2)))");
  EXPECT_NE(r.rc, 0);
}

TEST(Modules, MathfuncCombinesWithOperatorsAndParams) {
  Shell sh;
  auto r = sh.run("x=9; echo $((sqrt(x) + 1))");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "4.\n");
}

TEST(Modules, MathfuncUnloadRemovesFunctions) {
  Shell sh;
  EXPECT_EQ(sh.run("zmodload -u zsh/mathfunc").rc, 0);
  auto r = sh.run("echo $((sqrt(4)))");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("unknown math function"), std::string::npos);
}

TEST(Modules, LetUsesMathfunc) {
  Shell sh;
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
  // Pass a fixed epoch time (midnight 1970-01-01 UTC) — but result depends
  // on local TZ, so just check the shape with %s (which is the input).
  auto r = sh.run("strftime '%s' 1700000000");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "1700000000\n");
}

TEST(Modules, StrftimeDashSStoresInVariable) {
  Shell sh;
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
  auto r = sh.run("strftime -Q '%s'");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("bad option"), std::string::npos);
}

TEST(Modules, EpochSecondsIsIntegerWithPositiveValue) {
  Shell sh;
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
  auto r = sh.run("zmodload -L");
  // After marking, zmodload -L lists zsh/pcre in autoload form.
  EXPECT_NE(r.out.find("zmodload -a zsh/pcre"), std::string::npos);
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
  // strftime is contributed by zsh/datetime.
  auto r = sh.run("echo ${builtins[strftime]}");
  EXPECT_EQ(r.rc, 0);
  // Value is the contributing module's name.
  EXPECT_EQ(r.out, "zsh/datetime\n");
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
  auto r = sh.run("echo ${modules[zsh/datetime]} ${modules[zsh/mathfunc]}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "loaded loaded\n");
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
  EXPECT_EQ(r.out, "f  x \n");
}

TEST(Modules, ZparseoptsWithArgument) {
  Shell sh;
  auto r = sh.run(
      "set -- -O logfile.txt -v\n"
      "zparseopts -a opts O: v\n"
      "echo \"${opts[1]} ${opts[2]} ${opts[3]} ${opts[4]}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "O logfile.txt v \n");
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

TEST(Modules, IsAtLeastExactVersion) {
  Shell sh;
  auto r = sh.run("ZSH_VERSION=5.9; is-at-least 5.9 && echo ok");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "ok\n");
}

TEST(Modules, IsAtLeastGreater) {
  Shell sh;
  auto r = sh.run("is-at-least 4.0 5.9 && echo yes");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "yes\n");
}

TEST(Modules, IsAtLeastLess) {
  Shell sh;
  auto r = sh.run("is-at-least 6.0 5.9 || echo too-old");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "too-old\n");
}

TEST(Modules, IsAtLeastNumericComponents) {
  Shell sh;
  // 5.10 > 5.9 when components compared as integers.
  auto r = sh.run("is-at-least 5.9 5.10 && echo yes");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "yes\n");
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
  auto r = sh.run("zformat -f out '[%5x]' x:hi; echo \"$out\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "[   hi]\n");
}

TEST(Modules, ZformatWidthLeftAlign) {
  Shell sh;
  auto r = sh.run("zformat -f out '[%-5x]' x:hi; echo \"$out\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "[hi   ]\n");
}

TEST(Modules, ZformatMissingKeyEmpty) {
  Shell sh;
  auto r = sh.run("zformat -f out 'a%zb' x:v; echo $out");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "ab\n");
}

// ---------------------------------------------------------------------------
// zsh/system (Phase 2b).
// ---------------------------------------------------------------------------

TEST(Modules, ErrnosHashHasCommonEntries) {
  Shell sh;
  auto r = sh.run("echo $errnos[EACCES] $errnos[ENOENT]");
  EXPECT_EQ(r.rc, 0);
  // The actual numbers vary by platform; just sanity-check both present.
  EXPECT_NE(r.out, " \n");
  EXPECT_NE(r.out.find(' '), std::string::npos);
}

TEST(Modules, SyserrorPrintsMessageForErrnoName) {
  Shell sh;
  auto r = sh.run("syserror ENOENT 2>&1");
  EXPECT_EQ(r.rc, 0);
  // strerror text varies by libc but always contains "file" or "directory".
  EXPECT_FALSE(r.err.empty() and r.out.empty());
}

TEST(Modules, SyserrorDashEStoresMessage) {
  Shell sh;
  auto r = sh.run("syserror -e msg ENOENT; echo $msg");
  EXPECT_EQ(r.rc, 0);
  // Something non-empty was stored.
  EXPECT_GT(r.out.size(), 1u);
}

TEST(Modules, SyserrorUnknownErrnoFails) {
  Shell sh;
  auto r = sh.run("syserror FAKEERRNO");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("unknown errno"), std::string::npos);
}

TEST(Modules, SysopenSysreadSyswriteSysseekRoundTrip) {
  Shell sh;
  char path[] = "/tmp/jfsh_sysioXXXXXX";
  int fd = ::mkstemp(path);
  ASSERT_GE(fd, 0);
  ASSERT_EQ(::write(fd, "hello world", 11), 11);
  ::close(fd);

  // Open the file, read 5 bytes, then seek back to 6 and read "world".
  str cmd = str("sysopen -o read -u fd ") + path + "\n"
            "sysread -i $fd -s 5 part\n"
            "echo \"[$part]\"\n"
            "sysseek -u $fd -w start 6\n"
            "sysread -i $fd -s 5 part2\n"
            "echo \"[$part2]\"\n"
            "exec {fd}>&- 2>/dev/null; true\n";
  auto r = sh.run(cmd);
  ::unlink(path);
  EXPECT_EQ(r.out, "[hello]\n[world]\n");
}

TEST(Modules, SyswriteWritesToStdout) {
  Shell sh;
  auto r = sh.run("syswrite 'hi'");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "hi");
}

TEST(Modules, ZsystemSupportsFlock) {
  Shell sh;
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

TEST(Modules, SysreadTimeoutZeroReturnsTwoOnEmpty) {
  Shell sh;
  int pipefd[2];
  ASSERT_EQ(::pipe(pipefd), 0);
  str cmd = "sysread -t 0 -i " + std::to_string(pipefd[0]) + "; echo $?";
  auto r = sh.run(cmd);
  ::close(pipefd[0]);
  ::close(pipefd[1]);
  // Status printed is 2 when the read would block.
  EXPECT_EQ(r.out, "2\n");
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
  // Each is now a module-contributed builtin → value is "zsh/files".
  EXPECT_EQ(r.out, "zsh/files zsh/files zsh/files\n");
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
  auto r = sh.run("zmodload zsh/stat; echo ${builtins[zstat]}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "zsh/stat\n");
}

TEST(Modules, StatListsElementsWithDashL) {
  Shell sh;
  auto r = sh.run("zmodload zsh/stat; zstat -l");
  EXPECT_EQ(r.rc, 0);
  EXPECT_NE(r.out.find("size"),  std::string::npos);
  EXPECT_NE(r.out.find("mode"),  std::string::npos);
  EXPECT_NE(r.out.find("mtime"), std::string::npos);
  EXPECT_NE(r.out.find("type"),  std::string::npos);
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
  TempDir td;
  str path = td.path.string() + "/f";
  std::ofstream{path} << "abc";
  auto r = sh.run("zmodload zsh/stat; zstat -A a +size +type " + path + "; echo $a[1] $a[2]");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "3 regular\n");
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

TEST(Modules, StatDirectoryTypeIsDirectory) {
  Shell sh;
  TempDir td;
  auto r = sh.run("zmodload zsh/stat; zstat -H s +type " + td.path.string() + "; echo $s[type]");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "directory\n");
}

TEST(Modules, StatSymlinkFollowedByDefault) {
  Shell sh;
  TempDir td;
  str target = td.path.string() + "/real";
  str link   = td.path.string() + "/sym";
  std::ofstream{target} << "x";
  ::symlink(target.c_str(), link.c_str());
  auto r = sh.run("zmodload zsh/stat; zstat -H s +type " + link + "; echo $s[type]");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "regular\n");
}

TEST(Modules, StatDashLDoesNotFollowSymlink) {
  Shell sh;
  TempDir td;
  str target = td.path.string() + "/real";
  str link   = td.path.string() + "/sym";
  std::ofstream{target} << "x";
  ::symlink(target.c_str(), link.c_str());
  auto r = sh.run("zmodload zsh/stat; zstat -L -H s +type " + link + "; echo $s[type]");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "link\n");
}

TEST(Modules, StatMissingFileFails) {
  Shell sh;
  auto r = sh.run("zmodload zsh/stat; zstat +size /no/such/path");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("No such"), std::string::npos);
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

TEST(Modules, PcreMatchSetsPcreMatch) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/pcre\n"
      "pcre_compile 'he(l+)o'\n"
      "pcre_match 'say hello now'\n"
      "echo $PCRE_MATCH ${pcre_match[1]}");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "hello ll\n");
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
  auto r = sh.run(
      "zmodload zsh/pcre\n"
      "pcre_compile '[unclosed'");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("failed to compile"), std::string::npos);
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
  auto r = sh.run(
      "zmodload zsh/pcre\n"
      "pcre_match 'x'");
  EXPECT_NE(r.rc, 0);
  EXPECT_NE(r.err.find("no pattern compiled"), std::string::npos);
}

TEST(Modules, PcreMultipleCaptures) {
  Shell sh;
  auto r = sh.run(
      "zmodload zsh/pcre\n"
      "pcre_compile '(\\w+)=(\\d+)'\n"
      "pcre_match 'name=42'\n"
      "echo \"${pcre_match[1]}/${pcre_match[2]}\"");
  EXPECT_EQ(r.rc, 0);
  EXPECT_EQ(r.out, "name/42\n");
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

}  // namespace jfsh::test
