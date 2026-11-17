#ifndef JFSH_CORE_UTF8_H
#define JFSH_CORE_UTF8_H
// jfsh/core/utf8.h — UTF-8 text as the line editor and the prompt see it:
// code points, character boundaries and display widths.
//
// Bytes that don't form valid UTF-8 are taken one at a time, each one
// column wide (shown as is), so any byte string can be edited.

#include <cstddef>

#include <string_view>

#include <jfsh/core/aliases.h>

namespace jfsh {

// Decodes the code point at `pos`; `len` gets the number of bytes it
// takes (1 for an invalid byte, which decodes as itself).
char32_t utf8_decode(strv s, s::size_t pos, s::size_t& len) noexcept;

// Appends `c` encoded as UTF-8.
void utf8_append(str& out, char32_t c);

// Columns `c` takes: 2 for wide (East Asian) characters, 0 for combining
// marks and other zero-width characters, 1 otherwise.  wcwidth() decides
// when the locale knows the character; otherwise a built-in table does.
int char_width(char32_t c) noexcept;

// Columns `s` takes, with control characters shown as ^X (2 columns).
int text_width(strv s) noexcept;

// The start of the character after the one at `pos`, combining marks
// that follow it included (so a base character and its marks move as
// one); s.size() at the end.
s::size_t next_char(strv s, s::size_t pos) noexcept;

// The start of the character before `pos`, moving over any combining
// marks to their base character; 0 at the start.
s::size_t prev_char(strv s, s::size_t pos) noexcept;

// Number of characters (as next_char counts them) in `s`.
s::size_t char_count(strv s) noexcept;

}  // namespace jfsh

#endif  // JFSH_CORE_UTF8_H
