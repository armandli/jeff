#include <jfsh/lexer/lexer.hh>
#include <cstdio>
#include <string>

namespace jfsh {

// ---------------------------------------------------------------------------
// StringInputSource — lex from an in-memory string
// ---------------------------------------------------------------------------
class StringInputSource : public InputSource {
public:
    explicit StringInputSource(std::string src, int start_line = 1)
        : src_(std::move(src)), pos_(0), line_(start_line) {}

    int next_char() override {
        if (pos_ >= src_.size()) return -1;
        int c = static_cast<unsigned char>(src_[pos_++]);
        if (c == '\n') ++line_;
        return c;
    }
    void unget(int c) override {
        if (pos_ > 0) {
            --pos_;
            if (c == '\n' && line_ > 1) --line_;
        }
    }
    int line() const noexcept override { return line_; }

private:
    std::string src_;
    std::size_t pos_;
    int         line_;
};

// ---------------------------------------------------------------------------
// FileInputSource — lex from a FILE*
// ---------------------------------------------------------------------------
class FileInputSource : public InputSource {
public:
    explicit FileInputSource(std::FILE* f, int start_line = 1)
        : f_(f), line_(start_line) {}

    int next_char() override {
        int c = std::fgetc(f_);
        if (c == '\n') ++line_;
        return c;
    }
    void unget(int c) override {
        std::ungetc(c, f_);
        if (c == '\n' && line_ > 1) --line_;
    }
    int line() const noexcept override { return line_; }

private:
    std::FILE* f_;
    int        line_;
};

} // namespace jfsh
