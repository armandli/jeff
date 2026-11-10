#pragma once
// jfsh/parser/parser.hh — shell parser.
//
// Mirrors zsh's parse.c: par_list() → par_sublist() → par_pline() →
// par_cmd() → par_simple().  Consumes tokens from the Lexer and emits
// a CompiledProgram (WordCode bytecode + string table).

#include <jfsh/core/wordcode.hh>
#include <jfsh/lexer/lexer.hh>

namespace jfsh {

// ---------------------------------------------------------------------------
// ParseFlags — control parsing context
// ---------------------------------------------------------------------------
enum ParseFlags : std::uint32_t {
    PARSE_NORMAL    = 0,
    PARSE_INTERACTIVE = 1 << 0,   // in interactive mode (allow multiline)
    PARSE_SUBSHELL  = 1 << 1,     // inside ( … )
    PARSE_FUNCDEF   = 1 << 2,     // inside function body
    PARSE_COND      = 1 << 3,     // inside [[ … ]]
    PARSE_ARITH     = 1 << 4,     // inside (( … ))
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------
class Parser {
public:
    explicit Parser(Lexer& lex, ParseFlags flags = PARSE_NORMAL);

    // Parse a complete list (until EOF or interactive newline).
    // Returns a compiled program ready for execution.
    Result<ProgRef> parse();

    // Parse a single simple command (for `eval` and string evals).
    Result<ProgRef> parse_string(std::string_view src);

    // Current source line (for error messages).
    int line() const noexcept { return lex_.line(); }

    // Did parsing hit an error?
    bool has_error() const noexcept { return !error_.message.empty(); }
    const ShellError& error() const noexcept { return error_; }

private:
    Lexer&      lex_;
    ParseFlags  flags_;
    ShellError  error_;

    // Output buffer under construction.
    CompiledProgram* prog_ = nullptr;

    // Emit helpers
    std::size_t emit(WordCode w);
    std::size_t emit_str(std::string s);    // adds to strs table, emits WC_STRING
    void        patch(std::size_t addr, WordCode w);  // back-patch a jump

    // Grammar productions (recursive descent, mirroring zsh's par_* funcs)
    bool par_list();
    bool par_list1();
    bool par_sublist();
    bool par_sublist2();
    bool par_pline();
    bool par_cmd();
    bool par_simple();
    bool par_if();
    bool par_while();
    bool par_for();
    bool par_case();
    bool par_select();
    bool par_repeat();
    bool par_subsh();
    bool par_funcdef();
    bool par_cond();
    bool par_arith();
    bool par_redir();
    bool par_assign();

    // Token helpers
    const LexToken& cur()      { return lex_.peek(); }
    bool   eat(TokType t);
    bool   at(TokType t) const noexcept;
    void   skip_newlines();
    void   error(std::string msg);
};

} // namespace jfsh
