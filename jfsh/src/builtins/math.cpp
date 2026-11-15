#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include <limits>
#include <string>

#include <jfsh/builtins/math.h>

// TODO(math/mathfunc): the zsh/mathfunc functions (sin, sqrt, int, …), once
//   zmodload exists.
// TODO(math/output): output bases `$(( [#16] x ))` and C_BASES.

namespace jfsh {

long long MathNum::as_int() const noexcept {
  if (not is_float) return i;
  if (std::isnan(d)) return 0;
  constexpr double LIMIT = 9223372036854775807.0;
  if (d >= LIMIT)  return std::numeric_limits<long long>::max();
  if (d <= -LIMIT) return std::numeric_limits<long long>::min();
  return static_cast<long long>(d);
}

MathOptions math_options(const OptionState& opts) noexcept {
  return {opts.get(OPT_FORCEFLOAT), opts.get(OPT_OCTALZEROES), opts.get(OPT_CPRECEDENCES)};
}

std::string format_math(const MathNum& n) {
  if (not n.is_float) return std::to_string(n.i);
  if (std::isnan(n.d)) return "NaN";
  if (std::isinf(n.d)) return n.d < 0 ? "-Inf" : "Inf";
  char buf[64];
  std::snprintf(buf, sizeof buf, "%.17g", n.d);
  std::string s = buf;
  if (s.find_first_of(".e") == std::string::npos) s += '.';
  return s;
}

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

long long ipow(long long b, long long e) noexcept {
  long long r = 1;
  for (; e > 0; e >>= 1, b = wmul(b, b))
    if (e & 1) r = wmul(r, b);
  return r;
}

bool is_ident_start(char c) noexcept {
  return std::isalpha(static_cast<unsigned char>(c)) or c == '_';
}
bool is_ident_char(char c) noexcept {
  return std::isalnum(static_cast<unsigned char>(c)) or c == '_';
}

// Value of digit c, or 99 when it is not one.  zsh's base#digits also
// takes `@` and `_` as digits 62 and 63 above base 36; they are not
// supported here.
int digit_value(char c) noexcept {
  auto u = static_cast<unsigned char>(c);
  if (std::isdigit(u)) return c - '0';
  if (std::isalpha(u)) return std::tolower(u) - 'a' + 10;
  return 99;
}

// Binary operators (assignment and `?:` are handled on their own levels).
// Higher binds tighter.
struct BinOp {
  std::string_view text;
  int              zsh_prec;
  int              c_prec;
};

// Longest first, so `**` is not taken for `*` nor `<=` for `<`.
constexpr BinOp BIN_OPS[] = {
  {"**", 10, 15},
  {"<<", 14, 12}, {">>", 14, 12},
  {"<=",  7, 11}, {">=",  7, 11},
  {"==",  6, 10}, {"!=",  6, 10},
  {"&&",  5,  6}, {"||",  4,  4}, {"^^",  4,  5},
  {"<",   7, 11}, {">",   7, 11},
  {"+",   8, 13}, {"-",   8, 13},
  {"*",   9, 14}, {"/",   9, 14}, {"%",   9, 14},
  {"&",  13,  9}, {"^",  12,  8}, {"|",  11,  7},
};

// The binary operator each compound assignment applies.
constexpr std::string_view ASSIGN_OPS[] = {
  "**=", "<<=", ">>=", "&&=", "||=", "^^=",
  "+=", "-=", "*=", "/=", "%=", "&=", "^=", "|=",
};

struct Arith {
  std::string_view s;
  ParamTable&      params;
  MathOptions      opts;
  int              depth;
  std::size_t      pos = 0;
  std::string      err;

  // An lvalue is a parameter name, with the text of its subscript when it
  // is `name[sub]`; anything else has an empty name.
  struct Val {
    MathNum     n;
    std::string name;
    std::string sub;
    bool        indexed = false;
  };

  static Val num(MathNum n) { return {n, {}, {}, false}; }
  static Val num(long long v) { return num(MathNum::of(v)); }

  bool failed() const noexcept { return not err.empty(); }

  void fail(std::string msg) {
    if (err.empty()) err = std::move(msg);
  }

  void bad(std::string_view msg) { fail("bad math expression: " + std::string(msg)); }

  void skip_ws() {
    while (pos < s.size() and std::isspace(static_cast<unsigned char>(s[pos]))) ++pos;
  }

  bool at(std::string_view op) {
    skip_ws();
    return s.substr(pos, op.size()) == op;
  }

  bool eat(std::string_view op) {
    if (not at(op)) return false;
    pos += op.size();
    return true;
  }

  // The rest of the input, for "operator expected at `…'".
  std::string rest() const { return std::string(s.substr(pos)); }

  // A sub-evaluation of `text` with the same parameters and options.
  MathNum eval_sub(std::string_view text, const std::string& what) {
    if (depth >= MAX_DEPTH) { fail("math recursion limit exceeded: " + what); return {}; }
    Arith sub{text, params, opts, depth + 1, 0, {}};
    MathNum v = sub.run();
    if (sub.failed()) fail(sub.err);
    return v;
  }

  // ---------------------------------------------------------------------
  // Parameters.
  // ---------------------------------------------------------------------

  // The 0-based position of 1-based (or negative, from the end) index i in
  // an array of n elements, or -1 when it is before the start.
  static long long array_slot(long long i, long long n) noexcept {
    if (i < 0) i += n + 1;
    return i - 1;
  }

  MathNum lookup(const Val& v) {
    auto p = params.lookup(v.name);
    if (not p or not p->get().is_set()) return {};
    const Param& param = p->get();
    std::string text;
    if (v.indexed) {
      if (param.is_hash()) {
        auto h = param.hash();
        auto it = h.find(v.sub);
        if (it == h.end()) return {};
        text = it->second;
      } else {
        long long i = eval_sub(v.sub, v.name).as_int();
        auto arr = param.is_array() ? param.array() : std::vector<std::string>{param.scalar()};
        long long slot = array_slot(i, static_cast<long long>(arr.size()));
        if (failed() or slot < 0 or slot >= static_cast<long long>(arr.size())) return {};
        text = arr[static_cast<std::size_t>(slot)];
      }
    } else if (param.is_integer()) {
      return MathNum::of(param.integer());
    } else if (param.is_float()) {
      return MathNum::of(param.floatv());
    } else if (param.is_array()) {
      for (const auto& e : param.array()) text += (text.empty() ? "" : " ") + e;
    } else {
      text = param.scalar();
    }
    if (text.empty()) return {};
    return eval_sub(text, v.name);
  }

  MathNum rvalue(const Val& v) {
    if (failed()) return {};
    return v.name.empty() ? v.n : lookup(v);
  }

  void store(const Val& lv, MathNum v, bool live) {
    if (not live or failed()) return;
    if (lv.name.empty()) { bad("lvalue required"); return; }
    auto p = params.lookup(lv.name);
    if (p and p->get().is_readonly()) { fail("read-only variable: " + lv.name); return; }
    if (lv.indexed) { store_element(lv, v); return; }
    if (p and p->get().is_set()) {
      Param& param = p->get();
      if (param.is_integer())    param.set(v.as_int());
      else if (param.is_float()) param.set(v.as_float());
      else if (auto r = params.set_scalar(lv.name, format_math(v)); not r) fail(r.error().message);
      return;
    }
    // A new parameter takes the type of the value, as in zsh.
    if (auto r = params.set_scalar(lv.name, {}); not r) { fail(r.error().message); return; }
    Param& param = params.lookup(lv.name)->get();
    if (v.is_float) param.set(v.d);
    else            param.set(v.i);
  }

  void store_element(const Val& lv, MathNum v) {
    std::string text = format_math(v);
    auto p = params.lookup(lv.name);
    if (p and p->get().is_hash()) {
      auto h = p->get().hash();
      h[lv.sub] = std::move(text);
      p->get().set(std::move(h));
      return;
    }
    long long i = eval_sub(lv.sub, lv.name).as_int();
    if (failed()) return;
    std::vector<std::string> arr;
    if (p and p->get().is_set())
      arr = p->get().is_array() ? p->get().array() : std::vector<std::string>{p->get().scalar()};
    long long slot = array_slot(i, static_cast<long long>(arr.size()));
    if (slot < 0) { fail(lv.name + ": assignment to invalid subscript range"); return; }
    if (slot >= static_cast<long long>(arr.size())) arr.resize(static_cast<std::size_t>(slot) + 1);
    arr[static_cast<std::size_t>(slot)] = std::move(text);
    if (auto r = params.set_array(lv.name, std::move(arr)); not r) fail(r.error().message);
  }

  // ---------------------------------------------------------------------
  // Operands.
  // ---------------------------------------------------------------------

  // Digits valid in `base`, with `_` separators, from pos; they are
  // appended to `digits` without the separators.
  void read_digits(int base, std::string& digits) {
    while (pos < s.size() and (s[pos] == '_' or digit_value(s[pos]) < base)) {
      if (s[pos] != '_') digits += s[pos];
      ++pos;
    }
  }

  MathNum to_integer(const std::string& digits, int base) {
    long long v = 0;
    for (char c : digits) v = wadd(wmul(v, base), digit_value(c));
    if (opts.force_float) return MathNum::of(static_cast<double>(v));
    return MathNum::of(v);
  }

  MathNum number() {
    std::string digits;
    if (s.substr(pos, 2) == "0x" or s.substr(pos, 2) == "0X") {
      pos += 2;
      read_digits(16, digits);
      return to_integer(digits, 16);
    }
    if (s.substr(pos, 2) == "0b" or s.substr(pos, 2) == "0B") {
      pos += 2;
      read_digits(2, digits);
      return to_integer(digits, 2);
    }
    std::size_t start = pos;
    read_digits(10, digits);
    if (pos < s.size() and s[pos] == '#' and not digits.empty()) {
      int base = digits.size() > 2 ? 99 : std::stoi(digits);
      if (base < 2 or base > 36) {
        fail("invalid base (must be 2 to 36 inclusive): " + digits);
        return {};
      }
      ++pos;
      digits.clear();
      read_digits(base, digits);
      return to_integer(digits, base);
    }
    bool fraction = pos < s.size() and s[pos] == '.';
    bool exponent = false;
    if (fraction) {
      ++pos;
      digits += '.';
      read_digits(10, digits);
    }
    if (pos < s.size() and (s[pos] == 'e' or s[pos] == 'E')) {
      std::size_t e = pos + 1;
      if (e < s.size() and (s[e] == '+' or s[e] == '-')) ++e;
      if (e < s.size() and std::isdigit(static_cast<unsigned char>(s[e]))) {
        digits += s.substr(pos, e - pos);
        pos = e;
        read_digits(10, digits);
        exponent = true;
      }
    }
    if (fraction or exponent) return MathNum::of(std::strtod(digits.c_str(), nullptr));
    if (opts.octal_zeroes and digits.size() > 1 and digits[0] == '0') {
      pos = start;
      digits.clear();
      read_digits(8, digits);
      return to_integer(digits, 8);
    }
    return to_integer(digits, 10);
  }

  // `name[sub]`: the text between the brackets, which may nest.
  bool subscript(std::string& out) {
    int nest = 1;
    std::size_t start = ++pos;
    for (; pos < s.size(); ++pos) {
      if (s[pos] == '[') ++nest;
      else if (s[pos] == ']' and --nest == 0) break;
    }
    if (pos >= s.size()) { bad("']' expected"); return false; }
    out = std::string(s.substr(start, pos - start));
    ++pos;
    return true;
  }

  Val primary(bool live) {
    skip_ws();
    if (pos >= s.size()) { bad("operand expected at end of string"); return {}; }
    char c = s[pos];
    if (c == '(') {
      ++pos;
      Val v = num(comma(live));
      if (not failed() and not eat(")")) bad("')' expected");
      return v;
    }
    if (std::isdigit(static_cast<unsigned char>(c))
        or (c == '.' and pos + 1 < s.size() and std::isdigit(static_cast<unsigned char>(s[pos + 1]))))
      return num(number());
    if (is_ident_start(c)) {
      std::size_t start = pos;
      while (pos < s.size() and is_ident_char(s[pos])) ++pos;
      std::string name(s.substr(start, pos - start));
      auto lower = name;
      for (char& ch : lower) ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
      if (lower == "nan") return num(MathNum::of(std::numeric_limits<double>::quiet_NaN()));
      if (lower == "inf") return num(MathNum::of(std::numeric_limits<double>::infinity()));
      Val v{{}, std::move(name), {}, false};
      if (pos < s.size() and s[pos] == '[') v.indexed = subscript(v.sub);
      return v;
    }
    bad("operand expected at `" + rest() + "'");
    return {};
  }

  // `++`/`--` on an lvalue; returns the new value, or the old one for a
  // postfix operator.
  Val step(Val lv, bool inc, bool postfix, bool live) {
    if (lv.name.empty()) { bad("lvalue required"); return {}; }
    MathNum old = rvalue(lv);
    MathNum nv = old.is_float ? MathNum::of(old.d + (inc ? 1.0 : -1.0))
                              : MathNum::of(inc ? wadd(old.i, 1) : wsub(old.i, 1));
    store(lv, nv, live);
    return num(postfix ? old : nv);
  }

  Val postfix(bool live) {
    Val v = primary(live);
    if (failed()) return {};
    if (at("++") or at("--")) {
      bool inc = s[pos] == '+';
      pos += 2;
      return step(std::move(v), inc, true, live);
    }
    return v;
  }

  Val unary(bool live) {
    if (at("++") or at("--")) {
      bool inc = s[pos] == '+';
      pos += 2;
      return step(unary(live), inc, false, live);
    }
    if (eat("+")) return num(rvalue(unary(live)));
    if (eat("-")) {
      MathNum v = rvalue(unary(live));
      return num(v.is_float ? MathNum::of(-v.d) : MathNum::of(wneg(v.i)));
    }
    if (at("!") and not at("!=")) {
      ++pos;
      return num(rvalue(unary(live)).truthy() ? 0LL : 1LL);
    }
    if (eat("~")) return num(~rvalue(unary(live)).as_int());
    return postfix(live);
  }

  // ---------------------------------------------------------------------
  // Binary operators.
  // ---------------------------------------------------------------------

  MathNum divide(MathNum a, MathNum b, bool mod, bool live) {
    if (a.is_float or b.is_float) {
      double x = a.as_float(), y = b.as_float();
      return MathNum::of(mod ? std::fmod(x, y) : x / y);
    }
    if (b.i == 0) {
      if (live) fail("division by zero");
      return {};
    }
    if (b.i == -1) return MathNum::of(mod ? 0LL : wneg(a.i));  // LLONG_MIN / -1 overflows
    return MathNum::of(mod ? a.i % b.i : a.i / b.i);
  }

  MathNum power(MathNum a, MathNum b) {
    if (a.is_float or b.is_float or b.i < 0)
      return MathNum::of(std::pow(a.as_float(), b.as_float()));
    return MathNum::of(ipow(a.i, b.i));
  }

  // Applies a binary operator other than && || ^^ (the short-circuiting
  // ones are done by the caller).
  MathNum apply(std::string_view op, MathNum a, MathNum b, bool live) {
    bool fl = a.is_float or b.is_float;
    auto cmp = [&](auto f) {
      return MathNum::of(static_cast<long long>(fl ? f(a.as_float(), b.as_float()) : f(a.i, b.i)));
    };
    if (op == "+")  return fl ? MathNum::of(a.as_float() + b.as_float()) : MathNum::of(wadd(a.i, b.i));
    if (op == "-")  return fl ? MathNum::of(a.as_float() - b.as_float()) : MathNum::of(wsub(a.i, b.i));
    if (op == "*")  return fl ? MathNum::of(a.as_float() * b.as_float()) : MathNum::of(wmul(a.i, b.i));
    if (op == "/")  return divide(a, b, false, live);
    if (op == "%")  return divide(a, b, true, live);
    if (op == "**") return power(a, b);
    if (op == "<")  return cmp([](auto x, auto y) { return x < y; });
    if (op == "<=") return cmp([](auto x, auto y) { return x <= y; });
    if (op == ">")  return cmp([](auto x, auto y) { return x > y; });
    if (op == ">=") return cmp([](auto x, auto y) { return x >= y; });
    if (op == "==") return cmp([](auto x, auto y) { return x == y; });
    if (op == "!=") return cmp([](auto x, auto y) { return x != y; });
    long long x = a.as_int(), y = b.as_int();
    if (op == "<<") return MathNum::of(wshl(x, y));
    if (op == ">>") return MathNum::of(wshr(x, y));
    if (op == "&")  return MathNum::of(x & y);
    if (op == "^")  return MathNum::of(x ^ y);
    if (op == "|")  return MathNum::of(x | y);
    if (op == "&&") return MathNum::of(static_cast<long long>(a.truthy() and b.truthy()));
    if (op == "||") return MathNum::of(static_cast<long long>(a.truthy() or b.truthy()));
    return MathNum::of(static_cast<long long>(a.truthy() != b.truthy()));  // ^^
  }

  // The binary operator at pos, unless it starts an assignment operator.
  const BinOp* peek_binop() {
    skip_ws();
    for (auto op : ASSIGN_OPS)
      if (s.substr(pos, op.size()) == op) return nullptr;
    for (const auto& op : BIN_OPS)
      if (s.substr(pos, op.text.size()) == op.text) return &op;
    return nullptr;
  }

  int prec(const BinOp& op) const noexcept {
    return opts.c_precedences ? op.c_prec : op.zsh_prec;
  }

  Val binary(int min_prec, bool live) {
    Val l = unary(live);
    for (;;) {
      if (failed()) return {};
      const BinOp* op = peek_binop();
      if (not op or prec(*op) < min_prec) return l;
      pos += op->text.size();
      MathNum a = rvalue(l);
      bool rhs_live = live;
      if (op->text == "&&") rhs_live = live and a.truthy();
      if (op->text == "||") rhs_live = live and not a.truthy();
      // `**` is right-associative; the others are left-associative.
      int next = op->text == "**" ? prec(*op) : prec(*op) + 1;
      MathNum b = rvalue(binary(next, rhs_live));
      if (failed()) return {};
      l = num(apply(op->text, a, b, live));
    }
  }

  Val ternary(bool live) {
    Val c = binary(0, live);
    if (failed() or not eat("?")) return c;
    bool cv = rvalue(c).truthy();
    MathNum a = rvalue(assign(live and cv));
    if (failed()) return {};
    if (not eat(":")) { bad("':' expected"); return {}; }
    MathNum b = rvalue(ternary(live and not cv));
    return num(cv ? a : b);
  }

  Val assign(bool live) {
    Val l = ternary(live);
    if (failed()) return {};
    skip_ws();
    for (auto op : ASSIGN_OPS) {
      if (not eat(op)) continue;
      if (l.name.empty()) { bad("lvalue required"); return {}; }
      std::string_view bop = op.substr(0, op.size() - 1);
      MathNum a = rvalue(l);
      // `&&=` and `||=` only evaluate the right side when it matters.
      bool rhs_live = live;
      if (bop == "&&") rhs_live = live and a.truthy();
      if (bop == "||") rhs_live = live and not a.truthy();
      MathNum r = rvalue(assign(rhs_live));
      if (failed()) return {};
      MathNum v = apply(bop, a, r, live);
      store(l, v, live);
      return num(v);
    }
    if (at("=") and not at("==")) {
      ++pos;
      if (l.name.empty()) { bad("lvalue required"); return {}; }
      MathNum r = rvalue(assign(live));
      store(l, r, live);
      return num(r);
    }
    return l;
  }

  MathNum comma(bool live) {
    MathNum v = rvalue(assign(live));
    while (not failed() and eat(",")) v = rvalue(assign(live));
    return v;
  }

  MathNum run() {
    skip_ws();
    if (pos >= s.size()) return {};
    MathNum v = comma(true);
    skip_ws();
    if (not failed() and pos < s.size()) bad("operator expected at `" + rest() + "'");
    return v;
  }
};

}  // namespace

Result<MathNum> math_eval(std::string_view expr, ParamTable& params, MathOptions opts) {
  Arith a{expr, params, opts, 0, 0, {}};
  MathNum v = a.run();
  if (a.failed())
    return std::unexpected(ShellError{ErrorKind::BadSubstitution, a.err});
  return v;
}

Result<long long> arith_eval(std::string_view expr, ParamTable& params, MathOptions opts) {
  auto v = math_eval(expr, params, opts);
  if (not v) return std::unexpected(v.error());
  return v->as_int();
}

}  // namespace jfsh
