#include <cctype>
#include <memory>
#include <vector>

#include <jfsh/lexer/input.hh>
#include <jfsh/parser/parser.hh>

namespace jfsh {

Parser::Parser(Lexer& lex, ParseFlags flags)
  : mLex{lex}, mFlags{flags} {}

Result<ProgRef> Parser::parse() {
  auto prog = std::make_shared<CompiledProgram>();
  mProg  = prog.get();
  mError = {};
  bool ok = par_list();
  if (ok and not at(TokType::TK_EOF)) ok = error_near(mLex.peek());
  if (not ok and mError.message.empty()) error_near(mLex.peek());
  if (not ok) {
    mProg = nullptr;
    return std::unexpected(mError);
  }
  emit(make_wc(WC_END, 0));
  mProg = nullptr;
  return prog;
}

Result<ProgRef> Parser::parse_string(std::string_view src) {
  StringInputSource in{std::string(src)};
  Lexer lex{in};
  lex.push_state(LEX_SKIPCOMMENTS);
  Parser p{lex, mFlags};
  return p.parse();
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

// Words emitted since the instruction at addr.
static WordCode words_after(const CompiledProgram& prog, std::size_t addr) {
  return static_cast<WordCode>(prog.prog.size() - addr - 1);
}

bool Parser::eat(TokType t) {
  if (at(t)) { mLex.consume(); return true; }
  return false;
}

bool Parser::at(TokType t) const noexcept {
  return mLex.peek().type == t;
}

// Tokens that end a list: end of input, or the closing word of whatever
// compound command the list is the body of.
bool Parser::at_list_terminator() const noexcept {
  switch (mLex.peek().type) {
    case TokType::TK_EOF:
    case TokType::TK_RBRACE:  case TokType::TK_RPAREN:
    case TokType::TK_THEN:    case TokType::TK_ELIF:
    case TokType::TK_ELSE:    case TokType::TK_FI:
    case TokType::TK_DO:      case TokType::TK_DONE:
    case TokType::TK_ESAC:    case TokType::TK_DSEMI:
    case TokType::TK_SEMIAMP: case TokType::TK_SEMIBAR:
      return true;
    default:
      return false;
  }
}

void Parser::skip_newlines() {
  while (at(TokType::TK_NEWLINE)) mLex.consume();
}

void Parser::error(std::string msg) {
  mError = ShellError{ErrorKind::ParseError, std::move(msg), mLex.line()};
}

bool Parser::error_near(const LexToken& tok) {
  if (tok.is_eof())          error("parse error: unexpected end of input");
  else if (tok.is_newline()) error("parse error near `\\n'");
  else                       error("parse error near `" + tok.text + "'");
  return false;
}

bool Parser::unsupported(std::string_view what) {
  error(std::string(what) + " is not supported yet");
  return false;
}

bool Parser::par_list() {
  skip_newlines();
  if (at(TokType::TK_EOF)) return true;
  return par_list1();
}

// list := WC_LIST(flags, skip) sublist, repeated for each `;`, `&` or
// newline-separated element.
bool Parser::par_list1() {
  for (;;) {
    std::size_t head = emit(make_wc_list(0, 0));
    if (not par_sublist()) return false;
    WordCode flags = 0;
    bool     more  = false;
    if (at(TokType::TK_SEMI) or at(TokType::TK_AMP) or at(TokType::TK_NEWLINE)) {
      if (at(TokType::TK_AMP)) flags |= WC_LIST_ASYNC;
      mLex.consume();
      skip_newlines();
      more = not at_list_terminator();
    }
    if (not more) flags |= WC_LIST_END;
    patch(head, make_wc_list(flags, words_after(*mProg, head)));
    if (not more) return true;
  }
}

// sublist := WC_SUBLIST(connector | NOT, skip) pipeline, chained by && / ||.
bool Parser::par_sublist() {
  for (;;) {
    std::size_t head = emit(make_wc_sublist(0, 0));
    WordCode flags = eat(TokType::TK_BANG) ? WC_SUBLIST_NOT : 0;
    if (not par_pline()) return false;
    WordCode skip = words_after(*mProg, head);
    if      (at(TokType::TK_AMPAMP))   flags |= WC_SUBLIST_AND;
    else if (at(TokType::TK_PIPEPIPE)) flags |= WC_SUBLIST_OR;
    patch(head, make_wc_sublist(flags, skip));
    if ((flags & WC_SUBLIST_TYPE) == WC_SUBLIST_END) return true;
    mLex.consume();
    skip_newlines();
  }
}

// pipeline := WC_PIPE(count|stderr_mask) command{n}
bool Parser::par_pline() {
  std::size_t head = emit(make_wc_pipe(0, 0));
  WordCode n           = 0;
  WordCode stderr_mask = 0;
  for (;;) {
    if (not par_cmd()) return false;
    ++n;
    if (not (at(TokType::TK_PIPE) or at(TokType::TK_PIPEAMP))) break;
    if (at(TokType::TK_PIPEAMP)) stderr_mask |= (1u << (n - 1));
    mLex.consume();
    skip_newlines();
  }
  patch(head, make_wc_pipe(n, stderr_mask));
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

// `NAME=value` or `NAME+=value` with an unquoted identifier before the `=`.
// Returns the length of NAME, or 0 when the word is not an assignment.
static std::size_t assignment_name_len(std::string_view w, bool& append) {
  auto ident_start = [](char c) {
    return std::isalpha(static_cast<unsigned char>(c)) or c == '_';
  };
  auto ident_char = [](char c) {
    return std::isalnum(static_cast<unsigned char>(c)) or c == '_';
  };
  if (w.empty() or not ident_start(w[0])) return 0;
  std::size_t i = 1;
  while (i < w.size() and ident_char(w[i])) ++i;
  append = false;
  if (i + 1 < w.size() and w[i] == '+' and w[i + 1] == '=') {
    append = true;
    return i;
  }
  return (i < w.size() and w[i] == '=') ? i : 0;
}

// Reserved words are only reserved in command position; anywhere else
// (`echo done`, `git log --until …`) they are ordinary words.
static bool is_reserved_word(TokType t) noexcept {
  return (t >= TokType::TK_IF and t <= TokType::TK_ALWAYS)
      or t == TokType::TK_BANG;
}

// simple := (assign | redir)* WC_SIMPLE(argc) WC_STRING{argc}
bool Parser::par_simple() {
  std::vector<std::string> words;
  int nassign = 0;
  int nredir  = 0;
  for (;;) {
    const auto& tok = mLex.peek();
    if (tok.type == TokType::TK_REDIR) {
      // Process substitution reads like a redirection to the lexer, but it
      // behaves like a WORD whose value is `/dev/fd/N` once the pipe is up.
      // Forward it through as an argument; the executor sets up the pipe
      // during word expansion and substitutes the path in-place.
      if (tok.text.size() >= 2
          and tok.text.back() == ')'
          and (tok.text.substr(0, 2) == "<("
               or tok.text.substr(0, 2) == ">("
               or tok.text.substr(0, 2) == "=(")) {
        words.push_back(tok.text);
        mLex.consume();
        continue;
      }
      if (not par_redir()) return false;
      ++nredir;
      continue;
    }
    if (tok.type == TokType::TK_WORD) {
      bool append = false;
      std::size_t len = words.empty() ? assignment_name_len(tok.text, append) : 0;
      if (len) {
        emit(make_wc(WC_ASSIGN, append ? WC_ASSIGN_APPEND : 0));
        emit_str(tok.text.substr(0, len));
        emit_str(tok.text.substr(len + (append ? 2 : 1)));
        ++nassign;
      } else {
        words.push_back(tok.text);
      }
      mLex.consume();
      continue;
    }
    if (not words.empty() and is_reserved_word(tok.type)) {
      words.push_back(tok.text);
      mLex.consume();
      continue;
    }
    break;
  }
  if (words.empty() and nassign == 0 and nredir == 0)
    return error_near(mLex.peek());
  emit(make_wc(WC_SIMPLE, static_cast<WordCode>(words.size())));
  for (auto& w : words) emit_str(std::move(w));
  return true;
}

// TODO(parser/if): consume TK_IF; par_list(); expect TK_THEN; par_list();
//   repeat [TK_ELIF par_list() TK_THEN par_list()]*;
//   optional [TK_ELSE par_list()]; expect TK_FI.
//   Emit WC_IF with offsets to each branch.
bool Parser::par_if() { return unsupported("`if'"); }

// TODO(parser/while): consume TK_WHILE or TK_UNTIL; par_list() (cond);
//   expect TK_DO; par_list() (body); expect TK_DONE.
//   Emit WC_WHILE with until-flag and body offset.
bool Parser::par_while() { return unsupported("`while'/`until'"); }

// TODO(parser/for): Two forms:
//   `for name in words; do body; done`  → WC_FOR
//   `for ((init; cond; incr)); do body; done` → WC_FOR_COND
bool Parser::par_for() { return unsupported("`for'"); }

// TODO(parser/case): consume TK_CASE word TK_IN;
//   parse pattern-list: pat [| pat]* ) par_list() (;;|;&|;|) until TK_ESAC.
//   Emit WC_CASE with offset table.
bool Parser::par_case() { return unsupported("`case'"); }

// TODO(parser/select): Like for — consume TK_SELECT name TK_IN words TK_DO body TK_DONE.
bool Parser::par_select() { return unsupported("`select'"); }

// TODO(parser/repeat): consume TK_REPEAT word TK_NEWLINE|TK_LBRACE body TK_RBRACE.
bool Parser::par_repeat() { return unsupported("`repeat'"); }

// TODO(parser/subsh): consume TK_LPAREN; par_list(); consume TK_RPAREN.
//   Emit WC_SUBSH with body offset.
bool Parser::par_subsh() { return unsupported("subshell `( … )'"); }

// TODO(parser/funcdef): Two forms:
//   `function name [()] { body }`
//   `name () { body }`
//   Emit WC_FUNCDEF.
bool Parser::par_funcdef() { return unsupported("brace group or function definition"); }

// TODO(parser/cond): consume TK_DLPAREN (for [[);
//   parse binary/unary test operators (-f, -d, =, ==, =~, <, >, -eq, etc.)
//   and logical operators (&& || !).
//   Emit WC_COND with operand string refs.
bool Parser::par_cond() { return unsupported("`[[ … ]]'"); }

// TODO(parser/arith): consume TK_DLPAREN;
//   collect arithmetic expression tokens until TK_DRPAREN.
//   Emit WC_ARITH with expression string ref.
bool Parser::par_arith() { return unsupported("`(( … ))'"); }

// Split a redirection token `text` (produced by the lexer) into an optional
// leading fd prefix (as an int, or -1 when absent) and the operator string.
// `2>&1` → (2, ">&"), `>>` → (-1, ">>"), `3<` → (3, "<").  The special form
// `2>&1` keeps `1` as a separate argument the caller reads next — this
// helper only detects the fd BEFORE the operator, not after.
static std::pair<int, std::string> split_redir_token(const std::string& text) {
  std::size_t i = 0;
  while (i < text.size()
         and std::isdigit(static_cast<unsigned char>(text[i]))) ++i;
  if (i == 0) return {-1, text};
  if (i >= text.size()) return {-1, text};
  int fd = std::atoi(text.substr(0, i).c_str());
  return {fd, text.substr(i)};
}

static bool op_to_redir_type(const std::string& op, RedirType& out) noexcept {
  if (op == "<")   { out = REDIR_READ;       return true; }
  if (op == ">")   { out = REDIR_WRITE;      return true; }
  if (op == ">>")  { out = REDIR_APPEND;     return true; }
  if (op == ">|")  { out = REDIR_CLOBBER;    return true; }
  if (op == "<>")  { out = REDIR_READWRITE;  return true; }
  if (op == "<<")  { out = REDIR_HEREDOC;    return true; }
  if (op == "<<-") { out = REDIR_HEREDOCD;   return true; }
  if (op == "<<<") { out = REDIR_HERESTR;    return true; }
  if (op == ">&")  { out = REDIR_MERGE;      return true; }
  if (op == "<&")  { out = REDIR_IN_DUP;     return true; }
  if (op == "&>")  { out = REDIR_MERGEOUT;   return true; }
  if (op == "&>>") { out = REDIR_APPEND_ERR; return true; }
  if (op == "<(")  { out = REDIR_PROC_IN;    return true; }
  if (op == ">(")  { out = REDIR_PROC_OUT;   return true; }
  if (op == "=(")  { out = REDIR_PROC_EQ;    return true; }
  return false;
}

// Strip one layer of outer quotes from a here-doc delimiter and report
// whether any were present.  `'EOF'` → ("EOF", true); `EOF` → ("EOF", false).
static std::string heredoc_unquote(std::string_view raw, bool& quoted) {
  std::string out;
  quoted = false;
  for (std::size_t i = 0; i < raw.size(); ++i) {
    char c = raw[i];
    if (c == '\'' or c == '"' or c == '\\') { quoted = true; continue; }
    out += c;
  }
  return out;
}

bool Parser::par_redir() {
  LexToken op_tok = mLex.next_token();
  // Process substitution tokens look like `<(cmd)` / `>(cmd)` / `=(cmd)` —
  // the lexer already slurped the balanced parens into text, so recognize
  // them by prefix before invoking the general operator table.
  if (op_tok.text.size() >= 2 and op_tok.text.back() == ')'
      and (op_tok.text.substr(0, 2) == "<("
           or op_tok.text.substr(0, 2) == ">("
           or op_tok.text.substr(0, 2) == "=(")) {
    RedirType type =
        op_tok.text[0] == '<' ? REDIR_PROC_IN :
        op_tok.text[0] == '>' ? REDIR_PROC_OUT : REDIR_PROC_EQ;
    std::string body =
        op_tok.text.substr(2, op_tok.text.size() - 3);  // strip `<(` and `)`
    emit(make_wc_redir(type, -1));
    emit_str(std::move(body));
    return true;
  }

  auto [fd, op] = split_redir_token(op_tok.text);
  RedirType type;
  if (not op_to_redir_type(op, type))
    return unsupported("redirection `" + op_tok.text + "'");

  // REDIR_CLOSE is >&- with no target.
  if (type == REDIR_MERGE or type == REDIR_IN_DUP) {
    const auto& tgt = mLex.peek();
    if (tgt.type == TokType::TK_WORD and tgt.text == "-") {
      mLex.consume();
      emit(make_wc_redir(REDIR_CLOSE, fd));
      emit_str("");
      return true;
    }
  }

  // Normal redirs: expect a WORD target.
  const auto& tgt = mLex.peek();
  if (tgt.type != TokType::TK_WORD) {
    error("expected target after `" + op_tok.text + "'");
    return false;
  }
  std::string target = tgt.text;
  mLex.consume();

  if (type == REDIR_HEREDOC or type == REDIR_HEREDOCD) {
    bool quoted = false;
    std::string delim = heredoc_unquote(target, quoted);
    mLex.register_heredoc(delim, type == REDIR_HEREDOCD, not quoted);
    // Read the body now so it is captured before the next token advances
    // past the newline.  The lexer returns the body as a plain string.
    std::string body = mLex.read_heredoc();
    emit(make_wc_redir(type, fd));
    emit_str(std::move(body));
    return true;
  }

  emit(make_wc_redir(type, fd));
  emit_str(std::move(target));
  return true;
}

// Scalar `name=value` / `name+=value` words are recognized in par_simple.
// TODO(parser/assign): Array assignments `name=(a b c)` / `name+=(…)` — the
//   lexer currently splits the word at `(` (bug B8), so they reach the parser
//   as `name=` followed by TK_LPAREN and fail as a parse error.
bool Parser::par_assign() { return unsupported("array assignment"); }

}  // namespace jfsh
