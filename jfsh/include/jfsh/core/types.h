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

namespace jfsh {

enum class ErrorKind : std::uint8_t {
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
  ErrorKind   kind    = ErrorKind::None;
  std::string message;
  int         code    = 0;    // errno or exit code

  static ShellError from_errno(ErrorKind k, const char* ctx) noexcept;
};

template<typename T>
using Result = std::expected<T, ShellError>;

// Meta-encoding: byte b >= 0x80 or b == 0x00 is stored as {META, b^0x20}.
inline constexpr char META   = '\x83';
inline constexpr char MARKER = '\xa0';

struct Metastring {
  std::string raw;    // bytes possibly containing Meta sequences

  static Metastring from_plain(std::string_view s);
  std::string       to_plain() const;
  std::size_t       plain_len() const;
};

template<typename T>
using LinkList = std::list<T>;

using StringList = LinkList<std::string>;

template<typename V>
using HashTable = std::unordered_map<std::string, V>;

enum class AllocKind : std::uint8_t {
  Permanent,  // lives for the shell's lifetime
  Command,    // freed after the current top-level command finishes
  Function,   // freed when the enclosing shell function returns
};

using Pid = int;
using Fd  = int;

using WidgetFn = std::function<int()>;

struct Thingy {
  std::string   name;
  WidgetFn      handler;
  std::uint32_t flags = 0;
};

}  // namespace jfsh

#endif  // JFSH_CORE_TYPES_H
