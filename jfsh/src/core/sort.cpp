#include <jfsh/core/sort.h>

#include <cctype>
#include <cstring>

namespace jfsh {

namespace {

bool is_digit(char c) { return s::isdigit(static_cast<unsigned char>(c)) != 0; }

}  // namespace

// A port of zsh's eltpcmp: past the common prefix, digit runs (with
// `numeric`) compare by value, the rest by strcoll().
int zstrcmp(strv a, strv b, bool numeric, bool backslashes) {
  str x, y;
  for (char c : a) if (not (backslashes and c == '\\')) x += c;
  for (char c : b) if (not (backslashes and c == '\\')) y += c;
  if (x.find('\0') != str::npos or y.find('\0') != str::npos)
    return x.compare(y);
  const char* as = x.c_str();
  const char* bs = y.c_str();
  if (numeric) {
    const char* ao = as;
    for (; *as == *bs and *as; ++as, ++bs) {}
    if (is_digit(*as) or is_digit(*bs)) {
      for (; as > ao and is_digit(as[-1]); --as, --bs) {}
      if (is_digit(*as) and is_digit(*bs)) {
        while (*as == '0') ++as;
        while (*bs == '0') ++bs;
        for (; is_digit(*as) and *as == *bs; ++as, ++bs) {}
        if (is_digit(*as) or is_digit(*bs)) {
          int cmp = static_cast<unsigned char>(*as) - static_cast<unsigned char>(*bs);
          while (is_digit(*as) and is_digit(*bs)) ++as, ++bs;
          if (is_digit(*as) and not is_digit(*bs)) return 1;
          if (is_digit(*bs) and not is_digit(*as)) return -1;
          return cmp;
        }
      }
    }
  }
  return s::strcoll(as, bs);
}

}  // namespace jfsh
