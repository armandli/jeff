#ifndef JFSH_PARSER_PARSER_H
#define JFSH_PARSER_PARSER_H
// jfsh/parser/parser.h — shell parser.
//
// Mirrors zsh's parse.c: par_list() → par_sublist() → par_pline() →
// par_cmd() → par_simple().  Consumes tokens from the Lexer and emits
// a CompiledProgram (WordCode bytecode + string table).

#include <vector>

#include <jfsh/core/wordcode.h>
#include <jfsh/lexer/lexer.h>

namespace jfsh {

enum ParseFlags : std::uint32_t {
  PARSE_NORMAL      = 0,
  PARSE_INTERACTIVE = 1 << 0,
  PARSE_SUBSHELL    = 1 << 1,
  PARSE_FUNCDEF     = 1 << 2,
  PARSE_COND        = 1 << 3,
  PARSE_ARITH       = 1 << 4,
};

struct Parser {
  explicit Parser(Lexer& lex, ParseFlags flags = PARSE_NORMAL);

  Result<ProgRef> parse();
  Result<ProgRef> parse_string(std::string_view src);

  int  line()      const noexcept { return mLex.line(); }
  bool has_error() const noexcept { return not mError.message.empty(); }
  // The parse failed only because the input ended too early.
  bool incomplete() const noexcept { return mIncomplete; }

  const ShellError& error() const noexcept { return mError; }

protected:
  std::size_t emit(WordCode w);
  std::size_t emit_str(std::string s);
  void        patch(std::size_t addr, WordCode w);

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
  bool par_cursh();
  bool par_funcdef();
  bool par_funcdef_body(std::vector<std::string> names);
  // `{ list }`, or any single command (`f() echo hi`).
  bool par_function_body();
  bool par_cond();
  bool par_arith();
  bool par_redir();
  bool par_assign();

  // A nested program: list* WC_END.
  bool par_body();
  // `do list done`, `{ list }`, or with SHORT_LOOPS a single sublist.
  bool par_loop_body();
  // One sublist wrapped as a body (`f() cmd`, `for x (a b) cmd`).
  bool par_sublist_body();
  // `for`/`select` after the keyword: names, then the value list.
  bool par_for_words(std::size_t head, WordCode op);
  // Redirections after a compound command that starts at `start`, moved in
  // front of it.
  bool par_trailing_redirs(std::size_t start);
  void emit_words(std::vector<std::string> words);

  const LexToken& cur() { return mLex.peek(); }
  bool   eat(TokType t);
  bool   at(TokType t) const noexcept;
  bool   at_list_terminator() const noexcept;
  // A word, or a reserved word used as one (`for x in do done`).
  bool   at_word() const noexcept;
  bool   expect(TokType t);                     // eat it, or a parse error
  void   skip_newlines();
  void   error(std::string msg);
  bool   error_near(const LexToken& tok);       // always returns false
  bool   unsupported(std::string_view what);    // always returns false

private:
  Lexer&           mLex;
  ParseFlags       mFlags;
  ShellError       mError;
  bool             mIncomplete = false;
  CompiledProgram* mProg = nullptr;
  std::vector<std::size_t> mPendingHeredocs;   // strs indices awaiting bodies
};

// True when `src` is the start of a program cut off too early: an open
// `if`, loop, brace, quote or here-document.  An interactive shell then
// reads more lines; `comments` says whether `#` starts a comment.
bool is_incomplete(std::string_view src, bool comments = true);

// Human-readable listing of the bytecode, one instruction per line
// (src/parser/text.cpp).  Used by tests and for debugging.
std::string disassemble(const CompiledProgram& prog);

}  // namespace jfsh

#endif  // JFSH_PARSER_PARSER_H
