#ifndef JFSH_LEXER_INPUT_H
#define JFSH_LEXER_INPUT_H
// jfsh/lexer/input.h — concrete InputSources for the lexer.
//
// Mirrors zsh's input.c.  StringInputSource backs `-c`, `eval`, scripts read
// into memory and tests; FileInputSource reads a stream incrementally.

#include <cstdio>

#include <string>
#include <utility>

#include <jfsh/core/aliases.h>
#include <jfsh/lexer/lexer.h>

namespace jfsh {

struct StringInputSource : InputSource {
  explicit StringInputSource(str src, int start_line = 1)
    : mSrc{s::move(src)}, mPos{0}, mLine{start_line} {}

  int  next_char() override;
  void unget(int c) override;
  int  line() const noexcept override { return mLine; }

private:
  str       mSrc;
  s::size_t mPos;
  int       mLine;
};

struct FileInputSource : InputSource {
  explicit FileInputSource(s::FILE* f, int start_line = 1)
    : mFile{f}, mLine{start_line} {}

  int  next_char() override;
  void unget(int c) override;
  int  line() const noexcept override { return mLine; }

private:
  s::FILE* mFile;
  int      mLine;
};

}  // namespace jfsh

#endif  // JFSH_LEXER_INPUT_H
