#include <string>
#include <vector>

#include <gtest/gtest.h>

#include <jfsh/lexer/input.hh>
#include <jfsh/lexer/lexer.hh>

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

TEST(Lexer, GroupingTokens) {
  using enum TokType;
  EXPECT_EQ(types(lex_all("( a ) { b } (( c ))")),
            (std::vector<TokType>{TK_LPAREN, TK_WORD, TK_RPAREN, TK_LBRACE, TK_WORD,
                                  TK_RBRACE, TK_DLPAREN, TK_WORD, TK_DRPAREN}));
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

// Known gap: '(' and '{' end a word, so $(...) and ${...} are split into several
// tokens. Enable once command/parameter substitution lexing lands (TODO subst/*).
TEST(Lexer, DISABLED_SubstitutionsStayOneWord) {
  EXPECT_EQ(words(lex_all("echo $(date +%s) ${HOME}/x")),
            (std::vector<std::string>{"echo", "$(date +%s)", "${HOME}/x"}));
}

}  // namespace
}  // namespace jfsh
