#include <cerrno>
#include <cstdio>
#include <cstring>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <jfsh/builtins/builtins.hh>
#include <jfsh/exec/executor.hh>
#include <jfsh/lexer/input.hh>
#include <jfsh/parser/parser.hh>

namespace jfsh {

static BuiltinTable s_builtins;

Executor::Executor(ExecContext& ctx) : mCtx(ctx) {
  static bool initialized = false;
  if (not initialized) {
    s_builtins.register_all();
    initialized = true;
  }
}

int Executor::exec(const CompiledProgram& prog) {
  if (prog.empty()) return 0;
  ExecState es;
  es.prog = &prog;
  es.pc   = 0;
  return exec_list(es, EF_NONE);
}

int Executor::exec_string(std::string_view src) {
  StringInputSource in{std::string(src)};
  Lexer lex{in};
  // Comments are always recognized in scripts and -c strings, but
  // interactively only with INTERACTIVE_COMMENTS.
  if (not mCtx.opts.interactive() or mCtx.opts.get(OPT_INTERACTIVECOMMENTS))
    lex.push_state(LEX_SKIPCOMMENTS);
  Parser parser{lex};
  auto prog = parser.parse();
  if (not prog) {
    std::fprintf(stderr, "jfsh:%d: %s\n",
                 prog.error().code, prog.error().message.c_str());
    mCtx.last_exit = 1;
    return 1;
  }
  return exec(**prog);
}

// Value an assignment in front of a command gives its name for that command
// only.  Fails, like a real assignment would, on a readonly parameter.
static Result<std::string> prefix_value(const ParamTable& params,
                                        const Assignment& a) {
  auto existing = params.lookup(a.name);
  if (existing and existing->get().is_readonly())
    return std::unexpected(ShellError{ErrorKind::ExecError,
        "read-only variable: " + a.name});
  if (a.append and existing) return existing->get().scalar() + a.value;
  return a.value;
}

int Executor::exec_simple(std::vector<std::string> args,
                          std::vector<Assignment>  assigns,
                          ExecFlags flags) {
  args = expand_words(args);
  for (auto& a : assigns) a.value = expand_value(a.value);
  if (mCtx.opts.xtrace()) trace(args, assigns);

  // With no command word the assignments set shell parameters.
  if (args.empty()) return apply_assigns(assigns);

  // TODO(M4): shell functions are looked up here, before builtins.  Aliases
  //   never reach this point: they are expanded by the lexer (M7).
  if (auto def = s_builtins.find(args[0])) {
    // zsh keeps assignments in front of a special builtin only under
    // POSIX_BUILTINS; otherwise they last while the builtin runs.
    if ((def->get().flags & BINF_SPECIAL) and mCtx.opts.get(OPT_POSIXBUILTINS)) {
      if (int rc = apply_assigns(assigns)) return rc;
      return def->get().fn(args, mCtx);
    }
    mCtx.params.push_scope();
    int rc = 0;
    for (const auto& a : assigns) {
      auto value = prefix_value(mCtx.params, a);
      if (not value) {
        std::fprintf(stderr, "jfsh: %s\n", value.error().message.c_str());
        rc = 1;
        break;
      }
      auto outer = mCtx.params.lookup(a.name);
      std::uint32_t attrs = PM_SCALAR;
      if (outer and outer->get().is_exported()) attrs |= PM_EXPORTED;
      mCtx.params.assign(a.name, std::move(*value), attrs);
    }
    if (rc == 0) rc = def->get().fn(args, mCtx);
    mCtx.params.pop_scope();
    return rc;
  }
  return run_external(args, assigns);
}

int Executor::exec_list(ExecState& es, ExecFlags flags) {
  int rc = 0;
  while (not es.at_end()) {
    WordCode w = es.fetch();
    if (wc_code(w) != WC_LIST) break;  // WC_END
    std::size_t end = es.pc + wc_list_skip(w);
    if (wc_list_flags(w) & WC_LIST_ASYNC) {
      // TODO(M2): run the sublist as a background job.
      std::fprintf(stderr, "jfsh: background jobs (&) are not supported yet\n");
      rc = mCtx.last_exit = 1;
    } else {
      rc = exec_sublist(es, flags);
    }
    es.pc = end;
    if (mCtx.exiting) return mCtx.exit_val;
  }
  return rc;
}

// Runs a chain of pipelines joined by && / ||.  A pipeline whose connector
// rules it out is skipped, but the chain goes on: in `false && a || b`, `a`
// is skipped and `b` runs.
int Executor::exec_sublist(ExecState& es, ExecFlags flags) {
  int  rc   = 0;
  bool skip = false;
  for (;;) {
    WordCode    w   = es.fetch();  // WC_SUBLIST
    WordCode    f   = wc_sublist_flags(w);
    std::size_t end = es.pc + wc_sublist_skip(w);
    if (not skip) {
      rc = exec_pline(es, flags);
      if (f & WC_SUBLIST_NOT) rc = (rc == 0) ? 1 : 0;
      mCtx.last_exit = rc;
    }
    es.pc = end;
    if (mCtx.exiting) return rc;
    WordCode type = f & WC_SUBLIST_TYPE;
    if (type == WC_SUBLIST_END) return rc;
    skip = (type == WC_SUBLIST_AND) ? rc != 0 : rc == 0;
  }
}

int Executor::exec_pline(ExecState& es, ExecFlags flags) {
  WordCode n = wc_data(es.fetch());  // WC_PIPE
  if (n != 1) {
    // TODO(M2): pipelines.
    std::fprintf(stderr, "jfsh: pipelines are not supported yet\n");
    return 1;
  }
  return exec_cmd(es, flags);
}

int Executor::exec_cmd(ExecState& es, ExecFlags flags) {
  if (es.at_end()) return 0;
  WordCode w  = es.peek();
  WordCode op = wc_code(w);
  switch (op) {
    case WC_ASSIGN:
    case WC_SIMPLE:  return exec_simple_cmd(es, flags);
    case WC_IF:      { es.fetch(); return exec_if(es);           }
    case WC_WHILE:   { es.fetch(); return exec_while(es);        }
    case WC_FOR:     { es.fetch(); return exec_for(es);          }
    case WC_CASE:    { es.fetch(); return exec_case(es);         }
    case WC_SELECT:  { es.fetch(); return exec_select(es);       }
    case WC_REPEAT:  { es.fetch(); return exec_repeat(es);       }
    case WC_SUBSH:   { es.fetch(); return exec_subsh(es, flags); }
    case WC_FUNCDEF: { es.fetch(); return exec_funcdef(es);      }
    case WC_COND:    { es.fetch(); return exec_cond(es);         }
    case WC_ARITH:   { es.fetch(); return exec_arith(es);        }
    case WC_ALWAYS:  { es.fetch(); return exec_always(es);       }
    default:         return 0;
  }
}

// simple := (WC_ASSIGN WC_STRING(name) WC_STRING(value))*
//           WC_SIMPLE(argc) WC_STRING(word){argc}
int Executor::exec_simple_cmd(ExecState& es, ExecFlags flags) {
  std::vector<Assignment> assigns;
  while (wc_code(es.peek()) == WC_ASSIGN) {
    bool append = wc_data(es.fetch()) & WC_ASSIGN_APPEND;
    std::string name{es.fetch_str()};
    std::string value{es.fetch_str()};
    assigns.push_back({std::move(name), std::move(value), append});
  }
  WordCode argc = wc_data(es.fetch());  // WC_SIMPLE
  std::vector<std::string> words;
  words.reserve(argc);
  for (WordCode i = 0; i < argc; ++i) words.emplace_back(es.fetch_str());
  return exec_simple(std::move(words), std::move(assigns), flags);
}

// TODO(exec/if): Read WC_IF bytecode layout:
//   WC_IF cond_len then_len [elif_cond_len elif_then_len]* [else_len] WC_END
//   Execute cond_list; if exit==0 execute then_list, else try elif chain / else.
int Executor::exec_if(ExecState&) { return 0; }

// TODO(exec/while): WC_WHILE has a cond_prog and a body_prog offset.
//   Loop: exec cond; if exit!=0 (or ==0 for `until`) break; exec body;
//   Respect ctx_.break_level / ctx_.cont_level.
int Executor::exec_while(ExecState&) { return 0; }

// TODO(exec/for): Two variants:
//   WC_FOR word_list body — foreach word in list; exec body
//   WC_FOR_COND init cond incr body — C-style arithmetic for
int Executor::exec_for(ExecState&) { return 0; }

// TODO(exec/case): Pattern list is a series of (pattern, body) pairs terminated
//   by WC_END. Glob-match each pattern against the subject word.
int Executor::exec_case(ExecState&) { return 0; }

// TODO(exec/select): Like for but prints a numbered menu, reads $REPLY,
//   re-runs body until break.
int Executor::exec_select(ExecState&) { return 0; }

// TODO(exec/repeat): exec body N times (N is the word before the body).
int Executor::exec_repeat(ExecState&) { return 0; }

// TODO(exec/subsh): fork() a subshell, run the body in child, waitpid in parent.
//   With EF_NOFORK, exec directly (last command optimisation).
int Executor::exec_subsh(ExecState&, ExecFlags) { return 0; }

// TODO(exec/funcdef): Register a named function in a function table.
//   The body is a CompiledProgram offset stored in the bytecode.
//   The function table lives on ParamTable or a separate HashTable<ProgRef>.
int Executor::exec_funcdef(ExecState&) { return 0; }

// TODO(exec/cond): Evaluate a [[ ]] expression tree.
//   Opcodes inside the cond block: WC_COND_STR, WC_COND_REGEX, WC_COND_AND/OR/NOT
//   Delegate to builtins/cond.cpp evaluator.
int Executor::exec_cond(ExecState&) { return 0; }

// TODO(exec/arith): Evaluate (( expr )).
//   Fetch the expression string from the strs table, parse and compute
//   with the arithmetic evaluator in builtins/math.cpp.
//   Returns 0 if result != 0, 1 if result == 0 (like zsh).
int Executor::exec_arith(ExecState&) { return 0; }

// TODO(exec/always): { body } always { cleanup }
//   Run body; whether it exits normally or via break/continue/return/exit,
//   always run the cleanup block before propagating the control flow.
int Executor::exec_always(ExecState&) { return 0; }

// TODO(exec/redirects): Parse redirection records from ExecState, open file
//   descriptors (open/pipe/dup2/close), save originals for restoration after
//   the command.  Types: REDIR_READ, REDIR_WRITE, REDIR_APPEND, REDIR_HEREDOC,
//   REDIR_HERESTR, REDIR_MERGE, REDIR_PROC_IN/OUT (process substitution), etc.
void Executor::setup_redirects(ExecState&) {}

// Quote removal, the last step of word expansion: '…' is literal, "…"
// keeps backslash only before $ ` " \ and newline, and an unquoted
// backslash escapes the next character.  A backslash-newline disappears.
static std::string remove_quotes(std::string_view w) {
  constexpr std::string_view kDquoteEscapes = "$`\"\\\n";
  enum class Quote { None, Single, Double } q = Quote::None;
  std::string out;
  out.reserve(w.size());
  for (std::size_t i = 0; i < w.size(); ++i) {
    char c = w[i];
    bool escape = c == '\\' and i + 1 < w.size()
        and (q == Quote::None
             or (q == Quote::Double
                 and kDquoteEscapes.find(w[i + 1]) != std::string_view::npos));
    if (q == Quote::Single) {
      if (c == '\'') q = Quote::None;
      else           out += c;
    } else if (escape) {
      if (w[i + 1] != '\n') out += w[i + 1];
      ++i;
    } else if (q == Quote::Double) {
      if (c == '"') q = Quote::None;
      else          out += c;
    } else if (c == '\'') {
      q = Quote::Single;
    } else if (c == '"') {
      q = Quote::Double;
    } else {
      out += c;
    }
  }
  return out;
}

// TODO(exec/expand): Word expansion pipeline (mirrors zsh's prefork/postfork):
//   1. Tilde expansion  (~, ~user)
//   2. Parameter expansion  ($var, ${var:-default}, ${#var}, ${var##pat}, etc.)
//   3. Command substitution  $(cmd), `cmd`
//   4. Arithmetic substitution  $((expr))
//   5. Word splitting  (IFS)
//   6. Glob expansion  (*, ?, [...], **/...)
//   7. Quote removal
//   Only step 7 is implemented so far.
std::vector<std::string> Executor::expand_words(
    const std::vector<std::string>& words) {
  std::vector<std::string> out;
  out.reserve(words.size());
  for (const auto& w : words) out.push_back(remove_quotes(w));
  return out;
}

// Assignment values get the same expansions minus word splitting and
// globbing.
std::string Executor::expand_value(std::string_view value) {
  return remove_quotes(value);
}

// TODO(exec/assign): Array and hash assignments, arr=(a b c) / hash=(k v),
//   once the lexer keeps `name=(` together (bug B8).
int Executor::apply_assigns(const std::vector<Assignment>& assigns) {
  for (const auto& a : assigns) {
    if (auto r = mCtx.params.set_scalar(a.name, a.value, a.append); not r) {
      std::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
      return 1;
    }
  }
  return 0;
}

// Quotes a word for xtrace output only when it needs it, as zsh does.
static std::string trace_quote(std::string_view w) {
  constexpr std::string_view kSafe =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
      "_@%+=:,./-";
  if (not w.empty() and w.find_first_not_of(kSafe) == std::string_view::npos)
    return std::string(w);
  std::string out = "'";
  for (char c : w) {
    if (c == '\'') out += "'\\''";
    else           out += c;
  }
  return out + "'";
}

// TODO(M9): PS4 is printed as is; it should go through prompt expansion
//   (zsh's default is `+%N:%i> `).
void Executor::trace(const std::vector<std::string>& args,
                     const std::vector<Assignment>&  assigns) {
  auto ps4 = mCtx.params.lookup("PS4");
  std::string line = (ps4 and ps4->get().is_set()) ? ps4->get().scalar() : "+ ";
  bool first = true;
  auto add = [&](const std::string& word) {
    if (not first) line += ' ';
    line += word;
    first = false;
  };
  for (const auto& a : assigns)
    add(a.name + (a.append ? "+=" : "=") + trace_quote(a.value));
  for (const auto& w : args) add(trace_quote(w));
  std::fprintf(stderr, "%s\n", line.c_str());
}

bool Executor::is_builtin(std::string_view name) const noexcept {
  return s_builtins.find(name).has_value();
}

int Executor::call_builtin(std::string_view name,
                           std::vector<std::string>& args,
                           ExecContext& ctx) {
  if (auto def = s_builtins.find(name)) return def->get().fn(args, ctx);
  return 127;
}

int Executor::call_function(std::string_view /*name*/,
                            std::vector<std::string>& /*args*/) {
  return 127;
}

int Executor::do_fork(ExecState&, ExecFlags) { return 0; }

static bool is_executable_file(const std::string& path) {
  struct stat st{};
  return stat(path.c_str(), &st) == 0 and S_ISREG(st.st_mode)
     and access(path.c_str(), X_OK) == 0;
}

Result<std::string> Executor::find_command(std::string_view name) {
  if (name.find('/') != std::string_view::npos) return std::string(name);

  std::string path_var;
  if (auto p = mCtx.params.lookup("PATH"); p and p->get().is_set())
    path_var = p->get().scalar();
  if (path_var != mHashedPath) {
    mCmdHash.clear();
    mHashedPath = path_var;
  }
  bool hashing = mCtx.opts.get(OPT_HASHCMDS);
  std::string key(name);
  if (hashing) {
    auto it = mCmdHash.find(key);
    if (it != mCmdHash.end() and is_executable_file(it->second)) return it->second;
  }

  // An empty $PATH component means the current directory.  A matching file
  // that is not executable is remembered, so the error can say why.
  bool denied = false;
  std::size_t start = 0;
  while (not path_var.empty() and start <= path_var.size()) {
    std::size_t end = path_var.find(':', start);
    if (end == std::string::npos) end = path_var.size();
    std::string dir = path_var.substr(start, end - start);
    start = end + 1;
    std::string candidate = (dir.empty() ? "." : dir) + "/" + key;
    if (is_executable_file(candidate)) {
      if (hashing) mCmdHash[key] = candidate;
      return candidate;
    }
    struct stat st{};
    if (stat(candidate.c_str(), &st) == 0 and S_ISREG(st.st_mode)) denied = true;
  }
  if (denied)
    return std::unexpected(ShellError{ErrorKind::PermissionDenied,
        "permission denied: " + key});
  return std::unexpected(ShellError{ErrorKind::FileNotFound,
      "command not found: " + key});
}

static std::vector<char*> c_strings(std::vector<std::string>& v) {
  std::vector<char*> out;
  out.reserve(v.size() + 1);
  for (auto& s : v) out.push_back(s.data());
  out.push_back(nullptr);
  return out;
}

int Executor::run_external(const std::vector<std::string>& args,
                           const std::vector<Assignment>&  assigns) {
  auto path = find_command(args[0]);
  if (not path) {
    std::fprintf(stderr, "jfsh: %s\n", path.error().message.c_str());
    return path.error().kind == ErrorKind::PermissionDenied ? 126 : 127;
  }

  // The child's environment is the shell's exported parameters plus the
  // command's prefix assignments, which are exported to it alone.
  std::vector<std::string> env = mCtx.params.make_envp();
  for (const auto& a : assigns) {
    auto value = prefix_value(mCtx.params, a);
    if (not value) {
      std::fprintf(stderr, "jfsh: %s\n", value.error().message.c_str());
      return 1;
    }
    std::string prefix = a.name + "=";
    std::erase_if(env, [&](const std::string& e) { return e.starts_with(prefix); });
    env.push_back(prefix + *value);
  }
  std::vector<std::string> argv_s = args;
  auto argv = c_strings(argv_s);
  auto envp = c_strings(env);

  // Builtins write through stdio; flush so their output precedes the child's.
  std::fflush(nullptr);
  pid_t pid = fork();
  if (pid < 0) {
    std::fprintf(stderr, "jfsh: fork failed: %s\n", std::strerror(errno));
    return 1;
  }
  if (pid == 0) {
    execve(path->c_str(), argv.data(), envp.data());
    int err = errno;
    if (err == ENOEXEC) {
      // Neither a binary nor a #! script: run it with /bin/sh, as zsh does.
      std::vector<std::string> sh_s{"/bin/sh", *path};
      sh_s.insert(sh_s.end(), args.begin() + 1, args.end());
      auto sh_argv = c_strings(sh_s);
      execve("/bin/sh", sh_argv.data(), envp.data());
      err = errno;
    }
    const char* name = args[0].c_str();
    if (err == ENOENT) {
      std::fprintf(stderr, "jfsh: no such file or directory: %s\n", name);
      _exit(127);
    }
    if (err == EACCES or err == EISDIR or err == EPERM)
      std::fprintf(stderr, "jfsh: permission denied: %s\n", name);
    else
      std::fprintf(stderr, "jfsh: %s: %s\n", name, std::strerror(err));
    _exit(126);
  }

  // TODO(M6): wait with WUNTRACED and record stopped jobs.
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      std::fprintf(stderr, "jfsh: waitpid: %s\n", std::strerror(errno));
      return 1;
    }
  }
  if (WIFEXITED(status))   return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 1;
}

}  // namespace jfsh
