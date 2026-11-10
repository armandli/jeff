#include <jfsh/lexer/lexer.hh>
#include <cctype>
#include <cstring>

namespace jfsh {

// ---------------------------------------------------------------------------
// Keyword table
// ---------------------------------------------------------------------------
static const struct { const char* word; TokType type; } kKeywords[] = {
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

// ---------------------------------------------------------------------------
// Lexer
// ---------------------------------------------------------------------------

Lexer::Lexer(InputSource& src) : src_(src) {}
Lexer::~Lexer() = default;

bool Lexer::is_keyword(std::string_view s) const noexcept {
    for (auto* k = kKeywords; k->word; ++k)
        if (s == k->word) return true;
    return false;
}

LexToken Lexer::next_token() {
    if (peeked_) {
        auto t = std::move(*peeked_);
        peeked_.reset();
        return t;
    }
    return read_token();
}

const LexToken& Lexer::peek() {
    if (!peeked_) peeked_ = read_token();
    return *peeked_;
}

void Lexer::consume() {
    if (peeked_) peeked_.reset();
    else         read_token();
}

int Lexer::line() const noexcept { return src_.line(); }

void Lexer::push_state(LexFlags flags) {
    state_stack_.push_back(flags_);
    flags_ = flags;
}

void Lexer::pop_state() {
    if (!state_stack_.empty()) {
        flags_ = state_stack_.back();
        state_stack_.pop_back();
    }
}

void Lexer::register_heredoc(std::string delim, bool strip_tabs, bool expand) {
    heredoc_queue_.push_back({std::move(delim), strip_tabs, expand});
}

std::string Lexer::read_heredoc() {
    if (heredoc_queue_.empty()) return {};
    auto spec = std::move(heredoc_queue_.front());
    heredoc_queue_.erase(heredoc_queue_.begin());
    std::string body;
    std::string line_buf;
    for (;;) {
        int c = src_.next_char();
        if (c == -1) break;
        if (c == '\n') {
            if (line_buf == spec.delim) break;
            if (spec.strip_tabs && !line_buf.empty() && line_buf[0] == '\t')
                line_buf = line_buf.substr(1);
            body += line_buf + '\n';
            line_buf.clear();
        } else {
            line_buf += static_cast<char>(c);
        }
    }
    return body;
}

LexToken Lexer::read_token() {
    // Skip whitespace (not newlines)
    for (;;) {
        int c = src_.next_char();
        if (c == -1) return {TokType::TK_EOF, {}, src_.line()};
        if (c == '#' && (flags_ & LEX_SKIPCOMMENTS)) {
            // Skip to end of line
            while ((c = src_.next_char()) != -1 && c != '\n') {}
            if (c == '\n') return {TokType::TK_NEWLINE, "\n", src_.line()};
            return {TokType::TK_EOF, {}, src_.line()};
        }
        if (c == '\n') return {TokType::TK_NEWLINE, "\n", src_.line()};
        if (c == ' ' || c == '\t') continue;
        // Metacharacters
        switch (c) {
            case ';': {
                int n = src_.next_char();
                if (n == ';') return {TokType::TK_DSEMI, ";;", src_.line()};
                if (n == '&') return {TokType::TK_SEMIAMP, ";&", src_.line()};
                if (n == '|') return {TokType::TK_SEMIBAR, ";|", src_.line()};
                src_.unget(n);
                return {TokType::TK_SEMI, ";", src_.line()};
            }
            case '&': {
                int n = src_.next_char();
                if (n == '&') return {TokType::TK_AMPAMP, "&&", src_.line()};
                if (n == '>') return read_redir('&');
                if (n == '(') return {TokType::TK_AMPPAREN, "&(", src_.line()};
                src_.unget(n);
                return {TokType::TK_AMP, "&", src_.line()};
            }
            case '|': {
                int n = src_.next_char();
                if (n == '|') return {TokType::TK_PIPEPIPE, "||", src_.line()};
                if (n == '&') return {TokType::TK_PIPEAMP, "|&", src_.line()};
                src_.unget(n);
                return {TokType::TK_PIPE, "|", src_.line()};
            }
            case '!': return {TokType::TK_BANG, "!", src_.line()};
            case '(': {
                int n = src_.next_char();
                if (n == '(') return {TokType::TK_DLPAREN, "((", src_.line()};
                src_.unget(n);
                return {TokType::TK_LPAREN, "(", src_.line()};
            }
            case ')': {
                int n = src_.next_char();
                if (n == ')') return {TokType::TK_DRPAREN, "))", src_.line()};
                src_.unget(n);
                return {TokType::TK_RPAREN, ")", src_.line()};
            }
            case '{': return {TokType::TK_LBRACE, "{", src_.line()};
            case '}': return {TokType::TK_RBRACE, "}", src_.line()};
            case '<': case '>': return read_redir(c);
            default:  return read_word(c);
        }
    }
}

LexToken Lexer::read_word(int first_char) {
    std::string text;
    int c = first_char;
    int ln = src_.line();
    bool in_single = false, in_double = false;
    for (;;) {
        if (c == -1) break;
        if (!in_single && !in_double) {
            // Check for end-of-word
            if (c == ' ' || c == '\t' || c == '\n' ||
                c == ';' || c == '&' || c == '|' ||
                c == '(' || c == ')' || c == '{' || c == '}' ||
                c == '<' || c == '>') {
                src_.unget(c);
                break;
            }
        }
        if (c == '\'' && !in_double) in_single = !in_single;
        if (c == '"'  && !in_single) in_double = !in_double;
        if (c == '\\' && !in_single) {
            text += static_cast<char>(c);
            c = src_.next_char();
            if (c != -1) text += static_cast<char>(c);
            c = src_.next_char();
            continue;
        }
        text += static_cast<char>(c);
        c = src_.next_char();
    }
    // Check keyword
    if (!text.empty() && is_keyword(text)) {
        for (auto* k = kKeywords; k->word; ++k)
            if (text == k->word) return {k->type, text, ln};
    }
    return {TokType::TK_WORD, text, ln};
}

LexToken Lexer::read_redir(int first_char) {
    std::string text(1, static_cast<char>(first_char));
    int c = src_.next_char();
    if (c != -1) {
        text += static_cast<char>(c);
        // Handle >> << <<- <<< <> >| >& &>
        if (text == "<<") {
            int n = src_.next_char();
            if (n == '-') text += '-';
            else if (n == '<') text += '<';
            else src_.unget(n);
        } else if (text == ">>") {
            // ok
        } else if (text == ">|" || text == ">&" || text == "<>" ||
                   text == "&>" || text == "<(") {
            // ok
        } else if (c != '>' && c != '<' && c != '|' && c != '&' && c != '-') {
            src_.unget(c);
            text = text.substr(0, 1);
        }
    }
    return {TokType::TK_REDIR, text, src_.line()};
}

} // namespace jfsh
