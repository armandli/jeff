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
#if not defined(__APPLE__) and not defined(__FreeBSD__)
#include <stdio_ext.h>
#endif

#include <algorithm>
#include <tuple>
#include <unordered_map>
#include <utility>

#include <jfsh/builtins/builtins.h>
#include <jfsh/builtins/cond.h>
#include <jfsh/builtins/math.h>
#include <jfsh/core/aliases.h>
#include <jfsh/core/read_file.h>
#include <jfsh/core/stack.h>
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

// Comments are always recognized in scripts and -c strings, but
// interactively only with INTERACTIVE_COMMENTS.
static void set_comments(Lexer& lex, const OptionState& opts) {
  if (not opts.interactive() or opts.get(OPT_INTERACTIVECOMMENTS))
    lex.push_state(LEX_SKIPCOMMENTS);
}

Result<ProgRef> Executor::parse_string(strv src, int first_line) {
  StringInputSource in{str(src), first_line};
  Lexer lex{in};
  set_comments(lex, mCtx.opts);
  Parser parser{lex};
  if (mCtx.opts.get(OPT_ALIASES)) parser.set_aliases(&mCtx.aliases);
  return parser.parse();
}

void Executor::report_parse_error(const ShellError& e) {
  s::fprintf(stderr, "jfsh:%d: %s\n", e.code, e.message.c_str());
}

int Executor::exec_string(strv src, bool by_line, int first_line) {
  auto parse_failed = [&](const ShellError& e) {
    report_parse_error(e);
    mParseFailed = true;
    mStatusBeforeParseError = mCtx.last_exit;
    mCtx.last_exit = 1;
    return 1;
  };
  if (not by_line) {
    auto prog = parse_string(src, first_line);
    if (not prog) return parse_failed(prog.error());
    return exec_parsed(**prog);
  }
  StringDepth depth{*this};
  StringInputSource in{str(src), first_line};
  // VERBOSE: each line is shown as it is read (zsh's inputline).
  in.on_line([this](strv line) {
    if (not mCtx.opts.get(OPT_VERBOSE)) return;
    s::fwrite(line.data(), 1, line.size(), stderr);
    s::fflush(stderr);
  });
  Lexer lex{in};
  set_comments(lex, mCtx.opts);
  Parser parser{lex};
  int rc = 0;
  for (;;) {
    // A line may have turned ALIASES on or off for the next.
    parser.set_aliases(mCtx.opts.get(OPT_ALIASES) ? &mCtx.aliases : nullptr);
    auto prog = parser.parse_line();
    if (not prog) return parse_failed(prog.error());
    bool ran = (*prog)->prog.size() > 1;
    if (ran) rc = exec(**prog);
    // Between commands $LINENO is where reading has got to, as zsh's
    // lexer counts it (an EXIT trap at the end sees the line after).
    if (not (mCtx.exiting and mCtx.exit_now)) mCtx.lineno = static_cast<s::uint32_t>(in.line());
    if (mCtx.returning and mCtx.return_line and mCtx.func_depth == 0 and mStringDepth == 1) {
      mCtx.returning = mCtx.return_line = false;
      rc = mCtx.last_exit = mCtx.return_val;
    }
    // -t (SINGLE_COMMAND): the script or standard input ends after its
    // first command, as zsh's top-level loop does.
    if (ran and mStringDepth == 1 and mCtx.opts.get(OPT_SINGLECOMMAND) and not mCtx.exiting) {
      mCtx.exiting  = true;
      mCtx.exit_val = rc;
      mCtx.loop.single_done = true;
    }
    if (parser.at_eof() or interrupted()) break;
  }
  // A script that an error or a trap ended with status 0 (`kill` raising
  // a TRAPUSR1 that returned 1, `print -v 1x`) ends with 1; a -c string
  // keeps 0, as in zsh (its main loop's "fatal error exits non-zero").
  if (mCtx.aborting and mCtx.last_exit == 0 and mStringDepth == 1
      and not mCtx.opts.interactive())
    rc = mCtx.last_exit = 1;
  return end_string(rc);
}

int Executor::exec_parsed(const CompiledProgram& prog) {
  StringDepth depth{*this};
  int rc = exec(prog);
  // A -c string that an error or an interrupt ended (not `${x?}`) leaves
  // zsh's errflag set, and dotrapargs then runs no EXIT trap.  (A script's
  // loop clears it first.)
  if (mStringDepth == 1 and mCtx.aborting and not mCtx.hard_error
      and not mCtx.opts.interactive())
    mExitTrapRan = true;
  return end_string(rc);
}

int Executor::end_string(int rc) {
  // `return` outside a function ends the script or -c string, as in zsh;
  // one in an `eval` there goes on to end it (or the sourced file).
  bool in_eval = mStringDepth > 1 and not mCtx.funcstack.empty()
                 and mCtx.funcstack.back().name == "(eval)";
  if (mCtx.returning and mCtx.func_depth == 0 and not in_eval) {
    if (mStringDepth == 1) mTopReturn = true;
    mCtx.returning = mCtx.return_line = false;
    rc = mCtx.last_exit = mCtx.return_val;
  }
  // An aborted command line ends here: a non-interactive shell exits.  (An
  // interactive one goes back to the prompt; the REPL clears the flag.)
  if (mCtx.aborting and mStringDepth == 1 and mCtx.func_depth == 0
      and not mCtx.opts.interactive()) {
    mCtx.aborting = false;
    mCtx.end_trap_abort();
    if (not mCtx.exiting) mCtx.exit_val = mCtx.last_exit;
    mCtx.exiting  = true;
  }
  return rc;
}

// Around a trap: an error in it ends the trap, not the code it interrupted,
// as in zsh; an interrupt (`interrupt`) goes on unwinding.
struct TrapErrorScope {
  explicit TrapErrorScope(ExecContext& c) : ctx(c), was_aborting(c.aborting) {
    if (not was_aborting) ctx.interrupt = false;
  }
  ~TrapErrorScope() {
    if (not was_aborting and ctx.aborting and not ctx.interrupt) {
      ctx.aborting = ctx.abort_line = false;
      ctx.end_trap_abort();
    }
  }
  TrapErrorScope(const TrapErrorScope&) = delete;
  TrapErrorScope& operator=(const TrapErrorScope&) = delete;

  ExecContext& ctx;
  bool         was_aborting;
};

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
  if (wc_redir_varid(w)) {
    auto [name, target] = split_redir_varid(r.target);
    r.varid  = str(name);
    r.target = str(target);
  } else if (r.type == REDIR_HEREDOC or r.type == REDIR_HEREDOCD) {
    r.target = str(split_heredoc(r.target).second);
  }
  return r;
}

// Expands an assignment's value in place: a scalar without splitting, an
// array like command arguments.
static Result<void> expand_assignment(Expander& ex, Assignment& a) {
  if (auto lb = a.name.find('['); lb != str::npos and a.name.back() == ']') {
    strv raw = strv(a.name).substr(lb + 1, a.name.size() - lb - 2);
    int depth = 0;
    for (char c : raw) {
      if (c == '(' or c == '[' or c == '{') ++depth;
      else if (c == ')' or c == ']' or c == '}') --depth;
      else if (c == ',' and depth == 0) { a.subscript_comma = true; break; }
    }
    a.subscript_whole = raw == "@" or raw == "*";
    auto sub = ex.scalar(raw);
    if (not sub) return s::unexpected(sub.error());
    a.subscript = s::move(*sub);
    a.name.resize(lb);
  }
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

static str trace_quote(strv w);
static str errno_text(int err);
static str trace_assigns(const vec<Assignment>& assigns);
static int trace_fd(const SavedFds& saved);
static void write_trace(str line, int fd);
static bool posix_special(const BuiltinDef& def);
static bool is_executable_file(const str& path);
static Result<str> search_path(strv name, const vec<str>& dirs, HashTable<str>* hash);

// The fds above 9 that the shell opened for itself (pipes, saved fds),
// with the file each was open on, so that a number reused for something
// else no longer counts: `>&N` refuses them, as zsh refuses its own.
static umap<int, s::pair<dev_t, ino_t>> ShellFds;

static void note_shell_fd(int fd) {
  struct stat st{};
  if (fd >= 10 and ::fstat(fd, &st) == 0) ShellFds[fd] = {st.st_dev, st.st_ino};
}

static void forget_shell_fd(int fd) { ShellFds.erase(fd); }

static bool is_shell_fd(int fd) {
  auto it = ShellFds.find(fd);
  struct stat st{};
  return it != ShellFds.end() and ::fstat(fd, &st) == 0
     and it->second == s::pair{st.st_dev, st.st_ino};
}

// Moves `fd` to a number ≥ 10, close-on-exec, out of the way of the fds
// redirections and pipeline stages name.  Returns the new fd, or -1 (with
// errno, and `fd` closed).
static int move_high(int fd) {
  if (fd < 0) return fd;
  if (fd >= 10) {
    ::fcntl(fd, F_SETFD, FD_CLOEXEC);
    note_shell_fd(fd);
    return fd;
  }
  int high = ::fcntl(fd, F_DUPFD_CLOEXEC, 10);
  int err  = errno;
  ::close(fd);
  note_shell_fd(high);
  errno = err;
  return high;
}

// pipe(), with both ends moved by move_high: when the shell's own fd 0 or
// 1 is closed, pipe() hands out that number, and a stage's dup2 onto it
// would then lose the other end.
static bool pipe_high(int p[2]) {
  int raw[2];
  if (::pipe(raw) != 0) return false;
  p[0] = move_high(raw[0]);
  p[1] = move_high(raw[1]);
  if (p[0] >= 0 and p[1] >= 0) return true;
  if (p[0] >= 0) ::close(p[0]);
  if (p[1] >= 0) ::close(p[1]);
  return false;
}

// zsh's POSIX-special builtins (BINF_PSPECIAL): the special ones and the
// typeset variants, whose prefix assignments stay under POSIX_BUILTINS.
static bool posix_special(const BuiltinDef& def) {
  return def.flags & (BINF_SPECIAL | BINF_TYPESET);
}

int Executor::exec_simple(vec<str> args,
                          vec<Assignment>  assigns,
                          vec<Redirection> redirs,
                          ExecFlags flags,
                          vec<Assignment>  typeset_args,
                          bool simple) {
  opt<int> subst_status;
  // `noglob cmd`: the words are not globbed.
  bool noglob = false;
  while (not args.empty() and args[0] == "noglob") {
    args.erase(args.begin());
    noglob = true;
    for (auto& a : typeset_args) a.position -= a.position > 0 ? 1 : 0;
  }
  bool globbing = mCtx.opts.get(OPT_GLOB);
  if (noglob) mCtx.opts.unset(OPT_GLOB);
  // The process substitutions' fds and files last as long as the command:
  // as the last stage of a pipeline, a forked one's `=(…)` files wait
  // until exec_pline has waited for it.
  ProcSubstResources psubst;
  struct PSubstGuard {
    ~PSubstGuard() {
      exec->mPsubst = outer;
      PipeLast* last = exec->mPipeLast;
      if (pipe_last and last and last->pid > 0) {
        last->tmp_files.insert(last->tmp_files.end(), res->tmp_files.begin(),
                               res->tmp_files.end());
        res->tmp_files.clear();
      }
      exec->procsubst_release(*res);
    }

    Executor* exec;
    ProcSubstResources* res;
    ProcSubstResources* outer;
    bool pipe_last;
  } psubst_guard{this, &psubst, s::exchange(mPsubst, &psubst), (flags & EF_PIPELAST) != 0};
  // typeset's assignments go back between the words they were among.
  vec<s::size_t> counts;
  bool fatal = true;
  auto substs_before = mProcSubsts;
  bool had_words = not args.empty();
  bool expanded = expand_command(args, redirs, subst_status, fatal,
                                 typeset_args.empty() ? nullptr : &counts);
  for (auto& a : typeset_args) {
    s::size_t n = 0;
    for (s::size_t k = 0; k < a.position and k < counts.size(); ++k) n += counts[k];
    a.position = n;
  }
  if (noglob) mCtx.opts.assign(OPT_GLOB, globbing);
  // A failed expansion (`${x?msg}`, NO_UNSET, `$((1/0))`) ends the command
  // line, as zsh's errflag does: a non-interactive shell exits.
  if (not expanded) {
    if (fatal) mCtx.aborting = mCtx.abort_line = true;
    return 1;
  }
  // A trap's `return` during the expansion (`$(kill -USR1 $$)`): at the
  // top the command doesn't run; in a function it runs, and the function
  // returns its status, as zsh's retflag.
  if (mCtx.returning and s::exchange(mTrapReturned, false)) {
    if (mCtx.func_depth == 0) return mCtx.last_exit;
    mReturnAfterCommand = true;
  }
  // A trap that ended the line during the expansion (a TRAPxxx that failed
  // in `$( … )`): the command doesn't run, as zsh's errflag.
  if (mCtx.aborting) {
    if (mCtx.trap_status >= 0) mCtx.last_exit = 1;
    return mCtx.last_exit;
  }
  // The prefix scope of a builtin or function, popped when the command is
  // done.
  struct ScopeGuard {
    ParamTable* params = nullptr;
    ~ScopeGuard() { if (params) params->pop_scope(); }
  } prefix_scope;
  // typeset's `name=value` arguments: a scalar is passed as `name=value`; an
  // array is passed as `name`, and given its elements once the builtin has
  // declared it.  As in zsh they are expanded once the command's
  // redirections are applied and the assignments in front of it are made
  // (`x=1 typeset y=$x`): see finish_typeset below.
  vec<Assignment> typeset_arrays;
  str typeset_traced;   // xtrace shows them as assignments
  int typeset_failed = 1;   // the status when they fail
  auto expand_typeset_args = [&] {
    Expander ex{*this, mCtx};
    s::size_t placed = 0;
    for (auto& a : typeset_args) {
      if (auto r = expand_assignment(ex, a); not r) {
        s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
        // As any expansion error, this ends the command line.
        mCtx.aborting = true;
        return false;
      }
      // A TRAPxxx that failed in a `$( … )`: typeset doesn't run, with
      // status 0, as zsh's.
      if (mCtx.aborting and mCtx.trap_status >= 0) {
        typeset_failed = mCtx.last_exit = 0;
        return false;
      }
      typeset_traced += ' ' + trace_assigns({a});
      typeset_traced.pop_back();
      // Earlier ones were put in before it.
      auto at = args.begin()
              + static_cast<s::ptrdiff_t>(s::min(a.position + placed++, args.size()));
      if (a.array) {
        // `typeset a+=(…)` is refused by typeset (`a+` is no name), as in zsh.
        args.insert(at, a.append ? a.name + "+" : a.name);
        typeset_arrays.push_back(s::move(a));
      } else {
        // An element keeps its (expanded) subscript: typeset decides.
        str name = a.subscript ? a.name + "[" + *a.subscript + "]" : a.name;
        args.insert(at, name + (a.append ? "+=" : "=") + a.value);
      }
    }
    return true;
  };

  // Precommand modifiers: `builtin` and `command` pick what the name may
  // be; `exec` replaces the shell (or, alone, makes the redirections
  // permanent); `-` changes nothing here (`nocorrect` is the parser's).
  enum class Only { Any, Builtin, External } only = Only::Any;
  bool prefixed = noglob;   // after a precommand modifier other than exec
  bool bare_prefix = false; // `builtin` or `command` with nothing after it
  bool replace = false;
  bool clear_env = false;
  bool command_p = false;
  bool via_command = false;   // run through `command`: no longer special
  str  argv0;
  while (not args.empty()) {
    const str& w = args[0];
    if (w == "builtin" and only == Only::Any) {
      args.erase(args.begin());
      prefixed = true;
      bare_prefix = args.empty();
      if (bare_prefix) break;
      only = Only::Builtin;
      break;
    }
    if (w == "command" and only == Only::Any) {
      via_command = true;
      prefixed = true;
      s::size_t i = 1;
      bool lookup = false;
      bool default_path = false;
      for (; i < args.size() and args[i].size() > 1 and args[i][0] == '-'; ++i) {
        if (args[i] == "--") { ++i; break; }
        // A word with another letter is the command, as in zsh.
        if (args[i].find_first_not_of("pvV", 1) != str::npos) break;
        for (char f : strv(args[i]).substr(1)) {
          if (f == 'p') default_path = true;
          else          lookup = true;
        }
      }
      // `command -v name`: describe it, as whence does.
      if (lookup) break;
      args.erase(args.begin(), args.begin() + static_cast<s::ptrdiff_t>(i));
      bare_prefix = args.empty();
      if (bare_prefix) break;
      command_p = default_path;
      only = mCtx.opts.get(OPT_POSIXBUILTINS) ? Only::Any : Only::External;
      continue;
    }
    // After `command` (but under POSIX_BUILTINS), `exec` is a command
    // name, as in zsh.
    if (w == "exec" and not replace and only != Only::External) {
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
    // `-` not after `command` either (but under POSIX_BUILTINS).
    // (`nocorrect` is a reserved word the parser takes.)
    if (w == "-" and only != Only::External) {
      argv0 = "-";
      args.erase(args.begin());
      prefixed = true;
      continue;
    }
    break;
  }
  // A command of nothing but redirections runs $NULLCMD (cat) with them,
  // or $READNULLCMD (more) for a lone `<file`; under SH_NULLCMD it is `:`.
  // With CSH_NULLCMD, an empty $NULLCMD or after a precommand modifier it
  // is an error, which ends the command line, as in zsh.
  // $_ is the command's last word from now on, after the precommand
  // modifiers (zsh's setunderscore); words that expand to nothing leave
  // it, and `exec` with only redirections empties it.
  if (not args.empty())                          mCtx.underscore = args.back();
  else if (replace and not redirs.empty())        mCtx.underscore.clear();
  if (args.empty() and not redirs.empty() and assigns.empty() and not replace) {
    auto param = [&](strv name) {
      auto p = mCtx.params.lookup(name);
      return p and p->get().is_set() ? p->get().scalar() : str{};
    };
    str nullcmd = param("NULLCMD");
    str readnullcmd = param("READNULLCMD");
    if (prefixed or mCtx.opts.get(OPT_CSHNULLCMD) or nullcmd.empty()) {
      s::fprintf(stderr, "jfsh: redirection with no command\n");
      mCtx.aborting = true;
      return 1;
    }
    if (mCtx.opts.get(OPT_SHNULLCMD))
      args.emplace_back(":");
    else if (not readnullcmd.empty() and redirs.size() == 1 and redirs[0].type == REDIR_READ)
      args.push_back(s::move(readnullcmd));
    else
      args.push_back(s::move(nullcmd));
    mCtx.underscore = args.back();
  }
  if (bare_prefix) return 0;

  // xtrace shows the words without the precommand modifiers, and comes
  // before the command runs.  With assignments in front, it waits until
  // they are made, so it shows their values.
  bool tracing = mCtx.opts.xtrace();
  str  traced;
  if (tracing) {
    for (const auto& w : args) {
      if (not traced.empty()) traced += ' ';
      traced += trace_quote(w);
    }
    if (assigns.empty() and typeset_args.empty() and not args.empty()) trace_text(traced);
  }
  const str* child_trace = tracing and not assigns.empty() ? &traced : nullptr;
  // Expands typeset's arguments and adds them to the trace, which (without
  // assignments in front) is printed here, to `fd`.
  auto finish_typeset = [&](int fd) {
    if (typeset_args.empty()) return true;
    if (not expand_typeset_args()) return false;
    traced += typeset_traced;
    if (tracing and assigns.empty()) trace_text(traced, fd);
    return true;
  };
  // `command -p`: the name is looked up in the system's default $PATH.
  if (command_p and not args.empty() and args[0].find('/') == str::npos) {
    vec<str> dirs{"/usr/bin", "/bin", "/usr/sbin", "/sbin"};
    auto path = search_path(args[0], dirs, nullptr);
    if (not path) {
      s::fprintf(stderr, "jfsh: %s\n", path.error().message.c_str());
      return path.error().kind == ErrorKind::PermissionDenied ? 126 : 127;
    }
    args[0] = *path;
  }

  // With no command word (`exec` alone, or assignments) the redirections
  // apply to the current shell and the assignments update shell
  // parameters.  Both happen in-process: the redirections first, so a
  // failed one assigns nothing, as in zsh.
  if (args.empty()) {
    SavedFds saved;
    if (not redirs.empty()) {
      s::fflush(nullptr);
      if (not apply_redirects(redirs, saved, flags)) return 1;
    }
    // PS4 is expanded before the assignments, which may change it.
    str ps4 = tracing ? trace_prompt() : str{};
    // In front of `exec` with redirections they last only for it, as zsh
    // has them; alone, or with just redirections, they stay.
    bool temporary = replace and not redirs.empty() and not assigns.empty();
    if (temporary) mCtx.params.push_scope(/*prefix=*/true);
    opt<int> assign_status;
    int rc = apply_assigns(assigns, temporary ? AssignTo::Prefix : AssignTo::Shell,
                           assign_status);
    if (temporary) mCtx.params.pop_scope();
    // Assignments alone clear $_ (also `_=x`) when run by zsh's execsimple
    // (a simple sublist) or with redirections (execcmd_exec); otherwise
    // execcmd_exec returns before setunderscore and leaves it.
    if (not had_words and (not redirs.empty() or simple)) mCtx.underscore.clear();
    if (assign_status) subst_status = assign_status;
    // A TRAPxxx that failed in `x=$( … )`: 1, or 2 for SIGINT and SIGQUIT,
    // as zsh's `lastval = errflag ? errflag : cmdoutval`.
    if (mCtx.aborting and mCtx.trap_status >= 0)
      subst_status = mCtx.last_exit = mCtx.trap_failed ? 1 : 2;
    if (tracing and not assigns.empty())
      write_trace(ps4 + trace_assigns(assigns), trace_fd(saved));
    if (replace) saved.commit();
    s::fflush(nullptr);
    // A failed expansion or a readonly name ends the command line.
    if (rc != 0) mCtx.aborting = true;
    // `x=$(cmd)` has cmd's status.
    if (rc == 0 and subst_status) rc = *subst_status;
    return rc;
  }

  if (replace) {
    s::fflush(nullptr);
    SavedFds saved;
    if (not redirs.empty() and not apply_redirects(redirs, saved, flags)) return 1;
    if (not finish_typeset(trace_fd(saved))) return typeset_failed;
    int rc = exec_replace(args, s::move(assigns), argv0, clear_env, child_trace,
                          trace_fd(saved));
    saved.commit();
    return rc;
  }
  if (only == Only::External) {
    if (not finish_typeset(STDERR_FILENO)) return typeset_failed;
    return run_external(args, assigns, redirs, flags, child_trace);
  }

  // Functions come before builtins; an autoloaded one is loaded on its
  // first call.  Aliases were expanded by the parser.
  auto fn = mCtx.functions.end();
  if (only == Only::Any and not mCtx.disabled_functions.contains(args[0])) {
    fn = mCtx.functions.find(args[0]);
    if (fn == mCtx.functions.end() and mCtx.autoloads.contains(args[0])) {
      if (not load_autoload(args[0])) return 1;
      fn = mCtx.functions.find(args[0]);
    }
  }
  bool disabled = mCtx.disabled_builtins.contains(args[0]);
  auto def = fn == mCtx.functions.end() and not disabled ? Builtins.find(args[0]) : s::nullopt;
  // A loaded module may have contributed the builtin; fall through to that
  // before trying an external command.
  const BuiltinDef* mod_def = nullptr;
  if (not def and fn == mCtx.functions.end() and not disabled) {
    mod_def = mCtx.modules.find_builtin(args[0]);
    // An autoloaded builtin: its module comes in now.
    if (auto mod = mod_def ? s::nullopt : mCtx.modules.autoload_for_builtin(args[0])) {
      if (auto r = mCtx.modules.load(*mod, &mCtx, true); not r)
        s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
      mod_def = mCtx.modules.find_builtin(args[0]);
    }
  }
  const BuiltinDef* defp = def ? &def->get() : mod_def;
  if (only == Only::Builtin and not defp) {
    s::fprintf(stderr, "jfsh: no such builtin: %s\n", args[0].c_str());
    return 1;
  }
  if (fn == mCtx.functions.end() and not defp) {
    if (not finish_typeset(STDERR_FILENO)) return typeset_failed;
    return run_external(args, assigns, redirs, flags, child_trace);
  }

  SavedFds saved;
  if (not redirs.empty()) {
    // Push pending buffered output through the ORIGINAL fds before we
    // rewire them; otherwise the output lands in the wrong place after
    // SavedFds restores (bug: stdio buffers survive dup2).
    s::fflush(nullptr);
    if (not apply_redirects(redirs, saved, flags)) return 1;
  }
  // Assignments in front of a function are exported to what it runs.  zsh
  // keeps those in front of a special builtin (or a typeset variant) only
  // under POSIX_BUILTINS; otherwise they last while the builtin runs.
  // Without any, no scope is pushed, so `local` declares in the caller's
  // scope.
  bool is_fn  = fn != mCtx.functions.end();
  bool global = not is_fn and not via_command and posix_special(*defp)
            and mCtx.opts.get(OPT_POSIXBUILTINS);
  if (not assigns.empty() and not global) {
    mCtx.params.push_scope(/*prefix=*/true);
    prefix_scope.params = &mCtx.params;
  }
  int rc = 0;
  if (not assigns.empty()) {
    str ps4 = tracing ? trace_prompt() : str{};
    opt<int> assign_status;
    rc = apply_assigns(assigns,
        global ? AssignTo::Shell : is_fn ? AssignTo::Exported : AssignTo::Prefix,
        assign_status);
    // A readonly name in front of a builtin or function ends the command
    // line, as zsh's errflag does.
    if (rc != 0) {
      if (tracing) write_trace(ps4 + trace_assigns(assigns), trace_fd(saved));
      mCtx.aborting = true;
      return rc;
    }
    if (not finish_typeset(trace_fd(saved))) return typeset_failed;
    // As zsh: PS4 and the assignments, then PS4 again before the words.
    if (tracing)
      write_trace(ps4 + trace_assigns(assigns) + trace_prompt() + traced,
                  trace_fd(saved));
  } else if (not finish_typeset(trace_fd(saved))) {
    return typeset_failed;
  }
  if (rc == 0) {
    if (is_fn and s::exchange(mReturnAfterCommand, false)) {
      // A trap's `return` came during its words: as zsh's doshfunc clears
      // retflag, the function doesn't run and the caller goes on, with the
      // substitution's status.
      mCtx.returning = false;
      rc = mCtx.last_exit;
    } else if (is_fn) {
      ProgRef body = fn->second;  // the function may redefine itself
      rc = call_function(body, args);
    } else {
      // `typeset -r a=(…)`: the value goes in although -r was just given.
      vec<bool> was_readonly;
      for (const auto& a : typeset_arrays) {
        auto p = mCtx.params.lookup(a.name);
        was_readonly.push_back(p and p->get().is_readonly());
      }
      mTrapReturned = false;
      mTrapAbort.reset();
      mCtx.signal_in_builtin = false;
      bool was_aborting = mCtx.aborting;
      rc = defp->fn(args, mCtx);
      // The trap ran inside the builtin: one `read` handled, a signal that
      // cut `wait` short, or one `kill` sent the shell itself.
      bool own    = s::exchange(mCtx.signal_in_builtin, false);
      bool inside = own or mTrapAbort.has_value();
      // A signal the builtin brought on (`kill -USR1 $$`) is handled before
      // its status replaces $?, as in zsh.  (One the builtin handled itself,
      // as `read` does, has its trap's effects applied here too.)
      if (mCtx.signals.has_pending()) {
        bool was_self = s::exchange(mSelfSignal, own);
        handle_signals();
        mSelfSignal = was_self;
      }
      // zsh waits for a builtin's process substitutions, which is where its
      // signal queue runs (drop_trap_signals).
      if (mProcSubsts != substs_before) drop_trap_signals();
      // A trap's `return` makes the function return with the builtin's
      // status; a TRAPINT that returned non-zero gives its status, as
      // zsh's dotrapargs (other signals keep the builtin's; a script
      // makes a 0 of it 1, see exec_string).
      // (At an interactive shell's top the line ends with it too.)
      bool top_line = mCtx.opts.interactive() and mStringDepth == 1;
      if (mTrapReturned and (mCtx.func_depth > 0 or top_line)) mCtx.return_val = rc;
      if (mTrapAbort) {
        // As zsh: when the trap ran inside the builtin, the builtin's status
        // comes after the trap's (`kill -USR1 $$` 0, `read` 1, `wait`
        // 128 + the signal); a signal that just arrived meanwhile leaves the
        // trap's, and so do SIGINT and SIGQUIT.
        if (not inside or mTrapAbort->first == SIGINT or mTrapAbort->first == SIGQUIT)
          rc = mTrapAbort->second;
        mCtx.last_exit = rc;
      } else if (inside and mCtx.aborting and not was_aborting) {
        // A trap the builtin brought on that ended the line otherwise (an
        // interrupt), or the shell's own SIGINT: that status stays.
        rc = mCtx.last_exit;
        // `kill -INT $$`: no ^C was echoed, so no newline is owed.
        if (own) mCtx.intr_echoed = false;
      }
      mTrapReturned = false;
      mTrapAbort.reset();
      for (s::size_t k = 0; k < typeset_arrays.size() and rc == 0; ++k) {
        const auto& a = typeset_arrays[k];
        auto p = mCtx.params.lookup(a.name);
        bool lift = p and p->get().is_readonly() and not was_readonly[k];
        if (lift) p->get().flags &= ~PM_READONLY;
        auto r = mCtx.params.set_array(a.name, a.elements, a.append);
        if (lift) {
          if (auto q = mCtx.params.lookup(a.name)) q->get().flags |= PM_READONLY;
        }
        if (not r) {
          // As zsh's zerr in assignaparam: the command line ends, with
          // typeset's status (0) left as it was.
          s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
          mCtx.aborting = true;
          break;
        }
      }
    }
  }
  s::fflush(nullptr);
  return rc;
}

// $LINENO for the command at es.pc: its line in the script, -c string,
// sourced file or eval text, or in a function counted from the line the
// definition starts on (line 0), as zsh's par_funcdef numbers the body.
static opt<s::uint32_t> line_at(const ExecState& es) noexcept {
  const auto& lines = es.prog->line_map;
  if (es.pc >= lines.size()) return s::nullopt;
  s::uint32_t l = lines[es.pc];
  if (l == 0) return s::nullopt;   // a command with no line (see above)
  if (es.prog->line_base == 0) return l;
  return l >= es.prog->line_base ? l - es.prog->line_base : 0;
}

int Executor::exec_list(ExecState& es, ExecFlags flags) {
  // $LINENO is put back once the list is done, as zsh's execlist does;
  // not after an `exit` that leaves at once (zsh exits from inside it, so
  // the EXIT trap sees the line of the `exit`).
  struct KeepLineno {
    ExecContext& ctx;
    s::uint32_t  lineno;
    ~KeepLineno() {
      if (not (ctx.exiting and ctx.exit_now)) ctx.lineno = lineno;
    }
  } keep_lineno{mCtx, mCtx.lineno};
  int rc = 0;
  while (not es.at_end()) {
    WordCode w = es.fetch();
    if (wc_code(w) != WC_LIST) break;  // WC_END
    s::size_t end = es.pc + wc_list_skip(w);
    // DEBUG runs before each command, with its text in $ZSH_DEBUG_CMD,
    // or after it under NO_DEBUG_BEFORE_CMD, as in zsh; $LINENO is the
    // command's line by then (execlist's lnp1).
    if (mTrapDepth == 0 and mCtx.opts.get(OPT_DEBUGBEFORECMD)
        and mCtx.signals.traps().trapped(SIGDEBUG)) {
      ExecState first{es.prog, es.pc};
      while (not first.at_end() and (wc_code(first.peek()) == WC_SUBLIST
                                     or wc_code(first.peek()) == WC_PIPE))
        ++first.pc;
      if (mEvalTrapFuncDepth != mCtx.func_depth)
        if (auto line = line_at(first)) mCtx.lineno = *line;
      (void)mCtx.params.set_scalar("ZSH_DEBUG_CMD", list_text(*es.prog, es.pc));
      bool skip = run_debug_trap();
      (void)mCtx.params.unset("ZSH_DEBUG_CMD");
      // A trap that set ERR_EXIT skips the command, as zsh's donedebug.
      if (skip and not interrupted()) {
        es.pc = end;
        continue;
      }
    }
    if (interrupted()) return mCtx.exiting ? mCtx.exit_val : rc;
    // NO_EXEC (`set -n`): the rest is read but not run, as zsh's
    // execsimple and execcmd skip it.
    if (not mCtx.opts.get(OPT_EXEC)) {
      es.pc = end;
      continue;
    }
    if (wc_list_flags(w) & WC_LIST_ASYNC) {
      bool        jc   = job_control();
      // `&|` / `&!`: in the background, but not a job of the shell's.
      bool        disown = wc_list_flags(w) & WC_LIST_DISOWN;
      // A lone pipeline (no && || !) is exec'd by the child itself, so the
      // job's pid is the command's own.
      WordCode    sl   = es.peek();
      bool        lone = (wc_sublist_flags(sl) & (WC_SUBLIST_TYPE | WC_SUBLIST_NOT))
                         == WC_SUBLIST_END;
      // A lone pipeline's stages are forked straight from the shell, so
      // $! is the last one, as in zsh.
      if (lone and wc_pipe_count(es.prog->prog[es.pc + 1]) > 1) {
        ++es.pc;
        exec_pline(es, static_cast<ExecFlags>(EF_ASYNC | (disown ? +EF_DISOWN : 0)));
        rc = mCtx.last_exit = 0;
        es.pc = end;
        if (mCtx.exiting) return mCtx.exit_val;
        continue;
      }
      str text = sublist_text(*es.prog, es.pc);
      pid_t pid = fork_child(0, false, ChildKind::Subshell);
      if (pid < 0) {
        rc = mCtx.last_exit = 1;
      } else if (pid == 0) {
        if (jc and mCtx.opts.get(OPT_BGNICE)) {
          errno = 0;
          [[maybe_unused]] int r = ::nice(5);
        }
        // Without job control a background job must not see Ctrl-C: as
        // in zsh, it gets `trap '' INT QUIT`.
        if (not jc) {
          mCtx.signals.set_trap(SIGINT,  TrapAction::Ignore);
          mCtx.signals.set_trap(SIGQUIT, TrapAction::Ignore);
        }
        // The job's own failure doesn't set off ZERR or ERR_EXIT, as in
        // zsh (`false &`); what it runs inside still does.
        ExecState se{es.prog, es.pc};
        exit_child(exec_sublist(se, static_cast<ExecFlags>(
                                      EF_NOERRCHECK | (lone ? +EF_NOFORK : 0))));
      } else if (disown) {
        mCtx.last_bg_pid = pid;
        rc = mCtx.last_exit = 0;
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
    // (After the command, so the `trap … DEBUG` that sets it runs it too.)
    if (not mCtx.opts.get(OPT_DEBUGBEFORECMD) and mTrapDepth == 0
        and mCtx.signals.traps().trapped(SIGDEBUG) and not interrupted())
      run_debug_trap();
    if (interrupted()) {
      // More was to run: an `always` block that ended in the error wasn't
      // the last command (exit_child).
      if (not es.at_end() and wc_code(es.peek()) == WC_LIST) mTryEndedChild = false;
      return rc;
    }
  }
  return rc;
}

bool Executor::trap_returned_in_words() noexcept {
  if (not (mCtx.returning and mCtx.func_depth > 0 and s::exchange(mTrapReturned, false)))
    return false;
  mCtx.return_val = 0;
  return true;
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
  bool exempt = mCtx.noerrexit > 0 or mCtx.err_reported or mCtx.in_startup;
  mCtx.err_reported = true;
  // Neither ZERR nor DEBUG runs inside another trap.
  if (not exempt and mTrapDepth == 0 and mCtx.signals.traps().trapped(SIGZERR))
    run_trap(SIGZERR);
  if (mCtx.exiting) return;
  if (not exempt and mCtx.opts.errexit()) {
    mCtx.exiting  = true;
    mCtx.exit_now = true;
    mCtx.exit_val = rc;
  } else if (mCtx.opts.get(OPT_ERRRETURN) and mCtx.noerrreturn == 0
             and not (mCtx.in_startup and mCtx.func_depth == 0)) {
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
      auto runs   = mCtx.code_runs;
      mCtx.err_reported = false;
      if (exempt) { ++mCtx.noerrexit; ++mCtx.noerrreturn; }
      mSimpleSublist = f & WC_SUBLIST_SIMPLE;
      rc = exec_pline(es, static_cast<ExecFlags>(flags & ~EF_NOERRCHECK));
      if (exempt) { --mCtx.noerrexit; --mCtx.noerrreturn; }
      // A TRAPxxx's abort leaves its status, or whatever came after it, in
      // $?: what an enclosing { }, if, loop or function returns doesn't
      // replace it, as zsh's lastval.
      if (mCtx.aborting and mCtx.trap_status >= 0) rc = mCtx.last_exit;
      if (f & WC_SUBLIST_NOT) rc = (rc == 0) ? 1 : 0;
      // zsh 5.9 forgets the `!` of `! f` once the function (or eval, or
      // sourced file) has run commands of its own.
      if (type == WC_SUBLIST_END and mCtx.code_runs != runs) exempt = false;
      mCtx.last_exit = rc;
      if (rc != 0 and not exempt and counts and not interrupted()
          and not (flags & EF_NOERRCHECK))
        on_command_error(rc);
    }
    es.pc = end;
    if (mCtx.signals.has_pending()) handle_signals();
    if (mCtx.aborting and mCtx.trap_status >= 0) rc = mCtx.last_exit;   // as above
    if (interrupted()) return rc;
    if (type == WC_SUBLIST_END) return rc;
    skip = (type == WC_SUBLIST_AND) ? rc != 0 : rc == 0;
  }
}

// Runs a pipeline.  As in zsh, every stage but the last is forked and the
// last runs in the shell, its stdin the last pipe, so `echo x | read v`
// sets v; an external command or subshell there forks into the
// pipeline's job (EF_PIPELAST).  In the background (EF_ASYNC, a lone
// pipeline before `&`) every stage forks from the shell, and $! is the
// last one.
int Executor::exec_pline(ExecState& es, ExecFlags flags) {
  s::size_t pipe_pc = es.pc;
  WordCode w     = es.fetch();  // WC_PIPE
  WordCode n     = wc_pipe_count(w);
  if (n == 0) return 0;
  mPipeProg = es.prog;
  mPipePc   = pipe_pc;
  bool async  = flags & EF_ASYNC;
  bool disown = flags & EF_DISOWN;
  flags = static_cast<ExecFlags>(flags & ~(EF_ASYNC | EF_DISOWN | EF_PIPELAST | EF_STAGEOUT
                                           | EF_STAGEERR));
  if (n == 1 and not async) {
    // A simple sublist runs as zsh's execsimple, which sets no
    // $pipestatus; anything else runs as a job, whose status it is.
    bool simple = mSimpleSublist;
    int rc = exec_cmd(es, flags);
    if (not simple) mCtx.pipestatus.assign(1, rc);
    return rc;
  }
  bool jc = job_control();

  // Pre-decode every stage's bytecode slice so we can fork them from the
  // parent without re-entering the parser.  Each stage knows its own
  // start and end pc in the compiled program, and whether it has
  // redirections of its own.
  struct Stage { s::size_t start; s::size_t end; bool stderr_too; bool redirected; };
  vec<Stage> stages;
  stages.reserve(n);
  for (s::uint32_t i = 0; i < n; ++i) {
    s::size_t start = es.pc;
    bool redirected = false;
    // Walk this stage: redirections and assignments, then WC_SIMPLE or
    // WC_TYPESET and its words, or a compound command, which its header's
    // skip jumps over.
    while (es.pc < es.prog->prog.size()) {
      WordCode op = wc_code(es.peek());
      if (op == WC_ASSIGN) { es.pc += 3; }
      else if (op == WC_REDIR) { es.pc += 2; redirected = true; }
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
    stages.push_back({start, es.pc, i + 1 < n ? wc_pipe_stderr(w, i) : false,
                      redirected});
  }

  // Build n-1 pipes.
  vec<int> rfds(n, -1), wfds(n, -1);
  auto close_pipes = [&] {
    for (int& fd : rfds) if (fd >= 0) { ::close(fd); fd = -1; }
    for (int& fd : wfds) if (fd >= 0) { ::close(fd); fd = -1; }
  };
  for (s::uint32_t i = 0; i + 1 < n; ++i) {
    int p[2];
    if (not pipe_high(p)) {
      s::fprintf(stderr, "jfsh: pipe: %s\n", errno_text(errno).c_str());
      close_pipes();
      return 1;
    }
    rfds[i + 1] = p[0];
    wfds[i]     = p[1];
  }

  s::uint32_t forked = async ? n : n - 1;
  vec<pid_t> pids;
  pid_t pgid = 0;
  for (s::uint32_t i = 0; i < forked; ++i) {
    pid_t pid = fork_child(pgid, not async, async ? ChildKind::Subshell : ChildKind::Stage);
    if (pid < 0) {
      close_pipes();
      // The stages already started still have to be waited for.
      for (pid_t p : pids) {
        int st;
        while (::waitpid(p, &st, 0) < 0 and errno == EINTR) {}
      }
      return 1;
    }
    if (pid == 0) {
      auto stage_flags = static_cast<ExecFlags>(flags | EF_NOFORK);
      if (async and jc and mCtx.opts.get(OPT_BGNICE)) {
        errno = 0;
        [[maybe_unused]] int r = ::nice(5);
      }
      if (async and not jc) {
        // Without job control a background job must not see Ctrl-C.
        mCtx.signals.set_trap(SIGINT,  TrapAction::Ignore);
        mCtx.signals.set_trap(SIGQUIT, TrapAction::Ignore);
      }
      if (rfds[i] >= 0) ::dup2(rfds[i], 0);
      if (wfds[i] >= 0) {
        ::dup2(wfds[i], 1);
        // The pipe takes part in the stage's own redirections (MULTIOS:
        // `echo x >f | cat` writes to both), and `|&` comes after them.
        if (stages[i].redirected) {
          stage_flags = static_cast<ExecFlags>(stage_flags | EF_STAGEOUT
                                               | (stages[i].stderr_too ? +EF_STAGEERR : 0));
        } else if (stages[i].stderr_too) {
          ::dup2(wfds[i], 2);
        }
      }
      close_pipes();
      ExecState se{es.prog, stages[i].start};
      mStageChild = not async;
      exit_child(exec_cmd(se, stage_flags));
    }
    if (i == 0) pgid = jc ? mChildPgid : pid;
    pids.push_back(pid);
  }
  for (s::uint32_t i = 0; i < n; ++i) {
    if (wfds[i] >= 0) ::close(wfds[i]);
    if (rfds[i] >= 0 and (async or i + 1 < n)) ::close(rfds[i]);
  }

  // The forked stages make a job, so their statuses are recorded however
  // they are reaped.
  // As in zsh, a stage before `|&` shows as `cmd 2>&1`.
  auto texts = pipeline_stage_texts(*es.prog, pipe_pc);
  str text;
  for (s::size_t i = 0; i < texts.size(); ++i) {
    if (i + 1 < texts.size() and wc_pipe_stderr(w, static_cast<s::uint32_t>(i)))
      texts[i] += " 2>&1";
    if (i) text += " | ";
    text += texts[i];
  }
  auto add_process = [&](int jid, s::size_t i, pid_t pid) {
    Process p;
    p.pid    = pid;
    p.text   = i < texts.size() ? texts[i] : str{};
    p.status = ProcStatus::Running;
    mCtx.jobs.add_process(jid, s::move(p));
  };
  if (async and disown) {
    mCtx.last_bg_pid = pids.back();
    return 0;
  }
  if (async) {
    int jid = mCtx.jobs.alloc(pgid, JOB_ASYNC | JOB_NOTIFY | (jc ? +JOB_PGRP : 0),
                              s::move(text));
    for (s::size_t i = 0; i < pids.size(); ++i) add_process(jid, i, pids[i]);
    mCtx.last_bg_pid = pids.back();
    if (mCtx.opts.interactive()) {
      s::fprintf(stderr, "[%d]", jid);
      for (pid_t p : pids) s::fprintf(stderr, " %d", static_cast<int>(p));
      s::fprintf(stderr, "\n");
    }
    return 0;
  }
  int jid = mCtx.jobs.alloc(pgid, jc ? +JOB_PGRP : 0, s::move(text), false);
  for (s::size_t i = 0; i < pids.size(); ++i) add_process(jid, i, pids[i]);

  // The last stage, in the shell with its stdin from the last pipe.
  PipeLast last;
  int last_rc = 0;
  {
    SavedFds in;
    in.save(0);
    ::dup2(rfds[n - 1], 0);
    ::close(rfds[n - 1]);
    PipeLast* outer      = s::exchange(mPipeLast, &last);
    Pid       outer_pgid = s::exchange(mCtx.pipeline_pgid, jc ? pgid : 0);
    int       outer_jid  = s::exchange(mCtx.pipeline_jid, jc ? jid : 0);
    ExecState se{es.prog, stages[n - 1].start};
    last_rc = exec_cmd(se, static_cast<ExecFlags>((flags & ~EF_NOFORK) | EF_PIPELAST));
    s::fflush(nullptr);
    mPipeLast          = outer;
    mCtx.pipeline_pgid = outer_pgid;
    mCtx.pipeline_jid  = outer_jid;
  }
  // `exit` there doesn't wait for the other stages, as in zsh.
  if (mCtx.exiting) {
    mCtx.jobs.remove(jid);
    return last_rc;
  }
  bool in_shell = last.pid < 0;
  if (not in_shell) add_process(jid, n - 1, last.pid);
  struct Unlink {
    ~Unlink() { for (const auto& f : files) ::unlink(f.c_str()); }

    vec<str>& files;
  } unlink_after{last.tmp_files};

  int rc = 0;
  if (jc) {
    rc = wait_foreground_job(mCtx, jid);
    if (mCtx.jobs.get(jid)) return rc;   // stopped: it stays a job
  } else if (auto found = mCtx.jobs.get(jid)) {
    auto& job = found->get();
    for (auto& p : job.procs) {
      int status = 0;
      while (not p.done()) {
        mCtx.signals.let_kept_through(true);
        pid_t got = ::waitpid(p.pid, &status, 0);
        mCtx.signals.let_kept_through(false);
        if (got > 0) {
          mCtx.jobs.update(got, status);
        } else if (errno != EINTR) {
          // Reaped already, as the job's record says.
          if (not p.done()) p.status = ProcStatus::Done;
        }
      }
    }
    mCtx.pipestatus.clear();
    for (const auto& p : job.procs) mCtx.pipestatus.push_back(p.exit_code);
    mCtx.jobs.remove(jid);
    drop_trap_signals();
  }
  if (in_shell) mCtx.pipestatus.push_back(last_rc);
  // PIPEFAIL: the rightmost stage that failed.
  rc = mCtx.pipestatus.back();
  if (mCtx.opts.get(OPT_PIPEFAIL))
    for (int st : mCtx.pipestatus) if (st != 0) rc = st;
  return rc;
}

int Executor::exec_cmd(ExecState& es, ExecFlags flags) {
  // Whether the sublist this command makes up was parsed as simple; any
  // other command reached from here (a pipeline stage) is not.
  bool simple = s::exchange(mSimpleSublist, false);
  if (es.at_end()) return 0;
  // A command with no line of its own (the body of a short `f() cmd`)
  // keeps the line it was run from, as does a trap's own code.
  if (mEvalTrapFuncDepth != mCtx.func_depth)
    if (auto line = line_at(es)) mCtx.lineno = *line;
  // Process substitutions in the command's words (`for f in <(cmd)`) last
  // until it is done; those made outside any command go with the
  // outermost one.
  ProcSubstResources psubst;
  struct PSubstScope {
    ~PSubstScope() {
      exec->mPsubst = outer;
      exec->procsubst_release(*res);
      if (not outer) exec->procsubst_release(exec->mLooseSubst);
    }

    Executor* exec;
    ProcSubstResources* res;
    ProcSubstResources* outer;
  } psubst_scope{this, &psubst, s::exchange(mPsubst, &psubst)};
  // Redirections in front of a compound command belong to it; a simple
  // command goes on with an assignment or its words.
  s::size_t pc = es.pc;
  while (wc_code(es.prog->prog[pc]) == WC_REDIR) pc += 2;
  switch (wc_code(es.prog->prog[pc])) {
    case WC_ASSIGN: case WC_SIMPLE: case WC_TYPESET:
      return exec_simple_cmd(es, flags, simple);
    default: break;
  }
  if (pc == es.pc) return exec_compound(es, flags, simple);

  vec<Redirection> redirs;
  while (wc_code(es.peek()) == WC_REDIR) redirs.push_back(fetch_redir(es));
  vec<str>        no_args;
  opt<int>        subst_status;
  bool fatal = true;
  if (not expand_command(no_args, redirs, subst_status, fatal)) return 1;
  SavedFds saved;
  s::fflush(nullptr);
  if (not apply_redirects(redirs, saved, flags)) return 1;
  int rc = exec_compound(es, flags, simple);
  s::fflush(nullptr);
  return rc;
}

int Executor::exec_compound(ExecState& es, ExecFlags flags, bool simple) {
  WordCode head = es.fetch();
  // A compound command has no words: run as a job (zsh's execcmd_exec) it
  // empties $_ from its start; a simple sublist (WC_SUBLIST_SIMPLE, zsh's
  // execsimple) leaves it.  A function definition sees to its own.
  if (wc_code(head) != WC_FUNCDEF and not simple) mCtx.underscore.clear();
  switch (wc_code(head)) {
    case WC_IF:       return exec_if(es, head);
    case WC_WHILE:    return exec_while(es, head);
    case WC_FOR:      return loop_status(exec_for(es, head));
    case WC_FOR_COND: return loop_status(exec_for_cond(es, head));
    case WC_CASE:     return exec_case(es, head);
    case WC_SELECT:   return exec_select(es, head);
    case WC_REPEAT:   return loop_status(exec_repeat(es, head));
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
int Executor::exec_simple_cmd(ExecState& es, ExecFlags flags, bool simple) {
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
    if (wc_code(es.peek()) == WC_ASSIGN) {
      typeset_args.push_back(fetch_assign());
      typeset_args.back().position = words.size();
    } else {
      words.emplace_back(es.fetch_str());
    }
  }
  int rc = exec_simple(s::move(words), s::move(assigns), s::move(redirs),
                       flags, s::move(typeset_args), simple);
  // A trap returned during its expansion: the function returns its status
  // (an assignment's too).
  bool assign_only = mCtx.returning and mCtx.func_depth > 0
                     and s::exchange(mTrapReturned, false);
  if (s::exchange(mReturnAfterCommand, false) or assign_only) {
    mCtx.returning  = true;
    mCtx.return_val = rc;
  }
  return rc;
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
    // As zsh's execwhile: an error in the condition leaves the last body's
    // status for `while`, but `until` goes on into the body, which the
    // error skips, and so ends with 1 as an error in the body does.
    if (mCtx.aborting and not mCtx.interrupt) {
      rc = mCtx.trap_status >= 0 ? mCtx.last_exit : until ? 1 : rc;
      break;
    }
    if (interrupted()) {
      if (loop_done()) break;
      continue;
    }
    if ((status == 0) == until) break;
    rc = exec_list(es, EF_NONE);
    rc = loop_status(rc);
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
    mCtx.aborting = mCtx.abort_line = true;
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
  if (trap_returned_in_words()) {
    es.pc = end;
    return 0;
  }
  s::size_t body = es.pc;
  int rc = 0;
  ++mCtx.loop_depth;
  for (s::size_t i = 0; i < values.size(); i += names.size()) {
    bool ok = true;
    for (s::size_t k = 0; k < names.size() and ok; ++k)
      ok = assign_var(names[k], i + k < values.size() ? values[i + k] : str{});
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
      // As in zsh, an arithmetic error ends the command line.
      s::fprintf(stderr, "jfsh: %s\n", v.error().message.c_str());
      mCtx.aborting = mCtx.abort_line = true;
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
  if (trap_returned_in_words()) {
    es.pc = end;
    return 0;
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

// Patterns are compiled once per text and options, as a loop runs the same
// `case` or `[[ = ]]` again and again.  The reference lasts until the next
// call.
const CompiledPattern& Executor::cached_pattern(const str& pat) {
  bool ext = mCtx.opts.extended_glob(), mb = mCtx.opts.multibyte();
  str  key = str{ext ? 'e' : '-', mb ? 'm' : '-'} + pat;
  auto found = mPatterns.find(key);
  if (found == mPatterns.end()) {
    if (mPatterns.size() >= 4096) mPatterns.clear();
    found = mPatterns.emplace(s::move(key), CompiledPattern::compile(pat, false, ext, mb)).first;
  }
  return found->second;
}

// case := WC_CASE(HEAD, skip) WC_STRING(subject) arm*.  After a matching
// arm, `;;` ends the case, `;&` runs the next arm's body unconditionally
// and `;|` goes on testing the arms after it.
int Executor::exec_case(ExecState& es, WordCode head) {
  s::size_t end = es.pc + wc_cmd_skip(head);
  Expander ex{*this, mCtx};
  auto fail = [&](const ShellError& e) {
    s::fprintf(stderr, "jfsh: %s\n", e.message.c_str());
    mCtx.aborting = mCtx.abort_line = true;
    es.pc = end;
    return 1;
  };
  auto subject = ex.scalar(es.fetch_str());
  if (not subject) return fail(subject.error());
  // A TRAPxxx that failed in the word ends the line with status 0 (zsh's
  // execcase doesn't check errflag); a trap's `return` there makes the
  // function return 0.
  if ((mCtx.aborting and mCtx.trap_failed) or trap_returned_in_words()) {
    es.pc = end;
    return mCtx.last_exit = 0;
  }
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
      const CompiledPattern& cp = cached_pattern(*pat);
      if (cp.bad()) {
        // As in zsh: the command line ends, with status 0.
        s::fprintf(stderr, "jfsh: bad pattern: %s\n", glob_unescape(*pat).c_str());
        mCtx.aborting = mCtx.abort_line = true;
        es.pc = end;
        return 0;
      }
      PatternMatch pm;
      match = cp.matches(*subject, &pm);
      if (match)
        set_match_params(mCtx.params, cp, *subject, 0, subject->size(), pm,
                         mCtx.opts.get(OPT_KSH_ARRAYS));
    }
    if (not match) {
      es.pc = next;
      continue;
    }
    rc = exec_list(es, EF_NONE);
    if (interrupted() or wc_case_end(arm) == WC_CASE_BREAK) break;
    fall  = wc_case_end(arm) == WC_CASE_FALL;
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
  // A trap's `return` in the words: zsh still runs the menu, and the
  // function returns 0 after it.
  bool return_after = trap_returned_in_words();
  if (return_after) mCtx.returning = false;
  struct ReturnAfter {
    ExecContext& ctx;
    bool         on;
    ~ReturnAfter() {
      if (not on) return;
      ctx.returning  = true;
      ctx.return_val = 0;
    }
  } return_after_guard{mCtx, return_after};
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
    // The number at the start, as atoi reads it (`1x` is 1).
    long n = s::strtol(line->c_str(), nullptr, 10);
    str choice = (n >= 1
                  and static_cast<s::size_t>(n) <= values.size())
                       ? values[static_cast<s::size_t>(n) - 1] : str{};
    if (not assign_var(names[0], s::move(choice))) {
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
    mCtx.aborting = mCtx.abort_line = true;
    es.pc = end;
    return 1;
  }
  if (trap_returned_in_words()) {
    es.pc = end;
    return 0;
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
// With EF_NOFORK (already in a child of its own: a pipeline stage, a lone
// background job) the body runs in that child without another fork.
int Executor::exec_subsh(ExecState& es, WordCode head, ExecFlags flags) {
  s::size_t end = es.pc + wc_cmd_skip(head);
  pid_t pid = (flags & EF_NOFORK) ? 0 : fork_command(flags, ChildKind::Subshell);
  if (pid < 0) {
    es.pc = end;
    return 1;
  }
  // A foreground pipeline's stage kept every trap; as the subshell it is,
  // it resets them now.
  if (pid == 0 and s::exchange(mStageChild, false)) enter_child(ChildKind::Subshell);
  if (pid == 0) exit_child(exec_list(es, EF_NONE));
  es.pc = end;
  return wait_command(pid);
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
    // $_ is its last argument, as for any command.
    mCtx.underscore = args->empty() ? str{} : args->back();
    args->insert(args->begin(), "(anon)");
    return call_function(body, *args);
  }
  // The names are expanded as words, as zsh's execfuncdef does: `'if'()`
  // defines `if`, `$n()` the value of $n.
  auto raw = fetch_words(es);
  Expander ex{*this, mCtx};
  auto names = ex.words(raw);
  if (not names) {
    s::fprintf(stderr, "jfsh: %s\n", names.error().message.c_str());
    mCtx.aborting = mCtx.abort_line = true;
    es.pc = end;
    return 1;
  }
  ProgRef body = extract_body(*es.prog, es.pc, end);
  body->line_base = def_line;
  for (auto& name : *names) {
    // TRAPINT() { … } is the trap for SIGINT, and so on.
    if (name.starts_with("TRAP"))
      if (auto sig = SignalTable::name_to_sig(strv(name).substr(4));
          sig and not s::isdigit(static_cast<unsigned char>(name[4])))
        mCtx.signals.set_trap(*sig, TrapAction::Function, {}, body);
    mCtx.autoloads.erase(name);
    mCtx.function_files[name] = mCtx.source_name.empty() ? mCtx.arg0 : mCtx.source_name;
    mCtx.functions[s::move(name)] = body;
  }
  mCtx.underscore.clear();
  es.pc = end;
  return 0;
}

// cond := WC_COND(type, skip) …, a `[[ ]]` tree (see wordcode.h).  The
// status is 0 when true, 1 when false, 2 on an error and 3 for `-o` with
// no such option; `&&` and `||` stop at an error.  Operands are expanded
// without splitting or globbing; the right side of = == != is a pattern,
// of =~ an extended regex, and -eq … -ge compare arithmetic expressions.
static str trace_quote(strv w);

// Under XTRACE the parts of the condition that were evaluated are
// collected (operands expanded) and printed as one `[[ … ]]` line.
int Executor::exec_cond(ExecState& es, WordCode head) {
  int rc = 0;
  if (mCondTrace or not mCtx.opts.xtrace()) {
    rc = exec_cond_node(es, head);
  } else {
    str text;
    mCondTrace = &text;
    rc = exec_cond_node(es, head);
    mCondTrace = nullptr;
    trace_text("[[ " + text + " ]]");
  }
  // A TRAPxxx that failed in a word leaves the test's status, the failed
  // substitution expanding to nothing, as zsh's.  A trap's `return` there:
  // the function returns the test's status.
  if (mCtx.aborting and mCtx.trap_failed) mCtx.last_exit = rc;
  if (mCtx.returning and mCtx.func_depth > 0 and s::exchange(mTrapReturned, false))
    mCtx.return_val = rc;
  return rc;
}

// The right side of = == != as xtrace shows it: the pattern, with blanks
// escaped.
static str trace_pattern(strv pat) {
  str out;
  for (char c : pat) {
    if (c == ' ' or c == '\t') out += '\\';
    out += c;
  }
  return out.empty() ? str("''") : out;
}

// `-x` alone, unquoted: zsh's par_cond_2 reads a two-character word
// starting with `-` as a condition missing its operand (`[[ -foo ]]` is a
// string test).
static bool looks_like_cond_op(strv w) noexcept {
  return w.size() == 2 and w[0] == '-';
}

// `!`, `&&` and `||` go down their right side in a loop, so a long chain
// (laid out to the right by the parser) needs no recursion; a left
// operand that is a chain of its own, from parentheses, recurses.
int Executor::exec_cond_node(ExecState& es, WordCode head) {
  s::size_t end  = es.pc + wc_cmd_skip(head);
  bool      flip = false;   // under an odd number of `!`
  int       rc   = 0;
  auto note = [&](strv s) { if (mCondTrace) *mCondTrace += s; };
  for (;;) {
    WordCode type = wc_cmd_type(head);
    if (type == WC_COND_NOT) {
      note("! ");
      flip = not flip;
      head = es.fetch();
      continue;
    }
    if (type != WC_COND_AND and type != WC_COND_OR) {
      rc = exec_cond_leaf(es, head);
      break;
    }
    if (stack_low()) {
      s::fprintf(stderr, "jfsh: condition nested too deeply\n");
      mCtx.aborting = mCtx.abort_line = true;
      rc = 2;
      break;
    }
    bool is_and = type == WC_COND_AND;
    rc = exec_cond(es, es.fetch());
    // `&&` and `||` stop at an error, but not at a failed TRAPxxx: zsh's
    // evalcond has no errflag check there (cond.c).
    if (rc != (is_and ? 0 : 1) or (interrupted() and not mCtx.trap_failed)) break;
    note(is_and ? " && " : " || ");
    head = es.fetch();
  }
  es.pc = end;
  return flip and rc < 2 ? 1 - rc : rc;
}

// A `[[ ]]` pattern word as written, quoted characters escaped and the
// quotes dropped, nothing expanded.
static str pattern_as_written(strv w) {
  str  out;
  char quote = 0;
  for (s::size_t i = 0; i < w.size(); ++i) {
    char c = w[i];
    if (quote) {
      if (c == quote) quote = 0;
      else {
        if (quote == '"' and c == '\\' and i + 1 < w.size()) c = w[++i];
        out += '\\';
        out += c;
      }
    } else if (c == '\'' or c == '"') {
      quote = c;
    } else if (c == '\\' and i + 1 < w.size()) {
      out += c;
      out += w[++i];
    } else {
      out += c;
    }
  }
  return out;
}

// A primary of `[[ ]]`: a string, a unary or binary test, or a module's
// condition.
int Executor::exec_cond_leaf(ExecState& es, WordCode head) {
  s::size_t end = es.pc + wc_cmd_skip(head);
  Expander ex{*this, mCtx};
  // After a failed TRAPxxx zsh's prefork expands nothing, so the rest of
  // the condition sees its words as written (cond.c's singsub under
  // errflag): a string keeps its quotes, a pattern drops them.
  auto as_written = [&] { return mCtx.aborting and mCtx.trap_failed; };
  auto scalar = [&](strv w) -> Result<str> {
    if (as_written()) return str(w);
    return ex.scalar(w);
  };
  auto pattern = [&](strv w) -> Result<str> {
    if (as_written()) return pattern_as_written(w);
    return ex.pattern(w);
  };
  // Status 2 is an error that ends the command line, as zsh's execcond
  // makes it.
  auto finish = [&](int rc) {
    es.pc = end;
    if (rc == 2) mCtx.aborting = mCtx.abort_line = true;
    return rc;
  };
  // A failed expansion ends the command line, as for a simple command.
  auto fail = [&](const ShellError& e) {
    s::fprintf(stderr, "jfsh: %s\n", e.message.c_str());
    mCtx.aborting = mCtx.abort_line = true;
    return finish(1);
  };
  auto note = [&](strv s) { if (mCondTrace) *mCondTrace += s; };
  switch (wc_cmd_type(head)) {
    case WC_COND_STR: {
      strv raw = es.fetch_str();
      if (looks_like_cond_op(raw)) {
        s::fprintf(stderr, "jfsh: unknown condition: %.*s\n",
                   static_cast<int>(raw.size()), raw.data());
        mCtx.aborting = true;
        return finish(2);
      }
      auto word = scalar(raw);
      if (not word) return fail(word.error());
      note(trace_quote(*word));
      return finish(word->empty() ? 1 : 0);
    }
    case WC_COND_UNARY: {
      str op(es.fetch_str());
      auto arg = scalar(es.fetch_str());
      if (not arg) return fail(arg.error());
      note(op + " " + trace_quote(*arg));
      return finish(cond_unary(op, *arg, mCtx, ""));
    }
    case WC_COND_MOD: {
      str op(es.fetch_str());
      vec<str> args;
      for (WordCode k = 1; k < wc_cmd_skip(head); ++k) {
        // Patterns keep their quoting, as zsh hands them over tokenized.
        auto arg = pattern(es.fetch_str());
        if (not arg) return fail(arg.error());
        args.push_back(s::move(*arg));
      }
      note(op);
      for (const auto& a : args) note(" " + trace_quote(a));
      int rc = cond_completion(op, args, mCtx);
      if (rc < 0) {
        s::fprintf(stderr, "jfsh: unknown condition: %s\n", op.c_str());
        rc = 2;
      }
      if (rc > 1) mCtx.aborting = mCtx.abort_line = true;
      return finish(rc);
    }
    default: break;
  }
  str op(es.fetch_str());
  strv lhs_word = es.fetch_str();
  strv rhs_word = es.fetch_str();
  if (cond_is_numeric(op)) {
    note(str(lhs_word) + " " + op + " " + str(rhs_word));
    // The operands are words, expanded and then evaluated.  As in zsh, an
    // error ends the command line, the failed side counting as 0 in the
    // comparison that gives the status.
    MathNum side[2];
    strv words[2] = {lhs_word, rhs_word};
    bool error = false;
    for (int k = 0; k < 2 and not error; ++k) {
      auto text = scalar(words[k]);
      if (not text) return fail(text.error());
      auto v = ex.eval_math(*text);
      if (v) {
        side[k] = *v;
        continue;
      }
      s::fprintf(stderr, "jfsh: %s\n", v.error().message.c_str());
      mCtx.aborting = mCtx.abort_line = true;
      error = true;
    }
    // After a failed TRAPxxx, zsh's matheval gives 0 for every operand
    // while errflag is set.
    if (mCtx.aborting and mCtx.trap_failed) side[0] = side[1] = MathNum{};
    return finish(cond_numeric(op, side[0], side[1]));
  }
  auto lhs = scalar(lhs_word);
  if (not lhs) return fail(lhs.error());
  if (op == "=" or op == "==" or op == "!=") {
    auto pat = pattern(rhs_word);
    if (not pat) return fail(pat.error());
    note(trace_quote(*lhs) + " " + op + " " + trace_pattern(*pat));
    // After a failed TRAPxxx, zsh's patmatch stops at errflag before any
    // glob operator matches: only a plain string can be equal.
    if (as_written()) {
      bool equal = not has_glob_chars(*pat, mCtx.opts.extended_glob())
                   and glob_unescape(*pat) == *lhs;
      return finish(equal == (op != "!=") ? 0 : 1);
    }
    const CompiledPattern& cp = cached_pattern(*pat);
    if (cp.bad()) {
      s::fprintf(stderr, "jfsh: bad pattern: %s\n", glob_unescape(*pat).c_str());
      mCtx.aborting = mCtx.abort_line = true;
      return finish(2);
    }
    PatternMatch pm;
    bool match = cp.matches(*lhs, &pm);
    if (match)
      set_match_params(mCtx.params, cp, *lhs, 0, lhs->size(), pm, mCtx.opts.get(OPT_KSH_ARRAYS));
    return finish(match == (op != "!=") ? 0 : 1);
  }
  auto rhs = scalar(rhs_word);
  if (not rhs) return fail(rhs.error());
  note(trace_quote(*lhs) + " " + op + " " + trace_quote(*rhs));
  if (op == "=~") return finish(cond_rematch(*lhs, *rhs));
  if (op == "<")  return finish(*lhs < *rhs ? 0 : 1);
  if (op == ">")  return finish(*lhs > *rhs ? 0 : 1);
  if (op == "-nt" or op == "-ot" or op == "-ef") return finish(cond_file_binary(op, *lhs, *rhs));
  // Module-provided conditions (M10 Phase 2c polish).  The parser accepts
  // `-pcre-match` / `-regex-match` as binary ops whether or not the owning
  // module is loaded; this dispatch runs the function if the module is
  // loaded and reports a clear error otherwise.
  if (auto* cd = mCtx.modules.find_cond(op))
    return finish(cd->fn(*lhs, *rhs, mCtx));
  s::fprintf(stderr, "jfsh: unknown condition: %s\n", op.c_str());
  return finish(2);
}

// arith := WC_ARITH(0, 1) WC_STRING(expr).  Status 0 when the value is
// non-zero, 1 when it is zero, 2 on an error.
int Executor::cond_rematch(const str& lhs, const str& re) {
  static constexpr const char* MATCH_PARAMS[] = {
    "MATCH", "MBEGIN", "MEND", "match", "mbegin", "mend",
  };
  bool bash = mCtx.opts.get(OPT_BASHREMATCH);
  // With BASH_REMATCH the matchers' $MATCH / $match are put back after.
  struct Saved {
    bool     set = false, array = false, integer = false;
    str      scalar;
    vec<str> arr;
  };
  vec<Saved> saved;
  if (bash)
    for (const char* name : MATCH_PARAMS) {
      Saved sv;
      if (auto p = mCtx.params.lookup(name); p and p->get().is_set()) {
        const Param& prm = p->get();
        sv.set     = true;
        sv.array   = prm.is_array();
        sv.integer = prm.is_integer();
        if (sv.array) sv.arr = prm.array();
        else          sv.scalar = prm.scalar();
      }
      saved.push_back(s::move(sv));
    }
  int rc;
  const CondDef* pcre = nullptr;
  if (mCtx.opts.get(OPT_REMATCHPCRE)) {
    pcre = mCtx.modules.find_cond("-pcre-match");
    if (not pcre and mCtx.modules.load("zsh/pcre", &mCtx))
      pcre = mCtx.modules.find_cond("-pcre-match");
  }
  if (pcre) {
    rc = pcre->fn(lhs, re, mCtx);
    if (rc == 2) rc = 1;   // a bad pattern is a false test, as for an ERE
  } else {
    rc = cond_regex(lhs, re, mCtx, "");
  }
  if (not bash) return rc;
  if (rc == 0) {
    vec<str> all;
    if (auto m = mCtx.params.lookup("MATCH")) all.push_back(m->get().scalar());
    if (auto m = mCtx.params.lookup("match"); m and m->get().is_array())
      for (auto& e : m->get().array()) all.push_back(e);
    (void)mCtx.params.set_array("BASH_REMATCH", s::move(all));
  }
  for (s::size_t k = 0; k < saved.size(); ++k) {
    const char* name = MATCH_PARAMS[k];
    auto& sv = saved[k];
    if (not sv.set) {
      (void)mCtx.params.unset(name);
    } else if (sv.array) {
      (void)mCtx.params.set_array(name, s::move(sv.arr));
    } else {
      (void)mCtx.params.set_scalar(name, sv.scalar);
      if (sv.integer)
        mCtx.params.lookup(name)->get().set(s::strtoll(sv.scalar.c_str(), nullptr, 10));
    }
  }
  return rc;
}

int Executor::exec_arith(ExecState& es, WordCode) {
  Expander ex{*this, mCtx};
  strv expr = es.fetch_str();
  str traced;
  if (mCtx.opts.xtrace()) {
    // zsh shows the expression with its substitutions done.
    auto t = ex.heredoc(expr);
    traced = t ? *t : str(expr);
    trace_text("(( " + traced + " ))");
    expr = traced;
  }
  auto v = ex.math(expr);
  // A TRAPxxx that failed during the expansion: 2.  zsh's execarith clears
  // the error but not the trap's retflag, so the function returns 2, and
  // at the top just the line ends (the EXIT trap still runs).
  if (mCtx.aborting and mCtx.trap_failed) {
    mCtx.aborting = mCtx.abort_line = false;
    mCtx.end_trap_abort();
    mCtx.returning   = true;
    mCtx.return_val  = 2;
    mCtx.return_line = true;
    return mCtx.last_exit = 2;
  }
  if (mCtx.aborting and mCtx.trap_status >= 0) return mCtx.last_exit = 2;
  // A trap's `return` there: the function returns the test's status.
  if (mCtx.returning and mCtx.func_depth > 0 and s::exchange(mTrapReturned, false))
    mCtx.return_val = v ? (v->truthy() ? 0 : 1) : 2;
  if (not v) {
    s::fprintf(stderr, "jfsh: %s\n", v.error().message.c_str());
    return 2;
  }
  return v->truthy() ? 0 : 1;
}

// try := WC_TRY(0, skip) body body(always).  The always block runs however
// the first one ended, as zsh's exectry: a break, continue, return or exit
// pending from the first block is held while it runs and then goes on,
// unless the always block started one of its own.  An error that ended the
// first block is held too, as $TRY_BLOCK_ERROR: the always block may clear
// it to go on.  The status is the first block's, also for a `return` in the
// always block.  An exit that leaves at once (exit_now) skips the block.
int Executor::exec_always(ExecState& es, WordCode head) {
  s::size_t end    = es.pc + wc_cmd_skip(head);
  s::size_t always = body_end(*es.prog, es.pc);
  mCtx.interrupt = false;
  int rc = exec_list(es, EF_NONE);
  es.pc = end;
  if (mCtx.exiting and mCtx.exit_now) return rc;
  if (mCtx.aborting and rc == 0) rc = 1;
  auto held = s::tuple{mCtx.break_level, mCtx.continuing, mCtx.returning,
                       mCtx.return_val, mCtx.exiting, mCtx.exit_val};
  auto saved_try  = s::pair{mCtx.try_error, mCtx.try_interrupt};
  bool abort_line = mCtx.abort_line, interrupt = mCtx.aborting and mCtx.interrupt;
  bool trap_abort = mCtx.aborting and mCtx.trap_status >= 0;
  mCtx.try_error     = mCtx.aborting and not interrupt ? 1 : 0;
  mCtx.try_interrupt = interrupt ? 1 : 0;
  mCtx.aborting    = mCtx.abort_line = false;
  mCtx.break_level = 0;
  mCtx.continuing  = false;
  mCtx.returning   = false;
  mCtx.exiting     = false;
  es.pc = always;
  exec_list(es, EF_NONE);
  es.pc = end;
  // As in zsh, an error in the always block itself is dropped unless the
  // first block's still stands; so is an interrupt.
  mCtx.aborting   = mCtx.try_error != 0 or mCtx.try_interrupt != 0;
  mCtx.abort_line = mCtx.aborting and abort_line;
  if (not mCtx.aborting) mCtx.end_trap_abort();
  mCtx.interrupt  = mCtx.try_interrupt != 0;
  s::tie(mCtx.try_error, mCtx.try_interrupt) = saved_try;
  if (mCtx.break_level == 0 and not mCtx.returning and not mCtx.exiting) {
    s::tie(mCtx.break_level, mCtx.continuing, mCtx.returning,
           mCtx.return_val, mCtx.exiting, mCtx.exit_val) = held;
  }
  // A TRAPxxx that failed in the first block also set zsh's retflag, which
  // a TRY_BLOCK_ERROR=0 doesn't clear: the code goes on returning.
  if (trap_abort and not mCtx.aborting and not mCtx.exiting)
    mCtx.returning = mCtx.return_line = true;   // a script's top: just the line
  if (mCtx.returning) mCtx.return_val = rc;
  mCtx.last_exit = rc;   // the try list's status, as zsh's exectry
  mTryEndedChild = mCtx.aborting;
  return rc;
}

// ---------------------------------------------------------------------------
// SavedFds: RAII restore of fds overwritten by builtin redirections.
// ---------------------------------------------------------------------------
SavedFds& SavedFds::operator=(SavedFds&& other) noexcept {
  if (this != &other) {
    restore();
    entries    = s::move(other.entries);
    tees       = s::move(other.tees);
    fail       = other.fail;
    other.entries.clear();
    other.tees.clear();
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
  note_shell_fd(backup);
  entries.push_back({dst, backup});
}

void SavedFds::commit() noexcept {
  for (const auto& e : entries)
    if (e.backup >= 0) ::close(e.backup);
  entries.clear();
  tees.clear();
}

void SavedFds::restore() noexcept {
  // Output a builtin left in stdio goes where it was redirected; when that
  // fails (`type echo >&-`) it is dropped, not written to the restored fd
  // later.
  if (not entries.empty() and s::fflush(stdout) != 0) {
#if defined(__APPLE__) or defined(__FreeBSD__)
    ::fpurge(stdout);
#else
    ::__fpurge(stdout);
#endif
    s::clearerr(stdout);
  }
  for (auto it = entries.rbegin(); it != entries.rend(); ++it) {
    if (it->backup < 0) {
      ::close(it->dst);
    } else {
      ::dup2(it->backup, it->dst);
      ::close(it->backup);
    }
  }
  entries.clear();
  // As zsh waits for its multio processes: the next command sees all the
  // output in the files.
  for (Pid pid : tees) {
    int status;
    while (::waitpid(pid, &status, 0) < 0 and errno == EINTR) {}
  }
  tees.clear();
}

// The fd a redirection operator applies to by default: 0 for those that
// read, 1 for the rest.
static int default_fd(RedirType t) noexcept {
  switch (t) {
    case REDIR_READ: case REDIR_READWRITE: case REDIR_HEREDOC:
    case REDIR_HEREDOCD: case REDIR_HERESTR: case REDIR_IN_DUP:
    case REDIR_PROC_IN:
      return 0;

    break; default:
      return 1;
  }
}

// The fd named by the target of `>&N` / `<&N`, none when it is not a
// number.  As in zsh, a number too big for an int wraps.
static opt<int> parse_fd_target(const str& s) {
  if (s.empty()) return s::nullopt;
  s::uint64_t v = 0;
  for (char c : s) {
    if (not s::isdigit(static_cast<unsigned char>(c))) return s::nullopt;
    v = v * 10 + static_cast<s::uint64_t>(c - '0');
  }
  return static_cast<int>(static_cast<s::uint32_t>(v));
}

// Writes all of `n` bytes; false on an error.
static bool write_all(int fd, const char* p, s::size_t n) {
  while (n > 0) {
    ssize_t w = ::write(fd, p, n);
    if (w < 0) {
      if (errno == EINTR) continue;
      return false;
    }
    p += w;
    n -= static_cast<s::size_t>(w);
  }
  return true;
}

// A pipe whose read end gives `body`, for a here-document or here-string.
// A body that fits in the pipe is written straight in; the rest of a
// longer one is fed by a forked writer, so the shell can't block on it.
static int heredoc_body_fd(strv body) {
  int p[2];
  if (not pipe_high(p)) return -1;
  ::fcntl(p[1], F_SETFL, O_NONBLOCK);
  const char* data = body.data();
  s::size_t   left = body.size();
  while (left > 0) {
    ssize_t w = ::write(p[1], data, left);
    if (w < 0) {
      if (errno == EINTR) continue;
      break;
    }
    data += w;
    left -= static_cast<s::size_t>(w);
  }
  if (left > 0 and errno != EAGAIN) {
    ::close(p[0]);
    ::close(p[1]);
    return -1;
  }
  if (left > 0) {
    ::fcntl(p[1], F_SETFL, 0);
    s::fflush(nullptr);
    pid_t pid = ::fork();
    if (pid < 0) {
      ::close(p[0]);
      ::close(p[1]);
      return -1;
    }
    if (pid == 0) {
      Signals.child_reset(ChildKind::Other);
      ::close(p[0]);
      ::signal(SIGPIPE, SIG_DFL);
      write_all(p[1], data, left);
      _exit(0);
    }
  }
  ::close(p[1]);
  return p[0];
}

// The fd of a process substitution's pipe as the command gets it, as
// /dev/fd/N: open across exec.
static int keep_open(int fd) {
  ::fcntl(fd, F_SETFD, 0);
  forget_shell_fd(fd);
  return fd;
}

// In a process substitution's child: without job control, `<(…)` and
// `=(…)` read nothing from the command's stdin, as zsh's async subshells
// do.
static void stdin_from_null() {
  int fd = ::open("/dev/null", O_RDONLY);
  if (fd < 0) return;
  if (fd != 0) {
    ::dup2(fd, 0);
    ::close(fd);
  }
}

// Fork a subshell that runs `src` with its stdout piped to a fresh fd.
// Returns the read end for the parent to pass as /dev/fd/N.  On failure
// returns -1.  Used for <(cmd).
int Executor::procsubst_in(strv src) {
  ++mProcSubsts;
  int pfd[2];
  if (not pipe_high(pfd)) return -1;
  s::fflush(nullptr);
  pid_t pid = ::fork();
  if (pid < 0) { ::close(pfd[0]); ::close(pfd[1]); return -1; }
  if (pid == 0) {
    bool jc = job_control();
    enter_child(ChildKind::Subshell);
    // As any asynchronous child without job control, zsh's `<( … )`
    // doesn't see Ctrl-C.
    if (not jc) {
      mCtx.signals.set_trap(SIGINT,  TrapAction::Ignore);
      mCtx.signals.set_trap(SIGQUIT, TrapAction::Ignore);
    }
    ::close(pfd[0]);
    ::dup2(pfd[1], 1);
    ::close(pfd[1]);
    if (not jc) stdin_from_null();
    exit_child(exec_string(src));
  }
  ::close(pfd[1]);
  return keep_open(pfd[0]);
}

// Fork a subshell that runs `src` with its stdin piped from a fresh fd.
// Returns the write end for the parent to pass as /dev/fd/N.
int Executor::procsubst_out(strv src) {
  ++mProcSubsts;
  int pfd[2];
  if (not pipe_high(pfd)) return -1;
  s::fflush(nullptr);
  pid_t pid = ::fork();
  if (pid < 0) { ::close(pfd[0]); ::close(pfd[1]); return -1; }
  if (pid == 0) {
    bool jc = job_control();
    enter_child(ChildKind::Subshell);
    // As `<( … )`, an asynchronous child that doesn't see Ctrl-C.
    if (not jc) {
      mCtx.signals.set_trap(SIGINT,  TrapAction::Ignore);
      mCtx.signals.set_trap(SIGQUIT, TrapAction::Ignore);
    }
    ::close(pfd[1]);
    ::dup2(pfd[0], 0);
    ::close(pfd[0]);
    exit_child(exec_string(src));
  }
  ::close(pfd[0]);
  return keep_open(pfd[1]);
}

// Run `src` with its stdout captured into a fresh temp file and return
// the file's path, named after $TMPPREFIX as zsh's are.  Used for =(cmd).
str Executor::procsubst_eq(strv src) {
  str prefix = "/tmp/zsh";
  if (auto p = mCtx.params.lookup("TMPPREFIX"); p and p->get().is_set())
    prefix = p->get().scalar();
  str tmpl = prefix + "XXXXXX";
  int fd = move_high(::mkstemp(tmpl.data()));
  if (fd < 0) return {};
  s::fflush(nullptr);
  pid_t pid = ::fork();
  if (pid < 0) { ::close(fd); ::unlink(tmpl.c_str()); return {}; }
  if (pid == 0) {
    bool jc = job_control();
    enter_child(ChildKind::Subshell);
    ::dup2(fd, 1);
    ::close(fd);
    if (not jc) stdin_from_null();
    exit_child(exec_string(src));
  }
  int status = 0;
  while (::waitpid(pid, &status, 0) < 0 and errno == EINTR) {}
  ::close(fd);
  return tmpl;
}

// Forks the MULTIOS helper: copies what arrives on `in` to every fd of
// `outs` until no writer of `in` is left.  Every other fd is closed in
// it, so it keeps no pipe open.  Returns the pid, or -1.
static pid_t spawn_tee(int in, const vec<int>& outs) {
  s::fflush(nullptr);
  pid_t pid = ::fork();
  if (pid != 0) return pid;
  Signals.child_reset(ChildKind::Other);
  long top = ::sysconf(_SC_OPEN_MAX);
  if (top < 0 or top > 4096) top = 4096;
  for (int fd = 0; fd < top; ++fd)
    if (fd != in and s::find(outs.begin(), outs.end(), fd) == outs.end()) ::close(fd);
  char buf[4096];
  for (;;) {
    ssize_t n = ::read(in, buf, sizeof buf);
    if (n < 0 and errno == EINTR) continue;
    if (n <= 0) break;
    for (int fd : outs) write_all(fd, buf, static_cast<s::size_t>(n));
  }
  _exit(0);
}

// Opens the file of a redirection that writes, as zsh does.  Without
// CLOBBER (and not forced by `>|`), `>` creates the file but opens an
// existing one only when it is not a regular file (a device, a fifo) or,
// with CLOBBER_EMPTY, an empty one; `>>` creates nothing unless
// APPEND_CREATE is set.  -1 with errno on failure.
static int open_output(const OptionState& opts, const str& path, bool append,
                       bool force) {
  int  base    = O_WRONLY | O_NOCTTY | O_CLOEXEC;
  bool clobber = force or opts.get(OPT_CLOBBER);
  if (append) {
    if (clobber or opts.get(OPT_APPENDCREATE)) base |= O_CREAT;
    return ::open(path.c_str(), base | O_APPEND, 0666);
  }
  if (clobber) return ::open(path.c_str(), base | O_CREAT | O_TRUNC, 0666);
  int fd = ::open(path.c_str(), base | O_CREAT | O_EXCL, 0666);
  if (fd >= 0) return fd;
  int err = errno;
  fd = ::open(path.c_str(), base);
  if (fd >= 0) {
    struct stat st{};
    if (::fstat(fd, &st) == 0
        and (not S_ISREG(st.st_mode) or (opts.get(OPT_CLOBBEREMPTY) and st.st_size == 0)))
      return fd;
    ::close(fd);
  }
  errno = err;
  return -1;
}

// The redirections are made in order, as zsh's execcmd does.  Under
// MULTIOS the second output redirection to an fd splits it: the fd
// becomes a pipe to a tee that writes to what the fd went to before and
// to the new target, and later ones add targets.  The tee is forked once
// the redirections are done, or before another redirection reads from
// the fd (`>a 2>&1 >b`: the error output goes to a only).
bool Executor::apply_redirects(const vec<Redirection>& redirs,
                               SavedFds& saved, ExecFlags flags) {
  bool multios = mCtx.opts.get(OPT_MULTIOS);
  // fds with an output redirection: once split, the tee's targets and the
  // read end of its pipe.
  struct Stream { vec<int> outs; int in = -1; };
  s::map<int, Stream> streams;
  // In a pipeline stage, the pipe is fd 1's first output, and `|&` is a
  // `2>&1` after the command's own redirections.
  if ((flags & EF_STAGEOUT) and multios) streams[1];
  vec<Redirection> stage_err;
  if (flags & EF_STAGEERR) stage_err.push_back({REDIR_MERGE, 2, "1", false, {}});

  // Forks the tee of fd's stream, if it was split.
  auto finish = [&](int fd) {
    auto it = streams.find(fd);
    if (it == streams.end() or it->second.in < 0) return true;
    Stream& m = it->second;
    pid_t pid = spawn_tee(m.in, m.outs);
    ::close(m.in);
    for (int o : m.outs) ::close(o);
    m = Stream{};
    if (pid < 0) {
      s::fprintf(stderr, "jfsh: fork failed: %s\n", errno_text(errno).c_str());
      return false;
    }
    saved.tees.push_back(pid);
    return true;
  };
  // A failed redirection drops the streams not yet handed to a tee.
  auto abandon = [&] {
    for (auto& [fd, m] : streams) {
      if (m.in >= 0) ::close(m.in);
      for (int o : m.outs) ::close(o);
    }
    return false;
  };
  auto report = [&](const str& what) {
    s::fprintf(stderr, "jfsh: %s: %s\n", errno_text(errno).c_str(), what.c_str());
    return abandon();
  };
  // Opening a fifo blocks until the other end is opened, and a signal
  // interrupts it.  As zsh does, the signal is handled and, for one
  // without a trap (a job ending), the open tried again; a trapped one,
  // Ctrl-C or an exit makes the redirection fail without a message.
  bool interrupted = false;
  auto retry = [&](auto open) {
    int fd;
    while ((fd = open()) < 0 and errno == EINTR) {
      bool trapped = mCtx.signals.pending_trapped() != 0;
      handle_signals();
      if (trapped or mCtx.exiting or mCtx.aborting) {
        interrupted = true;
        errno = EINTR;
        break;
      }
    }
    return fd;
  };
  auto failed_open = [&](const str& what) {
    return interrupted ? abandon() : report(what);
  };
  // Makes the new `fd` the command's `dst`; an output joins dst's stream.
  auto place = [&](int fd, int dst, bool output) {
    fd = move_high(fd);
    if (fd < 0) return report("dup");
    auto it = streams.find(dst);
    if (output and it != streams.end()) {
      Stream& m = it->second;
      if (m.in < 0) {
        int p[2];
        if (not pipe_high(p)) {
          ::close(fd);
          return report("pipe");
        }
        int cur = ::fcntl(dst, F_DUPFD_CLOEXEC, 10);
        if (cur >= 0) m.outs.push_back(cur);
        m.in = p[0];
        saved.save(dst);
        ::dup2(p[1], dst);
        ::close(p[1]);
      }
      m.outs.push_back(fd);
      return true;
    }
    if (not finish(dst)) {
      ::close(fd);
      return abandon();
    }
    streams.erase(dst);
    saved.save(dst);
    int rc = ::dup2(fd, dst);
    ::close(fd);
    if (rc < 0) return report("dup2");
    if (output and multios) streams[dst];
    return true;
  };
  // `N>&M` and `N<&M`: M, which must not be one of the shell's own fds,
  // is duplicated onto N.
  // The shell's own fds (zsh's FDT_INTERNAL): those it moved high, the
  // self-pipe, and its terminal.
  auto internal = [&](int fd) {
    const auto& sig = mCtx.signals;
    return is_shell_fd(fd) or fd == sig.wake_fd() or fd == sig.self_pipe_write_fd()
        or fd == sig.tty_fd() or (mCtx.zle and fd == mCtx.zle->tty_out);
  };
  auto dup_fd = [&](int src, int dst, bool output) {
    if (not finish(src)) return abandon();
    bool user = src >= 0 and not internal(src);
    errno = EBADF;
    int fd = user ? ::fcntl(src, F_DUPFD_CLOEXEC, 10) : -1;
    if (fd < 0) {
      s::fprintf(stderr, "jfsh: %d: %s\n", src, errno_text(errno).c_str());
      return abandon();
    }
    return place(fd, dst, output);
  };
  auto close_fd = [&](int dst) {
    if (not finish(dst)) return abandon();
    streams.erase(dst);
    saved.save(dst);
    ::close(dst);
    return true;
  };
  // `&>file`, `>&file`, `N>&file`: the file goes to N (1 by default) and
  // to 2.
  auto both = [&](const Redirection& r, int dst, bool append, bool force) {
    int fd = retry([&] { return open_output(mCtx.opts, r.target, append, force); });
    if (fd < 0) return failed_open(r.target);
    int fd2 = ::fcntl(fd, F_DUPFD_CLOEXEC, 10);
    if (fd2 < 0) {
      ::close(fd);
      return report("dup");
    }
    if (not place(fd, dst, true)) {
      ::close(fd2);
      return false;
    }
    return place(fd2, 2, true);
  };

  // `{name}>…`: a new fd of 10 or more, left open after the command, its
  // number in $name; `{name}>&-` closes the fd $name holds.
  auto with_varid = [&](const Redirection& r) {
    bool closing = r.type == REDIR_CLOSE
                or ((r.type == REDIR_MERGE or r.type == REDIR_IN_DUP) and r.target == "-");
    if (closing) {
      auto p = mCtx.params.lookup(r.varid);
      str v = p and p->get().is_set() ? p->get().scalar() : str{};
      auto fd = parse_fd_target(v);
      if (not fd) {
        s::fprintf(stderr, "jfsh: parameter %s does not contain a file descriptor\n",
                   r.varid.c_str());
        return abandon();
      }
      if (*fd >= 10 and internal(*fd)) {
        s::fprintf(stderr, "jfsh: file descriptor %d used by shell, not closed\n", *fd);
        return abandon();
      }
      // A close that fails is only reported (zsh's "failed to close").
      if (::close(*fd) != 0)
        s::fprintf(stderr, "jfsh: failed to close file descriptor %d: %s\n", *fd,
                   errno_text(errno).c_str());
      return true;
    }
    int fd = -1;
    switch (r.type) {
      case REDIR_READ:
        fd = retry([&] { return ::open(r.target.c_str(), O_RDONLY | O_NOCTTY); });
      break; case REDIR_READWRITE:
        fd = retry([&] { return ::open(r.target.c_str(), O_RDWR | O_CREAT | O_NOCTTY, 0666); });
      break; case REDIR_WRITE: case REDIR_CLOBBER: case REDIR_APPEND: case REDIR_APPEND_CLOBBER: {
        bool append = r.type == REDIR_APPEND or r.type == REDIR_APPEND_CLOBBER;
        bool force  = r.type == REDIR_CLOBBER or r.type == REDIR_APPEND_CLOBBER;
        fd = retry([&] { return open_output(mCtx.opts, r.target, append, force); });
      }
      break; case REDIR_MERGE: case REDIR_IN_DUP: {
        // `>&p`/`<&p` name the coprocess, and there is none (no coproc).
        if (r.target == "p") {
          s::fprintf(stderr, "jfsh: coprocess: bad file descriptor\n");
          return abandon();
        }
        auto src = parse_fd_target(r.target);
        if (not src) {
          s::fprintf(stderr, "jfsh: file number expected\n");
          return abandon();
        }
        fd = ::fcntl(*src, F_DUPFD, 10);
        if (fd < 0) {
          s::fprintf(stderr, "jfsh: %d: %s\n", *src, errno_text(errno).c_str());
          return abandon();
        }
      }
      break; case REDIR_HERESTR:
        fd = heredoc_body_fd(r.target + '\n');
      break; default:
        s::fprintf(stderr, "jfsh: redirection type %d not supported\n", static_cast<int>(r.type));
        return abandon();
    }
    if (fd < 0) return failed_open(r.target);
    int high = fd >= 10 ? fd : ::fcntl(fd, F_DUPFD, 10);
    if (high != fd) ::close(fd);
    if (high < 0) return report("dup");
    ::fcntl(high, F_SETFD, 0);
    forget_shell_fd(high);   // the user's now
    if (auto rr = mCtx.params.set_scalar(r.varid, s::to_string(high)); not rr) {
      ::close(high);
      s::fprintf(stderr, "jfsh: %s\n", rr.error().message.c_str());
      return abandon();
    }
    return true;
  };

  auto apply = [&](const Redirection& r) {
    if (not r.varid.empty()) return with_varid(r);
    int dst = r.fd >= 0 ? r.fd : default_fd(r.type);
    switch (r.type) {
      case REDIR_READ: {
        int fd = retry([&] { return ::open(r.target.c_str(), O_RDONLY | O_NOCTTY | O_CLOEXEC); });
        if (fd < 0) return failed_open(r.target);
        return place(fd, dst, false);
      }

      break; case REDIR_READWRITE: {
        int fd = retry([&] {
          return ::open(r.target.c_str(), O_RDWR | O_CREAT | O_NOCTTY | O_CLOEXEC, 0666);
        });
        if (fd < 0) return failed_open(r.target);
        return place(fd, dst, false);
      }

      break; case REDIR_WRITE: case REDIR_CLOBBER:
      case REDIR_APPEND: case REDIR_APPEND_CLOBBER: {
        bool append = r.type == REDIR_APPEND or r.type == REDIR_APPEND_CLOBBER;
        bool force  = r.type == REDIR_CLOBBER or r.type == REDIR_APPEND_CLOBBER;
        int fd = retry([&] { return open_output(mCtx.opts, r.target, append, force); });
        if (fd < 0) return failed_open(r.target);
        return place(fd, dst, true);
      }

      break; case REDIR_MERGEOUT:
        return both(r, dst, false, false);

      break; case REDIR_MERGEOUT_CLOBBER:
        return both(r, dst, false, true);

      break; case REDIR_APPEND_ERR:
        return both(r, dst, true, false);

      break; case REDIR_APPEND_ERR_CLOBBER:
        return both(r, dst, true, true);

      break; case REDIR_MERGE: case REDIR_IN_DUP: {
        bool output = r.type == REDIR_MERGE;
        if (r.target == "-") return close_fd(dst);
        // `>&p`/`<&p` name the coprocess; jfsh has no coproc yet.
        if (r.target == "p") {
          s::fprintf(stderr, "jfsh: coprocess: bad file descriptor\n");
          return abandon();
        }
        if (auto src = parse_fd_target(r.target)) return dup_fd(*src, dst, output);
        // `>&file` writes the file, as `&>file`.
        if (output) return both(r, dst, false, false);
        s::fprintf(stderr, "jfsh: file number expected\n");
        mCtx.aborting = true;
        return abandon();
      }

      break; case REDIR_CLOSE:
        return close_fd(dst);

      break; case REDIR_HEREDOC: case REDIR_HEREDOCD: case REDIR_HERESTR: {
        str body = r.target;
        if (r.type == REDIR_HERESTR) body += '\n';
        int fd = heredoc_body_fd(body);
        if (fd < 0) return report("here document");
        return place(fd, dst, false);
      }

      break; case REDIR_PROC_IN: {
        int fd = procsubst_in(r.target);
        if (fd < 0) return report("process substitution");
        return place(fd, dst, false);
      }

      break; case REDIR_PROC_OUT: {
        int fd = procsubst_out(r.target);
        if (fd < 0) return report("process substitution");
        return place(fd, dst, true);
      }

      break; default:
        s::fprintf(stderr, "jfsh: redirection type %d not supported\n",
                   static_cast<int>(r.type));
        return abandon();
    }
    return false;
  };
  for (const auto& r : redirs)
    if (not apply(r)) return false;
  for (const auto& r : stage_err)
    if (not apply(r)) return false;
  for (auto& [fd, m] : streams)
    if (not finish(fd)) return abandon();
  return not saved.fail;
}

str Executor::procsubst(char kind, strv body) {
  ProcSubstResources& res = mPsubst ? *mPsubst : mLooseSubst;
  if (kind == '=') {
    str path = procsubst_eq(body);
    if (not path.empty()) res.tmp_files.push_back(path);
    return path;
  }
  int fd = kind == '<' ? procsubst_in(body) : procsubst_out(body);
  if (fd < 0) return {};
  res.open_fds.push_back(fd);
  return "/dev/fd/" + s::to_string(fd);
}

void Executor::procsubst_release(ProcSubstResources& res) noexcept {
  for (int fd : res.open_fds) ::close(fd);
  for (const auto& p : res.tmp_files) ::unlink(p.c_str());
  res.open_fds.clear();
  res.tmp_files.clear();
}


// Word expansion for one simple command (see include/jfsh/exec/subst.h).
// A redirection target that expands to several words becomes several
// redirections of the same kind, which MULTIOS then fans out.
bool Executor::expand_command(vec<str>& args,
                              vec<Redirection>& redirs,
                              opt<int>&         subst_status,
                              bool&             fatal,
                              vec<s::size_t>*   counts) {
  Expander ex{*this, mCtx};
  auto fail = [&](const ShellError& e) {
    s::fprintf(stderr, "jfsh: %s\n", e.message.c_str());
    subst_status = ex.subst_status();
    // In a here-document or here-string even ${x?} fails just the command
    // (zsh 5.9), so the shell doesn't exit for it.
    if (not fatal and mCtx.exit_now) mCtx.exiting = mCtx.exit_now = false;
    return false;
  };
  // The arguments are expanded first, then the redirection targets; the
  // assignments come last, as they are made (apply_assigns).
  if (counts) {
    vec<str> all;
    for (const auto& a : args) {
      auto words = ex.words({a});
      if (not words) return fail(words.error());
      counts->push_back(words->size());
      all.insert(all.end(), s::make_move_iterator(words->begin()),
                 s::make_move_iterator(words->end()));
    }
    args = s::move(all);
  } else {
    auto words = ex.words(args);
    if (not words) return fail(words.error());
    args = s::move(*words);
  }

  vec<Redirection> out;
  out.reserve(redirs.size());
  for (auto& r : redirs) {
    switch (r.type) {
      case REDIR_HEREDOC: case REDIR_HEREDOCD:
        if (not r.literal) {
          auto body = ex.heredoc(r.target);
          if (not body) { fatal = false; return fail(body.error()); }
          r.target = s::move(*body);
        }
        out.push_back(s::move(r));
      break; case REDIR_HERESTR: case REDIR_MERGE: case REDIR_IN_DUP: {
        auto v = ex.scalar(r.target);
        if (not v) { fatal = r.type != REDIR_HERESTR; return fail(v.error()); }
        r.target = s::move(*v);
        out.push_back(s::move(r));
      }
      break; case REDIR_CLOSE: case REDIR_PROC_IN: case REDIR_PROC_OUT:
        out.push_back(s::move(r));
      break; default: {
        // Without MULTIOS the target is one word, as for a scalar: no
        // splitting, braces or globbing (`> a*` writes `a*`).
        if (not mCtx.opts.get(OPT_MULTIOS)) {
          auto v = ex.scalar(r.target);
          if (not v) return fail(v.error());
          r.target = s::move(*v);
          out.push_back(s::move(r));
          break;
        }
        auto targets = ex.words({r.target});
        if (not targets) return fail(targets.error());
        // No words at all is a file with an empty name, which fails to
        // open as zsh's does: the command fails, the line goes on.
        if (targets->empty()) targets->emplace_back();
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
  int last_base = -1;
  MathOptions mo = math_options(ctx);
  mo.last_base = &last_base;
  auto v = math_eval(a.value, ctx.params, mo, &ctx.modules.math_funcs());
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
  if (p.is_float()) {
    p.set(n.as_float());
  } else {
    p.set(n.as_int());
    // As zsh's setstrvalue: an integer with no base takes the constants'.
    if (p.base == 0 and last_base >= 2) p.base = last_base;
  }
  return {};
}

// `name[sub]=value` (or `+=`, or `=(…)` for an array slice): one element
// of an associative array — through the handlers of a special one such as
// $mapfile — or of an ordinary array, which grows to fit.  On a scalar the
// subscript picks a character.
static Result<void> assign_element(ExecContext& ctx, const Assignment& a) {
  const str& key = *a.subscript;
  auto raw = ctx.params.lookup_raw(a.name);
  if (raw and raw->get().is_hash()) {
    Param& p = raw->get();
    if (a.array or a.subscript_whole)
      return s::unexpected(ShellError{ErrorKind::ExecError,
          a.name + ": attempt to set slice of associative array"});
    if (p.handlers and p.handlers->readonly_elem and p.handlers->readonly_elem(key))
      return s::unexpected(ShellError{ErrorKind::ExecError, "read-only variable: " + key});
    if (p.handlers and p.handlers->set_elem) {
      str value = a.value;
      if (a.append and p.handlers->get_elem)
        value = p.handlers->get_elem(key).value_or("") + value;
      p.handlers->set_elem(key, s::move(value));
      return {};
    }
    if (p.is_readonly())
      return s::unexpected(ShellError{ErrorKind::ExecError,
          "read-only variable: " + a.name});
    if (auto* h = p.hash_ptr(); h and p.is_set()) {
      str& slot = (*h)[key];
      slot = a.append ? slot + a.value : a.value;
      return {};
    }
    auto h = p.hash();
    h[key] = a.append ? h[key] + a.value : a.value;
    p.set(s::move(h));
    return {};
  }
  if (auto p = ctx.params.lookup(a.name); p and p->get().is_readonly()) {
    return s::unexpected(ShellError{ErrorKind::ExecError,
        "read-only variable: " + a.name});
  } else if (a.subscript_whole) {
    // `name[@]=…` is the whole parameter, whatever its type.
    bool set = p and p->get().is_set();
    if (set and not p->get().is_array() and a.array)
      return s::unexpected(ShellError{ErrorKind::ExecError,
          a.name + ": attempt to assign array value to non-array"});
    Assignment whole = a;
    whole.subscript.reset();
    if (set and (p->get().is_integer() or p->get().is_float()))
      return set_numeric(ctx, p->get(), whole);
    if (set and not p->get().is_array())
      return ctx.params.set_scalar(a.name, a.value, a.append);
    return ctx.params.set_array(a.name, a.array ? a.elements : vec<str>{a.value}, a.append);
  } else if (p and p->get().is_set() and (p->get().is_integer() or p->get().is_float())) {
    // As in zsh, a number has no elements: the whole value is assigned,
    // once its subscript has been checked (`k=@; i[$k]=5` and `i[0]=5` fail).
    if (not is_subscript_flags(key)) {
      auto n = math_eval(key, ctx.params, math_options(ctx), &ctx.modules.math_funcs());
      if (not n) return s::unexpected(n.error());
      if (n->as_int() == 0 and not ctx.opts.get(OPT_KSH_ARRAYS)
          and not ctx.opts.get(OPT_KSH_ZERO_SUBSCRIPT))
        return s::unexpected(ShellError{ErrorKind::ExecError,
            a.name + ": assignment to invalid subscript range"});
    }
    if (a.array)
      return s::unexpected(ShellError{ErrorKind::ExecError,
          a.name + ": attempt to assign array value to non-array"});
    if (a.append)
      return s::unexpected(ShellError{ErrorKind::ExecError,
          "attempt to add to slice of a numeric variable"});
    Assignment whole = a;
    whole.subscript.reset();
    return set_numeric(ctx, p->get(), whole);
  }
  auto math = [&](strv text) -> Result<s::int64_t> {
    auto n = math_eval(text, ctx.params, math_options(ctx), &ctx.modules.math_funcs());
    if (not n) return s::unexpected(n.error());
    return n->as_int();
  };
  s::int64_t first, last;
  if (is_subscript_flags(key)) {
    // a[(r)pat]=…: the element a search finds, as ${a[(i)pat]} gives it.
    str flags = key.substr(1, key.find(')') - 1);
    for (char& c : flags) c = c == 'r' ? 'i' : c == 'R' ? 'I' : c;
    Expander ex{*ctx.executor, ctx};
    auto v = ex.scalar("${" + a.name + "[(" + flags + ")" + key.substr(key.find(')') + 1) + "]}");
    if (not v) return s::unexpected(v.error());
    first = last = s::strtoll(v->c_str(), nullptr, 10);
  } else {
    s::size_t comma = str::npos;
    for (s::size_t k = 0, depth = 0; k < key.size(); ++k) {
      if (key[k] == '(' or key[k] == '[') ++depth;
      else if ((key[k] == ')' or key[k] == ']') and depth > 0) --depth;
      else if (key[k] == ',' and depth == 0) { comma = k; break; }
    }
    // As zsh's getindex, an index from an expansion stops at its first
    // top-level comma (`i=2,3; a[$i]=x` sets a[2]); only a comma written
    // in the subscript makes a range (`a[2,3]=x`).
    bool range = comma != str::npos and a.subscript_comma;
    // A range with a bound left out (`a[2,]=x`) is bad arithmetic, as in
    // zsh, where an empty expression would otherwise be 0.
    auto blank = [](strv text) { return text.find_first_not_of(" \t\n") == strv::npos; };
    if (range and (blank(strv(key).substr(0, comma)) or blank(strv(key).substr(comma + 1))))
      if (auto whole = math(key); not whole) return s::unexpected(whole.error());
    auto lo = math(strv(key).substr(0, comma));
    if (not lo) return s::unexpected(lo.error());
    first = last = *lo;
    if (range) {
      auto hi = math(strv(key).substr(comma + 1));
      if (not hi) return s::unexpected(hi.error());
      last = *hi;
    }
  }
  // KSH_ARRAYS subscripts start at 0; KSH_ZERO_SUBSCRIPT takes a[0] for a[1].
  if (ctx.opts.get(OPT_KSH_ARRAYS)) {
    // Wrapping as zsh's does (INT64_MAX + 1 ends up an invalid 0), without
    // signed overflow.
    auto up = [](s::int64_t i) {
      return i >= 0 ? static_cast<s::int64_t>(static_cast<s::uint64_t>(i) + 1) : i;
    };
    first = up(first);
    last  = up(last);
  } else if (first == 0 and last == 0 and ctx.opts.get(OPT_KSH_ZERO_SUBSCRIPT)) {
    first = last = 1;
  }
  return ctx.params.set_slice(a.name, first, last,
                              a.array ? a.elements : vec<str>{a.value}, a.append, a.array,
                              ctx.opts.multibyte());
}

// One assignment, expanded: `name=value` on an integer or float is
// arithmetic, `name+=value` on an array appends an element, as in zsh.
static Result<void> assign_one(ExecContext& ctx, const Assignment& a) {
  // `2=x` sets a positional parameter (`0=x` sets $0).
  if (not a.name.empty() and s::isdigit(static_cast<unsigned char>(a.name[0]))) {
    if (a.array or a.subscript)
      return s::unexpected(ShellError{ErrorKind::ExecError,
                                      a.name + ": attempt to assign array value to non-array"});
    s::size_t n = s::strtoul(a.name.c_str(), nullptr, 10);
    if (n == 0) {
      ctx.arg0 = a.append ? ctx.arg0 + a.value : a.value;
      return {};
    }
    if (ctx.positional.size() < n) ctx.positional.resize(n);
    str& slot = ctx.positional[n - 1];
    slot = a.append ? slot + a.value : a.value;
    return {};
  }
  if (a.subscript) return assign_element(ctx, a);
  if (a.array) return ctx.params.set_array(a.name, a.elements, a.append);
  if (auto p = ctx.params.lookup(a.name); p and p->get().is_set()) {
    Param& old = p->get();
    if (old.is_integer() or old.is_float()) return set_numeric(ctx, old, a);
    if (a.append and old.is_array())
      return ctx.params.set_array(a.name, {a.value}, true);
  }
  return ctx.params.set_scalar(a.name, a.value, a.append);
}

// In front of a command, `name` is first copied into the command's own
// scope with the outer parameter's value, type and attributes (not
// readonly: an assignment to that fails), so assigning to the copy acts
// as on the original and the original comes back afterwards.
static Result<void> prefix_param(ParamTable& params, const str& name, bool exported) {
  auto outer = params.lookup(name);
  if (outer and outer->get().is_readonly())
    return s::unexpected(ShellError{ErrorKind::ExecError, "read-only variable: " + name});
  Param* p = nullptr;
  if (outer and params.in_current_scope(outer->get())) {
    p = &outer->get();   // made by an earlier assignment of this command
  } else if (outer) {
    const Param& o = outer->get();
    int level = o.scope;
    p = &params.assign(name, o.value,
                       o.flags & ~(PM_READONLY | PM_SPECIAL | PM_PRIVATE | PM_TIED));
    p->base = o.base;
    // It stands for the original, so `typeset -p` in a function says -g
    // just when it would for that one.
    p->scope = level;
  } else {
    p = &params.assign(name, str{}, PM_SCALAR | PM_UNSET);
    p->scope = 0;
  }
  if (exported) p->flags |= PM_EXPORTED;
  return {};
}

Result<void> Executor::assign_expanded(const Assignment& a) { return assign_one(mCtx, a); }

bool Executor::assign_var(str name, str value) {
  auto digit = [](char c) { return s::isdigit(static_cast<unsigned char>(c)) != 0; };
  // As zsh's isident: a positional parameter's number, or an identifier
  // whose letters may be multibyte.
  bool ok = not name.empty()
        and (s::all_of(name.begin(), name.end(), digit)
             or (not digit(name[0]) and s::all_of(name.begin(), name.end(), [](char c) {
                   return s::isalnum(static_cast<unsigned char>(c)) or c == '_'
                       or static_cast<unsigned char>(c) >= 0x80;
                 })));
  if (not ok) {
    s::fprintf(stderr, "jfsh: not an identifier: %s\n", name.c_str());
    mCtx.aborting = true;
    return false;
  }
  Assignment a;
  a.name  = s::move(name);
  a.value = s::move(value);
  if (auto r = assign_one(mCtx, a); not r) {
    s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
    mCtx.aborting = true;
    return false;
  }
  return true;
}

int Executor::apply_assigns(vec<Assignment>& assigns, AssignTo to,
                            opt<int>& subst_status) {
  Expander ex{*this, mCtx};
  int rc = 0;
  for (auto& a : assigns) {
    auto r = expand_assignment(ex, a);
    // `a[i]=x` in front of a command changes the parameter itself, as in
    // zsh (before an external command that is in the child).
    if (r and to != AssignTo::Shell and not a.subscript)
      r = prefix_param(mCtx.params, a.name, to == AssignTo::Exported);
    if (r) r = assign_one(mCtx, a);
    if (not r) {
      s::fprintf(stderr, "jfsh: %s\n", r.error().message.c_str());
      rc = 1;
      break;
    }
  }
  subst_status = ex.subst_status();
  return rc;
}

// Quotes a word for xtrace output only when it needs it, as zsh does: in
// '…', or in $'…' when it has control characters.
static str trace_quote(strv w) {
  constexpr strv kSafe =
      "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"
      "_@%+:,./-!";
  auto safe = [&](char c) {
    return static_cast<unsigned char>(c) >= 0x80 or kSafe.find(c) != strv::npos;
  };
  auto control = [](char c) {
    auto u = static_cast<unsigned char>(c);
    return u < 0x20 or u == 0x7f;
  };
  if (not w.empty() and s::all_of(w.begin(), w.end(), safe)) return str(w);
  if (s::any_of(w.begin(), w.end(), control)) {
    str out = "$'";
    for (char c : w) {
      if      (c == '\n')  out += "\\n";
      else if (c == '\t')  out += "\\t";
      else if (c == '\'')  out += "\\'";
      else if (c == '\\')  out += "\\\\";
      else if (c == 0x7f)  out += "\\C-?";
      else if (control(c)) out += str("\\C-") + static_cast<char>(c + '@');
      else                 out += c;
    }
    return out + "'";
  }
  str out = "'";
  for (char c : w) {
    if (c == '\'') out += "'\\''";
    else           out += c;
  }
  return out + "'";
}

// The assignments as xtrace shows them, each followed by a space.
static str trace_assigns(const vec<Assignment>& assigns) {
  str line;
  for (const auto& a : assigns) {
    str value = trace_quote(a.value);
    if (a.array) {
      value = "(";
      for (const auto& e : a.elements) value += ' ' + trace_quote(e);
      value += " )";
    }
    str name = a.subscript ? a.name + "[" + *a.subscript + "]" : a.name;
    line += name + (a.append ? "+=" : "=") + value + " ";
  }
  return line;
}

// xtrace goes to the shell's stderr from before the command's own
// redirections, as zsh's does.
static int trace_fd(const SavedFds& saved) {
  for (const auto& e : saved.entries)
    if (e.dst == STDERR_FILENO and e.backup >= 0) return e.backup;
  return STDERR_FILENO;
}

// PS4 goes through prompt expansion; zsh's initial value is `+%N:%i> `,
// and an unset PS4 is no prefix.
str Executor::trace_prompt() {
  auto ps4 = mCtx.params.lookup("PS4");
  return (ps4 and ps4->get().is_set()) ? expand_prompt(ps4->get().scalar()) : str{};
}

void Executor::trace_text(strv text, int fd) {
  write_trace(trace_prompt() + str(text), fd);
}

// Writes one xtrace line to `fd`, after anything stdio still holds for
// stderr.
static void write_trace(str line, int fd) {
  line += '\n';
  s::fflush(stderr);
  for (s::size_t done = 0; done < line.size();) {
    auto n = ::write(fd, line.data() + done, line.size() - done);
    if (n < 0 and errno == EINTR) continue;
    if (n <= 0) break;
    done += static_cast<s::size_t>(n);
  }
}

vec<str> Executor::builtin_names() const {
  vec<str> names;
  Builtins.for_each([&](const BuiltinDef& d) { names.emplace_back(d.name); });
  // And those of the loaded modules (zpty, ztcp, …).
  for (const BuiltinDef* d : mCtx.modules.all_builtins()) names.emplace_back(d->name);
  s::sort(names.begin(), names.end());
  names.erase(s::unique(names.begin(), names.end()), names.end());
  return names;
}

bool Executor::is_builtin(strv name) const noexcept {
  return Builtins.find(name).has_value() or mCtx.modules.find_builtin(name) != nullptr
      or mCtx.modules.autoload_for_builtin(name).has_value();
}

int Executor::call_builtin(strv name,
                           vec<str>& args,
                           ExecContext& ctx) {
  if (auto def = Builtins.find(name)) return def->get().fn(args, ctx);
  if (auto mod = ctx.modules.find_builtin(name) ? s::nullopt
                                                : ctx.modules.autoload_for_builtin(name))
    (void)ctx.modules.load(*mod, &ctx, true);
  if (const BuiltinDef* def = ctx.modules.find_builtin(name)) return def->fn(args, ctx);
  return 127;
}

// zsh's default limit on nested function calls, used when $FUNCNEST is
// unset or not a number.
static constexpr long DEFAULT_FUNC_NEST = 700;

int Executor::call_function(const ProgRef& body,
                            vec<str>& args,
                            bool* returned) {
  // $FUNCNEST is an integer parameter in zsh: its text is evaluated
  // (`FUNCNEST=abc` is 0).
  s::int64_t limit = DEFAULT_FUNC_NEST;
  if (auto p = mCtx.params.lookup("FUNCNEST"); p and p->get().is_set()) {
    if (p->get().is_integer()) {
      limit = p->get().integer();
    } else if (auto n = arith_eval(p->get().scalar(), mCtx.params, math_options(mCtx),
                                   &mCtx.modules.math_funcs())) {
      limit = *n;
    }
  }
  // As zsh's zerr, this ends the command line.
  if (limit >= 0 and mCtx.func_depth >= limit) {
    s::fprintf(stderr,
               "jfsh: %s: maximum nested function level reached; increase FUNCNEST?\n",
               args[0].c_str());
    mCtx.aborting = mCtx.abort_line = true;
    return 1;
  }

  FuncFrame frame{mCtx, args[0]};
  auto saved_positional = s::exchange(
      mCtx.positional, vec<str>(args.begin() + 1, args.end()));
  str saved_arg0 = mCtx.arg0;
  if (mCtx.opts.get(OPT_FUNCTIONARGZERO)) mCtx.arg0 = args[0];
  str saved_script = s::exchange(mCtx.script_name, args[0]);
  // $_ is put back once the function is done (zsh's doshfunc).
  str saved_underscore = mCtx.underscore;
  OptionState saved_opts  = mCtx.opts;
  mCtx.signals.push_trap_frame();
  int saved_noerrreturn = s::exchange(mCtx.noerrreturn, 0);
  // getopts starts again in the function, as zsh's does.
  bool own_optind = not mCtx.opts.get(OPT_POSIXBUILTINS);
  auto saved_optind  = own_optind ? s::exchange(mCtx.optind, 1) : mCtx.optind;
  auto saved_optcind = own_optind ? s::exchange(mCtx.optcind, 0) : mCtx.optcind;
  ++mCtx.func_depth;
  mCtx.params.push_scope();
  for (const auto& [_, hook] : mCtx.hooks.function) hook(args[0], true);

  // An empty body runs no commands.
  if (not body->empty() and wc_code(body->prog[0]) != WC_END) ++mCtx.code_runs;
  int rc = exec(*body);
  // A ksh-style autoload body, as zsh's execautofn: the file has run in
  // this frame; the function it defined runs next, in it too, unless the
  // file returned or the line is ending.  A file that didn't define it
  // leaves the function to be loaded again, status 1.
  if (body->run_after) {
    auto f = mCtx.functions.find(args[0]);
    if (f != mCtx.functions.end() and f->second != body) {
      mCtx.autoloads.erase(args[0]);
      if (not mCtx.returning and not interrupted() and not mCtx.exiting) {
        ProgRef next = f->second;
        rc = exec(*next);
      }
    } else {
      if (f != mCtx.functions.end()) mCtx.functions.erase(f);
      // (zsh's zwarn says nothing once errflag is set, and ERR_EXIT has
      // left already.)
      if (not mCtx.aborting and not mCtx.exit_now)
        s::fprintf(stderr, "jfsh: %s: function not defined by file\n", args[0].c_str());
      mCtx.returning = false;
      rc = mCtx.last_exit = 1;
    }
  }
  bool did_return = mCtx.returning;
  if (mCtx.returning) {
    mCtx.returning = mCtx.return_line = false;
    rc = mCtx.return_val;
  }

  // LOCAL_TRAPS as it stands when the function returns decides, as does
  // LOCAL_OPTIONS.  An EXIT trap set in the function runs now.  Under
  // POSIX_TRAPS (M12 Phase 12c), the frame is always isolated: traps set
  // in the function stay in the function, matching POSIX.
  vec<s::pair<int, TrapAction>> restored;
  auto exit_trap = mCtx.signals.pop_trap_frame(
      mCtx.opts.get(OPT_LOCALTRAPS) or mCtx.opts.get(OPT_POSIXTRAPS), &restored);
  // The TRAPxxx functions follow the traps put back, as in zsh: one the
  // function hid (`trap - EXIT`) comes back, one it defined goes
  // (`TRAPEXIT() {…}` in a function is that function's EXIT trap).
  for (const auto& [sig, was] : restored) {
    const Trap& t = mCtx.signals.traps().get(sig);
    str name = str("TRAP") + SignalTable::sig_to_name(sig);
    if (t.action == TrapAction::Function and t.prog) mCtx.functions[name] = t.prog;
    else if (was == TrapAction::Function)         mCtx.functions.erase(name);
  }
  // It runs on an `exit` in the function too, which then goes on; there,
  // as in zsh, the trap sees $? 0.
  // Not inside a trap, though: zsh runs no EXIT trap there.
  if (exit_trap and mTrapDepth == 0
      and (exit_trap->action == TrapAction::Command
           or (exit_trap->action == TrapAction::Function and exit_trap->prog))) {
    int  saved = mCtx.last_exit;
    auto exit = s::tuple{mCtx.exiting, mCtx.exit_val, mCtx.exit_now};
    if (mCtx.exiting) mCtx.last_exit = 0;
    mCtx.exiting = mCtx.exit_now = false;
    {
      TrapErrorScope scope{mCtx};
      ++mTrapDepth;   // a function it calls runs no EXIT trap of its own
      if (exit_trap->action == TrapAction::Command) {
        run_trap_command(*exit_trap);
      } else {
        // A TRAPEXIT that ends with `return N` acts as `return N` in the
        // trap's command would.
        vec<str> fargs{"TRAPEXIT", "0"};
        bool trap_returned = false;
        int  trap_rc = call_function(exit_trap->prog, fargs, &trap_returned);
        if (trap_returned and trap_rc != 0) {
          mCtx.returning  = true;
          mCtx.return_val = trap_rc;
        }
      }
      --mTrapDepth;
    }
    if (not mCtx.exiting) {
      s::tie(mCtx.exiting, mCtx.exit_val, mCtx.exit_now) = exit;
      if (not mCtx.exiting) mCtx.last_exit = saved;
    }
  }
  for (const auto& [_, hook] : mCtx.hooks.function) hook(args[0], false);
  mCtx.params.pop_scope();
  --mCtx.func_depth;
  // An `exit` that has unwound the functions leaves at once from here, as
  // zsh's does: always blocks outside them don't run.
  if (mCtx.exiting and mCtx.func_depth == 0) mCtx.exit_now = true;
  mCtx.noerrreturn = saved_noerrreturn;
  if (own_optind) {
    mCtx.optind  = saved_optind;
    mCtx.optcind = saved_optcind;
  }
  if (mCtx.opts.get(OPT_LOCALOPTIONS)) mCtx.opts = saved_opts;
  mCtx.arg0       = s::move(saved_arg0);
  mCtx.script_name = s::move(saved_script);
  mCtx.underscore  = s::move(saved_underscore);
  mCtx.positional = s::move(saved_positional);
  // An `exit` in a function the EXIT trap called gives the final status
  // but acts as `return` from that function, with $? 0, as zsh's zexit
  // while the shell is exiting (shell_exiting == -1).
  if (mInExitTrap and mCtx.exiting and mCtx.func_depth >= mExitTrapFuncDepth
      and not mExitEndsExitTrap) {
    mExitFromTrap = mCtx.exit_val;
    mCtx.exiting  = mCtx.exit_now = false;
    rc = 0;
    // In a signal trap the EXIT trap brought on, after a -c string ran off
    // its end, it ends the EXIT trap once that trap is done (run_trap).
    if (mExitTrapRanOff and mSigTrapInExit) mExitInSigTrap = true;
  }
  if (returned) *returned = did_return;
  return rc;
}

static bool is_executable_file(const str& path) {
  struct stat st{};
  return stat(path.c_str(), &st) == 0 and S_ISREG(st.st_mode)
     and access(path.c_str(), X_OK) == 0;
}

vec<str> path_dirs(const ParamTable& params) {
  vec<str> dirs;
  auto p = params.lookup("PATH");
  if (not p or not p->get().is_set()) return dirs;
  str all = p->get().scalar();
  s::size_t start = 0;
  for (;;) {
    s::size_t end = all.find(':', start);
    if (end == str::npos) end = all.size();
    dirs.push_back(all.substr(start, end - start));
    if (end == all.size()) return dirs;
    start = end + 1;
  }
}

// Searches the directories `path_var` names for an executable `name`.  An
// empty or `.` directory runs `name` as it is, as zsh does; only those
// found through an absolute directory go into `hash`.  An entry of that
// name that can't be run (a directory, a file without x) makes it
// PermissionDenied rather than FileNotFound.
static Result<str> search_path(strv name, const vec<str>& dirs, HashTable<str>* hash) {
  bool denied = false;
  for (const auto& dir : dirs) {
    str candidate = (dir.empty() or dir == ".") ? str(name) : dir + "/" + str(name);
    if (is_executable_file(candidate)) {
      if (hash and dir[0] == '/') (*hash)[str(name)] = candidate;
      return candidate;
    }
    struct stat st{};
    if (stat(candidate.c_str(), &st) == 0) denied = true;
  }
  if (denied)
    return s::unexpected(ShellError{ErrorKind::PermissionDenied,
        "permission denied: " + str(name)});
  return s::unexpected(ShellError{ErrorKind::FileNotFound,
      "command not found: " + str(name)});
}

Result<str> Executor::find_command(strv name) {
  if (name.find('/') != strv::npos) return str(name);
  // Entries are used even under NO_HASH_CMDS (`hash name=path`); the
  // option only stops the search adding them.
  auto& hash = command_hash();
  if (auto it = hash.find(str(name)); it != hash.end() and is_executable_file(it->second))
    return it->second;
  bool hashing = mCtx.opts.get(OPT_HASHCMDS);
  return search_path(name, path_dirs(mCtx.params), hashing ? &hash : nullptr);
}

static vec<char*> c_strings(vec<str>& v) {
  vec<char*> out;
  out.reserve(v.size() + 1);
  for (auto& s : v) out.push_back(s.data());
  out.push_back(nullptr);
  return out;
}

// strerror() as zsh's %e prints it: lowercased unless it starts with an
// acronym.
static str errno_text(int err) {
  str text = s::strerror(err);
  if (text.size() > 1 and not s::isupper(static_cast<unsigned char>(text[1])))
    text[0] = static_cast<char>(s::tolower(static_cast<unsigned char>(text[0])));
  return text;
}

// Runs `path` in place of this process, as zsh's zexecve does: a file that
// is neither a binary (no NUL in its first bytes) nor a `#!` script runs
// under /bin/sh, and a `#!` line naming a missing interpreter says so.
// Returns only on failure, after the message, with the status: 126 for a
// file that can't be run, 127 for anything else.
static int exec_program(const str& path, vec<str> argv, vec<str>& env,
                        const str& name) {
  auto av = c_strings(argv);
  auto ev = c_strings(env);
  execve(path.c_str(), av.data(), ev.data());
  int err = errno;
  if (err == ENOEXEC or err == ENOENT) {
    char buf[128];
    ssize_t n = -1;
    if (int fd = ::open(path.c_str(), O_RDONLY | O_NOCTTY); fd >= 0) {
      n = ::read(fd, buf, sizeof buf);
      ::close(fd);
    }
    strv head(buf, n > 0 ? static_cast<s::size_t>(n) : 0);
    if (head.starts_with("#!")) {
      if (err == ENOENT) {
        // The interpreter is the first word of a whole line, as zsh has it.
        strv interp = head.substr(2);
        if (auto nl = interp.find('\n'); nl != strv::npos) {
          interp = interp.substr(0, nl);
          interp.remove_prefix(s::min(interp.find_first_not_of(' '), interp.size()));
          interp = interp.substr(0, interp.find_first_of(" \t"));
        }
        s::fprintf(stderr, "jfsh: %s: bad interpreter: %.*s: %s\n", path.c_str(),
                   static_cast<int>(interp.size()), interp.data(),
                   errno_text(err).c_str());
        return 127;
      }
    } else if (err == ENOEXEC and n >= 0 and head.find('\0') == strv::npos) {
      vec<str> sh{"sh", path};
      sh.insert(sh.end(), argv.begin() + 1, argv.end());
      auto sh_argv = c_strings(sh);
      execve("/bin/sh", sh_argv.data(), ev.data());
    }
  }
  s::fprintf(stderr, "jfsh: %s: %s\n", errno_text(err).c_str(), name.c_str());
  return (err == EACCES or err == ENOEXEC) ? 126 : 127;
}

// zsh's zexecve: the program finds its own path in $_ (absolute, from $PWD
// when it is relative).
static void put_underscore(vec<str>& env, const str& path) {
  str value = path;
  if (not path.starts_with('/')) {
    auto pwd = s::find_if(env.begin(), env.end(),
                          [](const str& e) { return e.starts_with("PWD="); });
    value = (pwd != env.end() ? pwd->substr(4) : str(".")) + "/" + path;
  }
  s::erase_if(env, [](const str& e) { return e.starts_with("_="); });
  env.push_back("_=" + value);
}

static bool assigns_path(const vec<Assignment>& assigns) {
  return s::any_of(assigns.begin(), assigns.end(),
                   [](const Assignment& a) { return a.name == "PATH"; });
}

// The shell itself (not a subshell) is about to become another program:
// one level fewer in $SHLVL, and before an `exec` the interactive shell's
// history is saved, as zsh's execute does.
void Executor::leave_shell(bool explicit_exec) {
  if (::getpid() != mCtx.shell_pid) return;
  if (auto p = mCtx.params.lookup("SHLVL"); p and p->get().is_set()) {
    s::int64_t lvl = s::strtoll(p->get().scalar().c_str(), nullptr, 10);
    (void)mCtx.params.set_scalar("SHLVL", s::to_string(lvl - 1));
  }
  if (explicit_exec and mCtx.loop.save_history) mCtx.loop.save_history();
}

int Executor::run_external(const vec<str>& args,
                           const vec<Assignment>&  assigns,
                           const vec<Redirection>& redirs,
                           ExecFlags flags,
                           const str* traced) {
  // The shell looks the command up (and hashes it) in its own $PATH; with
  // a PATH= in front the child searches that one instead.
  opt<Result<str>> found = find_command(args[0]);
  if (assigns_path(assigns)) found.reset();

  // Builtins write through stdio; flush so their output precedes the child's.
  s::fflush(nullptr);

  // EF_NOFORK: last command in `-c` string or last stage in a subshell runs
  // on this process directly, unless an `=(…)` file has to be removed once
  // it is done.
  bool tmp_files = mPsubst and not mPsubst->tmp_files.empty();
  if ((flags & EF_NOFORK) and not tmp_files) {
    leave_shell(false);
    exec_external_child(args, assigns, redirs, s::move(found), traced, flags);
  }

  pid_t pid = fork_command(flags, ChildKind::Other);
  if (pid < 0) return 1;
  if (pid == 0) exec_external_child(args, assigns, redirs, s::move(found), traced, flags);
  return wait_command(pid);
}

// As the last stage of a pipeline, fork_child puts the child in the
// pipeline's process group (ExecContext::pipeline_pgid).
pid_t Executor::fork_command(ExecFlags flags, ChildKind kind) {
  PipeLast* last = (flags & EF_PIPELAST) ? mPipeLast : nullptr;
  pid_t pid = fork_child(0, true, kind);
  if (pid > 0 and last) last->pid = pid;
  return pid;
}

int Executor::wait_command(pid_t pid) {
  if (mPipeLast and mPipeLast->pid == pid) return 0;
  if (job_control()) {
    Pid pgid = mChildPgid > 0 ? mChildPgid : pid;
    int jid = mCtx.jobs.alloc(pgid, JOB_PGRP, current_job_text(), false);
    Process p;
    p.pid = pid;
    mCtx.jobs.add_process(jid, s::move(p));
    return wait_foreground_job(mCtx, jid);
  }
  int status = 0;
  for (;;) {
    mCtx.signals.let_kept_through(true);
    pid_t got = ::waitpid(pid, &status, 0);
    mCtx.signals.let_kept_through(false);
    if (got >= 0) break;
    if (errno != EINTR) {
      s::fprintf(stderr, "jfsh: waitpid: %s\n", s::strerror(errno));
      return 1;
    }
    // Hung up: as zsh, the shell goes at once, leaving the command be.
    if (mCtx.signals.pending(SIGHUP) and not mCtx.signals.traps().trapped(SIGHUP)
        and mCtx.signals.traps().get(SIGHUP).action != TrapAction::Ignore)
      return 128 + SIGHUP;
  }
  drop_trap_signals();
  if (WIFEXITED(status))   return WEXITSTATUS(status);
  if (WIFSIGNALED(status)) return 128 + WTERMSIG(status);
  return 1;
}

// As in zsh, the child applies the redirections, then makes the
// assignments, which go into its environment (an array is not exported),
// then reports a command it can't find.  The redirections' saved fds are
// close-on-exec, and never restored.
void Executor::exec_external_child(const vec<str>&         args,
                                   vec<Assignment>         assigns,
                                   const vec<Redirection>& redirs,
                                   opt<Result<str>>        found,
                                   const str*              traced,
                                   ExecFlags               flags) {
  SavedFds saved;
  if (not redirs.empty() and not apply_redirects(redirs, saved, flags)) _exit(1);
  if (not assigns.empty()) {
    mCtx.params.push_scope(/*prefix=*/true);
    str ps4 = traced ? trace_prompt() : str{};
    opt<int> subst_status;
    int rc = apply_assigns(assigns, AssignTo::Exported, subst_status);
    if (traced) write_trace(ps4 + trace_assigns(assigns) + *traced, trace_fd(saved));
    if (rc != 0) _exit(rc);
  }
  if (not found) found = find_command(args[0]);
  if (not *found) {
    s::fprintf(stderr, "jfsh: %s\n", found->error().message.c_str());
    _exit(found->error().kind == ErrorKind::PermissionDenied ? 126 : 127);
  }
  vec<str> env = mCtx.params.make_envp();
  put_underscore(env, **found);
  _exit(exec_program(**found, args, env, args[0]));
}

// ---- exec, source, autoload, the command hash --------------------------------

int Executor::exec_replace(const vec<str>& args, vec<Assignment> assigns,
                           const str& argv0, bool clear_env, const str* traced,
                           int trace_to) {
  bool scoped = not assigns.empty();
  auto fail = [&](int rc) {
    if (scoped) mCtx.params.pop_scope();
    // A shell that can't exec goes on only when interactive.
    if (not mCtx.opts.interactive()) {
      mCtx.exiting  = true;
      mCtx.exit_now = true;
      mCtx.exit_val = rc;
    }
    return rc;
  };
  if (scoped) {
    mCtx.params.push_scope(/*prefix=*/true);
    str ps4 = traced ? trace_prompt() : str{};
    opt<int> subst_status;
    int rc = apply_assigns(assigns, AssignTo::Exported, subst_status);
    if (traced) write_trace(ps4 + trace_assigns(assigns) + *traced, trace_to);
    if (rc != 0) return fail(rc);
  }
  auto path = find_command(args[0]);
  if (not path) {
    s::fprintf(stderr, "jfsh: %s\n", path.error().message.c_str());
    return fail(path.error().kind == ErrorKind::PermissionDenied ? 126 : 127);
  }
  leave_shell(true);
  // `exec -c`: an empty environment, without even the assignments.
  vec<str> env = clear_env ? vec<str>{} : mCtx.params.make_envp();
  if (not clear_env) put_underscore(env, *path);
  vec<str> argv_s = args;
  if (argv0 == "-")        argv_s[0] = "-" + argv_s[0];
  else if (not argv0.empty()) argv_s[0] = argv0;
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
  int rc = exec_program(*path, s::move(argv_s), env, args[0]);
  for (auto& [sig, old] : changed) ::sigaction(sig, &old, nullptr);
  return fail(rc);
}


int Executor::source_file(const str& path, const vec<str>& args, const str& name,
                          bool startup) {
  auto text = read_file(path);
  // A directory reads as nothing, as zsh's source() finds it.
  if (not text and errno == EISDIR) text = str{};
  // One that can't be opened is zsh's SOURCE_NOT_FOUND: a startup file is
  // skipped, `.` says why (errno is left for it).
  if (not text) return startup ? 0 : SOURCE_NOT_FOUND;
  vec<str> saved_positional;
  bool     set_args = not args.empty();
  if (set_args) saved_positional = s::exchange(mCtx.positional, args);
  FuncFrame frame{mCtx, name};
  // `.` names $0 after the file under FUNCTION_ARGZERO and puts it back
  // after (zsh's bin_dot); otherwise, and for a startup file, $0 is left
  // alone, also when the file assigns it.
  opt<str> saved_arg0;
  if (not startup and mCtx.opts.get(OPT_FUNCTIONARGZERO))
    saved_arg0 = s::exchange(mCtx.arg0, name);
  str saved_source = s::exchange(mCtx.source_name, path);
  str saved_script = s::exchange(mCtx.script_name, path);
  // The file is not inside the caller's loops, as zsh's source sets
  // `loops` to 0: a `break` in it is an error.
  int saved_loops = s::exchange(mCtx.loop_depth, 0);
  s::uint32_t saved_lineno = mCtx.lineno;
  // Commands come from the file, not standard input (zsh's source()).
  bool saved_shinstdin = mCtx.opts.get(OPT_SHINSTDIN);
  mCtx.opts.assign(OPT_SHINSTDIN, false);
  mCtx.interrupt = false;
  mParseFailed   = false;
  ++mCtx.source_depth;
  ++mCtx.code_runs;
  int rc;
  {
    // A file read with no command around it (a startup file) is still not
    // the shell's top level: an error ends the file, not the shell.
    opt<StringDepth> outer;
    if (mStringDepth == 0) outer.emplace(*this);
    rc = exec_string(*text, /*by_line=*/true);
  }
  --mCtx.source_depth;
  mCtx.opts.assign(OPT_SHINSTDIN, saved_shinstdin);
  // A parse error in the file gives `.` 126, as in zsh's bin_dot; a
  // startup file leaves the status as the error left it.
  if (s::exchange(mParseFailed, false))
    rc = mCtx.last_exit = startup ? mStatusBeforeParseError : 126;
  mCtx.loop_depth = saved_loops;
  // (Not after an `exit` that leaves at once: the EXIT trap sees its line.)
  if (not (mCtx.exiting and mCtx.exit_now)) mCtx.lineno = saved_lineno;
  // `return` in the file ends the file.
  if (mCtx.returning) {
    mCtx.returning = false;
    rc = mCtx.return_val;
  }
  // An error ends the file but not the caller, which sees status 126, as
  // in zsh.  An interrupt goes on unwinding, and under ERR_EXIT so does
  // the error (the shell exits with its status, 1).
  if (mCtx.aborting and not mCtx.interrupt
      and not (mCtx.opts.errexit() and not mCtx.in_startup)) {
    mCtx.aborting = mCtx.abort_line = false;
    mCtx.end_trap_abort();
    rc = startup ? mCtx.last_exit : (mCtx.last_exit = 126);
  }
  mCtx.source_name = s::move(saved_source);
  mCtx.script_name = s::move(saved_script);
  if (saved_arg0) mCtx.arg0 = s::move(*saved_arg0);
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
    // The flags autoload gave it (zsh's loadautofn): -U parses without
    // aliases, -k and -z pick ksh or zsh style over KSH_AUTOLOAD.
    str flags;
    if (auto f = mCtx.autoloads.find(name); f != mCtx.autoloads.end()) flags = f->second;
    auto has = [&](char c) { return flags.find(c) != str::npos; };
    StringInputSource in{*text};
    Lexer lex{in};
    lex.push_state(LEX_SKIPCOMMENTS);
    Parser parser{lex};
    if (not has('U') and mCtx.opts.get(OPT_ALIASES)) parser.set_aliases(&mCtx.aliases);
    auto prog = parser.parse();
    if (not prog) {
      s::fprintf(stderr, "jfsh: %s: %s\n", name.c_str(), prog.error().message.c_str());
      return false;
    }
    // A file that is just `name() { … }` defines the function when run, as
    // does any file in ksh style; any other file is the function's body.
    const auto& code = (*prog)->prog;
    bool ksh_style = has('k') or (mCtx.opts.get(OPT_KSH_AUTOLOAD) and not has('z'));
    if (not ksh_style and code.size() > 4 and wc_code(code[3]) == WC_FUNCDEF
        and wc_cmd_type(code[3]) == WC_FUNCDEF_NAMED) {
      s::size_t after = 4 + wc_cmd_skip(code[3]);
      bool only = after + 1 == code.size() and wc_code(code[after]) == WC_END
              and wc_data(code[4]) == 1
              and (*prog)->strs[wc_data(code[5])] == name;
      ksh_style = only;
    }
    // Ksh style: the file is the body, run in the call's frame, which then
    // runs what it defined (call_function); the autoload stays until it
    // has (zsh's execautofn).
    mCtx.functions[name] = *prog;
    if (ksh_style) {
      (*prog)->run_after = true;
    } else {
      mCtx.autoloads.erase(name);
      mCtx.function_files[name] = path;
    }
    return true;
  }
  s::fprintf(stderr, "jfsh: %s: function definition file not found\n", name.c_str());
  return false;
}

// Dropped on any assignment to $PATH (path_generation), or when its text
// changed some other way.
HashTable<str>& Executor::command_hash() {
  auto p = mCtx.params.lookup("PATH");
  const str* text = p and p->get().is_set() ? s::get_if<str>(&p->get().value) : nullptr;
  strv path_var = text ? strv(*text) : strv{};
  s::uint64_t gen = mCtx.params.path_generation();
  if (gen != mHashedGen or path_var != mHashedPath) {
    mCmdHash.clear();
    mHashFull   = false;
    mHashedPath = path_var;
    mHashedGen  = gen;
  }
  return mCmdHash;
}

void Executor::fill_command_hash() {
  auto& hash = command_hash();
  hash.clear();
  // The first directory in $PATH with a name wins, as for a search.
  for (const auto& dir : path_dirs(mCtx.params)) {
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
  mHashFull = true;
}

// ---- Job control and signals -----------------------------------------------

void Executor::enter_child(ChildKind kind) {
  sigset_t keep = mCtx.signals.child_reset(kind, mCtx.opts.get(OPT_POSIXTRAPS));
  mCtx.jobs.clear();
  mExitTrapRan = false;
  // The parent's running traps are not the child's: a trap it sets runs.
  if (kind != ChildKind::Stage) mInTrap = {};
  mQueued  = {};
  mBacklog = {};
  mExitFromTrap  = -1;
  mTryEndedChild = false;
  // fork_child's block ends; only what child_reset keeps stays blocked.
  ::sigprocmask(SIG_SETMASK, &keep, nullptr);
}

void Executor::exit_child(int rc) {
  if (mCtx.exiting)        rc = mCtx.exit_val;
  else if (mCtx.returning) rc = mCtx.return_val;
  else if (mCtx.aborting)  rc = mCtx.last_exit;
  // An EXIT trap the child set runs as it ends, unless an error ended it
  // (`$((1/0))`, `${x?}`): zsh's errflag is still set, and dotrapargs runs
  // no trap then.
  // (An error an `always` block ended the child with is cleared, as zsh
  // runs that last command in exec mode.)
  if ((mCtx.aborting and not mCtx.exiting and not mTryEndedChild) or mCtx.hard_error)
    mExitTrapRan = true;
  // An `exit` in a function that the parent's EXIT trap ran here gives the
  // status, as zsh's exit_pending.
  if (mExitFromTrap >= 0) rc = mExitFromTrap;
  rc = run_exit_trap(rc);
  s::fflush(nullptr);
  _exit(rc);
}

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
  // In the last stage of a pipeline that the shell runs itself, a
  // foreground job joins the pipeline's group, which has the terminal.
  if (jc and pgid == 0 and foreground) pgid = mCtx.pipeline_pgid;
  if (pid == 0) {
    if (jc) {
      // Both sides set the group, so neither can run ahead of the other.
      // A group whose processes are all gone can't be joined: then the
      // child starts one of its own.
      if (::setpgid(0, pgid) != 0) ::setpgid(0, 0);
      if (foreground) mCtx.signals.give_terminal(::getpgrp());
    }
    enter_child(kind);   // which unblocks every signal
    mCtx.pipeline_pgid = 0;
    mCtx.pipeline_jid  = 0;
    return 0;
  }
  mChildPgid = 0;
  if (jc) {
    if (::setpgid(pid, pgid ? pgid : pid) != 0 and pgid) ::setpgid(pid, pid);
    mChildPgid = ::getpgid(pid);
    if (mChildPgid <= 0) mChildPgid = pgid ? pgid : pid;
  }
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
  h.script_name = [this] { return mCtx.script_name.empty() ? mCtx.arg0 : mCtx.script_name; };
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
  SignalCounts counts = mCtx.signals.take_pending();
  auto has = [&](int sig) { return counts[static_cast<s::size_t>(sig)] > 0; };
  if (has(SIGCHLD)) {
    mCtx.signals.reap(mCtx.jobs);
    // Without MONITOR nothing reports a finished background job: zsh
    // forgets it at once (`jobs` doesn't list it, `wait %1` can't name it).
    if (not mCtx.opts.monitor()) mCtx.jobs.forget_finished_async();
    continue_pipeline_stages();
  }
  if (has(SIGWINCH)) update_window_size();
  for (int sig = 1; sig <= MAX_SIG; ++sig) {
    auto n = counts[static_cast<s::size_t>(sig)];
    if (n == 0) continue;
    // (SIGINT whose trap did `trap - INT` acts at once, as zsh's handler.)
    if (mCtx.signals.running_trap(sig)
        and (mCtx.signals.traps().trapped(sig) or sig != SIGINT
             or not mCtx.signals.shell_handles(sig))) {
      // Its trap is running: as zsh's signal queue, the signal waits for
      // the trap to end (one that came while the trap waited for a
      // command was dropped there, drop_trap_signals).
      auto& q = mQueued[static_cast<s::size_t>(sig)];
      q = static_cast<s::uint16_t>(q + n);
    } else if (mCtx.signals.traps().trapped(sig)) {
      // Each arrival runs the trap, one after the other, as zsh queues
      // them; those still queued when the trap waits for a command (or in
      // `wait`) are lost there (drop_trap_signals).
      mBacklog[static_cast<s::size_t>(sig)] = static_cast<s::uint16_t>(n - 1);
      run_trap(sig);
    } else if (mCtx.signals.shell_handles(sig)) {
      untrapped_signal(sig);
    }
  }
}

bool Executor::run_debug_trap() {
  // ERR_EXIT is off while the DEBUG trap runs, as zsh's execlist has it.
  bool errexit = mCtx.opts.get(OPT_ERREXIT);
  mCtx.opts.assign(OPT_ERREXIT, false);
  run_trap(SIGDEBUG);
  bool skip = mCtx.opts.get(OPT_ERREXIT);
  mCtx.opts.assign(OPT_ERREXIT, errexit);
  return skip;
}

void Executor::untrapped_signal(int sig) {
  if (sig == SIGINT) {
    // zsh's errflag |= ERRFLAG_INT and lastval = 128 + SIGINT: the command
    // line ends.
    mCtx.aborting = mCtx.interrupt = true;
    mCtx.last_exit = 128 + SIGINT;
    if (not mCtx.signals.running_trap(sig)) mCtx.intr_echoed = true;
  } else if (sig == SIGHUP) {
    // zsh's zexit(SIGHUP, …): the status is the signal's number, 1.
    mCtx.hangup   = true;
    mCtx.exiting  = true;
    mCtx.exit_now = false;
    mCtx.exit_val = SIGHUP;
  }
}

void Executor::drop_trap_signals(bool backlog) {
  // Arrived while a trap for them ran (or, with `backlog`, queued before
  // it): zsh's queue is processed where the shell waits for a child, and
  // there their trap is ZSIG_IGNORED, so they are lost; with the trap gone
  // meanwhile (`trap - SIG`), the default acts at once.
  for (int sig = 1; sig <= MAX_SIG; ++sig) {
    if (not mCtx.signals.running_trap(sig)) continue;
    mQueued[static_cast<s::size_t>(sig)] = 0;
    if (backlog) mBacklog[static_cast<s::size_t>(sig)] = 0;
    if (not mCtx.signals.pending(sig)) continue;
    TrapAction now = mCtx.signals.traps().get(sig).action;
    if (now == TrapAction::Command or now == TrapAction::Function or now == TrapAction::Ignore) {
      mCtx.signals.clear_pending(sig);
    } else if (mCtx.signals.shell_handles(sig)) {
      mCtx.signals.clear_pending(sig);
      untrapped_signal(sig);
    } else {
      mCtx.signals.raise_default(sig);
    }
  }
}

void Executor::continue_pipeline_stages() {
  if (not mCtx.pipeline_jid or mCtx.aborting) return;
  auto found = mCtx.jobs.get(mCtx.pipeline_jid);
  if (not found) return;
  Job& j = found->get();
  if (s::none_of(j.procs.begin(), j.procs.end(), [](const Process& p) { return p.stopped(); }))
    return;
  s::fprintf(stderr, "jfsh: job can't be suspended\n");
  s::fflush(stderr);
  (void)SignalManager::kill_job(j.pgid, SIGCONT);
  j.continued();
  j.flags &= ~JOB_CHANGED;
}

void Executor::wait_for_input() {
  mCtx.signals.dispatch_pending(mCtx.jobs);   // drains the self-pipe
  handle_signals();
}

void Executor::run_trap(int sig) {
  if (sig < 0 or sig >= TRAP_TABLE_SIZE) return;
  Trap trap = mCtx.signals.traps().get(sig);
  if (trap.action != TrapAction::Command and trap.action != TrapAction::Function)
    return;
  if (mInTrap[static_cast<s::size_t>(sig)]) return;   // zsh's ZSIG_IGNORED
  mInTrap[static_cast<s::size_t>(sig)] = true;
  ++mTrapDepth;
  mCtx.signals.begin_trap(sig, s::exchange(mSelfSignal, false));
  // A signal trap the EXIT trap brought on (see call_function).
  bool outer_sig_in_exit = mInExitTrap and is_real_signal(sig) and not mSigTrapInExit;
  if (outer_sig_in_exit) mSigTrapInExit = true;
  int  saved_status = mCtx.last_exit;
  bool saved_err    = mCtx.err_reported;
  bool was_returning = mCtx.returning;
  opt<int> abort_status;
  opt<int> errexit_status;
  opt<TrapErrorScope> scope;
  scope.emplace(mCtx);
  if (trap.action == TrapAction::Function) {
    str name = str("TRAP") + SignalTable::sig_to_name(sig);
    auto fn = mCtx.functions.find(name);
    if (fn != mCtx.functions.end()) {
      ProgRef body = fn->second;
      vec<str> args{name};
      args.push_back(s::to_string(SignalTable::zsh_number(sig)));
      bool returned = false;
      int  rc = call_function(body, args, &returned);
      // A signal trap function that returns non-zero with `return` (not
      // by an error, nor by just failing) acts as if the signal had
      // interrupted the shell, as zsh's trapreturn.
      bool error = mCtx.aborting and not mCtx.interrupt;
      // (dotrapargs does this for ZERR and DEBUG too.)
      if (returned and rc != 0 and not error and sig != SIGEXIT and not mCtx.exiting)
        abort_status = rc;
      if (returned and rc != 0) errexit_status = rc;
    }
  } else {
    run_trap_command(trap);
    if (mCtx.returning and not was_returning and mCtx.return_val != 0)
      errexit_status = mCtx.return_val;
  }
  // zsh: a trap that ended in an error or an interrupt leaves $? 1.
  bool trap_err = mCtx.aborting and not abort_status;
  scope.reset();
  // Under ERR_EXIT a trap's `return N` (N non-zero) is a failing command
  // of the trap, as zsh's execlist sees it: the shell exits with N.
  if (errexit_status and sig != SIGDEBUG and mCtx.opts.errexit() and mCtx.noerrexit == 0
      and not mCtx.exiting) {
    mCtx.exiting  = true;
    mCtx.exit_now = false;
    mCtx.exit_val = *errexit_status;
  }
  if (abort_status) {
    // As zsh's dotrapargs: an interrupt for SIGINT and SIGQUIT, an error
    // for the others (eval and $( … ) then fail with 1, a sourced file
    // ends just itself), and lastval the trap's status.
    mCtx.aborting    = true;
    mCtx.interrupt   = sig == SIGINT or sig == SIGQUIT;
    mCtx.trap_failed = not mCtx.interrupt;
    mCtx.trap_status = *abort_status;
  }
  if (not mCtx.exiting)
    mCtx.last_exit = abort_status ? *abort_status : trap_err ? 1 : saved_status;
  // A `return` in the trap's command returns from the function the trap
  // interrupted (at the top level it ends the script), as in zsh.
  if (mCtx.returning and not was_returning and sig != SIGEXIT) mTrapReturned = true;
  if (abort_status) mTrapAbort = {sig, *abort_status};
  // An `exit` in a signal trap run from the EXIT trap gives the status
  // but lets the EXIT trap finish, as in zsh (not for a -c string that ran
  // off its end).
  if (mInExitTrap and mCtx.exiting and is_real_signal(sig) and not mExitTrapRanOff) {
    mExitFromTrap = mCtx.exit_val;
    mCtx.exiting  = mCtx.exit_now = false;
  }
  // There, an `exit` in a function the trap ran ends the EXIT trap as the
  // trap ends.
  if (outer_sig_in_exit) {
    mSigTrapInExit = false;
    if (s::exchange(mExitInSigTrap, false)) {
      mCtx.exiting  = true;
      mCtx.exit_val = mExitFromTrap;
      mExitEndsExitTrap = true;   // the functions around it don't stop it
    }
  }
  mCtx.err_reported = saved_err;
  --mTrapDepth;
  mInTrap[static_cast<s::size_t>(sig)] = false;
  mCtx.signals.end_trap(sig);   // a `trap - SIG` in it takes effect now
  if (is_real_signal(sig)) {
    // What came meanwhile and waited: its trap runs again, once for each;
    // with the trap gone the default disposition acts (the shell dies of
    // SIGUSR1; SIGINT and SIGHUP go to handle_signals, as untrapped).
    auto i      = static_cast<s::size_t>(sig);
    auto queued = s::exchange(mQueued[i], 0) + s::exchange(mBacklog[i], 0);
    TrapAction now = mCtx.signals.traps().get(sig).action;
    if (queued > 0 and now != TrapAction::Ignore) {
      if (now != TrapAction::Default or mCtx.signals.shell_handles(sig)) {
        for (; queued > 0; --queued) mCtx.signals.note(sig);
      } else {
        mCtx.signals.raise_default(sig);
      }
    }
  }
  // A signal that came while the trap ran (its own, sent from inside it)
  // is handled as soon as it ends, as in zsh.  The depth bounds the
  // recursion of a trap that keeps raising its signal (zsh loops for
  // ever); past it the signal waits for the next command.
  if (mCtx.signals.has_pending() and mTrapChain < 32 and not mCtx.exiting) {
    ++mTrapChain;
    handle_signals();
    --mTrapChain;
  }
}

int Executor::run_trap_command(const Trap& trap) {
  // Its commands leave $LINENO at the line the trap came in on, unless a
  // function they call sets it (zsh's IN_EVAL_TRAP).
  s::uint32_t saved_lineno = mCtx.lineno;
  int saved_depth = s::exchange(mEvalTrapFuncDepth, mCtx.func_depth);
  struct Restore {
    Executor& ex;
    s::uint32_t lineno;
    int depth;
    ~Restore() {
      ex.mCtx.lineno = lineno;
      ex.mEvalTrapFuncDepth = depth;
    }
  } restore{*this, saved_lineno, saved_depth};
  // The trap holds what it parsed when it was set; it is kept alive here
  // in case the trap resets itself.  A `return` in it is left for the
  // caller (run_trap) to act on.
  if (auto prog = trap.prog) {
    StringDepth depth{*this};
    return exec(*prog);
  }
  return exec_string(trap.command);
}

bool Executor::end_prompt_trap() noexcept {
  if (not mCtx.returning or mCtx.func_depth > 0 or mCtx.exiting) return false;
  mCtx.returning = mCtx.return_line = false;
  mTrapReturned  = false;
  mCtx.last_exit = mCtx.return_val;
  return true;
}

int Executor::run_exit_trap(int status, bool ran_off) {
  if (mExitTrapRan or not mCtx.signals.traps().trapped(SIGEXIT)) return status;
  mExitTrapRan = true;
  Trap trap = mCtx.signals.traps().get(SIGEXIT);
  mCtx.signals.set_trap(SIGEXIT, TrapAction::Default);
  bool was_exiting = mCtx.exiting;
  int  was_val     = mCtx.exit_val;
  mCtx.exiting = mCtx.returning = mCtx.aborting = mCtx.exit_now = false;
  mCtx.return_line = false;
  mCtx.end_trap_abort();
  mCtx.break_level = 0;
  mCtx.last_exit   = status;
  ++mTrapDepth;
  mInExitTrap = true;
  mExitTrapRanOff = ran_off;
  mExitEndsExitTrap = false;
  mExitFromTrap = -1;
  mExitTrapFuncDepth = mCtx.func_depth;
  if (trap.action == TrapAction::Command) {
    run_trap_command(trap);
    // `return N` in it gives the shell's exit status.
    if (mCtx.returning) {
      mCtx.returning = false;
      status = mCtx.return_val;
    }
  } else if (auto fn = mCtx.functions.find("TRAPEXIT"); fn != mCtx.functions.end()) {
    ProgRef body = fn->second;
    vec<str> args{"TRAPEXIT", "0"};
    call_function(body, args);
  }
  --mTrapDepth;
  mInExitTrap = false;
  mExitEndsExitTrap = false;
  if (mCtx.exiting) return mCtx.exit_val;   // the trap called `exit`
  if (mExitFromTrap >= 0) return mExitFromTrap;
  mCtx.exiting  = was_exiting;
  mCtx.exit_val = was_val;
  return status;
}

}  // namespace jfsh
