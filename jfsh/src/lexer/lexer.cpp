#include <cctype>
#include <cstring>

#include <jfsh/lexer/lexer.hh>

namespace jfsh {

static const struct { const char* word; TokType type; } KEYWORDS[] = {
  {"if",       TokType::TK_IF},
  {"then",     TokType::TK_THEN},
  {"elif",     TokType::TK_ELIF},
  {"else",     TokType::TK_ELSE},
  {"fi",       TokType::TK_FI},
  {"for",      TokType::TK_FOR},
  {"in",       TokType::TK_IN},
  {"do",       TokType::TK_DO},
  {"done",     TokType::TK_DONE},
  {"while",    TokType::TK_WHILE},
  {"until",    TokType::TK_UNTIL},
  {"case",     TokType::TK_CASE},
  {"esac",     TokType::TK_ESAC},
  {"select",   TokType::TK_SELECT},
  {"repeat",   TokType::TK_REPEAT},
  {"function", TokType::TK_FUNCTION},
  {"time",     TokType::TK_TIME},
  {"coproc",   TokType::TK_COPROC},
  {"always",   TokType::TK_ALWAYS},
  {nullptr,    TokType::TK_NULL},
};

Lexer::Lexer(InputSource& src) : mSrc{src} {}
Lexer::~Lexer() = default;

// Scoop balanced parentheses so `<(…)`, `>(…)` and `=(…)` become a single
// TK_REDIR token whose text holds the entire substitution source.
static void slurp_balanced_paren(InputSource& src, std::string& text) {
  int depth = 1;
  for (;;) {
    int c = src.next_char();
    if (c == -1) break;
    text += static_cast<char>(c);
    if (c == '(') ++depth;
    else if (c == ')') { if (--depth == 0) break; }
  }
}

bool Lexer::is_keyword(std::string_view s) const noexcept {
  for (auto* k = KEYWORDS; k->word; ++k)
    if (s == k->word) return true;
  return false;
}

LexToken Lexer::next_token() {
  if (mPeeked) {
    auto t = std::move(*mPeeked);
    mPeeked.reset();
    return t;
  }
  return read_token();
}

const LexToken& Lexer::peek() {
  if (not mPeeked) mPeeked = read_token();
  return *mPeeked;
}

void Lexer::consume() {
  if (mPeeked) mPeeked.reset();
  else         read_token();
}

int Lexer::line() const noexcept { return mSrc.line(); }

void Lexer::push_state(LexFlags flags) {
  mStateStack.push_back(mFlags);
  mFlags = flags;
}

void Lexer::pop_state() {
  if (not mStateStack.empty()) {
    mFlags = mStateStack.back();
    mStateStack.pop_back();
  }
}

void Lexer::register_heredoc(std::string delim, bool strip_tabs, bool expand) {
  mHeredocQueue.push_back({std::move(delim), strip_tabs, expand});
}

std::string Lexer::read_heredoc() {
  if (mHeredocQueue.empty()) return {};
  auto spec = std::move(mHeredocQueue.front());
  mHeredocQueue.erase(mHeredocQueue.begin());
  // Discard anything left on the opening line (the newline terminating it)
  // so the body starts cleanly.  Comments or trailing spaces after the
  // delimiter are not part of the body.
  int c;
  while ((c = mSrc.next_char()) != -1 and c != '\n') {}
  std::string body;
  std::string line_buf;
  for (;;) {
    c = mSrc.next_char();
    if (c == -1) break;
    if (c == '\n') {
      std::string_view check = line_buf;
      if (spec.strip_tabs)
        while (not check.empty() and check.front() == '\t')
          check.remove_prefix(1);
      if (check == spec.delim) break;
      body += (spec.strip_tabs ? std::string(check) : line_buf) + '\n';
      line_buf.clear();
    } else {
      line_buf += static_cast<char>(c);
    }
  }
  return body;
}

LexToken Lexer::read_token() {
  for (;;) {
    int c = mSrc.next_char();
    if (c == -1) return {TokType::TK_EOF, {}, mSrc.line()};
    if (c == '#' and (mFlags & LEX_SKIPCOMMENTS)) {
      while ((c = mSrc.next_char()) != -1 and c != '\n') {}
      if (c == '\n') return {TokType::TK_NEWLINE, "\n", mSrc.line()};
      return {TokType::TK_EOF, {}, mSrc.line()};
    }
    if (c == '\n') return {TokType::TK_NEWLINE, "\n", mSrc.line()};
    if (c == ' ' or c == '\t') continue;
    switch (c) {
      case ';': {
        int n = mSrc.next_char();
        if (n == ';') return {TokType::TK_DSEMI,   ";;", mSrc.line()};
        if (n == '&') return {TokType::TK_SEMIAMP, ";&", mSrc.line()};
        if (n == '|') return {TokType::TK_SEMIBAR, ";|", mSrc.line()};
        mSrc.unget(n);
        return {TokType::TK_SEMI, ";", mSrc.line()};
      }
      case '&': {
        int n = mSrc.next_char();
        if (n == '&') return {TokType::TK_AMPAMP,   "&&", mSrc.line()};
        if (n == '(') return {TokType::TK_AMPPAREN, "&(", mSrc.line()};
        if (n == '>') { mSrc.unget(n); return read_redir('&'); }
        mSrc.unget(n);
        return {TokType::TK_AMP, "&", mSrc.line()};
      }
      case '|': {
        int n = mSrc.next_char();
        if (n == '|') return {TokType::TK_PIPEPIPE, "||", mSrc.line()};
        if (n == '&') return {TokType::TK_PIPEAMP,  "|&", mSrc.line()};
        mSrc.unget(n);
        return {TokType::TK_PIPE, "|", mSrc.line()};
      }
      case '!': return {TokType::TK_BANG,   "!",  mSrc.line()};
      case '{': return {TokType::TK_LBRACE, "{",  mSrc.line()};
      case '}': return {TokType::TK_RBRACE, "}",  mSrc.line()};
      case '(': {
        int n = mSrc.next_char();
        if (n == '(') return {TokType::TK_DLPAREN, "((", mSrc.line()};
        mSrc.unget(n);
        return {TokType::TK_LPAREN, "(", mSrc.line()};
      }
      case ')': {
        int n = mSrc.next_char();
        if (n == ')') return {TokType::TK_DRPAREN, "))", mSrc.line()};
        mSrc.unget(n);
        return {TokType::TK_RPAREN, ")", mSrc.line()};
      }
      case '<':
      case '>': return read_redir(c);
      default:  return read_word(c);
    }
  }
}

LexToken Lexer::read_word(int first_char) {
  std::string text;
  int  c         = first_char;
  int  ln        = mSrc.line();
  bool in_single = false;
  bool in_double = false;
  bool all_digits_so_far =
      std::isdigit(static_cast<unsigned char>(first_char)) != 0;
  for (;;) {
    if (c == -1) break;
    if (not in_single and not in_double) {
      // Fd prefix: `2>`, `12<foo` etc.  When the word so far is all ASCII
      // digits and the next character is a redirection operator, hand the
      // whole lot over to read_redir and bake the fd into the token text.
      if (all_digits_so_far and not text.empty()
          and (c == '<' or c == '>')) {
        LexToken redir = read_redir(c);
        redir.text = text + redir.text;
        return redir;
      }
      if (c == ' ' or c == '\t' or c == '\n'
          or c == ';' or c == '&' or c == '|'
          or c == '(' or c == ')' or c == '{' or c == '}'
          or c == '<' or c == '>') {
        mSrc.unget(c);
        break;
      }
      // `=(` at the start of a word is zsh's =(cmd) substitution.
      if (text == "=" and c == '(') {
        std::string body = "=(";
        slurp_balanced_paren(mSrc, body);
        return {TokType::TK_REDIR, body, ln};
      }
    }
    if (not std::isdigit(static_cast<unsigned char>(c))) all_digits_so_far = false;
    if (c == '\'' and not in_double) in_single = not in_single;
    if (c == '"'  and not in_single) in_double = not in_double;
    if (c == '\\' and not in_single) {
      text += static_cast<char>(c);
      c = mSrc.next_char();
      if (c != -1) text += static_cast<char>(c);
      c = mSrc.next_char();
      continue;
    }
    text += static_cast<char>(c);
    c = mSrc.next_char();
  }
  if (not text.empty() and is_keyword(text)) {
    for (auto* k = KEYWORDS; k->word; ++k)
      if (text == k->word) return {k->type, text, ln};
  }
  return {TokType::TK_WORD, text, ln};
}

LexToken Lexer::read_redir(int first_char) {
  std::string text(1, static_cast<char>(first_char));
  int c = mSrc.next_char();
  if (c != -1) {
    text += static_cast<char>(c);
    if (text == "<<") {
      int n = mSrc.next_char();
      if      (n == '-') text += '-';
      else if (n == '<') text += '<';
      else               mSrc.unget(n);
    } else if (text == "&>") {
      int n = mSrc.next_char();
      if (n == '>') text += '>';
      else          mSrc.unget(n);
    } else if (text == "<(" or text == ">(") {
      slurp_balanced_paren(mSrc, text);
    } else if (text == ">>" or text == ">|" or text == ">&"
               or text == "<>" or text == "<&") {
      // ok
    } else if (c != '>' and c != '<' and c != '|'
               and c != '&' and c != '-') {
      mSrc.unget(c);
      text = text.substr(0, 1);
    }
  }
  return {TokType::TK_REDIR, text, mSrc.line()};
}

}  // namespace jfsh
