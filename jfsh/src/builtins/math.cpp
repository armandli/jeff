#include <cctype>

#include <string>

#include <jfsh/builtins/math.h>

// TODO(math/float): floating-point arithmetic (FORCE_FLOAT, `1.5`, `typeset
//   -F`) and the zsh/mathfunc functions (sin, sqrt, …).
// TODO(math/octal): OCTAL_ZEROES (`010` is 8) and C_BASES output.

namespace jfsh {
namespace {

constexpr int MAX_DEPTH = 64;

// Integer arithmetic wraps around on overflow, as in zsh, instead of being
// undefined behaviour: do it in unsigned and convert back.
using U = unsigned long long;
long long wrap(U v) noexcept { return static_cast<long long>(v); }
long long wadd(long long a, long long b) noexcept { return wrap(static_cast<U>(a) + static_cast<U>(b)); }
long long wsub(long long a, long long b) noexcept { return wrap(static_cast<U>(a) - static_cast<U>(b)); }
long long wmul(long long a, long long b) noexcept { return wrap(static_cast<U>(a) * static_cast<U>(b)); }
long long wneg(long long a) noexcept { return wrap(U{0} - static_cast<U>(a)); }
long long wshl(long long a, long long n) noexcept { return wrap(static_cast<U>(a) << (n & 63)); }
long long wshr(long long a, long long n) noexcept { return a >> (n & 63); }

struct Arith {
  std::string_view s;
  ParamTable&      params;
  int              depth;
  std::size_t      pos  = 0;
  std::string      err;

  // An lvalue is a parameter name; anything else has an empty name.
  struct Val {
    long long   v = 0;
    std::string name;
  };

  bool failed() const noexcept { return not err.empty(); }

  void fail(std::string msg) {
    if (err.empty()) err = std::move(msg);
  }

  void skip_ws() {
    while (pos < s.size() and std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
  }

  // Consumes `op` when it comes next and is not the start of a longer
  // operator listed in `unless`.
  bool eat(std::string_view op, std::string_view unless = {}) {
    skip_ws();
    if (s.substr(pos, op.size()) != op) return false;
    for (std::size_t i = 0; i < unless.size(); ++i)
      if (pos + op.size() < s.size() and s[pos + op.size()] == unless[i]) return false;
    pos += op.size();
    return true;
  }

  long long lookup(const std::string& name) {
    auto p = params.lookup(name);
    if (not p or not p->get().is_set()) return 0;
    const Param& param = p->get();
    if (param.is_integer()) return param.integer();
    std::string text = param.is_array()
        ? (param.array().empty() ? std::string{} : param.array().front())
        : param.scalar();
    if (text.empty()) return 0;
    if (depth >= MAX_DEPTH) { fail(name + ": max recursion depth exceeded"); return 0; }
    Arith sub{text, params, depth + 1, 0, {}};
    long long v = sub.run();
    if (sub.failed()) fail(sub.err);
    return v;
  }

  long long rvalue(const Val& v) {
    return v.name.empty() ? v.v : lookup(v.name);
  }

  void store(const Val& lv, long long v, bool live) {
    if (not live) return;
    if (lv.name.empty()) { fail("lvalue required"); return; }
    if (auto r = params.set_scalar(lv.name, std::to_string(v)); not r)
      fail(r.error().message);
  }

  Val number() {
    std::size_t start = pos;
    long long base = 10;
    if (s.substr(pos, 2) == "0x" or s.substr(pos, 2) == "0X") { base = 16; pos += 2; }
    else if (s.substr(pos, 2) == "0b" or s.substr(pos, 2) == "0B") { base = 2; pos += 2; }
    std::size_t digits_start = pos;
    while (pos < s.size() and std::isalnum(static_cast<unsigned char>(s[pos]))) ++pos;
    if (base == 10 and pos < s.size() and s[pos] == '#') {
      base = std::stoll(std::string(s.substr(start, pos - start)));
      ++pos;
      digits_start = pos;
      while (pos < s.size() and (std::isalnum(static_cast<unsigned char>(s[pos]))
                                 or s[pos] == '_' or s[pos] == '@')) ++pos;
      if (base < 2 or base > 36) { fail("invalid base: " + std::to_string(base)); return {}; }
    }
    if (pos < s.size() and s[pos] == '.') { fail("floating point is not supported"); return {}; }
    long long v = 0;
    for (std::size_t i = digits_start; i < pos; ++i) {
      int c = static_cast<unsigned char>(s[i]);
      int d = std::isdigit(c) ? c - '0'
            : std::isalpha(c) ? std::tolower(c) - 'a' + 10 : 99;
      if (d >= base) { fail("bad math expression: " + std::string(s.substr(start))); return {}; }
      v = wadd(wmul(v, base), d);
    }
    if (pos == digits_start) { fail("bad math expression: " + std::string(s.substr(start))); return {}; }
    return {v, {}};
  }

  Val primary(bool live) {
    skip_ws();
    if (pos >= s.size()) { fail("bad math expression: operand expected at end of string"); return {}; }
    char c = s[pos];
    if (c == '(') {
      ++pos;
      long long v = comma(live);
      if (not eat(")")) fail("bad math expression: ')' expected");
      return {v, {}};
    }
    if (std::isdigit(static_cast<unsigned char>(c))) return number();
    if (std::isalpha(static_cast<unsigned char>(c)) or c == '_') {
      std::size_t start = pos;
      while (pos < s.size() and (std::isalnum(static_cast<unsigned char>(s[pos])) or s[pos] == '_'))
        ++pos;
      return {0, std::string(s.substr(start, pos - start))};
    }
    fail(std::string("bad math expression: illegal character: ") + c);
    return {};
  }

  Val postfix(bool live) {
    Val v = primary(live);
    if (failed()) return {};
    if (eat("++") or eat("--")) {
      bool inc = s[pos - 1] == '+';
      long long old = rvalue(v);
      store(v, inc ? wadd(old, 1) : wsub(old, 1), live);
      return {old, {}};
    }
    return v;
  }

  Val unary(bool live) {
    if (eat("++") or eat("--")) {
      bool inc = s[pos - 1] == '+';
      Val v = unary(live);
      long long nv = wadd(rvalue(v), inc ? 1 : -1);
      store(v, nv, live);
      return {nv, {}};
    }
    if (eat("+")) return {rvalue(unary(live)), {}};
    if (eat("-")) return {wneg(rvalue(unary(live))), {}};
    if (eat("!", "=")) return {rvalue(unary(live)) == 0, {}};
    if (eat("~")) return {~rvalue(unary(live)), {}};
    return postfix(live);
  }

  static long long ipow(long long b, long long e) {
    long long r = 1;
    for (; e > 0; e >>= 1, b = wmul(b, b))
      if (e & 1) r = wmul(r, b);
    return r;
  }

  Val power(bool live) {
    Val b = unary(live);
    if (eat("**", "=")) {
      long long e = rvalue(power(live));
      if (e < 0) { fail("negative exponent"); return {}; }
      return {ipow(rvalue(b), e), {}};
    }
    return b;
  }

  long long divide(long long a, long long b, bool mod, bool live) {
    if (b == 0) {
      if (live) fail("division by zero");
      return 0;
    }
    if (b == -1) return mod ? 0 : wneg(a);   // LLONG_MIN / -1 overflows
    return mod ? a % b : a / b;
  }

  Val mul(bool live) {
    Val l = power(live);
    for (;;) {
      if (eat("*", "*=")) { long long a = rvalue(l); l = {wmul(a, rvalue(power(live))), {}}; }
      else if (eat("/", "=")) { long long a = rvalue(l); l = {divide(a, rvalue(power(live)), false, live), {}}; }
      else if (eat("%", "=")) { long long a = rvalue(l); l = {divide(a, rvalue(power(live)), true, live), {}}; }
      else return l;
    }
  }

  Val add(bool live) {
    Val l = mul(live);
    for (;;) {
      if (eat("+", "+=")) { long long a = rvalue(l); l = {wadd(a, rvalue(mul(live))), {}}; }
      else if (eat("-", "-=")) { long long a = rvalue(l); l = {wsub(a, rvalue(mul(live))), {}}; }
      else return l;
    }
  }

  Val shift(bool live) {
    Val l = add(live);
    for (;;) {
      if (eat("<<", "=")) { long long a = rvalue(l); l = {wshl(a, rvalue(add(live))), {}}; }
      else if (eat(">>", "=")) { long long a = rvalue(l); l = {wshr(a, rvalue(add(live))), {}}; }
      else return l;
    }
  }

  Val band(bool live) {
    Val l = shift(live);
    while (eat("&", "&=")) l = {rvalue(l) & rvalue(shift(live)), {}};
    return l;
  }

  Val bxor(bool live) {
    Val l = band(live);
    while (eat("^", "^=")) l = {rvalue(l) ^ rvalue(band(live)), {}};
    return l;
  }

  Val bor(bool live) {
    Val l = bxor(live);
    while (eat("|", "|=")) l = {rvalue(l) | rvalue(bxor(live)), {}};
    return l;
  }

  Val rel(bool live) {
    Val l = bor(live);
    for (;;) {
      if (eat("<=")) l = {rvalue(l) <= rvalue(bor(live)), {}};
      else if (eat(">=")) l = {rvalue(l) >= rvalue(bor(live)), {}};
      else if (eat("<", "<")) l = {rvalue(l) < rvalue(bor(live)), {}};
      else if (eat(">", ">")) l = {rvalue(l) > rvalue(bor(live)), {}};
      else return l;
    }
  }

  Val eq(bool live) {
    Val l = rel(live);
    for (;;) {
      if (eat("==")) l = {rvalue(l) == rvalue(rel(live)), {}};
      else if (eat("!=")) l = {rvalue(l) != rvalue(rel(live)), {}};
      else return l;
    }
  }

  Val land(bool live) {
    Val l = eq(live);
    while (eat("&&", "=")) {
      bool lv = rvalue(l) != 0;
      bool rv = rvalue(eq(live and lv)) != 0;
      l = {lv and rv, {}};
    }
    return l;
  }

  Val lor(bool live) {
    Val l = land(live);
    for (;;) {
      if (eat("||", "=")) {
        bool lv = rvalue(l) != 0;
        bool rv = rvalue(land(live and not lv)) != 0;
        l = {lv or rv, {}};
      } else if (eat("^^", "=")) {
        bool lv = rvalue(l) != 0;
        bool rv = rvalue(land(live)) != 0;
        l = {lv != rv, {}};
      } else {
        return l;
      }
    }
  }

  Val ternary(bool live) {
    Val c = lor(live);
    if (not eat("?")) return c;
    bool cv = rvalue(c) != 0;
    long long a = rvalue(assign(live and cv));
    if (not eat(":")) { fail("bad math expression: ':' expected"); return {}; }
    long long b = rvalue(ternary(live and not cv));
    return {cv ? a : b, {}};
  }

  Val assign(bool live) {
    std::size_t save = pos;
    Val l = ternary(live);
    if (failed() or l.name.empty()) return l;
    static constexpr std::string_view OPS[] = {
      "**=", "<<=", ">>=", "&&=", "||=", "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|=",
    };
    for (auto op : OPS) {
      if (eat(op)) {
        long long r = rvalue(assign(live));
        long long a = lookup(l.name);
        long long v = 0;
        std::string_view o = op.substr(0, op.size() - 1);
        if      (o == "**") v = r < 0 ? 0 : ipow(a, r);
        else if (o == "<<") v = wshl(a, r);
        else if (o == ">>") v = wshr(a, r);
        else if (o == "&&") v = a and r;
        else if (o == "||") v = a or r;
        else if (o == "+")  v = wadd(a, r);
        else if (o == "-")  v = wsub(a, r);
        else if (o == "*")  v = wmul(a, r);
        else if (o == "/")  v = divide(a, r, false, live);
        else if (o == "%")  v = divide(a, r, true, live);
        else if (o == "&")  v = a & r;
        else if (o == "^")  v = a ^ r;
        else                v = a | r;
        store(l, v, live);
        return {v, {}};
      }
    }
    if (eat("=", "=")) {
      long long r = rvalue(assign(live));
      store(l, r, live);
      return {r, {}};
    }
    (void)save;
    return l;
  }

  long long comma(bool live) {
    long long v = rvalue(assign(live));
    while (not failed() and eat(",")) v = rvalue(assign(live));
    return v;
  }

  long long run() {
    skip_ws();
    if (pos >= s.size()) return 0;
    long long v = comma(true);
    skip_ws();
    if (not failed() and pos < s.size())
      fail("bad math expression: illegal character: " + std::string(1, s[pos]));
    return v;
  }
};

}  // namespace

Result<long long> arith_eval(std::string_view expr, ParamTable& params) {
  Arith a{expr, params, 0, 0, {}};
  long long v = a.run();
  if (a.failed())
    return std::unexpected(ShellError{ErrorKind::BadSubstitution, a.err});
  return v;
}

}  // namespace jfsh
