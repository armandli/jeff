#include <jfsh/editor/completion.h>

#include <cstring>
#include <cwctype>
#include <dirent.h>
#include <pwd.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>

#include <jfsh/core/aliases.h>
#include <jfsh/core/sort.h>
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

// Reserved words that start a command, as zsh's $words sees them.
bool starts_command(strv w) {
  static const uset<str> words = {
    "if", "then", "else", "elif", "do", "while", "until", "!", "{", "time", "coproc",
    "nocorrect"};
  return words.contains(str(w));
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
  // zsh's $words: the words as typed, assignments and redirections left
  // out; and the elements of a `name=(…)` array being assigned.
  vec<str> words, elements;
  str array_param;
  bool cmd_pos = true, redir = false, in_word = false, backtick = false;
  int braces = 0;   // ${ … not closed yet in the word
  char quote = 0;
  str cur, redirect;
  s::size_t word_start = cursor;

  auto end_word = [&](s::size_t at) {
    if (not in_word) return;
    str raw(line.substr(word_start, at - word_start));
    if (not array_param.empty()) {
      elements.push_back(s::move(raw));
    } else if (redir) {
      redir = false;   // a redirection's target isn't an argument
    } else if (cmd_pos) {
      auto eq = cur.find('=');
      bool assignment = eq != str::npos and is_ident(strv(cur).substr(0, eq));
      if (keeps_command_position(cur)) {
        args.clear();
        if (starts_command(cur)) words.clear();
        else                     words.push_back(s::move(raw));
      } else if (not assignment) {
        args.push_back(cur);
        words.push_back(s::move(raw));
        cmd_pos = false;
      }
    } else {
      args.push_back(cur);
      words.push_back(s::move(raw));
    }
    cur.clear();
    in_word = false;
    braces = 0;
  };
  auto new_command = [&] {
    in_word = false;
    cur.clear();
    args.clear();
    words.clear();
    cmd_pos = true;
    redir = false;
    redirect.clear();
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
      end_word(i);
      ++i;
      continue;
    }
    if (c == '{' and in_word and cur.ends_with('$')) ++braces;
    else if (c == '}' and braces > 0) --braces;
    if (braces > 0) {   // ${(flags)…: all one word
      cur += c;
      ++i;
      continue;
    }
    if (c == '(' and in_word and not cur.empty() and cur.back() == '$') {   // $( … )
      new_command();
      ++i;
      continue;
    }
    if (c == '(' and in_word and cmd_pos and array_param.empty() and cur.ends_with('=')
        and is_ident(strv(cur).substr(0, cur.size() - 1))) {   // name=( … )
      array_param = cur.substr(0, cur.size() - 1);
      elements.clear();
      cur.clear();
      in_word = false;
      ++i;
      continue;
    }
    if (c == ')' and not array_param.empty()) {
      end_word(i);
      array_param.clear();
      ++i;
      continue;
    }
    if (c == '\n' or c == ';' or c == '&' or c == '|' or c == '(' or c == ')' or c == '`') {
      if (c == '`') backtick = not backtick;
      array_param.clear();
      new_command();
      ++i;
      continue;
    }
    if (c == '<' or c == '>') {
      s::size_t op = i;
      if (in_word and not cur.empty() and cur.find_first_not_of("0123456789") == str::npos) {
        op = word_start;
        cur.clear();
        in_word = false;
      } else {
        end_word(i);
      }
      while (i < cursor and strchr("<>&|!", line[i])) ++i;
      if (i < cursor and line[i] == '(') {   // <( … )
        new_command();
        ++i;
        continue;
      }
      redir = true;
      redirect = str(line.substr(op, i - op));
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
  ctx.command_position = cmd_pos and not redir and array_param.empty();
  ctx.args        = array_param.empty() ? args : vec<str>{};
  ctx.current_arg = static_cast<int>(ctx.args.size());
  ctx.args.push_back(cur);
  ctx.command     = ctx.args.size() > 1 ? ctx.args[0] : str{};
  ctx.backtick    = backtick;
  ctx.comp_start  = word_start;

  // zsh's context for $compstate, and the words a function sees.
  str whole(line.substr(word_start, ctx.word_end - word_start));
  if (not array_param.empty()) {
    // zsh leaves the first element out.
    words.assign(elements.begin() + (elements.empty() ? 0 : 1), elements.end());
    ctx.context   = "array_value";
    ctx.parameter = array_param;
  } else if (redir) {
    ctx.context  = "redirect";
    ctx.redirect = redirect;
  } else if (not words.empty() and words[0] == "[[") {
    ctx.context = "condition";
  }
  words.push_back(whole);
  ctx.current = static_cast<int>(words.size());
  // The words after the cursor, to the end of the command; a redirection
  // and its target are left out.
  bool skip = false;
  for (s::size_t at = ctx.word_end; at < line.size();) {
    char c = line[at];
    if (c == ' ' or c == '\t') {
      ++at;
      continue;
    }
    if (strchr(";&|()\n`", c)) break;
    if (c == '<' or c == '>') {
      while (at < line.size() and strchr("<>&|!", line[at])) ++at;
      skip = true;
      continue;
    }
    s::size_t from = at;
    char wq = 0;
    while (at < line.size()) {
      char d = line[at];
      if (wq) {
        if (d == wq) wq = 0;
      } else if (d == ' ' or d == '\t' or strchr(";&|()<>\n`", d)) {
        break;
      } else if (d == '\'' or d == '"') {
        wq = d;
      } else if (d == '\\') {
        ++at;
      }
      ++at;
    }
    strv w = line.substr(from, at - from);
    // 2>err: the fd number goes with the redirection.
    bool fd = w.find_first_not_of("0123456789") == strv::npos and at < line.size()
          and (line[at] == '<' or line[at] == '>');
    if (not skip and not fd) words.emplace_back(w);
    skip = false;
  }
  ctx.words = s::move(words);

  if (ctx.command_position and quote == 0) {
    // name=value, name[sub]=value and name[sub: the value or the subscript.
    s::size_t k = 0;
    while (k < cur.size() and (s::isalnum(static_cast<unsigned char>(cur[k])) or cur[k] == '_'))
      ++k;
    if (k > 0 and is_ident(strv(cur).substr(0, k)) and k < cur.size()) {
      strv raw = strv(line).substr(word_start, cursor - word_start);
      auto close = raw.find(']', k);
      if (cur[k] == '=' or (cur[k] == '[' and close != strv::npos and close + 1 < raw.size()
                            and raw[close + 1] == '=')) {
        auto eq = cur[k] == '=' ? k : close + 1;
        ctx.context    = "value";
        ctx.parameter  = cur.substr(0, k);
        ctx.comp_start = word_start + eq + 1;
        ctx.words      = {str(line.substr(ctx.comp_start, ctx.word_end - ctx.comp_start))};
        ctx.current    = 1;
      } else if (cur[k] == '[' and close == strv::npos) {
        ctx.context    = "subscript";
        ctx.parameter  = cur.substr(0, k);
        ctx.comp_start = word_start + k + 1;
        ctx.words.clear();
        ctx.current    = 0;
      }
    }
  }

  // Arithmetic: an (( or $(( not closed yet.
  strv before = strv(line).substr(0, cursor);
  auto open = before.rfind("((");
  if (open != strv::npos and before.find("))", open) == strv::npos) {
    ctx.context = "math";
    ctx.words.clear();
    ctx.current = 0;
    if (open == 0 or line[open - 1] != '$') ctx.comp_start = open + 2;
  }
  return ctx;
}

str comp_quote(strv w, char quote, bool at_start) {
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
      if (strchr(" \t\\'\"$&;|()<>*?[]!{}`^#", c) or (i == 0 and at_start and strchr("~=", c)))
        out += '\\';
      if (c == '\n') out += "$'\\n'";
      else           out += c;
    }
  }
  return out;
}

str comp_unquote(strv w, char quote) {
  str out;
  for (s::size_t i = 0; i < w.size(); ++i) {
    char c = w[i];
    if (quote == '\'') {
      out += c;
    } else if (c == '\\' and i + 1 < w.size()
               and (quote != '"' or strchr("\"\\$`", w[i + 1]))) {
      out += w[++i];
    } else {
      out += c;
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

CompletionResult CompletionEngine::complete(strv line, s::size_t cursor, const CompRequest& req) {
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
    if (auto c = mCompdefs.find(ctx.command);
        not mBuiltinDefs.contains(ctx.command) and c != mCompdefs.end() and mSources.call_function)
      return run_function(ctx, c->second, req);
    if (auto it = mBuiltinDefs.find(ctx.command); it != mBuiltinDefs.end()) {
      vec<CompCandidate> found = it->second(ctx);
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
        auto at = raw.find('=');
        r.start  = ctx.word_start + (at == strv::npos ? 0 : at + 1);
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

// ---- Completion functions (zsh/complete) -----------------------------------------

namespace {

s::size_t common_prefix_len(strv a, strv b) {
  s::size_t k = 0;
  while (k < a.size() and k < b.size() and a[k] == b[k]) ++k;
  return k;
}

// The longest common prefix, not ending inside a UTF-8 character.
str common_prefix_of(const vec<str>& v) {
  if (v.empty()) return {};
  str p = v[0];
  for (const auto& w : v) p.resize(common_prefix_len(p, w));
  while (not p.empty() and (static_cast<unsigned char>(p.back()) & 0xc0) == 0x80) p.pop_back();
  if (not p.empty() and static_cast<unsigned char>(p.back()) >= 0xc0) p.pop_back();
  return p;
}

// The word without its quote characters (backslashes stay, as zsh keeps
// them in $PREFIX).
str strip_quotes(strv w) {
  str out;
  char q = 0;
  for (s::size_t i = 0; i < w.size(); ++i) {
    char c = w[i];
    if (q) {
      if (c == q) q = 0;
      else        out += c;
    } else if (c == '\'' or c == '"') {
      q = c;
    } else {
      out += c;
      if (c == '\\' and i + 1 < w.size()) out += w[++i];
    }
  }
  return out;
}

}  // namespace

str CompState::text(const CompCandidate& m) const {
  bool at_start = m.ipre.empty() and m.apre.empty() and m.hpre.empty();
  str w = not m.qword.empty() ? m.qword : m.raw ? m.word : quoted(m.word, at_start);
  return m.ipre + m.apre + m.hpre + w + m.hsuf + m.asuf + m.isuf;
}

int CompState::add(const CompAddSpec& spec, const vec<str>& list, vec<bool>* matched,
                   vec<str>* internal) {
  // Groups with other sorting or duplicate rules are other groups.
  bool sorted = spec.sorted;
  int  dups   = spec.dups;
  s::size_t g = 0;
  while (g < groups.size()
         and not (groups[g].name == spec.group and groups[g].sorted == sorted
                  and groups[g].dups == dups and groups[g].by_match == spec.by_match))
    ++g;
  if (g == groups.size() and not matched)
    groups.push_back(CompGroup{spec.group, sorted, dups, spec.by_match, {}, {}});

  // The call's matchers apply to its words.
  MatchRun& mr = *run;
  mr.begin_add(spec.matcher, nmatches());
  struct EndAdd {
    MatchRun& m;
    ~EndAdd() { m.end_add(); }
  } end_add{mr};
  // compstate[list] as the matches are added says how they are laid out
  // (zsh's complistflags).
  auto lk = keys.find("list");
  struct MarkLayout {
    ~MarkLayout() {
      for (s::size_t k = first; k < m.size(); ++k) {
        m[k].packed = packed;
        m[k].rows   = rows;
      }
    }
    vec<CompCandidate>& m;
    s::size_t first;
    bool packed, rows;
  } mark{matches, matches.size(),
         lk != keys.end() and lk->second.find("packed") != str::npos,
         lk != keys.end() and lk->second.find("rows") != str::npos};
  bool domatch = not spec.no_match;
  if (not matched and (not list.empty() or spec.empties or spec.all)) {
    if (domatch) mr.hasmatched = true;
    else         mr.hasunmatched = true;
  }

  str lpre = prefix, lsuf = suffix;
  Cline *pline = nullptr, *sline = nullptr;
  bool none = false;   // -p or -s doesn't match: no word does
  if (domatch) {
    if (qiprefix.size() + iprefix.size() + lpre.size() != orig_lpre
        or qisuffix.size() + isuffix.size() + lsuf.size() != orig_lsuf)
      lenchanged = true;
    // A -P prefix already on the line (or the line a start of it) is not
    // matched.
    s::size_t k = common_prefix_len(spec.apre, lpre);
    if (not spec.apre.empty() and (k == spec.apre.size() or k == lpre.size())) lpre.erase(0, k);
    // -p and -s are matched with the line as far as it goes.
    if (const str& h = spec.hpre; not h.empty()) {
      int ml = mr.match_part(lpre, h, false, &pline);
      if (ml >= 0)                                               lpre.erase(0, s::size_t(ml));
      else if (lpre.size() <= h.size() and h.starts_with(lpre)) lpre.clear();
      else if (lpre.size() > h.size() and lpre.starts_with(h))  lpre.erase(0, h.size());
      else                                                       none = true;
    }
    if (const str& h = spec.hsuf; not h.empty()) {
      int ml = mr.match_part(lsuf, h, true, &sline);
      s::size_t cut = ml >= 0 ? s::size_t(ml) : h.size();
      if (ml >= 0 or (lsuf.size() > h.size() and lsuf.ends_with(h))) lsuf.resize(lsuf.size() - cut);
      else if (lsuf.size() <= h.size() and h.ends_with(lsuf))        lsuf.clear();
      else                                                           none = true;
    }
  }
  vec<CompiledPattern> ignore;
  for (const auto& p : spec.ignore) ignore.push_back(CompiledPattern::compile(p));
  // compstate[pattern_match]: a line with pattern characters is matched as
  // a pattern, a `*` at the cursor when it says `*` (zsh's comppatmatch).
  opt<CompiledPattern> line_pattern;
  if (auto pm = keys.find("pattern_match");
      domatch and pm != keys.end() and not pm->second.empty()) {
    str text = lpre + (pm->second[0] == '*' ? "*" : "") + lsuf;
    if (has_glob_chars(text, spec.extended))
      line_pattern = CompiledPattern::compile(text, false, spec.extended);
  }

  // A match the group has already (the last one with -1, none with -2)
  // is not added again.
  auto duplicate = [&](const CompCandidate& c) {
    if (dups == 2) return false;
    str t = text(c);
    for (auto it = matches.rbegin(); it != matches.rend(); ++it) {
      if (it->group != g or it->all) continue;
      if (it->word == c.word and it->empty == c.empty and text(*it) == t) return true;
      if (dups == 1) return false;
    }
    return false;
  };
  // An exact match is accepted if compstate[exact] says so when it is added.
  auto exact_key = keys.find("exact");
  bool accept_exact = rec_exact or (exact_key != keys.end() and not exact_key->second.empty());

  int added = 0;
  for (s::size_t i = 0; i < list.size() and not none; ++i) {
    const str& w = list[i];
    str qw = spec.raw ? w : quoted(w, spec.hpre.empty());
    Cline* lc = nullptr;
    bool isexact = false;
    opt<str> ms;
    if (line_pattern) {
      haspattern = true;
      if (line_pattern->matches(w)) {
        ms = qw;
        lc = mr.plain(qw);
      }
    } else if (domatch) {
      ms = mr.comp_match(lpre, lsuf, qw, &lc, &isexact);
    } else {
      ms = qw;
      lc = mr.plain(qw);
    }
    bool ok = ms.has_value();
    if (ok and s::any_of(ignore.begin(), ignore.end(),
                         [&](const CompiledPattern& p) { return p.matches(w); })) {
      ++ignored;
      ok = false;
    }
    if (matched) {
      matched->push_back(ok);
      if (ok and internal) internal->push_back(*ms);
      continue;
    }
    if (not ok) continue;
    CompCandidate c;
    c.word    = w;
    if (*ms != qw) c.qword = *ms;
    c.display = i < spec.display.size() ? spec.display[i] : str{};
    c.ipre    = (spec.no_match ? str{} : iprefix) + spec.ipre;
    c.apre    = spec.apre;
    c.hpre    = spec.hpre;
    c.hsuf    = spec.hsuf;
    c.asuf    = spec.asuf;
    c.isuf    = spec.isuf + (spec.no_match ? str{} : isuffix);
    c.raw     = spec.raw;
    c.hidden  = spec.hidden;
    c.line    = spec.lines and i < spec.display.size();
    c.group   = g;
    c.suffix  = spec.has_suffix ? str{} : spec.dir_suffix.empty() ? str(" ") : spec.dir_suffix;
    if (spec.files) {
      // As zsh's addmatch: the -W directory (or ./) and the word; the -p
      // prefix is not part of it (_path_files passes the path as both).
      str path = (spec.file_prefix.empty() ? str("./") : spec.file_prefix) + w;
      struct stat st{}, lst{};
      bool exists = ::stat(path.c_str(), &st) == 0;
      bool link   = ::lstat(path.c_str(), &lst) == 0 and S_ISLNK(lst.st_mode);
      bool dir    = exists and S_ISDIR(st.st_mode);
      bool exe    = exists and S_ISREG(st.st_mode) and ::access(path.c_str(), X_OK) == 0;
      // A hidden -s suffix follows the word (`mvd` + `/abx`): no slash or
      // space of its own then, as in zsh.
      if (not spec.has_suffix)
        c.suffix = not spec.hsuf.empty() ? str{} : dir ? "/" : exists ? " " : "";
      c.type = dir ? '/' : link ? '@' : exe ? '*' : (exists and S_ISFIFO(st.st_mode)) ? '|'
             : (exists and S_ISSOCK(st.st_mode)) ? '=' : 0;
    }
    c.remove_chars = spec.remove_chars;
    c.remove_func  = spec.remove_func;
    c.exact        = accept_exact and isexact;
    // Every match goes into the unambiguous string, duplicates too.
    mr.add(lc, qiprefix, c.ipre, c.apre, c.hpre, pline, c.hsuf, sline, c.isuf, qisuffix);
    mr.minmlen = s::min(mr.minmlen, static_cast<int>(ms->size() + c.hpre.size() + c.hsuf.size()));
    ++added;
    if (not duplicate(c)) matches.push_back(s::move(c));
  }
  if (matched) return 0;
  for (int k = 0; k < spec.empties; ++k) {
    CompCandidate c;
    c.empty   = true;
    c.suffix.clear();
    c.group   = g;
    auto at   = list.size() + static_cast<s::size_t>(k);
    c.display = at < spec.display.size() ? spec.display[at] : str{};
    if (not duplicate(c)) matches.push_back(s::move(c));
  }
  if (spec.all and s::none_of(matches.begin(), matches.end(),
                              [](const CompCandidate& c) { return c.all; })) {
    CompCandidate c;
    c.all   = true;
    c.group = g;
    c.suffix.clear();
    matches.push_back(s::move(c));
  }
  if (spec.explanation) {
    auto& ex = groups[g].explanations;
    if (s::find(ex.begin(), ex.end(), *spec.explanation) == ex.end())
      ex.push_back(*spec.explanation);
  }
  if (spec.message) groups[g].messages.push_back(*spec.message);
  return added;
}

s::int64_t CompState::nmatches() const { return static_cast<s::int64_t>(matches.size()); }

str CompState::unambiguous(s::size_t* cursor, vec<s::size_t>* positions) const {
  return run->unambiguous(cursor, positions);
}

bool CompState::accepts_exact() const {
  return s::any_of(matches.begin(), matches.end(), [](const CompCandidate& m) { return m.exact; });
}

opt<str> CompState::exact_string() const {
  str line = prefix + suffix;
  for (const auto& m : matches) {
    if (m.empty or m.all) continue;
    str w = not m.qword.empty() ? m.qword : m.raw ? m.word : quoted(m.word, m.hpre.empty());
    if (m.hpre + w + m.hsuf == line) return m.word;
  }
  return s::nullopt;
}

bool CompState::readonly_key(strv key) {
  static const uset<str> keys = {
    "nmatches", "unambiguous", "unambiguous_cursor", "unambiguous_positions",
    "insert_positions", "list_lines", "all_quotes", "ignored", "quote", "quoting"};
  return keys.contains(str(key));
}

opt<str> CompState::get(const str& key) const {
  if (key == "nmatches") return s::to_string(nmatches());
  if (key == "ignored")  return s::to_string(ignored);
  if (key == "unambiguous") return unambiguous();
  if (key == "unambiguous_cursor") {
    s::size_t at = 0;
    unambiguous(&at);
    return s::to_string(at + 1);
  }
  if (key == "unambiguous_positions") {
    vec<s::size_t> at;
    unambiguous(nullptr, &at);
    str out;
    for (auto p : at) out += (out.empty() ? "" : ":") + s::to_string(p);
    return out;
  }
  if (key == "quote") return quote ? opt<str>(str(1, quote)) : s::nullopt;
  if (key == "quoting") {
    if (not quote) return s::nullopt;
    return str(quote == '\'' ? "single" : quote == '"' ? "double" : "backtick");
  }
  if (key == "insert_positions") return str{};
  if (key == "exact_string") {
    if (auto e = exact_string()) return e;
  }
  if (key == "exact" and not keys.contains("exact") and accepts_exact()) return str("accept");
  if (key == "list_lines") {
    // Rows of each group's columns, and its explanations and messages.
    int cols = s::max(1, columns);
    s::int64_t lines = 0;
    for (s::size_t g = 0; g < groups.size(); ++g) {
      int n = 0, width = 0;
      for (const auto& m : matches) {
        if (m.group != g or m.hidden) continue;
        if (m.line) {
          ++lines;
          continue;
        }
        ++n;
        width = s::max(width, text_width(m.display.empty() ? m.word : m.display));
      }
      int ncols = s::max(1, (cols + 1) / (width + 2));
      if (n > 0)
        lines += (n + ncols - 1) / ncols + static_cast<s::int64_t>(groups[g].explanations.size());
      lines += static_cast<s::int64_t>(groups[g].messages.size());
    }
    return s::to_string(lines);
  }
  auto it = keys.find(key);
  if (it == keys.end()) return s::nullopt;
  return it->second;
}

umap<str, str> CompState::view() const {
  umap<str, str> out;
  for (const auto& [k, v] : keys) out[k] = v;
  for (strv k : {"nmatches", "ignored", "unambiguous", "unambiguous_cursor",
                 "unambiguous_positions", "insert_positions", "list_lines", "exact_string",
                 "quote", "quoting", "exact"})
    if (auto v = get(str(k))) out[str(k)] = *v;
  return out;
}

CompState CompletionEngine::make_state(const CompContext& ctx, const CompRequest& req) const {
  CompState st;
  st.words   = ctx.words;
  st.current = ctx.current;
  st.quote   = ctx.quote ? ctx.quote : ctx.backtick ? '`' : 0;
  strv line(ctx.line);
  strv before = line.substr(ctx.comp_start, ctx.cursor - ctx.comp_start);
  strv after  = line.substr(ctx.cursor, ctx.word_end - ctx.cursor);
  st.keys      = req.keys;
  st.rec_exact = req.rec_exact;
  st.keys["context"] = ctx.context;
  if (not ctx.parameter.empty()) st.keys["parameter"] = ctx.parameter;
  if (not ctx.redirect.empty())  st.keys["redirect"]  = ctx.redirect;
  st.keys["all_quotes"] = ctx.quote ? str(1, ctx.quote) : str("\\");

  // $name, ${name: the name is completed, the rest is an ignored prefix.
  if (ctx.quote != '\'' and ctx.context != "math") {
    s::size_t k = before.size();
    auto ident = [](char ch) { return s::isalnum(static_cast<unsigned char>(ch)) or ch == '_'; };
    while (k > 0 and ident(before[k - 1])) --k;
    bool brace = k > 0 and before[k - 1] == '{';
    s::size_t dollar = brace ? k - 1 : k;
    if (dollar > 0 and before[dollar - 1] == '$' and (dollar < 2 or before[dollar - 2] != '\\')) {
      st.keys["context"] = brace ? "brace_parameter" : "parameter";
      st.iprefix = str(before.substr(0, k));
      st.prefix  = str(before.substr(k));
      st.suffix  = strip_quotes(after);
      return st;
    }
    auto open = before.rfind("${");
    if (open != strv::npos and before.find('}', open) == strv::npos)
      st.keys["context"] = "brace_parameter";
  }
  if (ctx.quote and before.starts_with(ctx.quote)) {
    st.qiprefix = str(1, ctx.quote);
    st.prefix   = str(before.substr(1));
    if (after.ends_with(ctx.quote)) {
      st.qisuffix = str(1, ctx.quote);
      after.remove_suffix(1);
    }
    st.suffix = str(after);
  } else {
    st.prefix = strip_quotes(before);
    st.suffix = strip_quotes(after);
  }
  if (ctx.context == "math") st.prefix = str(before);
  return st;
}

CompletionResult CompletionEngine::run_function(const CompContext& ctx, const str& fn,
                                                const CompRequest& req) {
  CompState st = make_state(ctx, req);
  st.orig_lpre = st.qiprefix.size() + st.iprefix.size() + st.prefix.size();
  st.orig_lsuf = st.qisuffix.size() + st.isuffix.size() + st.suffix.size();
  if (auto p = mParams.lookup("COLUMNS"); p and p->get().is_set())
    st.columns = s::atoi(p->get().scalar().c_str());
  CompState* outer = mState;
  mState = &st;
  if (mSources.call_function) mSources.call_function(fn, st);
  mState = outer;

  CompletionResult r;
  r.function = true;
  r.haspattern = st.haspattern;
  r.quote    = ctx.quote;
  r.start    = s::min(ctx.comp_start + st.qiprefix.size(), ctx.word_end);
  r.end      = s::max(r.start, ctx.word_end - s::min(st.qisuffix.size(), ctx.word_end));
  r.prefix   = st.prefix;
  // The unambiguous string replaces the word between its quotes.
  s::size_t at = 0;
  str u = st.unambiguous(&at);
  r.keep_word = u.size() < ctx.word_end - ctx.comp_start and not st.lenchanged
            and not st.run->hasunmatched;
  if (u.starts_with(st.qiprefix)) {
    u.erase(0, st.qiprefix.size());
    at = at >= st.qiprefix.size() ? at - st.qiprefix.size() : 0;
  }
  if (not st.qisuffix.empty() and u.ends_with(st.qisuffix)) u.resize(u.size() - st.qisuffix.size());
  r.unambiguous        = s::move(u);
  r.unambiguous_cursor = s::min(at, r.unambiguous.size());
  for (const auto& [k, v] : st.view()) r.keys[k] = v;

  // The matches in listing order: group by group, each sorted unless it
  // was added with -V, with duplicates dropped as the group says.
  for (s::size_t g = 0; g < st.groups.size(); ++g) {
    const CompGroup& grp = st.groups[g];
    vec<CompCandidate> in;
    for (auto& m : st.matches)
      if (m.group == g) in.push_back(m);
    if (grp.sorted) {
      s::stable_sort(in.begin(), in.end(), [&](const CompCandidate& a, const CompCandidate& b) {
        if (a.all != b.all) return a.all;
        const str& x = grp.by_match or a.display.empty() ? a.word : a.display;
        const str& y = grp.by_match or b.display.empty() ? b.word : b.display;
        return zstrcmp(x, y, false, true) < 0;
      });
    }
    vec<CompCandidate> kept;
    for (auto& m : in) {
      auto same = [&](const CompCandidate& k) {
        return k.word == m.word and st.text(k) == st.text(m);
      };
      bool dup = not m.all
             and (grp.dups == 1 ? not kept.empty() and same(kept.back())
                                : grp.dups == 0 and s::any_of(kept.begin(), kept.end(), same));
      if (dup) continue;
      m.group = r.groups.size();
      kept.push_back(s::move(m));
    }
    for (auto& m : kept) r.matches.push_back(s::move(m));
    r.groups.push_back(grp);
  }
  // The -C match comes first.
  s::stable_partition(r.matches.begin(), r.matches.end(),
                      [](const CompCandidate& m) { return m.all; });
  return r;
}

CompletionResult CompletionEngine::complete_with(strv line, s::size_t cursor, const str& fn,
                                                 const CompRequest& req) {
  return run_function(build_context(line, cursor), fn, req);
}

// ---- The editor's side ------------------------------------------------------------

namespace {

// A match as menu completion puts it in the line: the word with what
// compadd adds around it, and a directory's slash.
str menu_text(const CompletionResult& r, const CompCandidate& m) {
  if (m.all) {
    str out;
    for (const auto& o : r.matches) {
      if (o.empty or o.all) continue;
      if (not out.empty()) out += ' ';
      out += menu_text(r, o);
    }
    return out;
  }
  bool at_start = m.ipre.empty() and m.apre.empty() and m.hpre.empty();
  str w = not m.qword.empty() ? m.qword : m.raw ? m.word : comp_quote(m.word, r.quote, at_start);
  str text = m.ipre + m.apre + m.hpre + w + m.hsuf + m.asuf + m.isuf;
  if (m.suffix == "/" or m.suffix == "}") text += m.suffix;
  return text;
}

// Whether typing `key` ("" for a widget that inserts nothing) removes an
// auto-removable suffix whose characters are `spec` (compadd -r: a
// character class, "\-" for the widgets that insert nothing).
bool removes_suffix(strv spec, strv key) {
  bool negate = spec.starts_with('^');
  if (negate) spec.remove_prefix(1);
  bool nothing = false, found = false;
  unsigned char c = key.empty() ? 0 : static_cast<unsigned char>(key[0]);
  for (s::size_t i = 0; i < spec.size(); ++i) {
    unsigned char lo = static_cast<unsigned char>(spec[i]);
    if (lo == '\\' and i + 1 < spec.size()) {
      char e = spec[++i];
      if (e == '-') {
        nothing = true;
        continue;
      }
      lo = e == 't' ? '\t' : e == 'n' ? '\n' : e == 'e' ? '\x1b' : static_cast<unsigned char>(e);
    }
    unsigned char hi = lo;
    if (i + 2 < spec.size() and spec[i + 1] == '-') {
      hi = static_cast<unsigned char>(spec[i + 2]);
      i += 2;
    }
    if (not key.empty() and c >= lo and c <= hi) found = true;
  }
  if (key.empty()) return nothing;
  return negate ? not found : found;
}

// The match compstate[exact] (or REC_EXACT) accepted when it was added.
opt<s::size_t> exact_match(const CompletionResult& r) {
  for (s::size_t i = 0; i < r.matches.size(); ++i)
    if (r.matches[i].exact) return i;
  return s::nullopt;
}

}  // namespace

// auto-suffix-remove: the suffix goes as for a key that inserts nothing.
bool Editor::w_auto_suffix_remove() {
  auto sfx = s::exchange(mAutoSuffix, s::nullopt);
  if (not sfx or mBuf.cursor != sfx->end or sfx->end > mBuf.text.size()) return true;
  if (not sfx->func.empty()) {
    if (mZle.hooks.run_widget) mZle.hooks.run_widget(*this, mWidget, sfx->func,
                                                     {s::to_string(sfx->len)});
    mBuf.cursor = s::min(mBuf.cursor, mBuf.text.size());
    return true;
  }
  if (not removes_suffix(sfx->chars, {})) return true;
  mBuf.erase(sfx->end - sfx->len, sfx->len);
  mBuf.cursor = sfx->end - sfx->len;
  return true;
}

bool Editor::is_completion_widget(strv name) const {
  static const uset<str> builtin = {
    "expand-or-complete", "expand-or-complete-prefix", "complete-word", "menu-complete",
    "reverse-menu-complete", "menu-expand-or-complete", "list-choices", "delete-char-or-list"};
  str n(name.starts_with('.') ? name.substr(1) : name);
  return builtin.contains(n) or mZle.comp_widgets.contains(n);
}

void Editor::insert_match(const CompletionResult& r, s::size_t index, bool final) {
  const CompCandidate& m = r.matches[index];
  str text = menu_text(r, m);
  // The rest of the word after the cursor goes when the match replaces it.
  s::size_t end = r.end;
  if (final) {
    if (m.suffix == "/" or m.suffix == "}") text.resize(text.size() - m.suffix.size());
    if (r.quote and m.suffix != "/") text += r.quote;
    text += m.suffix;
    if (r.quote and end < mBuf.text.size() and mBuf.text[end] == r.quote and m.suffix != "/") ++end;
  }
  mBuf.replace(r.start, end - r.start, text);
  mBuf.cursor = r.start + text.size();
  mAutoSuffix = auto_suffix(m, final);
  if (mAutoSuffix and mAutoSuffix->len <= mBuf.cursor) mAutoSuffix->end = mBuf.cursor;
  else                                                  mAutoSuffix.reset();
}

opt<Editor::AutoSuffix> Editor::auto_suffix(const CompCandidate& m, bool final) const {
  // compadd -q, -r, -R: the suffix goes again if the next key says so.
  if (not m.remove_chars.empty() or not m.remove_func.empty()) {
    s::size_t len = not m.asuf.empty() ? m.asuf.size()
                  : final or m.suffix == "/" ? m.suffix.size() : 0;
    if (len == 0) return s::nullopt;
    return AutoSuffix{0, len, m.remove_chars, m.remove_func};
  }
  if (not m.asuf.empty()) return s::nullopt;
  // The space after a match, and (AUTO_REMOVE_SLASH) a directory's slash,
  // go when a blank, ; & | (a slash after a slash) or a widget that
  // inserts nothing comes next.
  if (m.suffix == "/" and mOpts.get(OPT_AUTOREMOVESLASH))
    return AutoSuffix{0, 1, " \\t\\n;&|/\\-", {}};
  if (m.suffix == " " and final) return AutoSuffix{0, 1, " \\t\\n;&|\\-", {}};
  return s::nullopt;
}

void Editor::remove_suffix(strv widget) {
  // A user widget leaves it to the widgets it calls.
  if (not mAutoSuffix or mZle.user_widgets.contains(str(widget))) return;
  auto sfx = s::exchange(mAutoSuffix, s::nullopt);
  strv base = widget.starts_with('.') ? widget.substr(1) : widget;
  // zsh's ZLE_KEEPSUFFIX widgets leave it: those that insert after it,
  // kill or delete, take an argument, mark, redisplay (backspace takes
  // the suffix as the character it deletes).
  SuffixRule rule = SuffixRule::Remove;
  if (auto m = mZle.module_widgets.find(str(base)); m != mZle.module_widgets.end())
    rule = m->second.suffix;
  else if (auto it = widget_table().find(str(base)); it != widget_table().end())
    rule = it->second.suffix;
  if (rule != SuffixRule::Remove) {
    if (rule == SuffixRule::Pending) mAutoSuffix = sfx;
    return;
  }
  if (is_completion_widget(widget) or mBuf.cursor != sfx->end or sfx->end > mBuf.text.size())
    return;
  if (not sfx->func.empty()) {
    if (mZle.hooks.run_widget) mZle.hooks.run_widget(*this, str(widget), sfx->func,
                                                     {s::to_string(sfx->len)});
    mBuf.cursor = s::min(mBuf.cursor, mBuf.text.size());
    return;
  }
  bool inserts = widget == "self-insert" or widget == ".self-insert";
  str key = inserts ? mKeys : str{};
  if (key.size() > 1 and key[0] == '\x1b') key.erase(0, 1);
  if (not removes_suffix(sfx->chars, key)) return;
  mBuf.erase(sfx->end - sfx->len, sfx->len);
  mBuf.cursor = sfx->end - sfx->len;
}

void Editor::list_matches(const CompletionResult& r, strv mode) {
  bool types    = mOpts.get(OPT_LISTTYPES);
  bool explains = mode.find("explanations") != strv::npos;
  bool messages = mode.find("messages") != strv::npos;
  bool all      = not explains and not messages;
  bool rows_first = mOpts.get(OPT_LISTROWSFIRST) or mode.find("rows") != strv::npos;
  int  cols = s::max(terminal_width(), 2);

  // What is listed, group by group: explanations and messages on lines
  // of their own, then the matches in columns, and -l ones one a line.
  struct Block {
    vec<str> heads;
    vec<str> items;
    vec<str> lines;
    bool packed = true, rows = true;   // all its matches were added so
  };
  vec<Block> blocks;
  int n = 0;
  s::size_t ngroups = s::max<s::size_t>(r.groups.size(), 1);
  for (s::size_t g = 0; g < ngroups; ++g) {
    Block b;
    for (const auto& m : r.matches) {
      if (m.group != g or m.hidden or m.all) continue;
      str shown = m.display.empty() ? m.word : m.display;
      if (types and m.type) shown += m.type;
      if (not m.description.empty()) shown += "  -- " + m.description;
      if (m.line) {
        b.lines.push_back(s::move(shown));
        continue;
      }
      b.items.push_back(s::move(shown));
      // Without a function, compstate[list] (the options) says.
      b.packed = b.packed and (r.function ? m.packed : mode.find("packed") != strv::npos);
      b.rows   = b.rows and (r.function ? m.rows : rows_first);
    }
    bool has = not b.items.empty() or not b.lines.empty();
    if (g < r.groups.size()) {
      if (has and (all or explains))
        for (const auto& e : r.groups[g].explanations) b.heads.push_back(format_explanation(e));
      if (all or messages)
        for (const auto& e : r.groups[g].messages) b.heads.push_back(format_explanation(e));
    }
    if (not all) {
      b.items.clear();
      b.lines.clear();
    }
    n += static_cast<int>(b.items.size() + b.lines.size());
    if (not b.heads.empty() or not b.items.empty() or not b.lines.empty())
      blocks.push_back(s::move(b));
  }
  if (blocks.empty()) return;

  vec<str> out;
  // The columns as zsh's calclist() lays them out: each group in as many
  // columns of its widest match as fit, all as wide as the widest
  // group's; a packed group (compstate[list] `packed`) with columns each
  // as wide as its own widest entry, as many as fit.
  constexpr int CM_SPACE = 2;
  struct Lay {
    vec<int> lens, widths;
    int dcount = 0, cols = 0, width = 0, shortest = 0, lins = 0, totl = 0;
  };
  vec<Lay> lays(blocks.size());
  int max = 0;
  for (s::size_t k = 0; k < blocks.size(); ++k) {
    Lay& g = lays[k];
    int glong = 1, gshort = cols, totl = 0;
    for (const auto& it : blocks[k].items) {
      int l = text_width(it);
      g.lens.push_back(l);
      glong = s::max(glong, l);
      gshort = s::min(gshort, l);
      totl += l;
    }
    g.dcount = static_cast<int>(g.lens.size());
    g.totl = totl + g.dcount * CM_SPACE;
    g.width = glong + CM_SPACE;
    g.shortest = gshort + CM_SPACE;
    g.cols = s::min(cols / g.width, g.dcount);
    if (g.cols) max = s::max(max, g.cols * g.width - CM_SPACE);
  }
  for (auto& g : lays) {
    if (g.cols) {
      g.lins = (g.dcount + g.cols - 1) / g.cols;
      if (g.cols > 1) g.width += (max - (g.width * g.cols - CM_SPACE)) / g.cols;
    } else {
      g.cols = 1;
      g.width = 0;
      for (int l : g.lens) g.lins += 1 + (l - 1) / cols;
    }
  }
  for (s::size_t k = 0; k < blocks.size(); ++k) {
    Lay& g = lays[k];
    if (not blocks[k].packed or not g.width) continue;
    vec<int> ws(static_cast<s::size_t>(cols), 0);
    int tlines = g.lins, tcols = g.cols, width = 0;
    for (tcols = cols / (g.shortest + CM_SPACE); tcols > g.cols; --tcols) {
      s::fill(ws.begin(), ws.end(), 0);
      width = 0;
      if (blocks[k].rows) {
        tlines = 1;
        for (int nth = 0, tcol = 0; width < cols and nth < g.dcount; ++nth, ++tcol) {
          if (tcol == tcols) {
            tcol = 0;
            ++tlines;
          }
          int len = g.lens[static_cast<s::size_t>(nth)] + (tcol == tcols - 1 ? 0 : CM_SPACE);
          auto& w = ws[static_cast<s::size_t>(tcol)];
          if (len > w) {
            width += len - w;
            w = len;
          }
        }
        if (width < cols) break;
      } else {
        tlines = s::max(1, (g.dcount + tcols - 1) / tcols);
        int tcol = 0;
        for (int nth = 0, tline = 0; width < cols and nth < g.dcount; ++nth, ++tline) {
          if (tline == tlines) {
            ++tcol;
            tline = 0;
          }
          if (tcol == tcols) {
            tcol = 0;
            ++tlines;
          }
          int len = g.lens[static_cast<s::size_t>(nth)] + (tcol == tcols - 1 ? 0 : CM_SPACE);
          auto& w = ws[static_cast<s::size_t>(tcol)];
          if (len > w) {
            width += len - w;
            w = len;
          }
        }
        if (width < cols) {
          if (++tcol < tcols) tcols = tcol;
          break;
        }
      }
    }
    if (tcols <= g.cols) tlines = g.lins;
    if (tlines != g.lins) {
      g.lins = tlines;
      g.cols = tcols;
      g.totl = width;
      g.widths.assign(ws.begin(), ws.begin() + tcols);
      max = s::max(max, width - CM_SPACE);
    }
  }
  for (auto& g : lays) {
    if (not g.widths.empty()) {
      int a = (max - g.totl + CM_SPACE) / g.cols;
      for (int& w : g.widths) w += a;
    } else if (g.width and g.cols > 1) {
      g.width += (max - (g.width * g.cols - CM_SPACE)) / g.cols;
    }
  }
  for (s::size_t k = 0; k < blocks.size(); ++k) {
    const Block& b = blocks[k];
    const Lay& g = lays[k];
    for (const auto& h : b.heads) out.push_back(h);
    for (const auto& l : b.lines) out.push_back(l);
    int left = g.dcount;
    int step = b.rows ? 1 : g.lins;
    for (int row = 0, p = 0; left > 0 and row < g.lins; ++row) {
      str line;
      int q = p;
      for (int mc = 0; mc < g.cols and left > 0; ++mc) {
        if (q >= g.dcount) break;
        const str& it = b.items[static_cast<s::size_t>(q)];
        line += it;
        int wid = g.widths.empty() ? g.width : g.widths[static_cast<s::size_t>(mc)];
        if (mc + 1 < g.cols)
          line += str(static_cast<s::size_t>(s::max(0, wid - text_width(it))), ' ');
        --left;
        q += step;
      }
      while (not line.empty() and line.back() == ' ') line.pop_back();
      out.push_back(s::move(line));
      p += b.rows ? g.cols : 1;
    }
  }
  int nrows = static_cast<int>(out.size());

  // LISTMAX (or compstate[list_max]): how many matches to show without asking.
  str lm = r.keys.contains("list_max") ? r.keys.at("list_max") : mZle.param("LISTMAX");
  int listmax = lm.empty() ? 100 : s::atoi(lm.c_str());
  int height  = terminal_height();
  bool ask = (listmax > 0 and n > listmax) or (listmax == 0 and nrows + 1 >= height)
          or (listmax < 0 and nrows > -listmax);
  if (ask) {
    // zsh's getzlequery, the cursor after the question: keys typed ahead
    // are a no (`n` is shown, they stay to be read); else Tab is a yes,
    // a control character a no, and the answer is shown; only y lists.
    mMessage = "jfsh: do you wish to see all " + s::to_string(n) + " possibilities ("
             + s::to_string(nrows) + " lines)? ";
    refresh();
    int down = mEndRow + 1 - mCursorRow;
    int col  = s::min(static_cast<int>(char_count(mMessage)), terminal_width() - 1);
    write_out("\x1b[" + s::to_string(down) + "B\r\x1b[" + s::to_string(col) + "C");
    int waiting = 0;
    bool yes = false;
    if (mPending.empty() and ::ioctl(mIn, FIONREAD, &waiting) == 0 and waiting > 0) {
      write_out("n");
    } else {
      str k;
      int c = read_byte(-1);
      if (c >= 0) {
        k += static_cast<char>(c);
        (void)read_rest_char(k);
      }
      s::size_t len = 0;
      char32_t wc = k.empty() ? 0 : utf8_decode(k, 0, len);
      if (wc == '\t')
        wc = 'y';
      else if (k.empty() or s::iswcntrl(static_cast<wint_t>(wc)))
        wc = 'n';
      else
        wc = static_cast<char32_t>(s::towlower(static_cast<wint_t>(wc)));
      if (wc != '\n') {
        str echo;
        utf8_append(echo, wc);
        write_out(echo);
      }
      yes = wc == 'y';
    }
    // Back to the line, where refresh expects the cursor.
    write_out("\x1b[" + s::to_string(down) + "A");
    mMessage.clear();
    if (not yes) {
      refresh();
      return;
    }
  }

  str rows;
  for (s::size_t k = 0; k < out.size(); ++k) {
    if (k > 0) rows += '\n';
    rows += out[k];
  }
  if (nrows + 1 > height) {
    // Too tall to keep below the line (zsh keeps a listing that fits
    // with the line on the screen): print it, and the prompt after.
    trash();
    str text;
    for (char c : rows) text += c == '\n' ? str("\r\n") : str(1, c);
    write_out(text + "\r\n");
    mListing.clear();
  } else {
    show_listing(s::move(rows));
  }
  mListed = true;
}

str Editor::format_explanation(strv text) {
  // %B %b %S %s %U %u %F{…} %f %K{…} %k %{…%}: attributes, left out of
  // a plain listing; %% is a %.
  str out;
  for (s::size_t i = 0; i < text.size(); ++i) {
    if (text[i] != '%' or i + 1 >= text.size()) {
      out += text[i];
      continue;
    }
    char c = text[++i];
    if (c == '%') {
      out += '%';
    } else if ((c == 'F' or c == 'K') and i + 1 < text.size() and text[i + 1] == '{') {
      auto close = text.find('}', i);
      i = close == strv::npos ? text.size() : close;
    } else if (c == '{') {
      auto close = text.find("%}", i);
      i = close == strv::npos ? text.size() : close + 1;
    } else if (not strchr("BbSsUuFfKk", c)) {
      out += '%';
      out += c;
    }
  }
  return out;
}

bool Editor::do_complete(bool list_only, bool menu, bool reverse, const str& fn) {
  if (not mZle.completion) return false;
  bool completing = mLastCompleting;
  // Going round the menu.
  if (mMenuActive and not list_only and completing and not mMenu.empty()) {
    s::size_t n = mMenu.size();
    mMenuIndex = reverse ? (mMenuIndex + n - 1) % n : (mMenuIndex + 1) % n;
    const str& text = mMenu[mMenuIndex].first;
    mBuf.replace(mMenuStart, mMenuEnd - mMenuStart, text);
    mMenuEnd = mMenuStart + text.size();
    mBuf.cursor = mMenuEnd;
    mAutoSuffix.reset();
    if (auto& a = mMenu[mMenuIndex].second) {
      mAutoSuffix = *a;
      mAutoSuffix->end = mMenuEnd;
    }
    return true;
  }
  s::size_t cursor = mBuf.cursor;
  auto ctx = mZle.completion->build_context(mBuf.text, cursor);
  // Without COMPLETE_IN_WORD the whole word is completed, from its end.
  if (not mOpts.get(OPT_COMPLETEINWORD)) cursor = ctx.word_end;
  bool again = completing and mCompleteLine == mBuf.text;

  // $compstate as a completion function finds it.
  CompRequest req;
  auto& k = req.keys;
  if (not list_only) {
    k["insert"] = menu or mOpts.get(OPT_MENUCOMPLETE) ? "menu"
                : not mOpts.get(OPT_AUTOMENU)        ? "unambiguous"
                : again                              ? "automenu"
                                                     : "automenu-unambiguous";
  }
  str list = list_only ? "list"
           : not mOpts.get(OPT_AUTOLIST) ? ""
           : mOpts.get(OPT_LISTAMBIGUOUS) ? "ambiguous" : "autolist";
  if (not list.empty() and mOpts.get(OPT_LISTPACKED))   list += " packed";
  if (not list.empty() and mOpts.get(OPT_LISTROWSFIRST)) list += " rows";
  k["list"] = list;
  if (again and mLastResult) k["old_list"] = mListed ? "shown" : "yes";
  if (completing and mMenuActive) k["old_insert"] = s::to_string(mMenuIndex + 1);
  if (mOpts.get(OPT_ALWAYSLASTPROMPT)) k["last_prompt"] = "yes";
  k["pattern_insert"] = "menu";
  if (mOpts.get(OPT_GLOBCOMPLETE)) k["pattern_match"] = "*";
  str lm = mZle.param("LISTMAX");
  k["list_max"] = lm.empty() ? "100" : lm;
  k["restore"]  = "auto";
  k["to_end"]   = mOpts.get(OPT_ALWAYSTOEND) or mBuf.cursor >= ctx.word_end ? "match" : "single";
  k["vared"]    = mVaredName;
  req.rec_exact = mOpts.get(OPT_RECEXACT);

  CompletionResult r = fn.empty() ? mZle.completion->complete(mBuf.text, cursor, req)
                                  : mZle.completion->complete_with(mBuf.text, cursor, fn, req);
  if (not r.function) r.keys = req.keys;
  if (r.keys["old_list"] == "keep" and mLastResult) {
    auto keys = s::move(r.keys);
    r = *mLastResult;
    r.keys = s::move(keys);
  }
  return apply_completion(r, reverse);
}

bool Editor::apply_completion(CompletionResult& r, bool reverse) {
  vec<s::size_t> real;   // the matches that can go into the line
  for (s::size_t i = 0; i < r.matches.size(); ++i)
    if (not r.matches[i].empty) real.push_back(i);
  bool unique = real.size() == 1 and r.matches.size() == 1;
  opt<str> insert;
  if (auto it = r.keys.find("insert"); it != r.keys.end() and not it->second.empty())
    insert = it->second;
  // Matches found by matching the line as a pattern go in by menu
  // completion while compstate[pattern_insert] is `menu` (zsh's default);
  // otherwise the common prefix goes in, as for other matches.
  if (r.haspattern and insert and insert->find("unambiguous") != str::npos
      and r.keys["pattern_insert"] == "menu" and real.size() > 1)
    insert = "menu";
  str list = r.keys["list"];
  mLastResult = s::make_unique<CompletionResult>(r);

  if (real.empty()) {
    if (not list.empty()) list_matches(r, list);
    return false;
  }
  if (insert and insert->find("tab") != str::npos) {
    // Just the key that ran the widget, as self-insert would.
    str key = mKeys.empty() ? str("\t") : mKeys.substr(mKeys.size() - 1);
    mBuf.insert(key);
    return true;
  }
  str before = mBuf.text.substr(r.start, r.end - r.start);
  if (not r.function) {
    // The builtin completers: the common prefix of the words, quoted as
    // the word is, when it adds to what is there.
    vec<str> words;
    for (s::size_t i : real) words.push_back(r.matches[i].word);
    str lcp = common_prefix_of(words);
    r.unambiguous = lcp.size() > r.prefix.size() ? comp_quote(lcp, r.quote) : before;
    r.unambiguous_cursor = r.unambiguous.size();
  }
  bool inserted = false, started_menu = false;
  str ins = insert.value_or("");
  // A number: that match (wrapping round; a negative one inserts nothing,
  // as in zsh 5.9), as menu completion would unless it ends in a space.
  char* rest = nullptr;
  long num = ins.empty() ? 0 : s::strtol(ins.c_str(), &rest, 10);
  bool numeric = not ins.empty() and rest != ins.c_str() and (*rest == '\0' or str(rest) == " ");
  if (ins.empty()) {
    // Leave the line alone.
  } else if (ins == "all") {
    str text;
    for (s::size_t i : real) {
      if (r.matches[i].all) continue;
      text += menu_text(r, r.matches[i]) + " ";
    }
    mBuf.replace(r.start, r.end - r.start, text);
    mBuf.cursor = r.start + text.size();
    inserted = true;
  } else if (numeric) {
    if (num >= 0) {
      long n = static_cast<long>(real.size());
      long at = ((num - 1) % n + n) % n;
      insert_match(r, real[static_cast<s::size_t>(at)], *rest == ' ');
      inserted = true;
    }
  } else if (unique) {
    insert_match(r, real[0], true);
    inserted = true;
  } else if (auto e = exact_match(r); e) {
    // compstate[exact] (REC_EXACT): the match that is the word on the
    // line is taken, though others start with it.
    insert_match(r, *e, true);
    inserted = unique = true;
  } else if (ins.starts_with("menu")
             or (ins.starts_with("automenu") and ins != "automenu-unambiguous")) {
    mMenu.clear();
    for (s::size_t i : real) {
      const CompCandidate& m = r.matches[i];
      mMenu.emplace_back(menu_text(r, m), auto_suffix(m, false));
    }
    auto colon = ins.find(':');
    // menu:N starts at match N.
    s::size_t first = colon == str::npos
                    ? 0 : static_cast<s::size_t>(s::max(1, s::atoi(ins.c_str() + colon + 1)) - 1);
    mMenuIndex = reverse and colon == str::npos ? mMenu.size() - 1 : first % mMenu.size();
    mMenuStart = r.start;
    insert_match(r, real[mMenuIndex], false);
    mMenuEnd    = mBuf.cursor;
    mMenuActive = true;
    started_menu = true;
  } else if (r.unambiguous != before and not r.keep_word) {
    mBuf.replace(r.start, r.end - r.start, r.unambiguous);
    mBuf.cursor = r.start + s::min(r.unambiguous_cursor, r.unambiguous.size());
    inserted = true;
  } else if (r.function and r.unambiguous == before and not r.keep_word) {
    // The same text: the cursor still goes where the matches differ.
    mBuf.cursor = r.start + s::min(r.unambiguous_cursor, r.unambiguous.size());
  }

  // Listing, as $compstate[list] says.
  bool force = list.find("force") != str::npos;
  bool many  = real.size() > 1 or force;
  bool show  = false;
  if (list.empty())                       show = false;
  else if (started_menu)                  show = not mListed;
  else if (list.starts_with("list"))      show = true;   // asked for: even one match
  else if (list.starts_with("autolist"))  show = many and not (unique and inserted);
  else if (list.starts_with("ambiguous")) show = many and not inserted;
  if (unique and inserted and not force) show = false;
  if (show) list_matches(r, list);
  else if (not inserted and not started_menu) beep();
  // A Tab that added nothing makes the next one start the menu.
  mCompleteLine = inserted ? str{} : mBuf.text;
  return true;
}

bool Editor::complete_widget(strv builtin, const str& fn) {
  strv w = builtin.starts_with('.') ? builtin.substr(1) : builtin;
  if (w == "delete-char-or-list" and mBuf.cursor < mBuf.text.size()) return w_delete_char();
  str saved = s::exchange(mWidgetStyle, str(builtin));
  bool list = w == "list-choices" or w == "delete-char-or-list";
  bool menu = w == "menu-complete" or w == "menu-expand-or-complete"
           or w == "reverse-menu-complete";
  bool ok = do_complete(list, menu, w == "reverse-menu-complete", fn);
  mWidgetStyle = s::move(saved);
  return ok;
}

// zsh's usetab: Tab alone, with nothing but blanks before the cursor on
// its line, inserts itself instead of completing (not in a zle -C
// widget, where compfunc decides).
bool Editor::use_tab() const {
  if (mKeys != "\t" or mZle.comp_widgets.contains(mWidget)) return false;
  s::size_t b = line_start(mBuf.cursor);
  return strv(mBuf.text).substr(b, mBuf.cursor - b).find_first_not_of(" \t") == strv::npos;
}

bool Editor::w_expand_or_complete() {
  return use_tab() ? w_self_insert() : do_complete(false, false);
}

bool Editor::w_complete_word() { return use_tab() ? w_self_insert() : do_complete(false, false); }

bool Editor::w_menu_complete() { return use_tab() ? w_self_insert() : do_complete(false, true); }
bool Editor::w_reverse_menu_complete()  { return do_complete(false, true, true); }
bool Editor::w_list_choices()           { return do_complete(true, false); }

}  // namespace jfsh
