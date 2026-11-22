#include <jfsh/core/utf8.h>

#include <cwchar>

#include <jfsh/core/aliases.h>

namespace jfsh {

char32_t utf8_decode(strv s, s::size_t pos, s::size_t& len) noexcept {
  auto byte = [&](s::size_t i) { return static_cast<unsigned char>(s[i]); };
  unsigned char b = byte(pos);
  len = 1;
  if (b < 0x80) return b;
  s::size_t need;
  char32_t  c;
  if      ((b & 0xe0) == 0xc0) { need = 1; c = b & 0x1f; }
  else if ((b & 0xf0) == 0xe0) { need = 2; c = b & 0x0f; }
  else if ((b & 0xf8) == 0xf0) { need = 3; c = b & 0x07; }
  else return b;
  if (pos + need >= s.size()) return b;
  for (s::size_t i = 1; i <= need; ++i) {
    unsigned char cb = byte(pos + i);
    if ((cb & 0xc0) != 0x80) return b;
    c = (c << 6) | (cb & 0x3f);
  }
  // Overlong forms, surrogates and values past U+10FFFF are invalid.
  static constexpr char32_t min_for[] = {0, 0x80, 0x800, 0x10000};
  if (c < min_for[need] or c > 0x10ffff or (c >= 0xd800 and c <= 0xdfff)) return b;
  len = need + 1;
  return c;
}

void utf8_append(str& out, char32_t c) {
  if (c < 0x80) {
    out += static_cast<char>(c);
  } else if (c < 0x800) {
    out += static_cast<char>(0xc0 | (c >> 6));
    out += static_cast<char>(0x80 | (c & 0x3f));
  } else if (c < 0x10000) {
    out += static_cast<char>(0xe0 | (c >> 12));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (c & 0x3f));
  } else {
    out += static_cast<char>(0xf0 | (c >> 18));
    out += static_cast<char>(0x80 | ((c >> 12) & 0x3f));
    out += static_cast<char>(0x80 | ((c >> 6) & 0x3f));
    out += static_cast<char>(0x80 | (c & 0x3f));
  }
}

namespace {

struct Range { char32_t lo, hi; };

constexpr Range ZeroWidth[] = {
  {0x0300, 0x036f}, {0x0483, 0x0489}, {0x0591, 0x05bd}, {0x0610, 0x061a},
  {0x064b, 0x065f}, {0x0670, 0x0670}, {0x06d6, 0x06dc}, {0x0900, 0x0903},
  {0x093a, 0x094f}, {0x0e31, 0x0e31}, {0x0e34, 0x0e3a}, {0x0e47, 0x0e4e},
  {0x1ab0, 0x1aff}, {0x1dc0, 0x1dff}, {0x200b, 0x200f}, {0x202a, 0x202e},
  {0x2060, 0x2064}, {0x20d0, 0x20ff}, {0x302a, 0x302f}, {0x3099, 0x309a},
  {0xfe00, 0xfe0f}, {0xfe20, 0xfe2f}, {0xfeff, 0xfeff}, {0x1f3fb, 0x1f3ff},
  {0xe0100, 0xe01ef},
};

constexpr Range Wide[] = {
  {0x1100, 0x115f}, {0x231a, 0x231b}, {0x2329, 0x232a}, {0x23e9, 0x23ec},
  {0x23f0, 0x23f0}, {0x23f3, 0x23f3}, {0x25fd, 0x25fe}, {0x2614, 0x2615},
  {0x2648, 0x2653}, {0x267f, 0x267f}, {0x2693, 0x2693}, {0x26a1, 0x26a1},
  {0x26aa, 0x26ab}, {0x26bd, 0x26be}, {0x26c4, 0x26c5}, {0x26ce, 0x26ce},
  {0x26d4, 0x26d4}, {0x26ea, 0x26ea}, {0x26f2, 0x26f5}, {0x26fa, 0x26fa},
  {0x26fd, 0x26fd}, {0x2705, 0x2705}, {0x270a, 0x270b}, {0x2728, 0x2728},
  {0x274c, 0x274c}, {0x274e, 0x274e}, {0x2753, 0x2755}, {0x2757, 0x2757},
  {0x2795, 0x2797}, {0x27b0, 0x27b0}, {0x27bf, 0x27bf}, {0x2b1b, 0x2b1c},
  {0x2b50, 0x2b50}, {0x2b55, 0x2b55}, {0x2e80, 0x303e}, {0x3041, 0x33ff},
  {0x3400, 0x4dbf}, {0x4e00, 0x9fff}, {0xa000, 0xa4cf}, {0xa960, 0xa97f},
  {0xac00, 0xd7a3}, {0xf900, 0xfaff}, {0xfe10, 0xfe19}, {0xfe30, 0xfe6f},
  {0xff00, 0xff60}, {0xffe0, 0xffe6}, {0x16fe0, 0x16fe4}, {0x17000, 0x18cff},
  {0x1b000, 0x1b2ff}, {0x1f004, 0x1f004}, {0x1f0cf, 0x1f0cf}, {0x1f18e, 0x1f18e},
  {0x1f191, 0x1f19a}, {0x1f200, 0x1f251}, {0x1f300, 0x1f320}, {0x1f32d, 0x1f335},
  {0x1f337, 0x1f37c}, {0x1f37e, 0x1f393}, {0x1f3a0, 0x1f3ca}, {0x1f3cf, 0x1f3d3},
  {0x1f3e0, 0x1f3f0}, {0x1f3f4, 0x1f3f4}, {0x1f3f8, 0x1f43e}, {0x1f440, 0x1f440},
  {0x1f442, 0x1f4fc}, {0x1f4ff, 0x1f53d}, {0x1f54b, 0x1f54e}, {0x1f550, 0x1f567},
  {0x1f57a, 0x1f57a}, {0x1f595, 0x1f596}, {0x1f5a4, 0x1f5a4}, {0x1f5fb, 0x1f64f},
  {0x1f680, 0x1f6c5}, {0x1f6cc, 0x1f6cc}, {0x1f6d0, 0x1f6d2}, {0x1f6d5, 0x1f6d7},
  {0x1f6eb, 0x1f6ec}, {0x1f6f4, 0x1f6fc}, {0x1f7e0, 0x1f7eb}, {0x1f90c, 0x1f93a},
  {0x1f93c, 0x1f945}, {0x1f947, 0x1f9ff}, {0x1fa70, 0x1faff}, {0x20000, 0x2fffd},
  {0x30000, 0x3fffd},
};

template <s::size_t N>
bool in(const Range (&table)[N], char32_t c) noexcept {
  s::size_t lo = 0, hi = N;
  while (lo < hi) {
    s::size_t mid = (lo + hi) / 2;
    if (c > table[mid].hi)      lo = mid + 1;
    else if (c < table[mid].lo) hi = mid;
    else return true;
  }
  return false;
}

}  // namespace

int char_width(char32_t c) noexcept {
  if (c < 0x20 or c == 0x7f) return 2;           // shown as ^X
  if (c < 0x7f) return 1;
  int w = ::wcwidth(static_cast<wchar_t>(c));
  if (w >= 0) return w;
  if (in(ZeroWidth, c)) return 0;
  if (in(Wide, c)) return 2;
  return 1;
}

int text_width(strv s) noexcept {
  int w = 0;
  for (s::size_t i = 0; i < s.size();) {
    s::size_t len;
    w += char_width(utf8_decode(s, i, len));
    i += len;
  }
  return w;
}

s::size_t next_char(strv s, s::size_t pos) noexcept {
  if (pos >= s.size()) return s.size();
  s::size_t len;
  utf8_decode(s, pos, len);
  pos += len;
  while (pos < s.size()) {
    char32_t c = utf8_decode(s, pos, len);
    if (c < 0x300 or char_width(c) != 0) break;
    pos += len;
  }
  return pos;
}

s::size_t prev_char(strv s, s::size_t pos) noexcept {
  if (pos > s.size()) pos = s.size();
  while (pos > 0) {
    // Back to the start of the previous code point: at most 3
    // continuation bytes, and only when they decode as one character.
    s::size_t start = pos - 1;
    for (s::size_t k = 1; k <= 3 and pos >= k + 1; ++k) {
      if ((static_cast<unsigned char>(s[pos - k]) & 0xc0) != 0x80) break;
      s::size_t len;
      utf8_decode(s, pos - k - 1, len);
      if (len == k + 1) { start = pos - k - 1; break; }
    }
    s::size_t len;
    char32_t c = utf8_decode(s, start, len);
    pos = start;
    if (pos == 0 or c < 0x300 or char_width(c) != 0) break;
  }
  return pos;
}

s::size_t char_count(strv s) noexcept {
  s::size_t n = 0;
  for (s::size_t i = 0; i < s.size(); i = next_char(s, i)) ++n;
  return n;
}

s::size_t code_point_count(strv s) noexcept {
  s::size_t n = 0;
  for (s::size_t i = 0, len = 1; i < s.size(); i += len, ++n) (void)utf8_decode(s, i, len);
  return n;
}

s::size_t code_point_offset(strv s, s::size_t n) noexcept {
  s::size_t i = 0;
  for (s::size_t len = 1; i < s.size() and n > 0; i += len, --n) (void)utf8_decode(s, i, len);
  return i;
}

}  // namespace jfsh
