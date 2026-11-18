#include <jfsh/exec/executor.h>

#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <signal.h>
#include <unistd.h>

#include <algorithm>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/cond.h>
#include <jfsh/builtins/math.h>
#include <jfsh/core/aliases.h>
#include <jfsh/editor/completion.h>
#include <jfsh/editor/zle.h>
#include <jfsh/exec/subst.h>
#include <jfsh/glob/glob.h>
#include <jfsh/lexer/input.h>
#include <jfsh/parser/parser.h>

namespace jfsh {

static BuiltinTable Builtins;

Executor::Executor(ExecContext& ctx) : mCtx(ctx) {
  mCtx.executor = this;
  install_special_params(mCtx);
  static bool initialized = false;
  if (not initialized) {
    Builtins.register_all();
    initialized = true;
  }
  if (not mCtx.zle) {
    mOwnZle        = s::make_unique<Zle>();
    mOwnCompletion = s::make_unique<CompletionEngine>(mCtx.params);
    mOwnZle->completion = mOwnCompletion.get();
    mCtx.zle = mOwnZle.get();
  }
  install_zle_hooks(*this);
}

Executor::~Executor() {
  if (mCtx.zle == mOwnZle.get()) mCtx.zle = nullptr;
}

int Executor::exec(const CompiledProgram& prog) {
  if (prog.empty()) return 0;
  ExecState es;
  es.prog = &prog;
  es.pc   = 0;
  return exec_list(es, EF_NONE);
}

int Executor::exec_string(strv src, bool by_line) {
  struct Depth {
    int& d;
    explicit Depth(int& x) : d(x) { ++d; }
    ~Depth() { --d; }
  } depth{mStringDepth};
  StringInputSource in{str(src)};
  Lexer lex{in};
  // Comments are always recognized in scripts and -c strings, but
  // interactively only with INTERACTIVE_COMMENTS.
  if (not mCtx.opts.interactive() or mCtx.opts.get(OPT_INTERACTIVECOMMENTS))
    lex.push_state(LEX_SKIPCOMMENTS);
  Parser parser{lex};
  if (mCtx.opts.get(OPT_ALIASES)) parser.set_aliases(&mCtx.aliases);
  auto parse_failed = [&](const ShellError& e) {
    s::fprintf(stderr, "jfsh:%d: %s\n", e.code, e.message.c_str());
    mCtx.last_exit = 1;
    return 1;
  };
  int rc = 0;
  if (not by_line) {
    auto prog = parser.parse();
    if (not prog) return parse_failed(prog.error());
    rc = exec(**prog);
  } else {
    for (;;) {
      // A line may have turned ALIASES on or off for the next.
      parser.set_aliases(mCtx.opts.get(OPT_ALIASES) ? &mCtx.aliases : nullptr);
      auto prog = parser.parse_line();
      if (not prog) return parse_failed(prog.error());
      if ((*prog)->prog.size() > 1) rc = exec(**prog);
      if (parser.at_eof() or interrupted()) break;
    }
  }
  // `return` outside a function ends the script or -c string, as in zsh.
  if (mCtx.returning and mCtx.func_depth == 0) {
    mCtx.returning = false;
    rc = mCtx.last_exit = mCtx.return_val;
  }
  // An aborted command line ends here: a non-interactive shell exits.  (An
  // interactive one goes back to the prompt; the REPL clears the flag.)
  if (mCtx.aborting and mStringDepth == 1 and mCtx.func_depth == 0
      and not mCtx.opts.interactive()) {
    mCtx.aborting = false;
    mCtx.exiting  = true;
    mCtx.exit_val = mCtx.last_exit;
  }
  return rc;
}

// Copies the words of prog[begin, end) into a program of their own, with
// the strings they use renumbered.  A function body outlives the program
// that defined it.
static ProgRef extract_body(const CompiledProgram& prog, s::size_t begin,
                            s::size_t end) {
  auto body = s::make_shared<CompiledProgram>();
  for (s::size_t pc = begin; pc < end; ++pc) {
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
static s::size_t body_end(const CompiledProgram& prog, s::size_t pc) {
  while (pc < prog.prog.size() and wc_code(prog.prog[pc]) == WC_LIST)
    pc += 1 + wc_list_skip(prog.prog[pc]);
  return pc + 1;
}

// words := WC_WORDS(n) WC_STRING{n}
static vec<str> fetch_words(ExecState& es) {
  WordCode n = wc_data(es.fetch());
  vec<str> words;
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
  r.target  = str(es.fetch_str());
  return r;
}

// Expands an assignment's value in place: a scalar without splitting, an
// array like command arguments.
static Result<void> expand_assignment(Expander& ex, Assignment& a) {
  if (a.array) {
    auto el = ex.array_elements(a.value);
    if (not el) return s::unexpected(el.error());
    a.elements = s::move(*el);
  } else {
    auto v = ex.scalar(a.value, /*assignment=*/true);
    if (not v) return s::unexpected(v.error());
    a.value = s::move(*v);
  }
  return {};
}

// Value an assignment in front of a command gives its name for that command
// only.  Fails, like a real assignment would, on a readonly parameter.
static Result<str> prefix_value(const ParamTable& params,
                                const Assignment& a) {
  auto existing = params.lookup(a.name);
  if (existing and existing->get().is_readonly())
    return s::unexpected(ShellError{ErrorKind::ExecError,
        "read-only variable: " + a.name});
  str value = a.value;
  if (a.array) {
    value.clear();
    for (s::size_t i = 0; i < a.elements.size(); ++i) {
      if (i) value += ' ';
      value += a.elements[i];
    }
  }
  if (a.append and existing) return existing->get().scalar() + value;
  return value;
}

int Executor::exec_simple(vec<str> args,
                          vec<Assignment>  assigns,
                          vec<Redirection> redirs,
                          ExecFlags flags,
                          vec<Assignment>  typeset_args) {
  opt<int> subst_status;
  // `noglob cmd`: the words are not globbed.
  bool noglob = false;
  while (not args.empty() and args[0] == "noglob") {
    args.erase(args.begin());
    noglob = true;
  }
  bool globbing = mCtx.opts.get(OPT_GLOB);
  if (noglob) mCtx.opts.unset(OPT_GLOB);
  bool expanded = expand_command(args, assigns, redirs, subst_status);
  if (noglob) mCtx.opts.assign(OPT_GLOB, globbing);
  // A failed expansion (`${x?msg}`, NO_UNSET, `$((1/0))`) ends the command
  // line, as zsh's errflag does: a non-interactive shell exits.
  if (not expanded) {
    mCtx.aborting = true;
    return 1;
  }
  // typeset's `name=value` arguments: a scalar is passed as `name=value`; an
  // array is passed as `name`, and given its elements once the builtin has
  // declared it.
  vec<Assignment> typeset_arrays;
  if (not typeset_args.empty()) {
    Expander ex{*this, mCtx};
    for (auto& a : typeset_args) {
      if (auto r = expand_assignment(ex, a); not r) {
        s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
        return 1;
      }
      if (a.array) {
        args.push_back(a.name);
        typeset_arrays.push_back(s::move(a));
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

  // Precommand modifiers: `builtin` and `command` pick what the name may
  // be; `exec` replaces the shell (or, alone, makes the redirections
  // permanent); `-` and `nocorrect` change nothing here.
  enum class Only { Any, Builtin, External } only = Only::Any;
  bool replace = false;
  bool clear_env = false;
  str  argv0;
  while (not args.empty()) {
    const str& w = args[0];
    if (w == "builtin" and only == Only::Any) {
      args.erase(args.begin());
      if (args.empty()) return 0;
      only = Only::Builtin;
      break;
    }
    if (w == "command" and only == Only::Any) {
      s::size_t i = 1;
      bool lookup = false;
      bool default_path = false;
      for (; i < args.size() and args[i].size() > 1 and args[i][0] == '-'; ++i) {
        if (args[i] == "--") { ++i; break; }
        for (char f : strv(args[i]).substr(1)) {
          if (f == 'v' or f == 'V') lookup = true;
          else if (f == 'p') default_path = true;
          else {
            s::fprintf(stderr, "jfsh: command: bad option: -%c\n", f);
            return 1;
          }
        }
      }
      // `command -v name`: describe it, as whence does.
      if (lookup) break;
      args.erase(args.begin(), args.begin() + static_cast<s::ptrdiff_t>(i));
      if (args.empty()) return 0;
      if (default_path) {
        // -p: look the name up in the system's default $PATH.
        mCtx.params.push_scope();
        mCtx.params.assign("PATH", str("/usr/bin:/bin:/usr/sbin:/sbin"), PM_SCALAR);
        auto path = find_command(args[0]);
        mCtx.params.pop_scope();
        if (path) args[0] = *path;
      }
      only = mCtx.opts.get(OPT_POSIXBUILTINS) ? Only::Any : Only::External;
      continue;
    }
    if (w == "exec" and not replace) {
      s::size_t i = 1;
      for (; i < args.size() and args[i].size() > 1 and args[i][0] == '-'; ++i) {
        if (args[i] == "--") { ++i; break; }
        strv letters = strv(args[i]).substr(1);
        for (s::size_t k = 0; k < letters.size(); ++k) {
          char f = letters[k];
          if (f == 'c') {
            clear_env = true;
          } else if (f == 'l') {
            argv0 = "-";
          } else if (f == 'a') {
            if (k + 1 < letters.size()) {
              argv0 = str(letters.substr(k + 1));
            } else if (i + 1 < args.size()) {
              argv0 = args[++i];
            } else {
              s::fprintf(stderr, "jfsh: exec: argument expected: -a\n");
              return 1;
            }
            break;
          } else {
            s::fprintf(stderr, "jfsh: exec: bad option: -%c\n", f);
            return 1;
          }
        }
      }
      args.erase(args.begin(), args.begin() + static_cast<s::ptrdiff_t>(i));
      replace = true;
      continue;
    }
    if (w == "-" or w == "nocorrect") {
      if (w == "-") argv0 = "-";
      args.erase(args.begin());
      continue;
    }
    break;
  }

  // With no command word the redirections apply to the current shell and
  // the assignments update shell parameters.  Both happen in-process.
  if (args.empty()) {
    SavedFds saved;
    if (not redirs.empty()) {
      s::fflush(nullptr);
      if (not apply_redirects(redirs, saved)) return 1;
      if (replace) saved.commit();
    }
    int rc = apply_assigns(assigns);
    s::fflush(nullptr);
    // So does assigning to a readonly parameter.
    if (rc != 0) mCtx.aborting = true;
    // `x=$(cmd)` has cmd's status.
    if (rc == 0 and subst_status) rc = *subst_status;
    return rc;
  }

  if (replace) {
    s::fflush(nullptr);
    if (not redirs.empty()) {
      SavedFds saved;
      if (not apply_redirects(redirs, saved)) return 1;
      saved.commit();
    }
    return exec_replace(args, assigns, argv0, clear_env);
  }
  if (only == Only::External) return run_external(args, assigns, redirs, flags);

  // Functions come before builtins; an autoloaded one is loaded on its
  // first call.  Aliases were expanded by the parser.
  auto fn = mCtx.functions.end();
  if (only == Only::Any) {
    fn = mCtx.functions.find(args[0]);
    if (fn == mCtx.functions.end() and mCtx.autoloads.contains(args[0])) {
      if (not load_autoload(args[0])) return 1;
      fn = mCtx.functions.find(args[0]);
    }
  }
  auto def = fn == mCtx.functions.end() ? Builtins.find(args[0]) : s::nullopt;
  // A loaded module may have contributed the builtin; fall through to that
  // before trying an external command.
  const BuiltinDef* mod_def = nullptr;
  if (not def and fn == mCtx.functions.end())
    mod_def = mCtx.modules.find_builtin(args[0]);
  const BuiltinDef* defp = def ? &def->get() : mod_def;
  if (only == Only::Builtin and not defp) {
    s::fprintf(stderr, "jfsh: no such builtin: %s\n", args[0].c_str());
    return 1;
  }
  if (fn == mCtx.functions.end() and not defp)
    return run_external(args, assigns, redirs, flags);

  SavedFds saved;
  if (not redirs.empty()) {
    // Push pending buffered output through the ORIGINAL fds before we
    // rewire them; otherwise the output lands in the wrong place after
    // SavedFds restores (bug: stdio buffers survive dup2).
    s::fflush(nullptr);
    if (not apply_redirects(redirs, saved)) return 1;
  }
  int rc = 0;
  if (fn != mCtx.functions.end()) {
    ProgRef body = fn->second;  // the function may redefine itself
    // Assignments in front of a function are exported to what it runs.
    if (not assigns.empty()) rc = push_prefix_assigns(assigns, true);
    if (rc == 0) rc = call_function(body, args);
    if (not assigns.empty()) mCtx.params.pop_scope();
    s::fflush(nullptr);
    return rc;
  }
  // zsh keeps assignments in front of a special builtin only under
  // POSIX_BUILTINS; otherwise they last while the builtin runs.  Without
  // any, no scope is pushed, so `local` declares in the caller's scope.
  bool scoped = not assigns.empty();
  if ((defp->flags & BINF_SPECIAL) and mCtx.opts.get(OPT_POSIXBUILTINS)) {
    rc = apply_assigns(assigns);
    scoped = false;
  } else if (scoped) {
    rc = push_prefix_assigns(assigns, false);
  }
  if (rc == 0) rc = defp->fn(args, mCtx);
  // A signal the builtin brought on (`kill -USR1 $$`) is handled before
  // its status replaces $?, as in zsh.
  if (mCtx.signals.has_pending()) handle_signals();
  for (const auto& a : typeset_arrays) {
    if (rc != 0) break;
    if (auto r = mCtx.params.set_array(a.name, a.elements, a.append); not r) {
      s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
      rc = 1;
    }
  }
  if (scoped) mCtx.params.pop_scope();
  s::fflush(nullptr);
  return rc;
}

int Executor::push_prefix_assigns(const vec<Assignment>& assigns,
                                  bool exported) {
  mCtx.params.push_scope();
  for (const auto& a : assigns) {
    auto value = prefix_value(mCtx.params, a);
    if (not value) {
      s::fprintf(stderr, "jfsh: %s\n", value.error().message.c_str());
      return 1;
    }
    auto outer = mCtx.params.lookup(a.name);
    s::uint32_t attrs = a.array ? PM_ARRAY : PM_SCALAR;
    if (exported or (outer and outer->get().is_exported())) attrs |= PM_EXPORTED;
    if (a.array) mCtx.params.assign(a.name, a.elements, attrs);
    else         mCtx.params.assign(a.name, s::move(*value), attrs);
  }
  return 0;
}

int Executor::exec_list(ExecState& es, ExecFlags flags) {
  int rc = 0;
  while (not es.at_end()) {
    WordCode w = es.fetch();
    if (wc_code(w) != WC_LIST) break;  // WC_END
    s::size_t end = es.pc + wc_list_skip(w);
    if (mTrapDepth == 0 and mCtx.signals.traps().trapped(SIGDEBUG))
      run_trap(SIGDEBUG);
    if (interrupted()) return mCtx.exiting ? mCtx.exit_val : rc;
    if (wc_list_flags(w) & WC_LIST_ASYNC) {
      bool        jc   = job_control();
      // A lone pipeline (no && || !) is exec'd by the child itself, so the
      // job's pid is the command's own.
      WordCode    sl   = es.peek();
      bool        lone = (wc_sublist_flags(sl) & (WC_SUBLIST_TYPE | WC_SUBLIST_NOT))
                         == WC_SUBLIST_END;
      str text = sublist_text(*es.prog, es.pc);
      pid_t pid = fork_child(0, false, ChildKind::Other);
      if (pid < 0) {
        rc = mCtx.last_exit = 1;
      } else if (pid == 0) {
        if (jc and mCtx.opts.get(OPT_BGNICE)) {
          errno = 0;
          [[maybe_unused]] int r = ::nice(5);
        }
        if (not jc) {
          // Without job control a background job must not see Ctrl-C.
          ::signal(SIGINT,  SIG_IGN);
          ::signal(SIGQUIT, SIG_IGN);
        }
        ExecState se{es.prog, es.pc};
        int child_rc = exec_sublist(se, lone ? EF_NOFORK : EF_NONE);
        if (mCtx.exiting) child_rc = mCtx.exit_val;
        s::fflush(nullptr);
        _exit(child_rc);
      } else {
        int jid = mCtx.jobs.alloc(pid, JOB_ASYNC | JOB_NOTIFY | (jc ? +JOB_PGRP : 0),
                                  text);
        Process p;
        p.pid    = pid;
        p.text   = text;
        p.status = ProcStatus::Running;
        mCtx.jobs.add_process(jid, s::move(p));
        mCtx.last_bg_pid = pid;
        if (mCtx.opts.interactive())
          s::fprintf(stderr, "[%d] %d\n", jid, static_cast<int>(pid));
        rc = mCtx.last_exit = 0;
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

bool Executor::loop_done() noexcept {
  if (mCtx.exiting or mCtx.returning or mCtx.aborting) return true;
  if (mCtx.break_level == 0) return false;
  if (--mCtx.break_level > 0 or not mCtx.continuing) return true;
  mCtx.continuing = false;
  return false;
}

// Runs a chain of pipelines joined by && / ||.  A pipeline whose connector
// rules it out is skipped, but the chain goes on: in `false && a || b`, `a`
// is skipped and `b` runs.
// Whether a failure of the pipeline at `pc` (a WC_PIPE) is one ZERR and
// ERR_EXIT act on.  As in zsh: simple commands (function calls included),
// pipelines, `( … )`, `[[ … ]]` and `(( … ))` are; `{ … }`, if, loops and
// case are not, since the failing command inside them already was.
static bool failure_counts(const CompiledProgram& prog, s::size_t pc) {
  WordCode pipe = prog.prog[pc];
  if (wc_pipe_count(pipe) != 1) return true;
  s::size_t i = pc + 1;
  while (i < prog.prog.size() and wc_code(prog.prog[i]) == WC_REDIR) i += 2;
  if (i >= prog.prog.size()) return true;
  switch (wc_code(prog.prog[i])) {
    case WC_IF: case WC_WHILE: case WC_FOR: case WC_FOR_COND: case WC_CASE:
    case WC_SELECT: case WC_REPEAT: case WC_CURSH: case WC_TRY:
      return false;
    default:
      return true;
  }
}

void Executor::on_command_error(int rc) {
  // ZERR and ERR_EXIT are off anywhere inside a condition or a && / ||
  // chain, and act once per failure (a function whose last command failed
  // already set them off).  ERR_RETURN only minds the command's own place
  // and acts at every level, so a failure returns through each caller.
  bool exempt = mCtx.noerrexit > 0 or mCtx.err_reported;
  mCtx.err_reported = true;
  // Neither ZERR nor DEBUG runs inside another trap.
  if (not exempt and mTrapDepth == 0 and mCtx.signals.traps().trapped(SIGZERR))
    run_trap(SIGZERR);
  if (mCtx.exiting) return;
  if (not exempt and mCtx.opts.errexit()) {
    mCtx.exiting  = true;
    mCtx.exit_val = rc;
  } else if (mCtx.opts.get(OPT_ERRRETURN) and mCtx.noerrreturn == 0) {
    mCtx.returning  = true;
    mCtx.return_val = rc;
  }
}

int Executor::exec_sublist(ExecState& es, ExecFlags flags) {
  int  rc   = 0;
  bool skip = false;
  for (;;) {
    WordCode  w    = es.fetch();  // WC_SUBLIST
    WordCode  f    = wc_sublist_flags(w);
    s::size_t end  = es.pc + wc_sublist_skip(w);
    WordCode  type = f & WC_SUBLIST_TYPE;
    if (not skip) {
      // Only the last pipeline of a && / || chain, not negated, can set
      // off ZERR and ERR_EXIT; everything the others run is exempt.
      bool exempt = type != WC_SUBLIST_END or (f & WC_SUBLIST_NOT);
      bool counts = failure_counts(*es.prog, es.pc);
      mCtx.err_reported = false;
      if (exempt) { ++mCtx.noerrexit; ++mCtx.noerrreturn; }
      rc = exec_pline(es, flags);
      if (exempt) { --mCtx.noerrexit; --mCtx.noerrreturn; }
      if (f & WC_SUBLIST_NOT) rc = (rc == 0) ? 1 : 0;
      mCtx.last_exit = rc;
      if (rc != 0 and not exempt and counts and not interrupted())
        on_command_error(rc);
    }
    es.pc = end;
    if (mCtx.signals.has_pending()) handle_signals();
    if (interrupted()) return rc;
    if (type == WC_SUBLIST_END) return rc;
    skip = (type == WC_SUBLIST_AND) ? rc != 0 : rc == 0;
  }
}

int Executor::exec_pline(ExecState& es, ExecFlags flags) {
  s::size_t pipe_pc = es.pc;
  WordCode w     = es.fetch();  // WC_PIPE
  WordCode n     = wc_pipe_count(w);
  if (n == 0) return 0;
  mPipeProg = es.prog;
  mPipePc   = pipe_pc;
  if (n == 1) {
    int rc = exec_cmd(es, flags);
    mCtx.pipestatus.assign(1, rc);
    return rc;
  }
  bool jc = job_control();

  // Pre-decode every stage's bytecode slice so we can fork them from the
  // parent without re-entering the parser.  Each stage knows its own
  // start and end pc in the compiled program.
  struct Stage { s::size_t start; s::size_t end; bool stderr_too; };
  vec<Stage> stages;
  stages.reserve(n);
  for (s::uint32_t i = 0; i < n; ++i) {
    s::size_t start = es.pc;
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
        WordCode nitems = wc_data(es.fetch());
        for (WordCode a = 0; a < nitems; ++a)
          es.pc += wc_code(es.peek()) == WC_ASSIGN ? 3u : 1u;
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
  vec<int> rfds(n, -1), wfds(n, -1);
  for (s::uint32_t i = 0; i + 1 < n; ++i) {
    int p[2];
    if (::pipe(p) != 0) {
      s::fprintf(stderr, "jfsh: pipe: %s\n", s::strerror(errno));
      for (s::uint32_t j = 0; j < i; ++j) { ::close(rfds[j + 1]); ::close(wfds[j]); }
      return 1;
    }
    rfds[i + 1] = p[0];
    wfds[i]     = p[1];
  }

  vec<pid_t> pids(n, -1);
  pid_t pgid = 0;
  for (s::uint32_t i = 0; i < n; ++i) {
    pid_t pid = fork_child(pgid, true, ChildKind::Other);
    if (pid < 0) {
      for (int fd : rfds) if (fd >= 0) ::close(fd);
      for (int fd : wfds) if (fd >= 0) ::close(fd);
      // The stages already started still have to be waited for.
      for (s::uint32_t j = 0; j < i; ++j) {
        int st;
        while (::waitpid(pids[j], &st, 0) < 0 and errno == EINTR) {}
      }
      return 1;
    }
    if (pid == 0) {
      if (rfds[i] >= 0) { ::dup2(rfds[i], 0); }
      if (wfds[i] >= 0) {
        ::dup2(wfds[i], 1);
        if (stages[i].stderr_too) ::dup2(wfds[i], 2);
      }
      for (int fd : rfds) if (fd >= 0) ::close(fd);
      for (int fd : wfds) if (fd >= 0) ::close(fd);
      ExecState se{es.prog, stages[i].start};
      int rc = exec_cmd(se, static_cast<ExecFlags>(flags | EF_NOFORK));
      if (mCtx.exiting) rc = mCtx.exit_val;
      s::fflush(nullptr);
      _exit(rc);
    }
    if (i == 0) pgid = pid;
    pids[i] = pid;
  }
  for (int fd : rfds) if (fd >= 0) ::close(fd);
  for (int fd : wfds) if (fd >= 0) ::close(fd);

  if (jc) {
    auto texts = pipeline_stage_texts(*es.prog, pipe_pc);
    str text;
    for (s::size_t i = 0; i < texts.size(); ++i) {
      if (i) text += wc_pipe_stderr(w, static_cast<s::uint32_t>(i - 1)) ? " |& " : " | ";
      text += texts[i];
    }
    int jid = mCtx.jobs.alloc(pgid, JOB_PGRP, s::move(text), false);
    for (s::uint32_t i = 0; i < n; ++i) {
      Process p;
      p.pid  = pids[i];
      p.text = i < texts.size() ? texts[i] : str{};
      mCtx.jobs.add_process(jid, s::move(p));
    }
    return wait_foreground_job(mCtx, jid);
  }

  int last_rc = 0;
  int worst   = 0;
  mCtx.pipestatus.clear();
  for (s::uint32_t i = 0; i < n; ++i) {
    int status = 0;
    while (::waitpid(pids[i], &status, 0) < 0) {
      if (errno != EINTR) { worst = 1; break; }
    }
    int rc = 0;
    if (WIFEXITED(status))        rc = WEXITSTATUS(status);
    else if (WIFSIGNALED(status)) rc = 128 + WTERMSIG(status);
    if (rc != 0) worst = rc;
    last_rc = rc;
    mCtx.pipestatus.push_back(rc);
  }
  return mCtx.opts.get(OPT_PIPEFAIL) ? worst : last_rc;
}

// $LINENO for the command at es.pc: its line in the script, -c string,
// sourced file or eval text, or in a function counted from the line the
// definition starts on.
static s::uint32_t line_at(const ExecState& es) noexcept {
  const auto& lines = es.prog->line_map;
  if (es.pc >= lines.size()) return 0;
  s::uint32_t l = lines[es.pc];
  if (es.prog->line_base == 0) return l;
  return l > es.prog->line_base ? l - es.prog->line_base : 1;
}

int Executor::exec_cmd(ExecState& es, ExecFlags flags) {
  if (es.at_end()) return 0;
  mCtx.lineno = line_at(es);
  // Redirections in front of a compound command belong to it; a simple
  // command goes on with an assignment or its words.
  s::size_t pc = es.pc;
  while (wc_code(es.prog->prog[pc]) == WC_REDIR) pc += 2;
  switch (wc_code(es.prog->prog[pc])) {
    case WC_ASSIGN: case WC_SIMPLE: case WC_TYPESET:
      return exec_simple_cmd(es, flags);
    default: break;
  }
  if (pc == es.pc) return exec_compound(es, flags);

  vec<Redirection> redirs;
  while (wc_code(es.peek()) == WC_REDIR) redirs.push_back(fetch_redir(es));
  vec<str>        no_args;
  vec<Assignment> no_assigns;
  opt<int>        subst_status;
  if (not expand_command(no_args, no_assigns, redirs, subst_status)) return 1;
  SavedFds saved;
  s::fflush(nullptr);
  if (not apply_redirects(redirs, saved)) return 1;
  int rc = exec_compound(es, flags);
  s::fflush(nullptr);
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
      s::size_t end = es.pc + wc_cmd_skip(head);
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
    a.name   = str(es.fetch_str());
    a.value  = str(es.fetch_str());
    a.append = f & WC_ASSIGN_APPEND;
    a.array  = f & WC_ASSIGN_ARRAY;
    return a;
  };
  vec<Assignment> assigns;
  vec<Redirection> redirs;
  for (;;) {
    WordCode op = wc_code(es.peek());
    if      (op == WC_ASSIGN) assigns.push_back(fetch_assign());
    else if (op == WC_REDIR)  redirs.push_back(fetch_redir(es));
    else break;
  }
  WordCode head = es.fetch();  // WC_SIMPLE or WC_TYPESET
  WordCode argc = wc_data(head);
  vec<str>        words;
  vec<Assignment> typeset_args;
  words.reserve(argc);
  for (WordCode i = 0; i < argc; ++i) {
    if (wc_code(es.peek()) == WC_ASSIGN) typeset_args.push_back(fetch_assign());
    else                                 words.emplace_back(es.fetch_str());
  }
  return exec_simple(s::move(words), s::move(assigns), s::move(redirs),
                     flags, s::move(typeset_args));
}

// if := WC_IF(HEAD, skip) branch+; the first branch whose condition
// succeeds runs.  With none, the status is 0.
int Executor::exec_if(ExecState& es, WordCode head) {
  s::size_t end = es.pc + wc_cmd_skip(head);
  int rc = 0;
  while (es.pc < end) {
    WordCode branch = es.fetch();
    s::size_t next = es.pc + wc_cmd_skip(branch);
    if (wc_cmd_type(branch) == WC_IF_ELSE) {
      rc = exec_list(es, EF_NONE);
      break;
    }
    ++mCtx.noerrexit;
    ++mCtx.noerrreturn;
    int status = exec_list(es, EF_NONE);
    --mCtx.noerrexit;
    --mCtx.noerrreturn;
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
  s::size_t end   = es.pc + wc_cmd_skip(head);
  s::size_t cond  = es.pc;
  bool      until = wc_cmd_type(head) == WC_WHILE_UNTIL;
  int rc = 0;
  ++mCtx.loop_depth;
  for (;;) {
    es.pc = cond;
    ++mCtx.noerrexit;
    ++mCtx.noerrreturn;
    int status = exec_list(es, EF_NONE);
    --mCtx.noerrexit;
    --mCtx.noerrreturn;
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
                           vec<str>& names,
                           vec<str>& values) {
  names = fetch_words(es);
  if (wc_cmd_type(head) == WC_FOR_ARGS) {
    values = mCtx.positional;
    return true;
  }
  auto raw = fetch_words(es);
  Expander ex{*this, mCtx};
  auto expanded = ex.words(raw);
  if (not expanded) {
    s::fprintf(stderr, "jfsh: %s\n", expanded.error().message.c_str());
    return false;
  }
  values = s::move(*expanded);
  return true;
}

// for := WC_FOR(IN|ARGS, skip) words(names) [words(values)] body.  With
// several names each pass takes that many values; missing ones are empty.
int Executor::exec_for(ExecState& es, WordCode head) {
  s::size_t end = es.pc + wc_cmd_skip(head);
  vec<str> names, values;
  if (not loop_values(es, head, names, values)) {
    es.pc = end;
    return 1;
  }
  s::size_t body = es.pc;
  int rc = 0;
  ++mCtx.loop_depth;
  for (s::size_t i = 0; i < values.size(); i += names.size()) {
    bool ok = true;
    for (s::size_t k = 0; k < names.size() and ok; ++k) {
      str v = i + k < values.size() ? values[i + k] : str{};
      if (auto r = mCtx.params.set_scalar(names[k], s::move(v)); not r) {
        s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
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

static bool is_blank(strv s) noexcept {
  return s.find_first_not_of(" \t\n") == strv::npos;
}

// cfor := WC_FOR_COND(0, skip) WC_STRING(init) WC_STRING(cond)
// WC_STRING(step) body.  An empty condition is true.
int Executor::exec_for_cond(ExecState& es, WordCode head) {
  s::size_t end = es.pc + wc_cmd_skip(head);
  str init(es.fetch_str());
  str cond(es.fetch_str());
  str step(es.fetch_str());
  s::size_t body = es.pc;
  Expander ex{*this, mCtx};
  auto eval = [&](const str& expr, bool& value) {
    value = true;
    if (is_blank(expr)) return true;
    auto v = ex.math(expr);
    if (not v) {
      s::fprintf(stderr, "jfsh: %s\n", v.error().message.c_str());
      return false;
    }
    value = v->truthy();
    return true;
  };
  int rc = 0;
  bool value = false;
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
    if (not value) break;
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
  s::size_t end = es.pc + wc_cmd_skip(head);
  Expander ex{*this, mCtx};
  auto fail = [&](const ShellError& e) {
    s::fprintf(stderr, "jfsh: %s\n", e.message.c_str());
    es.pc = end;
    return 1;
  };
  auto subject = ex.scalar(es.fetch_str());
  if (not subject) return fail(subject.error());
  int  rc   = 0;
  bool fall = false;
  while (es.pc < end) {
    WordCode arm = es.fetch();
    s::size_t next = es.pc + wc_cmd_skip(arm);
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
static opt<str> read_line_fd0() {
  str line;
  for (;;) {
    char c;
    ssize_t n = ::read(STDIN_FILENO, &c, 1);
    if (n < 0 and errno == EINTR) continue;
    if (n <= 0) {
      if (line.empty()) return s::nullopt;
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
  s::size_t end = es.pc + wc_cmd_skip(head);
  vec<str> names, values;
  if (not loop_values(es, head, names, values)) {
    es.pc = end;
    return 1;
  }
  s::size_t body = es.pc;
  int rc = 0;
  if (values.empty()) {
    es.pc = end;
    return 0;
  }
  auto show_menu = [&] {
    str menu;
    s::size_t width = 0;
    for (s::size_t i = 0; i < values.size(); ++i) {
      str item = s::to_string(i + 1) + ") " + values[i] + "  ";
      if (width > 0 and width + item.size() > 80) {
        menu += '\n';
        width = 0;
      }
      menu += item;
      width += item.size();
    }
    s::fprintf(stderr, "%s\n", menu.c_str());
  };
  ++mCtx.loop_depth;
  bool menu = true;
  for (;;) {
    if (menu) show_menu();
    auto ps3 = mCtx.params.lookup("PS3");
    str prompt = expand_prompt((ps3 and ps3->get().is_set()) ? ps3->get().scalar() : "?# ");
    s::fprintf(stderr, "%s", prompt.c_str());
    s::fflush(stderr);
    auto line = read_line_fd0();
    if (not line) {
      s::fputc('\n', stderr);
      break;
    }
    menu = line->empty();
    if (menu) continue;
    (void)mCtx.params.set_scalar("REPLY", *line);
    char* rest = nullptr;
    long n = s::strtol(line->c_str(), &rest, 10);
    str choice = (*rest == '\0' and n >= 1
                  and static_cast<s::size_t>(n) <= values.size())
                       ? values[static_cast<s::size_t>(n) - 1] : str{};
    if (auto r = mCtx.params.set_scalar(names[0], choice); not r) {
      s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
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
  s::size_t end = es.pc + wc_cmd_skip(head);
  Expander ex{*this, mCtx};
  auto count = ex.arith(es.fetch_str());
  if (not count) {
    s::fprintf(stderr, "jfsh: %s\n", count.error().message.c_str());
    es.pc = end;
    return 1;
  }
  s::size_t body = es.pc;
  int rc = 0;
  ++mCtx.loop_depth;
  for (s::int64_t i = 0; i < *count; ++i) {
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
  s::size_t end = es.pc + wc_cmd_skip(head);
  bool jc = job_control();
  pid_t pid = fork_child(0, true, ChildKind::Subshell);
  if (pid < 0) {
    es.pc = end;
    return 1;
  }
  if (pid == 0) {
    mExitTrapRan = false;
    int rc = exec_list(es, EF_NONE);
    if (mCtx.exiting) rc = mCtx.exit_val;
    else if (mCtx.returning) rc = mCtx.return_val;
    rc = run_exit_trap(rc);
    s::fflush(nullptr);
    _exit(rc);
  }
  es.pc = end;
  if (jc) {
    int jid = mCtx.jobs.alloc(pid, JOB_PGRP, current_job_text(), false);
    Process p;
    p.pid = pid;
    mCtx.jobs.add_process(jid, s::move(p));
    return wait_foreground_job(mCtx, jid);
  }
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
  s::size_t end = es.pc + wc_cmd_skip(head);
  // The line the definition starts on is line 0 of the body's $LINENO.
  s::uint32_t def_line = es.pc > 0 and es.pc - 1 < es.prog->line_map.size()
                       ? es.prog->line_map[es.pc - 1] : 0;
  if (wc_cmd_type(head) == WC_FUNCDEF_ANON) {
    s::size_t begin = es.pc;
    es.pc = body_end(*es.prog, begin);
    ProgRef body = extract_body(*es.prog, begin, es.pc);
    body->line_base = def_line;
    auto raw = fetch_words(es);
    es.pc = end;
    Expander ex{*this, mCtx};
    auto args = ex.words(raw);
    if (not args) {
      s::fprintf(stderr, "jfsh: %s\n", args.error().message.c_str());
      return 1;
    }
    args->insert(args->begin(), "(anon)");
    return call_function(body, *args);
  }
  auto names = fetch_words(es);
  ProgRef body = extract_body(*es.prog, es.pc, end);
  body->line_base = def_line;
  for (auto& name : names) {
    // TRAPINT() { … } is the trap for SIGINT, and so on.
    if (name.starts_with("TRAP"))
      if (auto sig = SignalTable::name_to_sig(strv(name).substr(4));
          sig and not s::isdigit(static_cast<unsigned char>(name[4])))
        mCtx.signals.set_trap(*sig, TrapAction::Function);
    mCtx.autoloads.erase(name);
    mCtx.function_files[name] = mCtx.source_name.empty() ? mCtx.arg0 : mCtx.source_name;
    mCtx.functions[s::move(name)] = body;
  }
  es.pc = end;
  return 0;
}

// cond := WC_COND(type, skip) …, a `[[ ]]` tree (see wordcode.h).  The
// status is 0 when true, 1 when false, 2 on an error and 3 for `-o` with
// no such option; `&&` and `||` stop at an error.  Operands are expanded
// without splitting or globbing; the right side of = == != is a pattern,
// of =~ an extended regex, and -eq … -ge compare arithmetic expressions.
int Executor::exec_cond(ExecState& es, WordCode head) {
  s::size_t end = es.pc + wc_cmd_skip(head);
  Expander ex{*this, mCtx};
  auto finish = [&](int rc) {
    es.pc = end;
    return rc;
  };
  auto fail = [&](const ShellError& e) {
    s::fprintf(stderr, "jfsh: %s\n", e.message.c_str());
    return finish(1);
  };
  switch (wc_cmd_type(head)) {
    case WC_COND_NOT: {
      int rc = exec_cond(es, es.fetch());
      return finish(rc < 2 ? 1 - rc : rc);
    }
    case WC_COND_AND: case WC_COND_OR: {
      int rc = exec_cond(es, es.fetch());
      if (rc == (wc_cmd_type(head) == WC_COND_AND ? 0 : 1)) rc = exec_cond(es, es.fetch());
      return finish(rc);
    }
    case WC_COND_STR: {
      auto word = ex.scalar(es.fetch_str());
      if (not word) return fail(word.error());
      return finish(word->empty() ? 1 : 0);
    }
    case WC_COND_UNARY: {
      str op(es.fetch_str());
      auto arg = ex.scalar(es.fetch_str());
      if (not arg) return fail(arg.error());
      return finish(cond_unary(op, *arg, mCtx, ""));
    }
    default: break;
  }
  str op(es.fetch_str());
  strv lhs_word = es.fetch_str();
  strv rhs_word = es.fetch_str();
  if (cond_is_numeric(op)) {
    auto l = ex.math(lhs_word);
    if (not l) return fail(l.error());
    auto r = ex.math(rhs_word);
    if (not r) return fail(r.error());
    bool fl = l->is_float or r->is_float;
    int cmp = fl ? (l->as_float() < r->as_float() ? -1 : l->as_float() > r->as_float() ? 1 : 0)
                 : (l->i < r->i ? -1 : l->i > r->i ? 1 : 0);
    return finish(cond_compare(op, cmp));
  }
  auto lhs = ex.scalar(lhs_word);
  if (not lhs) return fail(lhs.error());
  if (op == "=" or op == "==" or op == "!=") {
    auto pat = ex.pattern(rhs_word);
    if (not pat) return fail(pat.error());
    bool match = CompiledPattern::compile(*pat, false, mCtx.opts.extended_glob()).matches(*lhs);
    return finish(match == (op != "!=") ? 0 : 1);
  }
  auto rhs = ex.scalar(rhs_word);
  if (not rhs) return fail(rhs.error());
  if (op == "=~") return finish(cond_regex(*lhs, *rhs, mCtx, ""));
  if (op == "<")  return finish(*lhs < *rhs ? 0 : 1);
  if (op == ">")  return finish(*lhs > *rhs ? 0 : 1);
  if (op == "-nt" or op == "-ot" or op == "-ef") return finish(cond_file_binary(op, *lhs, *rhs));
  // Module-provided conditions (M10 Phase 2c polish).  The parser accepts
  // `-pcre-match` / `-regex-match` as binary ops whether or not the owning
  // module is loaded; this dispatch runs the function if the module is
  // loaded and reports a clear error otherwise.
  if (auto* cd = mCtx.modules.find_cond(op))
    return finish(cd->fn(*lhs, *rhs, mCtx));
  if (op == "-pcre-match" or op == "-regex-match") {
    s::fprintf(stderr, "jfsh: [[: %s: module not loaded (zmodload %s)\n",
               op.c_str(),
               op == "-pcre-match" ? "zsh/pcre" : "zsh/regex");
    return finish(2);
  }
  s::fprintf(stderr, "jfsh: unknown condition: %s\n", op.c_str());
  return finish(2);
}

// arith := WC_ARITH(0, 1) WC_STRING(expr).  Status 0 when the value is
// non-zero, 1 when it is zero, 2 on an error.
int Executor::exec_arith(ExecState& es, WordCode) {
  Expander ex{*this, mCtx};
  auto v = ex.math(es.fetch_str());
  if (not v) {
    s::fprintf(stderr, "jfsh: %s\n", v.error().message.c_str());
    return 2;
  }
  return v->truthy() ? 0 : 1;
}

// try := WC_TRY(0, skip) body body(always).  The always block runs however
// the first one ended; a break, continue, return or exit pending from the
// first block is held while it runs and then goes on, unless the always
// block started one of its own.  The status is the first block's.
int Executor::exec_always(ExecState& es, WordCode head) {
  s::size_t end    = es.pc + wc_cmd_skip(head);
  s::size_t always = body_end(*es.prog, es.pc);
  int rc = exec_list(es, EF_NONE);
  auto held = s::tuple{mCtx.break_level, mCtx.continuing, mCtx.returning,
                       mCtx.return_val, mCtx.exiting, mCtx.exit_val};
  mCtx.break_level = 0;
  mCtx.continuing  = false;
  mCtx.returning   = false;
  mCtx.exiting     = false;
  es.pc = always;
  exec_list(es, EF_NONE);
  if (not interrupted()) {
    s::tie(mCtx.break_level, mCtx.continuing, mCtx.returning,
           mCtx.return_val, mCtx.exiting, mCtx.exit_val) = held;
  }
  es.pc = end;
  return rc;
}

// ---------------------------------------------------------------------------
// SavedFds: RAII restore of fds overwritten by builtin redirections.
// ---------------------------------------------------------------------------
SavedFds& SavedFds::operator=(SavedFds&& other) noexcept {
  if (this != &other) {
    restore();
    entries    = s::move(other.entries);
    fail       = other.fail;
    other.entries.clear();
  }
  return *this;
}

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

void SavedFds::commit() noexcept {
  for (const auto& e : entries)
    if (e.backup >= 0) ::close(e.backup);
  entries.clear();
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
static int parse_fd_target(const str& s) {
  if (s == "-") return -2;
  if (s.empty()) return -1;
  char* end = nullptr;
  long v = s::strtol(s.c_str(), &end, 10);
  if (end == s.c_str() or *end != '\0' or v < 0 or v > 1023) return -1;
  return static_cast<int>(v);
}

// Pour `body` into a pipe-backed fd suitable for stdin of a child.
// Uses memfd_create when available, falling back to a pipe + background
// writer fork so the writer cannot block the caller (bodies fit in one
// pipe buffer in practice, but a dedicated writer is the simplest
// general answer).
static int heredoc_body_fd(strv body) {
  int pfd[2];
  if (::pipe(pfd) != 0) return -1;
  pid_t pid = ::fork();
  if (pid < 0) { ::close(pfd[0]); ::close(pfd[1]); return -1; }
  if (pid == 0) {
    Signals.child_reset(ChildKind::Other);
    ::close(pfd[0]);
    ::signal(SIGPIPE, SIG_DFL);
    const char* p   = body.data();
    s::size_t n     = body.size();
    while (n > 0) {
      ssize_t w = ::write(pfd[1], p, n);
      if (w < 0) { if (errno == EINTR) continue; _exit(0); }
      p += w; n -= static_cast<s::size_t>(w);
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
int Executor::procsubst_in(strv src) {
  int pfd[2];
  if (::pipe(pfd) != 0) return -1;
  pid_t pid = ::fork();
  if (pid < 0) { ::close(pfd[0]); ::close(pfd[1]); return -1; }
  if (pid == 0) {
    mCtx.signals.child_reset(ChildKind::Other);
    mCtx.jobs.clear();
    ::close(pfd[0]);
    ::dup2(pfd[1], 1);
    if (pfd[1] != 1) ::close(pfd[1]);
    int rc = exec_string(src);
    s::fflush(nullptr);
    _exit(rc);
  }
  ::close(pfd[1]);
  return pfd[0];
}

// Fork a subshell that runs `src` with its stdin piped from a fresh fd.
// Returns the write end for the parent to pass as /dev/fd/N.
int Executor::procsubst_out(strv src) {
  int pfd[2];
  if (::pipe(pfd) != 0) return -1;
  pid_t pid = ::fork();
  if (pid < 0) { ::close(pfd[0]); ::close(pfd[1]); return -1; }
  if (pid == 0) {
    mCtx.signals.child_reset(ChildKind::Other);
    mCtx.jobs.clear();
    ::close(pfd[1]);
    ::dup2(pfd[0], 0);
    if (pfd[0] != 0) ::close(pfd[0]);
    int rc = exec_string(src);
    s::fflush(nullptr);
    _exit(rc);
  }
  ::close(pfd[0]);
  return pfd[1];
}

// Run `src` with its stdout captured into a fresh temp file and return
// the file's path.  Used for =(cmd).
str Executor::procsubst_eq(strv src) {
  char tmpl[] = "/tmp/jfsh-procsubst-XXXXXX";
  int fd = ::mkstemp(tmpl);
  if (fd < 0) return {};
  pid_t pid = ::fork();
  if (pid < 0) { ::close(fd); ::unlink(tmpl); return {}; }
  if (pid == 0) {
    mCtx.signals.child_reset(ChildKind::Other);
    mCtx.jobs.clear();
    ::dup2(fd, 1);
    if (fd != 1) ::close(fd);
    int rc = exec_string(src);
    s::fflush(nullptr);
    _exit(rc);
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 and errno == EINTR) {}
  ::close(fd);
  return str(tmpl);
}

// Fork a child that reads lines from `read_fd` and writes each byte to all
// file descriptors in `out_fds`.  `extra_close` lists fds (typically the
// pipe's write end) to close in the child so read() can see EOF once the
// parent releases its writers.
static int spawn_tee(int read_fd, const vec<int>& out_fds,
                     int extra_close) {
  pid_t pid = ::fork();
  if (pid < 0) return -1;
  if (pid == 0) {
    Signals.child_reset(ChildKind::Other);
    if (extra_close >= 0) ::close(extra_close);
    char buf[4096];
    for (;;) {
      ssize_t n = ::read(read_fd, buf, sizeof buf);
      if (n <= 0) break;
      for (int fd : out_fds) {
        const char* p = buf;
        ssize_t left = n;
        while (left > 0) {
          ssize_t w = ::write(fd, p, static_cast<s::size_t>(left));
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
static vec<s::size_t> multios_merge(
    const vec<Redirection>& redirs,
    SavedFds& saved,
    bool multios_on) {
  vec<s::size_t> consumed;
  if (not multios_on) return consumed;

  auto is_output_file = [](RedirType t) noexcept {
    return t == REDIR_WRITE or t == REDIR_APPEND
        or t == REDIR_CLOBBER or t == REDIR_MERGEOUT
        or t == REDIR_APPEND_ERR;
  };
  auto default_for = [](RedirType t) noexcept { return t == REDIR_APPEND_ERR
      or t == REDIR_MERGEOUT ? 1 : 1; };

  // Group by destination fd.
  umap<int, vec<s::size_t>> by_fd;
  for (s::size_t i = 0; i < redirs.size(); ++i) {
    const auto& r = redirs[i];
    if (not is_output_file(r.type)) continue;
    int dst = r.fd >= 0 ? r.fd : default_for(r.type);
    by_fd[dst].push_back(i);
  }

  for (auto& [dst, idxs] : by_fd) {
    if (idxs.size() < 2) continue;  // no MULTIOS trigger with one target
    vec<int> fds;
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
        s::fprintf(stderr, "jfsh: %s: %s\n",
                   r.target.c_str(), s::strerror(errno));
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

bool Executor::apply_redirects(const vec<Redirection>& redirs,
                               SavedFds& saved) {
  auto consumed = multios_merge(redirs, saved,
                                mCtx.opts.get(OPT_MULTIOS));
  s::sort(consumed.begin(), consumed.end());
  auto is_consumed = [&](s::size_t i) {
    return s::binary_search(consumed.begin(), consumed.end(), i);
  };
  for (s::size_t idx = 0; idx < redirs.size(); ++idx) {
    if (is_consumed(idx)) continue;
    const auto& r = redirs[idx];
    int dst = r.fd >= 0 ? r.fd : default_fd(r.type);

    auto dup_into = [&](int src_fd) -> bool {
      // A new fd that landed on dst itself (dst was closed: `3<<<x`) is
      // moved off first, so dst is saved as closed, not as the new fd.
      if (src_fd == dst) {
        int moved = ::fcntl(src_fd, F_DUPFD_CLOEXEC, 10);
        ::close(src_fd);
        if (moved < 0) {
          s::fprintf(stderr, "jfsh: dup: %s\n", s::strerror(errno));
          return false;
        }
        src_fd = moved;
      }
      saved.save(dst);
      if (::dup2(src_fd, dst) < 0) {
        s::fprintf(stderr, "jfsh: dup2: %s\n", s::strerror(errno));
        return false;
      }
      ::close(src_fd);
      return true;
    };

    switch (r.type) {
      case REDIR_READ: {
        int fd = ::open(r.target.c_str(), O_RDONLY);
        if (fd < 0) {
          s::fprintf(stderr, "jfsh: %s: %s\n",
                     r.target.c_str(), s::strerror(errno));
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
          s::fprintf(stderr, "jfsh: %s: %s\n",
                     r.target.c_str(), s::strerror(errno));
          return false;
        }
        if (not dup_into(fd)) return false;
      }
      break; case REDIR_APPEND: {
        int fd = ::open(r.target.c_str(),
                        O_WRONLY | O_CREAT | O_APPEND, 0666);
        if (fd < 0) {
          s::fprintf(stderr, "jfsh: %s: %s\n",
                     r.target.c_str(), s::strerror(errno));
          return false;
        }
        if (not dup_into(fd)) return false;
      }
      break; case REDIR_READWRITE: {
        int fd = ::open(r.target.c_str(), O_RDWR | O_CREAT, 0666);
        if (fd < 0) {
          s::fprintf(stderr, "jfsh: %s: %s\n",
                     r.target.c_str(), s::strerror(errno));
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
          s::fprintf(stderr, "jfsh: %s: %s\n",
                     r.target.c_str(), s::strerror(errno));
          return false;
        }
        saved.save(1);
        saved.save(2);
        if (::dup2(fd, 1) < 0 or ::dup2(fd, 2) < 0) {
          s::fprintf(stderr, "jfsh: dup2: %s\n", s::strerror(errno));
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
          s::fprintf(stderr, "jfsh: %s: bad fd\n", r.target.c_str());
          return false;
        }
        saved.save(dst);
        if (::dup2(src, dst) < 0) {
          s::fprintf(stderr, "jfsh: dup2: %s\n", s::strerror(errno));
          return false;
        }
      }
      break; case REDIR_CLOSE: {
        saved.save(dst);
        ::close(dst);
      }
      break; case REDIR_HEREDOC: case REDIR_HEREDOCD: case REDIR_HERESTR: {
        str body = r.target;
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
        s::fprintf(stderr, "jfsh: redirection type %d not supported\n",
                   static_cast<int>(r.type));
        return false;
    }
  }
  return not saved.fail;
}

Executor::ProcSubstResources Executor::expand_procsubst(
    vec<str>& args) {
  ProcSubstResources res;
  for (auto& a : args) {
    if (a.size() < 3 or a.back() != ')') continue;
    strv prefix(a.data(), 2);
    if (prefix != "<(" and prefix != ">(" and prefix != "=(") continue;
    str body = a.substr(2, a.size() - 3);
    if (prefix == "<(") {
      int fd = procsubst_in(body);
      if (fd < 0) { a.clear(); continue; }
      res.open_fds.push_back(fd);
      a = "/dev/fd/" + s::to_string(fd);
    } else if (prefix == ">(") {
      int fd = procsubst_out(body);
      if (fd < 0) { a.clear(); continue; }
      res.open_fds.push_back(fd);
      a = "/dev/fd/" + s::to_string(fd);
    } else {
      str path = procsubst_eq(body);
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

bool Executor::apply_redirects_child(const vec<Redirection>& redirs) {
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
bool Executor::expand_command(vec<str>& args,
                              vec<Assignment>&  assigns,
                              vec<Redirection>& redirs,
                              opt<int>&         subst_status) {
  Expander ex{*this, mCtx};
  auto fail = [&](const ShellError& e) {
    s::fprintf(stderr, "jfsh: %s\n", e.message.c_str());
    subst_status = ex.subst_status();
    return false;
  };
  // Assignment values are expanded first, then the arguments, then the
  // redirection targets.
  for (auto& a : assigns)
    if (auto r = expand_assignment(ex, a); not r) return fail(r.error());
  auto words = ex.words(args);
  if (not words) return fail(words.error());
  args = s::move(*words);

  vec<Redirection> out;
  out.reserve(redirs.size());
  for (auto& r : redirs) {
    switch (r.type) {
      case REDIR_HEREDOC: case REDIR_HEREDOCD:
        if (not r.literal) {
          auto body = ex.heredoc(r.target);
          if (not body) return fail(body.error());
          r.target = s::move(*body);
        }
        out.push_back(s::move(r));
      break; case REDIR_HERESTR: case REDIR_MERGE: case REDIR_IN_DUP: {
        auto v = ex.scalar(r.target);
        if (not v) return fail(v.error());
        r.target = s::move(*v);
        out.push_back(s::move(r));
      }
      break; case REDIR_CLOSE: case REDIR_PROC_IN: case REDIR_PROC_OUT:
      case REDIR_PROC_EQ:
        out.push_back(s::move(r));
      break; default: {
        str word = r.target;
        auto targets = ex.words({r.target});
        if (not targets) return fail(targets.error());
        if (targets->empty())
          return fail(ShellError{ErrorKind::ExecError,
              "no such file or directory: " + word});
        if (targets->size() > 1 and not mCtx.opts.get(OPT_MULTIOS))
          return fail(ShellError{ErrorKind::ExecError, "ambiguous redirect: " + word});
        for (auto& t : *targets) {
          Redirection copy = r;
          copy.target = s::move(t);
          out.push_back(s::move(copy));
        }
      }
    }
  }
  redirs = s::move(out);
  subst_status = ex.subst_status();
  return true;
}

// `name=value` on an integer or float parameter keeps its type: the value
// is an arithmetic expression, and `name+=value` adds it, as in zsh.
static Result<void> set_numeric(ExecContext& ctx, Param& p, const Assignment& a) {
  if (p.is_readonly())
    return s::unexpected(ShellError{ErrorKind::ExecError, "read-only variable: " + a.name});
  auto v = math_eval(a.value, ctx.params, math_options(ctx.opts), &ctx.modules.math_funcs());
  if (not v) return s::unexpected(v.error());
  MathNum n = *v;
  if (a.append) {
    MathNum old = p.is_float() ? MathNum::of(p.floatv()) : MathNum::of(p.integer());
    // Integer += wraps on overflow, as in zsh.
    using U = s::uint64_t;
    n = (old.is_float or n.is_float)
            ? MathNum::of(old.as_float() + n.as_float())
            : MathNum::of(static_cast<s::int64_t>(static_cast<U>(old.i) + static_cast<U>(n.i)));
  }
  // $SECONDS, $RANDOM, …: their setter decides what the value means.
  if (p.handlers) return ctx.params.set_scalar(a.name, s::to_string(n.as_int()));
  if (p.is_float()) p.set(n.as_float());
  else              p.set(n.as_int());
  return {};
}

// TODO(exec/assign): subscripted assignments `a[i]=x`, `h[key]=x`.
int Executor::apply_assigns(const vec<Assignment>& assigns) {
  for (const auto& a : assigns) {
    auto p = mCtx.params.lookup(a.name);
    bool numeric = not a.array and p and p->get().is_set()
               and (p->get().is_integer() or p->get().is_float());
    auto r = a.array ? mCtx.params.set_array(a.name, a.elements, a.append)
           : numeric ? set_numeric(mCtx, p->get(), a)
                     : mCtx.params.set_scalar(a.name, a.value, a.append);
    if (not r) {
      s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
      return 1;
    }
  }
  return 0;
}

// Quotes a word for xtrace output only when it needs it, as zsh does.
static str trace_quote(strv w) {
  constexpr strv kSafe =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
      "_@%+=:,./-";
  if (not w.empty() and w.find_first_not_of(kSafe) == strv::npos)
    return str(w);
  str out = "'";
  for (char c : w) {
    if (c == '\'') out += "'\\''";
    else           out += c;
  }
  return out + "'";
}

// PS4 goes through prompt expansion; zsh's default is `+%N:%i> `.
void Executor::trace(const vec<str>& args,
                     const vec<Assignment>&  assigns) {
  auto ps4 = mCtx.params.lookup("PS4");
  str line = expand_prompt((ps4 and ps4->get().is_set()) ? ps4->get().scalar() : "+%N:%i> ");
  // As zsh: each assignment is followed by a space, the words are
  // separated by one.
  for (const auto& a : assigns) {
    str value = trace_quote(a.value);
    if (a.array) {
      value = "(";
      for (const auto& e : a.elements) value += ' ' + trace_quote(e);
      value += " )";
    }
    line += a.name + (a.append ? "+=" : "=") + value + " ";
  }
  bool first = true;
  for (const auto& w : args) {
    if (not first) line += ' ';
    line += trace_quote(w);
    first = false;
  }
  s::fprintf(stderr, "%s\n", line.c_str());
}

vec<str> Executor::builtin_names() const {
  vec<str> names;
  Builtins.for_each([&](const BuiltinDef& d) { names.emplace_back(d.name); });
  s::sort(names.begin(), names.end());
  return names;
}

bool Executor::is_builtin(strv name) const noexcept {
  return Builtins.find(name).has_value();
}

int Executor::call_builtin(strv name,
                           vec<str>& args,
                           ExecContext& ctx) {
  if (auto def = Builtins.find(name)) return def->get().fn(args, ctx);
  return 127;
}

// zsh's default limit on nested function calls, used when $FUNCNEST is
// unset or not a number.
static constexpr long DEFAULT_FUNC_NEST = 700;

int Executor::call_function(const ProgRef& body,
                            vec<str>& args) {
  long limit = DEFAULT_FUNC_NEST;
  if (auto p = mCtx.params.lookup("FUNCNEST"); p and p->get().is_set()) {
    str text = p->get().is_integer() ? s::to_string(p->get().integer())
                                     : p->get().scalar();
    char* rest = nullptr;
    long n = s::strtol(text.c_str(), &rest, 10);
    if (not text.empty() and *rest == '\0') limit = n;
  }
  if (limit >= 0 and mCtx.func_depth >= limit) {
    s::fprintf(stderr,
               "jfsh: %s: maximum nested function level reached; increase FUNCNEST?\n",
               args[0].c_str());
    return 1;
  }

  auto saved_positional = s::exchange(
      mCtx.positional, vec<str>(args.begin() + 1, args.end()));
  str saved_arg0 = mCtx.arg0;
  if (mCtx.opts.get(OPT_FUNCTIONARGZERO)) mCtx.arg0 = args[0];
  OptionState saved_opts  = mCtx.opts;
  mCtx.signals.push_trap_frame();
  int saved_noerrreturn = s::exchange(mCtx.noerrreturn, 0);
  ++mCtx.func_depth;
  mCtx.params.push_scope();

  int rc = exec(*body);
  if (mCtx.returning) {
    mCtx.returning = false;
    rc = mCtx.return_val;
  }

  // LOCAL_TRAPS as it stands when the function returns decides, as does
  // LOCAL_OPTIONS.  An EXIT trap set in the function runs now.
  auto exit_trap = mCtx.signals.pop_trap_frame(mCtx.opts.get(OPT_LOCALTRAPS));
  if (exit_trap and exit_trap->action == TrapAction::Command and not mCtx.exiting) {
    int saved = mCtx.last_exit;
    exec_string(exit_trap->command);
    if (not mCtx.exiting) mCtx.last_exit = saved;
  }
  mCtx.params.pop_scope();
  --mCtx.func_depth;
  mCtx.noerrreturn = saved_noerrreturn;
  if (mCtx.opts.get(OPT_LOCALOPTIONS)) mCtx.opts = saved_opts;
  mCtx.arg0       = s::move(saved_arg0);
  mCtx.positional = s::move(saved_positional);
  return rc;
}

static bool is_executable_file(const str& path) {
  struct stat st{};
  return stat(path.c_str(), &st) == 0 and S_ISREG(st.st_mode)
     and access(path.c_str(), X_OK) == 0;
}

Result<str> Executor::find_command(strv name) {
  if (name.find('/') != strv::npos) return str(name);

  str path_var;
  if (auto p = mCtx.params.lookup("PATH"); p and p->get().is_set())
    path_var = p->get().scalar();
  if (path_var != mHashedPath) {
    mCmdHash.clear();
    mHashedPath = path_var;
  }
  bool hashing = mCtx.opts.get(OPT_HASHCMDS);
  str key(name);
  if (hashing) {
    auto it = mCmdHash.find(key);
    if (it != mCmdHash.end() and is_executable_file(it->second)) return it->second;
  }

  // An empty $PATH component means the current directory.  A matching file
  // that is not executable is remembered, so the error can say why.
  bool denied = false;
  s::size_t start = 0;
  while (not path_var.empty() and start <= path_var.size()) {
    s::size_t end = path_var.find(':', start);
    if (end == str::npos) end = path_var.size();
    str dir = path_var.substr(start, end - start);
    start = end + 1;
    str candidate = (dir.empty() ? "." : dir) + "/" + key;
    if (is_executable_file(candidate)) {
      if (hashing) mCmdHash[key] = candidate;
      return candidate;
    }
    struct stat st{};
    if (stat(candidate.c_str(), &st) == 0 and S_ISREG(st.st_mode)) denied = true;
  }
  if (denied)
    return s::unexpected(ShellError{ErrorKind::PermissionDenied,
        "permission denied: " + key});
  return s::unexpected(ShellError{ErrorKind::FileNotFound,
      "command not found: " + key});
}

static vec<char*> c_strings(vec<str>& v) {
  vec<char*> out;
  out.reserve(v.size() + 1);
  for (auto& s : v) out.push_back(s.data());
  out.push_back(nullptr);
  return out;
}

int Executor::run_external(const vec<str>& args,
                           const vec<Assignment>&  assigns,
                           const vec<Redirection>& redirs,
                           ExecFlags flags) {
  auto path = find_command(args[0]);
  if (not path) {
    s::fprintf(stderr, "jfsh: %s\n", path.error().message.c_str());
    return path.error().kind == ErrorKind::PermissionDenied ? 126 : 127;
  }

  // The child's environment is the shell's exported parameters plus the
  // command's prefix assignments, which are exported to it alone.
  vec<str> env = mCtx.params.make_envp();
  for (const auto& a : assigns) {
    auto value = prefix_value(mCtx.params, a);
    if (not value) {
      s::fprintf(stderr, "jfsh: %s\n", value.error().message.c_str());
      return 1;
    }
    str prefix = a.name + "=";
    s::erase_if(env, [&](const str& e) { return e.starts_with(prefix); });
    env.push_back(prefix + *value);
  }
  vec<str> argv_s = args;
  auto argv = c_strings(argv_s);
  auto envp = c_strings(env);

  // Builtins write through stdio; flush so their output precedes the child's.
  s::fflush(nullptr);

  // EF_NOFORK: last command in `-c` string or last stage in a subshell runs
  // on this process directly.  Redirections are applied in-process; no save
  // because we will never return.
  if (flags & EF_NOFORK) {
    if (not redirs.empty() and not apply_redirects_child(redirs)) _exit(1);
    execve(path->c_str(), argv.data(), envp.data());
    int err = errno;
    const char* name = args[0].c_str();
    if (err == ENOENT) {
      s::fprintf(stderr, "jfsh: no such file or directory: %s\n", name);
      _exit(127);
    }
    s::fprintf(stderr, "jfsh: %s: %s\n", name, s::strerror(err));
    _exit(126);
  }

  bool jc = job_control();
  pid_t pid = fork_child(0, true, ChildKind::Other);
  if (pid < 0) return 1;
  if (pid == 0) {
    if (not redirs.empty() and not apply_redirects_child(redirs)) _exit(1);
    execve(path->c_str(), argv.data(), envp.data());
    int err = errno;
    if (err == ENOEXEC) {
      // Neither a binary nor a #! script: run it with /bin/sh, as zsh does.
      vec<str> sh_s{"/bin/sh", *path};
      sh_s.insert(sh_s.end(), args.begin() + 1, args.end());
      auto sh_argv = c_strings(sh_s);
      execve("/bin/sh", sh_argv.data(), envp.data());
      err = errno;
    }
    const char* name = args[0].c_str();
    if (err == ENOENT) {
      s::fprintf(stderr, "jfsh: no such file or directory: %s\n", name);
      _exit(127);
    }
    if (err == EACCES or err == EISDIR or err == EPERM)
      s::fprintf(stderr, "jfsh: permission denied: %s\n", name);
    else
      s::fprintf(stderr, "jfsh: %s: %s\n", name, s::strerror(err));
    _exit(126);
  }

  if (jc) {
    int jid = mCtx.jobs.alloc(pid, JOB_PGRP, current_job_text(), false);
    Process p;
    p.pid = pid;
    mCtx.jobs.add_process(jid, s::move(p));
    return wait_foreground_job(mCtx, jid);
  }
  int status = 0;
  while (waitpid(pid, &status, 0) < 0) {
    if (errno != EINTR) {
      s::fprintf(stderr, "jfsh: waitpid: %s\n", s::strerror(errno));
      return 1;
    }
  }
  if (WIFEXITED(status))   return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 1;
}

// ---- exec, source, autoload, the command hash --------------------------------

int Executor::exec_replace(const vec<str>& args, const vec<Assignment>& assigns,
                           const str& argv0, bool clear_env) {
  auto fail = [&](int rc) {
    // A shell that can't exec goes on only when interactive.
    if (not mCtx.opts.interactive()) {
      mCtx.exiting  = true;
      mCtx.exit_val = rc;
    }
    return rc;
  };
  auto path = find_command(args[0]);
  if (not path) {
    s::fprintf(stderr, "jfsh: %s\n", path.error().message.c_str());
    return fail(path.error().kind == ErrorKind::PermissionDenied ? 126 : 127);
  }
  vec<str> env = clear_env ? vec<str>{} : mCtx.params.make_envp();
  for (const auto& a : assigns) {
    auto value = prefix_value(mCtx.params, a);
    if (not value) {
      s::fprintf(stderr, "jfsh: %s\n", value.error().message.c_str());
      return fail(1);
    }
    str prefix = a.name + "=";
    s::erase_if(env, [&](const str& e) { return e.starts_with(prefix); });
    env.push_back(prefix + *value);
  }
  vec<str> argv_s = args;
  if (argv0 == "-")        argv_s[0] = "-" + argv_s[0];
  else if (not argv0.empty()) argv_s[0] = argv0;
  auto argv = c_strings(argv_s);
  auto envp = c_strings(env);
  s::fflush(nullptr);
  // The new program starts with the shell's caught signals at their
  // defaults (ignored ones stay ignored); put back if exec fails.
  vec<s::pair<int, struct sigaction>> changed;
  for (int sig = 1; sig < NSIG; ++sig) {
    struct sigaction old{};
    if (::sigaction(sig, nullptr, &old) != 0) continue;
    if (old.sa_handler == SIG_IGN or old.sa_handler == SIG_DFL) continue;
    struct sigaction dfl{};
    dfl.sa_handler = SIG_DFL;
    sigemptyset(&dfl.sa_mask);
    if (::sigaction(sig, &dfl, nullptr) == 0) changed.emplace_back(sig, old);
  }
  execve(path->c_str(), argv.data(), envp.data());
  int err = errno;
  for (auto& [sig, old] : changed) ::sigaction(sig, &old, nullptr);
  s::fprintf(stderr, "jfsh: %s: %s\n", args[0].c_str(), s::strerror(err));
  return fail(err == ENOENT ? 127 : 126);
}

static opt<str> read_file(const str& path) {
  s::FILE* f = s::fopen(path.c_str(), "r");
  if (not f) return s::nullopt;
  str text;
  char buf[4096];
  s::size_t n;
  while ((n = s::fread(buf, 1, sizeof buf, f)) > 0) text.append(buf, n);
  s::fclose(f);
  return text;
}

int Executor::source_file(const str& path, const vec<str>& args, const str& name) {
  auto text = read_file(path);
  if (not text) {
    s::fprintf(stderr, "jfsh: %s: %s\n", s::strerror(errno), name.c_str());
    return 1;
  }
  vec<str> saved_positional;
  bool     set_args = not args.empty();
  if (set_args) saved_positional = s::exchange(mCtx.positional, args);
  str saved_arg0   = mCtx.arg0;
  str saved_source = s::exchange(mCtx.source_name, path);
  if (mCtx.opts.get(OPT_FUNCTIONARGZERO)) mCtx.arg0 = name;
  ++mCtx.source_depth;
  int rc = exec_string(*text, /*by_line=*/true);
  --mCtx.source_depth;
  // `return` in the file ends the file.
  if (mCtx.returning) {
    mCtx.returning = false;
    rc = mCtx.return_val;
  }
  mCtx.source_name = s::move(saved_source);
  mCtx.arg0        = s::move(saved_arg0);
  if (set_args) mCtx.positional = s::move(saved_positional);
  return rc;
}

bool Executor::load_autoload(const str& name) {
  vec<str> dirs;
  if (auto p = mCtx.params.lookup("fpath"); p and p->get().is_set()) {
    dirs = p->get().array();
  } else if (auto q = mCtx.params.lookup("FPATH"); q and q->get().is_set()) {
    str all = q->get().scalar();
    s::size_t start = 0;
    while (start <= all.size()) {
      s::size_t end = all.find(':', start);
      if (end == str::npos) end = all.size();
      dirs.push_back(all.substr(start, end - start));
      start = end + 1;
    }
  }
  vec<str> candidates;
  // `autoload /dir/name` named its file.
  if (auto f = mCtx.function_files.find(name); f != mCtx.function_files.end())
    candidates.push_back(f->second);
  for (const auto& dir : dirs) candidates.push_back((dir.empty() ? str(".") : dir) + "/" + name);
  for (const auto& path : candidates) {
    struct stat st{};
    if (stat(path.c_str(), &st) != 0 or not S_ISREG(st.st_mode)) continue;
    auto text = read_file(path);
    if (not text) continue;
    StringInputSource in{*text};
    Lexer lex{in};
    lex.push_state(LEX_SKIPCOMMENTS);
    Parser parser{lex};
    auto prog = parser.parse();
    if (not prog) {
      s::fprintf(stderr, "jfsh: %s: %s\n", name.c_str(), prog.error().message.c_str());
      return false;
    }
    mCtx.autoloads.erase(name);
    // A file that is just `name() { … }` (ksh style) defines the function
    // when run; any other file is the function's body.
    const auto& code = (*prog)->prog;
    bool ksh_style = mCtx.opts.get(OPT_KSH_AUTOLOAD);
    if (not ksh_style and code.size() > 4 and wc_code(code[3]) == WC_FUNCDEF
        and wc_cmd_type(code[3]) == WC_FUNCDEF_NAMED) {
      s::size_t after = 4 + wc_cmd_skip(code[3]);
      bool only = after + 1 == code.size() and wc_code(code[after]) == WC_END
              and wc_data(code[4]) == 1
              and (*prog)->strs[wc_data(code[5])] == name;
      ksh_style = only;
    }
    str saved_source = s::exchange(mCtx.source_name, path);
    if (ksh_style) {
      exec(**prog);
    } else {
      mCtx.functions[name]      = *prog;
      mCtx.function_files[name] = path;
    }
    mCtx.source_name = s::move(saved_source);
    if (mCtx.functions.contains(name)) return true;
    s::fprintf(stderr, "jfsh: %s: function not defined by file\n", name.c_str());
    return false;
  }
  s::fprintf(stderr, "jfsh: %s: function definition file not found\n", name.c_str());
  return false;
}

HashTable<str>& Executor::command_hash() {
  str path_var;
  if (auto p = mCtx.params.lookup("PATH"); p and p->get().is_set())
    path_var = p->get().scalar();
  if (path_var != mHashedPath) {
    mCmdHash.clear();
    mHashedPath = path_var;
  }
  return mCmdHash;
}

void Executor::fill_command_hash() {
  auto& hash = command_hash();
  hash.clear();
  // The first directory in $PATH with a name wins, as for a search.
  str path_var = mHashedPath;
  s::size_t start = 0;
  while (not path_var.empty() and start <= path_var.size()) {
    s::size_t end = path_var.find(':', start);
    if (end == str::npos) end = path_var.size();
    str dir = path_var.substr(start, end - start);
    start = end + 1;
    if (dir.empty() or dir[0] != '/') continue;   // relative ones aren't hashed
    DIR* d = ::opendir(dir.c_str());
    if (not d) continue;
    while (auto* e = ::readdir(d)) {
      str entry = e->d_name;
      if (entry == "." or entry == ".." or hash.contains(entry)) continue;
      str full = dir + "/" + entry;
      if (mCtx.opts.get(OPT_HASHEXECUTABLESONLY) and not is_executable_file(full)) continue;
      hash.emplace(s::move(entry), s::move(full));
    }
    ::closedir(d);
  }
}

// ---- Job control and signals -----------------------------------------------

pid_t Executor::fork_child(Pid pgid, bool foreground, ChildKind kind) {
  bool jc = job_control();
  s::fflush(nullptr);
  // Signals stay blocked until the child has its own dispositions: one sent
  // to it right away (`cmd & kill %1`) must not meet the shell's handlers
  // or ignored signals, and is delivered once the child unblocks.
  sigset_t all, old;
  sigfillset(&all);
  ::sigprocmask(SIG_BLOCK, &all, &old);
  pid_t pid = ::fork();
  if (pid < 0) {
    int err = errno;
    ::sigprocmask(SIG_SETMASK, &old, nullptr);
    s::fprintf(stderr, "jfsh: fork failed: %s\n", s::strerror(err));
    return -1;
  }
  if (pid == 0) {
    if (jc) {
      // Both sides set the group, so neither can run ahead of the other.
      ::setpgid(0, pgid);
      if (foreground) mCtx.signals.give_terminal(pgid ? pgid : ::getpid());
    }
    mCtx.signals.child_reset(kind);
    mCtx.jobs.clear();
    ::sigprocmask(SIG_SETMASK, &old, nullptr);
    return 0;
  }
  if (jc) ::setpgid(pid, pgid ? pgid : pid);
  ::sigprocmask(SIG_SETMASK, &old, nullptr);
  return pid;
}

str Executor::current_job_text() const {
  if (not mPipeProg or mPipePc >= mPipeProg->prog.size()) return {};
  return pipeline_text(*mPipeProg, mPipePc);
}

PromptHooks Executor::prompt_hooks() {
  PromptHooks h;
  h.subst = [this](strv text) -> str {
    Expander x(*this, mCtx);
    auto r = x.heredoc(text);
    return r ? *r : str(text);
  };
  h.job_count   = [this] {
    int n = 0;
    mCtx.jobs.for_each([&](const Job&) { ++n; });
    return n;
  };
  h.script_name = [this] { return mCtx.source_name.empty() ? mCtx.arg0 : mCtx.source_name; };
  h.eval_depth  = [this] { return s::max(0, mStringDepth - 1); };
  return h;
}

str Executor::expand_prompt(strv prompt) {
  PromptRenderer r(mCtx.params, mCtx.opts);
  r.set_hooks(prompt_hooks());
  return r.render(prompt);
}

void Executor::update_window_size() {
  struct winsize ws{};
  int fd = mCtx.signals.tty_fd() >= 0 ? mCtx.signals.tty_fd() : STDOUT_FILENO;
  if (::ioctl(fd, TIOCGWINSZ, &ws) != 0 or ws.ws_col == 0) return;
  (void)mCtx.params.set_scalar("COLUMNS", s::to_string(ws.ws_col));
  (void)mCtx.params.set_scalar("LINES", s::to_string(ws.ws_row));
}

void Executor::handle_signals() {
  auto bits = mCtx.signals.take_pending();
  auto has  = [&](int sig) {
    return sig < 64 ? (bits[0] >> sig) & 1 : (bits[1] >> (sig - 64)) & 1;
  };
  if (has(SIGCHLD)) mCtx.signals.reap(mCtx.jobs);
  if (has(SIGWINCH)) update_window_size();
  for (int sig = 1; sig <= MAX_SIG; ++sig) {
    if (not has(sig)) continue;
    if (mCtx.signals.traps().trapped(sig)) {
      run_trap(sig);
    } else if (sig == SIGINT and mCtx.signals.job_control()) {
      mCtx.aborting = true;
    } else if (sig == SIGHUP and mCtx.signals.job_control()) {
      mCtx.hangup   = true;
      mCtx.exiting  = true;
      mCtx.exit_val = 128 + SIGHUP;
    }
  }
}

void Executor::run_trap(int sig) {
  if (sig < 0 or sig >= TRAP_TABLE_SIZE) return;
  Trap trap = mCtx.signals.traps().get(sig);
  if (trap.action != TrapAction::Command and trap.action != TrapAction::Function)
    return;
  if (mInTrap[static_cast<s::size_t>(sig)]) {
    // Already running this trap: handle the signal once it is done.
    if (is_real_signal(sig)) mCtx.signals.note(sig);
    return;
  }
  mInTrap[static_cast<s::size_t>(sig)] = true;
  ++mTrapDepth;
  int  saved_status = mCtx.last_exit;
  bool saved_err    = mCtx.err_reported;
  if (trap.action == TrapAction::Function) {
    str name = str("TRAP") + SignalTable::sig_to_name(sig);
    auto fn = mCtx.functions.find(name);
    if (fn != mCtx.functions.end()) {
      ProgRef body = fn->second;
      vec<str> args{name};
      if (is_real_signal(sig))  args.push_back(s::to_string(sig));
      else if (sig == SIGEXIT)  args.push_back("0");
      int rc = call_function(body, args);
      // A signal trap function that fails acts as if the signal had
      // interrupted the shell.
      if (rc != 0 and is_real_signal(sig) and not mCtx.exiting) mCtx.aborting = true;
    }
  } else {
    exec_string(trap.command);
  }
  if (not mCtx.exiting) mCtx.last_exit = saved_status;
  mCtx.err_reported = saved_err;
  --mTrapDepth;
  mInTrap[static_cast<s::size_t>(sig)] = false;
}

int Executor::run_exit_trap(int status) {
  if (mExitTrapRan or not mCtx.signals.traps().trapped(SIGEXIT)) return status;
  mExitTrapRan = true;
  Trap trap = mCtx.signals.traps().get(SIGEXIT);
  mCtx.signals.set_trap(SIGEXIT, TrapAction::Default);
  bool was_exiting = mCtx.exiting;
  int  was_val     = mCtx.exit_val;
  mCtx.exiting = mCtx.returning = mCtx.aborting = false;
  mCtx.break_level = 0;
  mCtx.last_exit   = status;
  ++mTrapDepth;
  if (trap.action == TrapAction::Command) {
    exec_string(trap.command);
  } else if (auto fn = mCtx.functions.find("TRAPEXIT"); fn != mCtx.functions.end()) {
    ProgRef body = fn->second;
    vec<str> args{"TRAPEXIT", "0"};
    call_function(body, args);
  }
  --mTrapDepth;
  if (mCtx.exiting) return mCtx.exit_val;   // the trap called `exit`
  mCtx.exiting  = was_exiting;
  mCtx.exit_val = was_val;
  return status;
}

}  // namespace jfsh
