#include <cctype>
#include <cstring>

#include <utility>

#include <jfsh/lexer/lexer.h>

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

// Characters that end an unquoted word.
static bool is_word_break(int c) noexcept {
  return c == ' ' or c == '\t' or c == '\n' or c == ';' or c == '&'
      or c == '|' or c == '(' or c == ')' or c == '<' or c == '>';
}

static void slurp_nested(InputSource& src, std::string& text, char open,
                         char close);

// Appends a backquoted command substitution up to and including its closing
// backquote.  A backslash keeps the next character.
static void slurp_backquote(InputSource& src, std::string& text) {
  for (int c; (c = src.next_char()) != -1;) {
    text += static_cast<char>(c);
    if (c == '`') return;
    if (c == '\\') {
      int n = src.next_char();
      if (n == -1) return;
      text += static_cast<char>(n);
    }
  }
}

// Appends a double-quoted string up to and including its closing quote.
// Substitutions inside it are slurped whole so a `"` in `$(…)` doesn't end it.
static void slurp_dquote(InputSource& src, std::string& text) {
  for (int c; (c = src.next_char()) != -1;) {
    text += static_cast<char>(c);
    if (c == '"') return;
    if (c == '\\') {
      int n = src.next_char();
      if (n == -1) return;
      text += static_cast<char>(n);
    } else if (c == '`') {
      slurp_backquote(src, text);
    } else if (c == '$') {
      int n = src.next_char();
      if (n == '(' or n == '{') {
        text += static_cast<char>(n);
        slurp_nested(src, text, static_cast<char>(n), n == '(' ? ')' : '}');
      } else {
        src.unget(n);
      }
    }
  }
}

// Appends text up to and including the `close` that balances an `open` the
// caller has already consumed.  Quotes, backslashes, backquotes and nested
// `$(…)`/`${…}` are tracked so their contents can't close it early.
static void slurp_nested(InputSource& src, std::string& text, char open,
                         char close) {
  int depth = 1;
  for (int c; (c = src.next_char()) != -1;) {
    text += static_cast<char>(c);
    if (c == '\\') {
      int n = src.next_char();
      if (n == -1) return;
      text += static_cast<char>(n);
    } else if (c == '\'') {
      for (int q; (q = src.next_char()) != -1;) {
        text += static_cast<char>(q);
        if (q == '\'') break;
      }
    } else if (c == '"') {
      slurp_dquote(src, text);
    } else if (c == '`') {
      slurp_backquote(src, text);
    } else if (c == '$') {
      int n = src.next_char();
      if ((n == '(' or n == '{') and n != open) {
        text += static_cast<char>(n);
        slurp_nested(src, text, static_cast<char>(n), n == '(' ? ')' : '}');
      } else {
        src.unget(n);
      }
    } else if (c == open) {
      ++depth;
    } else if (c == close and --depth == 0) {
      return;
    }
  }
}

// After a `<` inside a word: reads the rest of a zsh numeric glob `<lo-hi>`
// (either bound optional) into `text`.  On anything else nothing is
// consumed and false is returned.
static bool read_numeric_range(InputSource& src, std::string& text) {
  std::string seen;
  bool dash = false;
  for (;;) {
    int c = src.next_char();
    if (c != -1) seen += static_cast<char>(c);
    if (c != -1 and std::isdigit(c)) continue;
    if (c == '-' and not dash) { dash = true; continue; }
    if (c == '>' and dash) {
      text += '<';
      text += seen;
      return true;
    }
    if (c == -1) src.unget(-1);
    for (auto it = seen.rbegin(); it != seen.rend(); ++it)
      src.unget(static_cast<unsigned char>(*it));
    return false;
  }
}

// `NAME=` or `NAME+=`: the start of an assignment, where a following `(`
// opens an array value instead of ending the word.
static bool is_assignment_prefix(std::string_view w) noexcept {
  if (w.empty() or w.back() != '=') return false;
  w.remove_suffix(1);
  if (not w.empty() and w.back() == '+') w.remove_suffix(1);
  if (w.empty() or std::isdigit(static_cast<unsigned char>(w[0]))) return false;
  for (char c : w)
    if (not std::isalnum(static_cast<unsigned char>(c)) and c != '_')
      return false;
  return true;
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

std::vector<std::string> Lexer::take_heredocs() {
  // Input that ends before the bodies (`cat <<EOF` alone) gets empty ones.
  if (not mHeredocQueue.empty()) mIncomplete = true;
  mHeredocBodies.resize(mHeredocBodies.size() + mHeredocQueue.size());
  mHeredocQueue.clear();
  return std::exchange(mHeredocBodies, {});
}

// Reads the body of every pending here-document, starting on the line after
// the one that registered them.
void Lexer::read_heredoc_bodies() {
  for (auto& spec : mHeredocQueue) {
    std::string body;
    std::string line_buf;
    for (;;) {
      int c = mSrc.next_char();
      if (c == -1) {
        if (line_buf != spec.delim) mIncomplete = true;
        if (not line_buf.empty() and line_buf != spec.delim) body += line_buf;
        break;
      }
      if (c != '\n') {
        line_buf += static_cast<char>(c);
        continue;
      }
      std::string_view check = line_buf;
      if (spec.strip_tabs)
        while (not check.empty() and check.front() == '\t') check.remove_prefix(1);
      if (check == spec.delim) break;
      body += std::string(check) + '\n';
      line_buf.clear();
    }
    mHeredocBodies.push_back(std::move(body));
  }
  mHeredocQueue.clear();
}

LexToken Lexer::newline_token() {
  int ln = mSrc.line();
  if (not mHeredocQueue.empty()) read_heredoc_bodies();
  return {TokType::TK_NEWLINE, "\n", ln};
}

LexToken Lexer::read_token() {
  for (;;) {
    int c = mSrc.next_char();
    if (c == -1) return {TokType::TK_EOF, {}, mSrc.line()};
    if (c == '#' and (mFlags & LEX_SKIPCOMMENTS)) {
      while ((c = mSrc.next_char()) != -1 and c != '\n') {}
      if (c == '\n') return newline_token();
      return {TokType::TK_EOF, {}, mSrc.line()};
    }
    if (c == '\n') return newline_token();
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
      // `!` is a token only on its own; `!=` or `!x` is a word.
      case '!': {
        int n = mSrc.next_char();
        mSrc.unget(n);
        if (n == -1 or is_word_break(n)) return {TokType::TK_BANG, "!", mSrc.line()};
        return read_word(c);
      }
      // `{` and `}` are reserved only as words of their own; otherwise they
      // start a word (`{a,b}`, `{1..3}`, `{}`) for brace expansion.
      case '{':
      case '}': {
        int n = mSrc.next_char();
        mSrc.unget(n);
        if (n == -1 or is_word_break(n))
          return c == '{' ? LexToken{TokType::TK_LBRACE, "{", mSrc.line()}
                          : LexToken{TokType::TK_RBRACE, "}", mSrc.line()};
        return read_word(c);
      }
      case '(': {
        int n = mSrc.next_char();
        if (n == '(' and not (mFlags & LEX_COND)) return {TokType::TK_DLPAREN, "((", mSrc.line()};
        mSrc.unget(n);
        return {TokType::TK_LPAREN, "(", mSrc.line()};
      }
      // `))` is two tokens, so `(a; (b))` closes both subshells; the end of
      // an arithmetic `(( … ))` is found by read_arith instead.
      case ')': return {TokType::TK_RPAREN, ")", mSrc.line()};
      case '<':
      case '>': {
        // A lone `<` or `>` inside [[ ]] compares strings.
        int n = mSrc.next_char();
        mSrc.unget(n);
        if ((mFlags & LEX_COND) and (n == -1 or n == ' ' or n == '\t' or n == '\n'))
          return {TokType::TK_WORD, std::string(1, static_cast<char>(c)), mSrc.line()};
        return read_redir(c);
      }
      default:  return read_word(c);
    }
  }
}

std::optional<std::string> Lexer::read_arith() {
  std::string text;
  int depth = 0;
  for (int c; (c = mSrc.next_char()) != -1;) {
    if (c == '(') {
      ++depth;
    } else if (c == ')') {
      if (depth > 0) {
        --depth;
      } else {
        int n = mSrc.next_char();
        if (n == ')') return text;
        if (n == -1) return std::nullopt;
        text += ')';
        c = n;
      }
    } else if (c == '\n') {
      if (not mHeredocQueue.empty()) read_heredoc_bodies();
    }
    text += static_cast<char>(c);
  }
  return std::nullopt;
}

std::optional<std::string> Lexer::read_cond_pattern() {
  int c = mSrc.next_char();
  while (c == ' ' or c == '\t') c = mSrc.next_char();
  if (c == -1) return std::nullopt;
  std::string text;
  int depth = 0;
  for (; c != -1; c = mSrc.next_char()) {
    if (depth == 0 and (c == ' ' or c == '\t' or c == '\n' or c == ')' or c == '|'
                        or c == ';' or c == '&')) {
      mSrc.unget(c);
      break;
    }
    text += static_cast<char>(c);
    if (c == '(') {
      ++depth;
    } else if (c == ')') {
      --depth;
    } else if (c == '\\') {
      int n = mSrc.next_char();
      if (n == -1) { mIncomplete = true; break; }
      text += static_cast<char>(n);
    } else if (c == '\'') {
      int q;
      while ((q = mSrc.next_char()) != -1 and q != '\'') text += static_cast<char>(q);
      if (q == -1) { mIncomplete = true; break; }
      text += '\'';
    } else if (c == '"') {
      slurp_dquote(mSrc, text);
    } else if (c == '`') {
      slurp_backquote(mSrc, text);
    } else if (c == '$') {
      int n = mSrc.next_char();
      if (n == '(' or n == '{') {
        text += static_cast<char>(n);
        slurp_nested(mSrc, text, static_cast<char>(n), n == '(' ? ')' : '}');
      } else {
        mSrc.unget(n);
      }
    }
  }
  if (text.empty()) return std::nullopt;
  return text;
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
      if (all_digits_so_far and not text.empty() and not (mFlags & LEX_COND)
          and (c == '<' or c == '>')) {
        LexToken redir = read_redir(c);
        redir.text = text + redir.text;
        return redir;
      }
      // `=(` at the start of a word is zsh's =(cmd) substitution.
      if (text == "=" and c == '(') {
        std::string body = "=(";
        slurp_balanced_paren(mSrc, body);
        return {TokType::TK_REDIR, body, ln};
      }
      // `name=(…)` is an array assignment, and inside a word `(…)` is a
      // glob group or qualifier (`*.(c|h)`, `*(.)`).  `name()` is left
      // alone: it is a function definition.
      if (c == '(' and not text.empty()) {
        int n = mSrc.next_char();
        mSrc.unget(n);
        if (n != ')' or is_assignment_prefix(text)) {
          text += '(';
          slurp_nested(mSrc, text, '(', ')');
          all_digits_so_far = false;
          c = mSrc.next_char();
          continue;
        }
      }
      if (c == '<' and not text.empty() and read_numeric_range(mSrc, text)) {
        all_digits_so_far = false;
        c = mSrc.next_char();
        continue;
      }
      if (is_word_break(c)) {
        mSrc.unget(c);
        break;
      }
    }
    if (not in_single) {
      // Substitutions are one unit of the word, whatever they contain.
      if (c == '$') {
        int n = mSrc.next_char();
        if (n == '(' or n == '{') {
          text += '$';
          text += static_cast<char>(n);
          slurp_nested(mSrc, text, static_cast<char>(n), n == '(' ? ')' : '}');
          all_digits_so_far = false;
          c = mSrc.next_char();
          continue;
        }
        if (n == '\'' and not in_double) {
          // $'…' — C-style escapes; `\'` does not end it.
          text += "$'";
          for (int q; (q = mSrc.next_char()) != -1;) {
            text += static_cast<char>(q);
            if (q == '\'') break;
            if (q == '\\') {
              int e = mSrc.next_char();
              if (e == -1) break;
              text += static_cast<char>(e);
            }
          }
          all_digits_so_far = false;
          c = mSrc.next_char();
          continue;
        }
        mSrc.unget(n);
      } else if (c == '`') {
        text += '`';
        slurp_backquote(mSrc, text);
        all_digits_so_far = false;
        c = mSrc.next_char();
        continue;
      }
    }
    if (not std::isdigit(static_cast<unsigned char>(c))) all_digits_so_far = false;
    if (c == '\'' and not in_double) in_single = not in_single;
    if (c == '"'  and not in_single) in_double = not in_double;
    if (c == '\\' and not in_single) {
      text += static_cast<char>(c);
      c = mSrc.next_char();
      if (c == -1) mIncomplete = true;
      else         text += static_cast<char>(c);
      c = mSrc.next_char();
      continue;
    }
    text += static_cast<char>(c);
    c = mSrc.next_char();
  }
  if (in_single or in_double) mIncomplete = true;
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
