// Arithmetic (M5): math_eval on its own, with each option, and through the
// shell as $((…)), (( )), let and typeset.  The expected values are what
// zsh 5.9 prints (`zsh -f`).

#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <jfsh/builtins/math.h>
#include <jfsh/core/aliases.h>
#include <jfsh/exec/executor.h>
#include <shell_fixture.h>

namespace jfsh {
namespace {

using test::Shell;

// $((expr)) as zsh prints it, or "error: <message>".
str eval(strv expr, MathOptions opts = {}) {
  ParamTable params;
  auto v = math_eval(expr, params, opts);
  if (not v) return "error: " + v.error().message;
  return format_math(*v);
}

str out(Shell& sh, strv src) { return sh.run(src).out; }

str eval(ParamTable& params, strv expr) {
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

TEST(Math, CharacterCodes) {
  EXPECT_EQ(eval("##a"), "97");
  EXPECT_EQ(eval("##^A"), "1");
  EXPECT_EQ(eval("##\\M-a"), "225");
  EXPECT_EQ(eval("##"), "error: bad math expression: character missing after ##");
  ParamTable params;
  params.assign("x", "hello", PM_SCALAR);
  EXPECT_EQ(eval(params, "#x + 1"), "105");
  EXPECT_EQ(eval(params, "#nosuch"), "0");
}

TEST(Math, LiteralsPastTheIntegerRangeAreTruncated) {
  Shell sh;
  auto r = sh.run("echo $((9223372036854775808)) $((0xffffffffffffffff))");
  EXPECT_EQ(r.out, "922337203685477580 1152921504606846975\n");
  EXPECT_EQ(r.err, "jfsh: number truncated after 18 digits: 9223372036854775808\n"
                   "jfsh: number truncated after 15 digits: ffffffffffffffff\n");
}

TEST(Math, DeepNestingFailsInsteadOfCrashing) {
  str ok = str(1000, '(') + "1" + str(1000, ')');
  EXPECT_EQ(eval(ok), "1");
  // The parser keeps its pending work on the heap, so on a 1 MB stack
  // parentheses and unary operators nest as deep as in zsh; chains that
  // keep values pending (`**`, `=`, `?:`) stop at zsh's 100-value stack.
  test::on_stack(1 << 20, [] {
    auto chain = [](strv op, int n, strv tail = "") {
      str e;
      for (int k = 0; k < n; ++k) e += op;
      return e + "1" + str(tail);
    };
    str colons;
    for (int k = 0; k < 20000; ++k) colons += ":1";
    ParamTable params;
    auto run = [&](const str& e) {
      auto v = math_eval(e, params);
      return v ? format_math(*v) : "error: " + v.error().message;
    };
    const str overflow = "error: stack overflow";
    EXPECT_EQ(run(str(20000, '(') + "1" + str(20000, ')')), "1");
    EXPECT_EQ(run(chain("-", 20000)).substr(0, 6), "error:");
    EXPECT_EQ(run(chain("- ", 20000)), "1");
    EXPECT_EQ(run(chain("!", 20001)), "0");
    EXPECT_EQ(run(chain("2**", 98)), "4");
    EXPECT_EQ(run(chain("2**", 99)), "16");
    EXPECT_EQ(run(chain("2**", 100)), overflow);
    EXPECT_EQ(run(chain("2**", 20000)), overflow);
    EXPECT_EQ(run(chain("a=", 20000)), overflow);
    EXPECT_EQ(run(chain("a+=", 20000)), overflow);
    EXPECT_EQ(run(chain("1?", 20000, colons)), overflow);
    EXPECT_EQ(run(chain("1+", 50000)), "50001");
    EXPECT_EQ(run(chain("2**", 3)), "16");
  });
  EXPECT_EQ(eval("'1' + 2"), "error: bad math expression: illegal character: '");
}

TEST(Math, NoUnsetNames) {
  ParamTable params;
  MathOptions o;
  o.no_unset = true;
  auto v = math_eval("zz + 1", params, o);
  ASSERT_FALSE(v);
  EXPECT_EQ(v.error().message, "zz: parameter not set");
  EXPECT_TRUE(math_eval("zz = 1", params, o));
}

// ---- M5 review: zsh 5.9's math.c, ported --------------------------------------

// "rc|out|err" of src, for the cases where all three matter.
str all(Shell& sh, strv src) {
  auto r = sh.run(src);
  return s::to_string(r.rc) + "|" + r.out + "|" + r.err;
}

TEST(MathM5Review, ConstantsAndBases) {
  // 0#… picks the base from the digits, as zstrtol's base 0 does; the base
  // itself is a decimal number however it is written.
  EXPECT_EQ(eval("0#0x10 + 0#010 + 0#12"), "36");
  EXPECT_EQ(eval("010#1"), "1");
  EXPECT_EQ(eval("100#1"), "error: invalid base (must be 2 to 36 inclusive): 100");
  EXPECT_EQ(eval("1#1"), "error: invalid base (must be 2 to 36 inclusive): 1");
  // zstrtol skips blanks and a sign after the `#`.
  EXPECT_EQ(eval("16# ff"), "255");
  EXPECT_EQ(eval("16#-1"), "-1");
  EXPECT_EQ(eval("1."), "1.");
  EXPECT_EQ(eval("."), "error: bad floating point constant");
  EXPECT_EQ(eval("1.5.3"), "error: bad floating point constant");
  EXPECT_EQ(eval("1..2"), "error: bad floating point constant");
  MathOptions octal;
  octal.octal_zeroes = true;
  EXPECT_EQ(eval("017 + 0.17 * 100 + 017e1 + 08.5 + 0_17", octal), "225.5");
  // zsh's overflow check: 20 nines wrap where 19 fit in unsigned.
  Shell sh;
  EXPECT_EQ(all(sh, "echo $((99999999999999999999 + 1))"),
            "0|-8446744073709551616\n|jfsh: number truncated after 19 digits: "
            "99999999999999999999 + 1\n");
}

TEST(MathM5Review, ErrorsNameWhatIsThere) {
  EXPECT_EQ(eval("@"), "error: bad math expression: illegal character: @");
  EXPECT_EQ(eval("1 @"), "error: bad math expression: illegal character: @");
  EXPECT_EQ(eval("1;2"), "error: bad math expression: illegal character: ;");
  EXPECT_EQ(eval("1 + @"), "error: bad math expression: operand expected at `@'");
  EXPECT_EQ(eval("(@)"), "error: bad math expression: ')' expected");
  EXPECT_EQ(eval("()"), "error: bad math expression: operand expected at `)'");
  EXPECT_EQ(eval("1)"), "error: bad math expression: unexpected ')'");
  EXPECT_EQ(eval("1 : 2"), "error: bad math expression: ':' without '?'");
  EXPECT_EQ(eval("1 2 + 3 + 4 + 5 + 6"),
            "error: bad math expression: operator expected at `2 + 3 + 4 ...'");
  // The operand after a postfix `--` is checked before the `--` runs.
  EXPECT_EQ(eval("3 -- 2"), "error: bad math expression: operator expected at `2'");
  EXPECT_EQ(eval("3--"), "error: bad math expression: lvalue required");
  EXPECT_EQ(eval("(-8) ** (1/3.)"), "error: bad math expression: imaginary power");
  // POSIX says to ignore double quotes.
  EXPECT_EQ(eval("\"1\" + 2"), "3");
}

TEST(MathM5Review, TernaryAndLvalues) {
  ParamTable params;
  // The middle operand is a conditional expression, not an assignment.
  EXPECT_EQ(eval(params, "1 ? x = 5 : 6"), "error: bad math expression: ':' expected");
  EXPECT_EQ(eval(params, "1 ? (x = 5) : 6"), "5");
  EXPECT_EQ(eval(params, "0 ? 1 : y = 6"), "error: bad math expression: lvalue required");
  // Parentheses and assignments keep the lvalue.
  EXPECT_EQ(eval(params, "x = 5, (x) = 3, x"), "3");
  EXPECT_EQ(eval(params, "(x)++, x"), "4");
  EXPECT_EQ(eval(params, "(x = 3) = 4, x"), "4");
  EXPECT_EQ(eval(params, "x = 1, (x += 1)++, x"), "3");
  EXPECT_EQ(eval(params, "-x = 3"), "error: bad math expression: lvalue required");
  EXPECT_EQ(eval(params, "(x, y) = 3"), "error: bad math expression: lvalue required");
  // `^^=` gives a ^^ b but, as in zsh, assigns nothing.
  EXPECT_EQ(eval(params, "z = 0, z ^^= 1"), "1");
  EXPECT_EQ(eval(params, "z"), "0");
  // && || ^^ and ! take floats as integers, so 0.5 is false.
  EXPECT_EQ(eval("0.5 && 1"), "0");
  EXPECT_EQ(eval("2.5 ^^ 0"), "1");
  EXPECT_EQ(eval("!0.5"), "0");
}

TEST(MathM5Review, UntakenBranchesAreNotEvaluated) {
  ParamTable params;
  MathOptions o;
  o.no_unset = true;
  EXPECT_EQ(eval("0 && zz", o), "0");
  EXPECT_EQ(eval("0 && nosuch(1)"), "0");
  EXPECT_EQ(eval("1 || x[1/0]"), "1");
  (void)params.set_scalar("a", "b");
  (void)params.set_scalar("b", "a");
  EXPECT_EQ(eval(params, "0 && a"), "0");
  EXPECT_EQ(eval(params, "a"), "error: math recursion limit exceeded: a");
}

TEST(MathM5Review, ShellValuesAndFunctions) {
  Shell sh;
  EXPECT_EQ(out(sh, "set -- a b c; echo $(( ARGC )) $(( # )) $(( #+1 ))"), "3 3 4\n");
  EXPECT_EQ(out(sh, "set -- 5 6; echo $(( argv[2] + 1 ))"), "7\n");
  EXPECT_EQ(out(sh, "f() { return 3; }; f; echo $(( ? + 1 ))"), "4\n");
  EXPECT_EQ(out(sh, "echo $(( $ > 0 ))"), "1\n");
  EXPECT_EQ(all(sh, "echo $(( nosuch(1) ))"), "1||jfsh: unknown function: nosuch\n");
  EXPECT_EQ(all(sh, "echo $(( sqrt(1,2) ))"),
            "1||jfsh: wrong number of arguments: sqrt(1,2)\n");
  EXPECT_EQ(all(sh, "echo $(( sqrt(4 2) ))"),
            "1||jfsh: bad math expression: operator expected at `2'\n");
  EXPECT_EQ(out(sh, "echo $(( atan(1,1) ))"), "0.78539816339744828\n");
}

TEST(MathM5Review, Subscripts) {
  Shell sh;
  // A scalar's subscript picks characters; a comma makes a range.
  EXPECT_EQ(out(sh, "s=4567; echo $(( s[2] )) $(( s[-1] )) $(( s[2,3] )) $(( s[0,1] ))"),
            "5 7 56 4\n");
  EXPECT_EQ(out(sh, "s=45; (( s[1] = 9 )); echo $s"), "95\n");
  // The subscript is worked out once for a read and the write after it.
  EXPECT_EQ(out(sh, "i=1; a=(1 2 3); (( a[i++]++ )); echo $i $a"), "2 2 2 3\n");
  EXPECT_EQ(out(sh, "i=1; a=(1 2 3); (( a[i++] += 7 )); echo $i $a"), "2 8 2 3\n");
  // An unset name's subscript is not evaluated.
  EXPECT_EQ(out(sh, "unset a; echo $(( a[1/0] ))"), "0\n");
  EXPECT_EQ(out(sh, "a=(1 2 3); (( a[2,3] = 9 )); echo $a"), "1 9\n");
  EXPECT_EQ(out(sh, "a=(1 2 3); (( a[-4] = 5 )); echo $a"), "5 1 2 3\n");
  EXPECT_EQ(all(sh, "a=(1 2 3); echo $(( a[2,3] ))"),
            "1||jfsh: bad math expression: operator expected at `3'\n");
  EXPECT_EQ(out(sh, "a=(x y); (( a[(r)y] = 5 )); echo $a $(( a[(i)5] ))"), "x 5 2\n");
  // Assignments outside arithmetic use the same rules.
  EXPECT_EQ(out(sh, "a=(1 2 3); a[-4,1]=5; echo $a; a[-9,2]=x; echo $a"), "5 2 3\nx 3\n");
  EXPECT_EQ(out(sh, "s=abc; s[-5]=x; echo $s"), "xabc\n");
  if (test::Utf8Ctype loc; loc.ok)
    EXPECT_EQ(out(sh, "s=héllo; s[2]=e; echo $s"), "hello\n");
  EXPECT_EQ(out(sh, "setopt kshzerosubscript; a=(1 2); a[0]=9; echo $a"), "9 2\n");
  EXPECT_EQ(out(sh, "setopt ksharrays; a=(1 2 3); a[1,2]=9; a[0]=7; echo ${a[@]}"), "7 9\n");
}

TEST(MathM5Review, IntegersTakeTheConstantsBase) {
  Shell sh;
  EXPECT_EQ(out(sh, "typeset -i x=16#ff; echo $x"), "16#FF\n");
  EXPECT_EQ(out(sh, "integer k; k=0x10+1; echo $k; k=2#1; echo $k"), "16#11\n16#1\n");
  EXPECT_EQ(out(sh, "(( n = 16#10 )); typeset -p n"), "typeset -i16 n=16\n");
  EXPECT_EQ(out(sh, "integer j; (( j = 5, q = 0x10 )); echo $j"), "5\n");
  // A base from a parameter's value doesn't count.
  EXPECT_EQ(out(sh, "a=16#ff; integer z; z=a; echo $z"), "255\n");
  sh.opts.set(OPT_OCTALZEROES);
  EXPECT_EQ(out(sh, "typeset -i o=010; echo $o"), "8#10\n");
}

TEST(MathM5Review, LetAndStatus) {
  Shell sh;
  // An error ends let with status 1 (zsh's errors in let are not fatal).
  EXPECT_EQ(all(sh, "let 'x=1' '1/0' 'y=2'; echo $? $x ${y-unset}"),
            "0|1 1 unset\n|jfsh: division by zero\n");
  EXPECT_EQ(all(sh, "let; echo $?"), "0|1\n|jfsh: let: not enough arguments\n");
  EXPECT_EQ(out(sh, "let '\"1\"+2'; echo $?"), "0\n");
  EXPECT_EQ(out(sh, "(( 0 )); echo $?; (( 2 )); echo $?; (( 1/0 )); echo $?"), "1\n0\n2\n");
}

TEST(MathM5Review, DeepAndLongExpressionsDontCrash) {
  test::on_stack(1 << 20, [] {
    ParamTable params;
    auto run = [&](const str& e) {
      auto v = math_eval(e, params);
      return v ? format_math(*v) : "error: " + v.error().message;
    };
    str plus = "1";
    for (int k = 0; k < 40000; ++k) plus += "+1";
    EXPECT_EQ(run(plus), "40001");
    EXPECT_EQ(run(str(200000, '(') + "1" + str(200000, ')')), "1");
    EXPECT_EQ(run(str(200000, '!') + "1"), "1");
    // 255 names deep is zsh's limit for values holding expressions.
    for (int k = 1; k < 256; ++k)
      (void)params.set_scalar("a" + s::to_string(k), "a" + s::to_string(k + 1));
    (void)params.set_scalar("a256", "1");
    EXPECT_EQ(run("a2"), "1");
    EXPECT_EQ(run("a1"), "error: math recursion limit exceeded: 1");
  });
}

// ---- M5 review, round 2 ------------------------------------------------------

TEST(MathM5Review2, LargeSubscriptsWrapAsZshsInt) {
  Shell sh;
  // zsh keeps an index in an int: 2^63-1 is -1, 10^12 a negative one.
  EXPECT_EQ(all(sh, "a=(1 2 3); a[9223372036854775807]=x; echo $? $a"), "0|0 1 2 x\n|");
  EXPECT_EQ(all(sh, "a=(1 2 3); a[1000000000000]=x; echo $? $a"), "0|0 x 1 2 3\n|");
  // Growing past 2^28 elements is refused (zsh tries to allocate them).
  EXPECT_EQ(all(sh, "a=(1 2 3); (( a[100000000000] = 1 )); echo $? ${#a}"),
            "0|2 3\n|jfsh: a: assignment to invalid subscript range\n");
  EXPECT_EQ(all(sh, "setopt ksharrays; a=(1 2); a[9223372036854775807]=x; echo $?"),
            "1||jfsh: a: assignment to invalid subscript range\n");
}

TEST(MathM5Review2, RangesAndAppends) {
  Shell sh;
  EXPECT_EQ(out(sh, "a=(1 2 3); a[0,1]=x; echo $a; a=(1 2 3); a[0,2]=x; echo $a"),
            "x 2 3\nx 3\n");
  EXPECT_EQ(out(sh, "a=(1 2 3); (( a[0,1] = 9 )); echo $a"), "9 2 3\n");
  EXPECT_EQ(all(sh, "a=(1 2 3); a[0]=x"), "1||jfsh: a: assignment to invalid subscript range\n");
  // `+=` adds a word to the last element and an array after it.
  EXPECT_EQ(out(sh, "a=(1 2 3); a[2,3]+=x; echo $a"), "1 2 3x\n");
  EXPECT_EQ(out(sh, "a=(1 2 3); a[1,2]+=x; echo $a"), "1 2x 3\n");
  EXPECT_EQ(out(sh, "a=(1 2 3); a[2,3]+=(x y); echo $a"), "1 2 3 x y\n");
  EXPECT_EQ(out(sh, "a=(1 2 3); a[2]+=(x y); echo $a"), "1 2 x y 3\n");
  EXPECT_EQ(out(sh, "a=(1 2 3); a[3,2]+=x; echo $a"), "1 2x 3\n");
  EXPECT_EQ(out(sh, "a=(1 2 3); a[5,6]+=x; echo ${(q)a}"), "1 2 3 '' '' x\n");
  EXPECT_EQ(out(sh, "s=hello; s[2,3]+=X; echo $s; s=hello; s[2]+=X; echo $s"),
            "helXlo\nheXllo\n");
}

TEST(MathM5Review2, NumbersHaveNoElements) {
  Shell sh;
  EXPECT_EQ(out(sh, "integer i=123; i[2]=9; typeset -p i"), "typeset -i i=9\n");
  EXPECT_EQ(out(sh, "integer i=123; (( i[2] = 9 )); typeset -p i"), "typeset -i i=9\n");
  EXPECT_EQ(out(sh, "float f=1.5; f[1]=7; typeset -p f"), "typeset -E f=7.000000000e+00\n");
  EXPECT_EQ(all(sh, "integer i=5; i[1]+=3"),
            "1||jfsh: attempt to add to slice of a numeric variable\n");
}

// ---- M5 review, round 3 ------------------------------------------------------

TEST(MathM5Review3, NumbersHaveNoElementsInArithmetic) {
  Shell sh;
  EXPECT_EQ(out(sh, "integer i=123; print $(( i[2] )) ${i[2]}"), "123 2\n");
  EXPECT_EQ(out(sh, "integer i=123; (( i[2] += 9 )); typeset -p i"), "typeset -i i=132\n");
  EXPECT_EQ(out(sh, "integer i=123; (( i[2]++ )); typeset -p i"), "typeset -i i=124\n");
  EXPECT_EQ(out(sh, "float f=1.5; print $(( f[1] ))"), "1.5\n");
  // The subscript is still evaluated, once.
  EXPECT_EQ(out(sh, "integer i=5; j=0; (( i[j++] += 1 )); echo $i $j"), "6 1\n");
  EXPECT_EQ(all(sh, "integer i=123; i[1]=(4 5)"),
            "1||jfsh: i: attempt to assign array value to non-array\n");
  EXPECT_EQ(all(sh, "s=hello; s[2]=(a b); echo $s"),
            "1||jfsh: s: attempt to assign array value to non-array\n");
  EXPECT_EQ(all(sh, "s=hello; s[2]+=(a b)"),
            "1||jfsh: s: attempt to assign array value to non-array\n");
}

TEST(MathM5Review3, ScalarCharactersFollowMultibyte) {
  test::Utf8Ctype loc;
  if (not loc.ok) GTEST_SKIP() << "no UTF-8 locale";
  Shell sh;
  EXPECT_EQ(out(sh, "s=héllo; s[3]=X; print -r -- $s; print $(( #s ))"), "héXlo\n104\n");
  EXPECT_EQ(out(sh, "s=hé12; print $(( s[3] )) $(( s[-1] ))"), "1 2\n");
  sh.opts.unset(OPT_MULTIBYTE);
  EXPECT_EQ(out(sh, "s=héllo; s[3]=X; print -r -- $s"), "h\xc3Xllo\n");
  EXPECT_EQ(out(sh, "s=héllo; (( s[3] = 7 )); print -r -- $s"), "h\xc3" "7llo\n");
  EXPECT_EQ(out(sh, "s=hé12; print $(( s[4] ))"), "1\n");
}

TEST(MathM5Review3, ArithmeticReadsWrapAsZshsInt) {
  Shell sh;
  EXPECT_EQ(out(sh, "a=(1 2 3); print $(( a[4294967297] )) $(( a[4294967298,4294967298] ))"),
            "1 2\n");
}

// ---- M5 review, round 4 ------------------------------------------------------

TEST(MathM5Review4, WholeSubscriptOnScalarsAndNumbers) {
  Shell sh;
  EXPECT_EQ(out(sh, "integer i=123; (( i[*] = 5 )); typeset -p i"), "typeset -i i=5\n");
  EXPECT_EQ(out(sh, "float f=1; (( f[@] += 2 )); typeset -p f"),
            "typeset -E f=3.000000000e+00\n");
  EXPECT_EQ(out(sh, "integer i=123; (( i[@]++ )); typeset -p i"), "typeset -i i=124\n");
  EXPECT_EQ(out(sh, "s=abc; (( s[@] = 5 )); typeset -p s"), "typeset s=5\n");
  EXPECT_EQ(out(sh, "integer i=123; print $(( i[@] )) $(( i[*] + 1 ))"), "123 124\n");
  // The same outside arithmetic; arrays and unset names take the words.
  EXPECT_EQ(out(sh, "s=abc; s[@]=5; typeset -p s; s[*]+=6; typeset -p s"),
            "typeset s=5\ntypeset s=56\n");
  EXPECT_EQ(out(sh, "integer i=123; i[@]+=7; typeset -p i"), "typeset -i i=130\n");
  EXPECT_EQ(out(sh, "a=(1 2 3); a[@]=x; a[*]+=y; typeset -p a; u[@]=(p q); typeset -p u"),
            "typeset -a a=( x y )\ntypeset -a u=( p q )\n");
  EXPECT_EQ(out(sh, "a=(1 2); (( a[@] = 5 )); typeset -p a"), "typeset -a a=( 5 )\n");
}

TEST(MathM5Review4, ParenthesesInAnIndexAreArithmetic) {
  Shell sh;
  EXPECT_EQ(out(sh, "a=(5 6 7); print $(( a[(1+1)] ))"), "6\n");
  EXPECT_EQ(out(sh, "a=(5 6 7); (( a[(1+1)] = 9 )); print $a"), "5 9 7\n");
  EXPECT_EQ(out(sh, "a=(5 6 7); i=1; print $(( a[(i+1)*1] ))"), "6\n");
  EXPECT_EQ(out(sh, "a=(1 2 3); print $(( a[(t=3),3] )) $t"), "3 3\n");
  EXPECT_EQ(out(sh, "a=(5 6 7); a[(1+1)]=9; print $a"), "5 9 7\n");
  // Flags still are flags.
  EXPECT_EQ(out(sh, "a=(x y); print $(( a[(i)y] )); (( a[(r)y] = 5 )); print $a"), "2\nx 5\n");
  EXPECT_TRUE(is_subscript_flags("(i)y"));
  EXPECT_TRUE(is_subscript_flags("(s:,:w)2"));
  EXPECT_FALSE(is_subscript_flags("(1+1)"));
  EXPECT_FALSE(is_subscript_flags("(t=3),3"));
}

TEST(MathM5Review4, IllegalCharactersShownAsZshDoes) {
  EXPECT_EQ(eval("\x01"), "error: bad math expression: illegal character: ^A");
  EXPECT_EQ(eval("\x7f"), "error: bad math expression: illegal character: ^?");
  // A byte with the top bit set, as zsh 5.9 shows 0x80 to 0x82: no `^`.
  MathOptions bytes;
  bytes.multibyte = false;
  EXPECT_EQ(eval("1 \x81", bytes), "error: bad math expression: illegal character: \\M-A");
  EXPECT_EQ(eval("1 \x80", bytes), "error: bad math expression: illegal character: \\M-@");
}

// ---- M5 review, round 5 ------------------------------------------------------

TEST(MathM5Review5, WholeAssociativeArrays) {
  Shell sh;
  // `h[@]` reads the values; assigning to it leaves the array alone.
  EXPECT_EQ(all(sh, "typeset -A h; h=(a 1); (( h[@] = 5 )); echo $?; typeset -p h"),
            "0|0\ntypeset -A h=( [a]=1 )\n|");
  EXPECT_EQ(out(sh, "typeset -A h; h=(a 1); (( h[@] = 5 )) && echo yes; echo $(( h[@] = 7 ))"),
            "yes\n7\n");
  EXPECT_EQ(out(sh, "typeset -A h; h=(a 1); echo $(( h[@] )) $(( h[*] + 1 ))"), "1 2\n");
  EXPECT_EQ(all(sh, "typeset -A h; h=(a 1 b 2); (( h[*] += 5 ))"),
            "2||jfsh: bad math expression: operator expected at `2'\n");
  EXPECT_EQ(all(sh, "typeset -A h; h=(a 1); h[@]=5; echo $?"),
            "1||jfsh: h: attempt to set slice of associative array\n");
  EXPECT_EQ(out(sh, "typeset -p h"), "typeset -A h=( [a]=1 )\n");
  EXPECT_EQ(all(sh, "typeset -A h; h=(a 1); h[*]+=5"),
            "1||jfsh: h: attempt to set slice of associative array\n");
}

TEST(MathM5Review5, FlagErrorsReachArithmetic) {
  Shell sh;
  // A flag the expander doesn't know fails instead of reading 0.
  auto r = sh.run("a=(5 6 7); echo $(( a[(e)2] ))");
  EXPECT_EQ(r.rc, 1);
  EXPECT_EQ(r.out, "");
  EXPECT_NE(r.err, "");
  EXPECT_EQ(out(sh, "a=(5 6 7); echo $(( a[(i)6] )) $(( a[(r)6] ))"), "2 6\n");
  // On the write path too, which leaves the array alone.
  r = sh.run("a=(5 6 7); (( a[(e)2] = 1 )); echo $?; echo $a");
  EXPECT_EQ(r.out, "2\n5 6 7\n");
  EXPECT_NE(r.err, "");
  EXPECT_EQ(out(sh, "x=9; (( x = a[(e)2] )); echo $? $x"), "2 9\n");
  // let has no expander: the flags are bad arithmetic.
  EXPECT_EQ(sh.run("let 'a[(e)2]'").rc, 1);
}

// ---- M5 review, round 6 ------------------------------------------------------

TEST(MathM5Review6, FlagsOnAnAssociativeArrayLeaveItAlone) {
  Shell sh;
  sh.run("typeset -A h; h=(a 1 b 2)");
  EXPECT_EQ(out(sh, "(( h[(k)a] = 5 )); echo $?; (( h[(i)1] = 5 )); echo $?; "
                    "(( h[(I)2] += 5 )); echo $?; (( h[(R)2]++ )); echo $?; typeset -p h"),
            "0\n0\n0\n0\ntypeset -A h=( [a]=1 [b]=2 )\n");
  // The value is what the operator gives on what the flags read.
  EXPECT_EQ(out(sh, "echo $(( h[(i)1] += 5 )) $(( h[(r)2]++ )) $(( h[(r)2] *= 3 )) "
                    "$(( ++h[(r)2] ))"),
            "5 2 6 3\n");
}

TEST(MathM5Review6, WholeArraysInArithmetic) {
  Shell sh;
  // `=` makes a number of an array or association.
  EXPECT_EQ(out(sh, "typeset -A h; h=(a 1 b 2); (( h = 5 )); typeset -p h"), "typeset -i h=5\n");
  EXPECT_EQ(out(sh, "unset h; typeset -A h; h=(a 1); (( h = 1.5 )); typeset -p h"),
            "typeset -F h=1.5000000000\n");
  EXPECT_EQ(out(sh, "a=(1 2); echo $(( a = 3 )); typeset -p a"), "3\ntypeset -i a=3\n");
  EXPECT_EQ(out(sh, "unset a; a=(5); (( a = a + 1 )); typeset -p a"), "typeset -i a=6\n");
  EXPECT_EQ(out(sh, "f() { local -a la=(1 2); (( la = 5 )); typeset -p la; }; f"),
            "typeset -i la=5\n");
  // Other assignments leave an association and give an array one element.
  EXPECT_EQ(out(sh, "unset h; typeset -A h; h=(a 1); (( h++ )); (( h *= 3 )); (( h-- )); "
                    "typeset -p h; echo $(( h++ )) $(( h += 4 ))"),
            "typeset -A h=( [a]=1 )\n1 5\n");
  EXPECT_EQ(out(sh, "unset a; a=(5); (( a += 1 )); typeset -p a; (( a++ )); typeset -p a"),
            "typeset -a a=( 6 )\ntypeset -a a=( 7 )\n");
  // A tied or special array keeps its type even with `=`.
  EXPECT_EQ(out(sh, "typeset -T TT tt; tt=(1 2); (( tt = 5 )); typeset -p tt"),
            "typeset -a tt=( 5 )\n");
  EXPECT_EQ(out(sh, "argv=(1 2); (( argv = 5 )); echo $# $1; (( argv++ )); echo $1"), "1 5\n6\n");
  EXPECT_EQ(sh.run("readonly -a ra=(1 2); (( ra = 5 ))").rc, 2);
}

TEST(MathM5Review6, WholeSubscriptAsWritten) {
  Shell sh;
  // Only `@` or `*` as written is the whole parameter; one from an
  // expansion is a key, or bad arithmetic.
  EXPECT_EQ(out(sh, "typeset -A h; h=(a 1); k=@; h[$k]=5; k='*'; h[$k]+=6; typeset -p h"),
            "typeset -A h=( ['*']=6 [@]=5 [a]=1 )\n");
  EXPECT_EQ(sh.run("a=(1 2 3); k=@; a[$k]=x").rc, 1);
  EXPECT_EQ(sh.run("integer i=1; k=@; i[$k]=5").rc, 1);
  EXPECT_EQ(all(sh, "integer i=1; j=0; i[j++]=4"),
            "1||jfsh: i: assignment to invalid subscript range\n");
  // In arithmetic the expansion comes first, so `@` is the whole array.
  EXPECT_EQ(out(sh, "typeset -A g; g=(a 1); k=@; (( g[$k] = 5 )); typeset -p g"),
            "typeset -A g=( [a]=1 )\n");
  EXPECT_EQ(all(sh, "typeset -A g; g=(a 1); g[@]=5"),
            "1||jfsh: g: attempt to set slice of associative array\n");
}

}  // namespace
}  // namespace jfsh
