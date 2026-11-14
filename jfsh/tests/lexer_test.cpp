#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <jfsh/lexer/input.h>
#include <jfsh/lexer/lexer.h>

namespace jfsh {
namespace {

std::vector<LexToken> lex_all(std::string src, LexFlags flags = LEX_NORMAL) {
  StringInputSource in{std::move(src)};
  Lexer lex{in};
  if (flags != LEX_NORMAL) lex.push_state(flags);
  std::vector<LexToken> out;
  for (;;) {
    auto t = lex.next_token();
    if (t.type == TokType::TK_EOF) break;
    out.push_back(std::move(t));
  }
  return out;
}

std::vector<TokType> types(const std::vector<LexToken>& toks) {
  std::vector<TokType> out;
  for (const auto& t : toks) out.push_back(t.type);
  return out;
}

std::vector<std::string> words(const std::vector<LexToken>& toks) {
  std::vector<std::string> out;
  for (const auto& t : toks) out.push_back(t.text);
  return out;
}

TEST(Lexer, SimpleCommandWords) {
  auto toks = lex_all("echo hello   world");
  EXPECT_EQ(types(toks), std::vector<TokType>(3, TokType::TK_WORD));
  EXPECT_EQ(words(toks), (std::vector<std::string>{"echo", "hello", "world"}));
}

TEST(Lexer, ControlOperators) {
  auto toks = lex_all("a | b || c && d ; e & f |& g ;; h");
  using enum TokType;
  EXPECT_EQ(types(toks),
            (std::vector<TokType>{TK_WORD, TK_PIPE, TK_WORD, TK_PIPEPIPE, TK_WORD,
                                  TK_AMPAMP, TK_WORD, TK_SEMI, TK_WORD, TK_AMP,
                                  TK_WORD, TK_PIPEAMP, TK_WORD, TK_DSEMI, TK_WORD}));
}

TEST(Lexer, QuotesKeepWordsTogether) {
  auto toks = lex_all(R"(echo "a b;c" 'd | e' f\ g)");
  EXPECT_EQ(words(toks),
            (std::vector<std::string>{"echo", R"("a b;c")", "'d | e'", R"(f\ g)"}));
}

TEST(Lexer, KeywordsAreRecognized) {
  using enum TokType;
  EXPECT_EQ(types(lex_all("if then elif else fi")),
            (std::vector<TokType>{TK_IF, TK_THEN, TK_ELIF, TK_ELSE, TK_FI}));
  EXPECT_EQ(types(lex_all("for in do done while until case esac")),
            (std::vector<TokType>{TK_FOR, TK_IN, TK_DO, TK_DONE, TK_WHILE,
                                  TK_UNTIL, TK_CASE, TK_ESAC}));
  // A keyword spelling inside a larger word is just a word.
  EXPECT_EQ(types(lex_all("iffy done_x")),
            (std::vector<TokType>{TK_WORD, TK_WORD}));
}

TEST(Lexer, Redirections) {
  auto toks = lex_all("cmd > out >> log < in 2>&1 <<- EOF");
  std::vector<std::string> redirs;
  for (const auto& t : toks)
    if (t.type == TokType::TK_REDIR) redirs.push_back(t.text);
  EXPECT_EQ(redirs, (std::vector<std::string>{">", ">>", "<", "2>&", "<<-"}));
}

TEST(Lexer, RedirectionsFdPrefixAndNewOps) {
  auto toks = lex_all("a 2>out 12<in &>>errlog <&0 >&-");
  std::vector<std::string> redirs;
  for (const auto& t : toks)
    if (t.type == TokType::TK_REDIR) redirs.push_back(t.text);
  EXPECT_EQ(redirs, (std::vector<std::string>{"2>", "12<", "&>>", "<&", ">&"}));
}

TEST(Lexer, ProcessSubstitutionKeepsBalancedParens) {
  auto toks = lex_all("diff <(echo a | wc) <(echo b)");
  std::vector<std::string> redirs;
  for (const auto& t : toks)
    if (t.type == TokType::TK_REDIR) redirs.push_back(t.text);
  EXPECT_EQ(redirs,
            (std::vector<std::string>{"<(echo a | wc)", "<(echo b)"}));
}

// `))` is two tokens so nested subshells close; the parser reads the
// inside of `(( … ))` raw with Lexer::read_arith.
TEST(Lexer, GroupingTokens) {
  using enum TokType;
  EXPECT_EQ(types(lex_all("( a ) { b } (( c ))")),
            (std::vector<TokType>{TK_LPAREN, TK_WORD, TK_RPAREN, TK_LBRACE, TK_WORD,
                                  TK_RBRACE, TK_DLPAREN, TK_WORD, TK_RPAREN,
                                  TK_RPAREN}));
}

TEST(Lexer, ReadArithStopsAtTheClosingParens) {
  StringInputSource in{"(( (a+1) * 2 )) rest"};
  Lexer lex{in};
  EXPECT_EQ(lex.next_token().type, TokType::TK_DLPAREN);
  EXPECT_EQ(lex.read_arith(), " (a+1) * 2 ");
  EXPECT_EQ(lex.next_token().text, "rest");
  StringInputSource open{"(( 1 +"};
  Lexer lex2{open};
  lex2.next_token();
  EXPECT_EQ(lex2.read_arith(), std::nullopt);
}

TEST(Lexer, BangIsATokenOnlyOnItsOwn) {
  using enum TokType;
  auto toks = lex_all("! a != b !x");
  EXPECT_EQ(types(toks), (std::vector<TokType>{TK_BANG, TK_WORD, TK_WORD, TK_WORD, TK_WORD}));
  EXPECT_EQ(words(toks), (std::vector<std::string>{"!", "a", "!=", "b", "!x"}));
}

TEST(Lexer, CondStateMakesComparisonsWords) {
  using enum TokType;
  auto toks = lex_all("a < b > c ((x)) 2>d", LEX_COND);
  EXPECT_EQ(types(toks),
            (std::vector<TokType>{TK_WORD, TK_WORD, TK_WORD, TK_WORD, TK_WORD, TK_LPAREN,
                                  TK_LPAREN, TK_WORD, TK_RPAREN, TK_RPAREN, TK_WORD,
                                  TK_REDIR, TK_WORD}));
  EXPECT_EQ(words(toks)[1], "<");
  EXPECT_EQ(words(toks)[3], ">");
  // Outside [[ ]] the same text is redirections.
  EXPECT_EQ(lex_all("a < b")[1].type, TK_REDIR);
}

TEST(Lexer, ReadCondPatternKeepsGroupsWhole) {
  StringInputSource in{"  *.(c|h)\"x y\"$(a b) ]] rest"};
  Lexer lex{in};
  EXPECT_EQ(lex.read_cond_pattern(), "*.(c|h)\"x y\"$(a b)");
  EXPECT_EQ(lex.next_token().text, "]]");
  StringInputSource stops{"a|b) c"};
  Lexer lex2{stops};
  EXPECT_EQ(lex2.read_cond_pattern(), "a");
  EXPECT_EQ(lex2.next_token().type, TokType::TK_PIPE);
  StringInputSource empty{"   "};
  Lexer lex3{empty};
  EXPECT_EQ(lex3.read_cond_pattern(), std::nullopt);
}

TEST(Lexer, ReportsInputCutOffInsideAQuote) {
  auto incomplete = [](std::string src) {
    StringInputSource in{std::move(src)};
    Lexer lex{in};
    while (not lex.next_token().is_eof()) {}
    lex.take_heredocs();
    return lex.incomplete();
  };
  EXPECT_TRUE(incomplete("echo 'abc"));
  EXPECT_TRUE(incomplete("echo \"abc"));
  EXPECT_TRUE(incomplete("echo abc\\"));
  EXPECT_FALSE(incomplete("echo 'abc'"));
}

TEST(Lexer, NewlinesAndLineNumbers) {
  auto toks = lex_all("a\nb\n\nc");
  using enum TokType;
  EXPECT_EQ(types(toks),
            (std::vector<TokType>{TK_WORD, TK_NEWLINE, TK_WORD, TK_NEWLINE,
                                  TK_NEWLINE, TK_WORD}));
  EXPECT_EQ(toks.front().line, 1);
  EXPECT_EQ(toks.back().line, 4);
}

TEST(Lexer, CommentsSkippedOnlyWhenEnabled) {
  EXPECT_EQ(words(lex_all("ls # list", LEX_SKIPCOMMENTS)),
            std::vector<std::string>{"ls"});
  EXPECT_EQ(words(lex_all("ls # list")),
            (std::vector<std::string>{"ls", "#", "list"}));
}

TEST(Lexer, PeekDoesNotConsume) {
  StringInputSource in{"a b"};
  Lexer lex{in};
  EXPECT_EQ(lex.peek().text, "a");
  EXPECT_EQ(lex.peek().text, "a");
  EXPECT_EQ(lex.next_token().text, "a");
  lex.consume();
  EXPECT_EQ(lex.next_token().type, TokType::TK_EOF);
}

// The lexer ungets the character it peeked even at EOF; that must not rewind
// the input, or the trailing operator would be produced forever (bug B3).
TEST(Lexer, TrailingOperatorAtEofTerminates) {
  for (const char* src : {"echo a;", "echo a&", "echo a|"}) {
    auto toks = lex_all(src);
    EXPECT_EQ(toks.size(), 3u) << src;
  }
}

// Substitutions are one unit of a word, however they nest (bug B8).
TEST(Lexer, SubstitutionsStayOneWord) {
  EXPECT_EQ(words(lex_all("echo $(date +%s) ${HOME}/x")),
            (std::vector<std::string>{"echo", "$(date +%s)", "${HOME}/x"}));
  EXPECT_EQ(words(lex_all("echo $((1 + (2*3))) `a b` ${x:-a b} $(a $(b) \")\")")),
            (std::vector<std::string>{"echo", "$((1 + (2*3)))", "`a b`", "${x:-a b}",
                                      "$(a $(b) \")\")"}));
  EXPECT_EQ(words(lex_all("echo \"$(echo \")\")\" x")),
            (std::vector<std::string>{"echo", "\"$(echo \")\")\"", "x"}));
}

TEST(Lexer, DollarSingleQuotesKeepEscapedQuotes) {
  EXPECT_EQ(words(lex_all("echo $'it\\'s' x")),
            (std::vector<std::string>{"echo", "$'it\\'s'", "x"}));
}

TEST(Lexer, BracesAreReservedOnlyAsWholeWords) {
  using enum TokType;
  EXPECT_EQ(types(lex_all("{ a{b,c} {1..3} {} }")),
            (std::vector<TokType>{TK_LBRACE, TK_WORD, TK_WORD, TK_WORD, TK_RBRACE}));
  EXPECT_EQ(words(lex_all("x a{b,c}d")), (std::vector<std::string>{"x", "a{b,c}d"}));
}

TEST(Lexer, ParenthesesInsideAWord) {
  EXPECT_EQ(words(lex_all("a=(x 'y z') b+=(1) *.(c|h) *(.) f()")),
            (std::vector<std::string>{"a=(x 'y z')", "b+=(1)", "*.(c|h)", "*(.)", "f", "(", ")"}));
}

TEST(Lexer, NumericGlobInsideAWord) {
  using enum TokType;
  auto toks = lex_all("ls f<1-5>.txt a<b");
  EXPECT_EQ(types(toks), (std::vector<TokType>{TK_WORD, TK_WORD, TK_WORD, TK_REDIR, TK_WORD}));
  EXPECT_EQ(toks[1].text, "f<1-5>.txt");
}

TEST(Lexer, HeredocBodiesAreReadAtTheEndOfTheLine) {
  StringInputSource in{"cat <<A <<B | x\nbody a\nA\nbody b\nB\nnext\n"};
  Lexer lex{in};
  std::vector<std::string> seen;
  for (;;) {
    auto t = lex.next_token();
    if (t.is_eof()) break;
    if (t.type == TokType::TK_REDIR) {
      auto delim = lex.next_token();
      seen.push_back(delim.text);
      lex.register_heredoc(delim.text, false, true);
      continue;
    }
    seen.push_back(t.is_newline() ? "NL" : t.text);
  }
  EXPECT_EQ(seen, (std::vector<std::string>{"cat", "A", "B", "|", "x", "NL", "next", "NL"}));
  EXPECT_EQ(lex.take_heredocs(), (std::vector<std::string>{"body a\n", "body b\n"}));
}

}  // namespace
}  // namespace jfsh
