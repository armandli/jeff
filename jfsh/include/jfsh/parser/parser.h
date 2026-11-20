#ifndef JFSH_PARSER_PARSER_H
#define JFSH_PARSER_PARSER_H
// jfsh/parser/parser.h — shell parser.
//
// Mirrors zsh's parse.c: par_list() → par_sublist() → par_pline() →
// par_cmd() → par_simple().  Consumes tokens from the Lexer and emits
// a CompiledProgram (WordCode bytecode + string table).

#include <cstddef>
#include <cstdint>

#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <jfsh/core/alias_table.h>
#include <jfsh/core/aliases.h>
#include <jfsh/core/wordcode.h>
#include <jfsh/lexer/lexer.h>

namespace jfsh {

enum ParseFlags : s::uint32_t {
  PARSE_NORMAL      = 0,
  PARSE_INTERACTIVE = 1 << 0,
  PARSE_SUBSHELL    = 1 << 1,
  PARSE_FUNCDEF     = 1 << 2,
  PARSE_COND        = 1 << 3,
  PARSE_ARITH       = 1 << 4,
};

struct Parser {
  explicit Parser(Lexer& lex, ParseFlags flags = PARSE_NORMAL)
    : mLex{lex}, mFlags{flags} {}

  Result<ProgRef> parse();
  Result<ProgRef> parse_string(strv src);
  // One top-level line: the lists up to a newline (or the end of the
  // input), so what it defines (aliases) is in force for the next line.
  // An empty program at the end of the input; `at_eof()` then says so.
  Result<ProgRef> parse_line();
  bool at_eof() { return mLex.peek().is_eof(); }

  // Aliases to expand while parsing; none when null.
  void set_aliases(const AliasTable* aliases) noexcept { mAliases = aliases; }

  int  line()      const noexcept { return mLex.line(); }
  bool has_error() const noexcept { return not mError.message.empty(); }
  // The parse failed only because the input ended too early.
  bool incomplete() const noexcept { return mIncomplete; }

  const ShellError& error() const noexcept { return mError; }

protected:
  s::size_t emit(WordCode w);
  s::size_t emit_str(str s);
  void        patch(s::size_t addr, WordCode w) { mProg->prog[addr] = w; }

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
  bool par_foreach();
  bool par_case();
  bool par_select();
  bool par_repeat();
  bool par_subsh();
  bool par_cursh();
  bool par_funcdef();
  bool par_funcdef_body(vec<str> names);
  // `{ list }`, or any single command (`f() echo hi`).
  bool par_function_body();
  bool wrap_body_redirs(s::size_t start);
  bool par_cond();
  bool par_cond_or();
  bool par_cond_and();
  bool par_cond_not();
  bool par_cond_primary();
  // An operand inside [[ ]]: a word, or a reserved word used as one, but
  // not the closing `]]`.
  bool at_cond_word() noexcept {
    return at_word() and not (at(TokType::TK_WORD) and mLex.peek().text == "]]");
  }
  // Puts a header in front of the instructions from `start` on.
  void insert(s::size_t start, WordCode w) {
    auto at = static_cast<s::ptrdiff_t>(start);
    mProg->prog.insert(mProg->prog.begin() + at, w);
    mProg->line_map.insert(mProg->line_map.begin() + at,
                           static_cast<s::uint32_t>(mLex.line()));
  }
  bool par_arith();
  bool par_redir();
  // `name=value`, `name+=value`, `name=(…)` and `name+=(…)` words are all
  // recognized in par_simple; the lexer keeps an array value in one word.
  // TODO(parser/assign): subscripted assignments `name[i]=value`.
  bool par_assign() { return unsupported("subscripted assignment"); }

  // A nested program: list* WC_END.
  bool par_body();
  // `do list done`, `{ list }`, or with SHORT_LOOPS a single sublist.
  bool par_loop_body();
  // One sublist wrapped as a body (`f() cmd`, `for x (a b) cmd`).
  bool par_sublist_body();
  // `for`/`select` after the keyword: names, then the value list.
  bool par_for_words(s::size_t head, WordCode op);
  // Redirections after a compound command that starts at `start`, moved in
  // front of it.
  bool par_trailing_redirs(s::size_t start);
  void emit_words(vec<str> words);

  // When the next token is an alias that applies here (any alias in
  // command position, a global one anywhere, a suffix alias for a command
  // word ending in .ext), replaces it with the alias text and returns true.
  bool expand_alias(bool cmdpos);

  const LexToken& cur() { return mLex.peek(); }
  bool   eat(TokType t) {
    if (at(t)) { mLex.consume(); return true; }
    return false;
  }
  bool   at(TokType t) const noexcept { return mLex.peek().type == t; }
  bool   at_list_terminator() const noexcept;
  // A word, or a reserved word used as one (`for x in do done`).
  bool   at_word() const noexcept;
  bool   expect(TokType t);                     // eat it, or a parse error
  void   skip_newlines() {
    while (at(TokType::TK_NEWLINE)) mLex.consume();
  }
  void   error(str msg) {
    mError = ShellError{ErrorKind::ParseError, s::move(msg), mLex.line()};
  }
  bool   error_near(const LexToken& tok);       // always returns false
  bool   unsupported(strv what) {
    error(str(what) + " is not supported yet");
    return false;
  }

private:
  Lexer&           mLex;
  ParseFlags       mFlags;
  ShellError       mError;
  bool             mIncomplete = false;
  CompiledProgram* mProg = nullptr;
  const AliasTable* mAliases = nullptr;
  bool             mLineMode = false;   // parse_line: stop at a newline
  vec<s::size_t> mPendingHeredocs;   // strs indices awaiting bodies
};

// True when `src` is the start of a program cut off too early: an open
// `if`, loop, brace, quote or here-document.  An interactive shell then
// reads more lines; `comments` says whether `#` starts a comment.
bool is_incomplete(strv src, bool comments = true);

// Human-readable listing of the bytecode, one instruction per line
// (src/parser/text.cpp).  Used by tests and for debugging.
str disassemble(const CompiledProgram& prog);

// One-line source text, as job listings show it (src/parser/text.cpp):
// the pipeline (WC_PIPE) or sublist chain (WC_SUBLIST) at `pc`, words as
// written and compound commands on one line.
str      pipeline_text(const CompiledProgram& prog, s::size_t pc);
vec<str> pipeline_stage_texts(const CompiledProgram& prog, s::size_t pc);
str      sublist_text(const CompiledProgram& prog, s::size_t pc);

// A function definition as `functions` prints it (src/parser/text.cpp):
// `names () {`, the body one command per line indented by tabs, `}`.
str function_text(strv names, const CompiledProgram& body);

}  // namespace jfsh

#endif  // JFSH_PARSER_PARSER_H
