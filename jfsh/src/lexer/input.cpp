#include <cstdio>
#include <string>

#include <jfsh/lexer/lexer.hh>

namespace jfsh {

struct StringInputSource : InputSource {
  explicit StringInputSource(std::string src, int start_line = 1)
    : mSrc{std::move(src)}, mPos{0}, mLine{start_line} {}

  int next_char() override {
    if (mPos >= mSrc.size()) return -1;
    int c = static_cast<unsigned char>(mSrc[mPos++]);
    if (c == '\n') ++mLine;
    return c;
  }

  void unget(int c) override {
    if (mPos > 0) {
      --mPos;
      if (c == '\n' and mLine > 1) --mLine;
    }
  }

  int line() const noexcept override { return mLine; }

private:
  std::string mSrc;
  std::size_t mPos;
  int         mLine;
};

struct FileInputSource : InputSource {
  explicit FileInputSource(std::FILE* f, int start_line = 1)
    : mFile{f}, mLine{start_line} {}

  int next_char() override {
    int c = std::fgetc(mFile);
    if (c == '\n') ++mLine;
    return c;
  }

  void unget(int c) override {
    std::ungetc(c, mFile);
    if (c == '\n' and mLine > 1) --mLine;
  }

  int line() const noexcept override { return mLine; }

private:
  std::FILE* mFile;
  int        mLine;
};

}  // namespace jfsh
