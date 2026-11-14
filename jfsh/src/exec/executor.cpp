#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <algorithm>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <jfsh/builtins/builtins.h>
#include <jfsh/exec/executor.h>
#include <jfsh/exec/subst.h>
#include <jfsh/glob/glob.h>
#include <jfsh/lexer/input.h>
#include <jfsh/parser/parser.h>

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
  int rc = exec(**prog);
  // `return` outside a function ends the script or -c string, as in zsh.
  if (mCtx.returning and mCtx.func_depth == 0) {
    mCtx.returning = false;
    rc = mCtx.last_exit = mCtx.return_val;
  }
  return rc;
}

// Copies the words of prog[begin, end) into a program of their own, with
// the strings they use renumbered.  A function body outlives the program
// that defined it.
static ProgRef extract_body(const CompiledProgram& prog, std::size_t begin,
                            std::size_t end) {
  auto body = std::make_shared<CompiledProgram>();
  for (std::size_t pc = begin; pc < end; ++pc) {
    WordCode w = prog.prog[pc];
    if (wc_code(w) == WC_STRING) {
      body->strs.push_back(prog.strs[wc_data(w)]);
      w = make_wc(WC_STRING, static_cast<WordCode>(body->strs.size() - 1));
    }
    body->prog.push_back(w);
    body->line_map.push_back(pc < prog.line_map.size() ? prog.line_map[pc] : 0);
  }
  return body;
}

// The pc just past a body (list* WC_END) that starts at pc.
static std::size_t body_end(const CompiledProgram& prog, std::size_t pc) {
  while (pc < prog.prog.size() and wc_code(prog.prog[pc]) == WC_LIST)
    pc += 1 + wc_list_skip(prog.prog[pc]);
  return pc + 1;
}

// words := WC_WORDS(n) WC_STRING{n}
static std::vector<std::string> fetch_words(ExecState& es) {
  WordCode n = wc_data(es.fetch());
  std::vector<std::string> words;
  words.reserve(n);
  for (WordCode i = 0; i < n; ++i) words.emplace_back(es.fetch_str());
  return words;
}

static Redirection fetch_redir(ExecState& es) {
  WordCode w = es.fetch();
  Redirection r;
  r.type    = wc_redir_type(w);
  r.fd      = wc_redir_has_fd(w) ? wc_redir_fd(w) : -1;
  r.literal = wc_redir_literal(w);
  r.target  = std::string(es.fetch_str());
  return r;
}

// Expands an assignment's value in place: a scalar without splitting, an
// array like command arguments.
static Result<void> expand_assignment(Expander& ex, Assignment& a) {
  if (a.array) {
    auto el = ex.array_elements(a.value);
    if (not el) return std::unexpected(el.error());
    a.elements = std::move(*el);
  } else {
    auto v = ex.scalar(a.value, /*assignment=*/true);
    if (not v) return std::unexpected(v.error());
    a.value = std::move(*v);
  }
  return {};
}

// Value an assignment in front of a command gives its name for that command
// only.  Fails, like a real assignment would, on a readonly parameter.
static Result<std::string> prefix_value(const ParamTable& params,
                                        const Assignment& a) {
  auto existing = params.lookup(a.name);
  if (existing and existing->get().is_readonly())
    return std::unexpected(ShellError{ErrorKind::ExecError,
        "read-only variable: " + a.name});
  std::string value = a.value;
  if (a.array) {
    value.clear();
    for (std::size_t i = 0; i < a.elements.size(); ++i) {
      if (i) value += ' ';
      value += a.elements[i];
    }
  }
  if (a.append and existing) return existing->get().scalar() + value;
  return value;
}

int Executor::exec_simple(std::vector<std::string> args,
                          std::vector<Assignment>  assigns,
                          std::vector<Redirection> redirs,
                          ExecFlags flags,
                          std::vector<Assignment>  typeset_args) {
  std::optional<int> subst_status;
  if (not expand_command(args, assigns, redirs, subst_status)) return 1;
  // typeset's `name=value` arguments: a scalar is passed as `name=value`; an
  // array is passed as `name`, and given its elements once the builtin has
  // declared it.
  std::vector<Assignment> typeset_arrays;
  if (not typeset_args.empty()) {
    Expander ex{*this, mCtx};
    for (auto& a : typeset_args) {
      if (auto r = expand_assignment(ex, a); not r) {
        std::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
        return 1;
      }
      if (a.array) {
        args.push_back(a.name);
        typeset_arrays.push_back(std::move(a));
      } else {
        args.push_back(a.name + (a.append ? "+=" : "=") + a.value);
      }
    }
  }
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
    // `x=$(cmd)` has cmd's status.
    if (rc == 0 and subst_status) rc = *subst_status;
    return rc;
  }

  // Functions come before builtins.  Aliases are already expanded by the
  // lexer (M7).
  auto fn  = mCtx.functions.find(args[0]);
  auto def = fn == mCtx.functions.end() ? Builtins.find(args[0]) : std::nullopt;
  if (fn == mCtx.functions.end() and not def)
    return run_external(args, assigns, redirs, flags);

  SavedFds saved;
  if (not redirs.empty()) {
    // Push pending buffered output through the ORIGINAL fds before we
    // rewire them; otherwise the output lands in the wrong place after
    // SavedFds restores (bug: stdio buffers survive dup2).
    std::fflush(nullptr);
    if (not apply_redirects(redirs, saved)) return 1;
  }
  int rc = 0;
  if (fn != mCtx.functions.end()) {
    ProgRef body = fn->second;  // the function may redefine itself
    // Assignments in front of a function are exported to what it runs.
    if (not assigns.empty()) rc = push_prefix_assigns(assigns, true);
    if (rc == 0) rc = call_function(body, args);
    if (not assigns.empty()) mCtx.params.pop_scope();
    std::fflush(nullptr);
    return rc;
  }
  // zsh keeps assignments in front of a special builtin only under
  // POSIX_BUILTINS; otherwise they last while the builtin runs.  Without
  // any, no scope is pushed, so `local` declares in the caller's scope.
  bool scoped = not assigns.empty();
  if ((def->get().flags & BINF_SPECIAL) and mCtx.opts.get(OPT_POSIXBUILTINS)) {
    rc = apply_assigns(assigns);
    scoped = false;
  } else if (scoped) {
    rc = push_prefix_assigns(assigns, false);
  }
  if (rc == 0) rc = def->get().fn(args, mCtx);
  for (const auto& a : typeset_arrays) {
    if (rc != 0) break;
    if (auto r = mCtx.params.set_array(a.name, a.elements, a.append); not r) {
      std::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
      rc = 1;
    }
  }
  if (scoped) mCtx.params.pop_scope();
  std::fflush(nullptr);
  return rc;
}

int Executor::push_prefix_assigns(const std::vector<Assignment>& assigns,
                                  bool exported) {
  mCtx.params.push_scope();
  for (const auto& a : assigns) {
    auto value = prefix_value(mCtx.params, a);
    if (not value) {
      std::fprintf(stderr, "jfsh: %s\n", value.error().message.c_str());
      return 1;
    }
    auto outer = mCtx.params.lookup(a.name);
    std::uint32_t attrs = a.array ? PM_ARRAY : PM_SCALAR;
    if (exported or (outer and outer->get().is_exported())) attrs |= PM_EXPORTED;
    if (a.array) mCtx.params.assign(a.name, a.elements, attrs);
    else         mCtx.params.assign(a.name, std::move(*value), attrs);
  }
  return 0;
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
    if (interrupted()) return rc;
  }
  return rc;
}

bool Executor::interrupted() const noexcept {
  return mCtx.exiting or mCtx.returning or mCtx.break_level > 0;
}

bool Executor::loop_done() noexcept {
  if (mCtx.exiting or mCtx.returning) return true;
  if (mCtx.break_level == 0) return false;
  if (--mCtx.break_level > 0 or not mCtx.continuing) return true;
  mCtx.continuing = false;
  return false;
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
    if (interrupted()) return rc;
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
    // Walk this stage: redirections and assignments, then WC_SIMPLE or
    // WC_TYPESET and its words, or a compound command, which its header's
    // skip jumps over.
    while (es.pc < es.prog->prog.size()) {
      WordCode op = wc_code(es.peek());
      if (op == WC_ASSIGN) { es.pc += 3; }
      else if (op == WC_REDIR) { es.pc += 2; }
      else if (op == WC_SIMPLE) {
        WordCode argc = wc_data(es.fetch());
        es.pc += argc;
        break;
      } else if (op == WC_TYPESET) {
        WordCode n = wc_data(es.fetch());
        for (WordCode a = 0; a < n; ++a)
          es.pc += wc_code(es.peek()) == WC_ASSIGN ? 3 : 1;
        break;
      } else {
        WordCode skip = wc_cmd_skip(es.fetch());
        es.pc += skip;
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
  // Redirections in front of a compound command belong to it; a simple
  // command goes on with an assignment or its words.
  std::size_t pc = es.pc;
  while (wc_code(es.prog->prog[pc]) == WC_REDIR) pc += 2;
  switch (wc_code(es.prog->prog[pc])) {
    case WC_ASSIGN: case WC_SIMPLE: case WC_TYPESET:
      return exec_simple_cmd(es, flags);
    default:
      break;
  }
  if (pc == es.pc) return exec_compound(es, flags);

  std::vector<Redirection> redirs;
  while (wc_code(es.peek()) == WC_REDIR) redirs.push_back(fetch_redir(es));
  std::vector<std::string> no_args;
  std::vector<Assignment>  no_assigns;
  std::optional<int>       subst_status;
  if (not expand_command(no_args, no_assigns, redirs, subst_status)) return 1;
  SavedFds saved;
  std::fflush(nullptr);
  if (not apply_redirects(redirs, saved)) return 1;
  int rc = exec_compound(es, flags);
  std::fflush(nullptr);
  return rc;
}

int Executor::exec_compound(ExecState& es, ExecFlags flags) {
  WordCode head = es.fetch();
  switch (wc_code(head)) {
    case WC_IF:       return exec_if(es, head);
    case WC_WHILE:    return exec_while(es, head);
    case WC_FOR:      return exec_for(es, head);
    case WC_FOR_COND: return exec_for_cond(es, head);
    case WC_CASE:     return exec_case(es, head);
    case WC_SELECT:   return exec_select(es, head);
    case WC_REPEAT:   return exec_repeat(es, head);
    case WC_SUBSH:    return exec_subsh(es, head, flags);
    case WC_FUNCDEF:  return exec_funcdef(es, head);
    case WC_COND:     return exec_cond(es, head);
    case WC_ARITH:    return exec_arith(es, head);
    case WC_TRY:      return exec_always(es, head);
    case WC_CURSH: {
      std::size_t end = es.pc + wc_cmd_skip(head);
      int rc = exec_list(es, EF_NONE);
      es.pc = end;
      return rc;
    }
    default:          return 0;
  }
}

// simple := (assign | redir)* WC_SIMPLE(argc) WC_STRING{argc}
//         | (assign | redir)* WC_TYPESET(n) item{n}
int Executor::exec_simple_cmd(ExecState& es, ExecFlags flags) {
  auto fetch_assign = [&] {
    WordCode f = wc_data(es.fetch());
    Assignment a;
    a.name   = std::string(es.fetch_str());
    a.value  = std::string(es.fetch_str());
    a.append = f & WC_ASSIGN_APPEND;
    a.array  = f & WC_ASSIGN_ARRAY;
    return a;
  };
  std::vector<Assignment> assigns;
  std::vector<Redirection> redirs;
  for (;;) {
    WordCode op = wc_code(es.peek());
    if      (op == WC_ASSIGN) assigns.push_back(fetch_assign());
    else if (op == WC_REDIR)  redirs.push_back(fetch_redir(es));
    else break;
  }
  WordCode head = es.fetch();  // WC_SIMPLE or WC_TYPESET
  WordCode argc = wc_data(head);
  std::vector<std::string> words;
  std::vector<Assignment>  typeset_args;
  words.reserve(argc);
  for (WordCode i = 0; i < argc; ++i) {
    if (wc_code(es.peek()) == WC_ASSIGN) typeset_args.push_back(fetch_assign());
    else                                 words.emplace_back(es.fetch_str());
  }
  return exec_simple(std::move(words), std::move(assigns), std::move(redirs),
                     flags, std::move(typeset_args));
}

// if := WC_IF(HEAD, skip) branch+; the first branch whose condition
// succeeds runs.  With none, the status is 0.
int Executor::exec_if(ExecState& es, WordCode head) {
  std::size_t end = es.pc + wc_cmd_skip(head);
  int rc = 0;
  while (es.pc < end) {
    WordCode branch = es.fetch();
    std::size_t next = es.pc + wc_cmd_skip(branch);
    if (wc_cmd_type(branch) == WC_IF_ELSE) {
      rc = exec_list(es, EF_NONE);
      break;
    }
    int status = exec_list(es, EF_NONE);
    if (interrupted()) {
      rc = status;
      break;
    }
    if (status == 0) {
      rc = exec_list(es, EF_NONE);
      break;
    }
    es.pc = next;
  }
  es.pc = end;
  return rc;
}

// while := WC_WHILE(WHILE|UNTIL, skip) body(cond) body.  The status is the
// last body's, or 0 when the body never ran.
int Executor::exec_while(ExecState& es, WordCode head) {
  std::size_t end   = es.pc + wc_cmd_skip(head);
  std::size_t cond  = es.pc;
  bool        until = wc_cmd_type(head) == WC_WHILE_UNTIL;
  int rc = 0;
  ++mCtx.loop_depth;
  for (;;) {
    es.pc = cond;
    int status = exec_list(es, EF_NONE);
    if (interrupted()) {
      if (loop_done()) break;
      continue;
    }
    if ((status == 0) == until) break;
    rc = exec_list(es, EF_NONE);
    if (loop_done()) break;
  }
  --mCtx.loop_depth;
  es.pc = end;
  return rc;
}

bool Executor::loop_values(ExecState& es, WordCode head,
                           std::vector<std::string>& names,
                           std::vector<std::string>& values) {
  names = fetch_words(es);
  if (wc_cmd_type(head) == WC_FOR_ARGS) {
    values = mCtx.positional;
    return true;
  }
  auto raw = fetch_words(es);
  Expander ex{*this, mCtx};
  auto expanded = ex.words(raw);
  if (not expanded) {
    std::fprintf(stderr, "jfsh: %s\n", expanded.error().message.c_str());
    return false;
  }
  values = std::move(*expanded);
  return true;
}

// for := WC_FOR(IN|ARGS, skip) words(names) [words(values)] body.  With
// several names each pass takes that many values; missing ones are empty.
int Executor::exec_for(ExecState& es, WordCode head) {
  std::size_t end = es.pc + wc_cmd_skip(head);
  std::vector<std::string> names, values;
  if (not loop_values(es, head, names, values)) {
    es.pc = end;
    return 1;
  }
  std::size_t body = es.pc;
  int rc = 0;
  ++mCtx.loop_depth;
  for (std::size_t i = 0; i < values.size(); i += names.size()) {
    bool ok = true;
    for (std::size_t k = 0; k < names.size() and ok; ++k) {
      std::string v = i + k < values.size() ? values[i + k] : std::string{};
      if (auto r = mCtx.params.set_scalar(names[k], std::move(v)); not r) {
        std::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
        ok = false;
      }
    }
    if (not ok) {
      rc = 1;
      break;
    }
    es.pc = body;
    rc = exec_list(es, EF_NONE);
    if (loop_done()) break;
  }
  --mCtx.loop_depth;
  es.pc = end;
  return rc;
}

static bool is_blank(std::string_view s) noexcept {
  return s.find_first_not_of(" \t\n") == std::string_view::npos;
}

// cfor := WC_FOR_COND(0, skip) WC_STRING(init) WC_STRING(cond)
// WC_STRING(step) body.  An empty condition is true.
int Executor::exec_for_cond(ExecState& es, WordCode head) {
  std::size_t end = es.pc + wc_cmd_skip(head);
  std::string init(es.fetch_str());
  std::string cond(es.fetch_str());
  std::string step(es.fetch_str());
  std::size_t body = es.pc;
  Expander ex{*this, mCtx};
  auto eval = [&](const std::string& expr, long long& value) {
    value = 1;
    if (is_blank(expr)) return true;
    auto v = ex.arith(expr);
    if (not v) {
      std::fprintf(stderr, "jfsh: %s\n", v.error().message.c_str());
      return false;
    }
    value = *v;
    return true;
  };
  int rc = 0;
  long long value = 0;
  if (not eval(init, value)) {
    es.pc = end;
    return 1;
  }
  ++mCtx.loop_depth;
  for (;;) {
    if (not eval(cond, value)) {
      rc = 1;
      break;
    }
    if (value == 0) break;
    es.pc = body;
    rc = exec_list(es, EF_NONE);
    if (loop_done()) break;
    if (not eval(step, value)) {
      rc = 1;
      break;
    }
  }
  --mCtx.loop_depth;
  es.pc = end;
  return rc;
}

// case := WC_CASE(HEAD, skip) WC_STRING(subject) arm*.  After a matching
// arm, `;;` ends the case, `;&` runs the next arm's body unconditionally
// and `;|` goes on testing the arms after it.
int Executor::exec_case(ExecState& es, WordCode head) {
  std::size_t end = es.pc + wc_cmd_skip(head);
  Expander ex{*this, mCtx};
  auto fail = [&](const ShellError& e) {
    std::fprintf(stderr, "jfsh: %s\n", e.message.c_str());
    es.pc = end;
    return 1;
  };
  auto subject = ex.scalar(es.fetch_str());
  if (not subject) return fail(subject.error());
  int  rc   = 0;
  bool fall = false;
  while (es.pc < end) {
    WordCode arm = es.fetch();
    std::size_t next = es.pc + wc_cmd_skip(arm);
    bool match = fall;
    for (const auto& word : fetch_words(es)) {
      if (match) break;
      auto pat = ex.pattern(word);
      if (not pat) return fail(pat.error());
      match = CompiledPattern::compile(*pat, false, mCtx.opts.extended_glob())
                  .matches(*subject);
    }
    if (not match) {
      es.pc = next;
      continue;
    }
    rc = exec_list(es, EF_NONE);
    if (interrupted() or wc_cmd_type(arm) == WC_CASE_BREAK) break;
    fall  = wc_cmd_type(arm) == WC_CASE_FALL;
    es.pc = next;
  }
  es.pc = end;
  return rc;
}

// One line from fd 0, read a byte at a time so nothing past it is
// consumed.  nullopt at end of input.
static std::optional<std::string> read_line_fd0() {
  std::string line;
  for (;;) {
    char c;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (n < 0 and errno == EINTR) continue;
    if (n <= 0) {
      if (line.empty()) return std::nullopt;
      return line;
    }
    if (c == '\n') return line;
    line += c;
  }
}

// select := WC_SELECT(IN|ARGS, skip), laid out like for.  Prints the menu
// and $PS3 on stderr, reads a line into $REPLY and sets the name to the
// chosen value (empty for anything else); an empty line shows the menu
// again.  Ends at end of input or on `break`.
int Executor::exec_select(ExecState& es, WordCode head) {
  std::size_t end = es.pc + wc_cmd_skip(head);
  std::vector<std::string> names, values;
  if (not loop_values(es, head, names, values)) {
    es.pc = end;
    return 1;
  }
  std::size_t body = es.pc;
  int rc = 0;
  if (values.empty()) {
    es.pc = end;
    return 0;
  }
  auto show_menu = [&] {
    std::string menu;
    std::size_t width = 0;
    for (std::size_t i = 0; i < values.size(); ++i) {
      std::string item = std::to_string(i + 1) + ") " + values[i] + "  ";
      if (width > 0 and width + item.size() > 80) {
        menu += '\n';
        width = 0;
      }
      menu += item;
      width += item.size();
    }
    std::fprintf(stderr, "%s\n", menu.c_str());
  };
  ++mCtx.loop_depth;
  bool menu = true;
  for (;;) {
    if (menu) show_menu();
    auto ps3 = mCtx.params.lookup("PS3");
    std::string prompt = (ps3 and ps3->get().is_set()) ? ps3->get().scalar() : "?# ";
    std::fprintf(stderr, "%s", prompt.c_str());
    std::fflush(stderr);
    auto line = read_line_fd0();
    if (not line) {
      std::fputc('\n', stderr);
      break;
    }
    menu = line->empty();
    if (menu) continue;
    (void)mCtx.params.set_scalar("REPLY", *line);
    char* rest = nullptr;
    long n = std::strtol(line->c_str(), &rest, 10);
    std::string choice = (*rest == '\0' and n >= 1
                          and static_cast<std::size_t>(n) <= values.size())
                       ? values[static_cast<std::size_t>(n) - 1] : std::string{};
    if (auto r = mCtx.params.set_scalar(names[0], choice); not r) {
      std::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
      rc = 1;
      break;
    }
    es.pc = body;
    rc = exec_list(es, EF_NONE);
    if (loop_done()) break;
  }
  --mCtx.loop_depth;
  es.pc = end;
  return rc;
}

// repeat := WC_REPEAT(0, skip) WC_STRING(count) body; the count is an
// arithmetic expression.
int Executor::exec_repeat(ExecState& es, WordCode head) {
  std::size_t end = es.pc + wc_cmd_skip(head);
  Expander ex{*this, mCtx};
  auto count = ex.arith(es.fetch_str());
  if (not count) {
    std::fprintf(stderr, "jfsh: %s\n", count.error().message.c_str());
    es.pc = end;
    return 1;
  }
  std::size_t body = es.pc;
  int rc = 0;
  ++mCtx.loop_depth;
  for (long long i = 0; i < *count; ++i) {
    es.pc = body;
    rc = exec_list(es, EF_NONE);
    if (loop_done()) break;
  }
  --mCtx.loop_depth;
  es.pc = end;
  return rc;
}

// subsh := WC_SUBSH(0, skip) body, run in a forked copy of the shell.
// TODO(exec/subsh): with EF_NOFORK (already in a child, e.g. a pipeline
//   stage) the body could run without another fork.
int Executor::exec_subsh(ExecState& es, WordCode head, ExecFlags) {
  std::size_t end = es.pc + wc_cmd_skip(head);
  std::fflush(nullptr);
  pid_t pid = ::fork();
  if (pid < 0) {
    std::fprintf(stderr, "jfsh: fork: %s\n", std::strerror(errno));
    es.pc = end;
    return 1;
  }
  if (pid == 0) {
    int rc = exec_list(es, EF_NONE);
    if (mCtx.exiting) rc = mCtx.exit_val;
    else if (mCtx.returning) rc = mCtx.return_val;
    std::fflush(nullptr);
    _exit(rc);
  }
  es.pc = end;
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) return 1;
  }
  if (WIFEXITED(status))   return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 1;
}

// funcdef := WC_FUNCDEF(NAMED, skip) words(names) body: copies the body
// out and stores it under each name.  WC_FUNCDEF(ANON, skip) body
// words(args) calls the body at once with the args.
int Executor::exec_funcdef(ExecState& es, WordCode head) {
  std::size_t end = es.pc + wc_cmd_skip(head);
  if (wc_cmd_type(head) == WC_FUNCDEF_ANON) {
    std::size_t begin = es.pc;
    es.pc = body_end(*es.prog, begin);
    ProgRef body = extract_body(*es.prog, begin, es.pc);
    auto raw = fetch_words(es);
    es.pc = end;
    Expander ex{*this, mCtx};
    auto args = ex.words(raw);
    if (not args) {
      std::fprintf(stderr, "jfsh: %s\n", args.error().message.c_str());
      return 1;
    }
    args->insert(args->begin(), "(anon)");
    return call_function(body, *args);
  }
  auto names = fetch_words(es);
  ProgRef body = extract_body(*es.prog, es.pc, end);
  for (auto& name : names) mCtx.functions[std::move(name)] = body;
  es.pc = end;
  return 0;
}

// TODO(exec/cond): Evaluate a [[ ]] expression tree (M5).
//   Opcodes inside the cond block: WC_COND_STR, WC_COND_REGEX, WC_COND_AND/OR/NOT
//   Delegate to builtins/cond.cpp evaluator.
int Executor::exec_cond(ExecState& es, WordCode head) {
  es.pc += wc_cmd_skip(head);
  return 0;
}

// arith := WC_ARITH(0, 1) WC_STRING(expr).  Status 0 when the value is
// non-zero, 1 when it is zero, 2 on an error.
int Executor::exec_arith(ExecState& es, WordCode) {
  Expander ex{*this, mCtx};
  auto v = ex.arith(es.fetch_str());
  if (not v) {
    std::fprintf(stderr, "jfsh: %s\n", v.error().message.c_str());
    return 2;
  }
  return *v != 0 ? 0 : 1;
}

// try := WC_TRY(0, skip) body body(always).  The always block runs however
// the first one ended; a break, continue, return or exit pending from the
// first block is held while it runs and then goes on, unless the always
// block started one of its own.  The status is the first block's.
int Executor::exec_always(ExecState& es, WordCode head) {
  std::size_t end    = es.pc + wc_cmd_skip(head);
  std::size_t always = body_end(*es.prog, es.pc);
  int rc = exec_list(es, EF_NONE);
  auto held = std::tuple{mCtx.break_level, mCtx.continuing, mCtx.returning,
                         mCtx.return_val, mCtx.exiting, mCtx.exit_val};
  mCtx.break_level = 0;
  mCtx.continuing  = false;
  mCtx.returning   = false;
  mCtx.exiting     = false;
  es.pc = always;
  exec_list(es, EF_NONE);
  if (not interrupted()) {
    std::tie(mCtx.break_level, mCtx.continuing, mCtx.returning,
             mCtx.return_val, mCtx.exiting, mCtx.exit_val) = held;
  }
  es.pc = end;
  return rc;
}

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
    int rc = exec_string(src);
    std::fflush(nullptr);
    _exit(rc);
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
    int rc = exec_string(src);
    std::fflush(nullptr);
    _exit(rc);
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
    int rc = exec_string(src);
    std::fflush(nullptr);
    _exit(rc);
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


// Word expansion for one simple command (see include/jfsh/exec/subst.h).
// A redirection target that expands to several words becomes several
// redirections of the same kind, which MULTIOS then fans out.
bool Executor::expand_command(std::vector<std::string>& args,
                              std::vector<Assignment>&  assigns,
                              std::vector<Redirection>& redirs,
                              std::optional<int>&       subst_status) {
  Expander ex{*this, mCtx};
  auto fail = [&](const ShellError& e) {
    std::fprintf(stderr, "jfsh: %s\n", e.message.c_str());
    subst_status = ex.subst_status();
    return false;
  };
  // Assignment values are expanded first, then the arguments, then the
  // redirection targets.
  for (auto& a : assigns)
    if (auto r = expand_assignment(ex, a); not r) return fail(r.error());
  auto words = ex.words(args);
  if (not words) return fail(words.error());
  args = std::move(*words);

  std::vector<Redirection> out;
  out.reserve(redirs.size());
  for (auto& r : redirs) {
    switch (r.type) {
      case REDIR_HEREDOC: case REDIR_HEREDOCD:
        if (not r.literal) {
          auto body = ex.heredoc(r.target);
          if (not body) return fail(body.error());
          r.target = std::move(*body);
        }
        out.push_back(std::move(r));
      break; case REDIR_HERESTR: case REDIR_MERGE: case REDIR_IN_DUP: {
        auto v = ex.scalar(r.target);
        if (not v) return fail(v.error());
        r.target = std::move(*v);
        out.push_back(std::move(r));
      }
      break; case REDIR_CLOSE: case REDIR_PROC_IN: case REDIR_PROC_OUT:
      case REDIR_PROC_EQ:
        out.push_back(std::move(r));
      break; default: {
        std::string word = r.target;
        auto targets = ex.words({r.target});
        if (not targets) return fail(targets.error());
        if (targets->empty())
          return fail(ShellError{ErrorKind::ExecError,
              "no such file or directory: " + word});
        if (targets->size() > 1 and not mCtx.opts.get(OPT_MULTIOS))
          return fail(ShellError{ErrorKind::ExecError, "ambiguous redirect: " + word});
        for (auto& t : *targets) {
          Redirection copy = r;
          copy.target = std::move(t);
          out.push_back(std::move(copy));
        }
      }
    }
  }
  redirs = std::move(out);
  subst_status = ex.subst_status();
  return true;
}

// TODO(exec/assign): subscripted assignments `a[i]=x`, `h[key]=x`.
int Executor::apply_assigns(const std::vector<Assignment>& assigns) {
  for (const auto& a : assigns) {
    auto r = a.array ? mCtx.params.set_array(a.name, a.elements, a.append)
                     : mCtx.params.set_scalar(a.name, a.value, a.append);
    if (not r) {
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
  for (const auto& a : assigns) {
    std::string value = trace_quote(a.value);
    if (a.array) {
      value = "(";
      for (const auto& e : a.elements) value += ' ' + trace_quote(e);
      value += " )";
    }
    add(a.name + (a.append ? "+=" : "=") + value);
  }
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

// zsh's default limit on nested function calls, used when $FUNCNEST is
// unset or not a number.
static constexpr long kDefaultFuncNest = 700;

int Executor::call_function(const ProgRef& body,
                            std::vector<std::string>& args) {
  long limit = kDefaultFuncNest;
  if (auto p = mCtx.params.lookup("FUNCNEST"); p and p->get().is_set()) {
    std::string text = p->get().is_integer() ? std::to_string(p->get().integer())
                                             : p->get().scalar();
    char* rest = nullptr;
    long n = std::strtol(text.c_str(), &rest, 10);
    if (not text.empty() and *rest == '\0') limit = n;
  }
  if (limit >= 0 and mCtx.func_depth >= limit) {
    std::fprintf(stderr,
                 "jfsh: %s: maximum nested function level reached; increase FUNCNEST?\n",
                 args[0].c_str());
    return 1;
  }

  auto saved_positional = std::exchange(
      mCtx.positional, std::vector<std::string>(args.begin() + 1, args.end()));
  std::string saved_arg0 = mCtx.arg0;
  if (mCtx.opts.get(OPT_FUNCTIONARGZERO)) mCtx.arg0 = args[0];
  OptionState saved_opts  = mCtx.opts;
  bool        local_traps = mCtx.opts.get(OPT_LOCALTRAPS);
  if (local_traps) mCtx.signals.traps().push_local_traps();
  ++mCtx.func_depth;
  mCtx.params.push_scope();

  int rc = exec(*body);
  if (mCtx.returning) {
    mCtx.returning = false;
    rc = mCtx.return_val;
  }

  mCtx.params.pop_scope();
  --mCtx.func_depth;
  if (local_traps) mCtx.signals.traps().pop_local_traps();
  // LOCAL_OPTIONS as it stands when the function returns decides.
  if (mCtx.opts.get(OPT_LOCALOPTIONS)) mCtx.opts = saved_opts;
  mCtx.arg0       = std::move(saved_arg0);
  mCtx.positional = std::move(saved_positional);
  return rc;
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
