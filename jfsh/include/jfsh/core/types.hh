#pragma once
// jfsh/core/types.hh — fundamental types shared across all subsystems.
//
// Mirrors the low-level types in zsh's zsh.h (LinkList, HashTable, Heap,
// metafication helpers) but expressed in C++26 idioms.
//
// Design principles:
//  - Prefer std containers with strong typedef wrappers over raw pointers.
//  - Use std::expected<T, ShellError> for fallible operations (no setjmp).
//  - Metafication (special quoting of bytes 0x80–0xff and 0x00) is preserved
//    via the Metastring type; ordinary std::string holds post-de-meta data.

#include <cstdint>
#include <cstddef>
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

// ---------------------------------------------------------------------------
// Error type
// ---------------------------------------------------------------------------

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

// ---------------------------------------------------------------------------
// Metastring — a string that may contain Meta-encoded bytes (like zsh).
// Meta-encoding: byte b >= 0x80 or b == 0x00 is stored as {Meta, b^0x20}.
// META char is 0x83; MARKER is 0xa0; IMETA 0x83.
// ---------------------------------------------------------------------------
inline constexpr char kMeta   = '\x83';
inline constexpr char kMarker = '\xa0';

struct Metastring {
    std::string raw;    // bytes possibly containing Meta sequences
    static Metastring from_plain(std::string_view s);
    std::string       to_plain() const;     // un-metafy
    std::size_t       plain_len() const;    // length after un-metafy
};

// ---------------------------------------------------------------------------
// LinkList — ordered list of heap-managed values (mirrors zsh LinkList).
// In practice most uses are small so std::list<T> is fine; specialise later.
// ---------------------------------------------------------------------------
template<typename T>
using LinkList = std::list<T>;

// String link-lists appear everywhere.
using StringList = LinkList<std::string>;

// ---------------------------------------------------------------------------
// HashTable — open-addressed map, the backbone of params/builtins/funcs/etc.
// Mirrors zsh's HashTable with named-node semantic (name is part of the node).
// ---------------------------------------------------------------------------
template<typename V>
using HashTable = std::unordered_map<std::string, V>;

// ---------------------------------------------------------------------------
// Heap / arena — zsh allocates most transient data in an arena that is freed
// after each top-level command. We replicate this with a simple arena tag so
// code can mark allocations as "command-local" vs "permanent".
// ---------------------------------------------------------------------------
enum class AllocKind : std::uint8_t {
    Permanent,  // lives for the shell's lifetime
    Command,    // freed after the current top-level command finishes
    Function,   // freed when the enclosing shell function returns
};

// ---------------------------------------------------------------------------
// PID / fd types
// ---------------------------------------------------------------------------
using Pid  = int;
using Fd   = int;

// ---------------------------------------------------------------------------
// Zsh "thingy" — a named editor widget.  Used by the line editor.
// (name, handler function pointer, flags)
// ---------------------------------------------------------------------------
using WidgetFn = std::function<int()>;

struct Thingy {
    std::string name;
    WidgetFn    handler;
    std::uint32_t flags = 0;
};

} // namespace jfsh
