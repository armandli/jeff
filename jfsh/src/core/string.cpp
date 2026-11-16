#include <cerrno>
#include <cstring>

#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>

namespace jfsh {

Metastring Metastring::from_plain(strv s) {
  Metastring m;
  m.raw.reserve(s.size());
  for (char ch : s) {
    auto c = static_cast<unsigned char>(ch);
    if (c == '\0' or c >= 0x80 or c == static_cast<unsigned char>(META)) {
      m.raw += META;
      m.raw += static_cast<char>(c ^ 0x20);
    } else {
      m.raw += static_cast<char>(c);
    }
  }
  return m;
}

str Metastring::to_plain() const {
  str out;
  out.reserve(raw.size());
  for (s::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == META and i + 1 < raw.size()) {
      out += static_cast<char>(
        static_cast<unsigned char>(raw[++i]) ^ 0x20);
    } else {
      out += raw[i];
    }
  }
  return out;
}

s::size_t Metastring::plain_len() const {
  s::size_t n = 0;
  for (s::size_t i = 0; i < raw.size(); ++i) {
    if (raw[i] == META and i + 1 < raw.size()) ++i;
    ++n;
  }
  return n;
}

ShellError ShellError::from_errno(ErrorKind k, const char* ctx) noexcept {
  ShellError e;
  e.kind    = k;
  e.code    = errno;
  e.message = str(ctx) + ": " + s::strerror(errno);
  return e;
}

}  // namespace jfsh
