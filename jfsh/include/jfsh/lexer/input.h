#ifndef JFSH_LEXER_INPUT_H
#define JFSH_LEXER_INPUT_H
// jfsh/lexer/input.h — concrete InputSources for the lexer.
//
// Mirrors zsh's input.c.  StringInputSource backs `-c`, `eval`, scripts read
// into memory and tests; FileInputSource reads a stream incrementally.

#include <cstdio>
#include <string>

#include <jfsh/lexer/lexer.h>

namespace jfsh {

struct StringInputSource : InputSource {
  explicit StringInputSource(std::string src, int start_line = 1);

  int  next_char() override;
  void unget(int c) override;
  int  line() const noexcept override { return mLine; }

private:
  std::string mSrc;
  std::size_t mPos;
  int         mLine;
};

struct FileInputSource : InputSource {
  explicit FileInputSource(std::FILE* f, int start_line = 1);

  int  next_char() override;
  void unget(int c) override;
  int  line() const noexcept override { return mLine; }

private:
  std::FILE* mFile;
  int        mLine;
};

}  // namespace jfsh

#endif  // JFSH_LEXER_INPUT_H
