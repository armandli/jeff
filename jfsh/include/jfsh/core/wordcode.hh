#ifndef JFSH_CORE_WORDCODE_HH
#define JFSH_CORE_WORDCODE_HH
// jfsh/core/wordcode.hh — compiled shell program representation.
//
// Mirrors zsh's Eprog / WordCode architecture from Src/parse.c and zsh.h.
//
// The parser emits a linear array of WordCode instructions rather than a tree
// AST.  Each 32-bit word encodes both an opcode (low 5 bits) and an immediate
// operand (high 27 bits).  String literals live in a parallel string table.

#include <cstdint>

#include <memory>
#include <string>
#include <vector>

#include <jfsh/core/types.hh>

namespace jfsh {

using WordCode = std::uint32_t;

inline constexpr int      kWCBits = 5;
inline constexpr int      kWCData = 32 - kWCBits;
inline constexpr WordCode kWCMask = (1u << kWCBits) - 1u;

constexpr WordCode wc_code(WordCode w) noexcept { return w & kWCMask; }
constexpr WordCode wc_data(WordCode w) noexcept { return w >> kWCBits; }
constexpr WordCode make_wc(WordCode op, WordCode d) noexcept {
  return (d << kWCBits) | (op & kWCMask);
}

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
  WC_MAX,
};

// ---------------------------------------------------------------------------
// Bytecode layout.  Every construct is PREFIX: its opcode comes first, and
// operands that a reader may need to skip carry an explicit length, so the
// executor can jump over an un-taken branch without decoding it.
//
//   program  := list* WC_END
//   list     := WC_LIST(flags, skip)     sublist
//                 flags: WC_LIST_ASYNC (`&`), WC_LIST_END (last list element)
//                 skip:  words in the sublist chain that follows
//   sublist  := WC_SUBLIST(flags, skip)  pipeline  [sublist]
//                 flags: connector to the NEXT element (WC_SUBLIST_END, _AND,
//                        _OR) | WC_SUBLIST_NOT (negate this pipeline)
//                 skip:  words in this element's pipeline
//                 The chain continues while the connector is AND or OR.
//   pipeline := WC_PIPE(n)  command{n}
//   command  := simple | compound (M4: WC_IF, WC_WHILE, …)
//   simple   := assign*  WC_SIMPLE(argc)  WC_STRING(word){argc}
//                 argc may be 0 for an assignment-only command
//   assign   := WC_ASSIGN(flags)  WC_STRING(name)  WC_STRING(value)
//                 flags: WC_ASSIGN_APPEND (`name+=value`)
//
// WC_STRING's operand indexes CompiledProgram::strs.  Words are stored as
// written (quotes included); expansion happens at execution time.
// ---------------------------------------------------------------------------

inline constexpr WordCode WC_LIST_ASYNC = 1u;  // & (background)
inline constexpr WordCode WC_LIST_END   = 2u;  // last in list
inline constexpr int      kWCListFlagBits = 2;

inline constexpr WordCode WC_SUBLIST_END  = 0u;
inline constexpr WordCode WC_SUBLIST_AND  = 1u;    // &&
inline constexpr WordCode WC_SUBLIST_OR   = 2u;    // ||
inline constexpr WordCode WC_SUBLIST_NOT  = 4u;    // !
inline constexpr WordCode WC_SUBLIST_TYPE = 3u;    // mask for END/AND/OR
inline constexpr int      kWCSublistFlagBits = 3;

inline constexpr WordCode WC_ASSIGN_APPEND = 1u;   // name+=value

constexpr WordCode make_wc_list(WordCode flags, WordCode skip) noexcept {
  return make_wc(WC_LIST, (skip << kWCListFlagBits) | flags);
}
constexpr WordCode wc_list_flags(WordCode w) noexcept {
  return wc_data(w) & ((1u << kWCListFlagBits) - 1u);
}
constexpr WordCode wc_list_skip(WordCode w) noexcept {
  return wc_data(w) >> kWCListFlagBits;
}

constexpr WordCode make_wc_sublist(WordCode flags, WordCode skip) noexcept {
  return make_wc(WC_SUBLIST, (skip << kWCSublistFlagBits) | flags);
}
constexpr WordCode wc_sublist_flags(WordCode w) noexcept {
  return wc_data(w) & ((1u << kWCSublistFlagBits) - 1u);
}
constexpr WordCode wc_sublist_skip(WordCode w) noexcept {
  return wc_data(w) >> kWCSublistFlagBits;
}

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

struct CompiledProgram {
  std::vector<WordCode>    prog;      // bytecode stream
  std::vector<std::string> strs;      // string literal table
  int    flags      = 0;
  int    shf_flags  = 0;              // set when this is a shell function body
  int    ref_count  = 0;
  std::vector<std::uint32_t> line_map; // prog index → source line number

  bool empty() const noexcept { return prog.empty(); }
};

using ProgRef = std::shared_ptr<CompiledProgram>;

struct ExecState {
  const CompiledProgram* prog = nullptr;
  std::size_t            pc   = 0;    // program counter into prog->prog

  WordCode peek()  const noexcept { return prog->prog[pc]; }
  WordCode fetch()       noexcept { return prog->prog[pc++]; }
  bool     at_end() const noexcept { return pc >= prog->prog.size(); }

  std::string_view str(std::size_t offset) const noexcept {
    return prog->strs[offset];
  }

  // Fetch a WC_STRING operand and return the string it refers to.
  std::string_view fetch_str() noexcept { return str(wc_data(fetch())); }
};

}  // namespace jfsh

#endif  // JFSH_CORE_WORDCODE_HH
