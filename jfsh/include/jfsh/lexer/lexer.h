#ifndef JFSH_LEXER_LEXER_H
#define JFSH_LEXER_LEXER_H
// jfsh/lexer/lexer.h — shell lexer (tokenizer).
//
// Mirrors zsh's lex.c: gettok() / gettokstr() / zshlex().

#include <cstdint>

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include <jfsh/core/aliases.h>
#include <jfsh/core/types.h>

namespace jfsh {

enum class TokType : s::uint16_t {
  TK_NULL = 0,
  TK_EOF,
  TK_NEWLINE,
  TK_SEMI,         // ;
  TK_DSEMI,        // ;;
  TK_SEMIAMP,      // ;&
  TK_SEMIBAR,      // ;|  (fallthrough)
  TK_AMP,          // &
  TK_AMPAMP,       // &&
  TK_PIPE,         // |
  TK_PIPEPIPE,     // ||
  TK_PIPEAMP,      // |& (stderr + stdout pipe)
  TK_BANG,         // !
  TK_AMPPAREN,     // &(  async subshell
  TK_LPAREN,       // (
  TK_RPAREN,       // )
  TK_DLPAREN,      // ((
  TK_DRPAREN,      // ))  (unused: see Lexer::read_arith)
  TK_LBRACE,       // {
  TK_RBRACE,       // }
  TK_REDIR,        // any < > << >> <> >| >& &> <<< <( >( =( etc.
  TK_WORD,         // an unquoted or quoted word
  TK_STRING,       // here-document delimiter
  TK_IF, TK_THEN, TK_ELIF, TK_ELSE, TK_FI,
  TK_FOR, TK_IN, TK_DO, TK_DONE,
  TK_WHILE, TK_UNTIL,
  TK_CASE, TK_ESAC,
  TK_SELECT,
  TK_REPEAT,
  TK_FOREACH, TK_END,
  TK_FUNCTION,
  TK_TIME,
  TK_COPROC,
  TK_ALWAYS,
  TK_MAX,
};

struct LexToken {
  TokType type = TokType::TK_NULL;
  str     text;
  int     line = 0;
  // Aliases whose text the token was read from (so they don't expand
  // again inside it), and whether it follows an alias whose text ends in a
  // blank, which makes it eligible for alias expansion too.
  sptr<const vec<str>> aliases = {};
  bool                 after_alias_blank = false;

  bool is_word()    const noexcept { return type == TokType::TK_WORD; }
  bool is_newline() const noexcept { return type == TokType::TK_NEWLINE; }
  bool is_eof()     const noexcept { return type == TokType::TK_EOF; }
};

enum LexFlags : s::uint32_t {
  LEX_NORMAL       = 0,
  LEX_SKIPCOMMENTS = 1 << 0,
  LEX_CASEMOD      = 1 << 1,
  LEX_ACTIVE       = 1 << 2,
  LEX_DOCSTRING    = 1 << 3,
  LEX_COND         = 1 << 4,   // inside [[ ]]: `<` `>` are words, `((` is two `(`
  // A case pattern: a leading `(…)` group, blanks and all, is part of the
  // word, as zsh's lexer reads one in incasepat.
  LEX_CASEPAT      = 1 << 5,
  // After a command's first word: `(` starts a glob group word
  // (`echo (a|b).c`, `ls (#i)x*`) unless `)` follows (`f () …`).
  LEX_ARGPAREN     = 1 << 6,
};

struct InputSource {
  virtual ~InputSource() = default;
  virtual int  next_char() = 0;
  virtual void unget(int c) = 0;
  virtual int  line() const noexcept { return 0; }
};

// The lexer's input: the source, with alias text pushed in front of it.
// A pushed text is dropped once read past; characters put back go back
// where they came from.
struct LexInput : InputSource {
  explicit LexInput(InputSource& raw) : mRaw{raw} {}

  int  next_char() override;
  void unget(int c) override;
  int  line() const noexcept override { return mRaw.line(); }

  void push(str text, sptr<const vec<str>> names);
  // Puts back text the lexer read ahead; unlike an alias's text, a blank
  // at its end doesn't make the next word an alias candidate.
  void push_back_text(str text);
  // The aliases of the text being read now; null when reading the source.
  const sptr<const vec<str>>& current_aliases() const noexcept;
  // An alias text ending in a blank was read past since the last call.
  bool take_blank_flag() noexcept { return s::exchange(mBlankDone, false); }

private:
  struct Pushed {
    str                  text;
    s::size_t            pos = 0;
    sptr<const vec<str>> names;
    bool                 alias = true;
  };
  InputSource&  mRaw;
  vec<Pushed>   mPushed;
  vec<bool>     mFromRaw;      // where recent characters came from
  bool          mBlankDone = false;
};

struct Lexer {
  explicit Lexer(InputSource& src) : mSrc{src} {}
  ~Lexer();

  LexToken         next_token();
  const LexToken&  peek();
  void             consume();
  int              line() const noexcept { return mSrc.line(); }

  void     push_state(LexFlags flags) {
    mStateStack.push_back(mFlags);
    mFlags = flags;
  }
  void     pop_state() {
    if (not mStateStack.empty()) {
      mFlags = mStateStack.back();
      mStateStack.pop_back();
    }
  }
  LexFlags state() const noexcept { return mFlags; }

  // Puts a token back so the next peek or next_token returns it; nothing
  // may be peeked already.
  void unread(LexToken tok) { mPeeked = s::move(tok); }
  // Puts `text` back in front of the input, to be read as tokens again.
  void push_back_text(str text) { mSrc.push_back_text(s::move(text)); }
  // Reads a peeked word that starts with `(` again under the current
  // flags: one peeked as a case pattern can be a subshell instead.
  void relex_peeked() {
    if (mPeeked and mPeeked->type == TokType::TK_WORD and not mPeeked->text.empty()
        and mPeeked->text[0] == '(') {
      mSrc.push_back_text(s::move(mPeeked->text));
      mPeeked.reset();
    }
  }

  void set_alias_expansion(bool on) noexcept { mAliasExpand = on; }
  bool alias_expansion() const noexcept { return mAliasExpand; }

  // Reads `text` before the rest of the input: the expansion of alias
  // `names.back()`, with `names` the aliases being expanded, which don't
  // expand again inside it.  Nothing may be peeked.
  void push_alias(str text, sptr<const vec<str>> names) {
    mSrc.push(s::move(text), s::move(names));
  }

  // After a `((` token (and with nothing peeked past it): the raw text up
  // to the `))` that closes it, with nested parentheses kept.  nullopt when
  // the input ends first.
  opt<str> read_arith();

  // Inside [[ ]], after a `=`, `==`, `!=` or `=~` token (and with nothing
  // peeked past it): the pattern or regex operand as written.  Unlike a
  // word, it keeps parentheses and `|` inside them (`(a|b)*`), and ends at
  // a blank, or at a `)` or `|` outside parentheses.  nullopt at the end of
  // the input.
  // `prefix` is what was read of it already (a `(` the lexer took as a
  // token).
  opt<str> read_cond_pattern(str prefix = {});

  // Here-documents: the parser registers each delimiter as it parses the
  // redirection; the lexer reads the bodies, in order, when it reaches the
  // end of that line, and take_heredocs() hands them over.
  void register_heredoc(str delim, bool strip_tabs, bool expand) {
    mHeredocQueue.push_back({s::move(delim), strip_tabs, expand});
  }
  vec<str> take_heredocs();

  // The input ended inside a quote, after a trailing backslash, or before
  // a here-document's delimiter line: more input would continue it.
  bool incomplete() const noexcept { return mIncomplete; }

protected:
  LexToken read_token();
  LexToken read_token_raw();
  // `prefix` is word text already read (a process substitution that
  // more of the word follows).
  LexToken read_word(int first_char, str prefix = {});
  LexToken read_redir(int first_char);
  LexToken read_heredoc_delimiter();
  LexToken newline_token();
  void     read_heredoc_bodies();
  bool     is_keyword(strv s) const noexcept;

private:
  struct HeredocSpec {
    str  delim;
    bool strip_tabs;
    bool expand;
  };

  LexInput          mSrc;
  opt<LexToken>     mPeeked;
  // Captured as each token starts (see read_token).
  sptr<const vec<str>> mTokAliases;
  bool                 mTokAfterBlank = false;
  LexFlags          mFlags       = LEX_NORMAL;
  bool              mAliasExpand = true;
  bool              mIncomplete  = false;
  vec<LexFlags>     mStateStack;
  vec<HeredocSpec>  mHeredocQueue;
  vec<str>          mHeredocBodies;
};

}  // namespace jfsh

#endif  // JFSH_LEXER_LEXER_H
