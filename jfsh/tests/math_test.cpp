// Arithmetic (M5): math_eval on its own, with each option, and through the
// shell as $((…)), (( )), let and typeset.  The expected values are what
// zsh 5.9 prints (`zsh -f`).

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <jfsh/builtins/math.h>
#include <jfsh/exec/executor.h>

#include "shell_fixture.h"

namespace jfsh {
namespace {

using test::Shell;

// $((expr)) as zsh prints it, or "error: <message>".
std::string eval(std::string_view expr, MathOptions opts = {}) {
  ParamTable params;
  auto v = math_eval(expr, params, opts);
  if (not v) return "error: " + v.error().message;
  return format_math(*v);
}

std::string eval(ParamTable& params, std::string_view expr) {
  auto v = math_eval(expr, params);
  if (not v) return "error: " + v.error().message;
  return format_math(*v);
}

TEST(Math, IntegerLiteralsAndBases) {
  EXPECT_EQ(eval("010"), "10");
  EXPECT_EQ(eval("1_000_000"), "1000000");
  EXPECT_EQ(eval("0x_ff"), "255");
  EXPECT_EQ(eval("16#ff"), "255");
  EXPECT_EQ(eval("36#z"), "35");
  EXPECT_EQ(eval("0b101 + 2#101"), "10");
  EXPECT_EQ(eval("017 + 0b11 + 0X1f"), "51");
  EXPECT_EQ(eval("0x"), "0");
  EXPECT_EQ(eval("2#"), "0");
}

TEST(Math, FloatLiteralsAndFormatting) {
  EXPECT_EQ(eval("1.5"), "1.5");
  EXPECT_EQ(eval("10.0"), "10.");
  EXPECT_EQ(eval("1."), "1.");
  EXPECT_EQ(eval(".5"), "0.5");
  EXPECT_EQ(eval("1e3"), "1000.");
  EXPECT_EQ(eval("1e+2"), "100.");
  EXPECT_EQ(eval("1.5e-2"), "0.014999999999999999");
  EXPECT_EQ(eval("1._5"), "1.5");
  EXPECT_EQ(eval("1e16"), "10000000000000000.");
  EXPECT_EQ(eval("1e17"), "1e+17");
  EXPECT_EQ(eval("-0.0"), "-0.");
  EXPECT_EQ(eval("nan"), "NaN");
  EXPECT_EQ(eval("Inf"), "Inf");
  EXPECT_EQ(eval("-inf"), "-Inf");
}

TEST(Math, MixedIntegerAndFloat) {
  EXPECT_EQ(eval("3/2"), "1");
  EXPECT_EQ(eval("3/2."), "1.5");
  EXPECT_EQ(eval("1/3."), "0.33333333333333331");
  EXPECT_EQ(eval("0.1+0.2"), "0.30000000000000004");
  EXPECT_EQ(eval("2**0.5"), "1.4142135623730951");
  EXPECT_EQ(eval("3 ** -1"), "0.33333333333333331");
  EXPECT_EQ(eval("7.5%2"), "1.5");
  EXPECT_EQ(eval("10 % 3.5"), "3.");
  EXPECT_EQ(eval("1.0/0"), "Inf");
  EXPECT_EQ(eval("-1.0/0"), "-Inf");
  // Comparisons and bitwise operators give integers.
  EXPECT_EQ(eval("1 < 2.5"), "1");
  EXPECT_EQ(eval("2.0 == 2"), "1");
  EXPECT_EQ(eval("~1.5"), "-2");
  EXPECT_EQ(eval("1.5 << 1"), "2");
  EXPECT_EQ(eval("1 ? 2.5 : 3"), "2.5");
}

TEST(Math, IntegerEdgeCases) {
  EXPECT_EQ(eval("-7/2"), "-3");
  EXPECT_EQ(eval("-7%3"), "-1");
  EXPECT_EQ(eval("9223372036854775807 + 1"), "-9223372036854775808");
  EXPECT_EQ(eval("(-9223372036854775807 - 1) / -1"), "-9223372036854775808");
  EXPECT_EQ(eval("1 << 64"), "1");
  EXPECT_EQ(eval("2**3**2"), "512");
  EXPECT_EQ(eval("-2**2"), "4");
}

TEST(Math, ZshPrecedence) {
  // Shifts and bitwise operators bind tighter than ** and * in zsh.
  EXPECT_EQ(eval("1 + 2 << 1"), "5");
  EXPECT_EQ(eval("2 * 3 & 1"), "2");
  EXPECT_EQ(eval("2 ** 1 | 2"), "8");
  EXPECT_EQ(eval("2 | 1 == 1"), "0");
  EXPECT_EQ(eval("1 + 2 == 3"), "1");
  EXPECT_EQ(eval("1 || 0 ^^ 1"), "0");
  EXPECT_EQ(eval("3 > 2 > 1"), "0");
  EXPECT_EQ(eval("0 ? 1 : 0 ? 2 : 3"), "3");
}

TEST(Math, OptionsChangeTheRules) {
  MathOptions c;
  c.c_precedences = true;
  EXPECT_EQ(eval("1 + 2 << 1", c), "6");
  EXPECT_EQ(eval("2 * 3 & 1", c), "0");
  EXPECT_EQ(eval("2 ** 1 | 2", c), "2");
  EXPECT_EQ(eval("1 || 0 ^^ 1", c), "1");
  EXPECT_EQ(eval("-2**2", c), "4");

  MathOptions octal;
  octal.octal_zeroes = true;
  EXPECT_EQ(eval("010", octal), "8");
  EXPECT_EQ(eval("0x10", octal), "16");
  EXPECT_EQ(eval("08", octal), "error: bad math expression: operator expected at `8'");

  MathOptions ff;
  ff.force_float = true;
  EXPECT_EQ(eval("3/2", ff), "1.5");
  EXPECT_EQ(eval("1+1", ff), "2.");
}

TEST(Math, Errors) {
  EXPECT_EQ(eval("1/0"), "error: division by zero");
  EXPECT_EQ(eval("1 % 0"), "error: division by zero");
  EXPECT_EQ(eval("1 +"), "error: bad math expression: operand expected at end of string");
  EXPECT_EQ(eval("1 2"), "error: bad math expression: operator expected at `2'");
  EXPECT_EQ(eval("a b"), "error: bad math expression: operator expected at `b'");
  EXPECT_EQ(eval("1 ? 2"), "error: bad math expression: ':' expected");
  EXPECT_EQ(eval("5 = 3"), "error: bad math expression: lvalue required");
  EXPECT_EQ(eval("++5"), "error: bad math expression: lvalue required");
  EXPECT_EQ(eval("1++"), "error: bad math expression: lvalue required");
  EXPECT_EQ(eval("2#12"), "error: bad math expression: operator expected at `2'");
  EXPECT_EQ(eval("37#1"), "error: invalid base (must be 2 to 36 inclusive): 37");
  EXPECT_EQ(eval("1e"), "error: bad math expression: operator expected at `e'");
  EXPECT_EQ(eval("0xff.5"), "error: bad math expression: operator expected at `.5'");
  // An un-taken branch is parsed but not evaluated.
  EXPECT_EQ(eval("0 && 1/0"), "0");
  EXPECT_EQ(eval("1 ? 2 : 1/0"), "2");
}

TEST(Math, Parameters) {
  ParamTable params;
  EXPECT_EQ(eval(params, "a=b=3, a+b"), "6");
  EXPECT_EQ(eval(params, "i=5, i++ + ++i"), "12");
  EXPECT_EQ(eval(params, "unset_name + 1"), "1");
  (void)params.set_scalar("x", "y");
  (void)params.set_scalar("y", "z");
  (void)params.set_scalar("z", "4");
  EXPECT_EQ(eval(params, "x + 1"), "5");
  (void)params.set_scalar("r", "r");
  EXPECT_EQ(eval(params, "r"), "error: math recursion limit exceeded: r");
  (void)params.set_scalar("f", "1.5");
  EXPECT_EQ(eval(params, "f * 2"), "3.");
  // `&&=` and `||=` only evaluate the right side when it matters.
  EXPECT_EQ(eval(params, "b = 1, b &&= 0, b ||= 5, b"), "1");
  EXPECT_EQ(eval(params, "0 && c++, c"), "0");
}

TEST(Math, AssignmentKeepsOrPicksTheType) {
  ParamTable params;
  // A new parameter takes the value's type.
  ASSERT_TRUE(math_eval("n = 5, g = 2.5 * 2", params));
  EXPECT_TRUE(params.lookup("n")->get().is_integer());
  EXPECT_TRUE(params.lookup("g")->get().is_float());
  EXPECT_EQ(params.lookup("g")->get().scalar(), "5.0000000000");
  // A scalar stays a scalar.
  (void)params.set_scalar("z", "3");
  ASSERT_TRUE(math_eval("z /= 2.", params));
  EXPECT_EQ(params.lookup("z")->get().scalar(), "1.5");
  // An integer parameter truncates.
  ASSERT_TRUE(math_eval("n = 7.9", params));
  EXPECT_EQ(params.lookup("n")->get().integer(), 7);
}

TEST(Math, ArraySubscripts) {
  Shell sh;
  EXPECT_EQ(sh.run("a=(10 20 30); i=2; echo $((a[2] + a[-1])) $((a[i]*2))").out, "50 40\n");
  EXPECT_EQ(sh.run("(( a[2]++ )); echo $a; (( a[5] = 7 )); echo $a").out,
            "10 21 30\n10 21 30 7\n");
  EXPECT_EQ(sh.run("typeset -A h; (( h[k] = 5, h[k] += 1 )); echo $h[k] $((h[k] * 2))").out,
            "6 12\n");
  EXPECT_EQ(sh.run("m=(1 2); echo $((m))").err,
            "jfsh: bad math expression: operator expected at `2'\n");
}

TEST(Math, ThroughTheShell) {
  Shell sh;
  EXPECT_EQ(sh.run("x=1.5; echo $((x*2)) $((x+1))").out, "3. 2.5\n");
  EXPECT_EQ(sh.run("(( y = 2.5 * 2 )); echo $y").out, "5.0000000000\n");
  EXPECT_EQ(sh.run("(( n = 5 )); n+=1; echo $n; n=2+3; echo $n").out, "6\n5\n");
  EXPECT_EQ(sh.run("(( 0.5 )); echo $?; (( 0.0 )); echo $?; (( 1/0 )); echo $?").out,
            "0\n1\n2\n");
  // A name first set by arithmetic to a float is a float parameter.
  EXPECT_EQ(sh.run("let 'w = 2.5' 'w * 2'; echo $? $w").out, "0 2.5000000000\n");
  EXPECT_EQ(sh.run("for (( i = 0.5; i < 2; i += 0.5 )); do echo $i; done").out,
            "0.5000000000\n1.0000000000\n1.5000000000\n");
  auto r = sh.run("readonly r=1; (( r = 2 )); echo $r");
  EXPECT_EQ(r.out, "1\n");
  EXPECT_EQ(r.err, "jfsh: read-only variable: r\n");
}

TEST(Math, TypesetNumbers) {
  Shell sh;
  EXPECT_EQ(sh.run("typeset -F f=2.5; echo $f $((f*2))").out, "2.5000000000 5.\n");
  EXPECT_EQ(sh.run("typeset -E e=12345.678; echo $e").out, "1.234567800e+04\n");
  EXPECT_EQ(sh.run("integer i=7; i+=1; echo $i; i=2+3; echo $i").out, "8\n5\n");
  EXPECT_EQ(sh.run("typeset -F g=1.5; g+=1; echo $g").out, "2.5000000000\n");
  EXPECT_EQ(sh.run("typeset -i k=1.9; echo $k").out, "1\n");
  EXPECT_EQ(sh.run("typeset -F h=1+1; echo $h").out, "2.0000000000\n");
}

TEST(Math, ShellOptions) {
  Shell sh;
  sh.opts.set(OPT_OCTALZEROES);
  EXPECT_EQ(sh.run("echo $((010))").out, "8\n");
  sh.opts.unset(OPT_OCTALZEROES);
  sh.opts.set(OPT_FORCEFLOAT);
  EXPECT_EQ(sh.run("echo $((3/2))").out, "1.5\n");
  sh.opts.unset(OPT_FORCEFLOAT);
  sh.opts.set(OPT_CPRECEDENCES);
  EXPECT_EQ(sh.run("echo $((1 + 2 << 1))").out, "6\n");
}

}  // namespace
}  // namespace jfsh
