#include <memory>

#include <jfsh/parser/parser.hh>

namespace jfsh {

Parser::Parser(Lexer& lex, ParseFlags flags)
  : mLex{lex}, mFlags{flags} {}

Result<ProgRef> Parser::parse() {
  auto prog = std::make_shared<CompiledProgram>();
  mProg = prog.get();
  if (not par_list()) {
    if (not mError.message.empty())
      return std::unexpected(mError);
  }
  mProg = nullptr;
  return prog;
}

Result<ProgRef> Parser::parse_string(std::string_view /*src*/) {
  // TODO: create a StringInputSource, Lexer, and delegate to parse()
  return std::unexpected(ShellError{
    ErrorKind::ParseError, "parse_string not yet implemented"});
}

std::size_t Parser::emit(WordCode w) {
  std::size_t pos = mProg->prog.size();
  mProg->prog.push_back(w);
  mProg->line_map.push_back(static_cast<std::uint32_t>(mLex.line()));
  return pos;
}

std::size_t Parser::emit_str(std::string s) {
  std::size_t idx = mProg->strs.size();
  mProg->strs.push_back(std::move(s));
  emit(make_wc(WC_STRING, static_cast<WordCode>(idx)));
  return idx;
}

void Parser::patch(std::size_t addr, WordCode w) {
  mProg->prog[addr] = w;
}

bool Parser::eat(TokType t) {
  if (at(t)) { mLex.consume(); return true; }
  return false;
}

bool Parser::at(TokType t) const noexcept {
  return mLex.peek().type == t;
}

void Parser::skip_newlines() {
  while (at(TokType::TK_NEWLINE)) mLex.consume();
}

void Parser::error(std::string msg) {
  mError = ShellError{ErrorKind::ParseError, std::move(msg), mLex.line()};
}

bool Parser::par_list() {
  skip_newlines();
  if (at(TokType::TK_EOF)) return true;
  return par_list1();
}

bool Parser::par_list1() {
  if (not par_sublist()) return false;
  while (at(TokType::TK_SEMI)
         or at(TokType::TK_AMP)
         or at(TokType::TK_NEWLINE)) {
    bool async = at(TokType::TK_AMP);
    mLex.consume();
    skip_newlines();
    if (async) emit(make_wc(WC_LIST, WC_LIST_ASYNC));
    if (at(TokType::TK_EOF)
        or at(TokType::TK_RBRACE)
        or at(TokType::TK_FI)
        or at(TokType::TK_DONE)
        or at(TokType::TK_ESAC))
      break;
    if (not par_sublist()) return false;
  }
  emit(make_wc(WC_LIST, WC_LIST_END));
  return true;
}

bool Parser::par_sublist() {
  if (not par_pline()) return false;
  while (at(TokType::TK_AMPAMP) or at(TokType::TK_PIPEPIPE)) {
    WordCode flag = at(TokType::TK_AMPAMP) ? WC_SUBLIST_AND : WC_SUBLIST_OR;
    mLex.consume();
    skip_newlines();
    emit(make_wc(WC_SUBLIST, flag));
    if (not par_pline()) return false;
  }
  return true;
}

bool Parser::par_pline() {
  bool bang = at(TokType::TK_BANG);
  if (bang) mLex.consume();
  if (not par_cmd()) return false;
  while (at(TokType::TK_PIPE) or at(TokType::TK_PIPEAMP)) {
    mLex.consume();
    skip_newlines();
    emit(make_wc(WC_PIPE, 0));
    if (not par_cmd()) return false;
  }
  if (bang) emit(make_wc(WC_SUBLIST, WC_SUBLIST_NOT));
  return true;
}

bool Parser::par_cmd() {
  skip_newlines();
  const auto& tok = mLex.peek();
  switch (tok.type) {
    case TokType::TK_IF:       return par_if();
    case TokType::TK_WHILE:
    case TokType::TK_UNTIL:    return par_while();
    case TokType::TK_FOR:      return par_for();
    case TokType::TK_CASE:     return par_case();
    case TokType::TK_SELECT:   return par_select();
    case TokType::TK_REPEAT:   return par_repeat();
    case TokType::TK_LPAREN:   return par_subsh();
    case TokType::TK_LBRACE:
    case TokType::TK_FUNCTION: return par_funcdef();
    case TokType::TK_DLPAREN:  return par_arith();
    default:                   return par_simple();
  }
}

bool Parser::par_simple() {
  int arg_count = 0;
  while (not at(TokType::TK_EOF)
         and not at(TokType::TK_NEWLINE)
         and not at(TokType::TK_SEMI)
         and not at(TokType::TK_AMP)
         and not at(TokType::TK_PIPE)
         and not at(TokType::TK_PIPEPIPE)
         and not at(TokType::TK_AMPAMP)
         and not at(TokType::TK_RPAREN)
         and not at(TokType::TK_RBRACE)) {
    if (at(TokType::TK_REDIR)) { par_redir(); continue; }
    if (at(TokType::TK_WORD)) {
      const auto& w = mLex.peek();
      emit_str(w.text);
      mLex.consume();
      ++arg_count;
    } else {
      break;
    }
  }
  if (arg_count == 0 and mProg->prog.empty()) return true;
  emit(make_wc(WC_SIMPLE, static_cast<WordCode>(arg_count)));
  return true;
}

// TODO(parser/if): consume TK_IF; par_list(); expect TK_THEN; par_list();
//   repeat [TK_ELIF par_list() TK_THEN par_list()]*;
//   optional [TK_ELSE par_list()]; expect TK_FI.
//   Emit WC_IF with offsets to each branch.
bool Parser::par_if()      { mLex.consume(); return true; }

// TODO(parser/while): consume TK_WHILE or TK_UNTIL; par_list() (cond);
//   expect TK_DO; par_list() (body); expect TK_DONE.
//   Emit WC_WHILE with until-flag and body offset.
bool Parser::par_while()   { mLex.consume(); return true; }

// TODO(parser/for): Two forms:
//   `for name in words; do body; done`  → WC_FOR
//   `for ((init; cond; incr)); do body; done` → WC_FOR_COND
bool Parser::par_for()     { mLex.consume(); return true; }

// TODO(parser/case): consume TK_CASE word TK_IN;
//   parse pattern-list: pat [| pat]* ) par_list() (;;|;&|;|) until TK_ESAC.
//   Emit WC_CASE with offset table.
bool Parser::par_case()    { mLex.consume(); return true; }

// TODO(parser/select): Like for — consume TK_SELECT name TK_IN words TK_DO body TK_DONE.
bool Parser::par_select()  { mLex.consume(); return true; }

// TODO(parser/repeat): consume TK_REPEAT word TK_NEWLINE|TK_LBRACE body TK_RBRACE.
bool Parser::par_repeat()  { mLex.consume(); return true; }

// TODO(parser/subsh): consume TK_LPAREN; par_list(); consume TK_RPAREN.
//   Emit WC_SUBSH with body offset.
bool Parser::par_subsh()   { mLex.consume(); return true; }

// TODO(parser/funcdef): Two forms:
//   `function name [()] { body }`
//   `name () { body }`
//   Emit WC_FUNCDEF.
bool Parser::par_funcdef() { mLex.consume(); return true; }

// TODO(parser/cond): consume TK_DLPAREN (for [[);
//   parse binary/unary test operators (-f, -d, =, ==, =~, <, >, -eq, etc.)
//   and logical operators (&& || !).
//   Emit WC_COND with operand string refs.
bool Parser::par_cond()    { mLex.consume(); return true; }

// TODO(parser/arith): consume TK_DLPAREN;
//   collect arithmetic expression tokens until TK_DRPAREN.
//   Emit WC_ARITH with expression string ref.
bool Parser::par_arith()   { mLex.consume(); return true; }

// TODO(parser/redir): Parse fd[redir_type]target:
//   fd is optional integer prefix; redir_type from RedirType enum;
//   target word or here-doc delimiter.
//   register_heredoc() when REDIR_HEREDOC / REDIR_HEREDOCD encountered.
//   Emit WC_REDIR with RedirType and string indices.
bool Parser::par_redir()   { mLex.consume(); return true; }

// TODO(parser/assign): Detect name=value or name+=(array) word.
//   Emit WC_ASSIGN with name and value indices.
bool Parser::par_assign()  { mLex.consume(); return true; }

}  // namespace jfsh
