#pragma once
// jfsh/core/wordcode.hh — compiled shell program representation.
//
// Mirrors zsh's Eprog / WordCode architecture from Src/parse.c and zsh.h.
//
// The parser emits a linear array of WordCode instructions rather than a tree
// AST.  This is more cache-friendly and cheaper to reference-count.  Each
// 32-bit word encodes both an opcode (low 5 bits) and an immediate operand
// (high 27 bits).  String literals live in a parallel string table; compiled
// patterns in a pattern table.

#include <jfsh/core/types.hh>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace jfsh {

// ---------------------------------------------------------------------------
// WordCode — a single instruction word (matches zsh's wordcode typedef)
// ---------------------------------------------------------------------------
using WordCode = std::uint32_t;

inline constexpr int   kWCBits   = 5;              // bits for opcode
inline constexpr int   kWCData   = 32 - kWCBits;   // bits for inline data
inline constexpr WordCode kWCMask = (1u << kWCBits) - 1u;

constexpr WordCode wc_code(WordCode w)        noexcept { return w & kWCMask; }
constexpr WordCode wc_data(WordCode w)        noexcept { return w >> kWCBits; }
constexpr WordCode make_wc(WordCode op, WordCode d) noexcept {
    return (d << kWCBits) | (op & kWCMask);
}

// ---------------------------------------------------------------------------
// Opcodes — mirrors zsh WC_* constants
// ---------------------------------------------------------------------------
enum WCOpcode : WordCode {
    WC_END       = 0,   // end of program / sub-program
    WC_LIST      = 1,   // list (;  &  newline separated)
    WC_SUBLIST   = 2,   // && || logic
    WC_PIPE      = 3,   // | pipeline segment
    WC_REDIR     = 4,   // redirection
    WC_SIMPLE    = 5,   // simple command (arg count in data)
    WC_TYPESET   = 6,   // typeset / declare
    WC_ASSIGN    = 7,   // name=value assignment
    WC_IF        = 8,   // if/elif/else
    WC_WHILE     = 9,   // while / until
    WC_FOR       = 10,  // for / foreach
    WC_CASE      = 11,  // case
    WC_SELECT    = 12,  // select
    WC_REPEAT    = 13,  // repeat N { }
    WC_SUBSH     = 14,  // ( subshell )
    WC_FUNCDEF   = 15,  // function definition
    WC_FOR_COND  = 16,  // C-style for ((;;))
    WC_ALWAYS    = 17,  // { } always { } (zsh extension)
    WC_TRY       = 18,  // { } always { } outer wrapper
    WC_COND      = 19,  // [[ ]] conditional expression
    WC_ARITH     = 20,  // (( arithmetic ))
    WC_AUTOFN    = 21,  // anonymous function () { }
    WC_STRING    = 22,  // string literal reference into strs table
    WC_MAX
};

// WC_LIST flags (data field bits)
inline constexpr WordCode WC_LIST_ASYNC = 1u;  // & (background)
inline constexpr WordCode WC_LIST_END   = 2u;  // last in list

// WC_SUBLIST flags
inline constexpr WordCode WC_SUBLIST_END  = 0u;
inline constexpr WordCode WC_SUBLIST_AND  = 1u;    // &&
inline constexpr WordCode WC_SUBLIST_OR   = 2u;    // ||
inline constexpr WordCode WC_SUBLIST_NOT  = 4u;    // !

// WC_REDIR type stored in data field
enum RedirType : WordCode {
    REDIR_READ      = 0,   // <
    REDIR_WRITE     = 1,   // >
    REDIR_APPEND    = 2,   // >>
    REDIR_HEREDOC   = 3,   // <<
    REDIR_HEREDOCD  = 4,   // <<- (strip tabs)
    REDIR_HERESTR   = 5,   // <<<
    REDIR_READWRITE = 6,   // <>
    REDIR_CLOBBER   = 7,   // >|
    REDIR_MERGE     = 8,   // >&
    REDIR_MERGEOUT  = 9,   // &>
    REDIR_PROC_IN   = 10,  // <(cmd)  process substitution
    REDIR_PROC_OUT  = 11,  // >(cmd)
    REDIR_PROC_EQ   = 12,  // =(cmd)  zsh-specific
    REDIR_CLOSE     = 13,  // >&-
};

// ---------------------------------------------------------------------------
// CompiledProgram (Eprog equivalent)
// ---------------------------------------------------------------------------
struct CompiledProgram {
    std::vector<WordCode>  prog;        // bytecode stream
    std::vector<std::string> strs;      // string literal table (indexed by offset)
    // pats: compiled glob/regex patterns — stored separately to allow sharing
    // across function definitions.

    int    flags        = 0;
    int    shf_flags    = 0;    // set when this is a shell function body
    int    ref_count    = 0;    // reference count for sharing

    // Source position table: prog index → line number, for error messages.
    std::vector<std::uint32_t> line_map;

    bool empty() const noexcept { return prog.empty(); }
};

using ProgRef = std::shared_ptr<CompiledProgram>;

// ---------------------------------------------------------------------------
// Execution state (Estate equivalent)
// ---------------------------------------------------------------------------
struct ExecState {
    const CompiledProgram* prog = nullptr;
    std::size_t            pc   = 0;    // program counter into prog->prog

    WordCode peek()       const noexcept { return prog->prog[pc]; }
    WordCode fetch()            noexcept { return prog->prog[pc++]; }
    bool     at_end()     const noexcept { return pc >= prog->prog.size(); }
    std::string_view str(std::size_t offset) const noexcept {
        return prog->strs[offset];
    }
};

} // namespace jfsh
