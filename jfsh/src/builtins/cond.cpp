// The `test` / `[` builtin in this file is derived from zsh 5.9's bin_test
// and testlex (Src/builtin.c), par_cond_2 and its helpers (Src/parse.c) and
// evalcond (Src/cond.c).  zsh's copyright and licence:
//
// The Z Shell is copyright (c) 1992-2017 Paul Falstad, Richard Coleman,
// Zoltán Hidvégi, Andrew Main, Peter Stephenson, Sven Wischnowsky, and
// others.  All rights reserved.  Individual authors, whether or not
// specifically named, retain copyright in all changes; in what follows, they
// are referred to as `the Zsh Development Group'.  This is for convenience
// only and this body has no legal status.  The Z shell is distributed under
// the following licence; any provisions made in individual files take
// precedence.
//
// Permission is hereby granted, without written agreement and without
// licence or royalty fees, to use, copy, modify, and distribute this
// software and to distribute modified versions of this software for any
// purpose, provided that the above copyright notice and the following
// two paragraphs appear in all copies of this software.
//
// In no event shall the Zsh Development Group be liable to any party for
// direct, indirect, special, incidental, or consequential damages arising out
// of the use of this software and its documentation, even if the Zsh
// Development Group have been advised of the possibility of such damage.
//
// The Zsh Development Group specifically disclaim any warranties, including,
// but not limited to, the implied warranties of merchantability and fitness
// for a particular purpose.  The software provided hereunder is on an "as is"
// basis, and the Zsh Development Group have no obligation to provide
// maintenance, support, updates, enhancements, or modifications.

#include <jfsh/builtins/cond.h>

#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <regex.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/math.h>
#include <jfsh/core/aliases.h>
#include <jfsh/core/stack.h>
#include <jfsh/exec/executor.h>
#include <jfsh/exec/subst.h>

// The conditions of `[[ … ]]` and `test`.  The `[[ ]]` grammar is parsed by
// Parser::par_cond and evaluated by Executor::exec_cond, which call the
// primitives here; `test` / `[` is builtin_test at the end of this file.

namespace jfsh {

bool cond_is_unary(strv op) noexcept {
  return op.size() == 2 and op[0] == '-'
     and strv{"abcdefghknoprstuvwxzLOGSN"}.find(op[1]) != strv::npos;
}

bool cond_is_numeric(strv op) noexcept {
  return op == "-eq" or op == "-ne" or op == "-lt" or op == "-le" or op == "-gt" or op == "-ge";
}

int cond_numeric(strv op, const MathNum& l, const MathNum& r) noexcept {
  // Operator by operator, so NaN is unequal to everything.
  auto test = [&](auto x, auto y) {
    return op == "-eq" ? x == y : op == "-ne" ? x != y
         : op == "-lt" ? x <  y : op == "-le" ? x <= y
         : op == "-gt" ? x >  y : x >= y;
  };
  bool t = l.is_float or r.is_float ? test(l.as_float(), r.as_float()) : test(l.i, r.i);
  return t ? 0 : 1;
}

static int status(bool b) noexcept { return b ? 0 : 1; }

static void complain(strv who, strv msg) {
  s::fprintf(stderr, "jfsh: %.*s%.*s\n", static_cast<int>(who.size()), who.data(),
             static_cast<int>(msg.size()), msg.data());
}

int cond_unary(strv op, const str& arg, ExecContext& ctx,
               strv who) {
  char c = op.size() == 2 and op[0] == '-' ? op[1] : 0;
  switch (c) {
    case 'n': return status(not arg.empty());
    case 'z': return status(arg.empty());
    case 'o': {
      // zsh's optison: one character is an option letter.
      bool negated = false;
      opt<OptIndex> index;
      if (arg.size() == 1) {
        if (auto l = ctx.opts.letter(arg[0])) {
          index = l->index;
          negated = l->negated;
        }
      } else if (auto info = OptionState::find(arg, &negated)) {
        index = info->get().index;
      }
      if (not index) {
        if (ctx.opts.get(OPT_POSIXBUILTINS)) return 1;
        complain(who, "no such option: " + arg);
        return 3;
      }
      return status(ctx.opts.get(*index) != negated);
    }
    case 'v': {
      // `name[sub]` asks for an element, as ${+name[sub]} does, and a
      // number for a positional parameter.
      bool digits = not arg.empty() and s::all_of(arg.begin(), arg.end(), [](char c) {
        return c >= '0' and c <= '9';
      });
      if ((digits or arg.find('[') != str::npos) and ctx.executor) {
        Expander ex{*ctx.executor, ctx};
        auto r = ex.scalar("${+" + arg + "}");
        return status(r and *r == "1");
      }
      auto p = ctx.params.lookup(arg);
      return status(p and p->get().is_set());
    }
    case 't': {
      // The descriptor is an arithmetic expression.
      auto fd = math_eval(arg, ctx.params, math_options(ctx), &ctx.modules.math_funcs());
      if (not fd) {
        s::fprintf(stderr, "jfsh: %s\n", fd.error().message.c_str());
        return 2;
      }
      s::int64_t n = fd->as_int();
      return status(n >= 0 and n <= INT32_MAX and ::isatty(static_cast<int>(n)));
    }
    case 'r': return status(::access(arg.c_str(), R_OK) == 0);
    case 'w': return status(::access(arg.c_str(), W_OK) == 0);
    case 'x': return status(::access(arg.c_str(), X_OK) == 0);
    case 'h': case 'L': {
      struct stat st;
      return status(::lstat(arg.c_str(), &st) == 0 and S_ISLNK(st.st_mode));
    }
    case 0:

      break; default: {
      if (not cond_is_unary(op)) break;
      struct stat st;
      if (::stat(arg.c_str(), &st) != 0) return 1;
      switch (c) {
        case 'a': case 'e': return 0;
        case 'b': return status(S_ISBLK(st.st_mode));
        case 'c': return status(S_ISCHR(st.st_mode));
        case 'd': return status(S_ISDIR(st.st_mode));
        case 'f': return status(S_ISREG(st.st_mode));
        case 'p': return status(S_ISFIFO(st.st_mode));
        case 'S': return status(S_ISSOCK(st.st_mode));
        case 'g': return status(st.st_mode & S_ISGID);
        case 'u': return status(st.st_mode & S_ISUID);
        case 'k': return status(st.st_mode & S_ISVTX);
        case 's': return status(st.st_size > 0);
        case 'O': return status(st.st_uid == ::geteuid());
        case 'G': return status(st.st_gid == ::getegid());
        case 'N': return status(st.st_atime <= st.st_mtime);
        default:  break;
      }
    }
  }
  complain(who, "unknown condition: " + str(op));
  return 2;
}

// Modification time in nanoseconds.
static s::int64_t mtime_ns(const struct stat& st) noexcept {
#ifdef __APPLE__
  return st.st_mtimespec.tv_sec * s::int64_t{1'000'000'000} + st.st_mtimespec.tv_nsec;
#else
  return st.st_mtim.tv_sec * s::int64_t{1'000'000'000} + st.st_mtim.tv_nsec;
#endif
}

int cond_file_binary(strv op, const str& l, const str& r) {
  struct stat a, b;
  if (::stat(l.c_str(), &a) != 0 or ::stat(r.c_str(), &b) != 0) return 1;
  if (op == "-nt") return status(mtime_ns(a) > mtime_ns(b));
  if (op == "-ot") return status(mtime_ns(a) < mtime_ns(b));
  return status(a.st_dev == b.st_dev and a.st_ino == b.st_ino);
}

// Characters (UTF-8 sequences) in the first n bytes of s.
static s::int64_t char_count(strv s, regoff_t n) noexcept {
  s::int64_t chars = 0;
  for (regoff_t i = 0; i < n; ++i)
    if ((static_cast<unsigned char>(s[static_cast<s::size_t>(i)]) & 0xC0) != 0x80) ++chars;
  return chars;
}

int cond_regex(const str& l, const str& re, ExecContext& ctx,
               strv who) {
  regex_t rx;
  if (int e = ::regcomp(&rx, re.c_str(), REG_EXTENDED); e != 0) {
    char buf[256];
    ::regerror(e, &rx, buf, sizeof buf);
    complain(who, str("failed to compile regex: ") + buf);
    return 1;
  }
  vec<regmatch_t> m(rx.re_nsub + 1);
  int rc = ::regexec(&rx, l.c_str(), m.size(), m.data(), 0);
  ::regfree(&rx);
  if (rc != 0) return 1;
  auto set_int = [&](const char* name, s::int64_t v) {
    (void)ctx.params.set_scalar(name, {});
    ctx.params.lookup(name)->get().set(v);
  };
  auto text = [&](const regmatch_t& r) {
    return l.substr(static_cast<s::size_t>(r.rm_so), static_cast<s::size_t>(r.rm_eo - r.rm_so));
  };
  (void)ctx.params.set_scalar("MATCH", text(m[0]));
  set_int("MBEGIN", char_count(l, m[0].rm_so) + 1);
  set_int("MEND", char_count(l, m[0].rm_eo));
  vec<str> match, mbegin, mend;
  for (s::size_t i = 1; i < m.size(); ++i) {
    bool used = m[i].rm_so >= 0;
    match.push_back(used ? text(m[i]) : str{});
    mbegin.push_back(used ? s::to_string(char_count(l, m[i].rm_so) + 1) : "-1");
    mend.push_back(used ? s::to_string(char_count(l, m[i].rm_eo)) : "-1");
  }
  (void)ctx.params.set_array("match", s::move(match));
  (void)ctx.params.set_array("mbegin", s::move(mbegin));
  (void)ctx.params.set_array("mend", s::move(mend));
  return 0;
}

// ---------------------------------------------------------------------------
// test / [
// ---------------------------------------------------------------------------
namespace {

// zsh's test lexes each argument as a token: -o, -a, !, ( and ) are
// operators, anything else a string.  Null is the end, LexErr reading
// past it (or a syntax error).
enum class TestTok : s::uint8_t { Null, LexErr, String, Or, And, Bang, InPar, OutPar };

// A condition after parsing: `||` or `&&` of the nodes in `items`, or a
// test whose operator and operands are the words in `items`.
struct TestNode {
  enum class Kind : s::uint8_t { Or, And, Unary, Binary, Mod };
  vec<s::uint32_t> items;
  Kind             kind;
  bool             negate = false;   // under an odd number of `!`
};

// zsh's get_cond_num: the infix conditions that take two operands.
bool is_cond_num(strv w) noexcept {
  return w == "-nt" or w == "-ot" or w == "-ef" or cond_is_numeric(w);
}

bool dash(strv w) noexcept { return w.size() > 1 and w[0] == '-'; }

// zsh's zstrtol: decimal, with blanks and a sign in front only.
opt<s::int64_t> test_integer(const str& w) {
  auto r = zstrtol(w, 0, 10);
  if (r.end != w.size()) return s::nullopt;
  return r.value;
}

// zsh's bin_test, with par_cond and par_cond_2 in their testlex mode: the
// whole argument list is parsed (syntax errors come before anything is
// tested), then evaluated.  `-o option` (two arguments) and `a < b` / `a >
// b` follow POSIX 2024, where zsh 5.9 rejects them.
struct Test {
  Test(const vec<str>& args, ExecContext& c, str name) : a(args), ctx(c), who(s::move(name)) {}

  void lex();
  // zsh's YYERROR and COND_ERROR; -1 for the caller to pass up.
  int syntax_error() {
    tok = TestTok::LexErr;
    return -1;
  }
  int cond_fail(strv msg);
  int add(TestNode::Kind kind, s::initializer_list<strv> args);
  int two(strv x, strv b);
  int three(strv l, strv op, strv r);
  int primary();
  int chain(TestNode::Kind kind);
  int expr() { return chain(TestNode::Kind::Or); }
  strv word(const TestNode& n, s::size_t k) const { return words[n.items[k]]; }
  int binary(strv op, const str& l, const str& r);
  int eval(const TestNode& n);

  const vec<str>& a;
  ExecContext&    ctx;
  str             who;
  vec<TestNode>   nodes;
  vec<strv>       words;       // the operators and operands of the tests
  s::size_t       next = 0;    // zsh's testargs: the argument after tok
  s::size_t       cur  = 0;    // zsh's curtestarg: tok's argument
  const str*      tokstr = nullptr;
  TestTok         tok = TestTok::Null;
  bool            cond_error = false;   // a message is out already
};

void Test::lex() {
  if (tok == TestTok::LexErr) return;
  cur = next;
  if (next >= a.size()) {
    tokstr = nullptr;
    tok = tok == TestTok::Null ? TestTok::LexErr : TestTok::Null;
    return;
  }
  tokstr = &a[next++];
  const str& w = *tokstr;
  tok = w == "-o" ? TestTok::Or : w == "-a" ? TestTok::And : w == "!" ? TestTok::Bang
      : w == "(" ? TestTok::InPar : w == ")" ? TestTok::OutPar : TestTok::String;
}

int Test::cond_fail(strv msg) {
  s::fprintf(stderr, "jfsh: %.*s\n", static_cast<int>(msg.size()), msg.data());
  cond_error = true;
  return syntax_error();
}

int Test::add(TestNode::Kind kind, s::initializer_list<strv> args) {
  TestNode n{{}, kind};
  for (strv w : args) {
    n.items.push_back(static_cast<s::uint32_t>(words.size()));
    words.push_back(w);
  }
  nodes.push_back(s::move(n));
  return static_cast<int>(nodes.size() - 1);
}

// par_cond_double: `-x b`.
int Test::two(strv x, strv b) {
  if (not dash(x)) return cond_fail("parse error: condition expected: " + str(x));
  if (x.size() == 2 and strv{"abcdefgknoprstuvwxzhLONGS"}.find(x[1]) != strv::npos)
    return add(TestNode::Kind::Unary, {x, b});
  return add(TestNode::Kind::Mod, {x, b});
}

// par_cond_triple: `l op r`, or `-x b c`.
int Test::three(strv l, strv op, strv r) {
  if (op == "=" or op == "==" or op == "!=" or op == "=~" or op == "<" or op == ">"
      or dash(op))
    return add(TestNode::Kind::Binary, {op, l, r});
  if (dash(l)) return add(TestNode::Kind::Mod, {l, op, r});
  return cond_fail("condition expected: " + str(op));
}

int Test::primary() {
  bool negate = false;
  auto done = [&](int n) {
    if (n >= 0 and negate) nodes[static_cast<s::size_t>(n)].negate ^= true;
    return n;
  };
  for (;;) {
    if (stack_low()) return cond_fail(who + "condition nested too deeply");
    s::size_t left = a.size() - next + 1;   // tok and the arguments after it
    if (tok == TestTok::Null) return done(two("-n", ""));
    if (left == 1) {
      strv s1 = *tokstr;
      lex();
      // ksh: `[ -t ]` means `[ -t 1 ]`.
      if (not ctx.opts.get(OPT_POSIXBUILTINS) and s1 == "-t") return done(two(s1, "1"));
      return done(two("-n", s1));
    }
    if (left == 2 and tok == TestTok::Or) {
      lex();
      strv name = *tokstr;
      lex();
      return done(add(TestNode::Kind::Unary, {"-o", name}));
    }
    if (left > 2) {
      strv w = a[next];
      if (w == "=" or w == "==" or w == "!=" or w == "<" or w == ">"
          or (dash(w) and is_cond_num(w))) {
        strv s1 = *tokstr;
        lex();
        strv s2 = *tokstr;
        lex();
        strv s3 = *tokstr;
        lex();
        return done(three(s1, s2, s3));
      }
    }
    // `! -a …` and `! -o …` take the `!` as a string.
    if (tok == TestTok::Bang and not (left > 2 and (a[next] == "-a" or a[next] == "-o"))) {
      lex();
      negate = not negate;
      continue;
    }
    break;
  }
  if (tok == TestTok::InPar) {
    lex();
    int r = expr();
    if (r < 0) return r;
    if (tok != TestTok::OutPar) return syntax_error();
    lex();
    return done(r);
  }
  s::size_t left = a.size() - next + 1;
  if (tok != TestTok::String) {
    if (not tokstr or tok == TestTok::LexErr) return syntax_error();
    strv s1 = *tokstr;
    lex();
    return done(two("-n", s1));
  }
  strv s1 = *tokstr;
  bool dble = s1.size() == 2 and s1[0] == '-'
          and strv{"abcdefghknoprstuvwxzLONGS"}.find(s1[1]) != strv::npos;
  lex();
  // `test -z` and then a token: the token is a string.
  if (left == 2 and tok != TestTok::String and tokstr and not s1.empty() and s1[0] == '-')
    tok = TestTok::String;
  if (tok != TestTok::String) {
    if (tok == TestTok::LexErr) return syntax_error();
    return done(two("-n", s1));
  }
  strv s2 = *tokstr;
  lex();
  if (tok != TestTok::String or dble) return done(two(s1, s2));
  strv s3 = *tokstr;
  lex();
  if (tok != TestTok::String) return done(three(s1, s2, s3));
  if (not dash(s1)) return cond_fail("condition expected: " + str(s1));
  int n = add(TestNode::Kind::Mod, {s1, s2, s3});
  while (tok == TestTok::String) {
    nodes.back().items.push_back(static_cast<s::uint32_t>(words.size()));
    words.push_back(*tokstr);
    lex();
  }
  return done(n);
}

// par_cond_1 and par_cond: `-a` binds tighter than `-o`; a chain is one
// node, read in a loop.
int Test::chain(TestNode::Kind kind) {
  TestTok sep = kind == TestNode::Kind::Or ? TestTok::Or : TestTok::And;
  auto operand = [&] {
    return kind == TestNode::Kind::Or ? chain(TestNode::Kind::And) : primary();
  };
  int first = operand();
  if (first < 0 or tok != sep) return first;
  vec<s::uint32_t> kids{static_cast<s::uint32_t>(first)};
  while (tok == sep) {
    lex();
    int k = operand();
    if (k < 0) return k;
    kids.push_back(static_cast<s::uint32_t>(k));
  }
  nodes.push_back({s::move(kids), kind});
  return static_cast<int>(nodes.size() - 1);
}

int Test::binary(strv op, const str& l, const str& r) {
  if (op == "=" or op == "==") return status(l == r);
  if (op == "!=") return status(l != r);
  if (op == "<")  return status(l < r);
  if (op == ">")  return status(l > r);
  if (op == "=~") return cond_regex(l, r, ctx, who);
  if (cond_is_numeric(op)) {
    auto x = test_integer(l);
    if (not x) {
      complain(who, "integer expression expected: " + l);
      return 2;
    }
    auto y = test_integer(r);
    if (not y) {
      complain(who, "integer expression expected: " + r);
      return 2;
    }
    return cond_numeric(op, MathNum::of(*x), MathNum::of(*y));
  }
  if (op == "-nt" or op == "-ot" or op == "-ef") return cond_file_binary(op, l, r);
  complain(who, "unknown condition: " + str(op));
  return 2;
}

// zsh's evalcond: `||` and `&&` stop at an error (2); `!` leaves one.
int Test::eval(const TestNode& n) {
  if (stack_low()) {
    complain(who, "condition nested too deeply");
    return 2;
  }
  int r = 0;
  switch (n.kind) {
    case TestNode::Kind::Or: case TestNode::Kind::And: {
      int go_on = n.kind == TestNode::Kind::Or ? 1 : 0;
      for (auto k : n.items)
        if ((r = eval(nodes[k])) != go_on) break;
    }

    break; case TestNode::Kind::Unary:
      r = cond_unary(word(n, 0), str(word(n, 1)), ctx, who);

    break; case TestNode::Kind::Binary:
      r = binary(word(n, 0), str(word(n, 1)), str(word(n, 2)));

    break; default:
      complain(who, "unknown condition: " + str(word(n, 0)));
      r = 2;
  }
  return n.negate and r < 2 ? 1 - r : r;
}

// zsh's is_cond_binary_op, for bin_test's `( a op b )` check.
bool test_binary_op(strv w) noexcept {
  return w == "=" or w == "==" or w == "!=" or w == "<" or w == ">" or w == "=~"
      or (w.size() > 1 and w[0] == '-' and is_cond_num(w));
}

}  // namespace

int builtin_test(vec<str>& args, ExecContext& ctx) {
  str who = args[0] + ": ";
  vec<str> operands(args.begin() + 1, args.end());
  if (args[0] == "[") {
    if (operands.empty() or operands.back() != "]") {
      complain(who, "']' expected");
      return 2;
    }
    operands.pop_back();
  }
  if (operands.empty()) return 1;
  // XSI's extensions: `( … )` around two or three arguments (unless the
  // three are a comparison), and `!` in front of three.
  bool sense = false;
  s::size_t n = operands.size();
  if (n == 3 or n == 4) {
    if (operands.front() == "(" and operands.back() == ")"
        and (n != 3 or not test_binary_op(operands[1]))) {
      operands.pop_back();
      operands.erase(operands.begin());
    }
    if (n == 4 and operands.front() == "!") {
      sense = true;
      operands.erase(operands.begin());
    }
  }
  Test t{operands, ctx, who};
  t.lex();
  int root = t.expr();
  if (t.cond_error) return 2;
  if (root < 0 or t.tok == TestTok::LexErr) {
    complain(who, t.tokstr ? "parse error" : "argument expected");
    return 2;
  }
  if (t.cur < operands.size()) {
    complain(who, "too many arguments");
    return 2;
  }
  int r = t.eval(t.nodes[static_cast<s::size_t>(root)]);
  return r < 2 and sense ? 1 - r : r;
}

}  // namespace jfsh
