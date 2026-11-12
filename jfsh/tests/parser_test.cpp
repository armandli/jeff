#include <string>

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
  EXPECT_EQ(listing("if true; then a; fi"), "error: `if' is not supported yet");
  EXPECT_EQ(listing("{ a; }"),
            "error: brace group or function definition is not supported yet");
}

TEST(Parser, ErrorCarriesTheLineNumber) {
  auto prog = parse("echo a\necho )");
  ASSERT_FALSE(prog);
  EXPECT_EQ(prog.error().code, 2);
}

}  // namespace
}  // namespace jfsh
