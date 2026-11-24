#ifndef JFSH_CORE_READ_FILE_H
#define JFSH_CORE_READ_FILE_H
// jfsh/core/read_file.h — the whole of a file, read on a close-on-exec fd
// (scripts, sourced files, $mapfile).

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <optional>
#include <string>

#include <jfsh/core/aliases.h>

namespace jfsh {

// What `path` holds, up to the first read error; nothing, with errno set,
// when it can't be opened or is a directory (EISDIR).
inline opt<str> read_file(const str& path) {
  int fd = ::open(path.c_str(), O_RDONLY | O_NOCTTY | O_CLOEXEC);
  if (fd < 0) return s::nullopt;
  struct stat st{};
  if (::fstat(fd, &st) == 0 and S_ISDIR(st.st_mode)) {
    ::close(fd);
    errno = EISDIR;
    return s::nullopt;
  }
  str text;
  char buf[65536];
  for (;;) {
    ssize_t n = ::read(fd, buf, sizeof buf);
    if (n < 0 and errno == EINTR) continue;
    if (n <= 0) break;
    text.append(buf, static_cast<s::size_t>(n));
  }
  int saved = errno;
  ::close(fd);
  errno = saved;
  return text;
}

}  // namespace jfsh

#endif  // JFSH_CORE_READ_FILE_H
