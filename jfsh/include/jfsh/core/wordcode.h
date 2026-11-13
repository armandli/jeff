#ifndef JFSH_CORE_WORDCODE_H
#define JFSH_CORE_WORDCODE_H
// jfsh/core/wordcode.h — compiled shell program representation.
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

#include <jfsh/core/types.h>

namespace jfsh {

using WordCode = std::uint32_t;

inline constexpr int      WC_BITS = 5;
inline constexpr int      WC_DATA = 32 - WC_BITS;
inline constexpr WordCode WC_MASK = (1u << WC_BITS) - 1u;

constexpr WordCode wc_code(WordCode w) noexcept { return w & WC_MASK; }
constexpr WordCode wc_data(WordCode w) noexcept { return w >> WC_BITS; }
constexpr WordCode make_wc(WordCode op, WordCode d) noexcept {
  return (d << WC_BITS) | (op & WC_MASK);
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
  WC_CURSH     = 23,  // { brace group } run in the current shell
  WC_WORDS     = 24,  // count of the WC_STRING words that follow
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
//   pipeline := WC_PIPE(count|stderr_mask)  command{n}
//                 low 8 bits: stage count n
//                 upper bits: bit i set ⇔ pipe between stage i and i+1 is `|&`
//   command  := simple | redir* compound
//                 redirections written after a compound command are moved
//                 in front of it; what follows them tells the two apart (a
//                 simple command goes on with WC_ASSIGN, WC_SIMPLE or
//                 WC_TYPESET)
//   simple   := (assign | redir)*  WC_SIMPLE(argc)  WC_STRING(word){argc}
//                 argc may be 0 for an assignment-only command
//                 redirs and assigns may interleave in any order
//   simple   |= (assign | redir)*  WC_TYPESET(n)  item{n}
//                 for typeset, local, declare, export, readonly, integer
//                 and float: item is WC_STRING(word) for the command name,
//                 options and bare names, or an assign for `name=value`,
//                 whose value is expanded as an assignment (no splitting)
//   assign   := WC_ASSIGN(flags)  WC_STRING(name)  WC_STRING(value)
//                 flags: WC_ASSIGN_APPEND (`name+=value`),
//                        WC_ASSIGN_ARRAY (`name=(…)`: value is the text
//                        between the parentheses, split into words and
//                        expanded at execution time)
//   redir    := WC_REDIR(type|fd|has_fd)  WC_STRING(target_or_body)
//                 low 5 bits: RedirType
//                 bits 5–9:   fd (0–31), meaningful only when has_fd is set
//                 bit 10:     has_fd flag (user wrote a prefix `2>`, `3<`)
//                 bit 11:     literal flag: a here-doc whose delimiter was
//                             quoted, so its body is not expanded
//                 target_or_body is the here-doc body for REDIR_HEREDOC{,D},
//                 empty for REDIR_CLOSE, the subcommand source for
//                 REDIR_PROC_{IN,OUT,EQ}, otherwise the target word
//
// Compound commands.  Each header is op(type, skip): the low 3 bits are a
// type, the rest the number of words after the header up to the end of the
// construct.  A body is a nested program, list* WC_END.
//   words    := WC_WORDS(n)  WC_STRING{n}
//   if       := WC_IF(HEAD, skip)  branch+
//   branch   := WC_IF(IF, skip) body(cond) body(then)    `if` and each `elif`
//             | WC_IF(ELSE, skip) body
//                 a branch's skip reaches the next branch
//   while    := WC_WHILE(WHILE|UNTIL, skip)  body(cond)  body
//   for      := WC_FOR(IN|ARGS, skip)  words(names)  [words(values)]  body
//                 IN has the value list; ARGS (`for x; do`) loops over "$@"
//   cfor     := WC_FOR_COND(0, skip)  WC_STRING(init) WC_STRING(cond)
//               WC_STRING(step)  body
//   select   := WC_SELECT(IN|ARGS, skip)  as for
//   repeat   := WC_REPEAT(0, skip)  WC_STRING(count)  body
//   case     := WC_CASE(HEAD, skip)  WC_STRING(subject)  arm*
//   arm      := WC_CASE(BREAK|FALL|TESTNEXT, skip)  words(patterns)  body
//                 the arm's terminator: `;;`, `;&` or `;|`
//   subsh    := WC_SUBSH(0, skip)  body                  `( … )`
//   cursh    := WC_CURSH(0, skip)  body                  `{ … }`
//   try      := WC_TRY(0, skip)  body  body(always)      `{ … } always { … }`
//   funcdef  := WC_FUNCDEF(NAMED, skip)  words(names)  body
//             | WC_FUNCDEF(ANON, skip)  body  words(args)   `() { … } args`
//   arith    := WC_ARITH(0, 1)  WC_STRING(expr)          `(( … ))`
//
// WC_STRING's operand indexes CompiledProgram::strs.  Words are stored as
// written (quotes and `$…` included); the executor expands them at run time
// (src/exec/subst.cpp).
// ---------------------------------------------------------------------------

inline constexpr WordCode WC_LIST_ASYNC = 1u;  // & (background)
inline constexpr WordCode WC_LIST_END   = 2u;  // last in list
inline constexpr int      WC_LIST_FLAG_BITS = 2;

inline constexpr WordCode WC_SUBLIST_END  = 0u;
inline constexpr WordCode WC_SUBLIST_AND  = 1u;    // &&
inline constexpr WordCode WC_SUBLIST_OR   = 2u;    // ||
inline constexpr WordCode WC_SUBLIST_NOT  = 4u;    // !
inline constexpr WordCode WC_SUBLIST_TYPE = 3u;    // mask for END/AND/OR
inline constexpr int      WC_SUBLIST_FLAG_BITS = 3;

inline constexpr WordCode WC_ASSIGN_APPEND = 1u;   // name+=value
inline constexpr WordCode WC_ASSIGN_ARRAY  = 2u;   // name=(…)

constexpr WordCode make_wc_list(WordCode flags, WordCode skip) noexcept {
  return make_wc(WC_LIST, (skip << WC_LIST_FLAG_BITS) | flags);
}
constexpr WordCode wc_list_flags(WordCode w) noexcept {
  return wc_data(w) & ((1u << WC_LIST_FLAG_BITS) - 1u);
}
constexpr WordCode wc_list_skip(WordCode w) noexcept {
  return wc_data(w) >> WC_LIST_FLAG_BITS;
}

constexpr WordCode make_wc_sublist(WordCode flags, WordCode skip) noexcept {
  return make_wc(WC_SUBLIST, (skip << WC_SUBLIST_FLAG_BITS) | flags);
}
constexpr WordCode wc_sublist_flags(WordCode w) noexcept {
  return wc_data(w) & ((1u << WC_SUBLIST_FLAG_BITS) - 1u);
}
constexpr WordCode wc_sublist_skip(WordCode w) noexcept {
  return wc_data(w) >> WC_SUBLIST_FLAG_BITS;
}

enum RedirType : WordCode {
  REDIR_READ        = 0,   // <
  REDIR_WRITE       = 1,   // >
  REDIR_APPEND      = 2,   // >>
  REDIR_HEREDOC     = 3,   // <<
  REDIR_HEREDOCD    = 4,   // <<- (strip tabs)
  REDIR_HERESTR     = 5,   // <<<
  REDIR_READWRITE   = 6,   // <>
  REDIR_CLOBBER     = 7,   // >|
  REDIR_MERGE       = 8,   // >&N
  REDIR_MERGEOUT    = 9,   // &>
  REDIR_PROC_IN     = 10,  // <(cmd)  process substitution
  REDIR_PROC_OUT    = 11,  // >(cmd)
  REDIR_PROC_EQ     = 12,  // =(cmd)  zsh-specific
  REDIR_CLOSE       = 13,  // >&-
  REDIR_IN_DUP      = 14,  // <&N
  REDIR_APPEND_ERR  = 15,  // &>>
  REDIR_TYPE_MAX,
};

inline constexpr int WC_REDIR_TYPE_BITS = 5;
inline constexpr int WC_REDIR_FD_BITS   = 5;
inline constexpr WordCode WC_REDIR_HAS_FD_BIT  = 1u << 10;
inline constexpr WordCode WC_REDIR_LITERAL_BIT = 1u << 11;

constexpr WordCode make_wc_redir(RedirType type, int fd,
                                 bool literal = false) noexcept {
  WordCode packed = static_cast<WordCode>(type);
  if (literal) packed |= WC_REDIR_LITERAL_BIT;
  if (fd >= 0) {
    packed |= WC_REDIR_HAS_FD_BIT;
    packed |= static_cast<WordCode>(fd & 0x1f) << WC_REDIR_TYPE_BITS;
  }
  return make_wc(WC_REDIR, packed);
}
constexpr RedirType wc_redir_type(WordCode w) noexcept {
  return static_cast<RedirType>(wc_data(w) & 0x1fu);
}
constexpr bool wc_redir_has_fd(WordCode w) noexcept {
  return (wc_data(w) & WC_REDIR_HAS_FD_BIT) != 0u;
}
constexpr bool wc_redir_literal(WordCode w) noexcept {
  return (wc_data(w) & WC_REDIR_LITERAL_BIT) != 0u;
}
constexpr int wc_redir_fd(WordCode w) noexcept {
  return static_cast<int>((wc_data(w) >> WC_REDIR_TYPE_BITS) & 0x1fu);
}

inline constexpr int WC_PIPE_COUNT_BITS = 8;

constexpr WordCode make_wc_pipe(WordCode n, WordCode stderr_mask) noexcept {
  return make_wc(WC_PIPE, n | (stderr_mask << WC_PIPE_COUNT_BITS));
}
constexpr WordCode wc_pipe_count(WordCode w) noexcept {
  return wc_data(w) & ((1u << WC_PIPE_COUNT_BITS) - 1u);
}
constexpr bool wc_pipe_stderr(WordCode w, unsigned i) noexcept {
  return ((wc_data(w) >> WC_PIPE_COUNT_BITS) >> i) & 1u;
}

inline constexpr int      WC_CMD_TYPE_BITS = 3;

inline constexpr WordCode WC_IF_HEAD = 0u;
inline constexpr WordCode WC_IF_IF   = 1u;
inline constexpr WordCode WC_IF_ELSE = 2u;

inline constexpr WordCode WC_WHILE_WHILE = 0u;
inline constexpr WordCode WC_WHILE_UNTIL = 1u;

inline constexpr WordCode WC_FOR_IN   = 0u;   // also WC_SELECT
inline constexpr WordCode WC_FOR_ARGS = 1u;

inline constexpr WordCode WC_CASE_HEAD     = 0u;
inline constexpr WordCode WC_CASE_BREAK    = 1u;   // ;;
inline constexpr WordCode WC_CASE_FALL     = 2u;   // ;&
inline constexpr WordCode WC_CASE_TESTNEXT = 3u;   // ;|

inline constexpr WordCode WC_FUNCDEF_NAMED = 0u;
inline constexpr WordCode WC_FUNCDEF_ANON  = 1u;

// Header of a compound command (see the layout above).
constexpr WordCode make_wc_cmd(WordCode op, WordCode type, WordCode skip) noexcept {
  return make_wc(op, (skip << WC_CMD_TYPE_BITS) | type);
}
constexpr WordCode wc_cmd_type(WordCode w) noexcept {
  return wc_data(w) & ((1u << WC_CMD_TYPE_BITS) - 1u);
}
constexpr WordCode wc_cmd_skip(WordCode w) noexcept {
  return wc_data(w) >> WC_CMD_TYPE_BITS;
}

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

#endif  // JFSH_CORE_WORDCODE_H
