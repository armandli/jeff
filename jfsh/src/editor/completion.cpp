#include <jfsh/editor/completion.h>

#include <cstring>
#include <dirent.h>
#include <pwd.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/core/utf8.h>
#include <jfsh/editor/editor.h>
#include <jfsh/glob/glob.h>

namespace jfsh {

namespace {

bool is_ident(strv s) {
  if (s.empty() or s::isdigit(static_cast<unsigned char>(s[0]))) return false;
  for (char c : s)
    if (not (s::isalnum(static_cast<unsigned char>(c)) or c == '_')) return false;
  return true;
}

// Words after which the next word is a command again.
bool keeps_command_position(strv w) {
  static const uset<str> words = {
    "if", "then", "else", "elif", "do", "while", "until", "!", "{", "time",
    "exec", "noglob", "nocorrect", "command", "builtin", "-", "sudo", "doas",
    "nohup", "nice", "env", "xargs", "coproc"};
  return words.contains(str(w));
}

str home_of(const ParamTable& params, strv user) {
  if (user.empty()) {
    auto p = params.lookup("HOME");
    return p and p->get().is_set() ? p->get().scalar() : str{};
  }
  struct passwd* pw = ::getpwnam(str(user).c_str());
  return pw ? str(pw->pw_dir) : str{};
}

}  // namespace

CompContext CompletionEngine::build_context(strv line, s::size_t cursor) const {
  CompContext ctx;
  cursor = s::min(cursor, line.size());
  ctx.line   = str(line);
  ctx.cursor = cursor;

  vec<str> args;
  bool cmd_pos = true, redir = false, in_word = false;
  char quote = 0;
  str cur;
  s::size_t word_start = cursor;

  auto end_word = [&] {
    if (not in_word) return;
    if (redir) {
      redir = false;   // a redirection's target isn't an argument
    } else if (cmd_pos) {
      auto eq = cur.find('=');
      bool assignment = eq != str::npos and is_ident(strv(cur).substr(0, eq));
      if (keeps_command_position(cur)) args.clear();
      else if (not assignment) { args.push_back(cur); cmd_pos = false; }
    } else {
      args.push_back(cur);
    }
    cur.clear();
    in_word = false;
  };
  auto new_command = [&] {
    in_word = false;
    cur.clear();
    args.clear();
    cmd_pos = true;
    redir = false;
  };
  auto start_word = [&](s::size_t at) {
    if (not in_word) {
      in_word = true;
      word_start = at;
    }
  };

  s::size_t i = 0;
  while (i < cursor) {
    char c = line[i];
    if (quote == '\'') {
      if (c == '\'') quote = 0;
      else cur += c;
      ++i;
      continue;
    }
    if (quote == '"') {
      if (c == '"') quote = 0;
      else if (c == '\\' and i + 1 < cursor and strchr("\"\\$`", line[i + 1])) { cur += line[i + 1]; ++i; }
      else cur += c;
      ++i;
      continue;
    }
    if (c == '\\') {
      start_word(i);
      if (i + 1 < cursor) cur += line[i + 1];
      i += 2;
      continue;
    }
    if (c == '\'' or c == '"') {
      start_word(i);
      quote = c;
      ++i;
      continue;
    }
    if (c == ' ' or c == '\t') {
      end_word();
      ++i;
      continue;
    }
    if (c == '(' and in_word and not cur.empty() and cur.back() == '$') {   // $( … )
      new_command();
      ++i;
      continue;
    }
    if (c == '\n' or c == ';' or c == '&' or c == '|' or c == '(' or c == ')' or c == '`') {
      new_command();
      ++i;
      continue;
    }
    if (c == '<' or c == '>') {
      if (in_word and not cur.empty() and cur.find_first_not_of("0123456789") == str::npos) {
        cur.clear();
        in_word = false;
      } else {
        end_word();
      }
      while (i < cursor and strchr("<>&|!", line[i])) ++i;
      if (i < cursor and line[i] == '(') {   // <( … )
        new_command();
        ++i;
        continue;
      }
      redir = true;
      continue;
    }
    start_word(i);
    cur += c;
    ++i;
  }
  if (not in_word) word_start = cursor;

  // The rest of the word, after the cursor.
  s::size_t end = cursor;
  char q = quote;
  while (end < line.size()) {
    char c = line[end];
    if (q) {
      if (c == q) q = 0;
      ++end;
      continue;
    }
    if (c == ' ' or c == '\t' or c == '\n' or strchr(";&|<>()", c)) break;
    if (c == '\'' or c == '"') q = c;
    if (c == '\\') ++end;
    ++end;
  }
  ctx.word_start = word_start;
  ctx.word_end   = s::min(end, line.size());
  ctx.word       = str(line.substr(word_start, cursor - word_start));
  ctx.prefix     = cur;
  ctx.suffix     = str(line.substr(cursor, ctx.word_end - cursor));
  ctx.quote      = quote;
  ctx.redirection      = redir;
  ctx.command_position = cmd_pos and not redir;
  ctx.args        = args;
  ctx.current_arg = static_cast<int>(args.size());
  ctx.args.push_back(cur);
  ctx.command     = args.empty() ? str{} : args[0];
  return ctx;
}

str CompletionEngine::quote_word(strv w, char quote) {
  str out;
  for (s::size_t i = 0; i < w.size(); ++i) {
    char c = w[i];
    if (quote == '\'') {
      if (c == '\'') out += "'\\''";
      else           out += c;
    } else if (quote == '"') {
      if (strchr("\"\\$`", c)) out += '\\';
      out += c;
    } else {
      if (strchr(" \t\\'\"$&;|()<>*?[]!{}`^", c) or (i == 0 and strchr("~#=", c))) out += '\\';
      if (c == '\n') out += "$'\\n'";
      else           out += c;
    }
  }
  return out;
}

void CompletionEngine::tidy(vec<CompCandidate>& v) {
  s::erase_if(v, [](const CompCandidate& c) { return c.word.empty(); });
  s::stable_sort(v.begin(), v.end(), [](const CompCandidate& a, const CompCandidate& b) {
    return a.word < b.word;
  });
  v.erase(s::unique(v.begin(), v.end(),
                    [](const CompCandidate& a, const CompCandidate& b) { return a.word == b.word; }),
          v.end());
}

vec<CompCandidate> CompletionEngine::complete_files(strv prefix, bool dirs_only, bool exec_only,
                                                    strv pattern) const {
  vec<CompCandidate> out;
  auto slash = prefix.rfind('/');
  str dir_part = slash == strv::npos ? str{} : str(prefix.substr(0, slash + 1));
  str name     = str(slash == strv::npos ? prefix : prefix.substr(slash + 1));
  str dir = dir_part.empty() ? str(".") : dir_part;
  if (dir.starts_with('~')) {
    auto end = dir.find('/');
    str home = home_of(mParams, strv(dir).substr(1, end == str::npos ? str::npos : end - 1));
    if (not home.empty()) dir = home + (end == str::npos ? str{} : dir.substr(end));
  }
  opt<CompiledPattern> pat;
  if (not pattern.empty()) pat = CompiledPattern::compile(pattern);
  DIR* d = ::opendir(dir.c_str());
  if (not d) return out;
  while (struct dirent* ent = ::readdir(d)) {
    strv n(ent->d_name);
    if (n == "." or n == "..") continue;
    if (not n.starts_with(name)) continue;
    if (n.starts_with('.') and not name.starts_with('.')) continue;
    str full = (dir.ends_with('/') ? dir : dir + "/") + str(n);
    struct stat st{}, lst{};
    bool ok   = ::stat(full.c_str(), &st) == 0;
    bool link = ::lstat(full.c_str(), &lst) == 0 and S_ISLNK(lst.st_mode);
    bool is_dir = ok and S_ISDIR(st.st_mode);
    bool is_exe = ok and S_ISREG(st.st_mode) and ::access(full.c_str(), X_OK) == 0;
    if (dirs_only and not is_dir) continue;
    if (exec_only and not is_dir and not is_exe) continue;
    if (pat and not is_dir and not pat->matches(n)) continue;
    CompCandidate c;
    c.word   = str(n);
    c.suffix = is_dir ? "/" : " ";
    c.type   = is_dir ? '/' : link ? '@' : is_exe ? '*'
             : (ok and S_ISFIFO(st.st_mode)) ? '|' : (ok and S_ISSOCK(st.st_mode)) ? '=' : 0;
    out.push_back(s::move(c));
  }
  ::closedir(d);
  tidy(out);
  return out;
}

vec<CompCandidate> CompletionEngine::complete_commands(strv prefix) {
  vec<CompCandidate> out;
  auto add = [&](const str& n) {
    if (not strv(n).starts_with(prefix)) return;
    CompCandidate c;
    c.word = n;
    out.push_back(s::move(c));
  };
  if (mSources.command_names)
    for (const auto& n : mSources.command_names()) add(n);

  // B52: executables only, each name once, sorted.
  auto p = mParams.lookup("PATH");
  str path = p and p->get().is_set() ? p->get().scalar() : str{};
  if (path != mPathCacheKey) {
    mPathCache.clear();
    mPathCacheKey = path;
  }
  vec<str> dirs;
  for (s::size_t at = 0; at <= path.size();) {
    auto colon = path.find(':', at);
    str dir = path.substr(at, colon == str::npos ? str::npos : colon - at);
    dirs.push_back(dir.empty() ? "." : dir);
    if (colon == str::npos) break;
    at = colon + 1;
  }
  if (mPathCache.size() != dirs.size()) {
    mPathCache.assign(dirs.size(), DirCache{});
  }
  for (s::size_t k = 0; k < dirs.size(); ++k) {
    DirCache& dc = mPathCache[k];
    struct stat st{};
    if (::stat(dirs[k].c_str(), &st) != 0) continue;
    if (dc.dir != dirs[k] or dc.mtime != st.st_mtime) {
      dc.dir   = dirs[k];
      dc.mtime = st.st_mtime;
      dc.names.clear();
      if (DIR* d = ::opendir(dirs[k].c_str())) {
        while (struct dirent* ent = ::readdir(d)) {
          strv n(ent->d_name);
          if (n == "." or n == "..") continue;
          str full = dirs[k] + "/" + str(n);
          struct stat fst{};
          if (::stat(full.c_str(), &fst) == 0 and S_ISREG(fst.st_mode)
              and ::access(full.c_str(), X_OK) == 0)
            dc.names.emplace_back(n);
        }
        ::closedir(d);
      }
    }
    for (const auto& n : dc.names) add(n);
  }
  tidy(out);
  return out;
}

vec<CompCandidate> CompletionEngine::complete_parameters(strv prefix) const {
  vec<CompCandidate> out;
  mParams.for_each([&](const Param& p) {
    if (not p.is_set() or not is_ident(p.name) or not strv(p.name).starts_with(prefix)) return;
    CompCandidate c;
    c.word   = p.name;
    c.suffix = "";
    out.push_back(s::move(c));
  });
  tidy(out);
  return out;
}

vec<CompCandidate> CompletionEngine::complete_users(strv prefix) const {
  vec<CompCandidate> out;
  ::setpwent();
  while (struct passwd* pw = ::getpwent()) {
    strv n(pw->pw_name);
    if (n.starts_with('_') and not prefix.starts_with('_')) continue;
    if (not n.starts_with(prefix)) continue;
    CompCandidate c;
    c.word   = str(n);
    c.suffix = "/";
    out.push_back(s::move(c));
  }
  ::endpwent();
  tidy(out);
  return out;
}

CompletionResult CompletionEngine::complete(strv line, s::size_t cursor) {
  CompContext ctx = build_context(line, cursor);
  CompletionResult r;
  r.end   = ctx.cursor;
  r.quote = ctx.quote;
  strv raw = ctx.word;

  // $name and ${name
  if (ctx.quote != '\'') {
    s::size_t k = raw.size();
    while (k > 0 and (s::isalnum(static_cast<unsigned char>(raw[k - 1])) or raw[k - 1] == '_')) --k;
    bool brace = k > 0 and raw[k - 1] == '{';
    s::size_t dollar = brace ? k - 1 : k;
    if (dollar > 0 and raw[dollar - 1] == '$' and (dollar < 2 or raw[dollar - 2] != '\\')) {
      r.start  = ctx.word_start + k;
      r.prefix = str(raw.substr(k));
      r.quote  = 0;
      r.matches = complete_parameters(r.prefix);
      for (auto& m : r.matches) m.suffix = brace ? "}" : "";
      return r;
    }
  }

  // Where the generated part starts: after the last /, or after the
  // quote that opened in the last component.
  auto component_start = [&] {
    s::size_t at = raw.rfind('/');
    at = at == strv::npos ? 0 : at + 1;
    if (ctx.quote) {
      for (s::size_t i = raw.size(); i-- > at;)
        if (raw[i] == ctx.quote) { at = i + 1; break; }
    }
    return ctx.word_start + at;
  };
  str name = ctx.prefix.substr(ctx.prefix.rfind('/') == str::npos ? 0 : ctx.prefix.rfind('/') + 1);

  // A compdef'd command's arguments.
  if (not ctx.command_position and not ctx.redirection and not ctx.command.empty()) {
    vec<CompCandidate> found;
    bool handled = false;
    if (auto it = mBuiltinDefs.find(ctx.command); it != mBuiltinDefs.end()) {
      found = it->second(ctx);
      handled = true;
    } else if (auto c = mCompdefs.find(ctx.command); c != mCompdefs.end() and mSources.call_function) {
      mCollect = &found;
      mCurrent = &ctx;
      mSources.call_function(c->second, ctx);
      mCollect = nullptr;
      mCurrent = nullptr;
      handled = true;
    }
    if (handled) {
      // Matches whose words carry a directory part replace the whole
      // word; the others just the last component.
      bool whole = s::any_of(found.begin(), found.end(),
                             [&](const CompCandidate& m) { return m.word.find('/') != str::npos; });
      r.start  = whole ? ctx.word_start + (ctx.quote and not raw.empty() and raw[0] == ctx.quote ? 1 : 0)
                       : component_start();
      r.prefix = whole ? ctx.prefix : name;
      s::erase_if(found, [&](const CompCandidate& m) { return not strv(m.word).starts_with(r.prefix); });
      tidy(found);
      r.matches = s::move(found);
      return r;
    }
  }

  // ~user
  if (ctx.prefix.starts_with('~') and ctx.prefix.find('/') == str::npos and not ctx.quote) {
    r.start  = ctx.word_start + 1;
    r.prefix = ctx.prefix.substr(1);
    r.matches = complete_users(r.prefix);
    return r;
  }

  r.start  = component_start();
  r.prefix = name;
  if (ctx.command_position) {
    auto eq = ctx.prefix.find('=');
    if (eq != str::npos and is_ident(strv(ctx.prefix).substr(0, eq))) {
      // NAME=value: the value is a file.
      str value = ctx.prefix.substr(eq + 1);
      if (value.find('/') == str::npos) {
        auto req = raw.find('=');
        r.start  = ctx.word_start + (req == strv::npos ? 0 : req + 1);
        r.prefix = value;
      }
      r.matches = complete_files(value);
    } else if (ctx.prefix.find('/') != str::npos) {
      r.matches = complete_files(ctx.prefix, false, true);
    } else {
      r.matches = complete_commands(ctx.prefix);
    }
    return r;
  }
  static const uset<str> dir_commands = {"cd", "pushd", "rmdir", "chdir"};
  bool dirs = not ctx.redirection and dir_commands.contains(ctx.command);
  r.matches = complete_files(ctx.prefix, dirs);
  return r;
}

// ---- The editor's side ------------------------------------------------------------

namespace {

str common_prefix(const vec<CompCandidate>& m) {
  if (m.empty()) return {};
  str p = m[0].word;
  for (const auto& c : m) {
    s::size_t k = 0;
    while (k < p.size() and k < c.word.size() and p[k] == c.word[k]) ++k;
    p.resize(k);
  }
  // Not in the middle of a UTF-8 character.
  while (not p.empty() and (static_cast<unsigned char>(p.back()) & 0xc0) == 0x80) p.pop_back();
  if (not p.empty() and static_cast<unsigned char>(p.back()) >= 0xc0) p.pop_back();
  return p;
}

}  // namespace

void Editor::apply_match(const CompletionResult& r, s::size_t index, bool final) {
  const CompCandidate& m = r.matches[index];
  str text = CompletionEngine::quote_word(m.word, r.quote);
  if (final) {
    if (r.quote and m.suffix != "/") text += r.quote;
    text += m.suffix;
  }
  // The rest of the word after the cursor goes when the match replaces it.
  s::size_t end = r.end;
  if (final and r.quote and end < mBuf.text.size() and mBuf.text[end] == r.quote
      and m.suffix != "/")
    ++end;
  mBuf.text.replace(r.start, end - r.start, text);
  mBuf.cursor = r.start + text.size();
}

void Editor::list_matches(const CompletionResult& r) {
  bool types = mOpts.get(OPT_LISTTYPES);
  vec<str> items;
  int width = 0;
  for (const auto& m : r.matches) {
    str shown = m.display.empty() ? m.word : m.display;
    if (types and m.type) shown += m.type;
    if (not m.description.empty()) shown += "  -- " + m.description;
    width = s::max(width, text_width(shown));
    items.push_back(s::move(shown));
  }
  int cols  = s::max(terminal_width(), 2);
  int colw  = width + 2;
  int ncols = s::max(1, s::min(static_cast<int>(items.size()), (cols + 1) / s::max(colw, 1)));
  int n     = static_cast<int>(items.size());
  int nrows = (n + ncols - 1) / ncols;
  bool rows_first = mOpts.get(OPT_LISTROWSFIRST);

  // LISTMAX: how many matches to show without asking.
  str lm = mZle.param("LISTMAX");
  int listmax = lm.empty() ? 100 : s::atoi(lm.c_str());
  int height  = terminal_height();
  bool ask = (listmax > 0 and n > listmax) or (listmax == 0 and nrows + 1 >= height)
          or (listmax < 0 and nrows > -listmax);
  if (ask) {
    mMessage = "jfsh: do you wish to see all " + s::to_string(n) + " possibilities ("
             + s::to_string(nrows) + " lines)? ";
    refresh();
    int c = read_byte(-1);
    mMessage.clear();
    if (c != 'y' and c != 'Y' and c != ' ' and c != '\t') {
      refresh();
      return;
    }
  }

  str rows;
  for (int row = 0; row < nrows; ++row) {
    str line;
    for (int col = 0; col < ncols; ++col) {
      int idx = rows_first ? row * ncols + col : col * nrows + row;
      if (idx >= n) continue;
      const str& it = items[static_cast<s::size_t>(idx)];
      line += it;
      bool last = rows_first ? (col + 1 >= ncols or idx + 1 >= n) : (idx + nrows >= n);
      if (not last) line += str(static_cast<s::size_t>(colw - text_width(it)), ' ');
    }
    if (row > 0) rows += '\n';
    rows += line;
  }
  if (nrows + 2 >= height) {
    // Too tall to keep below the line: print it, and the prompt after.
    trash();
    str out;
    for (char c : rows) out += c == '\n' ? str("\r\n") : str(1, c);
    write_out(out + "\r\n");
    mListing.clear();
  } else {
    show_listing(s::move(rows));
  }
  mListed = true;
}

bool Editor::do_complete(bool list_only, bool menu, bool reverse) {
  if (not mZle.completion) return false;
  bool completing = mLastWidget == "expand-or-complete" or mLastWidget == "complete-word"
                 or mLastWidget == "menu-complete" or mLastWidget == "reverse-menu-complete"
                 or mLastWidget == "expand-or-complete-prefix" or mLastWidget == "menu-expand-or-complete";
  // Going round the menu.
  if (mMenuActive and not list_only and completing and not mMenu.empty()) {
    s::size_t n = mMenu.size();
    mMenuIndex = reverse ? (mMenuIndex + n - 1) % n : (mMenuIndex + 1) % n;
    const str& text = mMenu[mMenuIndex];
    mBuf.text.replace(mMenuStart, mMenuEnd - mMenuStart, text);
    mMenuEnd = mMenuStart + text.size();
    mBuf.cursor = mMenuEnd;
    return true;
  }
  s::size_t cursor = mBuf.cursor;
  if (not mOpts.get(OPT_COMPLETEINWORD)) {
    // The whole word is completed, from its end.
    auto ctx = mZle.completion->build_context(mBuf.text, cursor);
    cursor = ctx.word_end;
  }
  bool again = completing and mCompleteLine == mBuf.text;
  CompletionResult r = mZle.completion->complete(mBuf.text, cursor);
  if (r.matches.empty()) return false;
  if (list_only) {
    list_matches(r);
    return true;
  }
  if (r.matches.size() == 1) {
    apply_match(r, 0, true);
    mListing.clear();
    return true;
  }
  str lcp = common_prefix(r.matches);
  bool inserted = false;
  if (lcp.size() > r.prefix.size()) {
    str text = CompletionEngine::quote_word(lcp, r.quote);
    mBuf.text.replace(r.start, r.end - r.start, text);
    mBuf.cursor = r.start + text.size();
    r.end = mBuf.cursor;
    inserted = true;
  }
  bool start_menu = menu or mOpts.get(OPT_MENUCOMPLETE)
                 or (again and not inserted and mOpts.get(OPT_AUTOMENU));
  if (start_menu) {
    mMenu.clear();
    for (const auto& m : r.matches)
      mMenu.push_back(CompletionEngine::quote_word(m.word, r.quote)
                      + (m.suffix == "/" or m.suffix == "}" ? m.suffix : str{}));
    mMenuIndex  = reverse ? mMenu.size() - 1 : 0;
    mMenuStart  = r.start;
    mBuf.text.replace(r.start, r.end - r.start, mMenu[mMenuIndex]);
    mMenuEnd    = r.start + mMenu[mMenuIndex].size();
    mBuf.cursor = mMenuEnd;
    mMenuActive = true;
    if (not mListed and mOpts.get(OPT_AUTOLIST)) list_matches(r);
    return true;
  }
  if (mOpts.get(OPT_AUTOLIST) and (not inserted or not mOpts.get(OPT_LISTAMBIGUOUS))) {
    list_matches(r);
  } else if (not inserted) {
    beep();
  }
  // A Tab that added nothing makes the next one start the menu.
  mCompleteLine = inserted ? str{} : mBuf.text;
  return true;
}

bool Editor::w_expand_or_complete()     { return do_complete(false, false); }
bool Editor::w_complete_word()          { return do_complete(false, false); }
bool Editor::w_menu_complete()          { return do_complete(false, true); }
bool Editor::w_reverse_menu_complete()  { return do_complete(false, true, true); }
bool Editor::w_list_choices()           { return do_complete(true, false); }

}  // namespace jfsh
