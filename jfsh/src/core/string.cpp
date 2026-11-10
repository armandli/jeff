#include <jfsh/core/types.hh>
#include <cstring>
#include <cerrno>

namespace jfsh {

// ---------------------------------------------------------------------------
// Metastring
// ---------------------------------------------------------------------------

Metastring Metastring::from_plain(std::string_view s) {
    Metastring m;
    m.raw.reserve(s.size());
    for (unsigned char c : s) {
        if (c == '\0' || c >= 0x80 || c == static_cast<unsigned char>(kMeta)) {
            m.raw += kMeta;
            m.raw += static_cast<char>(c ^ 0x20);
        } else {
            m.raw += static_cast<char>(c);
        }
    }
    return m;
}

std::string Metastring::to_plain() const {
    std::string out;
    out.reserve(raw.size());
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == kMeta && i + 1 < raw.size()) {
            out += static_cast<char>(static_cast<unsigned char>(raw[++i]) ^ 0x20);
        } else {
            out += raw[i];
        }
    }
    return out;
}

std::size_t Metastring::plain_len() const {
    std::size_t n = 0;
    for (std::size_t i = 0; i < raw.size(); ++i) {
        if (raw[i] == kMeta && i + 1 < raw.size()) ++i;
        ++n;
    }
    return n;
}

// ---------------------------------------------------------------------------
// ShellError
// ---------------------------------------------------------------------------

ShellError ShellError::from_errno(ErrorKind k, const char* ctx) noexcept {
    ShellError e;
    e.kind    = k;
    e.code    = errno;
    e.message = std::string(ctx) + ": " + std::strerror(errno);
    return e;
}

} // namespace jfsh
