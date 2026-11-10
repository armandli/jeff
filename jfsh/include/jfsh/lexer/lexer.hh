#pragma once
// jfsh/lexer/lexer.hh — shell lexer (tokenizer).
//
// Mirrors zsh's lex.c: gettok() / gettokstr() / zshlex().
// The lexer reads one token at a time from the current input source and
// produces a stream of LexToken values consumed by the parser.

#include <jfsh/core/types.hh>
#include <cstdint>
#include <string>
#include <optional>

namespace jfsh {

// ---------------------------------------------------------------------------
// Token types — mirrors zsh's ttype enumeration
// ---------------------------------------------------------------------------
enum class TokType : std::uint16_t {
    // End / error
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
    // Parentheses / braces
    TK_LPAREN,       // (
    TK_RPAREN,       // )
    TK_DLPAREN,      // ((
    TK_DRPAREN,      // ))
    TK_LBRACE,       // {
    TK_RBRACE,       // }
    // Redirection openers (the fd and type come from the token text)
    TK_REDIR,        // any < > << >> <> >| >& &> <<< <( >( =( etc.
    // Words / strings
    TK_WORD,         // an unquoted or quoted word
    TK_STRING,       // here-document delimiter
    // Keywords (checked via keyword table)
    TK_IF, TK_THEN, TK_ELIF, TK_ELSE, TK_FI,
    TK_FOR, TK_IN, TK_DO, TK_DONE,
    TK_WHILE, TK_UNTIL,
    TK_CASE, TK_ESAC,
    TK_SELECT,
    TK_REPEAT,
    TK_FUNCTION,
    TK_TIME,
    TK_COPROC,
    TK_ALWAYS,       // zsh always/try
    TK_MAX
};

// ---------------------------------------------------------------------------
// LexToken — one token from the input stream
// ---------------------------------------------------------------------------
struct LexToken {
    TokType     type    = TokType::TK_NULL;
    std::string text;           // raw text (may include quoting)
    int         line    = 0;    // source line number

    bool is_word()    const noexcept { return type == TokType::TK_WORD; }
    bool is_newline() const noexcept { return type == TokType::TK_NEWLINE; }
    bool is_eof()     const noexcept { return type == TokType::TK_EOF; }
};

// ---------------------------------------------------------------------------
// LexerState flags (mirrors zsh lexflags / LEXFLAGS_*)
// ---------------------------------------------------------------------------
enum LexFlags : std::uint32_t {
    LEX_NORMAL       = 0,
    LEX_SKIPCOMMENTS = 1 << 0,  // discard # comments
    LEX_CASEMOD      = 1 << 1,  // inside case pattern
    LEX_ACTIVE       = 1 << 2,  // lexer is running
    LEX_DOCSTRING    = 1 << 3,  // inside here-doc body
};

// ---------------------------------------------------------------------------
// InputSource — abstraction over string buffer / file descriptor / readline
// ---------------------------------------------------------------------------
struct InputSource {
    virtual ~InputSource() = default;
    virtual int  next_char() = 0;   // returns next byte or EOF (-1)
    virtual void unget(int c) = 0;  // push back one char
    virtual int  line() const noexcept { return 0; }
};

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------
class Lexer {
public:
    explicit Lexer(InputSource& src);
    ~Lexer();

    // Advance to the next token; returns it.
    LexToken next_token();

    // Peek at the next token without consuming it.
    const LexToken& peek();

    // Consume the peeked token.
    void consume();

    // Current line number.
    int  line() const noexcept;

    // Push/pop lexer state for nested sub-lexing (e.g. $(…)).
    void push_state(LexFlags flags);
    void pop_state();

    // Set the flag controlling whether aliases are expanded.
    void set_alias_expansion(bool on) noexcept { alias_expand_ = on; }

    // Here-document support: register a pending here-doc and read its body.
    void register_heredoc(std::string delim, bool strip_tabs, bool expand);
    std::string read_heredoc();

private:
    InputSource&            src_;
    std::optional<LexToken> peeked_;
    LexFlags                flags_    = LEX_NORMAL;
    bool                    alias_expand_ = true;

    std::vector<LexFlags>   state_stack_;

    // Here-doc queue: (delimiter, strip_tabs, expand)
    struct HeredocSpec { std::string delim; bool strip_tabs; bool expand; };
    std::vector<HeredocSpec> heredoc_queue_;

    LexToken read_token();
    LexToken read_word(int first_char);
    LexToken read_redir(int first_char);
    LexToken read_heredoc_delimiter();
    bool     is_keyword(std::string_view s) const noexcept;
};

} // namespace jfsh
