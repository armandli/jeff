#include <string>
#include <string_view>

#include <gtest/gtest.h>

#include <jfsh/lexer/input.h>
#include <jfsh/parser/parser.h>

namespace jfsh {
namespace {

Result<ProgRef> parse(std::string src) {
  StringInputSource in{std::move(src)};
  Lexer lex{in};
  lex.push_state(LEX_SKIPCOMMENTS);
  Parser parser{lex};
  return parser.parse();
}

// Disassembly of src, or the parse error message prefixed with "error: ".
std::string listing(std::string src) {
  auto prog = parse(std::move(src));
  if (not prog) return "error: " + prog.error().message;
  return disassemble(**prog);
}

TEST(Parser, EmptyInputIsJustEnd) {
  EXPECT_EQ(listing(""),           "END\n");
  EXPECT_EQ(listing("\n\n"),       "END\n");
  EXPECT_EQ(listing("# comment"),  "END\n");
}

TEST(Parser, SimpleCommandIsPrefixWithSkipLengths) {
  EXPECT_EQ(listing("echo hi"),
            "LIST end skip=5\n"
            "SUBLIST - skip=4\n"
            "PIPE 1\n"
            "SIMPLE 2\n"
            "STRING \"echo\"\n"
            "STRING \"hi\"\n"
            "END\n");
}

TEST(Parser, AssignmentsPrecedeTheCommand) {
  EXPECT_EQ(listing("X=1 Y+=two cmd a"),
            "LIST end skip=11\n"
            "SUBLIST - skip=10\n"
            "PIPE 1\n"
            "ASSIGN -\n"
            "STRING \"X\"\n"
            "STRING \"1\"\n"
            "ASSIGN append\n"
            "STRING \"Y\"\n"
            "STRING \"two\"\n"
            "SIMPLE 2\n"
            "STRING \"cmd\"\n"
            "STRING \"a\"\n"
            "END\n");
}

TEST(Parser, AssignmentOnlyCommandHasNoWords) {
  EXPECT_EQ(listing("X='a b'"),
            "LIST end skip=6\n"
            "SUBLIST - skip=5\n"
            "PIPE 1\n"
            "ASSIGN -\n"
            "STRING \"X\"\n"
            "STRING \"'a b'\"\n"
            "SIMPLE 0\n"
            "END\n");
}

TEST(Parser, OnlyUnquotedLeadingIdentifiersAreAssignments) {
  // A quoted name, a name starting with a digit, and an assignment after the
  // command word are all ordinary words.
  for (const char* src : {"'X'=1", "1X=2", "=x"})
    EXPECT_NE(listing(src).find("SIMPLE 1\n"), std::string::npos) << src;
  EXPECT_NE(listing("cmd X=1").find("SIMPLE 2\n"), std::string::npos);
}

TEST(Parser, AndOrNotChain) {
  EXPECT_EQ(listing("a && b || ! c"),
            "LIST end skip=12\n"
            "SUBLIST and skip=3\n" "PIPE 1\n" "SIMPLE 1\n" "STRING \"a\"\n"
            "SUBLIST or skip=3\n"  "PIPE 1\n" "SIMPLE 1\n" "STRING \"b\"\n"
            "SUBLIST not skip=3\n" "PIPE 1\n" "SIMPLE 1\n" "STRING \"c\"\n"
            "END\n");
}

TEST(Parser, NewlineAfterConnectorContinuesTheChain) {
  EXPECT_EQ(listing("a &&\n\nb"), listing("a && b"));
}

TEST(Parser, ListElementsAndAsync) {
  EXPECT_EQ(listing("a; b &"),
            "LIST - skip=4\n"           "SUBLIST - skip=3\n" "PIPE 1\n"
                                        "SIMPLE 1\n" "STRING \"a\"\n"
            "LIST async|end skip=4\n"   "SUBLIST - skip=3\n" "PIPE 1\n"
                                        "SIMPLE 1\n" "STRING \"b\"\n"
            "END\n");
  EXPECT_EQ(listing("a\n\nb\n"), listing("a; b"));
  EXPECT_EQ(listing("a;"), listing("a"));
}

TEST(Parser, PipelineCountsItsCommands) {
  // The PIPE line encodes both the stage count and a per-connector `|&` mask
  // using the character `&` for the second connector in `a | b |& c`.
  EXPECT_NE(listing("a | b |& c").find("PIPE 3 |&"), std::string::npos);
  EXPECT_NE(listing("a | b | c").find("PIPE 3 ||"),  std::string::npos);
}

TEST(Parser, RedirectionsAttachToSimpleCommands) {
  // A simple command with a `>` redirection: WC_REDIR + target string come
  // before WC_SIMPLE.
  EXPECT_NE(listing("echo hi > f").find("REDIR >\nSTRING \"f\"\nSIMPLE 2\n"),
            std::string::npos);
  // Fd prefix is recorded on the WC_REDIR instruction itself.
  EXPECT_NE(listing("a 2>err").find("REDIR > fd=2"), std::string::npos);
  // `>&-` closes an fd — explicit target is empty.
  EXPECT_NE(listing("a >&-").find("REDIR >&-"), std::string::npos);
}

TEST(Parser, HeredocBodyLandsInStringTable) {
  // Unquoted <<EOF: body in disassembly is the content between the opening
  // line and the EOF marker.  The disassembler emits raw newlines inside the
  // STRING literal rather than escaped ones.
  auto out = listing("cat <<EOF\nhello\nEOF\n");
  EXPECT_NE(out.find("REDIR <<\nSTRING \"hello\n\""), std::string::npos);
  // <<-EOF strips one leading tab per line.
  auto out2 = listing("cat <<-END\n\tx\n\ty\n\tEND\n");
  EXPECT_NE(out2.find("STRING \"x\ny\n\""), std::string::npos);
}

TEST(Parser, QuotedHeredocDelimiterMarksTheBodyLiteral) {
  EXPECT_NE(listing("cat <<'EOF'\n$x\nEOF\n").find("REDIR << literal\nSTRING \"$x\n\""),
            std::string::npos);
  EXPECT_NE(listing("cat <<EOF\n$x\nEOF\n").find("REDIR <<\nSTRING \"$x\n\""),
            std::string::npos);
}

// The body is read at the end of the line, so the rest of that line and
// the next lines still parse as commands (an M2 bug).
TEST(Parser, HeredocLeavesTheRestOfTheLineAndTheNextCommand) {
  auto out = listing("cat <<A | tr a b\nbody\nA\necho next\n");
  EXPECT_NE(out.find("PIPE 2"), std::string::npos) << out;
  EXPECT_NE(out.find("STRING \"body\n\""), std::string::npos) << out;
  EXPECT_NE(out.find("STRING \"next\""), std::string::npos) << out;
}

TEST(Parser, ArrayAssignment) {
  EXPECT_EQ(listing("a=(x 'y z') b+=(1)"),
            "LIST end skip=9\n"
            "SUBLIST - skip=8\n"
            "PIPE 1\n"
            "ASSIGN array\n"
            "STRING \"a\"\n"
            "STRING \"x 'y z'\"\n"
            "ASSIGN append|array\n"
            "STRING \"b\"\n"
            "STRING \"1\"\n"
            "SIMPLE 0\n"
            "END\n");
}

// Reserved words are only special in command position (bug B7).
TEST(Parser, ReservedWordsAreArgumentsAfterTheCommandWord) {
  EXPECT_NE(listing("echo done in if !").find("SIMPLE 5\n"), std::string::npos);
}

// Leftover tokens are a parse error rather than silently dropped (bug B9).
TEST(Parser, RejectsLeftoverAndMisplacedTokens) {
  EXPECT_EQ(listing("echo )"),  "error: parse error near `)'");
  EXPECT_EQ(listing("fi"),      "error: parse error near `fi'");
  EXPECT_EQ(listing("a ; ;"),   "error: parse error near `;'");
  EXPECT_EQ(listing("a &&"),    "error: parse error: unexpected end of input");
}

TEST(Parser, UnimplementedConstructsFailClearly) {
  EXPECT_EQ(listing("time a"), "error: parse error near `time'");
}

// ---- compound commands (M4) ------------------------------------------------

// The listing of a body holding one command of one word.
std::string body(std::string_view word) {
  return "LIST end skip=4\nSUBLIST - skip=3\nPIPE 1\nSIMPLE 1\n"
         "STRING \"" + std::string(word) + "\"\nEND\n";
}

// The listing of a program holding one command whose own words, after
// the PIPE, are `cmd` (n words).
std::string program(std::size_t n, const std::string& cmd) {
  return "LIST end skip=" + std::to_string(n + 2) + "\n"
         "SUBLIST - skip=" + std::to_string(n + 1) + "\n"
         "PIPE 1\n" + cmd + "END\n";
}

TEST(Parser, IfHasABranchPerConditionAndElse) {
  EXPECT_EQ(listing("if a; then b; elif c; then d; else e; fi"),
            program(34,
                    "IF head skip=33\n"
                    "IF if skip=12\n" + body("a") + body("b") +
                    "IF if skip=12\n" + body("c") + body("d") +
                    "IF else skip=6\n" + body("e")));
}

TEST(Parser, WhileAndUntil) {
  EXPECT_EQ(listing("while a; do b; done"),
            program(13, "WHILE while skip=12\n" + body("a") + body("b")));
  EXPECT_EQ(listing("until a\ndo\n  b\ndone"),
            program(13, "WHILE until skip=12\n" + body("a") + body("b")));
}

TEST(Parser, ForForms) {
  EXPECT_EQ(listing("for x y in 1 2; do b; done"),
            program(13, "FOR in skip=12\nWORDS 2\nSTRING \"x\"\nSTRING \"y\"\n"
                        "WORDS 2\nSTRING \"1\"\nSTRING \"2\"\n" + body("b")));
  EXPECT_EQ(listing("for x (1) b"),
            program(11, "FOR in skip=10\nWORDS 1\nSTRING \"x\"\n"
                        "WORDS 1\nSTRING \"1\"\n" + body("b")));
  EXPECT_EQ(listing("for x; do b; done"),
            program(9, "FOR args skip=8\nWORDS 1\nSTRING \"x\"\n" + body("b")));
  EXPECT_EQ(listing("for ((i=0; i<2; i++)) { b }"),
            program(10, "FOR_COND skip=9\nSTRING \"i=0\"\nSTRING \" i<2\"\n"
                        "STRING \" i++\"\n" + body("b")));
  // Reserved words in the value list are values (bug B7).
  EXPECT_NE(listing("for x in do done; do b; done")
                .find("WORDS 2\nSTRING \"do\"\nSTRING \"done\"\n"),
            std::string::npos);
}

TEST(Parser, CaseArmsCarryTheirTerminator) {
  EXPECT_EQ(listing("case $x in a|b) c;; (d) e;& f) g;| esac"),
            program(30,
                    "CASE head skip=29\nSTRING \"$x\"\n"
                    "CASE ;; skip=9\nWORDS 2\nSTRING \"a\"\nSTRING \"b\"\n" + body("c") +
                    "CASE ;& skip=8\nWORDS 1\nSTRING \"d\"\n" + body("e") +
                    "CASE ;| skip=8\nWORDS 1\nSTRING \"f\"\n" + body("g")));
  // The last arm may leave out `;;`.
  EXPECT_EQ(listing("case x in\n  a) b\nesac"),
            program(11, "CASE head skip=10\nSTRING \"x\"\n"
                        "CASE ;; skip=8\nWORDS 1\nSTRING \"a\"\n" + body("b")));
}

TEST(Parser, GroupsAndSubshells) {
  EXPECT_EQ(listing("{ a }"), program(7, "CURSH skip=6\n" + body("a")));
  EXPECT_EQ(listing("( a )"), program(7, "SUBSH skip=6\n" + body("a")));
  EXPECT_EQ(listing("{ a } always { b }"),
            program(13, "TRY skip=12\n" + body("a") + body("b")));
  // `))` closes two subshells.
  EXPECT_EQ(listing("((a))").substr(0, 5), "LIST ");
  EXPECT_EQ(listing("(a; (b))").find("error"), std::string::npos);
}

TEST(Parser, RedirectionsAfterACompoundCommandGoInFront) {
  EXPECT_EQ(listing("( a ) > out 2>&1"),
            program(11, "REDIR >\nSTRING \"out\"\nREDIR >&N fd=2\nSTRING \"1\"\n"
                        "SUBSH skip=6\n" + body("a")));
}

TEST(Parser, FunctionDefinitions) {
  std::string named = "FUNCDEF named skip=9\nWORDS 2\nSTRING \"f\"\nSTRING \"g\"\n" + body("a");
  EXPECT_EQ(listing("f g () { a; }"), program(10, named));
  EXPECT_EQ(listing("function f g { a }"), program(10, named));
  EXPECT_EQ(listing("f() a"),
            program(9, "FUNCDEF named skip=8\nWORDS 1\nSTRING \"f\"\n" + body("a")));
  EXPECT_EQ(listing("() { a } x y"),
            program(10, "FUNCDEF anon skip=9\n" + body("a") +
                        "WORDS 2\nSTRING \"x\"\nSTRING \"y\"\n"));
  EXPECT_EQ(listing("f() {\n}"),
            program(4, "FUNCDEF named skip=3\nWORDS 1\nSTRING \"f\"\nEND\n"));
}

TEST(Parser, ArithmeticCommandKeepsItsTextRaw) {
  EXPECT_EQ(listing("(( x > 1 ))"), program(2, "ARITH skip=1\nSTRING \" x > 1 \"\n"));
}

// ---- [[ ]] (M5) ---------------------------------------------------------------

TEST(Parser, CondUnaryBinaryAndSingleWord) {
  EXPECT_EQ(listing("[[ -f x ]]"),
            program(3, "COND unary skip=2\nSTRING \"-f\"\nSTRING \"x\"\n"));
  EXPECT_EQ(listing("[[ $a -lt 2 ]]"),
            program(4, "COND binary skip=3\nSTRING \"-lt\"\nSTRING \"$a\"\nSTRING \"2\"\n"));
  EXPECT_EQ(listing("[[ $x ]]"), program(2, "COND str skip=1\nSTRING \"$x\"\n"));
  // `<` and `>` compare strings instead of redirecting.
  EXPECT_EQ(listing("[[ b < c ]]"),
            program(4, "COND binary skip=3\nSTRING \"<\"\nSTRING \"b\"\nSTRING \"c\"\n"));
  // `-n = -n` compares two strings.
  EXPECT_EQ(listing("[[ -n = -n ]]"),
            program(4, "COND binary skip=3\nSTRING \"=\"\nSTRING \"-n\"\nSTRING \"-n\"\n"));
}

TEST(Parser, CondPatternOperandKeepsParenthesesAndBars) {
  EXPECT_EQ(listing("[[ $f == *.(c|h) ]]"),
            program(4, "COND binary skip=3\nSTRING \"==\"\nSTRING \"$f\"\n"
                       "STRING \"*.(c|h)\"\n"));
  EXPECT_EQ(listing("[[ $x =~ ^(a|b)+$ ]]"),
            program(4, "COND binary skip=3\nSTRING \"=~\"\nSTRING \"$x\"\n"
                       "STRING \"^(a|b)+$\"\n"));
  EXPECT_EQ(listing("[[ a != \"x y\"* ]]"),
            program(4, "COND binary skip=3\nSTRING \"!=\"\nSTRING \"a\"\n"
                       "STRING \"\"x y\"*\"\n"));
}

TEST(Parser, CondLogicIsATreeWithSkips) {
  // (a && !b) || c: && binds tighter, and each header precedes its operands.
  EXPECT_EQ(listing("[[ a && ! b || c ]]"),
            program(9, "COND or skip=8\nCOND and skip=5\n"
                       "COND str skip=1\nSTRING \"a\"\n"
                       "COND not skip=2\nCOND str skip=1\nSTRING \"b\"\n"
                       "COND str skip=1\nSTRING \"c\"\n"));
  EXPECT_EQ(listing("[[ ( a || b ) && c ]]"),
            program(8, "COND and skip=7\nCOND or skip=4\n"
                       "COND str skip=1\nSTRING \"a\"\nCOND str skip=1\nSTRING \"b\"\n"
                       "COND str skip=1\nSTRING \"c\"\n"));
  EXPECT_EQ(listing("[[ a ||\n  b ]]"),
            program(5, "COND or skip=4\nCOND str skip=1\nSTRING \"a\"\n"
                       "COND str skip=1\nSTRING \"b\"\n"));
}

TEST(Parser, CondIsACommandOnlyInCommandPosition) {
  EXPECT_NE(listing("echo [[ a ]]").find("SIMPLE 4\n"), std::string::npos);
  EXPECT_EQ(listing("[[ a ]] 2>/dev/null"),
            program(4, "REDIR > fd=2\nSTRING \"/dev/null\"\n"
                       "COND str skip=1\nSTRING \"a\"\n"));
  EXPECT_NE(listing("! [[ a ]] && b").find("SUBLIST and|not"), std::string::npos);
}

TEST(Parser, CondErrors) {
  EXPECT_EQ(listing("[[ ]]"), "error: parse error near `]]'");
  EXPECT_EQ(listing("[[ a b ]]"), "error: parse error: condition expected: a");
  EXPECT_EQ(listing("[[ a == a|b ]]"), "error: parse error near `|'");
  EXPECT_EQ(listing("[[ ( a ]]"), "error: parse error near `]]'");
  EXPECT_TRUE(is_incomplete("[[ a =="));
  EXPECT_TRUE(is_incomplete("[[ a &&"));
  EXPECT_TRUE(is_incomplete("[[ -n a"));
  EXPECT_FALSE(is_incomplete("[[ -n a ]]"));
}

TEST(Parser, TypesetArgumentsAreAssignments) {
  EXPECT_EQ(listing("local -a x=$(a b) y z=(1 2)"),
            program(10, "TYPESET 5\nSTRING \"local\"\nSTRING \"-a\"\n"
                        "ASSIGN -\nSTRING \"x\"\nSTRING \"$(a b)\"\nSTRING \"y\"\n"
                        "ASSIGN array\nSTRING \"z\"\nSTRING \"1 2\"\n"));
  // Only the unquoted command word counts.
  EXPECT_NE(listing("echo local x=1").find("SIMPLE 3\n"), std::string::npos);
}

TEST(Parser, CompoundCommandErrors) {
  EXPECT_EQ(listing("if a; then b"), "error: parse error: unexpected end of input");
  EXPECT_EQ(listing("if a; then b; done"), "error: parse error near `done'");
  EXPECT_EQ(listing("for in a; do b; done"), "error: parse error near `in'");
  EXPECT_EQ(listing("case x in a b) c;; esac"), "error: parse error near `b'");
  EXPECT_EQ(listing("for ((a;b)); do c; done"), "error: parse error near `((a;b))'");
  EXPECT_EQ(listing("{ a } <(b)"), "error: parse error near `<(b)'");
}

TEST(Parser, IncompleteInputAsksForMore) {
  EXPECT_TRUE(is_incomplete("if a; then"));
  EXPECT_TRUE(is_incomplete("for x in a b\ndo"));
  EXPECT_TRUE(is_incomplete("f() {"));
  EXPECT_TRUE(is_incomplete("case x in"));
  EXPECT_TRUE(is_incomplete("(( 1 +"));
  EXPECT_TRUE(is_incomplete("echo 'open"));
  EXPECT_TRUE(is_incomplete("cat <<E\nbody"));
  EXPECT_TRUE(is_incomplete("a &&"));
  EXPECT_FALSE(is_incomplete("if a; then b; fi"));
  EXPECT_FALSE(is_incomplete("cat <<E\nbody\nE\n"));
  EXPECT_FALSE(is_incomplete("echo )"));      // an error, not unfinished
  EXPECT_FALSE(is_incomplete("echo # if"));
}

TEST(Parser, ErrorCarriesTheLineNumber) {
  auto prog = parse("echo a\necho )");
  ASSERT_FALSE(prog);
  EXPECT_EQ(prog.error().code, 2);
}

}  // namespace
}  // namespace jfsh
