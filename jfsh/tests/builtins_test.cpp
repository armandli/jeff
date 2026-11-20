// M7 builtins.  Expected output is what zsh 5.9 prints for the same input
// (`zsh -f -c`), with "jfsh: cmd: msg" for zsh's "zsh:cmd:1: msg".

#include <climits>
#include <cstdio>
#include <cstdlib>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <regex>
#include <string>

#include <gtest/gtest.h>

#include <jfsh/core/aliases.h>
#include <jfsh/core/options.h>
#include <jfsh/exec/executor.h>
#include <jfsh/history/history.h>
#include <shell_fixture.h>

namespace jfsh {
namespace {

using test::Run;
using test::Shell;
using test::TempDir;
namespace sf = s::filesystem;

constexpr sf::perms RW = sf::perms::owner_read | sf::perms::owner_write;

str out(Shell& sh, strv src) { return sh.run(src).out; }

// cd changes the test process's directory: put it back.
struct CwdGuard {
  str dir;
  CwdGuard() {
    char buf[PATH_MAX];
    dir = ::getcwd(buf, sizeof buf) ? buf : "/";
  }
  ~CwdGuard() { [[maybe_unused]] int r = ::chdir(dir.c_str()); }
};

// mkdtemp under /tmp may be a symlink (macOS); compare real paths.
str real(const sf::path& p) { return sf::canonical(p).string(); }

// ---- the option table --------------------------------------------------------

// B50: the table is indexed by OptIndex, so every row must sit at its index.
TEST(Options, TableIsInIndexOrder) {
  for (int i = 0; i < OPT_COUNT; ++i) {
    EXPECT_EQ(static_cast<int>(OPTION_INFO[i].index), i) << OPTION_INFO[i].name;
    EXPECT_NE(OPTION_INFO[i].name, nullptr);
  }
}

TEST(Options, LettersAndDollarHyphen) {
  Shell sh;
  EXPECT_EQ(out(sh, "echo $-"), "569X\n");
  EXPECT_EQ(out(sh, "set -e; echo $-; set +e"), "569Xe\n");
  EXPECT_EQ(out(sh, "setopt shoptionletters; echo $-; unsetopt shoptionletters"), "b\n");
  auto r = sh.run("set -j");
  EXPECT_EQ(r.err, "jfsh: set: bad option: -j\n");
  EXPECT_EQ(r.rc, 1);
}

// ---- set, setopt, unsetopt, emulate --------------------------------------

TEST(Set, ErrExitAndNoUnsetCanBeTurnedOn) {
  Shell sh;
  auto r = sh.run("set -e; false; echo not-reached");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.rc, 1);
  r = sh.run("set +e; set -o errexit; if false; then :; fi; echo ok; set +o errexit");
  EXPECT_EQ(r.out, "ok\n");
  // An expansion error ends a non-interactive shell (zsh's errflag).
  r = sh.run("set -u; echo $nope; echo after");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: nope: parameter not set\n");
  EXPECT_TRUE(sh.ctx.exiting);
  sh.run("set +u");
}

TEST(Set, XTrace) {
  Shell sh;
  auto r = sh.run("set -x; echo hi; set +x; echo bye");
  EXPECT_EQ(r.out, "hi\nbye\n");
  EXPECT_EQ(r.err, "+jfsh:1> echo hi\n+jfsh:1> set +x\n");
}

TEST(Set, PositionalParameters) {
  Shell sh;
  EXPECT_EQ(out(sh, "set -- a b c; echo $# $2"), "3 b\n");
  EXPECT_EQ(out(sh, "set -s c a b; echo $@"), "a b c\n");
  EXPECT_EQ(out(sh, "set -- -x; echo $1"), "-x\n");
  EXPECT_EQ(out(sh, "set - a; echo $1"), "a\n");
  EXPECT_EQ(out(sh, "set --; echo $#"), "0\n");
  EXPECT_EQ(out(sh, "set -A arr x y z; echo $arr[2]; set +A arr Q; echo $arr"), "y\nQ y z\n");
}

TEST(Set, OptionListings) {
  Shell sh;
  auto r = sh.run("set -o");
  EXPECT_NE(r.out.find("noaliases             off\n"), str::npos);
  EXPECT_NE(r.out.find("errexit               off\n"), str::npos);
  EXPECT_NE(r.out.find("nonomatch             off\n"), str::npos);
  EXPECT_EQ(s::count(r.out.begin(), r.out.end(), '\n'), OPT_COUNT);
  EXPECT_EQ(out(sh, "set +o | head -2"), "set +o noaliases\nset +o aliasfuncdef\n");
  EXPECT_EQ(out(sh, "set -o errexit; set -o | grep '^errexit'; set +o errexit"),
            "errexit               on\n");
}

TEST(Setopt, NamesNegationAndListing) {
  Shell sh;
  EXPECT_EQ(out(sh, "setopt"), "");
  EXPECT_EQ(out(sh, "setopt EXTENDED_GLOB ksharrays; setopt"), "extendedglob\nksharrays\n");
  EXPECT_EQ(out(sh, "unsetopt extendedglob; setopt no_ksh_arrays; setopt"), "");
  EXPECT_EQ(out(sh, "setopt nonomatch; setopt; setopt nomatch"), "nonomatch\n");
  EXPECT_EQ(out(sh, "setopt -m 'ext*'; setopt; unsetopt -m 'ext*'"),
            "extendedglob\nextendedhistory\n");
  auto r = sh.run("setopt nosuch; echo rc=$?");
  EXPECT_EQ(r.err, "jfsh: setopt: no such option: nosuch\n");
  EXPECT_EQ(r.out, "rc=1\n");
  r = sh.run("setopt interactive");
  EXPECT_EQ(r.err, "jfsh: setopt: can't change option: interactive\n");
  // KSH_OPTION_PRINT makes setopt list every option.
  r = sh.run("setopt kshoptionprint; setopt | head -1; unsetopt kshoptionprint");
  EXPECT_EQ(r.out, "noaliases             off\n");
}

TEST(Emulate, ChangesTheEmulationOptions) {
  Shell sh;
  EXPECT_EQ(out(sh, "emulate"), "zsh\n");
  EXPECT_EQ(out(sh, "emulate sh; emulate; x='a b'; for w in $x; do echo $w; done; emulate zsh"),
            "sh\na\nb\n");
  EXPECT_EQ(out(sh, "emulate"), "zsh\n");
  EXPECT_EQ(out(sh, "emulate sh -c 'echo $-'; emulate"), "b\nzsh\n");
  // Plain `emulate` leaves the non-emulation options alone; -R resets them.
  EXPECT_EQ(out(sh, "setopt histignoredups; emulate zsh; setopt"), "histignoredups\n");
  EXPECT_EQ(out(sh, "emulate -R zsh; setopt"), "");
  // -L: local to the function.
  EXPECT_EQ(out(sh, "f() { emulate -L sh; emulate; }; f; emulate"), "sh\nzsh\n");
  EXPECT_EQ(sh.run("emulate bogus").err, "jfsh: emulate: unknown emulation: bogus\n");
}

TEST(Shift, DropsPositionalsAndArrayElements) {
  Shell sh;
  EXPECT_EQ(out(sh, "set -- a b c d; shift; echo $@; shift 2; echo $@"), "b c d\nd\n");
  auto r = sh.run("set -- a b; shift 5; echo rc=$? $#");
  EXPECT_EQ(r.err, "jfsh: shift: shift count must be <= $#\n");
  EXPECT_EQ(r.out, "rc=1 2\n");
  r = sh.run("shift -1");
  EXPECT_EQ(r.err, "jfsh: shift: argument to shift must be non-negative\n");
  EXPECT_EQ(out(sh, "a=(1 2 3 4); shift 2 a; echo $a; shift -p a; echo $a"), "3 4\n3\n");
  EXPECT_EQ(out(sh, "set -- a b c; n=2; shift n; echo $@"), "c\n");
}

// ---- typeset, export, readonly, unset --------------------------------------

TEST(Typeset, PrintsDefinitions) {
  Shell sh;
  auto r = sh.run(
      "x='a b'; y=\"it's\"; z=plain; e=''; n=$'a\\nb'; arr=(1 '2 3' ''); typeset -A h; "
      "h=(k v 'a b' c); integer i=3; typeset -r ro=1; export ex=1; float f=1.5; "
      "typeset -p x y z e n arr h i ro ex f");
  EXPECT_EQ(r.out,
            "typeset x='a b'\n"
            "typeset y='it'\\''s'\n"
            "typeset z=plain\n"
            "typeset e=''\n"
            "typeset n=$'a\\nb'\n"
            "typeset -a arr=( 1 '2 3' '' )\n"
            "typeset -A h=( ['a b']=c [k]=v )\n"
            "typeset -i i=3\n"
            "typeset -r ro=1\n"
            "export ex=1\n"
            "typeset -E f=1.500000000e+00\n");
  r = sh.run("typeset -p nope");
  EXPECT_EQ(r.err, "jfsh: typeset: no such variable: nope\n");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(out(sh, "typeset -m '[xyz]'"), "x='a b'\ny='it'\\''s'\nz=plain\n");
  // The integer specials ($SECONDS, $LINENO, …) are listed too, as in zsh.
  EXPECT_EQ(out(sh, "typeset -i | grep -v -e '^[A-Z]' -e '^status='"), "i=3\n");
  EXPECT_EQ(out(sh, "typeset -i | grep -c -e '^SECONDS=' -e '^LINENO='"), "2\n");
  EXPECT_EQ(out(sh, "readonly -p | grep ro="), "typeset -r ro=1\n");
  EXPECT_EQ(out(sh, "export -p | grep ex="), "export ex=1\n");
}

TEST(Typeset, CaseAndUniqueStayInForce) {
  Shell sh;
  EXPECT_EQ(out(sh, "typeset -u U=abc; echo $U; U=xyz; echo $U"), "ABC\nXYZ\n");
  EXPECT_EQ(out(sh, "typeset -l L=ABC; L+=DEF; echo $L"), "abcdef\n");
  EXPECT_EQ(out(sh, "typeset -U u=(a b a c b); echo $u; u+=(c d a); echo $u"),
            "a b c\na b c d\n");
}

TEST(Typeset, FloatIsExponentForm) {
  Shell sh;
  EXPECT_EQ(out(sh, "float f=1.5; echo $f"), "1.500000000e+00\n");
}

TEST(Unset, RemovesParametersElementsAndFunctions) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=1; unset x; echo ${x-unset}"), "unset\n");
  EXPECT_EQ(out(sh, "a=(1 2 3); unset 'a[2]'; echo $a $#a"), "1 3 3\n");
  EXPECT_EQ(out(sh, "typeset -A h; h=(k v j w); unset 'h[k]'; echo ${(k)h}"), "j\n");
  EXPECT_EQ(out(sh, "f() { :; }; unset -f f; whence f; echo rc=$?"), "rc=1\n");
  auto r = sh.run("unset -f nosuch");
  EXPECT_EQ(r.err, "jfsh: unset: no such hash table element: nosuch\n");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(sh.run("unset nosuch").rc, 0);
  EXPECT_EQ(out(sh, "ab=1 ac=2 b=3; unset -m 'a*'; echo ${ab-u} ${ac-u} $b"), "u u 3\n");
  // A local stays local when unset: the global doesn't show through.
  EXPECT_EQ(out(sh, "g=out; f() { local g=in; unset g; echo ${g-gone}; }; f; echo $g"),
            "gone\nout\n");
  r = sh.run("readonly r=1; unset r");
  EXPECT_EQ(r.err, "jfsh: read-only variable: r\n");
  EXPECT_EQ(r.rc, 1);
}

// zsh ends a non-interactive shell on an assignment to a readonly
// parameter, as on any other error that sets errflag.
TEST(Unset, ReadonlyAssignmentAbortsTheScript) {
  Shell sh;
  auto r = sh.run("readonly x=5; x=6; echo after");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.err, "jfsh: read-only variable: x\n");
  EXPECT_TRUE(sh.ctx.exiting);
}

// ---- cd, pushd, popd, dirs, pwd ---------------------------------------------

TEST(Cd, ChangesDirectoryAndPwd) {
  CwdGuard g;
  Shell sh;
  TempDir t;
  str d = real(t.path);
  sf::create_directories(t.path / "sub" / "deep");
  sf::create_directories(t.path / "a1" / "x");
  sf::create_directories(t.path / "a2" / "x");
  t.file("f.txt", "", RW);
  EXPECT_EQ(out(sh, "cd " + d + "; cd sub; pwd; echo $OLDPWD; cd -; pwd"),
            d + "/sub\n" + d + "\n" + d + "\n");
  auto r = sh.run("cd /nonexist");
  EXPECT_EQ(r.err, "jfsh: cd: no such file or directory: /nonexist\n");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(sh.run("cd f.txt").err, "jfsh: cd: not a directory: f.txt\n");
  EXPECT_EQ(sh.run("cd a b c").err, "jfsh: cd: too many arguments\n");
  // cd old new: substitution in $PWD.
  EXPECT_EQ(out(sh, "cd " + d + "/a1/x; cd a1 a2; pwd"), d + "/a2/x\n");
  EXPECT_EQ(sh.run("cd zzz yyy").err, "jfsh: cd: string not in pwd: zzz\n");
  EXPECT_EQ(out(sh, "cd " + d + "; cd ''; pwd"), d + "\n");
  // CDPATH.
  EXPECT_EQ(out(sh, "CDPATH=" + d + "/sub; cd /; cd deep; pwd; unset CDPATH"),
            d + "/sub/deep\n");
  // ~+ and ~-.
  EXPECT_EQ(out(sh, "cd " + d + "; cd sub; echo ~+ ~-"), d + "/sub " + d + "\n");
}

TEST(Cd, LogicalAndPhysicalPaths) {
  CwdGuard g;
  Shell sh;
  TempDir t;
  str d = real(t.path);
  sf::create_directories(t.path / "r" / "s");
  sf::create_directory_symlink(t.path / "r" / "s", t.path / "l");
  EXPECT_EQ(out(sh, "cd " + d + "/l; pwd; pwd -P; cd ..; pwd; cd -P l; pwd"),
            d + "/l\n" + d + "/r/s\n" + d + "\n" + d + "/r/s\n");
}

TEST(Cd, ChpwdHookAndAutoPushd) {
  CwdGuard g;
  Shell sh;
  TempDir t;
  str d = real(t.path);
  sf::create_directories(t.path / "a");
  EXPECT_EQ(out(sh, "cd " + d + "; chpwd() { echo now $PWD; }; cd a; unset -f chpwd"),
            "now " + d + "/a\n");
  EXPECT_EQ(out(sh, "cd " + d + "; dirs -c; setopt autopushd; cd a; dirs -p; unsetopt autopushd"),
            d + "/a\n" + d + "\n");
}

TEST(Pushd, StackOperations) {
  CwdGuard g;
  Shell sh;
  TempDir t;
  str d = real(t.path);
  sf::create_directories(t.path / "a");
  sf::create_directories(t.path / "b");
  str a = d + "/a", b = d + "/b";
  EXPECT_EQ(out(sh, "cd " + d + "; pushd a; pushd ../b; dirs; dirs -v; echo $dirstack"),
            b + " " + a + " " + d + "\n0\t" + b + "\n1\t" + a + "\n2\t" + d + "\n" + a + " " + d + "\n");
  EXPECT_EQ(out(sh, "pushd; dirs"), a + " " + b + " " + d + "\n");
  EXPECT_EQ(out(sh, "pushd +1; dirs"), b + " " + d + " " + a + "\n");
  EXPECT_EQ(out(sh, "echo ~1 ~-0"), d + " " + a + "\n");
  EXPECT_EQ(out(sh, "popd; dirs; popd; dirs"), d + " " + a + "\n" + a + "\n");
  auto r = sh.run("popd");
  EXPECT_EQ(r.err, "jfsh: popd: directory stack empty\n");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(sh.run("cd -2").err, "jfsh: cd: no such entry in dir stack\n");
  // dirs abbreviates $HOME.
  EXPECT_EQ(out(sh, "HOME=" + d + "; dirs; dirs -l"), "~/a\n" + a + "\n");
}

// ---- echo, print, printf --------------------------------------------------------

TEST(Echo, EscapesAndOptions) {
  Shell sh;
  EXPECT_EQ(out(sh, "echo -n a; echo b"), "ab\n");
  EXPECT_EQ(out(sh, "echo -e 'a\\tb\\c'; echo x"), "a\tbx\n");
  EXPECT_EQ(out(sh, "echo -E 'a\\tb'; echo 'a\\tb'"), "a\\tb\na\tb\n");
  EXPECT_EQ(out(sh, "echo -- -n; echo -x"), "-- -n\n-x\n");
  EXPECT_EQ(out(sh, "echo '\\x41\\0101\\u00e9\\e'"), "AA\xc3\xa9\x1b\n");
  EXPECT_EQ(out(sh, "setopt bsdecho; echo 'a\\tb'; echo -e 'a\\tb'; unsetopt bsdecho"),
            "a\\tb\na\tb\n");
}

// A failed write is reported with its own errno, lowercased as zsh's, and
// status 1; nothing stays buffered to come out later.  Output keeps its
// order with what other commands write.  (zsh is silent, status 0, for a
// closed stdout: see TODO.md M2.)
TEST(Echo, WriteErrors) {
  Shell sh;
  for (str cmd : {"echo", "print", "printf"}) {
    auto r = sh.run(cmd + " x >&-; echo st=$?");
    EXPECT_EQ(r.err, "jfsh: " + cmd + ": write error: bad file descriptor\n");
    EXPECT_EQ(r.out, "st=1\n");
  }
  auto r = sh.run("(trap '' PIPE; sleep 0.2; echo a) | true");
  EXPECT_NE(r.err.find("write error: broken pipe"), str::npos) << r.err;
  // Output a builtin buffered for a closed fd is dropped, as zsh's.
  r = sh.run("type echo >&-; pwd >&-; echo y");
  EXPECT_EQ(r.out, "y\n");
  TempDir dir;
  str f = (dir.path / "f").string();
  EXPECT_EQ(out(sh, "{ echo -n a; printf b; print -n c; /bin/echo d; type echo; echo e; } > " + f
                    + "; cat " + f),
            "abcd\necho is a shell builtin\ne\n");
}

TEST(Print, Options) {
  Shell sh;
  EXPECT_EQ(out(sh, "print -r 'a\\tb'; print 'a\\tb'"), "a\\tb\na\tb\n");
  EXPECT_EQ(out(sh, "print -l a b; print -n a b; echo"), "a\nb\na b\n");
  EXPECT_EQ(out(sh, "print -N a b"), str("a\0b\0", 4));
  EXPECT_EQ(out(sh, "print -o c a b; print -O c a b; print -i -o B a C"),
            "a b c\nc b a\na B C\n");
  EXPECT_EQ(out(sh, "print -a -C 2 a b c d"), "a  b\nc  d\n");
  EXPECT_EQ(out(sh, "print -C 2 a b c d"), "a  c\nb  d\n");
  EXPECT_EQ(out(sh, "print -f '%s-%s\\n' a b c"), "a-b\nc-\n");
  EXPECT_EQ(out(sh, "print -u2 err 2>&1"), "err\n");
  EXPECT_EQ(out(sh, "print -R -n x; print - -n"), "x-n\n");
  EXPECT_EQ(out(sh, "print -P '%%x'"), "%x\n");
  EXPECT_EQ(out(sh, "print -m 'a*' ab cd ac"), "ab ac\n");
  EXPECT_EQ(out(sh, "print -v v hello; echo $v"), "hello\n");
  EXPECT_EQ(out(sh, "HOME=/h; print -D /h/x"), "~/x\n");
  EXPECT_EQ(sh.run("print -u").err, "jfsh: print: argument expected: -u\n");
}

TEST(Printf, Conversions) {
  Shell sh;
  EXPECT_EQ(out(sh, "printf '%s|%5s|%-5s|%.2s\\n' a b c defg"), "a|    b|c    |de\n");
  EXPECT_EQ(out(sh, "printf '%d %i %x %X %o %5.2f %e %g %c %%\\n' 42 -3 255 255 8 3.14159 1234.5 0.0001 xyz"),
            "42 -3 ff FF 10  3.14 1.234500e+03 0.0001 x %\n");
  EXPECT_EQ(out(sh, "printf '%5d|%-5d|%05d|%+d|% d\\n' 1 2 3 4 5"), "    1|2    |00003|+4| 5\n");
  EXPECT_EQ(out(sh, "printf '%#x %#o %.3d\\n' 255 8 7"), "0xff 010 007\n");
  EXPECT_EQ(out(sh, "printf '%*d|%-*d|\\n' 4 1 3 2"), "   1|2  |\n");
  EXPECT_EQ(out(sh, "printf '%d\\n' \"'A\" 1.5 0x1f 010"), "65\n1\n31\n10\n");
  EXPECT_EQ(out(sh, "printf '\\x41\\101\\n'"), "AA\n");
}

TEST(Printf, ArgumentsAndQuoting) {
  Shell sh;
  EXPECT_EQ(out(sh, "printf '%s\\n' a b c"), "a\nb\nc\n");
  EXPECT_EQ(out(sh, "printf '%s %s\\n' a b c"), "a b\nc \n");
  EXPECT_EQ(out(sh, "printf 'x\\n' extra"), "x\n");
  EXPECT_EQ(out(sh, "printf '%2$s %1$s\\n' a b"), "b a\n");
  EXPECT_EQ(out(sh, "printf '%b|' 'a\\tb\\c' more; echo"), "a\tb\n");
  EXPECT_EQ(out(sh, "printf '%q\\n' 'a b' \"it's\" '' '$x'"), "a\\ b\nit\\'s\n''\n\\$x\n");
  EXPECT_EQ(out(sh, "printf -v var '%s=%d' k 3; echo $var"), "k=3\n");
  auto r = sh.run("printf");
  EXPECT_EQ(r.err, "jfsh: printf: not enough arguments\n");
  EXPECT_EQ(r.rc, 1);
  r = sh.run("printf '%z\\n' 1");
  EXPECT_EQ(r.err, "jfsh: printf: %z: invalid directive\n");
  EXPECT_EQ(r.rc, 1);
}

// ---- read -----------------------------------------------------------------------

// At the end of input with nothing read, `read` still empties its names
// (status 1), as zsh; a timeout leaves them.
TEST(Read, EndOfInputEmptiesTheNames) {
  Shell sh;
  EXPECT_EQ(out(sh, "x=5; read x </dev/null; echo \"[$x] $?\"; a=1 b=2; read a b </dev/null; "
                    "echo \"[$a][$b]\"; REPLY=r; read </dev/null; echo \"[$REPLY]\""),
            "[] 1\n[][]\n[]\n");
  EXPECT_EQ(out(sh, "arr=(1 2); read -A arr </dev/null; echo \"${#arr} [$arr]\"; "
                    "z=9; read -d: z </dev/null; echo \"z[$z]\"; "
                    "seq 1 3 | while read x; do :; done; echo \"[$x]\""),
            "1 []\nz[]\n[]\n");
  EXPECT_EQ(out(sh, "t=9; sleep 1 | { read -t 0.2 t; echo \"t[$t] $?\"; }"), "t[9] 1\n");
  // -k gives what it got, -q `n` with status 2.
  EXPECT_EQ(out(sh, "k=z; read -u0 -k 3 k </dev/null; echo \"st=$? [$k]\"; "
                    "printf ab | { k=z; read -u0 -k 3 k; echo \"st=$? [$k]\"; }; "
                    "k=z; read -u0 -q k </dev/null; echo \"st=$? [$k]\""),
            "st=1 []\nst=1 [ab]\nst=2 [n]\n");
}

TEST(Read, MoreOptions) {
  Shell sh;
  EXPECT_EQ(out(sh, "read -k 3 x <<< abcdef; echo $x"), "abc\n");
  EXPECT_EQ(out(sh, "read -k x <<< zq; echo $x"), "z\n");
  EXPECT_EQ(out(sh, "read -q x <<< y; echo rc=$? $x; read -q x <<< n; echo rc=$? $x"),
            "rc=0 y\nrc=1 n\n");
  EXPECT_EQ(out(sh, "printf 'a,b;c' | { read -d ';' x; echo $x rc=$?; }"), "a,b rc=0\n");
  EXPECT_EQ(out(sh, "read -e <<< line1; read -E y <<< zz; echo y=$y"), "line1\nzz\ny=zz\n");
  EXPECT_EQ(out(sh, "read 'x?prompt: ' <<< q; echo \"<$x>\""), "<q>\n");
  EXPECT_EQ(out(sh, "exec 9<<< fromfd; read -u 9 w; exec 9<&-; echo $w"), "fromfd\n");
  EXPECT_EQ(out(sh, "sleep 0.3 | { read -t 0.05 x; echo rc=$?; }"), "rc=1\n");
  EXPECT_EQ(sh.run("read -z").err, "jfsh: read: -z: not implemented\n");
}

// ---- ., source, eval, exec, exit, return ------------------------------------

TEST(Source, RunsAFileInThisShell) {
  CwdGuard g;
  Shell sh;
  TempDir t;
  str d = real(t.path);
  t.file("src.zsh", "echo in-src $# $1 $0\nsrcvar=set\nreturn 4\necho no\n", RW);
  auto r = sh.run("source " + d + "/src.zsh p q; echo rc=$? $# $srcvar");
  EXPECT_EQ(r.out, "in-src 2 p " + d + "/src.zsh\nrc=4 0 set\n");
  EXPECT_EQ(out(sh, "cd " + d + "; source src.zsh; . ./src.zsh x"), "in-src 0 src.zsh\nin-src 1 x ./src.zsh\n");
  // `.` searches $PATH, not the current directory.
  r = sh.run(". src.zsh; echo rc=$?");
  EXPECT_EQ(r.err, "jfsh: .: no such file or directory: src.zsh\n");
  EXPECT_EQ(r.out, "rc=127\n");
  EXPECT_EQ(out(sh, "PATH=" + d + ":$PATH; cd /; . src.zsh"), "in-src 0 src.zsh\n");
  EXPECT_EQ(sh.run("source /nofile").err, "jfsh: source: no such file or directory: /nofile\n");
  // return in a file sourced from a function ends only the file.
  EXPECT_EQ(out(sh, "f() { source " + d + "/src.zsh >/dev/null; echo f-goes-on $?; }; f"),
            "f-goes-on 4\n");
}

TEST(Source, AliasesApplyFromTheNextLine) {
  CwdGuard g;
  Shell sh;
  TempDir t;
  t.file("al.zsh", "alias x=echo\nx hi\n", RW);
  EXPECT_EQ(out(sh, "source " + real(t.path) + "/al.zsh"), "hi\n");
}

TEST(Eval, RunsItsArguments) {
  Shell sh;
  EXPECT_EQ(out(sh, "eval 'echo e1;' 'echo e2'; eval 'x=5'; echo $x"), "e1\ne2\n5\n");
  EXPECT_EQ(sh.run("eval").rc, 0);
  EXPECT_EQ(sh.run("eval false").rc, 1);
  EXPECT_EQ(out(sh, "eval 'echo $((1+1))'"), "2\n");
}

TEST(Exec, RedirectionsAloneArePermanent) {
  Shell sh;
  TempDir t;
  str f = real(t.path) + "/o.txt";
  EXPECT_EQ(out(sh, "exec 9>" + f + "; echo to9 >&9; exec 9>&-; cat " + f), "to9\n");
}

// Runs `jfsh -c src` (fds 3 and up are free there, unlike in this test
// process) and returns its output and status.
s::pair<str, int> run_binary(strv src) {
  str cmd = str(JFSH_BINARY) + " --norouter -c '" + str(src) + "' 2>&1";
  s::FILE* p = ::popen(cmd.c_str(), "r");
  if (not p) return {"", -1};
  str o;
  char buf[256];
  while (s::fgets(buf, sizeof buf, p)) o += buf;
  int st = ::pclose(p);
  return {o, WIFEXITED(st) ? WEXITSTATUS(st) : 128 + WTERMSIG(st)};
}

// The signal self-pipe lives on a high fd: `exec 3<…` used to close its
// read end, and the next SIGCHLD killed the shell with SIGPIPE.
TEST(Exec, LowFdsAreTheUsers) {
  auto [o, rc] = run_binary("exec 3>&- 4>&-; exec 3</dev/null 4>/dev/null; sleep 0.01 & wait; echo ok");
  EXPECT_EQ(o, "ok\n");
  EXPECT_EQ(rc, 0);
  // A here-string whose pipe lands on the target fd itself, and is gone
  // again after the command.
  auto [o2, rc2] = run_binary("exec 3>&-; cat 3<<<a <&3; echo x >&3");
  EXPECT_EQ(o2.substr(0, 2), "a\n");
  EXPECT_NE(o2.find("jfsh: 3: bad file descriptor"), str::npos) << o2;
  EXPECT_EQ(rc2, 1);
}

// A trap runs while `read` blocks, not once it returns.
TEST(Read, TrapsRunWhileItBlocks) {
  TempDir dir;
  str f = (dir.path / "f").string();
  auto [o, rc] = run_binary("trap \"echo T >" + f + "\" USR1; { kill -USR1 $$; sleep 0.5; "
                            "[[ -e " + f + " ]] && echo seen; } | { read x; echo got $x; }");
  EXPECT_EQ(o, "got seen\n");
  EXPECT_EQ(rc, 0);
  // A trap ends `read -t` as a timeout would, as zsh.
  auto [o2, rc2] = run_binary("zmodload zsh/datetime; t=$EPOCHREALTIME; trap : USR1; "
                              "(sleep 0.3; kill -USR1 $$) & "
                              "{ read -t 2 x; echo rc=$? $(( EPOCHREALTIME - t < 1 )); } "
                              "< <(sleep 3)");
  EXPECT_EQ(o2, "rc=1 1\n");
}

TEST(Exec, ReplacesTheProcess) {
  Shell sh;
  // In a subshell, so the test process isn't the one replaced.
  EXPECT_EQ(out(sh, "(exec echo replaced; echo not-here); echo after"), "replaced\nafter\n");
  EXPECT_EQ(out(sh, "(x=1 exec sh -c 'echo $x $0') "), "1 sh\n");
  EXPECT_EQ(out(sh, "(exec -a NAME sh -c 'echo $0')"), "NAME\n");
  auto r = sh.run("(exec nosuchcmd; echo not-here)");
  EXPECT_EQ(r.err, "jfsh: command not found: nosuchcmd\n");
  EXPECT_EQ(r.out, "");
  EXPECT_EQ(r.rc, 127);
}

TEST(Exit, StatusIsArithmeticModulo256) {
  Shell sh;
  EXPECT_EQ(sh.run("exit 300").rc, 44);
  EXPECT_EQ(sh.run("x=3; exit x+1").rc, 4);
  auto r = sh.run("exit 1 2");
  EXPECT_EQ(r.err, "jfsh: exit: too many arguments\n");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(out(sh, "f() { return 3; echo no; }; f; echo $?"), "3\n");
}

// ---- precommand modifiers -------------------------------------------------------

TEST(Precommand, CommandBuiltinNoglob) {
  Shell sh;
  EXPECT_EQ(out(sh, "echo() { print func; }; builtin echo real; echo x; unset -f echo"),
            "real\nfunc\n");
  EXPECT_EQ(out(sh, "ls() { echo func; }; command ls -d /; unset -f ls"), "/\n");
  auto r = sh.run("builtin nosuch");
  EXPECT_EQ(r.err, "jfsh: no such builtin: nosuch\n");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(out(sh, "FOO=bar command env | grep '^FOO='"), "FOO=bar\n");
  EXPECT_EQ(sh.run("command nosuchcmd").rc, 127);
  TempDir t;
  t.file("a1", "", RW);
  EXPECT_EQ(out(sh, "noglob echo " + real(t.path) + "/a*"), real(t.path) + "/a*\n");
  EXPECT_EQ(out(sh, "echo " + real(t.path) + "/a*"), real(t.path) + "/a1\n");
}

// ---- aliases ----------------------------------------------------------------------

TEST(Alias, ExpandsInCommandPosition) {
  Shell sh;
  sh.run("alias ll='echo LL' e=echo");
  EXPECT_EQ(out(sh, "ll x"), "LL x\n");
  EXPECT_EQ(out(sh, "echo ll"), "ll\n");                 // not a command word
  EXPECT_EQ(out(sh, "X=1 ll y"), "LL y\n");              // after an assignment
  EXPECT_EQ(out(sh, "true && ll z | cat"), "LL z\n");
  // Quoting the word stops expansion.
  EXPECT_EQ(sh.run("\\e hi").rc, 127);
  EXPECT_EQ(sh.run("'e' hi").rc, 127);
  // Text with several commands.
  EXPECT_EQ(out(sh, "alias two='echo one; echo two'"), "");
  EXPECT_EQ(out(sh, "two"), "one\ntwo\n");
  // Reserved words can be aliased.
  EXPECT_EQ(out(sh, "alias for=echo"), "");
  EXPECT_EQ(out(sh, "for hi"), "hi\n");
}

TEST(Alias, RecursionTrailingBlankGlobalSuffix) {
  Shell sh;
  TempDir t;
  str d = real(t.path);
  t.file("f.txt", "file body\n", RW);
  sh.run("alias a=b b=a ls='ls -d' l='echo ' d=DIR; alias -g G='| wc -l'; alias -s txt=cat");
  auto r = sh.run("a");
  EXPECT_EQ(r.err, "jfsh: command not found: a\n");
  EXPECT_EQ(out(sh, "ls /"), "/\n");               // ls inside its own text stays ls
  EXPECT_EQ(out(sh, "l d"), "DIR\n");              // the blank makes `d` an alias too
  EXPECT_EQ(out(sh, "echo a b G | tr -d ' '"), "1\n");
  EXPECT_EQ(out(sh, d + "/f.txt"), "file body\n");
}

TEST(Alias, ParsedWholeForDashCLineByLineForScripts) {
  Shell sh;
  // zsh -c parses the whole string first, so an alias it defines doesn't
  // apply in it; a script is parsed a line at a time. The alias names must
  // not be commands on $PATH (x-cmd installs an `x`).
  EXPECT_EQ(sh.run("alias jfsh_ax=echo\njfsh_ax hi").rc, 127);
  sh.run("unalias jfsh_ax");
  EXPECT_EQ(sh.run_script("alias jfsh_ax=echo\njfsh_ax hi\n").out, "hi\n");
  // Function bodies keep the aliases of when they were defined.
  EXPECT_EQ(sh.run_script("f() { jfsh_ay in-f; }\nalias jfsh_ay=echo\nf\n").rc, 127);
  EXPECT_EQ(sh.run_script("unsetopt aliases\njfsh_ax hi\nsetopt aliases\njfsh_ax back\n").out, "back\n");
}

TEST(Alias, ListingAndUnalias) {
  Shell sh;
  sh.run("alias q=\"it's\" r='a b'; alias -g G=x; alias -s txt=cat");
  EXPECT_EQ(out(sh, "alias"), "G=x\nq='it'\\''s'\nr='a b'\n");
  EXPECT_EQ(out(sh, "alias -g; alias -s"), "G=x\ntxt=cat\n");
  EXPECT_EQ(out(sh, "alias -L"), "alias -g G=x\nalias q='it'\\''s'\nalias r='a b'\nalias -s txt=cat\n");
  EXPECT_EQ(out(sh, "alias q; alias -m 'r*'"), "q='it'\\''s'\nr='a b'\n");
  EXPECT_EQ(sh.run("alias nosuch").rc, 1);
  // Quoted: a global alias expands anywhere, in unalias's arguments too.
  auto r = sh.run("unalias 'G' nosuch");
  EXPECT_EQ(r.err, "jfsh: unalias: no such hash table element: nosuch\n");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(out(sh, "unalias -s txt; unalias -m 'q*'; alias; alias -s"), "r='a b'\n");
  EXPECT_EQ(out(sh, "unalias -a; alias"), "");
}

// ---- whence, which, type, command -v, hash --------------------------------------

TEST(Whence, Styles) {
  Shell sh;
  sh.run("f() { :; }; alias ll='ls -l'");
  str ls = out(sh, "command -v ls");
  ASSERT_FALSE(ls.empty());
  ls.pop_back();
  EXPECT_EQ(out(sh, "whence ll f echo if ls; whence nosuch; echo rc=$?"),
            "ls -l\nf\necho\nif\n" + ls + "\nrc=1\n");
  auto r = sh.run("whence -v ll f echo if ls nosuch");
  EXPECT_EQ(r.out, "ll is an alias for ls -l\nf is a shell function from jfsh\n"
                   "echo is a shell builtin\nif is a reserved word\nls is " + ls + "\nnosuch not found\n");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(out(sh, "type echo"), "echo is a shell builtin\n");
  EXPECT_EQ(out(sh, "whence -c ll f echo if"),
            "ll: aliased to ls -l\nf () {\n\t:\n}\necho: shell built-in command\nif: shell reserved word\n");
  EXPECT_EQ(out(sh, "which echo nosuch; echo rc=$?"), "echo: shell built-in command\nnosuch not found\nrc=1\n");
  EXPECT_EQ(out(sh, "whence -w ll f echo if ls nosuch noglob '[['"),
            "ll: alias\nf: function\necho: builtin\nif: reserved\nls: command\nnosuch: none\n"
            "noglob: builtin\n[[: reserved\n");
  EXPECT_EQ(out(sh, "command -v ll f echo if"), "alias ll='ls -l'\nf\necho\nif\n");
  EXPECT_EQ(out(sh, "command -V echo"), "echo is a shell builtin\n");
  EXPECT_EQ(out(sh, "whence -p echo | grep -c /echo"), "1\n");
  EXPECT_EQ(out(sh, "whence -f f"), "f () {\n\t:\n}\n");
  EXPECT_EQ(out(sh, "alias -g G=x; whence -v G"), "G is a global alias for x\n");
}

TEST(Hash, CommandTable) {
  Shell sh;
  EXPECT_EQ(out(sh, "hash -r; hash foo=/bin/ls; hash"), "foo=/bin/ls\n");
  EXPECT_EQ(out(sh, "hash -r; hash | wc -l | tr -d ' '"), "0\n");
  auto r = sh.run("hash nosuchcmd");
  EXPECT_EQ(r.err, "jfsh: hash: no such command: nosuchcmd\n");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(out(sh, "hash -r; hash sh; hash | cut -d= -f1"), "sh\n");
  EXPECT_EQ(out(sh, "rehash; hash"), "");
  // A hashed path is what runs.
  EXPECT_EQ(out(sh, "hash -r; hash zzecho=/bin/echo; whence zzecho"), "/bin/echo\n");
}

// ---- ulimit, umask, times ------------------------------------------------------

TEST(Ulimit, ShowsAndSetsLimits) {
  struct rlimit saved{};
  ASSERT_EQ(::getrlimit(RLIMIT_NOFILE, &saved), 0);
  Shell sh;
  EXPECT_EQ(out(sh, "ulimit -n 200; ulimit -n"), "200\n");
  EXPECT_EQ(out(sh, "ulimit -Sn"), "200\n");
  auto r = sh.run("ulimit -a");
  EXPECT_NE(r.out.find("-n: file descriptors                200\n"), str::npos);
  EXPECT_NE(r.out.find("-t: cpu time (seconds)              "), str::npos);
  if (saved.rlim_max != RLIM_INFINITY) {
    r = sh.run("ulimit -n " + s::to_string(saved.rlim_max + 1));
    EXPECT_EQ(r.err, "jfsh: ulimit: value exceeds hard limit\n");
  }
  EXPECT_EQ(sh.run("ulimit -Z").err, "jfsh: ulimit: bad option: -Z\n");
  EXPECT_EQ(out(sh, "ulimit -t -n"), "-t: cpu time (seconds)              " +
            out(sh, "ulimit -t") + "-n: file descriptors                200\n");
  ::setrlimit(RLIMIT_NOFILE, &saved);
}

TEST(Umask, OctalAndSymbolic) {
  mode_t saved = ::umask(022);
  Shell sh;
  EXPECT_EQ(out(sh, "umask; umask -S"), "022\nu=rwx,g=rx,o=rx\n");
  EXPECT_EQ(out(sh, "umask 027; umask; umask -S"), "027\nu=rwx,g=rx,o=\n");
  EXPECT_EQ(out(sh, "umask u=rwx,g=rx,o=; umask; umask g+w; umask; umask a-x; umask"),
            "027\n007\n117\n");
  auto r = sh.run("umask 99");
  EXPECT_EQ(r.err, "jfsh: umask: bad umask\n");
  EXPECT_EQ(r.rc, 1);
  ::umask(saved);
}

TEST(Times, PrintsFourTimes) {
  Shell sh;
  str o = out(sh, "times");
  EXPECT_TRUE(s::regex_match(o, s::regex(R"(\d+m\d+\.\d\ds \d+m\d+\.\d\ds\n\d+m\d+\.\d\ds \d+m\d+\.\d\ds\n)")))
      << o;
}

// ---- functions, autoload, trap listing ---------------------------------------------

TEST(Functions, PrintsDefinitionsAsZshDoes) {
  Shell sh;
  sh.run(
      "f() { echo hi; ls | wc -l && echo a || echo b; x=1 y=(a b) cmd >out 2>&1 <in; "
      "if true; then echo t; elif false; then :; else echo e; fi; for i in 1 2; do echo $i; done; "
      "for ((i=0;i<3;i++)) { echo $i }; while true; do break; done; "
      "case $x in (a|b) echo ab ;; c) echo c ;& *) ;; esac; (cd /; ls); { echo g; }; "
      "repeat 3 echo r; [[ -n $x && $y == a* ]]; (( x++ )); sleep 1 & ; g() { echo inner; }; "
      "select s in a b; do break; done; { a } always { b }; local z=3; ! true; echo \"q $x\" 's' }");
  EXPECT_EQ(out(sh, "functions f"),
            "f () {\n"
            "\techo hi\n"
            "\tls | wc -l && echo a || echo b\n"
            "\tx=1 y=(a b) cmd > out 2>&1 < in\n"
            "\tif true\n\tthen\n\t\techo t\n\telif false\n\tthen\n\t\t:\n\telse\n\t\techo e\n\tfi\n"
            "\tfor i in 1 2\n\tdo\n\t\techo $i\n\tdone\n"
            "\tfor ((i=0; i<3; i++)) do\n\t\techo $i\n\tdone\n"
            "\twhile true\n\tdo\n\t\tbreak\n\tdone\n"
            "\tcase $x in\n\t\t(a|b) echo ab ;;\n\t\t(c) echo c ;&\n\t\t(*)  ;;\n\tesac\n"
            "\t(\n\t\tcd /\n\t\tls\n\t)\n"
            "\t{\n\t\techo g\n\t}\n"
            "\trepeat 3\n\tdo\n\t\techo r\n\tdone\n"
            "\t[[ -n $x && $y == a* ]]\n"
            "\t(( x++ ))\n"
            "\tsleep 1 &\n"
            "\tg () {\n\t\techo inner\n\t}\n"
            "\tselect s in a b\n\tdo\n\tbreak\n\tdone\n"
            "\t{\n\t\ta\n\t} always {\n\t\tb\n\t}\n"
            "\tlocal z=3 \n"
            "\t! true\n"
            "\techo \"q $x\" 's'\n"
            "}\n");
}

TEST(Functions, HereDocumentsListingAndMatching) {
  Shell sh;
  sh.run("h() { cat <<EOF >out\nx $y\nEOF\necho after; }");
  EXPECT_EQ(out(sh, "functions h"), "h () {\n\tcat <<EOF > out\nx $y\nEOF\n\techo after\n}\n");
  sh.run("unset -f h; g() { :; }; f() { case $x in (a) echo 1; echo 2 ;; esac; }");
  EXPECT_EQ(out(sh, "functions"),
            "f () {\n\tcase $x in\n\t\t(a) echo 1\n\t\t\techo 2 ;;\n\tesac\n}\ng () {\n\t:\n}\n");
  EXPECT_EQ(out(sh, "functions +"), "f\ng\n");
  EXPECT_EQ(out(sh, "functions -m '[g]'"), "g () {\n\t:\n}\n");
  EXPECT_EQ(out(sh, "typeset -f g"), "g () {\n\t:\n}\n");
  EXPECT_EQ(sh.run("functions nosuch").rc, 1);
}

TEST(Autoload, LoadsFromFpath) {
  Shell sh;
  TempDir t;
  str d = real(t.path);
  t.file("af", "echo body-of-af \"$@\"\n", RW);
  t.file("kf", "kf() { echo ksh-style $1; }\n", RW);
  EXPECT_EQ(out(sh, "fpath=(" + d + "); autoload af kf; functions af"),
            "af () {\n\t# undefined\n\tbuiltin autoload -XU\n}\n");
  EXPECT_EQ(out(sh, "af 1 2; kf 3; kf 4"), "body-of-af 1 2\nksh-style 3\nksh-style 4\n");
  EXPECT_EQ(out(sh, "whence -v af"), "af is a shell function from " + d + "/af\n");
  EXPECT_EQ(out(sh, "functions af"), "af () {\n\techo body-of-af \"$@\"\n}\n");
  auto r = sh.run("autoload -U zz; zz; echo rc=$?");
  EXPECT_EQ(r.err, "jfsh: zz: function definition file not found\n");
  EXPECT_EQ(r.out, "rc=1\n");
  // autoload /path/name loads from that file.
  t.file("other", "echo other-file\n", RW);
  EXPECT_EQ(out(sh, "autoload " + d + "/other; other"), "other-file\n");
}

TEST(Trap, ListsTrapFunctions) {
  Shell sh;
  EXPECT_EQ(out(sh, "TRAPUSR1() { echo x; }; trap 'echo y' USR2; trap"),
            "TRAPUSR1 () {\n\techo x\n}\ntrap -- 'echo y' USR2\n");
  EXPECT_EQ(out(sh, "unset -f TRAPUSR1; trap"), "trap -- 'echo y' USR2\n");
}

// ---- fc, history, r ------------------------------------------------------------------

TEST(History, ListsAndReruns) {
  Shell sh;
  HistoryManager hist{HistOptions{}};
  hist.add("echo a");
  hist.add("echo b");
  hist.add("ls x");
  sh.ctx.history = &hist;
  EXPECT_EQ(out(sh, "fc -l"), "    1  echo a\n    2  echo b\n    3  ls x\n");
  EXPECT_EQ(out(sh, "history -2"), "    2  echo b\n    3  ls x\n");
  EXPECT_EQ(out(sh, "fc -ln 1 2"), "echo a\necho b\n");
  EXPECT_EQ(out(sh, "fc -lr 1 2"), "    2  echo b\n    1  echo a\n");
  EXPECT_EQ(out(sh, "fc -l echo"), "    2  echo b\n    3  ls x\n");
  EXPECT_EQ(out(sh, "fc -s a=Z 1"), "echo Z\nZ\n");
  EXPECT_EQ(out(sh, "r b=B echo"), "echo Z\nZ\n");   // the latest `echo …` is now `echo Z`
  EXPECT_EQ(out(sh, "fc -e - 2"), "echo b\nb\n");
  EXPECT_EQ(sh.run("fc -l 99").err, "jfsh: fc: event not found: 99\n");
  sh.ctx.history = nullptr;
  EXPECT_EQ(sh.run("history").err, "jfsh: history: no history\n");
}

// ---- builtins that need later milestones ---------------------------------------

// zpty was a "not implemented" stub until zsh/zpty (M11) took it over: as
// in zsh, it is there once the module is loaded.
TEST(Unimplemented, ZptyComesFromItsModule) {
  Shell sh;
  EXPECT_EQ(sh.run("whence -w zpty").out, "zpty: none\n");
  EXPECT_EQ(sh.run("zmodload zsh/zpty; whence -w zpty").out, "zpty: builtin\n");
}

// ---- parser fixes found on the way ----------------------------------------------

TEST(Parse, AsyncThenSemicolonAndKeywordTargets) {
  Shell sh;
  TempDir t;
  str d = real(t.path);
  EXPECT_EQ(out(sh, "true & ; echo x; wait"), "x\n");
  EXPECT_EQ(out(sh, "echo hi >" + d + "/in; cat <" + d + "/in"), "hi\n");
  CwdGuard g;
  EXPECT_EQ(out(sh, "cd " + d + "; echo kw > in; cat < in"), "kw\n");
}

}  // namespace
}  // namespace jfsh
