#ifndef JFSH_CORE_TYPES_H
#define JFSH_CORE_TYPES_H
// jfsh/core/types.h — fundamental types shared across all subsystems.
//
// Mirrors the low-level types in zsh's zsh.h (LinkList, HashTable, Heap,
// metafy helpers) but expressed in C++26 idioms.
//
// Design notes:
//  - Prefer std containers over raw pointers.
//  - Use std::expected<T, ShellError> for fallible operations (no setjmp).
//  - Metafy (special quoting of bytes >=0x80 and 0x00) is preserved via
//    Metastring; ordinary std::string holds post-de-meta data.

#include <cstddef>
#include <cstdint>

#include <expected>
#include <functional>
#include <list>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <variant>
#include <vector>

#include <jfsh/core/aliases.h>

namespace jfsh {

enum class ErrorKind : s::uint8_t {
  None = 0,
  ParseError,
  ExecError,
  SignalInterrupt,
  FileNotFound,
  PermissionDenied,
  SyntaxError,
  BadSubstitution,
  BadPattern,
  ModuleError,
  RouterError,
  AgentError,
};

struct ShellError {
  ErrorKind kind    = ErrorKind::None;
  str       message;
  int       code    = 0;    // errno or exit code

  static ShellError from_errno(ErrorKind k, const char* ctx) noexcept;
};

template<typename T>
using Result = s::expected<T, ShellError>;

// Meta-encoding: byte b >= 0x80 or b == 0x00 is stored as {META, b^0x20}.
inline constexpr char META   = '\x83';
inline constexpr char MARKER = '\xa0';

struct Metastring {
  str raw;    // bytes possibly containing Meta sequences

  static Metastring from_plain(strv s);
  str               to_plain() const;
  s::size_t         plain_len() const;
};

template<typename T>
using LinkList = s::list<T>;

using StringList = LinkList<str>;

template<typename V>
using HashTable = umap<str, V>;

enum class AllocKind : s::uint8_t {
  Permanent,  // lives for the shell's lifetime
  Command,    // freed after the current top-level command finishes
  Function,   // freed when the enclosing shell function returns
};

using Pid = int;
using Fd  = int;

using WidgetFn = func<int()>;

struct Thingy {
  str         name;
  WidgetFn    handler;
  s::uint32_t flags = 0;
};

}  // namespace jfsh

#endif  // JFSH_CORE_TYPES_H
