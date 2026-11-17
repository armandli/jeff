#include <jfsh/history/history.h>

#include <cctype>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/core/aliases.h>

namespace jfsh {

namespace {

// zsh writes its history file metafied: NUL and the bytes 0x83 (Meta)
// through 0xa2 (Marker) become Meta followed by the byte xor 32.
constexpr unsigned char Meta = 0x83, Marker = 0xa2;

str metafy(strv s) {
  str out;
  out.reserve(s.size());
  for (char ch : s) {
    auto c = static_cast<unsigned char>(ch);
    if (c == 0 or (c >= Meta and c <= Marker)) {
      out += static_cast<char>(Meta);
      out += static_cast<char>(c ^ 32);
    } else {
      out += ch;
    }
  }
  return out;
}

str unmetafy(strv s) {
  str out;
  out.reserve(s.size());
  for (s::size_t i = 0; i < s.size(); ++i) {
    if (static_cast<unsigned char>(s[i]) == Meta and i + 1 < s.size())
      out += static_cast<char>(static_cast<unsigned char>(s[++i]) ^ 32);
    else
      out += s[i];
  }
  return out;
}

ShellError hist_error(str msg) {
  return ShellError{ErrorKind::ExecError, s::move(msg)};
}

// An open, locked history file; unlocked and closed when it goes away.
struct LockedFile {
  int fd = -1;
  LockedFile(strv path, bool write, bool lock) {
    fd = ::open(str(path).c_str(), write ? O_RDWR | O_CREAT | O_CLOEXEC : O_RDONLY | O_CLOEXEC, 0600);
    if (fd < 0 or not lock) return;
    struct flock fl{};
    fl.l_type   = static_cast<short>(write ? F_WRLCK : F_RDLCK);
    fl.l_whence = SEEK_SET;
    while (::fcntl(fd, F_SETLKW, &fl) < 0 and errno == EINTR) {}
  }
  ~LockedFile() { if (fd >= 0) ::close(fd); }   // closing drops the lock
  LockedFile(const LockedFile&) = delete;
  LockedFile& operator=(const LockedFile&) = delete;

  str read_from(off_t offset) const {
    str data;
    if (::lseek(fd, offset, SEEK_SET) < 0) return data;
    char buf[8192];
    for (;;) {
      ssize_t n = ::read(fd, buf, sizeof buf);
      if (n < 0 and errno == EINTR) continue;
      if (n <= 0) break;
      data.append(buf, static_cast<s::size_t>(n));
    }
    return data;
  }
  bool write_all(strv data) const {
    while (not data.empty()) {
      ssize_t n = ::write(fd, data.data(), data.size());
      if (n < 0 and errno == EINTR) continue;
      if (n <= 0) return false;
      data.remove_prefix(static_cast<s::size_t>(n));
    }
    return true;
  }
  off_t size() const {
    struct stat st{};
    return ::fstat(fd, &st) == 0 ? st.st_size : 0;
  }
};

bool is_blank(char c) { return c == ' ' or c == '\t' or c == '\n'; }

// The first word of a command line and the rest, for HIST_NO_STORE.
bool is_history_command(strv text) {
  vec<str> w = HistoryManager::split_words(text);
  if (w.empty()) return false;
  if (w[0] == "history") return true;
  if (w[0] != "fc") return false;
  for (s::size_t i = 1; i < w.size(); ++i)
    if (w[i].starts_with('-') and w[i].find('l') != str::npos) return true;
  return false;
}

// :h :t :r :e on a path.
str path_head(const str& p) {
  auto slash = p.rfind('/');
  if (slash == str::npos) return ".";
  if (slash == 0) return "/";
  return p.substr(0, slash);
}
str path_tail(const str& p) {
  auto slash = p.rfind('/');
  return slash == str::npos ? p : p.substr(slash + 1);
}
str path_root(const str& p) {
  auto slash = p.rfind('/');
  auto dot   = p.rfind('.');
  if (dot == str::npos or (slash != str::npos and dot < slash)) return p;
  return p.substr(0, dot);
}
str path_ext(const str& p) {
  auto slash = p.rfind('/');
  auto dot   = p.rfind('.');
  if (dot == str::npos or (slash != str::npos and dot < slash)) return {};
  return p.substr(dot + 1);
}
str single_quote(strv s) {
  str out = "'";
  for (char c : s) {
    if (c == '\'') out += "'\\''";
    else           out += c;
  }
  return out + "'";
}

}  // namespace

vec<str> HistoryManager::split_words(strv text) {
  vec<str> words;
  str cur;
  bool in_word = false;
  auto flush = [&] {
    if (in_word) words.push_back(s::move(cur));
    cur.clear();
    in_word = false;
  };
  for (s::size_t i = 0; i < text.size(); ++i) {
    char c = text[i];
    if (is_blank(c)) { flush(); continue; }
    if (c == '\\' and i + 1 < text.size()) {
      cur += c;
      cur += text[++i];
      in_word = true;
      continue;
    }
    if (c == '\'' or c == '"' or c == '`') {
      auto end = text.find(c, i + 1);
      if (c == '"') {   // backslash-escaped quotes stay inside
        end = i + 1;
        while (end < text.size() and text[end] != '"') end += text[end] == '\\' ? 2 : 1;
        if (end >= text.size()) end = strv::npos;
      }
      if (end == strv::npos) end = text.size() - 1;
      cur += text.substr(i, end - i + 1);
      i = end;
      in_word = true;
      continue;
    }
    if (c == '$' and i + 1 < text.size() and text[i + 1] == '(') {
      int depth = 0;
      s::size_t j = i + 1;
      for (; j < text.size(); ++j) {
        if (text[j] == '(') ++depth;
        else if (text[j] == ')' and --depth == 0) break;
      }
      if (j >= text.size()) j = text.size() - 1;
      cur += text.substr(i, j - i + 1);
      i = j;
      in_word = true;
      continue;
    }
    if (strv(";|&<>()").find(c) != strv::npos) {
      flush();
      str op(1, c);
      // && || ;; >> << >& <& >| &> and digits glued in front stay one word
      if (i + 1 < text.size() and strv("&|;><").find(text[i + 1]) != strv::npos
          and (text[i + 1] == c or c == '>' or c == '<' or c == '&')) {
        op += text[++i];
      }
      words.push_back(s::move(op));
      continue;
    }
    cur += c;
    in_word = true;
  }
  flush();
  return words;
}

bool HistoryManager::should_suppress(strv text) const noexcept {
  if (text.find_first_not_of(" \t\n") == strv::npos) return true;
  if (mOpts.ignore_space and text[0] == ' ') return true;
  // B40: HIST_NO_STORE drops `history` and `fc -l` lines themselves; it
  // no longer skips the duplicate checks.
  if (mOpts.no_store and is_history_command(text)) return true;
  if (mOpts.ignore_dups and not mEntries.empty() and mEntries.back().text == text)
    return true;
  return false;
}

void HistoryManager::trim_to_limit() {
  s::size_t limit = static_cast<s::size_t>(s::max(mOpts.histsize, 1));
  if (mEntries.size() > limit)
    mEntries.erase(mEntries.begin(),
                   mEntries.begin() + static_cast<s::ptrdiff_t>(mEntries.size() - limit));
}

s::uint32_t HistoryManager::add(str text, s::uint32_t elapsed) {
  if (mOpts.reduce_blanks) {
    str trimmed;
    bool last_ws = false;
    for (char c : text) {
      bool ws = c == ' ' or c == '\t';
      if (not ws or not last_ws) trimmed += c;
      last_ws = ws;
    }
    while (not trimmed.empty() and (trimmed.back() == ' ' or trimmed.back() == '\t'))
      trimmed.pop_back();
    text = s::move(trimmed);
  }
  if (should_suppress(text)) return 0;
  // B58: HIST_IGNORE_ALL_DUPS drops the older copy and keeps the new one.
  if (mOpts.ignore_all) {
    s::erase_if(mEntries, [&](const HistEntry& x) { return x.text == text; });
  }
  HistEntry e;
  e.num       = mNextNum++;
  e.text      = s::move(text);
  e.timestamp = s::time(nullptr);
  e.elapsed   = elapsed;
  mEntries.push_back(s::move(e));
  trim_to_limit();
  return mEntries.back().num;
}

Result<HistExpansion> HistoryManager::expand_line(strv line) {
  HistExpansion res;
  if (not mOpts.bang or mOpts.histchars.empty()) {
    res.text = str(line);
    return res;
  }
  char bang = mOpts.histchars[0];
  char hat  = mOpts.histchars.size() > 1 ? mOpts.histchars[1] : '\0';

  auto previous = [&]() -> Result<str> {
    if (mEntries.empty()) return s::unexpected(hist_error("no such event: 0"));
    return mEntries.back().text;
  };
  auto substitute = [&](str& text, const str& from, const str& to, bool global) -> bool {
    if (from.empty()) return false;
    str repl;
    for (s::size_t k = 0; k < to.size(); ++k) {
      if (to[k] == '\\' and k + 1 < to.size() and to[k + 1] == '&') { repl += '&'; ++k; }
      else if (to[k] == '&') repl += from;
      else repl += to[k];
    }
    s::size_t at = text.find(from);
    if (at == str::npos) return false;
    do {
      text.replace(at, from.size(), repl);
      at = text.find(from, at + repl.size());
    } while (global and at != str::npos);
    return true;
  };

  // ^old^new^rest: the previous command with old replaced by new.
  if (hat and not line.empty() and line[0] == hat) {
    s::size_t i = 1;
    str from, to;
    while (i < line.size() and line[i] != hat) {
      if (line[i] == '\\' and i + 1 < line.size() and line[i + 1] == hat) ++i;
      from += line[i++];
    }
    if (i < line.size()) ++i;
    while (i < line.size() and line[i] != hat and line[i] != '\n') {
      if (line[i] == '\\' and i + 1 < line.size() and line[i + 1] == hat) ++i;
      to += line[i++];
    }
    if (i < line.size() and line[i] == hat) ++i;
    auto prev = previous();
    if (not prev) return s::unexpected(prev.error());
    str text = *prev;
    if (from.empty()) from = mLastSubstFrom;
    if (not substitute(text, from, to, false))
      return s::unexpected(hist_error("substitution failed"));
    mLastSubstFrom = from;
    mLastSubstTo   = to;
    res.text    = text + str(line.substr(i));
    res.changed = true;
    return res;
  }

  str out;
  bool squote = false, dquote = false;
  s::size_t i = 0;
  while (i < line.size()) {
    char c = line[i];
    if (c == '\\' and i + 1 < line.size() and not squote) {
      out += c;
      out += line[i + 1];
      i += 2;
      continue;
    }
    if (c == '\'' and not dquote) squote = not squote;
    else if (c == '"' and not squote) dquote = not dquote;
    if (c != bang or squote) {
      out += c;
      ++i;
      continue;
    }
    s::size_t j = i + 1;
    if (j >= line.size() or is_blank(line[j]) or line[j] == '=' or line[j] == '('
        or (dquote and line[j] == '"')) {
      out += c;
      ++i;
      continue;
    }

    // The event.
    bool braces = line[j] == '{';
    if (braces) ++j;
    str event;
    char n = j < line.size() ? line[j] : '\0';
    if (n == bang) {
      auto p = previous();
      if (not p) return s::unexpected(p.error());
      event = *p;
      ++j;
    } else if (n == '#') {
      event = out;
      ++j;
    } else if (isdigit(static_cast<unsigned char>(n))
               or (n == '-' and j + 1 < line.size() and isdigit(static_cast<unsigned char>(line[j + 1])))) {
      s::size_t k = j + 1;
      while (k < line.size() and isdigit(static_cast<unsigned char>(line[k]))) ++k;
      str spec(line.substr(j, k - j));
      long num = s::strtol(spec.c_str(), nullptr, 10);
      auto e = lookup(static_cast<int>(s::clamp(num, static_cast<long>(INT_MIN + 1), static_cast<long>(INT_MAX))));
      if (not e or num == 0) return s::unexpected(hist_error("no such event: " + spec));
      event = e->get().text;
      j = k;
    } else if (n == '?') {
      s::size_t k = j + 1;
      while (k < line.size() and line[k] != '?' and line[k] != '\n') ++k;
      str needle(line.substr(j + 1, k - j - 1));
      if (needle.empty()) needle = mLastSearch;
      auto e = search(needle);
      if (not e or needle.empty()) return s::unexpected(hist_error("event not found: " + needle));
      mLastSearch = needle;
      event = e->get().text;
      j = k < line.size() and line[k] == '?' ? k + 1 : k;
    } else if (n == ':' or n == '^' or n == '$' or n == '*' or n == '%') {
      auto p = previous();
      if (not p) return s::unexpected(p.error());
      event = *p;
    } else {
      s::size_t k = j;
      while (k < line.size() and not is_blank(line[k])
             and strv(":;|&()<>\"'`^$*%}").find(line[k]) == strv::npos)
        ++k;
      if (braces) while (k < line.size() and line[k] != '}' and line[k] != ':') ++k;
      str prefix(line.substr(j, k - j));
      const HistEntry* found = nullptr;
      for (auto it = mEntries.rbegin(); it != mEntries.rend(); ++it)
        if (it->text.starts_with(prefix)) { found = &*it; break; }
      if (not found or prefix.empty())
        return s::unexpected(hist_error("event not found: " + prefix));
      event = found->text;
      j = k;
    }

    // Word designators.
    str selected = event;
    auto designator_char = [](char ch) {
      return isdigit(static_cast<unsigned char>(ch)) or ch == '^' or ch == '$' or ch == '%';
    };
    bool colon = j + 1 < line.size() and line[j] == ':'
                 and (designator_char(line[j + 1]) or line[j + 1] == '*' or line[j + 1] == '-');
    if (colon or (j < line.size() and strv("^$*%").find(line[j]) != strv::npos)) {
      if (colon) ++j;
      vec<str> words = split_words(event);
      long last = static_cast<long>(words.size()) - 1;
      auto index = [&](long& out_idx) -> bool {
        if (j >= line.size()) return false;
        char ch = line[j];
        if (isdigit(static_cast<unsigned char>(ch))) {
          long v = 0;
          while (j < line.size() and isdigit(static_cast<unsigned char>(line[j])))
            v = v * 10 + (line[j++] - '0');
          out_idx = v;
          return true;
        }
        if (ch == '^') { ++j; out_idx = 1; return true; }
        if (ch == '$') { ++j; out_idx = last; return true; }
        if (ch == '%') {
          ++j;
          out_idx = -1;
          for (s::size_t w = 0; w < words.size(); ++w)
            if (not mLastSearch.empty() and words[w].find(mLastSearch) != str::npos) {
              out_idx = static_cast<long>(w);
              break;
            }
          return true;
        }
        return false;
      };
      long from = 0, to = 0;
      bool empty_ok = false;
      if (line[j] == '*') {
        ++j;
        from = 1;
        to = last;
        empty_ok = true;
      } else if (line[j] == '-') {
        ++j;
        from = 0;
        if (not index(to)) to = last - 1;
      } else {
        index(from);
        if (j < line.size() and line[j] == '*') {
          ++j;
          to = last;
          empty_ok = true;
        } else if (j < line.size() and line[j] == '-') {
          ++j;
          if (not (j < line.size() and designator_char(line[j]) and index(to))) to = last - 1;
        } else {
          to = from;
        }
      }
      if (empty_ok and from > to) {
        selected.clear();
      } else {
        if (from < 0 or to > last or from > to)
          return s::unexpected(hist_error("no such word in event"));
        selected.clear();
        for (long w = from; w <= to; ++w) {
          if (w > from) selected += ' ';
          selected += words[static_cast<s::size_t>(w)];
        }
      }
    }

    // Modifiers.
    while (j + 1 < line.size() and line[j] == ':'
           and (s::isalpha(static_cast<unsigned char>(line[j + 1])) or line[j + 1] == '&')) {
      ++j;
      char m = line[j++];
      bool global = false;
      if (m == 'g' and j < line.size() and (line[j] == 's' or line[j] == '&')) {
        global = true;
        m = line[j++];
      }
      switch (m) {
        case 'h': selected = path_head(selected); break;
        case 't': selected = path_tail(selected); break;
        case 'r': selected = path_root(selected); break;
        case 'e': selected = path_ext(selected); break;
        case 'q': selected = single_quote(selected); break;
        case 'x': {
          str q;
          for (const auto& w : split_words(selected)) q += (q.empty() ? "" : " ") + single_quote(w);
          selected = q;
          break;
        }
        case 'p': res.print_only = true; break;
        case 'u': for (char& ch : selected) ch = static_cast<char>(s::toupper(static_cast<unsigned char>(ch))); break;
        case 'l': for (char& ch : selected) ch = static_cast<char>(s::tolower(static_cast<unsigned char>(ch))); break;
        case 'a': case 'A': {
          if (not selected.starts_with('/')) {
            char cwd[4096];
            if (::getcwd(cwd, sizeof cwd)) selected = str(cwd) + "/" + selected;
          }
          if (m == 'A') {
            char real[PATH_MAX];
            if (::realpath(selected.c_str(), real)) selected = real;
          }
          break;
        }
        case 's': {
          if (j >= line.size()) return s::unexpected(hist_error("substitution failed"));
          char delim = line[j++];
          auto part = [&] {
            str p;
            while (j < line.size() and line[j] != delim and line[j] != '\n') {
              if (line[j] == '\\' and j + 1 < line.size() and line[j + 1] == delim) ++j;
              p += line[j++];
            }
            if (j < line.size() and line[j] == delim) ++j;
            return p;
          };
          str from = part();
          str to   = part();
          if (from.empty()) from = mLastSubstFrom;
          mLastSubstFrom = from;
          mLastSubstTo   = to;
          if (not substitute(selected, from, to, global))
            return s::unexpected(hist_error("substitution failed"));
          break;
        }
        case '&':
          if (mLastSubstFrom.empty()) return s::unexpected(hist_error("no previous substitution"));
          if (not substitute(selected, mLastSubstFrom, mLastSubstTo, global))
            return s::unexpected(hist_error("substitution failed"));
          break;
        default:
          return s::unexpected(hist_error(str("illegal modifier: ") + m));
      }
    }
    if (braces and j < line.size() and line[j] == '}') ++j;
    out += selected;
    res.changed = true;
    i = j;
  }
  res.text = s::move(out);
  return res;
}

optref<const HistEntry> HistoryManager::lookup(int num) const noexcept {
  if (num > 0) {
    for (auto const& e : mEntries)
      if (static_cast<int>(e.num) == num) return s::cref(e);
  } else {
    // -1 is the last entry; 0 is none (B6).
    int idx = static_cast<int>(mEntries.size()) + num;
    if (num < 0 and idx >= 0) return s::cref(mEntries[static_cast<s::size_t>(idx)]);
  }
  return s::nullopt;
}

optref<const HistEntry> HistoryManager::search(strv substr) const noexcept {
  for (auto it = mEntries.rbegin(); it != mEntries.rend(); ++it)
    if (it->text.find(substr) != str::npos) return s::cref(*it);
  return s::nullopt;
}

vec<HistEntry> HistoryManager::parse_file(strv data) const {
  vec<HistEntry> out;
  str pending;
  bool continued = false;
  auto finish = [&](str text) {
    HistEntry e;
    e.saved = true;
    // `: start:elapsed;command`
    if (text.starts_with(": ")) {
      s::size_t k = 2;
      s::size_t start_digits = k;
      while (k < text.size() and isdigit(static_cast<unsigned char>(text[k]))) ++k;
      if (k > start_digits and k < text.size() and text[k] == ':') {
        s::time_t ts = static_cast<s::time_t>(s::strtoll(text.c_str() + start_digits, nullptr, 10));
        s::size_t el = ++k;
        while (k < text.size() and isdigit(static_cast<unsigned char>(text[k]))) ++k;
        if (k < text.size() and text[k] == ';') {
          e.timestamp = ts;
          e.elapsed   = static_cast<s::uint32_t>(s::strtoul(text.c_str() + el, nullptr, 10));
          text.erase(0, k + 1);
        }
      }
    }
    if (text.empty()) return;
    e.text = s::move(text);
    out.push_back(s::move(e));
  };
  s::size_t at = 0;
  while (at < data.size()) {
    auto nl = data.find('\n', at);
    strv line = data.substr(at, nl == strv::npos ? strv::npos : nl - at);
    at = nl == strv::npos ? data.size() : nl + 1;
    s::size_t slashes = 0;
    while (slashes < line.size() and line[line.size() - 1 - slashes] == '\\') ++slashes;
    str text = unmetafy(line);
    if (continued) pending += '\n';
    if (slashes % 2 == 1) {   // a multi-line entry goes on
      text.pop_back();
      pending += text;
      continued = true;
      continue;
    }
    pending += text;
    continued = false;
    finish(s::move(pending));
    pending.clear();
  }
  if (continued) finish(s::move(pending));
  return out;
}

str HistoryManager::format_entry(const HistEntry& e) const {
  str line;
  if (mOpts.extended) {
    line = ": " + s::to_string(static_cast<long long>(e.timestamp ? e.timestamp : s::time(nullptr)))
         + ":" + s::to_string(e.elapsed) + ";";
  }
  for (char c : e.text) {
    if (c == '\n') line += "\\\n";
    else           line += c;
  }
  return metafy(line) + "\n";
}

Result<void> HistoryManager::load(strv path) {
  LockedFile f(path, false, mOpts.fcntl_lock);
  if (f.fd < 0) return {};  // file not found is not an error on first run
  str data = f.read_from(0);
  mFileOffset = static_cast<off_t>(data.size());
  for (auto& e : parse_file(data)) {
    e.num = mNextNum++;
    mEntries.push_back(s::move(e));
  }
  trim_to_limit();
  return {};
}

Result<void> HistoryManager::sync(strv path) {
  LockedFile f(path, false, mOpts.fcntl_lock);
  if (f.fd < 0) return {};
  off_t size = f.size();
  if (size < mFileOffset) {   // another shell rewrote it
    mFileOffset = size;
    return {};
  }
  if (size == mFileOffset) return {};
  str data = f.read_from(mFileOffset);
  // Only whole lines: another shell may be half way through a write.
  auto last_nl = data.rfind('\n');
  if (last_nl == str::npos) return {};
  data.resize(last_nl + 1);
  mFileOffset += static_cast<off_t>(data.size());
  for (auto& e : parse_file(data)) {
    e.num = mNextNum++;
    mEntries.push_back(s::move(e));
  }
  trim_to_limit();
  return {};
}

Result<void> HistoryManager::save(strv path, HistSave mode) {
  if (path.empty()) return {};
  if (mode == HistSave::Options and mOpts.savehist <= 0) return {};
  LockedFile f(path, true, mOpts.fcntl_lock);
  if (f.fd < 0)
    return s::unexpected(ShellError{ErrorKind::FileNotFound, "cannot open histfile"});
  bool append = mode == HistSave::Append
             or (mode == HistSave::Options and (mOpts.append or mOpts.inc_append or mOpts.share));

  // The entries to keep when the file is rewritten: the newest SAVEHIST,
  // and with HIST_SAVE_NO_DUPS only the newest copy of each line.
  auto keep = [&](vec<HistEntry> list, bool limit) {
    if (mOpts.save_no_dups) {
      uset<str> seen;
      vec<HistEntry> kept;
      for (auto it = list.rbegin(); it != list.rend(); ++it)
        if (seen.insert(it->text).second) kept.push_back(s::move(*it));
      s::reverse(kept.begin(), kept.end());
      list = s::move(kept);
    }
    s::size_t max = static_cast<s::size_t>(s::max(mOpts.savehist, 0));
    if (limit and list.size() > max)
      list.erase(list.begin(), list.begin() + static_cast<s::ptrdiff_t>(list.size() - max));
    str data;
    for (const auto& e : list) data += format_entry(e);
    return data;
  };

  if (append) {
    // B5: only the entries that aren't in the file yet.  With
    // SHARE_HISTORY, what other shells wrote meanwhile is read first.
    if (mOpts.share and f.size() > mFileOffset) {
      str theirs = f.read_from(mFileOffset);
      auto last_nl = theirs.rfind('\n');
      if (last_nl != str::npos) {
        theirs.resize(last_nl + 1);
        vec<HistEntry> others = parse_file(theirs);
        for (auto& e : others) e.num = mNextNum++;
        // Ours stay newest.
        auto first_new = s::find_if(mEntries.begin(), mEntries.end(),
                                    [](const HistEntry& e) { return not e.saved; });
        mEntries.insert(first_new, others.begin(), others.end());
        trim_to_limit();
      }
    }
    str data;
    for (auto& e : mEntries) {
      if (e.saved) continue;
      data += format_entry(e);
      e.saved = true;
    }
    if (::lseek(f.fd, 0, SEEK_END) < 0 or not f.write_all(data))
      return s::unexpected(ShellError{ErrorKind::ExecError, "cannot write histfile"});
    // On the way out, a file grown past SAVEHIST is cut back to it.
    if (mode == HistSave::Options) {
      vec<HistEntry> all = parse_file(f.read_from(0));
      if (all.size() > static_cast<s::size_t>(mOpts.savehist)) {
        str trimmed = keep(s::move(all), true);
        if (::ftruncate(f.fd, 0) != 0 or ::lseek(f.fd, 0, SEEK_SET) < 0
            or not f.write_all(trimmed))
          return s::unexpected(ShellError{ErrorKind::ExecError, "cannot write histfile"});
      }
    }
  } else {
    str data = keep(mEntries, mode == HistSave::Options);
    if (::ftruncate(f.fd, 0) != 0 or ::lseek(f.fd, 0, SEEK_SET) < 0 or not f.write_all(data))
      return s::unexpected(ShellError{ErrorKind::ExecError, "cannot write histfile"});
    for (auto& e : mEntries) e.saved = true;
  }
  mFileOffset = f.size();
  return {};
}

}  // namespace jfsh
