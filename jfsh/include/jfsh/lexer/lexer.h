#ifndef JFSH_LEXER_LEXER_H
#define JFSH_LEXER_LEXER_H
// jfsh/lexer/lexer.h — shell lexer (tokenizer).
//
// Mirrors zsh's lex.c: gettok() / gettokstr() / zshlex().

#include <cstdint>

#include <optional>
#include <string>
#include <vector>

#include <jfsh/core/types.h>

namespace jfsh {

enum class TokType : std::uint16_t {
  TK_NULL = 0,
  TK_EOF,
  TK_NEWLINE,
  TK_SEMI,         // ;
  TK_DSEMI,        // ;;
  TK_SEMIAMP,      // ;&
  TK_SEMIBAR,      // ;|  (fallthrough)
  TK_AMP,          // &
  TK_AMPAMP,       // &&
  TK_PIPE,         // |
  TK_PIPEPIPE,     // ||
  TK_PIPEAMP,      // |& (stderr + stdout pipe)
  TK_BANG,         // !
  TK_AMPPAREN,     // &(  async subshell
  TK_LPAREN,       // (
  TK_RPAREN,       // )
  TK_DLPAREN,      // ((
  TK_DRPAREN,      // ))
  TK_LBRACE,       // {
  TK_RBRACE,       // }
  TK_REDIR,        // any < > << >> <> >| >& &> <<< <( >( =( etc.
  TK_WORD,         // an unquoted or quoted word
  TK_STRING,       // here-document delimiter
  TK_IF, TK_THEN, TK_ELIF, TK_ELSE, TK_FI,
  TK_FOR, TK_IN, TK_DO, TK_DONE,
  TK_WHILE, TK_UNTIL,
  TK_CASE, TK_ESAC,
  TK_SELECT,
  TK_REPEAT,
  TK_FUNCTION,
  TK_TIME,
  TK_COPROC,
  TK_ALWAYS,
  TK_MAX,
};

struct LexToken {
  TokType     type = TokType::TK_NULL;
  std::string text;
  int         line = 0;

  bool is_word()    const noexcept { return type == TokType::TK_WORD; }
  bool is_newline() const noexcept { return type == TokType::TK_NEWLINE; }
  bool is_eof()     const noexcept { return type == TokType::TK_EOF; }
};

enum LexFlags : std::uint32_t {
  LEX_NORMAL       = 0,
  LEX_SKIPCOMMENTS = 1 << 0,
  LEX_CASEMOD      = 1 << 1,
  LEX_ACTIVE       = 1 << 2,
  LEX_DOCSTRING    = 1 << 3,
};

struct InputSource {
  virtual ~InputSource() = default;
  virtual int  next_char() = 0;
  virtual void unget(int c) = 0;
  virtual int  line() const noexcept { return 0; }
};

struct Lexer {
  explicit Lexer(InputSource& src);
  ~Lexer();

  LexToken         next_token();
  const LexToken&  peek();
  void             consume();
  int              line() const noexcept;

  void push_state(LexFlags flags);
  void pop_state();

  void set_alias_expansion(bool on) noexcept { mAliasExpand = on; }

  void        register_heredoc(std::string delim, bool strip_tabs, bool expand);
  std::string read_heredoc();

protected:
  LexToken read_token();
  LexToken read_word(int first_char);
  LexToken read_redir(int first_char);
  LexToken read_heredoc_delimiter();
  bool     is_keyword(std::string_view s) const noexcept;

private:
  struct HeredocSpec {
    std::string delim;
    bool        strip_tabs;
    bool        expand;
  };

  InputSource&              mSrc;
  std::optional<LexToken>   mPeeked;
  LexFlags                  mFlags       = LEX_NORMAL;
  bool                      mAliasExpand = true;
  std::vector<LexFlags>     mStateStack;
  std::vector<HeredocSpec>  mHeredocQueue;
};

}  // namespace jfsh

#endif  // JFSH_LEXER_LEXER_H
