#include <jfsh/builtins/util.h>

#include <cctype>
#include <cstdio>

#include <algorithm>

#include <jfsh/core/aliases.h>

namespace jfsh {

int builtin_error(strv cmd, strv msg) {
  s::fprintf(stderr, "jfsh: %.*s: %.*s\n", static_cast<int>(cmd.size()), cmd.data(),
             static_cast<int>(msg.size()), msg.data());
  return 1;
}

bool is_identifier(strv s) noexcept {
  if (s.empty() or s::isdigit(static_cast<unsigned char>(s[0]))) return false;
  for (char c : s)
    if (not s::isalnum(static_cast<unsigned char>(c)) and c != '_') return false;
  return true;
}

str shell_quote(strv s) {
  if (s.empty()) return "''";
  bool control = false;
  bool special = false;
  for (char ch : s) {
    auto c = static_cast<unsigned char>(ch);
    if (c < 0x20 or c == 0x7f) control = true;
    else if (strv(" \t'\"\\$`|&;<>()*?[]#~={}^").find(static_cast<char>(c)) != strv::npos)
      special = true;
  }
  if (control) {
    str out = "$'";
    for (char ch : s) {
      auto c = static_cast<unsigned char>(ch);
      switch (c) {
        case '\n': out += "\\n";  break;
        case '\t': out += "\\t";  break;
        case '\r': out += "\\r";  break;
        case '\a': out += "\\a";  break;
        case '\b': out += "\\b";  break;
        case '\f': out += "\\f";  break;
        case '\v': out += "\\v";  break;
        case 033:  out += "\\e";  break;
        case '\'': out += "\\'";  break;
        case '\\': out += "\\\\"; break;
        default:
          if (c < 0x20 or c == 0x7f) {
            char buf[8];
            s::snprintf(buf, sizeof buf, "\\%03o", c);
            out += buf;
          } else {
            out += static_cast<char>(c);
          }
      }
    }
    return out + "'";
  }
  if (not special) return str(s);
  str out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else           out += c;
  }
  return out + "'";
}

bool OptParse::has(char c) const noexcept {
  return s::any_of(flags.begin(), flags.end(),
                   [c](const Flag& f) { return f.c == c and f.on; });
}

bool OptParse::has_plus(char c) const noexcept {
  return s::any_of(flags.begin(), flags.end(),
                   [c](const Flag& f) { return f.c == c and not f.on; });
}

const str* OptParse::arg(char c) const noexcept {
  const str* found = nullptr;
  for (const auto& f : flags)
    if (f.c == c) found = &f.arg;
  return found;
}

bool parse_options(const vec<str>& args, strv letters, strv with_arg,
                   OptParse& out, bool plus, bool numbers) {
  strv cmd = args[0];
  s::size_t i = 1;
  for (; i < args.size(); ++i) {
    const str& a = args[i];
    if (a == "--" or a == "-") { ++i; break; }
    if (a.size() < 2 or not (a[0] == '-' or (plus and a[0] == '+'))) break;
    if (numbers and s::isdigit(static_cast<unsigned char>(a[1]))) break;
    bool on = a[0] == '-';
    for (s::size_t k = 1; k < a.size(); ++k) {
      char c = a[k];
      if (letters.find(c) == strv::npos and with_arg.find(c) == strv::npos) {
        s::fprintf(stderr, "jfsh: %.*s: bad option: %c%c\n",
                   static_cast<int>(cmd.size()), cmd.data(), a[0], c);
        return false;
      }
      OptParse::Flag f{c, on, {}};
      if (with_arg.find(c) != strv::npos) {
        if (k + 1 < a.size()) {
          f.arg = a.substr(k + 1);
        } else if (i + 1 < args.size()) {
          f.arg = args[++i];
        } else {
          s::fprintf(stderr, "jfsh: %.*s: argument expected: %c%c\n",
                     static_cast<int>(cmd.size()), cmd.data(), a[0], c);
          return false;
        }
        out.flags.push_back(s::move(f));
        break;
      }
      out.flags.push_back(s::move(f));
    }
  }
  out.next = i;
  return true;
}

str param_value_text(const Param& p) {
  if (p.is_array()) {
    str out = "(";
    for (const auto& e : p.array()) out += ' ' + shell_quote(e);
    return out + " )";
  }
  if (p.is_hash()) {
    auto h = p.hash();
    vec<str> keys;
    for (const auto& [k, v] : h) keys.push_back(k);
    s::sort(keys.begin(), keys.end());
    str out = "(";
    for (const auto& k : keys) out += " [" + shell_quote(k) + "]=" + shell_quote(h[k]);
    return out + " )";
  }
  if (p.is_integer()) return s::to_string(p.integer());
  if (p.is_float()) return p.scalar();
  return shell_quote(p.scalar());
}

str param_type_flags(const Param& p) {
  str f;
  if (p.is_array()) f += 'a';
  if (p.is_hash()) f += 'A';
  if (p.is_integer()) f += 'i' + (p.base > 0 ? s::to_string(p.base) : str{});
  if (p.is_float()) f += (p.flags & PM_EFLOAT) ? 'E' : 'F';
  if (p.flags & PM_LOWER) f += 'l';
  if (p.is_readonly()) f += 'r';
  if (p.flags & PM_UPPER) f += 'u';
  if (p.flags & PM_UNIQUE) f += 'U';
  if (p.is_exported()) f += 'x';
  return f.empty() ? f : "-" + f;
}

vec<const Param*> sorted_params(const ParamTable& params,
                                const func<bool(const Param&)>& pred) {
  vec<const Param*> out;
  params.for_each([&](const Param& p) {
    if (pred(p)) out.push_back(&p);
  });
  s::sort(out.begin(), out.end(),
          [](const Param* a, const Param* b) { return a->name < b->name; });
  return out;
}

str home_dir(const ParamTable& params) {
  auto p = params.lookup("HOME");
  return (p and p->get().is_set()) ? p->get().scalar() : str{};
}

str tilde_abbrev(const ParamTable& params, strv dir) {
  str home = home_dir(params);
  while (home.size() > 1 and home.back() == '/') home.pop_back();
  if (home.empty() or home == "/") return str(dir);
  if (dir == home) return "~";
  if (dir.starts_with(home) and dir.size() > home.size() and dir[home.size()] == '/')
    return "~" + str(dir.substr(home.size()));
  return str(dir);
}

}  // namespace jfsh
