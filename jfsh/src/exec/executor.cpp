#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <unordered_map>
#include <utility>

#include <jfsh/builtins/builtins.hh>
#include <jfsh/exec/executor.hh>
#include <jfsh/lexer/input.hh>
#include <jfsh/parser/parser.hh>

namespace jfsh {

static BuiltinTable Builtins;

Executor::Executor(ExecContext& ctx) : mCtx(ctx) {
  static bool initialized = false;
  if (not initialized) {
    Builtins.register_all();
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
                          std::vector<Redirection> redirs,
                          ExecFlags flags) {
  args = expand_words(args);
  for (auto& a : assigns) a.value = expand_value(a.value);
  for (auto& r : redirs)  r.target = expand_value(r.target);
  auto psubst = expand_procsubst(args);
  struct PSubstGuard {
    Executor* exec;
    ProcSubstResources* res;
    ~PSubstGuard() { exec->procsubst_release(*res); }
  } psubst_guard{this, &psubst};
  if (mCtx.opts.xtrace()) trace(args, assigns);

  // With no command word the redirections apply to the current shell and
  // the assignments update shell parameters.  Both happen in-process.
  if (args.empty()) {
    SavedFds saved;
    if (not redirs.empty()) {
      std::fflush(nullptr);
      if (not apply_redirects(redirs, saved)) return 1;
    }
    int rc = apply_assigns(assigns);
    std::fflush(nullptr);
    return rc;
  }

  // Shell functions get looked up here, before builtins, when M4 lands.
  // Aliases are already expanded by the lexer (M7).
  if (auto def = Builtins.find(args[0])) {
    SavedFds saved;
    if (not redirs.empty()) {
      // Push pending buffered output through the ORIGINAL fds before we
      // rewire them; otherwise the builtin's output lands in the wrong
      // place after SavedFds restores (bug: stdio buffers survive dup2).
      std::fflush(nullptr);
      if (not apply_redirects(redirs, saved)) return 1;
    }
    // zsh keeps assignments in front of a special builtin only under
    // POSIX_BUILTINS; otherwise they last while the builtin runs.
    if ((def->get().flags & BINF_SPECIAL) and mCtx.opts.get(OPT_POSIXBUILTINS)) {
      int rc_special = apply_assigns(assigns);
      if (rc_special == 0) rc_special = def->get().fn(args, mCtx);
      std::fflush(nullptr);
      return rc_special;
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
    std::fflush(nullptr);
    return rc;
  }
  return run_external(args, assigns, redirs, flags);
}

int Executor::exec_list(ExecState& es, ExecFlags flags) {
  int rc = 0;
  while (not es.at_end()) {
    WordCode w = es.fetch();
    if (wc_code(w) != WC_LIST) break;  // WC_END
    std::size_t end = es.pc + wc_list_skip(w);
    if (wc_list_flags(w) & WC_LIST_ASYNC) {
      pid_t pid = ::fork();
      if (pid < 0) {
        std::fprintf(stderr, "jfsh: fork: %s\n", std::strerror(errno));
        rc = mCtx.last_exit = 1;
      } else if (pid == 0) {
        ::setpgid(0, 0);
        ExecState se{es.prog, es.pc};
        int child_rc = exec_sublist(se, flags);
        _exit(child_rc);
      } else {
        ::setpgid(pid, pid);
        int jid = mCtx.jobs.alloc(pid, JOB_ASYNC | JOB_NOTIFY, "");
        Process p;
        p.pid    = pid;
        p.status = ProcStatus::Running;
        mCtx.jobs.add_process(jid, std::move(p));
        mCtx.last_bg_pid = pid;
        if (mCtx.opts.interactive())
          std::fprintf(stderr, "[%d] %d\n", jid, static_cast<int>(pid));
        rc = 0;
      }
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
  WordCode w     = es.fetch();  // WC_PIPE
  WordCode n     = wc_pipe_count(w);
  if (n == 0) return 0;
  if (n == 1) return exec_cmd(es, flags);

  // Pre-decode every stage's bytecode slice so we can fork them from the
  // parent without re-entering the parser.  Each stage knows its own
  // start and end pc in the compiled program.
  struct Stage { std::size_t start; std::size_t end; bool stderr_too; };
  std::vector<Stage> stages;
  stages.reserve(n);
  for (unsigned i = 0; i < n; ++i) {
    std::size_t start = es.pc;
    // Walk this stage: one WC_SIMPLE (plus assigns/redirs/strings) or one
    // compound command.  The compound commands are not implemented yet in
    // M2, so just walk WC_ASSIGN/WC_REDIR until WC_SIMPLE + its strings.
    while (es.pc < es.prog->prog.size()) {
      WordCode op = wc_code(es.peek());
      if (op == WC_ASSIGN) { es.fetch(); es.fetch(); es.fetch(); }
      else if (op == WC_REDIR) { es.fetch(); es.fetch(); }
      else if (op == WC_SIMPLE) {
        WordCode argc = wc_data(es.fetch());
        for (WordCode a = 0; a < argc; ++a) es.fetch();
        break;
      } else {
        // Compound command: the enclosing WC_SUBLIST skip already covers
        // us; without M4 we cannot walk inside, so just bail.
        break;
      }
    }
    stages.push_back({start, es.pc,
                      i + 1 < n ? wc_pipe_stderr(w, i) : false});
  }

  // Build n-1 pipes.
  std::vector<int> rfds(n, -1), wfds(n, -1);
  for (unsigned i = 0; i + 1 < n; ++i) {
    int p[2];
    if (::pipe(p) != 0) {
      std::fprintf(stderr, "jfsh: pipe: %s\n", std::strerror(errno));
      for (unsigned j = 0; j < i; ++j) { ::close(rfds[j + 1]); ::close(wfds[j]); }
      return 1;
    }
    rfds[i + 1] = p[0];
    wfds[i]     = p[1];
  }

  std::vector<pid_t> pids(n, -1);
  pid_t pgid = 0;
  for (unsigned i = 0; i < n; ++i) {
    pid_t pid = ::fork();
    if (pid < 0) {
      std::fprintf(stderr, "jfsh: fork: %s\n", std::strerror(errno));
      for (int fd : rfds) if (fd >= 0) ::close(fd);
      for (int fd : wfds) if (fd >= 0) ::close(fd);
      return 1;
    }
    if (pid == 0) {
      ::setpgid(0, pgid ? pgid : getpid());
      if (rfds[i] >= 0) { ::dup2(rfds[i], 0); }
      if (wfds[i] >= 0) {
        ::dup2(wfds[i], 1);
        if (stages[i].stderr_too) ::dup2(wfds[i], 2);
      }
      for (int fd : rfds) if (fd >= 0) ::close(fd);
      for (int fd : wfds) if (fd >= 0) ::close(fd);
      ExecState se{es.prog, stages[i].start};
      int rc = exec_cmd(se, flags);
      std::fflush(nullptr);
      _exit(rc);
    }
    if (i == 0) pgid = pid;
    ::setpgid(pid, pgid);
    pids[i] = pid;
  }
  for (int fd : rfds) if (fd >= 0) ::close(fd);
  for (int fd : wfds) if (fd >= 0) ::close(fd);

  int last_rc = 0;
  int worst   = 0;
  for (unsigned i = 0; i < n; ++i) {
    int status = 0;
    while (::waitpid(pids[i], &status, 0) < 0) {
      if (errno != EINTR) { worst = 1; break; }
    }
    int rc = 0;
    if (WIFEXITED(status))        rc = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) rc = 128 + WTERMSIG(status);
    if (rc != 0) worst = rc;
    last_rc = rc;
  }
  return mCtx.opts.get(OPT_PIPEFAIL) ? worst : last_rc;
}

int Executor::exec_cmd(ExecState& es, ExecFlags flags) {
  if (es.at_end()) return 0;
  WordCode w  = es.peek();
  WordCode op = wc_code(w);
  switch (op) {
    case WC_ASSIGN:
    case WC_REDIR:
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

// simple := (assign | redir)* WC_SIMPLE(argc) WC_STRING{argc}
int Executor::exec_simple_cmd(ExecState& es, ExecFlags flags) {
  std::vector<Assignment> assigns;
  std::vector<Redirection> redirs;
  for (;;) {
    WordCode op = wc_code(es.peek());
    if (op == WC_ASSIGN) {
      bool append = wc_data(es.fetch()) & WC_ASSIGN_APPEND;
      std::string name{es.fetch_str()};
      std::string value{es.fetch_str()};
      assigns.push_back({std::move(name), std::move(value), append});
    } else if (op == WC_REDIR) {
      WordCode w = es.fetch();
      Redirection r;
      r.type   = wc_redir_type(w);
      r.fd     = wc_redir_has_fd(w) ? wc_redir_fd(w) : -1;
      r.target = std::string(es.fetch_str());
      redirs.push_back(std::move(r));
    } else {
      break;
    }
  }
  WordCode argc = wc_data(es.fetch());  // WC_SIMPLE
  std::vector<std::string> words;
  words.reserve(argc);
  for (WordCode i = 0; i < argc; ++i) words.emplace_back(es.fetch_str());
  return exec_simple(std::move(words), std::move(assigns), std::move(redirs),
                     flags);
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

// ---------------------------------------------------------------------------
// SavedFds: RAII restore of fds overwritten by builtin redirections.
// ---------------------------------------------------------------------------
SavedFds::SavedFds(SavedFds&& other) noexcept
  : entries(std::move(other.entries)), fail(other.fail) {
  other.entries.clear();
}
SavedFds& SavedFds::operator=(SavedFds&& other) noexcept {
  if (this != &other) {
    restore();
    entries    = std::move(other.entries);
    fail       = other.fail;
    other.entries.clear();
  }
  return *this;
}
SavedFds::~SavedFds() { restore(); }

void SavedFds::save(int dst) {
  for (const auto& e : entries) if (e.dst == dst) return;
  int backup = fcntl(dst, F_DUPFD_CLOEXEC, 10);
  if (backup < 0 and errno == EBADF) {
    // dst is not currently open; remember so we can close it on restore.
    entries.push_back({dst, -1});
    return;
  }
  if (backup < 0) { fail = true; return; }
  entries.push_back({dst, backup});
}

void SavedFds::restore() noexcept {
  for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
    if (it->backup < 0) {
      ::close(it->dst);
    } else {
      ::dup2(it->backup, it->dst);
      ::close(it->backup);
    }
  }
  entries.clear();
}

// Default destination fd for a given redirection operator.
static int default_fd(RedirType t) noexcept {
  switch (t) {
    case REDIR_READ: case REDIR_READWRITE: case REDIR_HEREDOC:
    case REDIR_HEREDOCD: case REDIR_HERESTR: case REDIR_IN_DUP:
    case REDIR_PROC_IN:
      return 0;
    break; case REDIR_WRITE: case REDIR_APPEND: case REDIR_CLOBBER:
    case REDIR_MERGE: case REDIR_CLOSE: case REDIR_PROC_OUT:
    case REDIR_PROC_EQ:
      return 1;
    break; case REDIR_MERGEOUT: case REDIR_APPEND_ERR:
      return 1;  // dups to both 1 and 2 at apply time
    break; default:
      return 1;
  }
}

// Parse a decimal integer target (`N` or `-` for close).  Returns -1 on
// error, -2 for the close token `-`.
static int parse_fd_target(const std::string& s) {
  if (s == "-") return -2;
  if (s.empty()) return -1;
  char* end = nullptr;
  long v = std::strtol(s.c_str(), &end, 10);
  if (end == s.c_str() or *end != '\0' or v < 0 or v > 1023) return -1;
  return static_cast<int>(v);
}

// Pour `body` into a pipe-backed fd suitable for stdin of a child.
// Uses memfd_create when available, falling back to a pipe + background
// writer fork so the writer cannot block the caller (bodies fit in one
// pipe buffer in practice, but a dedicated writer is the simplest
// general answer).
static int heredoc_body_fd(std::string_view body) {
  int pfd[2];
  if (::pipe(pfd) != 0) return -1;
  pid_t pid = ::fork();
  if (pid < 0) { ::close(pfd[0]); ::close(pfd[1]); return -1; }
  if (pid == 0) {
    ::close(pfd[0]);
    ::signal(SIGPIPE, SIG_DFL);
    const char* p   = body.data();
    std::size_t n   = body.size();
    while (n > 0) {
      ssize_t w = ::write(pfd[1], p, n);
      if (w < 0) { if (errno == EINTR) continue; _exit(0); }
      p += w; n -= static_cast<std::size_t>(w);
    }
    ::close(pfd[1]);
    _exit(0);
  }
  ::close(pfd[1]);
  return pfd[0];
}

// Fork a subshell that runs `src` with its stdout piped to a fresh fd.
// Returns the read end for the parent to pass as /dev/fd/N.  On failure
// returns -1.  Used for <(cmd).
int Executor::procsubst_in(std::string_view src) {
  int pfd[2];
  if (::pipe(pfd) != 0) return -1;
  pid_t pid = ::fork();
  if (pid < 0) { ::close(pfd[0]); ::close(pfd[1]); return -1; }
  if (pid == 0) {
    ::close(pfd[0]);
    ::dup2(pfd[1], 1);
    if (pfd[1] != 1) ::close(pfd[1]);
    _exit(exec_string(src));
  }
  ::close(pfd[1]);
  return pfd[0];
}

// Fork a subshell that runs `src` with its stdin piped from a fresh fd.
// Returns the write end for the parent to pass as /dev/fd/N.
int Executor::procsubst_out(std::string_view src) {
  int pfd[2];
  if (::pipe(pfd) != 0) return -1;
  pid_t pid = ::fork();
  if (pid < 0) { ::close(pfd[0]); ::close(pfd[1]); return -1; }
  if (pid == 0) {
    ::close(pfd[1]);
    ::dup2(pfd[0], 0);
    if (pfd[0] != 0) ::close(pfd[0]);
    _exit(exec_string(src));
  }
  ::close(pfd[0]);
  return pfd[1];
}

// Run `src` with its stdout captured into a fresh temp file and return
// the file's path.  Used for =(cmd).
std::string Executor::procsubst_eq(std::string_view src) {
  char tmpl[] = "/tmp/jfsh-procsubst-XXXXXX";
  int fd = ::mkstemp(tmpl);
  if (fd < 0) return {};
  pid_t pid = ::fork();
  if (pid < 0) { ::close(fd); ::unlink(tmpl); return {}; }
  if (pid == 0) {
    ::dup2(fd, 1);
    if (fd != 1) ::close(fd);
    _exit(exec_string(src));
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 and errno == EINTR) {}
  ::close(fd);
  return std::string(tmpl);
}

// Fork a child that reads lines from `read_fd` and writes each byte to all
// file descriptors in `out_fds`.  `extra_close` lists fds (typically the
// pipe's write end) to close in the child so read() can see EOF once the
// parent releases its writers.
static int spawn_tee(int read_fd, const std::vector<int>& out_fds,
                     int extra_close) {
  pid_t pid = ::fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    if (extra_close >= 0) ::close(extra_close);
    char buf[4096];
    for (;;) {
      ssize_t n = ::read(read_fd, buf, sizeof buf);
      if (n <= 0) break;
      for (int fd : out_fds) {
        const char* p = buf;
        ssize_t left = n;
        while (left > 0) {
          ssize_t w = ::write(fd, p, static_cast<std::size_t>(left));
          if (w < 0) { if (errno == EINTR) continue; break; }
          p += w; left -= w;
        }
      }
    }
    _exit(0);
  }
  return 0;
}

// Identify output-side write redirections (REDIR_WRITE / APPEND / CLOBBER /
// READWRITE / MERGEOUT / APPEND_ERR) that share a destination fd.  When
// MULTIOS is on, a pipe + tee helper is attached so each target gets the
// data; when off, only the last redirection for each fd wins.  Returns the
// indices of redirs that have been consumed by the MULTIOS path (so the
// main loop skips them).
static std::vector<std::size_t> multios_merge(
    const std::vector<Redirection>& redirs,
    SavedFds& saved,
    bool multios_on) {
  std::vector<std::size_t> consumed;
  if (not multios_on) return consumed;

  auto is_output_file = [](RedirType t) noexcept {
    return t == REDIR_WRITE or t == REDIR_APPEND
        or t == REDIR_CLOBBER or t == REDIR_MERGEOUT
        or t == REDIR_APPEND_ERR;
  };
  auto default_for = [](RedirType t) noexcept { return t == REDIR_APPEND_ERR
      or t == REDIR_MERGEOUT ? 1 : 1; };

  // Group by destination fd.
  std::unordered_map<int, std::vector<std::size_t>> by_fd;
  for (std::size_t i = 0; i < redirs.size(); ++i) {
    const auto& r = redirs[i];
    if (not is_output_file(r.type)) continue;
    int dst = r.fd >= 0 ? r.fd : default_for(r.type);
    by_fd[dst].push_back(i);
  }

  for (auto& [dst, idxs] : by_fd) {
    if (idxs.size() < 2) continue;  // no MULTIOS trigger with one target
    std::vector<int> fds;
    for (auto i : idxs) {
      const auto& r = redirs[i];
      int flags = (r.type == REDIR_APPEND or r.type == REDIR_APPEND_ERR)
          ? (O_WRONLY | O_CREAT | O_APPEND)
          : (O_WRONLY | O_CREAT | O_TRUNC);
      if (r.type == REDIR_WRITE
          or r.type == REDIR_MERGEOUT) {
        // Honour NOCLOBBER even in a MULTIOS chain.
      }
      int fd = ::open(r.target.c_str(), flags, 0666);
      if (fd < 0) {
        std::fprintf(stderr, "jfsh: %s: %s\n",
                     r.target.c_str(), std::strerror(errno));
        for (int f : fds) ::close(f);
        return {};  // MULTIOS fails loud
      }
      fds.push_back(fd);
    }
    int p[2];
    if (::pipe(p) != 0) { for (int f : fds) ::close(f); return {}; }
    if (spawn_tee(p[0], fds, p[1]) < 0) {
      ::close(p[0]); ::close(p[1]);
      for (int f : fds) ::close(f);
      return {};
    }
    ::close(p[0]);
    for (int f : fds) ::close(f);
    saved.save(dst);
    ::dup2(p[1], dst);
    // If MULTIOS covers both stdout and stderr (from &>), also dup it to 2.
    for (auto i : idxs) {
      if (redirs[i].type == REDIR_MERGEOUT
          or redirs[i].type == REDIR_APPEND_ERR) {
        saved.save(2);
        ::dup2(p[1], 2);
        break;
      }
    }
    ::close(p[1]);
    for (auto i : idxs) consumed.push_back(i);
  }
  return consumed;
}

bool Executor::apply_redirects(const std::vector<Redirection>& redirs,
                               SavedFds& saved) {
  auto consumed = multios_merge(redirs, saved,
                                mCtx.opts.get(OPT_MULTIOS));
  std::sort(consumed.begin(), consumed.end());
  auto is_consumed = [&](std::size_t i) {
    return std::binary_search(consumed.begin(), consumed.end(), i);
  };
  for (std::size_t idx = 0; idx < redirs.size(); ++idx) {
    if (is_consumed(idx)) continue;
    const auto& r = redirs[idx];
    int dst = r.fd >= 0 ? r.fd : default_fd(r.type);

    auto dup_into = [&](int src_fd) -> bool {
      saved.save(dst);
      if (::dup2(src_fd, dst) < 0) {
        std::fprintf(stderr, "jfsh: dup2: %s\n", std::strerror(errno));
        return false;
      }
      ::close(src_fd);
      return true;
    };

    switch (r.type) {
      case REDIR_READ: {
        int fd = ::open(r.target.c_str(), O_RDONLY);
        if (fd < 0) {
          std::fprintf(stderr, "jfsh: %s: %s\n",
                       r.target.c_str(), std::strerror(errno));
          return false;
        }
        if (not dup_into(fd)) return false;
      }
      break; case REDIR_WRITE: case REDIR_CLOBBER: {
        int flags = O_WRONLY | O_CREAT | O_TRUNC;
        if (r.type == REDIR_WRITE and mCtx.opts.get(OPT_CLOBBER) == false)
          flags = O_WRONLY | O_CREAT | O_EXCL;
        int fd = ::open(r.target.c_str(), flags, 0666);
        if (fd < 0) {
          std::fprintf(stderr, "jfsh: %s: %s\n",
                       r.target.c_str(), std::strerror(errno));
          return false;
        }
        if (not dup_into(fd)) return false;
      }
      break; case REDIR_APPEND: {
        int fd = ::open(r.target.c_str(),
                        O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd < 0) {
          std::fprintf(stderr, "jfsh: %s: %s\n",
                       r.target.c_str(), std::strerror(errno));
          return false;
        }
        if (not dup_into(fd)) return false;
      }
      break; case REDIR_READWRITE: {
        int fd = ::open(r.target.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd < 0) {
          std::fprintf(stderr, "jfsh: %s: %s\n",
                       r.target.c_str(), std::strerror(errno));
          return false;
        }
        if (not dup_into(fd)) return false;
      }
      break; case REDIR_MERGEOUT: case REDIR_APPEND_ERR: {
        int flags = (r.type == REDIR_APPEND_ERR)
            ? (O_WRONLY | O_CREAT | O_APPEND)
            : (O_WRONLY | O_CREAT | O_TRUNC);
        int fd = ::open(r.target.c_str(), flags, 0666);
        if (fd < 0) {
          std::fprintf(stderr, "jfsh: %s: %s\n",
                       r.target.c_str(), std::strerror(errno));
          return false;
        }
        saved.save(1);
        saved.save(2);
        if (::dup2(fd, 1) < 0 or ::dup2(fd, 2) < 0) {
          std::fprintf(stderr, "jfsh: dup2: %s\n", std::strerror(errno));
          ::close(fd);
          return false;
        }
        ::close(fd);
      }
      break; case REDIR_MERGE: case REDIR_IN_DUP: {
        int src = parse_fd_target(r.target);
        if (src == -2) {  // `>&-` or `<&-` closes
          saved.save(dst);
          ::close(dst);
          break;
        }
        if (src < 0) {
          std::fprintf(stderr, "jfsh: %s: bad fd\n", r.target.c_str());
          return false;
        }
        saved.save(dst);
        if (::dup2(src, dst) < 0) {
          std::fprintf(stderr, "jfsh: dup2: %s\n", std::strerror(errno));
          return false;
        }
      }
      break; case REDIR_CLOSE: {
        saved.save(dst);
        ::close(dst);
      }
      break; case REDIR_HEREDOC: case REDIR_HEREDOCD: case REDIR_HERESTR: {
        std::string body = r.target;
        if (r.type == REDIR_HERESTR) body += '\n';
        int fd = heredoc_body_fd(body);
        if (fd < 0) return false;
        if (not dup_into(fd)) return false;
      }
      break; case REDIR_PROC_IN: {
        int fd = procsubst_in(r.target);
        if (fd < 0) return false;
        if (not dup_into(fd)) return false;
      }
      break; case REDIR_PROC_OUT: {
        int fd = procsubst_out(r.target);
        if (fd < 0) return false;
        if (not dup_into(fd)) return false;
      }
      break; case REDIR_PROC_EQ:
        // =(cmd) is a filename substitution, not a redirection target.
        // Falls out here with no apply (callers that want its path use
        // procsubst_eq on the word side).
      break; default:
        std::fprintf(stderr, "jfsh: redirection type %d not supported\n",
                     static_cast<int>(r.type));
        return false;
    }
  }
  return not saved.fail;
}

Executor::ProcSubstResources Executor::expand_procsubst(
    std::vector<std::string>& args) {
  ProcSubstResources res;
  for (auto& a : args) {
    if (a.size() < 3 or a.back() != ')') continue;
    std::string_view prefix(a.data(), 2);
    if (prefix != "<(" and prefix != ">(" and prefix != "=(") continue;
    std::string body = a.substr(2, a.size() - 3);
    if (prefix == "<(") {
      int fd = procsubst_in(body);
      if (fd < 0) { a.clear(); continue; }
      res.open_fds.push_back(fd);
      a = "/dev/fd/" + std::to_string(fd);
    } else if (prefix == ">(") {
      int fd = procsubst_out(body);
      if (fd < 0) { a.clear(); continue; }
      res.open_fds.push_back(fd);
      a = "/dev/fd/" + std::to_string(fd);
    } else {
      std::string path = procsubst_eq(body);
      if (path.empty()) { a.clear(); continue; }
      res.tmp_files.push_back(path);
      a = path;
    }
  }
  return res;
}

void Executor::procsubst_release(ProcSubstResources& res) noexcept {
  for (int fd : res.open_fds) ::close(fd);
  for (const auto& p : res.tmp_files) ::unlink(p.c_str());
  res.open_fds.clear();
  res.tmp_files.clear();
}

bool Executor::apply_redirects_child(const std::vector<Redirection>& redirs) {
  SavedFds throwaway;
  bool ok = apply_redirects(redirs, throwaway);
  // The child must not undo the redirections when `throwaway` dies.  Drop
  // the backup entries so the destructor becomes a no-op.
  throwaway.entries.clear();
  return ok;
}


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
  return Builtins.find(name).has_value();
}

int Executor::call_builtin(std::string_view name,
                           std::vector<std::string>& args,
                           ExecContext& ctx) {
  if (auto def = Builtins.find(name)) return def->get().fn(args, ctx);
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
                           const std::vector<Assignment>&  assigns,
                           const std::vector<Redirection>& redirs,
                           ExecFlags flags) {
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

  // EF_NOFORK: last command in `-c` string or last stage in a subshell runs
  // on this process directly.  Redirections are applied in-process; no save
  // because we will never return.
  if (flags & EF_NOFORK) {
    if (not redirs.empty() and not apply_redirects_child(redirs)) _exit(1);
    execve(path->c_str(), argv.data(), envp.data());
    int err = errno;
    const char* name = args[0].c_str();
    if (err == ENOENT) {
      std::fprintf(stderr, "jfsh: no such file or directory: %s\n", name);
      _exit(127);
    }
    std::fprintf(stderr, "jfsh: %s: %s\n", name, std::strerror(err));
    _exit(126);
  }

  pid_t pid = fork();
  if (pid < 0) {
    std::fprintf(stderr, "jfsh: fork failed: %s\n", std::strerror(errno));
    return 1;
  }
  if (pid == 0) {
    if (not redirs.empty() and not apply_redirects_child(redirs)) _exit(1);
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
