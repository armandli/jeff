#include <jfsh/parser/parser.hh>
#include <memory>

namespace jfsh {

Parser::Parser(Lexer& lex, ParseFlags flags)
    : lex_(lex), flags_(flags) {}

Result<ProgRef> Parser::parse() {
    auto prog = std::make_shared<CompiledProgram>();
    prog_ = prog.get();
    if (!par_list()) {
        if (!error_.message.empty())
            return std::unexpected(error_);
    }
    prog_ = nullptr;
    return prog;
}

Result<ProgRef> Parser::parse_string(std::string_view /*src*/) {
    // TODO: create a StringInputSource, Lexer, and delegate to parse()
    return std::unexpected(ShellError{ErrorKind::ParseError, "parse_string not yet implemented"});
}

// ---------------------------------------------------------------------------
// Emit helpers
// ---------------------------------------------------------------------------

std::size_t Parser::emit(WordCode w) {
    std::size_t pos = prog_->prog.size();
    prog_->prog.push_back(w);
    prog_->line_map.push_back(static_cast<std::uint32_t>(lex_.line()));
    return pos;
}

std::size_t Parser::emit_str(std::string s) {
    std::size_t idx = prog_->strs.size();
    prog_->strs.push_back(std::move(s));
    emit(make_wc(WC_STRING, static_cast<WordCode>(idx)));
    return idx;
}

void Parser::patch(std::size_t addr, WordCode w) {
    prog_->prog[addr] = w;
}

// ---------------------------------------------------------------------------
// Token helpers
// ---------------------------------------------------------------------------

bool Parser::eat(TokType t) {
    if (at(t)) { lex_.consume(); return true; }
    return false;
}

bool Parser::at(TokType t) const noexcept {
    return lex_.peek().type == t;
}

void Parser::skip_newlines() {
    while (at(TokType::TK_NEWLINE)) lex_.consume();
}

void Parser::error(std::string msg) {
    error_ = ShellError{ErrorKind::ParseError, std::move(msg), lex_.line()};
}

// ---------------------------------------------------------------------------
// Grammar stubs — full recursive-descent parsing TBD
// ---------------------------------------------------------------------------

bool Parser::par_list() {
    skip_newlines();
    if (at(TokType::TK_EOF)) return true;
    return par_list1();
}

bool Parser::par_list1() {
    if (!par_sublist()) return false;
    while (at(TokType::TK_SEMI) || at(TokType::TK_AMP) || at(TokType::TK_NEWLINE)) {
        bool async = at(TokType::TK_AMP);
        lex_.consume();
        skip_newlines();
        if (async) emit(make_wc(WC_LIST, WC_LIST_ASYNC));
        if (at(TokType::TK_EOF) || at(TokType::TK_RBRACE) || at(TokType::TK_FI) ||
            at(TokType::TK_DONE) || at(TokType::TK_ESAC))
            break;
        if (!par_sublist()) return false;
    }
    emit(make_wc(WC_LIST, WC_LIST_END));
    return true;
}

bool Parser::par_sublist() {
    if (!par_pline()) return false;
    while (at(TokType::TK_AMPAMP) || at(TokType::TK_PIPEPIPE)) {
        WordCode flag = at(TokType::TK_AMPAMP) ? WC_SUBLIST_AND : WC_SUBLIST_OR;
        lex_.consume(); skip_newlines();
        emit(make_wc(WC_SUBLIST, flag));
        if (!par_pline()) return false;
    }
    return true;
}

bool Parser::par_pline() {
    bool bang = at(TokType::TK_BANG);
    if (bang) lex_.consume();
    if (!par_cmd()) return false;
    while (at(TokType::TK_PIPE) || at(TokType::TK_PIPEAMP)) {
        lex_.consume(); skip_newlines();
        emit(make_wc(WC_PIPE, 0));
        if (!par_cmd()) return false;
    }
    if (bang) emit(make_wc(WC_SUBLIST, WC_SUBLIST_NOT));
    return true;
}

bool Parser::par_cmd() {
    skip_newlines();
    const auto& tok = lex_.peek();
    switch (tok.type) {
        case TokType::TK_IF:       return par_if();
        case TokType::TK_WHILE:
        case TokType::TK_UNTIL:    return par_while();
        case TokType::TK_FOR:      return par_for();
        case TokType::TK_CASE:     return par_case();
        case TokType::TK_SELECT:   return par_select();
        case TokType::TK_REPEAT:   return par_repeat();
        case TokType::TK_LPAREN:   return par_subsh();
        case TokType::TK_LBRACE:   return par_funcdef();
        case TokType::TK_FUNCTION: return par_funcdef();
        case TokType::TK_DLPAREN:  return par_arith();
        default:                   return par_simple();
    }
}

bool Parser::par_simple() {
    std::vector<std::size_t> arg_indices;
    int arg_count = 0;
    // Collect words / redirs / assignments
    while (!at(TokType::TK_EOF) && !at(TokType::TK_NEWLINE) &&
           !at(TokType::TK_SEMI) && !at(TokType::TK_AMP) &&
           !at(TokType::TK_PIPE) && !at(TokType::TK_PIPEPIPE) &&
           !at(TokType::TK_AMPAMP) && !at(TokType::TK_RPAREN) &&
           !at(TokType::TK_RBRACE)) {
        if (at(TokType::TK_REDIR)) { par_redir(); continue; }
        if (at(TokType::TK_WORD)) {
            const auto& w = lex_.peek();
            emit_str(w.text);
            lex_.consume();
            ++arg_count;
        } else {
            break;
        }
    }
    if (arg_count == 0 && prog_->prog.empty()) return true;
    emit(make_wc(WC_SIMPLE, static_cast<WordCode>(arg_count)));
    return true;
}

// Stub compound command parsers
bool Parser::par_if()      { lex_.consume(); return true; }
bool Parser::par_while()   { lex_.consume(); return true; }
bool Parser::par_for()     { lex_.consume(); return true; }
bool Parser::par_case()    { lex_.consume(); return true; }
bool Parser::par_select()  { lex_.consume(); return true; }
bool Parser::par_repeat()  { lex_.consume(); return true; }
bool Parser::par_subsh()   { lex_.consume(); return true; }
bool Parser::par_funcdef() { lex_.consume(); return true; }
bool Parser::par_cond()    { lex_.consume(); return true; }
bool Parser::par_arith()   { lex_.consume(); return true; }
// par_always is not in the grammar stubs — always/try handled via par_cmd dispatch
bool Parser::par_redir()   { lex_.consume(); return true; }
bool Parser::par_assign()  { lex_.consume(); return true; }

} // namespace jfsh
