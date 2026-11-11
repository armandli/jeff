#include <cstdio>
#include <string>

#include <jfsh/lexer/input.hh>

namespace jfsh {

StringInputSource::StringInputSource(std::string src, int start_line)
  : mSrc{std::move(src)}, mPos{0}, mLine{start_line} {}

int StringInputSource::next_char() {
  if (mPos >= mSrc.size()) return -1;
  int c = static_cast<unsigned char>(mSrc[mPos++]);
  if (c == '\n') ++mLine;
  return c;
}

// The lexer ungets whatever it peeked, including -1 at EOF; only real
// characters may move the position back.
void StringInputSource::unget(int c) {
  if (c == -1 or mPos == 0) return;
  --mPos;
  if (c == '\n' and mLine > 1) --mLine;
}

FileInputSource::FileInputSource(std::FILE* f, int start_line)
  : mFile{f}, mLine{start_line} {}

int FileInputSource::next_char() {
  int c = std::fgetc(mFile);
  if (c == '\n') ++mLine;
  return c;
}

void FileInputSource::unget(int c) {
  if (c == EOF) return;
  std::ungetc(c, mFile);
  if (c == '\n' and mLine > 1) --mLine;
}

}  // namespace jfsh
