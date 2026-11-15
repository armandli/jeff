#include <algorithm>
#include <cctype>
#include <memory>
#include <vector>

#include <jfsh/lexer/input.h>
#include <jfsh/parser/parser.h>

namespace jfsh {

Parser::Parser(Lexer& lex, ParseFlags flags)
  : mLex{lex}, mFlags{flags} {}

Result<ProgRef> Parser::parse() {
  auto prog = std::make_shared<CompiledProgram>();
  mProg  = prog.get();
  mError = {};
  mIncomplete = false;
  bool ok = par_list();
  if (ok and not at(TokType::TK_EOF)) ok = error_near(mLex.peek());
  if (not ok and mError.message.empty()) error_near(mLex.peek());
  if (not ok) {
    mProg = nullptr;
    mPendingHeredocs.clear();
    return std::unexpected(mError);
  }
  auto bodies = mLex.take_heredocs();
  for (std::size_t i = 0; i < mPendingHeredocs.size() and i < bodies.size(); ++i)
    prog->strs[mPendingHeredocs[i]] = std::move(bodies[i]);
  mPendingHeredocs.clear();
  emit(make_wc(WC_END, 0));
  mProg = nullptr;
  return prog;
}

bool is_incomplete(std::string_view src, bool comments) {
  StringInputSource in{std::string(src)};
  Lexer lex{in};
  if (comments) lex.push_state(LEX_SKIPCOMMENTS);
  Parser parser{lex};
  auto prog = parser.parse();
  return lex.incomplete() or (not prog and parser.incomplete());
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

// Reserved words are only reserved in command position; anywhere else
// (`echo done`, `git log --until …`) they are ordinary words.
static bool is_reserved_word(TokType t) noexcept {
  return (t >= TokType::TK_IF and t <= TokType::TK_ALWAYS)
      or t == TokType::TK_BANG;
}

bool Parser::at_word() const noexcept {
  return at(TokType::TK_WORD) or is_reserved_word(mLex.peek().type);
}

bool Parser::expect(TokType t) {
  if (eat(t)) return true;
  return error_near(mLex.peek());
}

// `<(…)`, `>(…)` or `=(…)`: process substitution, which the lexer returns
// as a redirection token holding the whole command.
static bool is_procsubst(std::string_view text) noexcept {
  return text.size() >= 2 and text.back() == ')'
     and (text.starts_with("<(") or text.starts_with(">(")
          or text.starts_with("=("));
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
  mIncomplete = tok.is_eof();
  if (tok.is_eof())          error("parse error: unexpected end of input");
  else if (tok.is_newline()) error("parse error near `\\n'");
  else                       error("parse error near `" + tok.text + "'");
  return false;
}

bool Parser::unsupported(std::string_view what) {
  error(std::string(what) + " is not supported yet");
  return false;
}

// An empty list is allowed (`f() { }`); at the top level the token that
// stopped it is then reported by parse().
bool Parser::par_list() {
  skip_newlines();
  if (at_list_terminator()) return true;
  return par_list1();
}

bool Parser::par_body() {
  if (not par_list()) return false;
  emit(make_wc(WC_END, 0));
  return true;
}

void Parser::emit_words(std::vector<std::string> words) {
  emit(make_wc(WC_WORDS, static_cast<WordCode>(words.size())));
  for (auto& w : words) emit_str(std::move(w));
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
  std::size_t start = mProg->prog.size();
  bool ok = false;
  switch (mLex.peek().type) {
    case TokType::TK_IF:       ok = par_if();      break;
    case TokType::TK_WHILE:
    case TokType::TK_UNTIL:    ok = par_while();   break;
    case TokType::TK_FOR:      ok = par_for();     break;
    case TokType::TK_CASE:     ok = par_case();    break;
    case TokType::TK_SELECT:   ok = par_select();  break;
    case TokType::TK_REPEAT:   ok = par_repeat();  break;
    case TokType::TK_LPAREN:   ok = par_subsh();   break;
    case TokType::TK_LBRACE:   ok = par_cursh();   break;
    case TokType::TK_FUNCTION: ok = par_funcdef(); break;
    case TokType::TK_DLPAREN:  ok = par_arith();   break;
    case TokType::TK_WORD:
      if (mLex.peek().text != "[[") return par_simple();
      ok = par_cond();
      break;
    default:                   return par_simple();
  }
  return ok and par_trailing_redirs(start);
}

bool Parser::par_trailing_redirs(std::size_t start) {
  std::size_t mid = mProg->prog.size();
  while (at(TokType::TK_REDIR)) {
    if (is_procsubst(mLex.peek().text)) return error_near(mLex.peek());
    if (not par_redir()) return false;
  }
  auto& prog = mProg->prog;
  auto& lines = mProg->line_map;
  std::rotate(prog.begin() + start, prog.begin() + mid, prog.end());
  std::rotate(lines.begin() + start, lines.begin() + mid, lines.end());
  return true;
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

// Commands whose `name=value` arguments are assignments, expanded without
// splitting (`local x=$(cmd)`), as zsh's reserved-word typeset does.
static bool is_typeset_command(std::string_view w) noexcept {
  return w == "typeset" or w == "local" or w == "declare" or w == "export"
      or w == "readonly" or w == "integer" or w == "float";
}

// An assignment word after its NAME (of length len): sets the WC_ASSIGN
// flags and the value, without the parentheses of an array.
static std::string assignment_value(const std::string& word, std::size_t len,
                                    bool append, WordCode& flags) {
  std::string value = word.substr(len + (append ? 2 : 1));
  flags = append ? WC_ASSIGN_APPEND : 0;
  if (value.size() >= 2 and value.front() == '(' and value.back() == ')') {
    flags |= WC_ASSIGN_ARRAY;
    value = value.substr(1, value.size() - 2);
  }
  return value;
}

// simple := (assign | redir)* WC_SIMPLE(argc) WC_STRING{argc}
//         | (assign | redir)* WC_TYPESET(n) item{n}
// A word list followed by `()` is a function definition instead.
bool Parser::par_simple() {
  struct Item {
    std::string word;      // the name, for an assignment
    std::string value;
    WordCode    flags  = 0;
    bool        assign = false;
  };
  std::vector<std::string> words;
  std::vector<Item>        items;   // typeset arguments, in order
  bool typeset = false;
  int  nassign = 0;
  int  nredir  = 0;
  for (;;) {
    const auto& tok = mLex.peek();
    if (tok.type == TokType::TK_REDIR) {
      // Process substitution reads like a redirection to the lexer, but it
      // behaves like a WORD whose value is `/dev/fd/N` once the pipe is up.
      // Forward it through as an argument; the executor sets up the pipe
      // during word expansion and substitutes the path in-place.
      if (is_procsubst(tok.text)) {
        words.push_back(tok.text);
        if (typeset) items.push_back({tok.text});
        mLex.consume();
        continue;
      }
      if (not par_redir()) return false;
      ++nredir;
      continue;
    }
    if (tok.type == TokType::TK_WORD) {
      bool append = false;
      std::size_t len = (words.empty() or typeset)
                      ? assignment_name_len(tok.text, append) : 0;
      if (len) {
        WordCode flags = 0;
        std::string value = assignment_value(tok.text, len, append, flags);
        if (typeset) {
          items.push_back({tok.text.substr(0, len), std::move(value), flags, true});
        } else {
          emit(make_wc(WC_ASSIGN, flags));
          emit_str(tok.text.substr(0, len));
          emit_str(std::move(value));
          ++nassign;
        }
      } else {
        if (words.empty() and is_typeset_command(tok.text)) typeset = true;
        words.push_back(tok.text);
        if (typeset) items.push_back({tok.text});
      }
      mLex.consume();
      continue;
    }
    if (not words.empty() and is_reserved_word(tok.type)) {
      words.push_back(tok.text);
      if (typeset) items.push_back({tok.text});
      mLex.consume();
      continue;
    }
    break;
  }
  if (at(TokType::TK_LPAREN) and not words.empty() and not typeset
      and nassign == 0 and nredir == 0) {
    mLex.consume();
    if (not expect(TokType::TK_RPAREN)) return false;
    return par_funcdef_body(std::move(words));
  }
  if (words.empty() and nassign == 0 and nredir == 0)
    return error_near(mLex.peek());
  if (typeset) {
    emit(make_wc(WC_TYPESET, static_cast<WordCode>(items.size())));
    for (auto& item : items) {
      if (item.assign) {
        emit(make_wc(WC_ASSIGN, item.flags));
        emit_str(std::move(item.word));
        emit_str(std::move(item.value));
      } else {
        emit_str(std::move(item.word));
      }
    }
    return true;
  }
  emit(make_wc(WC_SIMPLE, static_cast<WordCode>(words.size())));
  for (auto& w : words) emit_str(std::move(w));
  return true;
}

// if := WC_IF(HEAD, skip) branch+, one branch for `if`, each `elif` and
// the `else`.
bool Parser::par_if() {
  mLex.consume();  // if
  std::size_t head = emit(make_wc_cmd(WC_IF, WC_IF_HEAD, 0));
  for (;;) {
    std::size_t branch = emit(make_wc_cmd(WC_IF, WC_IF_IF, 0));
    if (not par_body()) return false;  // condition
    if (not expect(TokType::TK_THEN)) return false;
    if (not par_body()) return false;
    patch(branch, make_wc_cmd(WC_IF, WC_IF_IF, words_after(*mProg, branch)));
    if (eat(TokType::TK_ELIF)) continue;
    if (eat(TokType::TK_ELSE)) {
      std::size_t other = emit(make_wc_cmd(WC_IF, WC_IF_ELSE, 0));
      if (not par_body()) return false;
      patch(other, make_wc_cmd(WC_IF, WC_IF_ELSE, words_after(*mProg, other)));
    }
    if (not expect(TokType::TK_FI)) return false;
    break;
  }
  patch(head, make_wc_cmd(WC_IF, WC_IF_HEAD, words_after(*mProg, head)));
  return true;
}

bool Parser::par_loop_body() {
  skip_newlines();
  if (eat(TokType::TK_DO))
    return par_body() and expect(TokType::TK_DONE);
  if (eat(TokType::TK_LBRACE))
    return par_body() and expect(TokType::TK_RBRACE);
  return par_sublist_body();
}

bool Parser::par_sublist_body() {
  std::size_t list = emit(make_wc_list(0, 0));
  if (not par_sublist()) return false;
  patch(list, make_wc_list(WC_LIST_END, words_after(*mProg, list)));
  emit(make_wc(WC_END, 0));
  return true;
}

// while := WC_WHILE(WHILE|UNTIL, skip) body(cond) body
bool Parser::par_while() {
  WordCode type = at(TokType::TK_UNTIL) ? WC_WHILE_UNTIL : WC_WHILE_WHILE;
  mLex.consume();
  std::size_t head = emit(make_wc_cmd(WC_WHILE, type, 0));
  if (not par_body()) return false;
  if (not par_loop_body()) return false;
  patch(head, make_wc_cmd(WC_WHILE, type, words_after(*mProg, head)));
  return true;
}

// The rest of `for`/`select` after the keyword (head already emitted):
//   names in words ;    names ( words )    names [;]   (loops over "$@")
bool Parser::par_for_words(std::size_t head, WordCode op) {
  std::vector<std::string> names;
  while (at(TokType::TK_WORD)) {
    names.push_back(mLex.peek().text);
    mLex.consume();
  }
  if (names.empty()) return error_near(mLex.peek());
  skip_newlines();
  WordCode type = WC_FOR_IN;
  std::vector<std::string> values;
  if (eat(TokType::TK_IN)) {
    while (at_word()) {
      values.push_back(mLex.peek().text);
      mLex.consume();
    }
    if (not (eat(TokType::TK_SEMI) or eat(TokType::TK_NEWLINE)))
      return error_near(mLex.peek());
  } else if (eat(TokType::TK_LPAREN)) {
    for (skip_newlines(); not eat(TokType::TK_RPAREN); skip_newlines()) {
      if (not at_word()) return error_near(mLex.peek());
      values.push_back(mLex.peek().text);
      mLex.consume();
    }
  } else {
    type = WC_FOR_ARGS;
    eat(TokType::TK_SEMI);
  }
  emit_words(std::move(names));
  if (type == WC_FOR_IN) emit_words(std::move(values));
  if (not par_loop_body()) return false;
  patch(head, make_wc_cmd(op, type, words_after(*mProg, head)));
  return true;
}

// Splits the text of `for (( init; cond; step ))` at its top-level `;`s.
static bool split_for_cond(std::string_view text, std::vector<std::string>& out) {
  int depth = 0;
  std::size_t start = 0;
  for (std::size_t i = 0; i < text.size(); ++i) {
    if (text[i] == '(') ++depth;
    else if (text[i] == ')') --depth;
    else if (text[i] == ';' and depth == 0) {
      out.emplace_back(text.substr(start, i - start));
      start = i + 1;
    }
  }
  out.emplace_back(text.substr(start));
  return out.size() == 3;
}

// for  := WC_FOR(IN|ARGS, skip) words(names) [words(values)] body
// cfor := WC_FOR_COND(0, skip) WC_STRING{3} body
bool Parser::par_for() {
  mLex.consume();  // for
  if (at(TokType::TK_DLPAREN)) {
    mLex.consume();
    auto text = mLex.read_arith();
    if (not text) return error_near(mLex.peek());
    std::vector<std::string> parts;
    if (not split_for_cond(*text, parts)) {
      error("parse error near `((" + *text + "))'");
      return false;
    }
    std::size_t head = emit(make_wc_cmd(WC_FOR_COND, 0, 0));
    for (auto& part : parts) emit_str(std::move(part));
    eat(TokType::TK_SEMI);
    if (not par_loop_body()) return false;
    patch(head, make_wc_cmd(WC_FOR_COND, 0, words_after(*mProg, head)));
    return true;
  }
  std::size_t head = emit(make_wc_cmd(WC_FOR, 0, 0));
  return par_for_words(head, WC_FOR);
}

// case := WC_CASE(HEAD, skip) WC_STRING(subject) arm*
// arm  := WC_CASE(BREAK|FALL|TESTNEXT, skip) words(patterns) body
bool Parser::par_case() {
  mLex.consume();  // case
  if (not at_word()) return error_near(mLex.peek());
  std::string subject = mLex.peek().text;
  mLex.consume();
  skip_newlines();
  if (not expect(TokType::TK_IN)) return false;
  std::size_t head = emit(make_wc_cmd(WC_CASE, WC_CASE_HEAD, 0));
  emit_str(std::move(subject));
  for (;;) {
    skip_newlines();
    if (eat(TokType::TK_ESAC)) break;
    std::size_t arm = emit(make_wc_cmd(WC_CASE, WC_CASE_BREAK, 0));
    eat(TokType::TK_LPAREN);
    std::vector<std::string> patterns;
    for (;;) {
      if (not at_word()) return error_near(mLex.peek());
      patterns.push_back(mLex.peek().text);
      mLex.consume();
      if (not eat(TokType::TK_PIPE)) break;
    }
    if (not expect(TokType::TK_RPAREN)) return false;
    emit_words(std::move(patterns));
    if (not par_body()) return false;
    WordCode type = WC_CASE_BREAK;
    if      (eat(TokType::TK_SEMIAMP)) type = WC_CASE_FALL;
    else if (eat(TokType::TK_SEMIBAR)) type = WC_CASE_TESTNEXT;
    else if (not eat(TokType::TK_DSEMI) and not at(TokType::TK_ESAC))
      return error_near(mLex.peek());
    patch(arm, make_wc_cmd(WC_CASE, type, words_after(*mProg, arm)));
  }
  patch(head, make_wc_cmd(WC_CASE, WC_CASE_HEAD, words_after(*mProg, head)));
  return true;
}

// select := WC_SELECT(IN|ARGS, skip), laid out like for.
bool Parser::par_select() {
  mLex.consume();  // select
  std::size_t head = emit(make_wc_cmd(WC_SELECT, 0, 0));
  return par_for_words(head, WC_SELECT);
}

// repeat := WC_REPEAT(0, skip) WC_STRING(count) body
bool Parser::par_repeat() {
  mLex.consume();  // repeat
  if (not at_word()) return error_near(mLex.peek());
  std::size_t head = emit(make_wc_cmd(WC_REPEAT, 0, 0));
  emit_str(mLex.peek().text);
  mLex.consume();
  eat(TokType::TK_SEMI);
  if (not par_loop_body()) return false;
  patch(head, make_wc_cmd(WC_REPEAT, 0, words_after(*mProg, head)));
  return true;
}

// subsh := WC_SUBSH(0, skip) body, or `() { … } args`, an anonymous
// function called at once: WC_FUNCDEF(ANON, skip) body words(args).
bool Parser::par_subsh() {
  mLex.consume();  // (
  if (eat(TokType::TK_RPAREN)) {
    std::size_t head = emit(make_wc_cmd(WC_FUNCDEF, WC_FUNCDEF_ANON, 0));
    if (not par_function_body()) return false;
    std::vector<std::string> args;
    while (at_word()) {
      args.push_back(mLex.peek().text);
      mLex.consume();
    }
    emit_words(std::move(args));
    patch(head, make_wc_cmd(WC_FUNCDEF, WC_FUNCDEF_ANON, words_after(*mProg, head)));
    return true;
  }
  std::size_t head = emit(make_wc_cmd(WC_SUBSH, 0, 0));
  if (not par_body() or not expect(TokType::TK_RPAREN)) return false;
  patch(head, make_wc_cmd(WC_SUBSH, 0, words_after(*mProg, head)));
  return true;
}

// cursh := WC_CURSH(0, skip) body
// try   := WC_TRY(0, skip) body body     `{ … } always { … }`
bool Parser::par_cursh() {
  mLex.consume();  // {
  std::size_t head = emit(make_wc_cmd(WC_CURSH, 0, 0));
  if (not par_body() or not expect(TokType::TK_RBRACE)) return false;
  WordCode op = WC_CURSH;
  if (eat(TokType::TK_ALWAYS)) {
    op = WC_TRY;
    if (not expect(TokType::TK_LBRACE)) return false;
    if (not par_body() or not expect(TokType::TK_RBRACE)) return false;
  }
  patch(head, make_wc_cmd(op, 0, words_after(*mProg, head)));
  return true;
}

// `function name… [()] body`; `name… () body` comes through par_simple.
bool Parser::par_funcdef() {
  mLex.consume();  // function
  std::vector<std::string> names;
  while (at(TokType::TK_WORD)) {
    names.push_back(mLex.peek().text);
    mLex.consume();
  }
  if (names.empty()) return error_near(mLex.peek());
  if (eat(TokType::TK_LPAREN) and not expect(TokType::TK_RPAREN)) return false;
  return par_funcdef_body(std::move(names));
}

// funcdef := WC_FUNCDEF(NAMED, skip) words(names) body
bool Parser::par_funcdef_body(std::vector<std::string> names) {
  std::size_t head = emit(make_wc_cmd(WC_FUNCDEF, WC_FUNCDEF_NAMED, 0));
  emit_words(std::move(names));
  if (not par_function_body()) return false;
  patch(head, make_wc_cmd(WC_FUNCDEF, WC_FUNCDEF_NAMED, words_after(*mProg, head)));
  return true;
}

bool Parser::par_function_body() {
  skip_newlines();
  if (eat(TokType::TK_LBRACE))
    return par_body() and expect(TokType::TK_RBRACE);
  return par_sublist_body();
}

void Parser::insert(std::size_t start, WordCode w) {
  auto at = static_cast<std::ptrdiff_t>(start);
  mProg->prog.insert(mProg->prog.begin() + at, w);
  mProg->line_map.insert(mProg->line_map.begin() + at,
                         static_cast<std::uint32_t>(mLex.line()));
}

bool Parser::at_cond_word() noexcept {
  return at_word() and not (at(TokType::TK_WORD) and mLex.peek().text == "]]");
}

// `[[ cond ]]`, read with the lexer in LEX_COND state, so `<` and `>` are
// words.  Newlines may come between the parts.
//   or      := and (|| and)*
//   and     := not (&& not)*
//   not     := ! not | primary
//   primary := ( or ) | word | unop word | word binop word
bool Parser::par_cond() {
  mLex.consume();  // [[
  mLex.push_state(static_cast<LexFlags>(mLex.state() | LEX_COND));
  bool ok = par_cond_or();
  if (ok) {
    skip_newlines();
    if (at(TokType::TK_WORD) and mLex.peek().text == "]]") {
      mLex.pop_state();
      mLex.consume();
      return true;
    }
    ok = error_near(mLex.peek());
  }
  mLex.pop_state();
  return ok;
}

// cond := WC_COND(OR, skip) cond cond, left-associative; the header goes in
// front of the left side once the `||` is seen.
bool Parser::par_cond_or() {
  std::size_t start = mProg->prog.size();
  if (not par_cond_and()) return false;
  for (;;) {
    skip_newlines();
    if (not eat(TokType::TK_PIPEPIPE)) return true;
    skip_newlines();
    if (not par_cond_and()) return false;
    insert(start, make_wc_cmd(WC_COND, WC_COND_OR,
                              static_cast<WordCode>(mProg->prog.size() - start)));
  }
}

bool Parser::par_cond_and() {
  std::size_t start = mProg->prog.size();
  if (not par_cond_not()) return false;
  for (;;) {
    skip_newlines();
    if (not eat(TokType::TK_AMPAMP)) return true;
    skip_newlines();
    if (not par_cond_not()) return false;
    insert(start, make_wc_cmd(WC_COND, WC_COND_AND,
                              static_cast<WordCode>(mProg->prog.size() - start)));
  }
}

bool Parser::par_cond_not() {
  skip_newlines();
  if (not eat(TokType::TK_BANG)) return par_cond_primary();
  std::size_t head = emit(make_wc_cmd(WC_COND, WC_COND_NOT, 0));
  if (not par_cond_not()) return false;
  patch(head, make_wc_cmd(WC_COND, WC_COND_NOT, words_after(*mProg, head)));
  return true;
}

static bool is_cond_pattern_op(std::string_view op) noexcept {
  return op == "=" or op == "==" or op == "!=" or op == "=~";
}

static bool is_cond_binary_op(std::string_view op) noexcept {
  return is_cond_pattern_op(op) or op == "<" or op == ">"
      or op == "-nt" or op == "-ot" or op == "-ef" or op == "-eq" or op == "-ne"
      or op == "-lt" or op == "-le" or op == "-gt" or op == "-ge";
}

bool Parser::par_cond_primary() {
  skip_newlines();
  if (eat(TokType::TK_LPAREN)) {
    if (not par_cond_or()) return false;
    skip_newlines();
    return expect(TokType::TK_RPAREN);
  }
  if (not at_cond_word()) return error_near(mLex.peek());
  std::string w1 = mLex.peek().text;
  mLex.consume();
  // A single word: true when it is not empty.
  if (not at_cond_word()) {
    emit(make_wc_cmd(WC_COND, WC_COND_STR, 1));
    emit_str(std::move(w1));
    return true;
  }
  std::string w2 = mLex.peek().text;
  // `-x arg`, unless the second word is an operator (`-n = -n`).  An
  // unknown `-x` is reported when the condition runs, as zsh does.
  bool binary = is_cond_binary_op(w2)
             or ((w1.size() < 2 or w1[0] != '-') and w2.size() > 1 and w2[0] == '-');
  if (not binary) {
    if (w1.size() < 2 or w1[0] != '-') {
      error("parse error: condition expected: " + w1);
      return false;
    }
    mLex.consume();
    emit(make_wc_cmd(WC_COND, WC_COND_UNARY, 2));
    emit_str(std::move(w1));
    emit_str(std::move(w2));
    return true;
  }
  mLex.consume();
  std::string w3;
  if (is_cond_pattern_op(w2)) {
    auto pat = mLex.read_cond_pattern();
    if (not pat or *pat == "]]") {
      if (pat) mLex.unread({TokType::TK_WORD, *pat, mLex.line()});
      return error_near(mLex.peek());
    }
    w3 = std::move(*pat);
  } else {
    if (not at_cond_word()) return error_near(mLex.peek());
    w3 = mLex.peek().text;
    mLex.consume();
  }
  emit(make_wc_cmd(WC_COND, WC_COND_BINARY, 3));
  emit_str(std::move(w2));
  emit_str(std::move(w1));
  emit_str(std::move(w3));
  return true;
}

// arith := WC_ARITH(0, 1) WC_STRING(expr)
bool Parser::par_arith() {
  mLex.consume();  // ((
  auto text = mLex.read_arith();
  if (not text) return error_near(mLex.peek());
  emit(make_wc_cmd(WC_ARITH, 0, 1));
  emit_str(std::move(*text));
  return true;
}

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
  if (is_procsubst(op_tok.text)) {
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
    // The lexer reads the body once it reaches the end of this line;
    // parse() fills the placeholder in.
    emit(make_wc_redir(type, fd, quoted));
    mPendingHeredocs.push_back(emit_str({}));
    return true;
  }

  emit(make_wc_redir(type, fd));
  emit_str(std::move(target));
  return true;
}

// `name=value`, `name+=value`, `name=(…)` and `name+=(…)` words are all
// recognized in par_simple; the lexer keeps an array value in one word.
// TODO(parser/assign): subscripted assignments `name[i]=value`.
bool Parser::par_assign() { return unsupported("subscripted assignment"); }

}  // namespace jfsh
